#pragma once
#include <cstring>
#include "lifeline/memflash.h"
#include "lifeline/protocol.h"

namespace testutil {

// Tiny deterministic PRNG so tests are reproducible on every compiler.
struct Lcg {
  uint64_t s;
  explicit Lcg(uint64_t seed) : s(seed * 2862933555777941757ULL + 3037000493ULL) {}
  uint32_t next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint32_t>(s >> 33);
  }
  uint32_t below(uint32_t n) { return next() % n; }
};

struct FakeClock : lifeline::Clock {
  uint64_t t = 1000;
  uint64_t now_ms() override { return t += 7; }
};

inline lifeline::Record make(lifeline::Cls c, uint64_t seq, uint16_t key, const char* text) {
  lifeline::Record r;
  r.node = 7;
  r.epoch = 1;
  r.cls = c;
  r.seq = seq;
  r.key = key;
  r.len = static_cast<uint16_t>(std::strlen(text));
  r.ts_ms = 42;
  std::memcpy(r.payload, text, r.len);
  return r;
}

}  // namespace testutil
