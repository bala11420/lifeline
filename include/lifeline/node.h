#pragma once
#include <deque>
#include <map>
#include <vector>
#include "lifeline/journal.h"
#include "lifeline/messages.h"
#include "lifeline/protocol.h"

namespace lifeline {

struct NodeConfig {
  uint32_t node_id = 1;
  size_t critical_reserve = 512;  // journal bytes only Critical records (and acks) may use
  // Journal bytes only Epoch records may use, so even a completely full journal can start a new
  // boot epoch. 128 bytes = about 9 reboots. Without it a reused epoch makes the gateway reject
  // the node's telemetry as stale (found by the simulator on a 600-byte journal).
  size_t system_reserve = 128;
  // Persist an ack record only after this many newly acked records. A lost ack is harmless (the
  // gateway's Welcome re-trims on reconnect) but every ack record costs journal space. 1 = always.
  size_t ack_batch = 8;
};

enum class SubmitResult { Accepted, StorageFull, IoError, TooLarge };

struct NodeStats {
  uint64_t accepted[3] = {0, 0, 0};
  uint64_t rejected_full[3] = {0, 0, 0};
  uint64_t rejected_io[3] = {0, 0, 0};
  uint64_t sent[3] = {0, 0, 0};
};

// The node-side protocol engine. It never touches the network: the driver loop calls
// poll_send(), moves the record over whatever link exists, and feeds acks back in.
//
// Guarantee (see README): a record for which submit() returned Accepted is durably committed
// (Critical/Control) and survives modelled crashes. Telemetry is a latest-value register kept
// in RAM and is intentionally not durable. Telemetry storage is one register per configured key
// and does not grow with outage duration.
class Node {
 public:
  Node(Flash& flash, Clock& clock, const NodeConfig& cfg)
      : flash_(flash), clock_(clock), cfg_(cfg), journal_(flash, cfg.critical_reserve, cfg.system_reserve) {}

  // Recover the journal, rebuild unacked streams, start a new boot epoch (previous committed
  // epoch + 1, appended to the journal). Returns false if the new epoch could not be persisted:
  // in that case the epoch is NOT durably committed and the caller must not treat the node as
  // started (a later boot could reuse the same epoch number).
  bool boot();

  uint32_t epoch() const { return epoch_; }
  Hello hello() const;

  // Critical/Control: Accepted means durably committed. Telemetry: replaces the register.
  SubmitResult submit(Cls cls, uint16_t key, const void* data, size_t len, uint64_t* seq_out = nullptr);

  // Next record to put on the wire, strict priority Critical > Control > Telemetry.
  // A Critical record submitted at any time is chosen at the very next call.
  bool poll_send(Record& out);

  // Cumulative acknowledgement from the gateway for a stream.
  void on_ack(Cls cls, uint64_t upto);  // ignores any ack above the highest seq ever issued

  // Gateway reply to hello(): same as on_resync(), also records an epoch refusal.
  // Returns false if the gateway refused this node's epoch (see epoch_rejected()).
  bool on_welcome(const Welcome& w);
  bool epoch_rejected() const { return epoch_rejected_; }

  // Telemetry convergence: the gateway acks (epoch, key, version); the driver calls
  // retry_telemetry() on a timer to re-mark every register whose newest value is not acked yet.
  void on_telemetry_ack(uint32_t epoch, uint16_t key, uint64_t version);
  size_t retry_telemetry();

  // After (re)connecting: the gateway's cumulative marks. Trims acked records, rewinds the send
  // cursors so everything still unacked is sent again (critical first), re-marks telemetry dirty.
  void on_resync(uint64_t crit_hwm, uint64_t ctrl_hwm);

  size_t unacked(Cls cls) const;  // stream records not yet acknowledged
  // Unacked records of a stream (Critical or Control only), oldest first. For drivers and checks.
  const std::deque<Record>& pending_records(Cls cls) const { return streams_[idx(cls)].pending; }
  size_t unsent(Cls cls) const;   // stream records not yet transmitted since last resync
  size_t dirty_telemetry() const;
  bool storage_fault() const { return fault_; }  // sticky: some record was rejected for storage
  const NodeStats& stats() const { return stats_; }
  size_t journal_used() const { return journal_.used(); }

 private:
  struct Stream {
    std::deque<Record> pending;
    size_t cursor = 0;
    uint64_t next_seq = 1;
    uint64_t acked = 0;
  };
  struct Reg {
    Record rec;
    bool dirty = false;
    uint64_t acked_version = 0;
  };
  static int idx(Cls c) { return c == Cls::Critical ? 0 : 1; }

  Flash& flash_;
  Clock& clock_;
  NodeConfig cfg_;
  Journal journal_;
  uint32_t epoch_ = 0;
  Stream streams_[2];
  std::map<uint16_t, Reg> tele_;
  std::map<uint16_t, uint64_t> tele_version_;
  uint64_t persisted_ack_[2] = {0, 0};
  bool epoch_rejected_ = false;
  uint16_t tele_last_ = 0;
  bool tele_started_ = false;
  bool fault_ = false;
  NodeStats stats_;
};

}  // namespace lifeline
