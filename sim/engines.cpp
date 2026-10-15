#include "sim/engines.h"
#include "lifeline/codec.h"

namespace lifeline { namespace sim {

namespace {

void pack8(uint8_t* out, uint32_t id, uint32_t val) {
  std::vector<uint8_t> b;
  put_u32(b, id);
  put_u32(b, val);
  for (int i = 0; i < 8; i++) out[i] = b[i];
}

Applied make_applied(uint64_t now, const Record& r) {
  Applied a;
  a.t = now; a.node = r.node; a.cls = r.cls; a.epoch = r.epoch; a.seq = r.seq; a.key = r.key;
  a.id = r.len >= 4 ? get_u32(r.payload) : 0;
  a.val = r.len >= 8 ? get_u32(r.payload + 4) : 0;
  return a;
}

// Gateway-side command outbox: keeps every unacknowledged command and retransmits it until acked.
class CmdOutbox {
 public:
  uint64_t issue(uint32_t node, uint16_t actuator, uint8_t desired) {
    Slot& s = nodes_[node];
    Pending p;
    p.cmd.node = node; p.cmd.term = 1; p.cmd.seq = s.next++; p.cmd.actuator = actuator; p.cmd.desired = desired;
    s.pending[p.cmd.seq] = p;
    return p.cmd.seq;
  }
  void tick(uint64_t now, std::vector<std::vector<uint8_t>>& out) {
    for (std::map<uint32_t, Slot>::iterator n = nodes_.begin(); n != nodes_.end(); ++n)
      for (std::map<uint64_t, Pending>::iterator it = n->second.pending.begin(); it != n->second.pending.end(); ++it)
        if (it->second.last_sent < 0 || now >= static_cast<uint64_t>(it->second.last_sent) + 800) {
          it->second.last_sent = static_cast<int64_t>(now);
          std::vector<uint8_t> f;
          encode_msg(it->second.cmd, f);
          out.push_back(f);
        }
  }
  // Lifeline: the actuator reports its highest applied (term, seq); everything at or below it is moot.
  void ack_upto(uint32_t node, uint32_t term, uint64_t seq) {
    Slot& s = nodes_[node];
    for (std::map<uint64_t, Pending>::iterator it = s.pending.begin(); it != s.pending.end();) {
      if (it->second.cmd.term == term && it->second.cmd.seq <= seq) s.pending.erase(it++);
      else ++it;
    }
  }
  // Naive: a per-message acknowledgement removes exactly that command.
  void ack_exact(uint32_t node, uint64_t seq) { nodes_[node].pending.erase(seq); }

