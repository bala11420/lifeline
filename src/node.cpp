#include "lifeline/node.h"

namespace lifeline {

bool Node::boot() {
  std::vector<JournalEntry> entries;
  journal_.recover(entries);

  uint32_t max_epoch = 0;
  uint64_t acked[2] = {0, 0};
  uint64_t max_seq[2] = {0, 0};
  std::vector<Record> data[2];
  for (const JournalEntry& e : entries) {
    if (e.kind == Kind::Epoch) {
      if (e.epoch > max_epoch) max_epoch = e.epoch;
    } else if (e.kind == Kind::Data) {
      if (e.rec.cls == Cls::Telemetry) continue;
      int i = idx(e.rec.cls);
      data[i].push_back(e.rec);
      if (e.rec.seq > max_seq[i]) max_seq[i] = e.rec.seq;
    } else if (e.kind == Kind::Ack) {
      if (e.ack_cls == Cls::Telemetry) continue;
      int i = idx(e.ack_cls);
      if (e.ack_upto > acked[i]) acked[i] = e.ack_upto;
    }
  }
  for (int i = 0; i < 2; i++) {
    Stream& s = streams_[i];
    s = Stream();
    s.acked = acked[i];
    s.next_seq = (max_seq[i] > acked[i] ? max_seq[i] : acked[i]) + 1;
    for (const Record& r : data[i])
      if (r.seq > acked[i]) s.pending.push_back(r);
  }
  persisted_ack_[0] = acked[0];
  persisted_ack_[1] = acked[1];
  epoch_rejected_ = false;
  tele_.clear();
  tele_version_.clear();
  tele_started_ = false;
  fault_ = false;

  epoch_ = max_epoch + 1;
  return journal_.append_epoch(epoch_) == Journal::Result::Ok;
}

Hello Node::hello() const {
  Hello h;
  h.node = cfg_.node_id;
  h.epoch = epoch_;
  h.crit_next_seq = streams_[0].next_seq;
  h.ctrl_next_seq = streams_[1].next_seq;
  return h;
}

SubmitResult Node::submit(Cls cls, uint16_t key, const void* data, size_t len, uint64_t* seq_out) {
  int c = static_cast<int>(cls);
  if (len > kMaxPayload) return SubmitResult::TooLarge;

  Record r;
  r.node = cfg_.node_id;
  r.epoch = epoch_;
  r.cls = cls;
  r.key = key;
  r.len = static_cast<uint16_t>(len);
  r.ts_ms = static_cast<uint32_t>(clock_.now_ms());
  if (len) std::memcpy(r.payload, data, len);

  if (cls == Cls::Telemetry) {
    r.seq = ++tele_version_[key];
    Reg& g = tele_[key];
    g.rec = r;
    g.dirty = true;
    stats_.accepted[c]++;
    if (seq_out) *seq_out = r.seq;
    return SubmitResult::Accepted;
  }

  Stream& s = streams_[idx(cls)];
  r.seq = s.next_seq;
  Journal::Result jr = journal_.append_data(r);
  if (jr == Journal::Result::Full) {
    stats_.rejected_full[c]++;
    fault_ = true;
    return SubmitResult::StorageFull;
  }
  if (jr == Journal::Result::IoError) {
    stats_.rejected_io[c]++;
    fault_ = true;
    return SubmitResult::IoError;
  }
  s.pending.push_back(r);
  s.next_seq++;
  stats_.accepted[c]++;
  if (seq_out) *seq_out = r.seq;
  return SubmitResult::Accepted;
}

bool Node::poll_send(Record& out) {
  for (int i = 0; i < 2; i++) {
    Stream& s = streams_[i];
    if (s.cursor < s.pending.size()) {
      out = s.pending[s.cursor++];
      stats_.sent[i]++;
      return true;
    }
  }
  if (tele_.empty()) return false;
  // Round-robin over dirty registers in key order, starting after the last key sent.
  std::map<uint16_t, Reg>::iterator it = tele_started_ ? tele_.upper_bound(tele_last_) : tele_.begin();
  for (int pass = 0; pass < 2; pass++) {
    for (; it != tele_.end(); ++it) {
      if (it->second.dirty) {
        it->second.dirty = false;
        tele_last_ = it->first;
        tele_started_ = true;
        out = it->second.rec;
        stats_.sent[2]++;
        return true;
      }
    }
    it = tele_.begin();
  }
  return false;
}

void Node::on_ack(Cls cls, uint64_t upto) {
  if (cls == Cls::Telemetry) return;
  Stream& s = streams_[idx(cls)];
  if (upto >= s.next_seq) return;  // ack for records that never existed: ignore it entirely, never
                                   // clamp, or a bogus ack would delete unsent critical records
  if (upto <= s.acked) return;
  size_t popped = 0;
  while (!s.pending.empty() && s.pending.front().seq <= upto) {
    s.pending.pop_front();
    popped++;
  }
  s.cursor = s.cursor > popped ? s.cursor - popped : 0;
  s.acked = upto;
  int i = idx(cls);
  if (upto - persisted_ack_[i] >= cfg_.ack_batch &&
      journal_.append_ack(cls, upto) == Journal::Result::Ok)
    persisted_ack_[i] = upto;  // a lost or skipped ack record only causes a harmless resend
}

void Node::on_resync(uint64_t crit_hwm, uint64_t ctrl_hwm) {
  on_ack(Cls::Critical, crit_hwm);
  on_ack(Cls::Control, ctrl_hwm);
  streams_[0].cursor = 0;
  streams_[1].cursor = 0;
  tele_started_ = false;
  for (std::map<uint16_t, Reg>::iterator it = tele_.begin(); it != tele_.end(); ++it)
    it->second.dirty = true;
}

bool Node::on_welcome(const Welcome& w) {
  on_resync(w.crit_hwm, w.ctrl_hwm);
  if (!w.epoch_ok) epoch_rejected_ = true;
  return w.epoch_ok;
}

void Node::on_telemetry_ack(uint32_t epoch, uint16_t key, uint64_t version) {
  if (epoch != epoch_) return;
  std::map<uint16_t, Reg>::iterator it = tele_.find(key);
  if (it != tele_.end() && version > it->second.acked_version) it->second.acked_version = version;
}

size_t Node::retry_telemetry() {
  size_t n = 0;
  for (std::map<uint16_t, Reg>::iterator it = tele_.begin(); it != tele_.end(); ++it) {
    if (it->second.rec.seq > it->second.acked_version && !it->second.dirty) {
      it->second.dirty = true;
      n++;
    }
  }
  return n;
}

std::vector<Record> Node::pending_records(Cls cls) const {
  std::vector<Record> v;
  if (cls == Cls::Telemetry) return v;
  const Stream& s = streams_[idx(cls)];
  v.assign(s.pending.begin(), s.pending.end());
  return v;
}

size_t Node::unacked(Cls cls) const {
  return cls == Cls::Telemetry ? 0 : streams_[idx(cls)].pending.size();
}

size_t Node::unsent(Cls cls) const {
  if (cls == Cls::Telemetry) return 0;
  const Stream& s = streams_[idx(cls)];
  return s.pending.size() - s.cursor;
}

size_t Node::dirty_telemetry() const {
  size_t n = 0;
  for (std::map<uint16_t, Reg>::const_iterator it = tele_.begin(); it != tele_.end(); ++it)
    if (it->second.dirty) n++;
  return n;
}

}  // namespace lifeline
