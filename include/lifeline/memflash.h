// In-memory Flash with crash-injection hooks, for the simulator and the tests.
#pragma once
#include <vector>
#include "lifeline/protocol.h"

namespace lifeline {

class MemFlash : public Flash {
 public:
  explicit MemFlash(size_t capacity) : cap_(capacity) {}

  size_t size() const override { return d_.size(); }
  size_t capacity() const override { return cap_; }
  size_t read(size_t off, void* buf, size_t n) const override {
    if (off >= d_.size()) return 0;
    if (n > d_.size() - off) n = d_.size() - off;
    if (n) std::memcpy(buf, d_.data() + off, n);
    return n;
  }
  bool append(const void* buf, size_t n) override {
    if (dead_) return false;
    if (d_.size() + n > cap_) return false;
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    if (budget_ >= 0 && static_cast<size_t>(budget_) < n) {  // power fails mid-write
      d_.insert(d_.end(), p, p + budget_);                    // torn write: only a prefix lands
      budget_ = 0;
      dead_ = true;
      return false;
    }
    d_.insert(d_.end(), p, p + n);
    if (budget_ >= 0) budget_ -= static_cast<long>(n);
    return true;
  }
  void truncate(size_t n) override {
    if (dead_) return;
    if (n < d_.size()) d_.resize(n);
    if (synced_ > d_.size()) synced_ = d_.size();
  }
  bool sync() override {
    if (dead_) return false;
    if (sync_fail_armed_) {  // power dies at the flush: bytes written but never made durable
      sync_fail_armed_ = false;
      dead_ = true;
      return false;
    }
    synced_ = d_.size();
    return true;
  }

  // ---- test / simulator hooks ----
  // After `bytes` more bytes are written, the next write is torn and the device goes dead.
  void arm_crash_after(size_t bytes) { budget_ = static_cast<long>(bytes); }
  // The next sync() fails and the device goes dead (power loss between write and flush).
  void arm_sync_failure() { sync_fail_armed_ = true; }
  // Reboot: the device works again. With lose_unsynced=true, bytes never successfully synced
  // are lost (the realistic outcome of a failed flush); otherwise they stay (a torn tail).
  void power_cycle(bool lose_unsynced = false) {
    dead_ = false;
    budget_ = -1;
    sync_fail_armed_ = false;
    if (lose_unsynced) drop_unsynced();
  }
  void drop_unsynced() { if (synced_ < d_.size()) d_.resize(synced_); }
  void load(const std::vector<uint8_t>& bytes) { d_ = bytes; synced_ = d_.size(); }
  const std::vector<uint8_t>& bytes() const { return d_; }
  bool dead() const { return dead_; }

 private:
  size_t cap_;
  std::vector<uint8_t> d_;
  size_t synced_ = 0;
  bool dead_ = false;
  bool sync_fail_armed_ = false;
  long budget_ = -1;
};

}  // namespace lifeline
