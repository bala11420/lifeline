#pragma once
#include <ostream>
#include <string>
#include "sim/engines.h"

namespace lifeline { namespace sim {

enum class Build { Naive, Lifeline };
inline const char* build_name(Build b) { return b == Build::Naive ? "B1_qos1_model" : "B3_lifeline"; }

struct ScenarioOptions {
  uint64_t work_ms = 90000;     // workload and faults happen in this window
  uint64_t quiesce_ms = 90000;  // then the link is healthy and we wait for recovery
  size_t flash_bytes = 16384;   // Lifeline journal size
  size_t naive_queue = 64;      // baseline RAM queue length
};

// One run = one seed = one workload + one fault schedule, identical for every build.
struct RunResult {
  uint64_t seed = 0;
  Build build = Build::Lifeline;
  uint64_t crit_accepted = 0, crit_lost = 0, crit_maybe = 0, crit_rejected = 0;
  uint64_t ctrl_accepted = 0, ctrl_lost = 0;
  uint64_t dup_effects = 0, stale_overwrites = 0, crit_first_violations = 0, tele_not_converged = 0;
  // Operator -> actuator commands: issued, effects applied more than once, old command applied after a
  // newer one, and cmd_not_converged = actuator output wrong: at the end of the run, right after a
  // reboot, or changed by a command that was not durable.
  uint64_t cmd_issued = 0, cmd_dup_effects = 0, cmd_stale_applied = 0, cmd_not_converged = 0;
  uint64_t crit_p99_ms = 0,  /* p99 delivery delay with link-down time subtracted; delivered events only */  journal_bytes = 0, trace_hash = 0;
  bool violated() const {
    return crit_lost || ctrl_lost || dup_effects || stale_overwrites || crit_first_violations || tele_not_converged ||
           cmd_dup_effects || cmd_stale_applied || cmd_not_converged;
  }
};

RunResult run_scenario(uint64_t seed, Build build, const ScenarioOptions& opt = ScenarioOptions(),
                       std::ostream* story = nullptr);

std::string csv_header();
std::string csv_row(const RunResult& r);

}}  // namespace lifeline::sim
