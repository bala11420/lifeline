#include "lifeline/messages.h"
#include "lifeline/codec.h"

namespace lifeline {

namespace {
void begin(std::vector<uint8_t>& o, MsgType t, size_t& start) {
  start = o.size();
  put_u8(o, static_cast<uint8_t>(t));
}
void seal(std::vector<uint8_t>& o, size_t start) { put_u32(o, crc32(o.data() + start, o.size() - start)); }

// Checks CRC and type; exposes the body bytes.
bool open_frame(const uint8_t* p, size_t n, MsgType want, const uint8_t*& body, size_t& blen) {
  if (n < 1 + 4) return false;
  if (crc32(p, n - 4) != get_u32(p + n - 4)) return false;
  if (p[0] != static_cast<uint8_t>(want)) return false;
  body = p + 1;
  blen = n - 5;
  return true;
}
}  // namespace

void encode_msg(const Record& m, std::vector<uint8_t>& o) {
  size_t s; begin(o, MsgType::Data, s); encode_body(m, o); seal(o, s);
}
void encode_msg(const Hello& m, std::vector<uint8_t>& o) {
  size_t s; begin(o, MsgType::Hello, s);
  put_u32(o, m.node); put_u32(o, m.epoch); put_u64(o, m.crit_next_seq); put_u64(o, m.ctrl_next_seq);
  seal(o, s);
}
void encode_msg(const Welcome& m, std::vector<uint8_t>& o) {
  size_t s; begin(o, MsgType::Welcome, s);
  put_u32(o, m.node); put_u64(o, m.crit_hwm); put_u64(o, m.ctrl_hwm); put_u8(o, m.epoch_ok ? 1 : 0);
  seal(o, s);
}
void encode_msg(const Ack& m, std::vector<uint8_t>& o) {
  size_t s; begin(o, MsgType::Ack, s);
  put_u32(o, m.node); put_u8(o, static_cast<uint8_t>(m.cls)); put_u64(o, m.upto);
  seal(o, s);
}
void encode_msg(const TeleAck& m, std::vector<uint8_t>& o) {
  size_t s; begin(o, MsgType::TeleAck, s);
  put_u32(o, m.node); put_u32(o, m.epoch); put_u16(o, m.key); put_u64(o, m.version);
  seal(o, s);
}

void encode_msg(const Command& m, std::vector<uint8_t>& o) {
  size_t s; begin(o, MsgType::Command, s);
  put_u32(o, m.node); put_u32(o, m.term); put_u64(o, m.seq); put_u16(o, m.actuator); put_u8(o, m.desired);
  seal(o, s);
}
void encode_msg(const CmdAck& m, std::vector<uint8_t>& o) {
  size_t s; begin(o, MsgType::CmdAck, s);
  put_u32(o, m.node); put_u32(o, m.term); put_u64(o, m.seq);
  seal(o, s);
}

bool msg_type(const uint8_t* p, size_t n, MsgType& t) {
  if (n < 1 + 4 || crc32(p, n - 4) != get_u32(p + n - 4)) return false;
  if (p[0] < 1 || p[0] > 7) return false;
  t = static_cast<MsgType>(p[0]);
  return true;
}

bool decode_msg(const uint8_t* p, size_t n, Record& m) {
  const uint8_t* b; size_t bl;
  return open_frame(p, n, MsgType::Data, b, bl) && decode_body(b, bl, m);
}
bool decode_msg(const uint8_t* p, size_t n, Hello& m) {
  const uint8_t* b; size_t bl;
  if (!open_frame(p, n, MsgType::Hello, b, bl) || bl != 24) return false;
  m.node = get_u32(b); m.epoch = get_u32(b + 4); m.crit_next_seq = get_u64(b + 8); m.ctrl_next_seq = get_u64(b + 16);
  return true;
}
bool decode_msg(const uint8_t* p, size_t n, Welcome& m) {
  const uint8_t* b; size_t bl;
  if (!open_frame(p, n, MsgType::Welcome, b, bl) || bl != 21 || b[20] > 1) return false;
  m.node = get_u32(b); m.crit_hwm = get_u64(b + 4); m.ctrl_hwm = get_u64(b + 12); m.epoch_ok = b[20] == 1;
  return true;
}
bool decode_msg(const uint8_t* p, size_t n, Ack& m) {
  const uint8_t* b; size_t bl;
  if (!open_frame(p, n, MsgType::Ack, b, bl) || bl != 13 || b[4] > 1) return false;  // streams only
  m.node = get_u32(b); m.cls = static_cast<Cls>(b[4]); m.upto = get_u64(b + 5);
  return true;
}
bool decode_msg(const uint8_t* p, size_t n, TeleAck& m) {
  const uint8_t* b; size_t bl;
  if (!open_frame(p, n, MsgType::TeleAck, b, bl) || bl != 18) return false;
  m.node = get_u32(b); m.epoch = get_u32(b + 4); m.key = get_u16(b + 8); m.version = get_u64(b + 10);
  return true;
}

bool decode_msg(const uint8_t* p, size_t n, Command& m) {
  const uint8_t* b; size_t bl;
  if (!open_frame(p, n, MsgType::Command, b, bl) || bl != 19) return false;
  m.node = get_u32(b); m.term = get_u32(b + 4); m.seq = get_u64(b + 8); m.actuator = get_u16(b + 16); m.desired = b[18];
  return true;
}
bool decode_msg(const uint8_t* p, size_t n, CmdAck& m) {
  const uint8_t* b; size_t bl;
  if (!open_frame(p, n, MsgType::CmdAck, b, bl) || bl != 16) return false;
  m.node = get_u32(b); m.term = get_u32(b + 4); m.seq = get_u64(b + 8);
  return true;
}

}  // namespace lifeline
