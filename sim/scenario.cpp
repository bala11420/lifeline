#include "sim/scenario.h"
#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include "lifeline/codec.h"

namespace lifeline { namespace sim {

namespace {

class Runner {
 public:
  Runner(uint64_t seed, Build b, const ScenarioOptions& o, std::ostream* story)
      : seed_(seed), build_(b), opt_(o), story_(story),
        frng_(seed * 4 + 1), wrng_(seed * 4 + 2), crng_(seed * 4 + 3) {
    work_end_ = opt_.work_ms;
    end_ = opt_.work_ms + opt_.quiesce_ms;

    LinkConfig lc;  // fault intensity differs per seed
    lc.base_ms = crng_.range(10, 60);
    lc.jitter_ms = crng_.range(5, 40);
    lc.loss_ppm = crng_.below(200001);     // up to 20%
    lc.dup_ppm = crng_.below(100001);      // up to 10%
    lc.corrupt_ppm = crng_.below(30001);   // up to 3%
    lc.reorder_ppm = crng_.below(200001);  // up to 20%
    link_.reset(new Link(sim_, frng_, lc));
    link_->on_gateway([this](const std::vector<uint8_t>& f) { gateway_frame(f); });
    link_->on_node([this](const std::vector<uint8_t>& f) { node_frame(f); });

    if (b == Build::Lifeline) {
      node_ = make_lifeline_node(sim_, 1, opt_.flash_bytes);
      gw_ = make_lifeline_gateway();
    } else {
      node_ = make_naive_node(sim_, 1, opt_.naive_queue);
      gw_ = make_naive_gateway();
    }
    node_->boot();
    schedule_faults();
    schedule_workload();
    sim_.after(20, [this]() { tick(); });
  }

  RunResult run() {
    sim_.run_until(end_ + 1);
    sync_applied();
    return evaluate();
  }

 private:
  struct Gen { uint32_t id; Cls cls; uint16_t key; uint32_t val; uint64_t t; Accept acc; };

  void say(const std::string& s) {
    if (!story_) return;
    *story_ << "[" << std::setw(7) << std::fixed << std::setprecision(3) << (sim_.now() / 1000.0) << "s] " << s << "\n";
  }

  void schedule_faults() {
    uint32_t nparts = 1 + crng_.below(3);
    for (uint32_t i = 0; i < nparts; i++) {
      uint64_t start = crng_.range(3000, static_cast<uint32_t>(work_end_ - 20000));
      uint64_t dur = crng_.range(5000, 40000);
      if (start + dur > work_end_) dur = work_end_ - start;
      sim_.at(start, [this, dur]() {
        if (down_ == 0) down_since_ = static_cast<int64_t>(sim_.now());
        down_++;
        link_->set_up(false);
        say("LINK DOWN (partition for " + std::to_string(dur / 1000) + "s)");
      });
      sim_.at(start + dur, [this]() {
        if (down_ > 0 && --down_ == 0) {
          link_->set_up(true);
          say("LINK UP");
          down_spans_.push_back(std::make_pair(down_since_, static_cast<int64_t>(sim_.now())));
        }
      });
    }
    uint32_t ncrash = crng_.below(4);
    for (uint32_t i = 0; i < ncrash; i++) {
      uint64_t t = crng_.range(5000, static_cast<uint32_t>(work_end_ - 5000));
      int kind = static_cast<int>(crng_.below(3));
      uint32_t bytes = crng_.below(150), delay = crng_.range(200, 3000);
      sim_.at(t, [this, kind, bytes, delay]() {
        if (kind < 2) {
          say(kind == 0 ? "FLASH FAULT: torn write armed" : "FLASH FAULT: power loss at flush armed");
          node_->crash_arm(kind, bytes);
          sim_.after(delay, [this]() { do_reboot(); });
        } else {
          do_reboot();
        }
      });
    }
  }

  void do_reboot() {
    say("NODE REBOOT");
    node_->reboot();
    {  // the valve must come back in the last state it durably accepted, not in a default position
      uint8_t expect = 0, out = node_->actuator_output();
      uint64_t last_seq = 0;
      for (size_t i = 0; i < node_->effects().size(); i++)
        if (node_->effects()[i].durable) { expect = node_->effects()[i].desired; last_seq = node_->effects()[i].seq; }
      bool ok = (out == expect);
      for (size_t i = 0; i < node_->in_doubt().size(); i++)  // a command that hit an I/O error may have survived
        if (node_->in_doubt()[i].seq > last_seq && node_->in_doubt()[i].desired == out) ok = true;
      if (!ok) {
        reboot_bad_++;
        say("ACTUATOR came back in the WRONG state after reboot");
      }
    }
    handshake_ = false;
    prev_unacked_ = 0;
    last_progress_ = static_cast<int64_t>(sim_.now());
    unsent_.clear();
  }

