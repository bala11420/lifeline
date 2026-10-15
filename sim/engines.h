// What the simulator needs from a node and from a gateway. Lifeline implements both; the naive
// baseline (a MODEL of MQTT QoS 1 semantics with a RAM queue, NOT real Mosquitto) implements the
// same interfaces, so both builds face the identical workload and fault schedule.
#pragma once
#include <deque>
#include <map>
#include <memory>
#include <vector>
#include "lifeline/actuator.h"
#include "lifeline/dedup.h"
#include "lifeline/memflash.h"
#include "lifeline/messages.h"
#include "lifeline/node.h"
#include "sim/sim.h"

namespace lifeline { namespace sim {

// Rejected = storage full / too large (never claimed durable). MaybeCommitted = I/O error
// (the bytes may or may not have survived; the oracle must not count it as lost).
enum class Accept { Accepted, Rejected, MaybeCommitted };

struct Applied {  // one effect applied at the gateway
  uint64_t t = 0;
  uint32_t node = 0;
  Cls cls = Cls::Telemetry;
  uint32_t epoch = 0;
  uint64_t seq = 0;
  uint16_t key = 0;
  uint32_t id = 0;   // ground-truth event id carried in the payload
  uint32_t val = 0;  // telemetry reading counter
};

struct CmdEffect {  // one actuator output change caused by a command (ground-truth log, outlives reboots)
  uint64_t t = 0;
  uint32_t term = 0;
  uint64_t seq = 0;  // = the operator's issue order, 1,2,3... for every build
  uint8_t desired = 0;
  bool durable = true;  // false: the output changed although the command was NOT made durable
};

class NodeEngine {
 public:
  virtual ~NodeEngine() {}
  virtual void boot() = 0;
  // kind 0 = torn write after `bytes`, 1 = power loss at flush. RAM-only engines ignore it.
  virtual void crash_arm(int kind, size_t bytes) = 0;
  virtual void reboot() = 0;  // power cycle then boot; RAM state is lost
  virtual Accept submit(Cls cls, uint16_t key, uint32_t id, uint32_t val) = 0;
  // Next encoded frame to put on the link (Data frames only). cls/id describe it for the checks.
  virtual bool next_frame(std::vector<uint8_t>& out, Cls& cls, uint32_t& id) = 0;
  virtual void on_frame(const uint8_t* p, size_t n) = 0;  // Welcome / Ack / TeleAck from gateway
  virtual void make_hello(std::vector<uint8_t>& out) = 0;
  virtual size_t unacked() const = 0;                     // records awaiting acknowledgement
  virtual std::vector<uint32_t> pending_critical_ids() const = 0;
  virtual void retry() = 0;                               // retransmission timer fired
  virtual size_t storage_used() const = 0;
  virtual const std::vector<CmdEffect>& effects() const = 0;  // every command effect, in order
  virtual uint8_t actuator_output() const = 0;                 // output right now
  // Commands whose persist returned an I/O error: they MAY have reached flash, so after a reboot the
  // output may legitimately reflect them. The oracle must accept either outcome.
  virtual const std::vector<CmdEffect>& in_doubt() const = 0;
};

class GatewayEngine {
 public:
  virtual ~GatewayEngine() {}
  virtual void on_frame(uint64_t now, const uint8_t* p, size_t n,
                        std::vector<std::vector<uint8_t>>& replies) = 0;
  virtual const std::vector<Applied>& applied() const = 0;
  // Operator action: returns the command's seq (1,2,3... in issue order).
  virtual uint64_t issue_command(uint64_t now, uint32_t node, uint16_t actuator, uint8_t desired) = 0;
  // Called every few ms: emits (re)transmissions of commands that are not yet acknowledged.
  virtual void tick(uint64_t now, std::vector<std::vector<uint8_t>>& out) = 0;
};

std::unique_ptr<NodeEngine> make_lifeline_node(Sim& sim, uint32_t node_id, size_t flash_bytes);
std::unique_ptr<GatewayEngine> make_lifeline_gateway();
std::unique_ptr<NodeEngine> make_naive_node(Sim& sim, uint32_t node_id, size_t queue_cap);
std::unique_ptr<GatewayEngine> make_naive_gateway();

}}  // namespace lifeline::sim
