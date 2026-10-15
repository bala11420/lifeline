// Deterministic discrete-event simulator: virtual clock + event queue + faulty link.
// Single-threaded, virtual time, seeded pseudorandom sampling. A run is replayable under the same
// seed, configuration and build. Events at the same virtual time run in the order they were
// scheduled (tie-break by insertion order). Virtual time advances without waiting for equivalent
// wall-clock time; throughput is measured separately, not guaranteed.
#pragma once
#include <cstdint>
#include <functional>
#include <queue>
#include <vector>
#include "lifeline/protocol.h"
#include "sim/rng.h"

namespace lifeline { namespace sim {

class Sim {
 public:
  uint64_t now() const { return now_; }
  void at(uint64_t t, std::function<void()> fn);
  void after(uint64_t dt, std::function<void()> fn) { at(now_ + dt, fn); }
  void run_until(uint64_t t);  // runs every event with time <= t, then sets now = t
  void trace(uint64_t v) { hash_ = (hash_ ^ v) * 1099511628211ULL; }  // FNV-style event-trace hash
  uint64_t trace_hash() const { return hash_; }

 private:
  struct Ev { uint64_t t; uint64_t order; std::function<void()> fn; };
  struct Later {
    bool operator()(const Ev& a, const Ev& b) const { return a.t != b.t ? a.t > b.t : a.order > b.order; }
  };
  std::priority_queue<Ev, std::vector<Ev>, Later> q_;
  uint64_t now_ = 0, order_ = 0;
  uint64_t hash_ = 1469598103934665603ULL;
};

struct SimClock : Clock {
  explicit SimClock(Sim& s) : s_(s) {}
  uint64_t now_ms() override { return s_.now(); }
  Sim& s_;
};

struct LinkConfig {
  uint32_t base_ms = 20, jitter_ms = 15;
  uint32_t loss_ppm = 0, dup_ppm = 0, corrupt_ppm = 0;  // parts per million, per frame
  uint32_t reorder_ppm = 0, reorder_extra_ms = 300;     // a reordered frame gets extra random delay
};

struct LinkStats { uint64_t sent = 0, dropped = 0, duplicated = 0, corrupted = 0, delivered = 0; };

// Bidirectional frame link. A partition (set_up(false)) also kills frames already in flight.
class Link {
 public:
  typedef std::function<void(const std::vector<uint8_t>&)> Handler;
  Link(Sim& s, Rng& r, const LinkConfig& c) : sim_(s), rng_(r), cfg_(c) {}
  void set_up(bool u) { up_ = u; }
  bool up() const { return up_; }
  void on_gateway(Handler h) { gw_ = h; }
  void on_node(Handler h) { node_ = h; }
  void to_gateway(const std::vector<uint8_t>& f) { carry(f, true); }
  void to_node(const std::vector<uint8_t>& f) { carry(f, false); }
  const LinkStats& stats() const { return stats_; }

 private:
  void carry(const std::vector<uint8_t>& f, bool to_gw);
  Sim& sim_;
  Rng& rng_;
  LinkConfig cfg_;
  bool up_ = true;
  Handler gw_, node_;
  LinkStats stats_;
};

}}  // namespace lifeline::sim
