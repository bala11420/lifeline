"""Gateway regression tests. Run from the repository root:  python3 -m unittest tests.test_gateway -v"""
import unittest
from gateway import wire
from gateway.gateway import Gateway, open_db


def data(seq, epoch=1, cls=wire.CRIT, key=1, payload=b"12345678", node=1):
    return wire.encode(wire.Record(node, epoch, cls, seq, key, 0, payload))


def hello(epoch, node=1):
    return wire.encode(wire.Hello(node, epoch, 1, 1))


def msgs(replies):
    return [wire.decode(f)[1] for _, f in replies]


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.gw = Gateway(open_db())

    def test_duplicate_is_acked_but_applied_once(self):
        self.gw.on_frame(hello(1))
        for _ in range(3):
            r = msgs(self.gw.on_frame(data(1)))
            self.assertEqual(r[0].upto, 1)
        self.assertEqual(self.gw.status()["app"]["critical_events"], 1)

    def test_stale_telemetry_never_overwrites_newer(self):
        self.gw.on_frame(hello(1))
        self.gw.on_frame(data(5, cls=wire.TELE, key=2, payload=b"new"))
        self.gw.on_frame(data(3, cls=wire.TELE, key=2, payload=b"old"))
        st = self.gw.status()["latest_state"][0]
        self.assertEqual((st["epoch"], st["version"]), (1, 5))

    def test_wiped_node_is_refused_not_acked(self):
        """A node whose journal was wiped restarts at epoch 1 / seq 1. Its new event must not be silently
        swallowed as a 'duplicate': the gateway must refuse it and must not acknowledge it."""
        self.gw.on_frame(hello(5))
        for s in (1, 2, 3):
            self.gw.on_frame(data(s, epoch=5))
        w = msgs(self.gw.on_frame(hello(1)))[0]
        self.assertFalse(w.epoch_ok)
        r = msgs(self.gw.on_frame(data(1, epoch=1, payload=b"NEWGAS!!")))
        self.assertIsInstance(r[0], wire.Welcome)          # refusal, not an Ack
        self.assertFalse(r[0].epoch_ok)
        self.assertEqual(self.gw.status()["events"], 3)    # nothing applied
        self.assertEqual(self.gw.status()["refused_frames"], 1)

    def test_refusal_survives_gateway_restart(self):
        self.gw.on_frame(hello(5))
        self.gw.on_frame(hello(1))
        gw2 = Gateway(self.gw.db)                          # restart = new object, same database
        r = msgs(gw2.on_frame(data(1)))
        self.assertIsInstance(r[0], wire.Welcome)

    def test_old_epoch_records_from_a_healthy_reboot_are_still_accepted(self):
        """After a normal reboot the node resends records stamped with its previous epoch. That is legal."""
        self.gw.on_frame(hello(1))
        self.gw.on_frame(hello(2))                         # rebooted: epoch 2
        r = msgs(self.gw.on_frame(data(1, epoch=1)))
        self.assertIsInstance(r[0], wire.Ack)
        self.assertEqual(self.gw.status()["events"], 1)

    def test_reenroll_lets_a_replacement_device_start_clean(self):
        self.gw.on_frame(hello(5))
        self.gw.on_frame(data(1, epoch=5))
        self.gw.on_frame(hello(1))                         # refused
        self.gw.reenroll(1)
        self.assertTrue(msgs(self.gw.on_frame(hello(1)))[0].epoch_ok)
        r = msgs(self.gw.on_frame(data(1, epoch=1, payload=b"NEWGAS!!")))
        self.assertIsInstance(r[0], wire.Ack)
        self.assertEqual(r[0].upto, 1)
        archived = self.gw.db.execute("SELECT COUNT(*) FROM events_archive").fetchone()[0]
        self.assertEqual(archived, 1)                      # old history kept, not destroyed


if __name__ == "__main__":
    unittest.main()