 private:
  struct Pending { Command cmd; int64_t last_sent = -1; };
  struct Slot { uint64_t next = 1; std::map<uint64_t, Pending> pending; };
  std::map<uint32_t, Slot> nodes_;
};

// ---------------------------------------------------------------- Lifeline
class LifelineNode : public NodeEngine {
 public:
  LifelineNode(Sim& sim, uint32_t node_id, size_t flash_bytes)
      : sim_(sim), flash_(flash_bytes), act_flash_(4096), clock_(sim) {
    cfg_.node_id = node_id;
    cfg_.critical_reserve = flash_bytes / 4;
  }
  void boot() override {
    node_.reset(new Node(flash_, clock_, cfg_));
    node_->boot();
    act_.reset(new Actuator(act_flash_, 1));
    act_->boot();  // reconciles the output to the persisted desired state
    acks_.clear();
  }
  void crash_arm(int kind, size_t bytes) override {  // one device: both flashes lose power together
    armed_ = (armed_ == 1) ? 1 : kind;  // once a flush failure is armed, the reboot must lose unsynced bytes
    if (kind == 0) { flash_.arm_crash_after(bytes); act_flash_.arm_crash_after(bytes / 3); }
    else if (kind == 1) { flash_.arm_sync_failure(); act_flash_.arm_sync_failure(); }
  }
  void reboot() override {
    flash_.power_cycle(armed_ == 1);  // a failed flush loses its unsynced bytes
    act_flash_.power_cycle(armed_ == 1);
    armed_ = -1;
    boot();
  }
  Accept submit(Cls cls, uint16_t key, uint32_t id, uint32_t val) override {
    uint8_t p[8];
    pack8(p, id, val);
    switch (node_->submit(cls, key, p, 8)) {
      case SubmitResult::Accepted: return Accept::Accepted;
      case SubmitResult::IoError: return Accept::MaybeCommitted;
      default: return Accept::Rejected;
    }
  }
  bool next_frame(std::vector<uint8_t>& out, Cls& cls, uint32_t& id) override {
    if (!acks_.empty() && node_->unsent(Cls::Critical) == 0) {  // tiny control-plane frame, never ahead of a critical record
      out = acks_.front();
      acks_.pop_front();
      cls = Cls::Control;
      id = 0;  // not an event: ignored by the priority check
      return true;
    }
    Record r;
    if (!node_->poll_send(r)) return false;
    out.clear();
    encode_msg(r, out);
    cls = r.cls;
    id = get_u32(r.payload);
    return true;
  }
  void on_frame(const uint8_t* p, size_t n) override {
    MsgType t;
    if (!msg_type(p, n, t)) return;
    if (t == MsgType::Command) {
      Command c;
      if (!decode_msg(p, n, c)) return;
      uint8_t before = act_->output();
      Actuator::Result r = act_->on_command(c);
      if (r == Actuator::Result::Applied || act_->output() != before) {  // observe the output pin itself
        CmdEffect e; e.t = sim_.now(); e.term = c.term; e.seq = c.seq; e.desired = c.desired;
        e.durable = (r == Actuator::Result::Applied);
        effects_.push_back(e);
      }
      if (r == Actuator::Result::IoError) {
        CmdEffect d; d.t = sim_.now(); d.term = c.term; d.seq = c.seq; d.desired = c.desired; d.durable = false;
        doubt_.push_back(d);
      }
      if (r != Actuator::Result::IoError) {  // applied, duplicate or stale: report what we hold
        CmdAck a; a.node = cfg_.node_id; a.term = act_->term(); a.seq = act_->seq();
        std::vector<uint8_t> f;
        encode_msg(a, f);
        acks_.push_back(f);
        if (acks_.size() > 16) acks_.pop_front();
      }
      return;
    }
    if (t == MsgType::Welcome) { Welcome w; if (decode_msg(p, n, w)) node_->on_welcome(w); }
    else if (t == MsgType::Ack) { Ack a; if (decode_msg(p, n, a)) node_->on_ack(a.cls, a.upto); }
    else if (t == MsgType::TeleAck) { TeleAck a; if (decode_msg(p, n, a)) node_->on_telemetry_ack(a.epoch, a.key, a.version); }
  }
  void make_hello(std::vector<uint8_t>& out) override { out.clear(); encode_msg(node_->hello(), out); }
  size_t unacked() const override { return node_->unacked(Cls::Critical) + node_->unacked(Cls::Control); }
  std::vector<uint32_t> pending_critical_ids() const override {
    std::vector<uint32_t> v;
    const std::deque<Record>& d = node_->pending_records(Cls::Critical);
    for (size_t i = 0; i < d.size(); i++) v.push_back(get_u32(d[i].payload));
    return v;
  }
  void retry() override { node_->retry_telemetry(); }
  size_t storage_used() const override { return node_->journal_used(); }
  const std::vector<CmdEffect>& effects() const override { return effects_; }
  uint8_t actuator_output() const override { return act_->output(); }
  const std::vector<CmdEffect>& in_doubt() const override { return doubt_; }

