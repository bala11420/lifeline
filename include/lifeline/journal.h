#pragma once
#include <vector>
#include "lifeline/protocol.h"

namespace lifeline {

// On-flash frame:  [u32 len][len body bytes][u32 crc32(len bytes + body)]
// Body kinds: Data (a Record), Ack (class + cumulative seq), Epoch (boot epoch), Cmd (actuator).
// Recovery rejects an incomplete or corrupt trailing frame and returns the verified committed
// prefix. Length and CRC identify complete frames; durability comes only from a successful sync().
// Current prototype: fixed-capacity journal without reclamation. Capacity exhaustion is explicit
// (Result::Full is returned and counted); nothing is ever reclaimed or compacted.
enum class Kind : uint8_t { Data = 1, Ack = 2, Epoch = 3, Cmd = 4 };

struct JournalEntry {
  Kind kind = Kind::Data;
  Record rec;                       // Data
  Cls ack_cls = Cls::Critical;      // Ack
  uint64_t ack_upto = 0;            // Ack
  uint32_t epoch = 0;               // Epoch
  uint32_t cmd_term = 0;            // Cmd (actuator store)
  uint64_t cmd_seq = 0;
  uint8_t cmd_state = 0;
};

class Journal {
 public:
  enum class Result { Ok, Full, IoError };

  // Space policy (from the end of the capacity backwards):
  //   `system_reserve`   bytes: only Epoch records (so a full journal can still start a new epoch)
  //   `critical_reserve` bytes: Critical records and acks; Control records may not use them
  Journal(Flash& f, size_t critical_reserve, size_t system_reserve = 0)
      : f_(f), reserve_(critical_reserve), sys_(system_reserve) {}

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
  Result append_cmd(uint32_t term, uint64_t seq, uint8_t state);  // actuator: last applied command

  size_t used() const { return f_.size(); }
  size_t capacity() const { return f_.capacity(); }
  size_t discarded_tail_bytes() const { return discarded_; }
  bool poisoned() const { return poisoned_; }

 private:
  Result append_frame(const std::vector<uint8_t>& body, size_t limit);
  size_t room(size_t reserved) const { return f_.capacity() > reserved ? f_.capacity() - reserved : 0; }
  Flash& f_;
  size_t reserve_;
  size_t sys_;
  size_t discarded_ = 0;
  bool poisoned_ = false;  // set after a failed append until recover() runs again
};

}  // namespace lifeline
