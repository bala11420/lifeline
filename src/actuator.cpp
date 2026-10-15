#include "lifeline/actuator.h"

namespace lifeline {

void Actuator::boot() {
  std::vector<JournalEntry> entries;
  journal_.recover(entries);
  gate_ = VersionGate();
  output_ = 0;
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].kind != Kind::Cmd) continue;
    gate_.restore(entries[i].cmd_term, entries[i].cmd_seq);
    output_ = entries[i].cmd_state;  // reconcile: output follows the persisted desired state
  }
}

Actuator::Result Actuator::on_command(const Command& c) {
  if (c.actuator != id_) return Result::Stale;
  if (!gate_.newer(c.term, c.seq)) return Result::Stale;  // duplicate, replay or older decision
  if (journal_.append_cmd(c.term, c.seq, c.desired) != Journal::Result::Ok) return Result::IoError;
  gate_.accept(c.term, c.seq);
  output_ = c.desired;  // durable first, then act
  return Result::Applied;
}

}  // namespace lifeline
