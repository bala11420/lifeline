// lifeline_sim: run the same seeded workload + fault schedule against both builds and print the
// evidence table.   Examples:
//   ./lifeline_sim                         200 seeds, both builds, table on the terminal
//   ./lifeline_sim --seeds 2000 --csv out.csv
//   ./lifeline_sim --story 17 --build b3   human-readable timeline of one seed
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include "sim/scenario.h"

using namespace lifeline::sim;

namespace {

struct Agg {
  uint64_t runs = 0, bad_runs = 0, crit_accepted = 0, crit_lost = 0, ctrl_lost = 0, dup = 0, stale = 0,
           first_viol = 0, first_viol_runs = 0, not_conv = 0, cmd_dup = 0, cmd_stale = 0, cmd_noconv = 0, cmd_total = 0;
  std::vector<uint64_t> p99;
  std::vector<uint64_t> bad_seeds;
};

void add(Agg& a, const RunResult& r) {
  a.runs++;
  if (r.violated()) { a.bad_runs++; if (a.bad_seeds.size() < 10) a.bad_seeds.push_back(r.seed); }
  a.crit_accepted += r.crit_accepted; a.crit_lost += r.crit_lost; a.ctrl_lost += r.ctrl_lost;
  a.dup += r.dup_effects; a.stale += r.stale_overwrites; a.first_viol += r.crit_first_violations;
  if (r.crit_first_violations) a.first_viol_runs++;
  a.not_conv += r.tele_not_converged;
  a.cmd_total += r.cmd_issued; a.cmd_dup += r.cmd_dup_effects; a.cmd_stale += r.cmd_stale_applied; a.cmd_noconv += r.cmd_not_converged;
  a.p99.push_back(r.crit_p99_ms);
}

uint64_t pct(std::vector<uint64_t> v, int p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = v.size() * static_cast<size_t>(p) / 100;
  return v[i >= v.size() ? v.size() - 1 : i];
}

void print_row(const char* name, const Agg& a) {
  std::cout << std::left << std::setw(15) << name << std::right << std::setw(6) << a.runs << std::setw(10) << a.bad_runs
            << std::setw(10) << a.crit_lost << std::setw(8) << a.ctrl_lost << std::setw(8) << a.dup << std::setw(8) << a.stale
            << std::setw(14) << a.first_viol_runs << std::setw(10) << a.not_conv << std::setw(9) << a.cmd_dup << std::setw(11) << a.cmd_stale << std::setw(11) << a.cmd_noconv
            << std::setw(11) << pct(a.p99, 50) << std::setw(11) << pct(a.p99, 99) << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t first = 1, count = 200, story = 0;
  bool want_story = false, b1 = true, b3 = true;
  std::string csv;
  ScenarioOptions opt;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
    if (a == "--seeds") count = std::strtoull(next(), nullptr, 10);
    else if (a == "--first") first = std::strtoull(next(), nullptr, 10);
    else if (a == "--csv") csv = next();
    else if (a == "--story") { story = std::strtoull(next(), nullptr, 10); want_story = true; }
    else if (a == "--build") { std::string b = next(); b1 = (b == "b1" || b == "both"); b3 = (b == "b3" || b == "both"); }
    else if (a == "--flash") opt.flash_bytes = std::strtoull(next(), nullptr, 10);
    else { std::cerr << "unknown option " << a << "\n"; return 2; }
  }

  if (want_story) {
    Build b = b3 && !b1 ? Build::Lifeline : (b1 && !b3 ? Build::Naive : Build::Lifeline);
    RunResult r = run_scenario(story, b, opt, &std::cout);
    std::cout << "trace hash " << r.trace_hash << "\n";
    return 0;
  }

  std::ofstream out;
  if (!csv.empty()) { out.open(csv); out << csv_header() << "\n"; }
  Agg naive, life;
  for (uint64_t s = first; s < first + count; s++) {
    if (b1) { RunResult r = run_scenario(s, Build::Naive, opt); add(naive, r); if (out) out << csv_row(r) << "\n"; }
    if (b3) { RunResult r = run_scenario(s, Build::Lifeline, opt); add(life, r); if (out) out << csv_row(r) << "\n"; }
  }
  std::cout << "Same " << count << " seeded workloads and fault schedules for every build "
            << "(partitions, loss, duplication, reordering, corruption, node crashes).\n\n";
  std::cout << std::left << std::setw(15) << "build" << std::right << std::setw(6) << "runs" << std::setw(10) << "bad_runs"
            << std::setw(10) << "crit_lost" << std::setw(8) << "ctrl_lo" << std::setw(8) << "dup_eff" << std::setw(8) << "stale"
            << std::setw(14) << "crit1st_runs" << std::setw(10) << "not_conv" << std::setw(9) << "cmd_dup" << std::setw(11) << "cmd_stale" << std::setw(11) << "cmd_wrong" << std::setw(11) << "recov_p50" << std::setw(11) << "recov_p99" << "\n";
  std::cout << "(recov_* = per-run p99 delivery delay in ms with link-DOWN time subtracted; delivered events only)\n";
  if (b1) print_row("B1_qos1_model", naive);
  if (b3) print_row("B3_lifeline", life);
  if (b3 && life.bad_runs) {
    std::cout << "\nLIFELINE VIOLATIONS FOUND. First failing seeds:";
    for (size_t i = 0; i < life.bad_seeds.size(); i++) std::cout << ' ' << life.bad_seeds[i];
    std::cout << "\nReplay one with: ./lifeline_sim --story <seed> --build b3\n";
    return 1;
  }
  return 0;
}
