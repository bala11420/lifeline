#include "doctest.h"
#include "helpers.h"
#include "lifeline/codec.h"
#include "lifeline/messages.h"

using namespace lifeline;
using namespace testutil;

TEST_CASE("crc32 matches the standard check value") {
  CHECK(crc32("123456789", 9) == 0xCBF43926u);
  uint32_t part = crc32("1234", 4);
  CHECK(crc32("56789", 5, part) == 0xCBF43926u);  // chaining works
}

namespace {
template <class M>
void check_corruption(const M& m) {
  std::vector<uint8_t> w;
  encode_msg(m, w);
  M tmp;
  REQUIRE(decode_msg(w.data(), w.size(), tmp));
  for (size_t i = 0; i < w.size(); i++)
    for (int bit = 0; bit < 8; bit++) {
      std::vector<uint8_t> bad = w;
      bad[i] ^= static_cast<uint8_t>(1u << bit);
      CHECK_FALSE(decode_msg(bad.data(), bad.size(), tmp));
    }
  for (size_t n = 0; n < w.size(); n++) CHECK_FALSE(decode_msg(w.data(), n, tmp));
}
}  // namespace

TEST_CASE("every message type round-trips and detects any single-bit corruption or truncation") {
  Record r = make(Cls::Critical, 99, 5, "gas>400ppm");
  Hello h; h.node = 7; h.epoch = 3; h.crit_next_seq = 12; h.ctrl_next_seq = 4;
  Welcome w; w.node = 7; w.crit_hwm = 11; w.ctrl_hwm = 3; w.epoch_ok = false;
  Ack a; a.node = 7; a.cls = Cls::Control; a.upto = 55;
  TeleAck t; t.node = 7; t.epoch = 3; t.key = 9; t.version = 1234567;

  std::vector<uint8_t> buf;
  encode_msg(r, buf);
  Record r2; REQUIRE(decode_msg(buf.data(), buf.size(), r2)); CHECK(same(r, r2));
  buf.clear(); encode_msg(h, buf);
  Hello h2; REQUIRE(decode_msg(buf.data(), buf.size(), h2));
  CHECK((h2.node == 7 && h2.epoch == 3 && h2.crit_next_seq == 12 && h2.ctrl_next_seq == 4));
  buf.clear(); encode_msg(w, buf);
  Welcome w2; REQUIRE(decode_msg(buf.data(), buf.size(), w2));
  CHECK((w2.crit_hwm == 11 && w2.ctrl_hwm == 3 && !w2.epoch_ok));
  buf.clear(); encode_msg(a, buf);
  Ack a2; REQUIRE(decode_msg(buf.data(), buf.size(), a2));
  CHECK((a2.cls == Cls::Control && a2.upto == 55));
  buf.clear(); encode_msg(t, buf);
  TeleAck t2; REQUIRE(decode_msg(buf.data(), buf.size(), t2));
  CHECK((t2.key == 9 && t2.version == 1234567 && t2.epoch == 3));

  Command c; c.node = 7; c.term = 2; c.seq = 41; c.actuator = 3; c.desired = 1;
  CmdAck ca; ca.node = 7; ca.term = 2; ca.seq = 41;
  buf.clear(); encode_msg(c, buf);
  Command c2; REQUIRE(decode_msg(buf.data(), buf.size(), c2));
  CHECK((c2.term == 2 && c2.seq == 41 && c2.actuator == 3 && c2.desired == 1));
  buf.clear(); encode_msg(ca, buf);
  CmdAck ca2; REQUIRE(decode_msg(buf.data(), buf.size(), ca2));
  CHECK((ca2.term == 2 && ca2.seq == 41));
  check_corruption(r); check_corruption(h); check_corruption(w); check_corruption(a); check_corruption(t);
  check_corruption(c); check_corruption(ca);
}

TEST_CASE("msg_type identifies frames and a decoder rejects the wrong type") {
  Hello h; h.node = 1;
  std::vector<uint8_t> buf;
  encode_msg(h, buf);
  MsgType ty;
  REQUIRE(msg_type(buf.data(), buf.size(), ty));
  CHECK(ty == MsgType::Hello);
  Ack a;
  CHECK_FALSE(decode_msg(buf.data(), buf.size(), a));
  uint8_t junk[3] = {1, 2, 3};
  CHECK_FALSE(msg_type(junk, 3, ty));
}
