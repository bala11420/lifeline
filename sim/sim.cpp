#include "sim/sim.h"

namespace lifeline { namespace sim {

void Sim::at(uint64_t t, std::function<void()> fn) {
  if (t < now_) t = now_;
  Ev e;
  e.t = t;
  e.order = order_++;
  e.fn = fn;
  q_.push(e);
}

void Sim::run_until(uint64_t t) {
  while (!q_.empty() && q_.top().t <= t) {
    Ev e = q_.top();
    q_.pop();
    now_ = e.t;
    e.fn();
  }
  if (t > now_) now_ = t;
}

void Link::carry(const std::vector<uint8_t>& f, bool to_gw) {
  stats_.sent++;
  if (!up_ || rng_.chance_ppm(cfg_.loss_ppm)) { stats_.dropped++; return; }
  int copies = 1;
  if (rng_.chance_ppm(cfg_.dup_ppm)) { copies = 2; stats_.duplicated++; }
  for (int i = 0; i < copies; i++) {
    std::vector<uint8_t> frame = f;
    if (!frame.empty() && rng_.chance_ppm(cfg_.corrupt_ppm)) {
      frame[rng_.below(static_cast<uint32_t>(frame.size()))] ^= static_cast<uint8_t>(1 + rng_.below(255));
      stats_.corrupted++;
    }
    uint64_t delay = cfg_.base_ms + rng_.below(cfg_.jitter_ms + 1);
    if (rng_.chance_ppm(cfg_.reorder_ppm)) delay += rng_.below(cfg_.reorder_extra_ms + 1);
    sim_.after(delay, [this, frame, to_gw]() {
      if (!up_) { stats_.dropped++; return; }  // cable cut while the frame was in flight
      stats_.delivered++;
      const Handler& h = to_gw ? gw_ : node_;
      if (h) h(frame);
    });
  }
}

}}  // namespace lifeline::sim
