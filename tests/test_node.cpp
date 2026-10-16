#include <set>
#include "doctest.h"
#include "helpers.h"
#include "lifeline/node.h"

using namespace lifeline;
using namespace testutil;

namespace {
NodeConfig cfg() {
  NodeConfig c;
  c.node_id = 7;
  c.critical_reserve = 200;
  c.ack_batch = 1;  // persist every ack so tests are exact
  return c;
}
std::vector<Record> drain(Node& n) {
  std::vector<Record> v;
  Record r;
  while (n.poll_send(r)) v.push_back(r);
  return v;
}
}  // namespace

TEST_CASE("strict priority: critical, then control, then telemetry") {
  MemFlash f(8192);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  for (uint16_t k = 1; k <= 5; k++) n.submit(Cls::Telemetry, k, "t", 1);
  for (int i = 0; i < 3; i++) n.submit(Cls::Control, 1, "c", 1);
  for (int i = 0; i < 2; i++) n.submit(Cls::Critical, 1, "!", 1);

  std::vector<Record> out = drain(n);
  REQUIRE(out.size() == 10);
  CHECK(out[0].cls == Cls::Critical);
  CHECK(out[1].cls == Cls::Critical);
  CHECK(out[0].seq == 1);
  CHECK(out[1].seq == 2);
  for (int i = 2; i < 5; i++) CHECK(out[i].cls == Cls::Control);
  for (int i = 5; i < 10; i++) CHECK(out[i].cls == Cls::Telemetry);
}

TEST_CASE("a new critical event preempts a telemetry backlog at the next send") {
  MemFlash f(8192);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  for (uint16_t k = 1; k <= 20; k++) n.submit(Cls::Telemetry, k, "t", 1);
  Record r;
  REQUIRE(n.poll_send(r));
  CHECK(r.cls == Cls::Telemetry);
  n.submit(Cls::Critical, 1, "GAS", 3);
  REQUIRE(n.poll_send(r));
  CHECK(r.cls == Cls::Critical);
}

TEST_CASE("telemetry is a latest-value register, so an outage cannot grow it") {
  MemFlash f(8192);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  for (int i = 1; i <= 100; i++) {
    uint8_t v = static_cast<uint8_t>(i);
    n.submit(Cls::Telemetry, 4, &v, 1);
  }
  CHECK(n.dirty_telemetry() == 1);
  Record r;
  REQUIRE(n.poll_send(r));
  CHECK(r.seq == 100);          // version of the newest value
  CHECK(r.payload[0] == 100);
  CHECK_FALSE(n.poll_send(r));
}

TEST_CASE("critical and control records survive a reboot; acked ones do not come back") {
  MemFlash f(8192);
  FakeClock clk;
  {
    Node n(f, clk, cfg());
    REQUIRE(n.boot());
    CHECK(n.epoch() == 1);
    for (int i = 0; i < 3; i++) CHECK(n.submit(Cls::Critical, 1, "A", 1) == SubmitResult::Accepted);
  }
  {
    Node n(f, clk, cfg());
    REQUIRE(n.boot());
    CHECK(n.epoch() == 2);
    std::vector<Record> out = drain(n);
    REQUIRE(out.size() == 3);
    CHECK(out[0].seq == 1);
    CHECK(out[2].seq == 3);
    n.on_ack(Cls::Critical, 2);
    CHECK(n.unacked(Cls::Critical) == 1);
  }
  {
    Node n(f, clk, cfg());
    REQUIRE(n.boot());
    CHECK(n.epoch() == 3);
    std::vector<Record> out = drain(n);
    REQUIRE(out.size() == 1);
    CHECK(out[0].seq == 3);
    uint64_t seq = 0;
    REQUIRE(n.submit(Cls::Critical, 1, "B", 1, &seq) == SubmitResult::Accepted);
    CHECK(seq == 4);  // sequence numbers continue across reboots
  }
}

TEST_CASE("a crash mid-write rejects that event cleanly and never creates a phantom") {
  MemFlash f(8192);
  FakeClock clk;
  {
    Node n(f, clk, cfg());
    REQUIRE(n.boot());
    REQUIRE(n.submit(Cls::Critical, 1, "ok", 2) == SubmitResult::Accepted);
    f.arm_crash_after(12);
    CHECK(n.submit(Cls::Critical, 1, "lost", 4) == SubmitResult::IoError);
    CHECK(n.storage_fault());
    CHECK(n.stats().rejected_io[0] == 1);
  }
  f.power_cycle();
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  std::vector<Record> out = drain(n);
  REQUIRE(out.size() == 1);
  CHECK(out[0].seq == 1);
  uint64_t seq = 0;
  REQUIRE(n.submit(Cls::Critical, 1, "again", 5, &seq) == SubmitResult::Accepted);
  CHECK(seq == 2);  // the failed event did not consume a sequence number
}

