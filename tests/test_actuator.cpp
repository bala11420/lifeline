#include "doctest.h"
#include "helpers.h"
#include "lifeline/actuator.h"

using namespace lifeline;
using namespace testutil;

namespace {
Cmd cmd(uint32_t term, uint64_t seq, uint8_t desired) {
  Cmd c; c.node = 3; c.term = term; c.seq = seq; c.desired = desired;
  return c;
}
}  // namespace

TEST_CASE("actuator: newer commands apply, duplicates are harmless, older ones are rejected") {
  MemFlash f(1024);
  Actuator a(f);
  REQUIRE(a.boot());
  CHECK(a.desired() == Valve::Open);
  CHECK(a.on_command(cmd(1, 1, 1)) == CmdResult::Applied);
  CHECK(a.desired() == Valve::Closed);
  CHECK(a.on_command(cmd(1, 1, 1)) == CmdResult::Duplicate);
  CHECK(a.on_command(cmd(1, 2, 0)) == CmdResult::Applied);
  CHECK(a.on_command(cmd(1, 1, 1)) == CmdResult::Stale);  // old CLOSED replayed after OPEN
  CHECK(a.desired() == Valve::Open);
  CHECK(a.on_command(cmd(2, 1, 1)) == CmdResult::Applied);  // a new authority term beats any seq
  CHECK(a.on_command(cmd(1, 99, 0)) == CmdResult::Stale);
}

TEST_CASE("actuator: the committed decision survives a reboot and still rejects old commands") {
  MemFlash f(1024);
  {
    Actuator a(f);
    REQUIRE(a.boot());
    REQUIRE(a.on_command(cmd(1, 5, 1)) == CmdResult::Applied);
  }
  Actuator b(f);
  REQUIRE(b.boot());
  CHECK(b.have());
  CHECK(b.desired() == Valve::Closed);
  CHECK(b.seq() == 5);
  CHECK(b.on_command(cmd(1, 4, 0)) == CmdResult::Stale);
  CHECK(b.on_command(cmd(1, 5, 1)) == CmdResult::Duplicate);
}

TEST_CASE("actuator: a torn or unsynced commit is never reported applied and recovery is clean") {
  MemFlash f(1024);
  Actuator a(f);
  REQUIRE(a.boot());
  REQUIRE(a.on_command(cmd(1, 1, 1)) == CmdResult::Applied);
  f.arm_crash_after(7);
  CHECK(a.on_command(cmd(1, 2, 0)) == CmdResult::IoError);
  CHECK(a.desired() == Valve::Closed);  // in-memory decision did not move
  f.power_cycle();
  CHECK(a.on_command(cmd(1, 2, 0)) == CmdResult::Applied);  // retry heals the torn bytes
  Actuator b(f);
  REQUIRE(b.boot());
  CHECK(b.seq() == 2);
  CHECK(b.desired() == Valve::Open);
}

TEST_CASE("actuator: the local interlock beats a remote OPEN and releases on reset") {
  MemFlash f(1024);
  Actuator a(f);
  REQUIRE(a.boot());
  a.set_interlock(true);
  CHECK(a.on_command(cmd(1, 1, 0)) == CmdResult::Applied);  // remote asks OPEN
  CHECK(a.desired() == Valve::Open);
  CHECK(a.output() == Valve::Closed);                        // but the local hazard wins
  a.set_interlock(false);
  CHECK(a.output() == Valve::Open);                          // reconciled after the explicit reset
}