  void schedule_workload() {
    sim_.after(100, [this]() { gen_telemetry(); });
    sim_.after(wrng_.range(1500, 8000), [this]() { gen_critical(); });
    sim_.after(wrng_.range(8000, 15000), [this]() { gen_control(); });
    sim_.after(wrng_.range(4000, 9000), [this]() { gen_command(); });
  }

  void gen_telemetry() {
    if (sim_.now() >= work_end_) return;
    for (uint16_t k = 1; k <= 3; k++) submit(Cls::Telemetry, k);
    sim_.after(500, [this]() { gen_telemetry(); });
  }
  void gen_critical() {
    if (sim_.now() >= work_end_) return;
    submit(Cls::Critical, 1);
    sim_.after(wrng_.range(1500, 8000), [this]() { gen_critical(); });
  }
  void gen_control() {
    if (sim_.now() >= work_end_) return;
    submit(Cls::Control, 2);
    sim_.after(wrng_.range(8000, 15000), [this]() { gen_control(); });
    sim_.after(wrng_.range(4000, 9000), [this]() { gen_command(); });
  }

  // Operator closes / opens the valve from the gateway (absolute desired state, never a toggle).
  void gen_command() {
    if (sim_.now() >= work_end_) return;
    uint8_t desired = static_cast<uint8_t>(wrng_.below(2));
    uint64_t seq = gw_->issue_command(sim_.now(), 1, 1, desired);
    cmd_issued_.push_back(desired);
    sim_.trace(seq * 977u + desired);
    say("OPERATOR command #" + std::to_string(seq) + ": valve " + (desired ? "CLOSED" : "OPEN") +
        (link_->up() ? "" : " (link is down: queued at the gateway)"));
    sim_.after(wrng_.range(6000, 14000), [this]() { gen_command(); });
  }

  void submit(Cls cls, uint16_t key) {
    Gen g;
    g.id = next_id_++;
    g.cls = cls;
    g.key = key;
    g.val = cls == Cls::Telemetry ? ++tval_[key] : 0;
    g.t = sim_.now();
    g.acc = node_->submit(cls, key, g.id, g.val);
    gens_.push_back(g);
    apply_cnt_.resize(next_id_, 0);
    first_apply_.resize(next_id_, -1);
    sim_.trace(g.id * 31u + static_cast<uint64_t>(g.acc));
    if (cls == Cls::Critical) {
      if (g.acc == Accept::Accepted) unsent_.insert(g.id);
      say("critical event #" + std::to_string(g.id) + (g.acc == Accept::Accepted ? " ACCEPTED (local alarm fires now)"
                                                       : g.acc == Accept::Rejected ? " REJECTED (storage full)" : " I/O ERROR"));
    }
  }

  void tick() {
    int64_t now = static_cast<int64_t>(sim_.now());
    if (now > static_cast<int64_t>(end_)) return;
    sim_.after(20, [this]() { tick(); });
    {
      std::vector<std::vector<uint8_t> > cmds;
      gw_->tick(sim_.now(), cmds);  // command (re)transmissions; a down link simply drops them
      for (size_t i = 0; i < cmds.size(); i++) link_->to_node(cmds[i]);
    }
    if (!link_->up()) { handshake_ = false; return; }
    std::vector<uint8_t> buf;
    if (!handshake_) {
      if (now - last_hello_ >= 500) {
        node_->make_hello(buf);
        link_->to_gateway(buf);
        last_hello_ = now;
      }
      return;
    }
    for (int i = 0; i < 4; i++) {
      Cls c; uint32_t id;
      if (!node_->next_frame(buf, c, id)) break;
      on_sent(c, id);
      link_->to_gateway(buf);
    }
    size_t u = node_->unacked();
    if (u == 0 || u < prev_unacked_) last_progress_ = now;
    prev_unacked_ = u;
    if (u > 0 && now - last_progress_ > 1500) {  // retransmission timer: re-handshake
      handshake_ = false;
      last_progress_ = now;
    }
    if (now - last_retry_ >= 1000) { node_->retry(); last_retry_ = now; }
  }

  // Strict priority check: nothing but Critical may be sent while an accepted, unapplied critical
  // event is still waiting to be (re)sent since the last handshake.
  void on_sent(Cls c, uint32_t id) {
    sync_applied();
    if (c == Cls::Critical) unsent_.erase(id);
    else if (!unsent_.empty()) crit_first_viol_++;
  }

