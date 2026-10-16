#include "lifeline/capi.h"
#include <memory>
#include <vector>
#include "lifeline/actuator.h"
#include "lifeline/codec.h"
#include "lifeline/memflash.h"
#include "lifeline/messages.h"
#include "lifeline/node.h"

using namespace lifeline;

namespace {
struct SimClock : Clock {
  uint64_t t = 0;
  uint64_t now_ms() override { return t; }
};
size_t copy_out(const std::vector<uint8_t>& v, uint8_t* buf, size_t cap) {
  if (v.size() > cap) return 0;
  for (size_t i = 0; i < v.size(); i++) buf[i] = v[i];
  return v.size();
}
}  // namespace

struct ll_dev {
  NodeConfig cfg;
  MemFlash flash;
  MemFlash aflash;
  SimClock clock;
  std::unique_ptr<Node> node;
  std::unique_ptr<Actuator> act;
  bool interlock = false;
  ll_dev(size_t cap, size_t acap) : flash(cap), aflash(acap) {}
};

extern "C" {

ll_dev* ll_new(uint32_t node_id, size_t cap, size_t reserve, size_t ack_batch, size_t acap) {
  ll_dev* d = new ll_dev(cap, acap);
  d->cfg.node_id = node_id;
  d->cfg.critical_reserve = reserve;
  d->cfg.ack_batch = ack_batch;
  d->node.reset(new Node(d->flash, d->clock, d->cfg));
  d->act.reset(new Actuator(d->aflash));
  d->node->boot();
  d->act->boot();
  return d;
}
void ll_free(ll_dev* d) { delete d; }

int ll_reboot(ll_dev* d, int lose_unsynced) {
  d->flash.power_cycle(lose_unsynced != 0);
  d->aflash.power_cycle(lose_unsynced != 0);
  d->node.reset(new Node(d->flash, d->clock, d->cfg));
  d->act.reset(new Actuator(d->aflash));
  bool ok = d->node->boot();
  d->act->boot();
  d->act->set_interlock(d->interlock);  // the hazard input is a physical line, it survives reboot
  return ok ? 1 : 0;
}
void ll_set_time(ll_dev* d, uint64_t ms) { d->clock.t = ms; }

int ll_submit(ll_dev* d, int cls, uint16_t key, const uint8_t* data, size_t len, uint64_t* seq) {
  uint64_t s = 0;
  SubmitResult r = d->node->submit(static_cast<Cls>(cls), key, data, len, &s);
  if (seq) *seq = s;
  return static_cast<int>(r);
}

size_t ll_poll_send(ll_dev* d, uint8_t* buf, size_t cap) {
  Record r;
  if (!d->node->poll_send(r)) return 0;
  std::vector<uint8_t> v;
  encode_msg(r, v);
  return copy_out(v, buf, cap);
}

size_t ll_hello(ll_dev* d, uint8_t* buf, size_t cap) {
  std::vector<uint8_t> v;
  encode_msg(d->node->hello(), v);
  return copy_out(v, buf, cap);
}

int ll_on_frame(ll_dev* d, const uint8_t* in, size_t n, uint8_t* out, size_t outcap, size_t* outlen,
                int* detail) {
  if (outlen) *outlen = 0;
  if (detail) *detail = 0;
  MsgType t;
  if (!msg_type(in, n, t)) return -1;
  switch (t) {
    case MsgType::Welcome: {
      Welcome w;
      if (!decode_msg(in, n, w)) return -1;
      bool ok = d->node->on_welcome(w);
      if (detail) *detail = ok ? 1 : 0;
      return 3;
    }
    case MsgType::Ack: {
      Ack a;
      if (!decode_msg(in, n, a)) return -1;
      d->node->on_ack(a.cls, a.upto);
      return 4;
    }
    case MsgType::TeleAck: {
      TeleAck a;
      if (!decode_msg(in, n, a)) return -1;
      d->node->on_telemetry_ack(a.epoch, a.key, a.version);
      return 5;
    }
    case MsgType::Cmd: {
      Cmd c;
      if (!decode_msg(in, n, c)) return -1;
      CmdResult r = d->act->on_command(c);
      if (detail) *detail = static_cast<int>(r);
      if (r != CmdResult::IoError) {  // acknowledge only after the durable commit (or a rejection)
        CmdAck a;
        a.node = d->cfg.node_id; a.term = c.term; a.seq = c.seq;
        a.status = (r == CmdResult::Stale) ? 1 : 0;
        std::vector<uint8_t> v;
        encode_msg(a, v);
        if (outlen) *outlen = copy_out(v, out, outcap);
      }
      return 6;
    }
    default:
      return -1;
  }
}

size_t ll_retry_telemetry(ll_dev* d) { return d->node->retry_telemetry(); }
uint32_t ll_epoch(ll_dev* d) { return d->node->epoch(); }
size_t ll_unacked(ll_dev* d, int cls) { return d->node->unacked(static_cast<Cls>(cls)); }
size_t ll_unsent(ll_dev* d, int cls) { return d->node->unsent(static_cast<Cls>(cls)); }
size_t ll_journal_used(ll_dev* d) { return d->flash.size(); }
size_t ll_flash_capacity(ll_dev* d) { return d->flash.capacity(); }
int ll_storage_fault(ll_dev* d) { return d->node->storage_fault() ? 1 : 0; }
int ll_flash_dead(ll_dev* d) { return d->flash.dead() ? 1 : 0; }

size_t ll_pending(ll_dev* d, int cls, uint64_t* seqs, uint64_t* oids, size_t max) {
  std::vector<Record> v = d->node->pending_records(static_cast<Cls>(cls));
  size_t n = v.size() < max ? v.size() : max;
  for (size_t i = 0; i < n; i++) {
    seqs[i] = v[i].seq;
    oids[i] = v[i].len >= 8 ? get_u64(v[i].payload) : 0;
  }
  return n;
}

void ll_arm_torn(ll_dev* d, size_t bytes) { d->flash.arm_crash_after(bytes); }
void ll_arm_sync_fail(ll_dev* d) { d->flash.arm_sync_failure(); }
void ll_power_cycle(ll_dev* d, int lose_unsynced) { d->flash.power_cycle(lose_unsynced != 0); }

void ll_set_interlock(ll_dev* d, int on) {
  d->interlock = on != 0;
  d->act->set_interlock(d->interlock);
}
void ll_act_state(ll_dev* d, uint32_t* term, uint64_t* seq, int* have, int* desired, int* output,
                  int* interlock) {
  *term = d->act->term();
  *seq = d->act->seq();
  *have = d->act->have() ? 1 : 0;
  *desired = static_cast<int>(d->act->desired());
  *output = static_cast<int>(d->act->output());
  *interlock = d->act->interlock() ? 1 : 0;
}

}  // extern "C"
