// LIFELINE shared protocol types and the three platform interfaces.
// Keep this file small and stable: both leads and the firmware teammate depend on it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lifeline {

// Critical and Control are durable, ordered streams (per-class sequence numbers).
// Telemetry is NOT a stream: it is a latest-value register per key, ordered by
// (epoch, version), so there are never gaps to wait for.
enum class Cls : uint8_t { Critical = 0, Control = 1, Telemetry = 2 };

constexpr size_t kMaxPayload = 64;

struct Record {
  uint32_t node = 0;
  uint32_t epoch = 0;   // boot epoch of the node that created the record
  Cls cls = Cls::Telemetry;
  uint64_t seq = 0;     // Critical/Control: stream sequence 1,2,3...  Telemetry: per-key version
  uint16_t key = 0;     // sensor id (telemetry) or event type (critical/control)
  uint16_t len = 0;
  uint32_t ts_ms = 0;   // node clock, for DISPLAY ONLY; never used for ordering
  uint8_t payload[kMaxPayload] = {};
};

inline bool same(const Record& a, const Record& b) {
  return a.node == b.node && a.epoch == b.epoch && a.cls == b.cls && a.seq == b.seq &&
         a.key == b.key && a.len == b.len && a.ts_ms == b.ts_ms &&
         std::memcmp(a.payload, b.payload, a.len) == 0;
}

// Append-only byte device. On ESP32 back it with a LittleFS file; in the simulator use MemFlash.
struct Flash {
  virtual ~Flash() {}
  virtual size_t size() const = 0;      // bytes currently stored
  virtual size_t capacity() const = 0;  // maximum bytes
  virtual size_t read(size_t off, void* buf, size_t n) const = 0;
  virtual bool append(const void* buf, size_t n) = 0;  // false on failure or if it would not fit
  virtual void truncate(size_t n) = 0;
  // Data appended before a successful sync() must survive power loss. Returning false means the
  // flush FAILED and nothing since the last good sync is guaranteed durable.
  virtual bool sync() = 0;
};

struct Clock {
  virtual ~Clock() {}
  virtual uint64_t now_ms() = 0;
};

// Link to the gateway. The Node never calls this; the driver loop does. Keeping the Node
// transport-free is what makes the same code run in the simulator, in Wokwi and over MQTT.
struct Transport {
  virtual ~Transport() {}
  virtual bool up() = 0;
  virtual bool send(const Record& r) = 0;
  virtual bool poll(Record& r) = 0;
};

}  // namespace lifeline