  void node_frame(const std::vector<uint8_t>& f) {
    sim_.trace(crc32(f.data(), f.size()) ^ 0xA5A5u);
    MsgType t;
    if (!msg_type(f.data(), f.size(), t)) return;  // corrupted frame: CRC rejects it
    node_->on_frame(f.data(), f.size());
    for (; eff_seen_ < node_->effects().size(); eff_seen_++) {
      const CmdEffect& e = node_->effects()[eff_seen_];
      say("ACTUATOR applied command #" + std::to_string(e.seq) + ": valve " + (e.desired ? "CLOSED" : "OPEN"));
    }
    if (t == MsgType::Welcome) {
      if (!handshake_) say("handshake complete, resending unacknowledged records (critical first)");
      handshake_ = true;
      last_progress_ = static_cast<int64_t>(sim_.now());
      sync_applied();
      unsent_.clear();
      std::vector<uint32_t> ids = node_->pending_critical_ids();
      for (size_t i = 0; i < ids.size(); i++)
        if (ids[i] < apply_cnt_.size() && apply_cnt_[ids[i]] == 0) unsent_.insert(ids[i]);
    }
  }

  void gateway_frame(const std::vector<uint8_t>& f) {
    sim_.trace(crc32(f.data(), f.size()) ^ 0x5A5Au);
    std::vector<std::vector<uint8_t>> replies;
    gw_->on_frame(sim_.now(), f.data(), f.size(), replies);
    sync_applied();
    for (size_t i = 0; i < replies.size(); i++) link_->to_node(replies[i]);
  }

  void sync_applied() {
    const std::vector<Applied>& a = gw_->applied();
    for (; seen_ < a.size(); seen_++) {
      const Applied& e = a[seen_];
      sim_.trace(e.id * 2654435761u ^ e.t);
      if (e.id == 0 || e.id >= apply_cnt_.size()) continue;
      if (apply_cnt_[e.id]++ == 0) {
        first_apply_[e.id] = static_cast<int64_t>(e.t);
        if (e.cls == Cls::Critical) {
          unsent_.erase(e.id);
          say("critical event #" + std::to_string(e.id) + " APPLIED at gateway (" +
              std::to_string(e.t - gens_[e.id - 1].t) + " ms after it was detected)");
        }
      }
    }
  }

  RunResult evaluate() {
    RunResult r;
    r.seed = seed_;
    r.build = build_;
    r.crit_first_violations = crit_first_viol_;
    r.journal_bytes = node_->storage_used();
    std::vector<uint64_t> lat;
    uint32_t last_gen_val[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < gens_.size(); i++) {
      const Gen& g = gens_[i];
      int cnt = apply_cnt_[g.id];
      if (cnt > 1) r.dup_effects += static_cast<uint64_t>(cnt - 1);
      if (g.cls == Cls::Critical) {
        if (g.acc == Accept::Accepted) {
          r.crit_accepted++;
          if (cnt == 0) r.crit_lost++;
          else {  // delivery delay with the time the link was DOWN subtracted
            int64_t t0 = static_cast<int64_t>(g.t), t1 = first_apply_[g.id], d = t1 - t0;
            for (size_t k = 0; k < down_spans_.size(); k++) {
              int64_t lo = std::max(t0, down_spans_[k].first), hi = std::min(t1, down_spans_[k].second);
              if (hi > lo) d -= hi - lo;
            }
            lat.push_back(d > 0 ? static_cast<uint64_t>(d) : 0);
          }
        } else if (g.acc == Accept::MaybeCommitted) r.crit_maybe++;
        else r.crit_rejected++;
      } else if (g.cls == Cls::Control) {
        if (g.acc == Accept::Accepted) { r.ctrl_accepted++; if (cnt == 0) r.ctrl_lost++; }
      } else if (g.acc == Accept::Accepted && g.key < 4) {
        last_gen_val[g.key] = g.val;
      }
    }
    uint32_t max_val[4] = {0, 0, 0, 0}, final_val[4] = {0, 0, 0, 0};
    const std::vector<Applied>& a = gw_->applied();
    for (size_t i = 0; i < a.size(); i++) {
      if (a[i].cls != Cls::Telemetry || a[i].key >= 4) continue;
      if (a[i].val < max_val[a[i].key]) r.stale_overwrites++;  // an older reading replaced a newer one
      else max_val[a[i].key] = a[i].val;
      final_val[a[i].key] = a[i].val;
    }
    for (int k = 1; k <= 3; k++)
      if (final_val[k] != last_gen_val[k]) r.tele_not_converged++;
    if (!lat.empty()) {
      std::sort(lat.begin(), lat.end());
      r.crit_p99_ms = lat[(lat.size() * 99) / 100 >= lat.size() ? lat.size() - 1 : (lat.size() * 99) / 100];
    }
    {  // operator commands: ground truth is the issue order (seq 1,2,3...)
      const std::vector<CmdEffect>& eff = node_->effects();
      std::vector<int> cnt(cmd_issued_.size() + 2, 0);
      uint64_t maxseq = 0;
      for (size_t i = 0; i < eff.size(); i++) {
        if (eff[i].seq == 0 || eff[i].seq >= cnt.size()) continue;
        cnt[eff[i].seq]++;
        if (eff[i].seq < maxseq) r.cmd_stale_applied++;  // an older decision overrode a newer one
        else maxseq = eff[i].seq;
      }
      for (size_t i = 0; i < cnt.size(); i++) if (cnt[i] > 1) r.cmd_dup_effects += static_cast<uint64_t>(cnt[i] - 1);
      r.cmd_issued = cmd_issued_.size();
      for (size_t i = 0; i < eff.size(); i++) if (!eff[i].durable) r.cmd_not_converged++;  // acted before durable
      r.cmd_not_converged += reboot_bad_;                                                    // forgot state at reboot
      if (!cmd_issued_.empty() && node_->actuator_output() != cmd_issued_.back()) r.cmd_not_converged++;  // ended wrong
    }
    r.trace_hash = sim_.trace_hash();
    say("run finished: critical accepted=" + std::to_string(r.crit_accepted) + " lost=" + std::to_string(r.crit_lost) +
        " duplicate effects=" + std::to_string(r.dup_effects) + " stale overwrites=" + std::to_string(r.stale_overwrites) +
        " | commands issued=" + std::to_string(r.cmd_issued) + " stale applied=" + std::to_string(r.cmd_stale_applied) +
        " duplicate effects=" + std::to_string(r.cmd_dup_effects) + " final valve " + (r.cmd_not_converged ? "WRONG" : "correct"));
    return r;
  }

