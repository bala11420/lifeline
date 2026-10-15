// Everything that crosses the link between a node and the gateway.
// Frame: [u8 type][body][u32 CRC32 over type+body]. Any corruption makes decode return false.
#pragma once
#include <vector>
#include "lifeline/protocol.h"

namespace lifeline {

enum class MsgType : uint8_t { Data = 1, Hello = 2, Welcome = 3, Ack = 4, TeleAck = 5, Command = 6, CmdAck = 7 };

// node -> gateway, first message after every (re)connect.
struct Hello { uint32_t node = 0; uint32_t epoch = 0; uint64_t crit_next_seq = 1; uint64_t ctrl_next_seq = 1; };
// gateway -> node, reply to Hello. hwm = cumulative committed sequence per stream.
// epoch_ok=false means the gateway has seen a higher epoch from this node id and refuses it.
struct Welcome { uint32_t node = 0; uint64_t crit_hwm = 0; uint64_t ctrl_hwm = 0; bool epoch_ok = true; };
// gateway -> node, cumulative acknowledgement of a Critical or Control stream.
struct Ack { uint32_t node = 0; Cls cls = Cls::Critical; uint64_t upto = 0; };
// gateway -> node, newest telemetry version applied for a key.
struct TeleAck { uint32_t node = 0; uint32_t epoch = 0; uint16_t key = 0; uint64_t version = 0; };

// gateway -> actuator node. ABSOLUTE desired state (never a toggle), ordered by (term, seq).
struct Command { uint32_t node = 0; uint32_t term = 1; uint64_t seq = 0; uint16_t actuator = 1; uint8_t desired = 0; };
// actuator node -> gateway: the highest (term, seq) the actuator has applied. Sent for applied AND for
// stale or duplicate commands, so the gateway stops retrying anything at or below it.
struct CmdAck { uint32_t node = 0; uint32_t term = 0; uint64_t seq = 0; };

void encode_msg(const Record& m, std::vector<uint8_t>& out);   // MsgType::Data
void encode_msg(const Command& m, std::vector<uint8_t>& out);
void encode_msg(const CmdAck& m, std::vector<uint8_t>& out);
void encode_msg(const Hello& m, std::vector<uint8_t>& out);
void encode_msg(const Welcome& m, std::vector<uint8_t>& out);
void encode_msg(const Ack& m, std::vector<uint8_t>& out);
void encode_msg(const TeleAck& m, std::vector<uint8_t>& out);

// Validates length and CRC and reports the type. Call this first, then the matching decode_msg.
bool msg_type(const uint8_t* p, size_t n, MsgType& t);
bool decode_msg(const uint8_t* p, size_t n, Record& m);
bool decode_msg(const uint8_t* p, size_t n, Hello& m);
bool decode_msg(const uint8_t* p, size_t n, Welcome& m);
bool decode_msg(const uint8_t* p, size_t n, Ack& m);
bool decode_msg(const uint8_t* p, size_t n, TeleAck& m);
bool decode_msg(const uint8_t* p, size_t n, Command& m);
bool decode_msg(const uint8_t* p, size_t n, CmdAck& m);

}  // namespace lifeline
