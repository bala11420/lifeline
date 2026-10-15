#include "lifeline/codec.h"

namespace lifeline {

uint32_t crc32(const void* data, size_t n, uint32_t prev) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~prev;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void put_u8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void put_u16(std::vector<uint8_t>& o, uint16_t v) {
  for (int i = 0; i < 2; i++) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; i++) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<uint8_t>& o, uint64_t v) {
  for (int i = 0; i < 8; i++) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

void encode_body(const Record& r, std::vector<uint8_t>& out) {
  put_u32(out, r.node);
  put_u32(out, r.epoch);
  put_u8(out, static_cast<uint8_t>(r.cls));
  put_u64(out, r.seq);
  put_u16(out, r.key);
  put_u16(out, r.len);
  put_u32(out, r.ts_ms);
  out.insert(out.end(), r.payload, r.payload + r.len);
}

bool decode_body(const uint8_t* p, size_t n, Record& r) {
  if (n < kBodyFixed) return false;
  uint8_t cls = p[8];
  uint16_t len = get_u16(p + 19);
  if (cls > 2 || len > kMaxPayload || n != kBodyFixed + len) return false;
  r.node = get_u32(p);
  r.epoch = get_u32(p + 4);
  r.cls = static_cast<Cls>(cls);
  r.seq = get_u64(p + 9);
  r.key = get_u16(p + 17);
  r.len = len;
  r.ts_ms = get_u32(p + 21);
  std::memset(r.payload, 0, sizeof(r.payload));
  std::memcpy(r.payload, p + kBodyFixed, len);
  return true;
}

}  // namespace lifeline