  uint64_t seed_;
  Build build_;
  ScenarioOptions opt_;
  std::ostream* story_;
  Sim sim_;
  Rng frng_, wrng_, crng_;
  uint64_t work_end_ = 0, end_ = 0;
  std::unique_ptr<Link> link_;
  std::unique_ptr<NodeEngine> node_;
  std::unique_ptr<GatewayEngine> gw_;

  std::vector<Gen> gens_;
  std::vector<int> apply_cnt_ = std::vector<int>(1, 0);
  std::vector<int64_t> first_apply_ = std::vector<int64_t>(1, -1);
  std::vector<std::pair<int64_t, int64_t> > down_spans_;
  int64_t down_since_ = 0;
  uint32_t next_id_ = 1;
  uint32_t tval_[4] = {0, 0, 0, 0};
  size_t seen_ = 0;
  std::set<uint32_t> unsent_;
  std::vector<uint8_t> cmd_issued_;
  size_t eff_seen_ = 0;
  uint64_t reboot_bad_ = 0;
  uint64_t crit_first_viol_ = 0;

  bool handshake_ = false;
  int down_ = 0;
  int64_t last_hello_ = -1000000, last_progress_ = 0, last_retry_ = 0;
  size_t prev_unacked_ = 0;
};

}  // namespace

RunResult run_scenario(uint64_t seed, Build build, const ScenarioOptions& opt, std::ostream* story) {
  Runner r(seed, build, opt, story);
  return r.run();
}

std::string csv_header() {
  return "seed,build,crit_accepted,crit_lost,crit_maybe,crit_rejected,ctrl_accepted,ctrl_lost,dup_effects,"
         "stale_overwrites,crit_first_violations,crit_p99_ms,tele_not_converged,cmd_issued,cmd_dup_effects,cmd_stale_applied,"
         "cmd_not_converged,journal_bytes,trace_hash";
}

std::string csv_row(const RunResult& r) {
  std::ostringstream o;
  o << r.seed << ',' << build_name(r.build) << ',' << r.crit_accepted << ',' << r.crit_lost << ',' << r.crit_maybe
    << ',' << r.crit_rejected << ',' << r.ctrl_accepted << ',' << r.ctrl_lost << ',' << r.dup_effects << ','
    << r.stale_overwrites << ',' << r.crit_first_violations << ',' << r.crit_p99_ms << ',' << r.tele_not_converged
    << ',' << r.cmd_issued << ',' << r.cmd_dup_effects << ',' << r.cmd_stale_applied << ',' << r.cmd_not_converged
    << ',' << r.journal_bytes << ',' << r.trace_hash;
  return o.str();
}

}}  // namespace lifeline::sim
