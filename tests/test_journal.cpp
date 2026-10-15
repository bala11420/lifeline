#include <string>
#include "doctest.h"
#include "helpers.h"
#include "lifeline/journal.h"

using namespace lifeline;
using namespace testutil;

TEST_CASE("journal recovers exactly what was committed") {
  MemFlash f(4096);
  Journal j(f, 0);
  CHECK(j.append_epoch(3) == Journal::Result::Ok);
  CHECK(j.append_data(make(Cls::Critical, 1, 1, "alarm")) == Journal::Result::Ok);
  CHECK(j.append_data(make(Cls::Control, 1, 2, "cmd")) == Journal::Result::Ok);
  CHECK(j.append_ack(Cls::Critical, 1) == Journal::Result::Ok);

  MemFlash g(4096);
  g.load(f.bytes());
  Journal k(g, 0);
  std::vector<JournalEntry> e;
  REQUIRE(k.recover(e) == 4);
  CHECK(e[0].kind == Kind::Epoch);
  CHECK(e[0].epoch == 3);
  CHECK(e[1].kind == Kind::Data);
  CHECK(same(e[1].rec, make(Cls::Critical, 1, 1, "alarm")));
  CHECK(e[3].kind == Kind::Ack);
  CHECK(e[3].ack_upto == 1);
}

TEST_CASE("a cut at EVERY byte offset recovers a clean prefix and stays appendable") {
  MemFlash f(8192);
  Journal j(f, 0);
  std::vector<size_t> ends;
  for (int i = 1; i <= 20; i++) {
    std::string text(static_cast<size_t>(i % 9) * 3, 'a' + (i % 26));
    REQUIRE(j.append_data(make(i % 2 ? Cls::Critical : Cls::Control, i, 1, text.c_str())) ==
            Journal::Result::Ok);
    ends.push_back(f.size());
  }
  const std::vector<uint8_t> full = f.bytes();

  for (size_t cut = 0; cut <= full.size(); cut++) {
    std::vector<uint8_t> part(full.begin(), full.begin() + static_cast<long>(cut));
    MemFlash g(8192);
    g.load(part);
    Journal k(g, 0);
    std::vector<JournalEntry> e;
    size_t n = k.recover(e);

    size_t expect = 0, good_end = 0;
    for (size_t i = 0; i < ends.size(); i++)
      if (ends[i] <= cut) { expect = i + 1; good_end = ends[i]; }
    REQUIRE(n == expect);
    REQUIRE(g.size() == good_end);  // torn tail was removed
    for (size_t i = 0; i < n; i++) REQUIRE(e[i].rec.seq == i + 1);

    // After recovery new appends must not be hidden behind leftover garbage.
    REQUIRE(k.append_data(make(Cls::Critical, 1000, 9, "new")) == Journal::Result::Ok);
    MemFlash h(8192);
    h.load(g.bytes());
    Journal m(h, 0);
    std::vector<JournalEntry> e2;
    REQUIRE(m.recover(e2) == n + 1);
  }
}

TEST_CASE("random garbage after a valid log is discarded") {
  MemFlash f(4096);
  Journal j(f, 0);
  for (int i = 1; i <= 5; i++) j.append_data(make(Cls::Critical, i, 1, "x"));
  std::vector<uint8_t> bytes = f.bytes();
  Lcg rng(5);
  for (int i = 0; i < 40; i++) bytes.push_back(static_cast<uint8_t>(rng.next()));
  MemFlash g(4096);
  g.load(bytes);
  Journal k(g, 0);
  std::vector<JournalEntry> e;
  CHECK(k.recover(e) == 5);
  CHECK(k.discarded_tail_bytes() == 40);
}

TEST_CASE("a crash mid-append poisons the journal until recovery") {
  MemFlash f(4096);
  Journal j(f, 0);
  REQUIRE(j.append_data(make(Cls::Critical, 1, 1, "first")) == Journal::Result::Ok);
  f.arm_crash_after(10);  // power dies 10 bytes into the next frame
  CHECK(j.append_data(make(Cls::Critical, 2, 1, "second")) == Journal::Result::IoError);
  CHECK(j.poisoned());
  CHECK(j.append_data(make(Cls::Critical, 3, 1, "third")) == Journal::Result::IoError);

  f.power_cycle();
  Journal k(f, 0);
  std::vector<JournalEntry> e;
  REQUIRE(k.recover(e) == 1);
  CHECK(e[0].rec.seq == 1);
  CHECK(k.append_data(make(Cls::Critical, 2, 1, "second")) == Journal::Result::Ok);
}

TEST_CASE("critical records may use the reserved region, others may not") {
  MemFlash f(1000);
  Journal j(f, 300);
  int control_ok = 0;
  while (j.append_data(make(Cls::Control, control_ok + 1, 1, "cccccccccc")) == Journal::Result::Ok) control_ok++;
  CHECK(control_ok > 0);
  CHECK(j.used() <= 700);

  int crit_ok = 0;
  while (j.append_data(make(Cls::Critical, crit_ok + 1, 1, "cccccccccc")) == Journal::Result::Ok) crit_ok++;
  CHECK(crit_ok > 0);
  CHECK(j.used() <= 1000);
  CHECK(j.append_data(make(Cls::Critical, 99, 1, "x")) == Journal::Result::Full);
}

TEST_CASE("after a write error the journal heals itself without a reboot") {
  MemFlash f(4096);
  Journal j(f, 0);
  REQUIRE(j.append_data(make(Cls::Critical, 1, 1, "first")) == Journal::Result::Ok);
  f.arm_crash_after(9);
  REQUIRE(j.append_data(make(Cls::Critical, 2, 1, "torn")) == Journal::Result::IoError);
  CHECK(j.append_data(make(Cls::Critical, 2, 1, "x")) == Journal::Result::IoError);  // device still dead
  CHECK(j.poisoned());

  f.power_cycle();  // device is healthy again; the SAME Journal object keeps running
  REQUIRE(j.append_data(make(Cls::Critical, 2, 1, "second")) == Journal::Result::Ok);
  CHECK_FALSE(j.poisoned());
  REQUIRE(j.append_data(make(Cls::Critical, 3, 1, "third")) == Journal::Result::Ok);

  MemFlash g(4096);
  g.load(f.bytes());
  Journal k(g, 0);
  std::vector<JournalEntry> e;
  REQUIRE(k.recover(e) == 3);
  CHECK(k.discarded_tail_bytes() == 0);  // the torn bytes were removed, not left in the middle
  CHECK(e[1].rec.seq == 2);
}

TEST_CASE("a failed sync is reported as an error, never as a committed record") {
  MemFlash f(4096);
  Journal j(f, 0);
  REQUIRE(j.append_data(make(Cls::Critical, 1, 1, "safe")) == Journal::Result::Ok);
  f.arm_sync_failure();
  CHECK(j.append_data(make(Cls::Critical, 2, 1, "unsynced")) == Journal::Result::IoError);
  f.power_cycle(true);  // power loss: bytes that were never flushed are gone
  Journal k(f, 0);
  std::vector<JournalEntry> e;
  REQUIRE(k.recover(e) == 1);
  CHECK(e[0].rec.seq == 1);
}

TEST_CASE("every Ok record survives losing all unsynced data") {
  MemFlash f(4096);
  Journal j(f, 0);
  for (int i = 1; i <= 10; i++) REQUIRE(j.append_data(make(Cls::Critical, i, 1, "d")) == Journal::Result::Ok);
  f.drop_unsynced();
  Journal k(f, 0);
  std::vector<JournalEntry> e;
  CHECK(k.recover(e) == 10);
}