 private:
  Sim& sim_;
  MemFlash flash_;
  MemFlash act_flash_;
  SimClock clock_;
  NodeConfig cfg_;
  std::unique_ptr<Node> node_;
  std::unique_ptr<Actuator> act_;
  std::deque<std::vector<uint8_t> > acks_;
  std::vector<CmdEffect> effects_;
  std::vector<CmdEffect> doubt_;
  int armed_ = -1;
};

struct PerNode {
  StreamTracker crit, ctrl;
  uint32_t max_epoch = 0;
  std::map<uint16_t, VersionGate> tele;
};

class LifelineGateway : public GatewayEngine {
 public:
  void on_frame(uint64_t now, const uint8_t* p, size_t n, std::vector<std::vector<uint8_t>>& replies) override {
    MsgType t;
    if (!msg_type(p, n, t)) return;
    if (t == MsgType::CmdAck) {
      CmdAck a;
      if (decode_msg(p, n, a)) outbox_.ack_upto(a.node, a.term, a.seq);
      return;
    }
    if (t == MsgType::Hello) {
      Hello h;
      if (!decode_msg(p, n, h)) return;
      PerNode& s = nodes_[h.node];
      bool ok = h.epoch >= s.max_epoch;  // refuse an epoch that went backwards
      if (ok) s.max_epoch = h.epoch;
      reply_welcome(h.node, s, ok, replies);
    } else if (t == MsgType::Data) {
      Record r;
      if (!decode_msg(p, n, r)) return;
      PerNode& s = nodes_[r.node];
      std::vector<uint8_t> out;
      if (r.cls == Cls::Critical) {
        if (s.crit.accept(r.seq)) applied_.push_back(make_applied(now, r));
        Ack a; a.node = r.node; a.cls = Cls::Critical; a.upto = s.crit.cumulative();
        encode_msg(a, out); replies.push_back(out);
      } else if (r.cls == Cls::Control) {
        StreamTracker::Order o = s.ctrl.accept_in_order(r.seq);
        if (o == StreamTracker::Order::Applied) applied_.push_back(make_applied(now, r));
        if (o == StreamTracker::Order::Gap) {
          reply_welcome(r.node, s, true, replies);  // make the node rewind and resend in order
        } else {
          Ack a; a.node = r.node; a.cls = Cls::Control; a.upto = s.ctrl.cumulative();
          encode_msg(a, out); replies.push_back(out);
        }
      } else {
        if (s.tele[r.key].accept(r.epoch, r.seq)) applied_.push_back(make_applied(now, r));
        TeleAck a; a.node = r.node; a.epoch = r.epoch; a.key = r.key; a.version = r.seq;
        encode_msg(a, out); replies.push_back(out);
      }
    }
  }
  const std::vector<Applied>& applied() const override { return applied_; }
  uint64_t issue_command(uint64_t, uint32_t node, uint16_t actuator, uint8_t desired) override {
    return outbox_.issue(node, actuator, desired);
  }
  void tick(uint64_t now, std::vector<std::vector<uint8_t> >& out) override { outbox_.tick(now, out); }

 private:
  CmdOutbox outbox_;
  static void reply_welcome(uint32_t node, PerNode& s, bool ok, std::vector<std::vector<uint8_t>>& replies) {
    Welcome w; w.node = node; w.crit_hwm = s.crit.cumulative(); w.ctrl_hwm = s.ctrl.cumulative(); w.epoch_ok = ok;
    std::vector<uint8_t> out;
    encode_msg(w, out);
    replies.push_back(out);
  }
  std::map<uint32_t, PerNode> nodes_;
  std::vector<Applied> applied_;
};

// ------------------------------------------------- naive baseline (QoS 1 style model)
// One FIFO queue in RAM, bounded (oldest dropped when full), per-message acks, retransmit until
// acked, no priority, no sequence-based dedup, no staleness gate. Lost on every reboot.
class NaiveNode : public NodeEngine {
 public:
  NaiveNode(Sim& sim, uint32_t node_id, size_t cap) : sim_(sim), node_(node_id), cap_(cap) {}
  void boot() override { q_.clear(); next_ = 1; acks_.clear(); output_ = 0; }  // all RAM: output reverts to default
  void crash_arm(int, size_t) override {}
  void reboot() override { boot(); }
  Accept submit(Cls cls, uint16_t key, uint32_t id, uint32_t val) override {
    if (q_.size() >= cap_) q_.pop_front();  // typical bounded offline buffer
    Item it; it.cls = cls; it.key = key; it.id = id; it.val = val; it.msg = next_++; it.sent = false;
    q_.push_back(it);
    return Accept::Accepted;
  }
  bool next_frame(std::vector<uint8_t>& out, Cls& cls, uint32_t& id) override {
    if (!acks_.empty()) {
      out = acks_.front();
      acks_.pop_front();
      cls = Cls::Control;
      id = 0;
      return true;
    }
    for (size_t i = 0; i < q_.size(); i++) {
      if (q_[i].sent) continue;
      q_[i].sent = true;
      Record r;
      r.node = node_; r.epoch = 0; r.cls = q_[i].cls; r.seq = q_[i].msg; r.key = q_[i].key; r.len = 8;
      pack8(r.payload, q_[i].id, q_[i].val);
      out.clear();
      encode_msg(r, out);
      cls = r.cls; id = q_[i].id;
      return true;
    }
    return false;
  }
  void on_frame(const uint8_t* p, size_t n) override {
    MsgType t;
    if (!msg_type(p, n, t)) return;
    if (t == MsgType::Command) {  // applies every command it receives, in arrival order, remembers nothing
      Command c;
      if (!decode_msg(p, n, c)) return;
      CmdEffect e; e.t = sim_.now(); e.term = c.term; e.seq = c.seq; e.desired = c.desired;
      effects_.push_back(e);
      output_ = c.desired;
      CmdAck a; a.node = node_; a.term = c.term; a.seq = c.seq;
      std::vector<uint8_t> f;
      encode_msg(a, f);
      acks_.push_back(f);
      return;
    }
    if (t == MsgType::Ack) {
      Ack a;
      if (!decode_msg(p, n, a)) return;
      for (size_t i = 0; i < q_.size(); i++)
        if (q_[i].msg == a.upto) { q_.erase(q_.begin() + static_cast<long>(i)); break; }
    } else if (t == MsgType::Welcome) {  // persistent-session style: resend everything unacked
      for (size_t i = 0; i < q_.size(); i++) q_[i].sent = false;
    }
  }
  void make_hello(std::vector<uint8_t>& out) override {
    Hello h; h.node = node_; h.epoch = 0;
    out.clear();
    encode_msg(h, out);
  }
  size_t unacked() const override { return q_.size(); }
  std::vector<uint32_t> pending_critical_ids() const override {
    std::vector<uint32_t> v;
    for (size_t i = 0; i < q_.size(); i++) if (q_[i].cls == Cls::Critical) v.push_back(q_[i].id);
    return v;
  }
  void retry() override {}
  size_t storage_used() const override { return 0; }
  const std::vector<CmdEffect>& effects() const override { return effects_; }
  uint8_t actuator_output() const override { return output_; }
  const std::vector<CmdEffect>& in_doubt() const override { return no_doubt_; }

