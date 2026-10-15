#include "doctest.h"
#include "sim/scenario.h"

using namespace lifeline;
using namespace lifeline::sim;

TEST_CASE("same seed gives an identical event trace, different seeds differ") {
  RunResult a = run_scenario(5, Build::Lifeline);
  RunResult b = run_scenario(5, Build::Lifeline);
  RunResult c = run_scenario(6, Build::Lifeline);
  CHECK(a.trace_hash == b.trace_hash);
  CHECK(a.trace_hash != c.trace_hash);
  CHECK(csv_row(a) == csv_row(b));
  RunResult n1 = run_scenario(5, Build::Naive), n2 = run_scenario(5, Build::Naive);
  CHECK(n1.trace_hash == n2.trace_hash);
}

TEST_CASE("a partition drops frames, including ones already in flight") {
  Sim s;
  Rng r(1);
  LinkConfig lc;
  lc.base_ms = 50; lc.jitter_ms = 0;
  Link l(s, r, lc);
  int got = 0;
  l.on_gateway([&](const std::vector<uint8_t>&) { got++; });
  std::vector<uint8_t> f(4, 7);
  l.to_gateway(f);
  s.run_until(100);
  CHECK(got == 1);
  l.to_gateway(f);          // in flight...
  l.set_up(false);          // ...cable cut
  s.run_until(300);
  CHECK(got == 1);
  l.to_gateway(f);          // sent while down
  l.set_up(true);
  s.run_until(500);
  CHECK(got == 1);
  l.to_gateway(f);
  s.run_until(700);
  CHECK(got == 2);
}

TEST_CASE("Lifeline holds every invariant over 200 seeded fault schedules") {
  for (uint64_t seed = 1; seed <= 200; seed++) {
    RunResult r = run_scenario(seed, Build::Lifeline);
    INFO("replay with: ./lifeline_sim --story " << seed << " --build b3");
    REQUIRE(r.crit_lost == 0);
    REQUIRE(r.ctrl_lost == 0);
    REQUIRE(r.dup_effects == 0);
    REQUIRE(r.stale_overwrites == 0);
    REQUIRE(r.crit_first_violations == 0);
    REQUIRE(r.tele_not_converged == 0);
    REQUIRE(r.cmd_dup_effects == 0);
    REQUIRE(r.cmd_stale_applied == 0);
    REQUIRE(r.cmd_not_converged == 0);
  }
}

TEST_CASE("the checks are sensitive: the naive QoS 1 model is caught on every invariant") {
  uint64_t lost = 0, dup = 0, stale = 0, first = 0, notconv = 0, accepted = 0, cdup = 0, cstale = 0, cwrong = 0, cissued = 0;
  for (uint64_t seed = 1; seed <= 100; seed++) {
    RunResult r = run_scenario(seed, Build::Naive);
    lost += r.crit_lost; dup += r.dup_effects; stale += r.stale_overwrites;
    first += r.crit_first_violations; notconv += r.tele_not_converged; accepted += r.crit_accepted;
    cdup += r.cmd_dup_effects; cstale += r.cmd_stale_applied; cwrong += r.cmd_not_converged; cissued += r.cmd_issued;
  }
  CHECK(accepted > 0);
  CHECK(lost > 0);
  CHECK(dup > 0);
  CHECK(stale > 0);
  CHECK(first > 0);
  CHECK(notconv > 0);
  CHECK(cissued > 0);
  CHECK(cdup > 0);
  CHECK(cstale > 0);
  CHECK(cwrong > 0);
}

TEST_CASE("a tiny journal rejects critical events explicitly and never loses one silently") {
  ScenarioOptions o;
  o.flash_bytes = 500;
  uint64_t rejected = 0;
  for (uint64_t seed = 1; seed <= 60; seed++) {
    RunResult r = run_scenario(seed, Build::Lifeline, o);
    INFO("seed " << seed);
    REQUIRE(r.crit_lost == 0);
    REQUIRE(r.ctrl_lost == 0);
    REQUIRE(r.tele_not_converged == 0);
    rejected += r.crit_rejected;
  }
  CHECK(rejected > 0);  // the storage-full path really was exercised
}
