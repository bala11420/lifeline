#include "lifeline/actuator.h"
#include <vector>
#include "lifeline/codec.h"

namespace lifeline {

namespace {
// Frame: [u8 body_len=13][u8 desired][u32 term][u64 seq][u32 crc32 of the first 14 bytes] = 18 bytes
constexpr size_t kFrame = 18;

// Returns the offset after the last valid frame; reports the highest committed (term, seq).
size_t scan(const std::vector<uint8_t>& b, size_t total, bool& have, uint32_t& term, uint64_t& seq,
            uint8_t& desired) {
  size_t off = 0;
  have = false;
  while (off + kFrame <= total) {
    const uint8_t* p = b.data() + off;
    if (p[0] != 13 || crc32(p, kFrame - 4) != get_u32(p + kFrame - 4) || p[1] > 1) break;
    uint32_t t = get_u32(p + 2);
    uint64_t s = get_u64(p + 6);
    if (!have || t > term || (t == term && s > seq)) {
      have = true; term = t; seq = s; desired = p[1];
    }
    off += kFrame;
  }
  return off;
}
}  // namespace

bool Actuator::boot() {
  std::vector<uint8_t> buf(f_.size());
  size_t total = buf.empty() ? 0 : f_.read(0, buf.data(), buf.size());
  uint8_t d = 0;
  size_t good = scan(buf, total, have_, term_, seq_, d);
  if (have_) desired_ = d ? Valve::Closed : Valve::Open;
  poisoned_ = false;
  if (good < total) {
    f_.truncate(good);
    if (!f_.sync() || f_.size() != good) poisoned_ = true;
  }
  return !poisoned_;
}

bool Actuator::heal() {
  std::vector<uint8_t> buf(f_.size());
  size_t total = buf.empty() ? 0 : f_.read(0, buf.data(), buf.size());
  bool h; uint32_t t; uint64_t s; uint8_t d;
  size_t good = scan(buf, total, h, t, s, d);
  if (good < total) {
    f_.truncate(good);
    if (!f_.sync() || f_.size() != good) return false;
  }
  poisoned_ = false;
  return true;
}

CmdResult Actuator::on_command(const Cmd& c) {
  if (have_ && (c.term < term_ || (c.term == term_ && c.seq < seq_))) return CmdResult::Stale;
  if (have_ && c.term == term_ && c.seq == seq_) return CmdResult::Duplicate;
  if (poisoned_ && !heal()) return CmdResult::IoError;
  std::vector<uint8_t> fr;
  put_u8(fr, 13);
  put_u8(fr, c.desired ? 1 : 0);
  put_u32(fr, c.term);
  put_u64(fr, c.seq);
  put_u32(fr, crc32(fr.data(), fr.size()));
  size_t before = f_.size();
  if (!f_.append(fr.data(), fr.size()) || !f_.sync()) {
    f_.truncate(before);
    poisoned_ = true;  // heal() on the next command cuts any surviving or torn bytes
    return CmdResult::IoError;
  }
  have_ = true; term_ = c.term; seq_ = c.seq;
  desired_ = c.desired ? Valve::Closed : Valve::Open;
  return CmdResult::Applied;
}

}  // namespace lifeline