TEST_CASE("when storage fills, control is rejected first and the fault is visible") {
  MemFlash f(700);
  FakeClock clk;
  Node n(f, clk, cfg());  // reserve 200
  REQUIRE(n.boot());
  int control_ok = 0;
  while (n.submit(Cls::Control, 1, "cccccccccc", 10) == SubmitResult::Accepted) control_ok++;
  CHECK(control_ok > 0);
  CHECK(n.storage_fault());
  CHECK(n.stats().rejected_full[1] >= 1);
  CHECK(n.submit(Cls::Critical, 1, "FIRE", 4) == SubmitResult::Accepted);  // reserve still open
  CHECK(n.submit(Cls::Telemetry, 1, "t", 1) == SubmitResult::Accepted);    // RAM register, unaffected
}

TEST_CASE("resync trims acknowledged records and resends the rest, critical first") {
  MemFlash f(8192);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  for (int i = 0; i < 4; i++) n.submit(Cls::Critical, 1, "x", 1);
  for (int i = 0; i < 2; i++) n.submit(Cls::Control, 1, "y", 1);
  n.submit(Cls::Telemetry, 3, "t", 1);
  CHECK(drain(n).size() == 7);          // everything sent once, link then drops

  n.on_resync(2, 0);                    // gateway committed critical 1..2 and no control
  std::vector<Record> out = drain(n);
  REQUIRE(out.size() == 5);             // crit 3,4 + ctrl 1,2 + telemetry again
  CHECK(out[0].cls == Cls::Critical);
  CHECK(out[0].seq == 3);
  CHECK(out[1].seq == 4);
  CHECK(out[2].cls == Cls::Control);
  CHECK(out[4].cls == Cls::Telemetry);
  n.on_ack(Cls::Critical, 999);         // bogus ack beyond anything that exists is ignored
  CHECK(n.unacked(Cls::Critical) == 2);   // still 3 and 4
}

TEST_CASE("the journal heals after a write error and the node keeps journaling critical events") {
  MemFlash f(8192);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  REQUIRE(n.submit(Cls::Critical, 1, "a", 1) == SubmitResult::Accepted);
  f.arm_crash_after(5);
  CHECK(n.submit(Cls::Critical, 1, "b", 1) == SubmitResult::IoError);
  f.power_cycle();  // flash recovers; NO reboot of the node
  uint64_t seq = 0;
  CHECK(n.submit(Cls::Critical, 1, "c", 1, &seq) == SubmitResult::Accepted);
  CHECK(seq == 2);
  Node r(f, clk, cfg());
  REQUIRE(r.boot());
  CHECK(drain(r).size() == 2);
}

TEST_CASE("a failed flush is never reported as Accepted and the event is absent after power loss") {
  MemFlash f(8192);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  f.arm_sync_failure();
  CHECK(n.submit(Cls::Critical, 1, "gas", 3) == SubmitResult::IoError);
  CHECK(n.storage_fault());
  f.power_cycle(true);
  Node r(f, clk, cfg());
  REQUIRE(r.boot());
  CHECK(drain(r).empty());
}

TEST_CASE("ack records burn journal space; batching stretches it; the limit is still real") {
  for (int batch : {1, 8}) {
    NodeConfig c = cfg();
    c.ack_batch = static_cast<size_t>(batch);
    MemFlash f(600);  // the probe: 11 criticals acked one at a time
    FakeClock clk;
    Node n(f, clk, c);
    REQUIRE(n.boot());
    for (int i = 0; i < 11; i++) {
      REQUIRE(n.submit(Cls::Critical, 1, "x", 1) == SubmitResult::Accepted);
      n.on_ack(Cls::Critical, static_cast<uint64_t>(i + 1));
    }
    CHECK(n.unacked(Cls::Critical) == 0);
    SubmitResult r = n.submit(Cls::Critical, 1, "y", 1);
    if (batch == 1) CHECK(r == SubmitResult::StorageFull);  // without compaction the log just fills
    else CHECK(r == SubmitResult::Accepted);
  }
}

TEST_CASE("an unpersisted ack only causes a harmless resend that the gateway welcome trims") {
  NodeConfig c = cfg();
  c.ack_batch = 8;
  MemFlash f(8192);
  FakeClock clk;
  {
    Node n(f, clk, c);
    REQUIRE(n.boot());
    for (int i = 0; i < 3; i++) n.submit(Cls::Critical, 1, "x", 1);
    n.on_ack(Cls::Critical, 2);  // below the batch threshold: not written to flash
  }
  Node r(f, clk, c);
  REQUIRE(r.boot());
  CHECK(r.unacked(Cls::Critical) == 3);  // 1 and 2 come back: duplicates, never losses
  Welcome w;
  w.node = 7; w.crit_hwm = 2; w.ctrl_hwm = 0; w.epoch_ok = true;
  CHECK(r.on_welcome(w));
  std::vector<Record> out = drain(r);
  REQUIRE(out.size() == 1);
  CHECK(out[0].seq == 3);
}

TEST_CASE("a refused epoch is surfaced to the driver") {
  MemFlash f(4096);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  Welcome w;
  w.node = 7; w.epoch_ok = false;
  CHECK_FALSE(n.on_welcome(w));
  CHECK(n.epoch_rejected());
}

