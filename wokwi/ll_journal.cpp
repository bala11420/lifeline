#include "ll_journal.h"
#include "ll_codec.h"

namespace lifeline {

namespace {
constexpr size_t kMaxBody = 128;  // any frame claiming more is treated as corrupt

// Walks valid frames from offset 0. Returns the offset just past the last valid frame.
size_t scan(const std::vector<uint8_t>& buf, size_t total, std::vector<JournalEntry>* out) {
  size_t off = 0;
  while (off + 4 <= total) {
    uint32_t len = get_u32(buf.data() + off);
    if (len == 0 || len > kMaxBody) break;
    if (off + 4 + len + 4 > total) break;  // torn tail
    if (crc32(buf.data() + off, 4 + len) != get_u32(buf.data() + off + 4 + len)) break;
    const uint8_t* body = buf.data() + off + 4;
    JournalEntry e;
    bool ok = false;
    switch (body[0]) {
      case 1:
        e.kind = Kind::Data;
        ok = decode_body(body + 1, len - 1, e.rec);
        break;
      case 2:
        if (len == 1 + 1 + 8 && body[1] <= 2) {
          e.kind = Kind::Ack;
          e.ack_cls = static_cast<Cls>(body[1]);
          e.ack_upto = get_u64(body + 2);
          ok = true;
        }
        break;
      case 3:
        if (len == 1 + 4) {
          e.kind = Kind::Epoch;
          e.epoch = get_u32(body + 1);
          ok = true;
        }
        break;
      default:
        break;
    }
    if (!ok) break;
    if (out) out->push_back(e);
    off += 4 + len + 4;
  }
  return off;
}
}  // namespace

size_t Journal::recover(std::vector<JournalEntry>& out) {
  out.clear();
  poisoned_ = false;
  discarded_ = 0;
  std::vector<uint8_t> buf(f_.size());
  size_t total = buf.empty() ? 0 : f_.read(0, buf.data(), buf.size());
  size_t good = scan(buf, total, &out);
  if (good < total) {
    discarded_ = total - good;
    f_.truncate(good);  // drop the torn tail so later appends are not hidden behind garbage
    if (!f_.sync() || f_.size() != good) poisoned_ = true;  // could not clean up: refuse appends
  }
  return out.size();
}

bool Journal::heal() {
  std::vector<uint8_t> buf(f_.size());
  size_t total = buf.empty() ? 0 : f_.read(0, buf.data(), buf.size());
  size_t good = scan(buf, total, nullptr);
  if (good > fail_off_) good = fail_off_;  // the caller was told IoError: drop a frame that survived
  if (good < total) {
    f_.truncate(good);
    if (!f_.sync()) return false;
    if (f_.size() != good) return false;
  }
  poisoned_ = false;
  fail_off_ = static_cast<size_t>(-1);
  return true;
}

Journal::Result Journal::append_frame(const std::vector<uint8_t>& body, int tier) {
  if (poisoned_ && !heal()) return Result::IoError;
  std::vector<uint8_t> frame;
  put_u32(frame, static_cast<uint32_t>(body.size()));
  frame.insert(frame.end(), body.begin(), body.end());
  put_u32(frame, crc32(frame.data(), frame.size()));

  size_t cap = f_.capacity();
  size_t hold = tier >= 2 ? 0 : kMetaReserve + (tier == 1 ? 0 : reserve_);
  size_t limit = cap > hold ? cap - hold : 0;
  size_t before = f_.size();
  if (before + frame.size() > limit) return Result::Full;

  if (!f_.append(frame.data(), frame.size()) || !f_.sync()) {
    f_.truncate(before);  // best effort; a dead device ignores this and heal() retries later
    fail_off_ = before;
    poisoned_ = true;     // never append after a possibly-torn frame without cleaning it first
    return Result::IoError;
  }
  return Result::Ok;
}

Journal::Result Journal::append_data(const Record& r) {
  std::vector<uint8_t> body;
  put_u8(body, 1);
  encode_body(r, body);
  return append_frame(body, r.cls == Cls::Critical ? 1 : 0);
}

Journal::Result Journal::append_ack(Cls c, uint64_t upto) {
  std::vector<uint8_t> body;
  put_u8(body, 2);
  put_u8(body, static_cast<uint8_t>(c));
  put_u64(body, upto);
  return append_frame(body, 1);  // optional: a missing ack record only causes a harmless resend, so it never eats the epoch reserve
}

Journal::Result Journal::append_epoch(uint32_t epoch) {
  std::vector<uint8_t> body;
  put_u8(body, 3);
  put_u32(body, epoch);
  return append_frame(body, 2);
}

}  // namespace lifeline
