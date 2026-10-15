#include "doctest.h"
#include "helpers.h"
#include "lifeline/actuator.h"

using namespace lifeline;
using namespace testutil;

namespace {
Command cmd(uint32_t term, uint64_t seq, uint8_t desired) {
  Command c;
  c.node = 1; c.term = term; c.seq = seq; c.actuator = 1; c.desired = desired;
  return c;
}
}  // namespace

TEST_CASE("actuator applies newer commands and ignores replays and older decisions") {
  MemFlash f(1024);
  Actuator a(f, 1);
  a.boot();
  CHECK(a.output() == 0);
  CHECK(a.on_command(cmd(1, 1, 1)) == Actuator::Result::Applied);
  CHECK(a.output() == 1);
  CHECK(a.on_command(cmd(1, 1, 1)) == Actuator::Result::Stale);  // duplicate delivery
  CHECK(a.on_command(cmd(1, 3, 0)) == Actuator::Result::Applied);
  CHECK(a.output() == 0);
  CHECK(a.on_command(cmd(1, 2, 1)) == Actuator::Result::Stale);  // old command arrives late
  CHECK(a.output() == 0);                                        // the newer decision stands
  CHECK(a.on_command(cmd(2, 1, 1)) == Actuator::Result::Applied);  // new gateway term beats any seq
  CHECK(a.on_command(cmd(1, 99, 0)) == Actuator::Result::Stale);
  Command other = cmd(5, 5, 1);
  other.actuator = 9;
  CHECK(a.on_command(other) == Actuator::Result::Stale);  // addressed to a different actuator
}

TEST_CASE("the gate and the output survive a reboot, so an old command cannot sneak in afterwards") {
  MemFlash f(1024);
  {
    Actuator a(f, 1);
    a.boot();
    REQUIRE(a.on_command(cmd(1, 7, 1)) == Actuator::Result::Applied);
  }
  Actuator b(f, 1);
  b.boot();
  CHECK(b.output() == 1);  // reconciled to the persisted desired state
  CHECK(b.seq() == 7);
  CHECK(b.on_command(cmd(1, 6, 0)) == Actuator::Result::Stale);
  CHECK(b.on_command(cmd(1, 7, 0)) == Actuator::Result::Stale);
  CHECK(b.output() == 1);
  CHECK(b.on_command(cmd(1, 8, 0)) == Actuator::Result::Applied);
}

TEST_CASE("a failed persist means the command is not applied; it works after the device heals") {
  MemFlash f(1024);
  Actuator a(f, 1);
  a.boot();
  REQUIRE(a.on_command(cmd(1, 1, 1)) == Actuator::Result::Applied);
  f.arm_crash_after(6);
  CHECK(a.on_command(cmd(1, 2, 0)) == Actuator::Result::IoError);
  CHECK(a.output() == 1);  // unchanged: never act on a command that was not made durable
  CHECK(a.seq() == 1);
  f.power_cycle();
  CHECK(a.on_command(cmd(1, 2, 0)) == Actuator::Result::Applied);  // the gateway's retry succeeds
  CHECK(a.output() == 0);

  Actuator r(f, 1);  // and the torn bytes did not corrupt anything
  r.boot();
  CHECK(r.seq() == 2);
  CHECK(r.output() == 0);
}

TEST_CASE("a flush failure also leaves the command unapplied and absent after power loss") {
  MemFlash f(1024);
  Actuator a(f, 1);
  a.boot();
  REQUIRE(a.on_command(cmd(1, 1, 1)) == Actuator::Result::Applied);
  f.arm_sync_failure();
  CHECK(a.on_command(cmd(1, 2, 0)) == Actuator::Result::IoError);
  f.power_cycle(true);
  Actuator r(f, 1);
  r.boot();
  CHECK(r.seq() == 1);
  CHECK(r.output() == 1);
}

TEST_CASE("a cut at every byte offset recovers the last complete command") {
  MemFlash f(2048);
  Actuator a(f, 1);
  a.boot();
  std::vector<size_t> ends;
  for (uint64_t s = 1; s <= 12; s++) {
    REQUIRE(a.on_command(cmd(1, s, static_cast<uint8_t>(s % 2))) == Actuator::Result::Applied);
    ends.push_back(f.size());
  }
  const std::vector<uint8_t> full = f.bytes();
  for (size_t cut = 0; cut <= full.size(); cut++) {
    MemFlash g(2048);
    g.load(std::vector<uint8_t>(full.begin(), full.begin() + static_cast<long>(cut)));
    Actuator r(g, 1);
    r.boot();
    uint64_t expect = 0;
    for (size_t i = 0; i < ends.size(); i++) if (ends[i] <= cut) expect = i + 1;
    REQUIRE(r.has_applied() == (expect > 0));
    if (expect > 0) {
      REQUIRE(r.seq() == expect);
      REQUIRE(r.output() == expect % 2);
    }
    REQUIRE(r.on_command(cmd(1, expect + 1, 1)) == Actuator::Result::Applied);  // still usable
  }
}
