"""lifeline-gw: the gateway. SQLite is the ledger; every accepted event is committed in ONE transaction
together with its application effect and the advanced watermark, and only then acknowledged.

on_frame(frame) -> list of (node_id, reply_frame).  The gateway holds no state outside SQLite, so a
"gateway restart" is just a new Gateway object on the same database.
"""
import sqlite3
from . import wire

SCHEMA = """
CREATE TABLE IF NOT EXISTS nodes(node INTEGER PRIMARY KEY, max_epoch INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS events(node INTEGER, cls INTEGER, seq INTEGER, epoch INTEGER, key INTEGER,
                                  payload BLOB, PRIMARY KEY(node, cls, seq));
CREATE TABLE IF NOT EXISTS watermark(node INTEGER, cls INTEGER, upto INTEGER NOT NULL, PRIMARY KEY(node, cls));
CREATE TABLE IF NOT EXISTS state(node INTEGER, key INTEGER, epoch INTEGER, version INTEGER, payload BLOB,
                                 PRIMARY KEY(node, key));
CREATE TABLE IF NOT EXISTS app(name TEXT PRIMARY KEY, value INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS effects(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, node INTEGER, cls INTEGER,
                                   seq INTEGER, oid INTEGER, epoch INTEGER, version INTEGER);
CREATE TABLE IF NOT EXISTS commands(actuator INTEGER, term INTEGER, seq INTEGER, desired INTEGER,
                                    done INTEGER NOT NULL DEFAULT 0, status INTEGER,
                                    PRIMARY KEY(actuator, term, seq));
CREATE TABLE IF NOT EXISTS meta(name TEXT PRIMARY KEY, value INTEGER NOT NULL);
"""


def open_db(path=":memory:", durable=True):
    db = sqlite3.connect(path, isolation_level=None, check_same_thread=False)
    if path != ":memory:" and durable:
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA synchronous=FULL")
    db.executescript(SCHEMA)
    return db


