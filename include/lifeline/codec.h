#pragma once
#include <vector>
#include "lifeline/protocol.h"

namespace lifeline {

// Standard CRC-32 (IEEE, reflected). Chain calls by passing the previous result as `prev`.
uint32_t crc32(const void* data, size_t n, uint32_t prev = 0);

void put_u8(std::vector<uint8_t>& o, uint8_t v);
void put_u16(std::vector<uint8_t>& o, uint16_t v);
void put_u32(std::vector<uint8_t>& o, uint32_t v);
void put_u64(std::vector<uint8_t>& o, uint64_t v);
uint16_t get_u16(const uint8_t* p);
uint32_t get_u32(const uint8_t* p);
uint64_t get_u64(const uint8_t* p);

// Wire format: codec.cpp is the authoritative definition. Fields are serialized one by one in
// explicit little-endian order; the in-memory layout of struct Record is never written to flash or
// to the wire (it is not packed), so compiler padding and host byte order do not matter.
// Encoded size of a Record = kBodyFixed + len bytes (len <= kMaxPayload).
constexpr size_t kBodyFixed = 25;  // bytes before the payload
// Checked at compile time: node(4) epoch(4) cls(1) seq(8) key(2) len(2) ts_ms(4).
static_assert(kBodyFixed == 4 + 4 + 1 + 8 + 2 + 2 + 4, "kBodyFixed must match encode_body()");
static_assert(kBodyFixed + kMaxPayload == 89, "maximum encoded Record size changed; update docs");

void encode_body(const Record& r, std::vector<uint8_t>& out);
bool decode_body(const uint8_t* p, size_t n, Record& r);

}  // namespace lifeline
