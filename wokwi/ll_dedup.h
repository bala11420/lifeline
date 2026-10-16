// Receiver-side (gateway / actuator) ordering and duplicate rules.
#pragma once
#include <set>
#include "ll_protocol.h"

namespace lifeline {

// Exact duplicate detection for a Critical or Control stream (sequence numbers start at 1).
// NOT a sliding window: a legitimately late record is never rejected for being "too old".
// cumulative() is the cumulative acknowledgement: every seq in [1, cumulative()] is committed.
class StreamTracker {
 public:
  bool accept(uint64_t seq) {  // true if new, false if a duplicate
    if (seq == 0 || seq <= low_) return false;
    if (!above_.insert(seq).second) return false;
    while (!above_.empty() && *above_.begin() == low_ + 1) {
      ++low_;
      above_.erase(above_.begin());
    }
    return true;
  }
  // Strict in-order mode for Control streams / commands whose effects must apply in sequence.
  // Gap means: do NOT apply, reply with cumulative() so the node rewinds (on_resync) and resends.
  // Do not mix accept() and accept_in_order() on the same tracker.
  enum class Order { Applied, Duplicate, Gap };
  Order accept_in_order(uint64_t seq) {
    if (seq == 0 || seq <= low_) return Order::Duplicate;
    if (seq == low_ + 1) { ++low_; return Order::Applied; }
    return Order::Gap;
  }
  uint64_t expected() const { return low_ + 1; }
  uint64_t cumulative() const { return low_; }
  size_t out_of_order() const { return above_.size(); }

 private:
  uint64_t low_ = 0;
  std::set<uint64_t> above_;
};

// "Newer wins" gate for one value: telemetry per (node,key) ordered by (epoch, version), or an
// actuator command ordered by (term, sequence). Accepts only strictly newer pairs.
// The actuator must persist this state, or a reboot would forget what it already applied.
class VersionGate {
 public:
  bool accept(uint32_t major, uint64_t minor) {
    if (have_ && (major < major_ || (major == major_ && minor <= minor_))) return false;
    have_ = true;
    major_ = major;
    minor_ = minor;
    return true;
  }
  bool have() const { return have_; }
  uint32_t major() const { return major_; }
  uint64_t minor() const { return minor_; }
  void restore(uint32_t major, uint64_t minor) { have_ = true; major_ = major; minor_ = minor; }

 private:
  bool have_ = false;
  uint32_t major_ = 0;
  uint64_t minor_ = 0;
};

}  // namespace lifeline