class Gateway:
    def __init__(self, db, term=1):
        self.db = db
        row = db.execute("SELECT value FROM meta WHERE name='term'").fetchone()
        if row is None:
            db.execute("INSERT INTO meta VALUES('term', ?)", (term,))
        self.refused_epochs = 0

    # ---------------------------------------------------------------- helpers
    def _term(self):
        return self.db.execute("SELECT value FROM meta WHERE name='term'").fetchone()[0]

    def _hwm(self, node, cls):
        r = self.db.execute("SELECT upto FROM watermark WHERE node=? AND cls=?", (node, cls)).fetchone()
        return r[0] if r else 0

    def _advance(self, node, cls):
        w = self._hwm(node, cls)
        while self.db.execute("SELECT 1 FROM events WHERE node=? AND cls=? AND seq=?", (node, cls, w + 1)).fetchone():
            w += 1
        self.db.execute("INSERT INTO watermark VALUES(?,?,?) ON CONFLICT(node,cls) DO UPDATE SET upto=?",
                        (node, cls, w, w))
        return w

    def _bump(self, name):
        self.db.execute("INSERT INTO app VALUES(?,1) ON CONFLICT(name) DO UPDATE SET value=value+1", (name,))

    def _cmd_frames(self, node):
        rows = self.db.execute("SELECT term, seq, desired FROM commands WHERE actuator=? AND done=0 ORDER BY term, seq",
                               (node,)).fetchall()
        return [(node, wire.encode(wire.Cmd(node, t, s, d))) for t, s, d in rows]

    # ---------------------------------------------------------------- commands
    def issue_command(self, actuator, desired):
        """Persist the command BEFORE it is ever sent. Returns the Cmd."""
        term = self._term()
        self.db.execute("BEGIN IMMEDIATE")
        r = self.db.execute("SELECT COALESCE(MAX(seq),0) FROM commands WHERE actuator=? AND term=?",
                            (actuator, term)).fetchone()
        seq = r[0] + 1
        self.db.execute("INSERT INTO commands(actuator, term, seq, desired) VALUES(?,?,?,?)",
                        (actuator, term, seq, desired))
        self.db.execute("COMMIT")
        return wire.Cmd(actuator, term, seq, desired)

    def tick(self):
        """Retransmit every command that has not been acknowledged yet."""
        out = []
        for (node,) in self.db.execute("SELECT DISTINCT actuator FROM commands WHERE done=0").fetchall():
            out += self._cmd_frames(node)
        return out

    # ---------------------------------------------------------------- receive
    def on_frame(self, frame):
        m = wire.decode(frame)
        if m is None:
            return []  # corrupt frames are dropped, the sender retries
        t, o = m
        if t == wire.HELLO:
            return self._hello(o)
        if t == wire.DATA:
            return self._data(o)
        if t == wire.CMDACK:
            self.db.execute("UPDATE commands SET done=1, status=? WHERE actuator=? AND term=? AND seq=?",
                            (o.status, o.node, o.term, o.seq))
            return []
        return []

    def _hello(self, h):
        self.db.execute("BEGIN IMMEDIATE")
        r = self.db.execute("SELECT max_epoch FROM nodes WHERE node=?", (h.node,)).fetchone()
        ok = r is None or h.epoch >= r[0]
        if ok:
            self.db.execute("INSERT INTO nodes VALUES(?,?) ON CONFLICT(node) DO UPDATE SET max_epoch=?",
                            (h.node, h.epoch, h.epoch))
        else:
            self.refused_epochs += 1
        crit, ctrl = self._hwm(h.node, wire.CRIT), self._hwm(h.node, wire.CTRL)
        self.db.execute("COMMIT")
        return [(h.node, wire.encode(wire.Welcome(h.node, crit, ctrl, ok)))] + self._cmd_frames(h.node)

    def _welcome(self, node):
        return (node, wire.encode(wire.Welcome(node, self._hwm(node, wire.CRIT), self._hwm(node, wire.CTRL), True)))

    def _data(self, r):
        if r.cls == wire.TELE:
            return self._telemetry(r)
        self.db.execute("BEGIN IMMEDIATE")
        w = self._hwm(r.node, r.cls)
        have = self.db.execute("SELECT 1 FROM events WHERE node=? AND cls=? AND seq=?",
                               (r.node, r.cls, r.seq)).fetchone()
        reply = None
        if have or r.seq == 0 or r.seq <= w:
            pass  # duplicate: acknowledge again, never re-apply
        elif r.cls == wire.CTRL and r.seq != w + 1:
            # command-like stream: effects must apply in order. Do not apply; tell the node where we
            # are so it rewinds (Welcome) and resends.
            self.db.execute("COMMIT")
            return [self._welcome(r.node)]
        else:
            self.db.execute("INSERT INTO events VALUES(?,?,?,?,?,?)",
                            (r.node, r.cls, r.seq, r.epoch, r.key, r.payload))
            self.db.execute("INSERT INTO effects(kind,node,cls,seq,oid,epoch,version) VALUES(?,?,?,?,?,?,?)",
                            ("critical" if r.cls == wire.CRIT else "control", r.node, r.cls, r.seq,
                             wire.oid_of(r.payload), r.epoch, r.seq))
            self._bump("critical_events" if r.cls == wire.CRIT else "control_events")
            self._advance(r.node, r.cls)
        upto = self._hwm(r.node, r.cls)
        self.db.execute("COMMIT")  # <- the durable commit; only now do we acknowledge
        return [(r.node, wire.encode(wire.Ack(r.node, r.cls, upto)))]

    def _telemetry(self, r):
        self.db.execute("BEGIN IMMEDIATE")
        cur = self.db.execute("SELECT epoch, version FROM state WHERE node=? AND key=?", (r.node, r.key)).fetchone()
        if cur is None or (r.epoch, r.seq) > tuple(cur):  # strictly newer (epoch, version) wins
            self.db.execute("INSERT INTO state VALUES(?,?,?,?,?) ON CONFLICT(node,key) DO UPDATE SET "
                            "epoch=?, version=?, payload=?", (r.node, r.key, r.epoch, r.seq, r.payload,
                                                              r.epoch, r.seq, r.payload))
            self.db.execute("INSERT INTO effects(kind,node,cls,seq,oid,epoch,version) VALUES(?,?,?,?,?,?,?)",
                            ("telemetry", r.node, r.cls, r.key, wire.oid_of(r.payload), r.epoch, r.seq))
        self.db.execute("COMMIT")
        return [(r.node, wire.encode(wire.TeleAck(r.node, r.epoch, r.key, r.seq)))]  # stale/dup: still acked

    # ---------------------------------------------------------------- dashboard
    def status(self):
        q = lambda s, *a: self.db.execute(s, a).fetchall()
        return {
            "term": self._term(),
            "watermarks": [dict(node=n, cls=c, upto=u) for n, c, u in q("SELECT node, cls, upto FROM watermark")],
            "latest_state": [dict(node=n, key=k, epoch=e, version=v) for n, k, e, v in
                             q("SELECT node, key, epoch, version FROM state")],
            "app": dict(q("SELECT name, value FROM app")),
            "commands": [dict(actuator=a, term=t, seq=s, desired=d, done=dn, status=st) for a, t, s, d, dn, st in
                         q("SELECT actuator, term, seq, desired, done, status FROM commands ORDER BY seq")],
            "events": q("SELECT COUNT(*) FROM events")[0][0],
            "refused_epochs": self.refused_epochs,
        }
