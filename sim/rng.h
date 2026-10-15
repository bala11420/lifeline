// Own PRNG (xoshiro256**, seeded via splitmix64) with fixed-width integer arithmetic only, so a
// seed yields the same number sequence regardless of the standard library in use.
// Never use std::uniform_int_distribution or std::random_device in the simulator.
#pragma once
#include <cstdint>

namespace lifeline { namespace sim {

class Rng {
 public:
  explicit Rng(uint64_t seed) {
    uint64_t x = seed;
    for (int i = 0; i < 4; i++) s_[i] = splitmix(x);
  }
  uint64_t next() {
    const uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
    s_[2] ^= t; s_[3] = rotl(s_[3], 45);
    return result;
  }
  uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1); }  // inclusive
  bool chance_ppm(uint32_t ppm) { return below(1000000u) < ppm; }

 private:
  static uint64_t splitmix(uint64_t& x) {
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t s_[4];
};

}}  // namespace lifeline::sim
