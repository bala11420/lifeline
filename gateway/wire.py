"""Frame codec matching include/lifeline/messages.h exactly (little-endian, CRC32 over type+body).

Frame: [u8 type][body][u32 crc32(type+body)].  Any corruption makes decode() return None.
"""
import struct
import zlib
from collections import namedtuple

DATA, HELLO, WELCOME, ACK, TELEACK, CMD, CMDACK = 1, 2, 3, 4, 5, 6, 7
CRIT, CTRL, TELE = 0, 1, 2

Record = namedtuple("Record", "node epoch cls seq key ts payload")
Hello = namedtuple("Hello", "node epoch crit_next_seq ctrl_next_seq")
Welcome = namedtuple("Welcome", "node crit_hwm ctrl_hwm epoch_ok")
Ack = namedtuple("Ack", "node cls upto")
TeleAck = namedtuple("TeleAck", "node epoch key version")
Cmd = namedtuple("Cmd", "node term seq desired")
CmdAck = namedtuple("CmdAck", "node term seq status")

MAX_PAYLOAD = 64


def _seal(t, body):
    raw = bytes([t]) + body
    return raw + struct.pack("<I", zlib.crc32(raw) & 0xFFFFFFFF)


def _open(frame):
    if len(frame) < 5:
        return None
    raw, crc = frame[:-4], struct.unpack("<I", frame[-4:])[0]
    if zlib.crc32(raw) & 0xFFFFFFFF != crc:
        return None
    return raw[0], raw[1:]


def encode(m):
    if isinstance(m, Record):
        body = struct.pack("<IIBQHHI", m.node, m.epoch, m.cls, m.seq, m.key, len(m.payload), m.ts) + m.payload
        return _seal(DATA, body)
    if isinstance(m, Hello):
        return _seal(HELLO, struct.pack("<IIQQ", *m))
    if isinstance(m, Welcome):
        return _seal(WELCOME, struct.pack("<IQQB", m.node, m.crit_hwm, m.ctrl_hwm, 1 if m.epoch_ok else 0))
    if isinstance(m, Ack):
        return _seal(ACK, struct.pack("<IBQ", m.node, m.cls, m.upto))
    if isinstance(m, TeleAck):
        return _seal(TELEACK, struct.pack("<IIHQ", *m))
    if isinstance(m, Cmd):
        return _seal(CMD, struct.pack("<IIQB", *m))
    if isinstance(m, CmdAck):
        return _seal(CMDACK, struct.pack("<IIQB", *m))
    raise TypeError(m)


def decode(frame):
    """Returns (msg_type, namedtuple) or None if the frame is invalid."""
    o = _open(frame)
    if o is None:
        return None
    t, b = o
    try:
        if t == DATA:
            if len(b) < 25:
                return None
            node, epoch, cls, seq, key, ln, ts = struct.unpack("<IIBQHHI", b[:25])
            if cls > 2 or ln > MAX_PAYLOAD or len(b) != 25 + ln:
                return None
            return t, Record(node, epoch, cls, seq, key, ts, bytes(b[25:]))
        if t == HELLO and len(b) == 24:
            return t, Hello(*struct.unpack("<IIQQ", b))
        if t == WELCOME and len(b) == 21 and b[20] <= 1:
            n, c, k, ok = struct.unpack("<IQQB", b)
            return t, Welcome(n, c, k, bool(ok))
        if t == ACK and len(b) == 13 and b[4] <= 1:
            return t, Ack(*struct.unpack("<IBQ", b))
        if t == TELEACK and len(b) == 18:
            return t, TeleAck(*struct.unpack("<IIHQ", b))
        if t == CMD and len(b) == 17 and b[16] <= 1:
            return t, Cmd(*struct.unpack("<IIQB", b))
        if t == CMDACK and len(b) == 17 and b[16] <= 1:
            return t, CmdAck(*struct.unpack("<IIQB", b))
    except struct.error:
        return None
    return None


def oid_of(payload):
    """Demo payload convention: the first 8 bytes are an id the test oracle assigned."""
    return struct.unpack("<Q", payload[:8])[0] if len(payload) >= 8 else 0
