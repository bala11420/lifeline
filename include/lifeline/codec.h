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

constexpr size_t kBodyFixed = 25;  // bytes before the payload

void encode_body(const Record& r, std::vector<uint8_t>& out);
bool decode_body(const uint8_t* p, size_t n, Record& r);

}  // namespace lifeline
