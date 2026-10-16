// Actuator-side command handling: desired-state commands ordered by (term, seq), persisted
// BEFORE they take effect, plus a local safety interlock that the network cannot override.
#pragma once
#include "ll_messages.h"
#include "ll_protocol.h"

namespace lifeline {

enum class Valve : uint8_t { Open = 0, Closed = 1 };
// Applied: newer command committed. Duplicate: already committed (same version), harmless.
// Stale: older than the committed version, rejected. IoError: could not be made durable.
enum class CmdResult { Applied, Duplicate, Stale, IoError };

class Actuator {
 public:
  explicit Actuator(Flash& f) : f_(f) {}

  // Recovers the highest committed command (torn tail removed). Output is reconciled from it.
  bool boot();
  CmdResult on_command(const Cmd& c);

  // Local hazard input (e.g. gas alarm). While set, the output is forced Closed no matter what
  // the committed desired state says; clearing it re-reconciles to the desired state.
  void set_interlock(bool on) { interlock_ = on; }
  bool interlock() const { return interlock_; }

  Valve desired() const { return desired_; }
  Valve output() const { return (interlock_ && desired_ == Valve::Open) ? Valve::Closed : desired_; }
  bool have() const { return have_; }
  uint32_t term() const { return term_; }
  uint64_t seq() const { return seq_; }

 private:
  bool heal();
  Flash& f_;
  bool have_ = false;
  bool poisoned_ = false;
  bool interlock_ = false;
  uint32_t term_ = 0;
  uint64_t seq_ = 0;
  Valve desired_ = Valve::Open;  // normal operation until the first committed command
};

}  // namespace lifeline