 private:
  struct Item { Cls cls; uint16_t key; uint32_t id, val; uint64_t msg; bool sent; };
  Sim& sim_;
  uint32_t node_;
  size_t cap_;
  std::deque<Item> q_;
  std::deque<std::vector<uint8_t> > acks_;
  std::vector<CmdEffect> effects_;
  std::vector<CmdEffect> no_doubt_;
  uint8_t output_ = 0;
  uint64_t next_ = 1;
};

class NaiveGateway : public GatewayEngine {
 public:
  void on_frame(uint64_t now, const uint8_t* p, size_t n, std::vector<std::vector<uint8_t>>& replies) override {
    MsgType t;
    if (!msg_type(p, n, t)) return;
    std::vector<uint8_t> out;
    if (t == MsgType::CmdAck) {
      CmdAck a;
      if (decode_msg(p, n, a)) outbox_.ack_exact(a.node, a.seq);
      return;
    }
    if (t == MsgType::Hello) {
      Hello h;
      if (!decode_msg(p, n, h)) return;
      Welcome w; w.node = h.node;
      encode_msg(w, out); replies.push_back(out);
    } else if (t == MsgType::Data) {
      Record r;
      if (!decode_msg(p, n, r)) return;
      applied_.push_back(make_applied(now, r));  // applies every delivery, in arrival order
      Ack a; a.node = r.node; a.cls = Cls::Critical; a.upto = r.seq;
      encode_msg(a, out); replies.push_back(out);
    }
  }
  const std::vector<Applied>& applied() const override { return applied_; }
  uint64_t issue_command(uint64_t, uint32_t node, uint16_t actuator, uint8_t desired) override {
    return outbox_.issue(node, actuator, desired);
  }
  void tick(uint64_t now, std::vector<std::vector<uint8_t> >& out) override { outbox_.tick(now, out); }

 private:
  CmdOutbox outbox_;
  std::vector<Applied> applied_;
};

}  // namespace

std::unique_ptr<NodeEngine> make_lifeline_node(Sim& sim, uint32_t node_id, size_t flash_bytes) {
  return std::unique_ptr<NodeEngine>(new LifelineNode(sim, node_id, flash_bytes));
}
std::unique_ptr<GatewayEngine> make_lifeline_gateway() { return std::unique_ptr<GatewayEngine>(new LifelineGateway()); }
std::unique_ptr<NodeEngine> make_naive_node(Sim& sim, uint32_t node_id, size_t cap) {
  return std::unique_ptr<NodeEngine>(new NaiveNode(sim, node_id, cap));
}
std::unique_ptr<GatewayEngine> make_naive_gateway() { return std::unique_ptr<GatewayEngine>(new NaiveGateway()); }

}}  // namespace lifeline::sim
