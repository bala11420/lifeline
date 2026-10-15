// Actuator (valve / siren / ventilation) with a PERSISTENT command gate.
//
// Rules: commands are ABSOLUTE desired states ordered by (term, seq). A command is applied only if
// strictly newer than the last applied one. The new (term, seq, state) is made durable BEFORE the
// output changes, and at boot the output is reconciled to the persisted state. So:
//   - duplicate and stale commands cannot introduce a newer decision, even after a reboot
//   - accepted commands persist a desired state; repeated application converges to that state
//   - a crash between "persist" and "apply" does not lose the command (boot re-applies the state)
//   - a failed persist means the command is NOT applied and NOT acked, so the gateway retries
// This gives idempotent convergence to the desired state; it is not a claim of exactly-once
// physical actuation.
#pragma once
#include "lifeline/dedup.h"
#include "lifeline/journal.h"
#include "lifeline/messages.h"

namespace lifeline {

class Actuator {
 public:
  enum class Result { Applied, Stale, IoError };
  Actuator(Flash& flash, uint16_t actuator_id) : journal_(flash, 0, 0), id_(actuator_id) {}

  // Recover the persisted gate and drive the output to the persisted desired state.
  void boot();
  Result on_command(const Command& c);

  uint8_t output() const { return output_; }  // 0 until a command was ever applied
  bool has_applied() const { return gate_.have(); }
  uint32_t term() const { return gate_.major(); }
  uint64_t seq() const { return gate_.minor(); }

 private:
  Journal journal_;
  uint16_t id_;
  VersionGate gate_;
  uint8_t output_ = 0;
};

}  // namespace lifeline
