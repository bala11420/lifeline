#pragma once
#include <vector>
#include "ll_protocol.h"

namespace lifeline {

// On-flash frame:  [u32 len][len body bytes][u32 crc32(len bytes + body)]
// Body kinds: Data (a Record), Ack (class + cumulative seq), Epoch (boot epoch).
enum class Kind : uint8_t { Data = 1, Ack = 2, Epoch = 3 };

struct JournalEntry {
  Kind kind = Kind::Data;
  Record rec;                       // Data
  Cls ack_cls = Cls::Critical;      // Ack
  uint64_t ack_upto = 0;            // Ack
  uint32_t epoch = 0;               // Epoch
};

class Journal {
 public:
  enum class Result { Ok, Full, IoError };

  // `critical_reserve` bytes at the end of the capacity can only be used by Critical records.
  Journal(Flash& f, size_t critical_reserve) : f_(f), reserve_(critical_reserve) {}

  // Scans the log, truncates a torn or corrupt tail, and returns every valid entry in order.
  // Must be called after every boot and before any append.
  size_t recover(std::vector<JournalEntry>& out);

  // Re-scans after a failed append, removes any torn tail and re-enables appends WITHOUT a reboot.
  // Called automatically by the next append; returns false if the device is still failing.
  bool heal();

  // Ok means the entry is durably committed: written AND sync() returned true.
  Result append_data(const Record& r);
  Result append_ack(Cls c, uint64_t upto);
  Result append_epoch(uint32_t epoch);

  size_t used() const { return f_.size(); }
  size_t capacity() const { return f_.capacity(); }
  size_t discarded_tail_bytes() const { return discarded_; }
  bool poisoned() const { return poisoned_; }

 private:
  Result append_frame(const std::vector<uint8_t>& body, bool may_use_reserve);
  Flash& f_;
  size_t reserve_;
  size_t discarded_ = 0;
  size_t fail_off_ = static_cast<size_t>(-1);  // size before the append that failed
  bool poisoned_ = false;  // set after a failed append until recover() runs again
};

}  // namespace lifeline