TEST_CASE("telemetry converges: unacked values are re-sent by retry_telemetry, acked ones are not") {
  MemFlash f(4096);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  n.submit(Cls::Telemetry, 1, "a", 1);
  n.submit(Cls::Telemetry, 2, "b", 1);
  std::vector<Record> out = drain(n);   // both sent; assume both packets were lost
  REQUIRE(out.size() == 2);
  CHECK(n.dirty_telemetry() == 0);
  n.on_telemetry_ack(n.epoch(), 1, out[0].seq);  // the gateway got key 1 only
  CHECK(n.retry_telemetry() == 1);               // only key 2 is re-marked
  out = drain(n);
  REQUIRE(out.size() == 1);
  CHECK(out[0].key == 2);
  n.on_telemetry_ack(n.epoch() + 1, 2, out[0].seq);  // ack from another epoch is ignored
  CHECK(n.retry_telemetry() == 1);               // key 2 is still unacked, so it is re-marked again
  CHECK(drain(n).size() == 1);
  n.submit(Cls::Telemetry, 1, "newer", 5);       // a newer value is unacked again
  CHECK(n.dirty_telemetry() == 1);
}

TEST_CASE("repeated random crashes (torn writes and failed flushes, also during acks) never lose a committed critical event") {
  for (uint64_t seed = 1; seed <= 600; seed++) {
    Lcg rng(seed);
    MemFlash f(1200 + rng.below(1500));
    FakeClock clk;
    std::set<uint64_t> accepted;
    uint64_t gw_acked = 0, max_accepted = 0;

    for (int cycle = 0; cycle < 3; cycle++) {
      Node n(f, clk, cfg());
      n.boot();
      n.on_resync(gw_acked, 0);  // hello/welcome after every reboot
      uint32_t crash_at = rng.below(30);
      uint32_t crash_type = rng.below(2);  // 0 = torn write, 1 = power loss at flush
      for (uint32_t i = 0; i < 30; i++) {
        if (i == crash_at) {
          if (crash_type == 0) f.arm_crash_after(rng.below(150));
          else f.arm_sync_failure();
        }
        uint32_t op = rng.below(10);
        uint64_t seq = 0;
        if (op < 5) {
          if (n.submit(Cls::Critical, 1, "evt", 3, &seq) == SubmitResult::Accepted) {
            REQUIRE_MESSAGE(accepted.insert(seq).second, "seed " << seed << " reused critical seq " << seq);
            max_accepted = seq;
          }
        } else if (op < 7) {
          n.submit(Cls::Control, 2, "cmd", 3);
        } else if (op < 9) {
          n.submit(Cls::Telemetry, static_cast<uint16_t>(rng.below(4)), "t", 1);
        } else {
          uint64_t upto = gw_acked + rng.below(3);
          if (upto > max_accepted) upto = max_accepted;
          n.on_ack(Cls::Critical, upto);  // may itself hit the armed crash
          if (upto > gw_acked) gw_acked = upto;
        }
        if (rng.below(8) == 0) f.power_cycle(crash_type == 1);  // device may recover mid-run
      }
      f.power_cycle(crash_type == 1);  // reboot: a failed flush loses its unsynced bytes
    }

    Node r(f, clk, cfg());
    r.boot();
    std::set<uint64_t> recovered;
    for (const Record& rec : drain(r))
      if (rec.cls == Cls::Critical) recovered.insert(rec.seq);

    for (uint64_t s : accepted)
      if (s > gw_acked)
        REQUIRE_MESSAGE(recovered.count(s) == 1, "seed " << seed << " LOST committed critical seq " << s);
    for (uint64_t s : recovered)
      REQUIRE_MESSAGE(accepted.count(s) == 1, "seed " << seed << " phantom critical seq " << s);
  }
}

TEST_CASE("a frame that survives a failed flush is dropped, so a sequence number is never reused") {
  MemFlash f(8192);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  f.arm_sync_failure();
  CHECK(n.submit(Cls::Critical, 1, "AAA", 3) == SubmitResult::IoError);  // caller is told: failed
  f.power_cycle(false);                                                   // ...but the bytes survived
  uint64_t seq = 0;
  CHECK(n.submit(Cls::Critical, 1, "BBB", 3, &seq) == SubmitResult::Accepted);
  CHECK(seq == 1);
  Node r(f, clk, cfg());
  REQUIRE(r.boot());
  std::vector<Record> out = drain(r);
  REQUIRE(out.size() == 1);  // before the fix: two different records, both seq 1
  CHECK(out[0].seq == 1);
  CHECK(std::memcmp(out[0].payload, "BBB", 3) == 0);
}

TEST_CASE("pending_records exposes exactly the unacknowledged durable records") {
  MemFlash f(8192);
  FakeClock clk;
  Node n(f, clk, cfg());
  REQUIRE(n.boot());
  for (int i = 0; i < 3; i++) n.submit(Cls::Critical, 1, "x", 1);
  n.on_ack(Cls::Critical, 1);
  std::vector<Record> p = n.pending_records(Cls::Critical);
  REQUIRE(p.size() == 2);
  CHECK(p[0].seq == 2);
  CHECK(n.pending_records(Cls::Telemetry).empty());
}
