// LIFELINE on ESP32 (Wokwi): the REAL core (journal, CRC32 codec, sequences, priority, actuator)
// running on LittleFS. The gateway is a small stand-in inside this sketch (same rules as gateway.py)
// whose state is also stored in LittleFS, so a reboot does not make it forget.
//
// Serial commands (type one letter, Enter not needed):
//   g gas event    d link down    u link up      r reboot      x tear next write (power loss)
//   e control rec  1 CLOSED cmd   0 OPEN cmd     z reset alarm l lose next ACK
//   p replay last critical frame   s replay stale telemetry     k replay old command
//   f flood criticals (capacity)   i status       w wipe + reboot
#include <Arduino.h>
#include <LittleFS.h>
#include <vector>
#include "ll_node.h"
#include "ll_actuator.h"
#include "ll_messages.h"
#include "ll_codec.h"
#include "esp_flash.h"

#ifndef HOST_TEST
#define USE_OLED 1
#define USE_DHT 1
#endif
#if defined(USE_OLED) && USE_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
#endif
#if defined(USE_DHT) && USE_DHT
#include <DHT.h>
DHT dht(15, DHT22);
#endif

using namespace lifeline;

// ---------------------------------------------------------------- pins and thresholds
static const int PIN_GAS = 34;      // MQ2 AOUT
static const int PIN_BUZZER = 25;   // local alarm output
static const int PIN_VALVE_LED = 27;  // ON = valve open
static const int GAS_TRIP = 2500, GAS_CLEAR = 1800;  // hysteresis: one event per excursion
static const uint32_t NODE_ID = 1;
static const size_t JOURNAL_CAP = 2048;  // small on purpose, so capacity limits are easy to show
#ifndef LINK_UP_AT_BOOT
#define LINK_UP_AT_BOOT 0
#endif

#ifdef HOST_TEST
int host_gas = 500;
static int readGas() { return host_gas; }
static float readTemp() { return 24.5f; }
static float readHum() { return 55.0f; }
#else
static int readGas() { return analogRead(PIN_GAS); }
static float readTemp() {
#if USE_DHT
  float t = dht.readTemperature();
  if (!isnan(t)) return t;
#endif
  return 24.0f;
}
static float readHum() {
#if USE_DHT
  float h = dht.readHumidity();
  if (!isnan(h)) return h;
#endif
  return 50.0f;
}
#endif

// ---------------------------------------------------------------- the node (real core)
struct EspClock : Clock { uint64_t now_ms() override { return millis(); } };
static EspClock g_clock;
static LittleFsFlash g_journal("/journal.bin", JOURNAL_CAP);
static LittleFsFlash g_valveLog("/valve.bin", 256);
static Node* g_node = nullptr;
static Actuator* g_valve = nullptr;

static bool g_link = false, g_welcomed = false, g_alarm = false, g_dropAck = false;
static unsigned long g_lastTx = 0, g_lastSample = 0, g_lastGas = 0, g_lastHello = 0, g_lastProgress = 0,
                     g_lastCmd = 0;
#if defined(USE_OLED) && USE_OLED
static unsigned long g_lastOled = 0;
#endif
static std::vector<uint8_t> g_lastCritFrame;

// ---------------------------------------------------------------- simulated gateway (persisted)
#pragma pack(push, 1)
struct GwState {
  uint32_t magic;
  uint64_t crit_hwm, ctrl_hwm;
  uint32_t effects, ctrl_effects;
  uint32_t tele_epoch[4];
  uint64_t tele_ver[4];
  uint32_t cmd_term;
  uint64_t cmd_seq, cmd_acked;
  uint8_t cmd_desired[2];
  uint32_t crc;
};
#pragma pack(pop)
static GwState g_gw;

static void gwReset() {
  memset(&g_gw, 0, sizeof g_gw);
  g_gw.magic = 0x4C46474D;
  g_gw.cmd_term = 1;
}
static void gwSave() {
  g_gw.crc = crc32(&g_gw, sizeof g_gw - 4);
  File f = LittleFS.open("/gw.tmp", "w");
  if (!f) return;
  f.write(reinterpret_cast<const uint8_t*>(&g_gw), sizeof g_gw);
  f.close();
  LittleFS.rename("/gw.tmp", "/gw.bin");
}
static void gwLoad() {
  gwReset();
  File f = LittleFS.open("/gw.bin", "r");
  if (!f) return;
  GwState t;
  if (f.size() == sizeof t && f.read(reinterpret_cast<uint8_t*>(&t), sizeof t) == sizeof t &&
      t.magic == 0x4C46474D && t.crc == crc32(&t, sizeof t - 4))
    g_gw = t;
  f.close();
}

// ---------------------------------------------------------------- helpers
static const char* clsName(Cls c) { return c == Cls::Critical ? "CRIT" : c == Cls::Control ? "CTRL" : "TELE"; }
static const char* valveName(Valve v) { return v == Valve::Open ? "OPEN" : "CLOSED"; }

static void localAlarm(bool on) {
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
}
static void applyValveOutput() {
  digitalWrite(PIN_VALVE_LED, g_valve->output() == Valve::Open ? HIGH : LOW);
}
static void printValve() {
  Serial.printf("[VALVE] desired=%s output=%s interlock=%s committed=v%u.%u\n", valveName(g_valve->desired()),
                valveName(g_valve->output()), g_valve->interlock() ? "ON" : "off", (unsigned)g_valve->term(),
                (unsigned)g_valve->seq());
}

static void toGateway(const std::vector<uint8_t>& fr);
static void toNode(const std::vector<uint8_t>& fr);

static void toNodeLater(const std::vector<uint8_t>& fr) {  // gateway -> node (may be lost on purpose)
  MsgType t;
  if (msg_type(fr.data(), fr.size(), t) && t == MsgType::Ack && g_dropAck) {
    g_dropAck = false;
    Serial.println("[LINK] ACK lost on the way back to the node");
    return;
  }
  if (!g_link) return;
  toNode(fr);
}

// ---------------------------------------------------------------- gateway logic
static void gwReply(const std::vector<uint8_t>& fr) { toNodeLater(fr); }

static void toGateway(const std::vector<uint8_t>& fr) {
  MsgType t;
  if (!msg_type(fr.data(), fr.size(), t)) { Serial.println("[GW] corrupt frame dropped"); return; }
  std::vector<uint8_t> out;
  if (t == MsgType::Hello) {
    Hello h; decode_msg(fr.data(), fr.size(), h);
    Welcome w; w.node = h.node; w.crit_hwm = g_gw.crit_hwm; w.ctrl_hwm = g_gw.ctrl_hwm; w.epoch_ok = true;
    Serial.printf("[GW] HELLO epoch=%u -> WELCOME crit_hwm=%u ctrl_hwm=%u\n", (unsigned)h.epoch,
                  (unsigned)w.crit_hwm, (unsigned)w.ctrl_hwm);
    encode_msg(w, out); gwReply(out);
  } else if (t == MsgType::Data) {
    Record r; decode_msg(fr.data(), fr.size(), r);
    if (r.cls == Cls::Telemetry) {
      int k = r.key < 4 ? r.key : 3;
      bool newer = r.epoch > g_gw.tele_epoch[k] || (r.epoch == g_gw.tele_epoch[k] && r.seq > g_gw.tele_ver[k]);
      if (newer) {
        g_gw.tele_epoch[k] = r.epoch; g_gw.tele_ver[k] = r.seq; gwSave();
        Serial.printf("[GW] TELE key=%u v=%u.%u applied\n", r.key, (unsigned)r.epoch, (unsigned)r.seq);
      } else {
        Serial.printf("[GW] STALE telemetry key=%u v=%u.%u REJECTED (have v=%u.%u)\n", r.key, (unsigned)r.epoch,
                      (unsigned)r.seq, (unsigned)g_gw.tele_epoch[k], (unsigned)g_gw.tele_ver[k]);
      }
      TeleAck a; a.node = r.node; a.epoch = r.epoch; a.key = r.key; a.version = r.seq;
      encode_msg(a, out); gwReply(out);
      return;
    }
    uint64_t& hwm = (r.cls == Cls::Critical) ? g_gw.crit_hwm : g_gw.ctrl_hwm;
    if (r.seq <= hwm) {
      Serial.printf("[GW] DUPLICATE %s seq=%u ignored, no new effect (effects=%u); ACK re-sent\n", clsName(r.cls),
                    (unsigned)r.seq, (unsigned)g_gw.effects);
    } else if (r.seq == hwm + 1) {
      hwm = r.seq;
      uint32_t& eff = (r.cls == Cls::Critical) ? g_gw.effects : g_gw.ctrl_effects;
      eff++;
      gwSave();  // the effect and the watermark are committed together, THEN we acknowledge
      Serial.printf("[GW] COMMIT %s seq=%u \"%.*s\" (effect #%u)\n", clsName(r.cls), (unsigned)r.seq, (int)r.len,
                    (const char*)r.payload, (unsigned)eff);
    } else {
      Welcome w; w.node = r.node; w.crit_hwm = g_gw.crit_hwm; w.ctrl_hwm = g_gw.ctrl_hwm; w.epoch_ok = true;
      Serial.printf("[GW] GAP %s seq=%u (expected %u): not applied, asking node to resync\n", clsName(r.cls),
                    (unsigned)r.seq, (unsigned)(hwm + 1));
      encode_msg(w, out); gwReply(out);
      return;
    }
    Ack a; a.node = r.node; a.cls = r.cls; a.upto = hwm;
    encode_msg(a, out); gwReply(out);
  } else if (t == MsgType::CmdAck) {
    CmdAck a; decode_msg(fr.data(), fr.size(), a);
    if (a.seq > g_gw.cmd_acked) { g_gw.cmd_acked = a.seq; gwSave(); }
    Serial.printf("[GW] CMDACK v%u.%u status=%s\n", (unsigned)a.term, (unsigned)a.seq, a.status ? "STALE" : "committed");
  }
}

// ---------------------------------------------------------------- node-side receive
static void submitReport(Cls cls, uint16_t key, const char* text) {
  uint64_t seq = 0;
  SubmitResult r = g_node->submit(cls, key, text, strlen(text), &seq);
  if (r == SubmitResult::Accepted) Serial.printf("[ACCEPTED] %s seq=%u durably journaled \"%s\"\n", clsName(cls), (unsigned)seq, text);
  else if (r == SubmitResult::StorageFull) Serial.printf("[FULL] %s record NOT accepted (journal full); earlier records are untouched\n", clsName(cls));
  else if (r == SubmitResult::IoError) Serial.printf("[IO ERROR] %s record NOT accepted (storage write failed)\n", clsName(cls));
}

static void toNode(const std::vector<uint8_t>& fr) {
  MsgType t;
  if (!msg_type(fr.data(), fr.size(), t)) return;
  if (t == MsgType::Welcome || t == MsgType::Ack) g_lastProgress = millis();  // reliable-stream progress only
  if (t == MsgType::Welcome) {
    Welcome w; decode_msg(fr.data(), fr.size(), w);
    g_node->on_welcome(w);
    g_welcomed = true;
    Serial.printf("[NODE] welcomed: gateway has crit<=%u ctrl<=%u; unacked crit=%u ctrl=%u will be sent\n",
                  (unsigned)w.crit_hwm, (unsigned)w.ctrl_hwm, (unsigned)g_node->unacked(Cls::Critical),
                  (unsigned)g_node->unacked(Cls::Control));
  } else if (t == MsgType::Ack) {
    Ack a; decode_msg(fr.data(), fr.size(), a);
    g_node->on_ack(a.cls, a.upto);
  } else if (t == MsgType::TeleAck) {
    TeleAck a; decode_msg(fr.data(), fr.size(), a);
    g_node->on_telemetry_ack(a.epoch, a.key, a.version);
  } else if (t == MsgType::Cmd) {
    Cmd c; decode_msg(fr.data(), fr.size(), c);
    CmdResult r = g_valve->on_command(c);
    const char* what = r == CmdResult::Applied ? "APPLIED" : r == CmdResult::Duplicate ? "DUPLICATE (harmless)" :
                       r == CmdResult::Stale ? "STALE, rejected" : "IO ERROR, not committed";
    Serial.printf("[VALVE] command v%u.%u SET %s -> %s\n", (unsigned)c.term, (unsigned)c.seq, valveName(c.desired ? Valve::Closed : Valve::Open), what);
    applyValveOutput();
    printValve();
    if (r != CmdResult::IoError) {  // acknowledge only after the durable commit
      CmdAck a; a.node = NODE_ID; a.term = c.term; a.seq = c.seq; a.status = r == CmdResult::Stale ? 1 : 0;
      std::vector<uint8_t> out; encode_msg(a, out); toGateway(out);
    }
    if (r == CmdResult::Applied) {
      char text[32];
      snprintf(text, sizeof text, "VALVE v%u.%u %s", (unsigned)c.term, (unsigned)c.seq, valveName(g_valve->desired()));
      submitReport(Cls::Control, 1, text);
    }
  }
}

// ---------------------------------------------------------------- application behaviour
static void gasEvent(int ppm) {
  if (g_alarm) { Serial.println("[GAS] alarm already latched: debounced, no new record"); return; }
  g_alarm = true;
  localAlarm(true);                 // 1. local response FIRST, needs no network and no storage
  g_valve->set_interlock(true);     //    the interlock holds the valve closed against remote OPEN
  applyValveOutput();
  Serial.printf("[ALARM] LOCAL ALARM ON (gas=%d), valve interlock engaged\n", ppm);
  char p[24];
  snprintf(p, sizeof p, "GAS ppm=%d", ppm);
  submitReport(Cls::Critical, 1, p);  // 2. then try to journal it; the result says whether it is durable
}

static void sendHello() {
  std::vector<uint8_t> v; encode_msg(g_node->hello(), v);
  g_lastHello = millis();
  Serial.printf("[NODE] HELLO epoch=%u\n", (unsigned)g_node->epoch());
  toGateway(v);
}

static void sampleTelemetry() {
  int16_t t = (int16_t)(readTemp() * 100), h = (int16_t)(readHum() * 100);
  uint16_t gas = (uint16_t)readGas();
  g_node->submit(Cls::Telemetry, 1, &t, sizeof t);
  g_node->submit(Cls::Telemetry, 2, &h, sizeof h);
  g_node->submit(Cls::Telemetry, 3, &gas, sizeof gas);
}

static void printStatus() {
  Serial.printf("[STATUS] link=%s alarm=%s epoch=%u journal=%uB/%uB unacked crit=%u ctrl=%u dirty_tele=%u\n",
                g_link ? "UP" : "DOWN", g_alarm ? "LATCHED" : "clear", (unsigned)g_node->epoch(),
                (unsigned)g_node->journal_used(), (unsigned)JOURNAL_CAP, (unsigned)g_node->unacked(Cls::Critical),
                (unsigned)g_node->unacked(Cls::Control), (unsigned)g_node->dirty_telemetry());
  Serial.printf("[STATUS] gateway: crit_hwm=%u effects=%u ctrl_hwm=%u tele_v(temp)=%u.%u cmd_seq=%u acked=%u\n",
                (unsigned)g_gw.crit_hwm, (unsigned)g_gw.effects, (unsigned)g_gw.ctrl_hwm, (unsigned)g_gw.tele_epoch[1],
                (unsigned)g_gw.tele_ver[1], (unsigned)g_gw.cmd_seq, (unsigned)g_gw.cmd_acked);
  printValve();
}

static void issueCommand(uint8_t desired) {
  g_gw.cmd_seq++;
  g_gw.cmd_desired[g_gw.cmd_seq & 1] = desired;
  gwSave();  // persisted BEFORE it is sent
  Serial.printf("[GW] command v%u.%u SET %s issued and stored%s\n", (unsigned)g_gw.cmd_term, (unsigned)g_gw.cmd_seq,
                desired ? "CLOSED" : "OPEN", g_link ? "" : " (link down: will send when the node reconnects)");
}

static void sendPendingCommand() {
  if (!g_link || !g_welcomed || g_gw.cmd_seq <= g_gw.cmd_acked) return;
  Cmd c; c.node = NODE_ID; c.term = g_gw.cmd_term; c.seq = g_gw.cmd_seq; c.desired = g_gw.cmd_desired[g_gw.cmd_seq & 1];
  std::vector<uint8_t> v; encode_msg(c, v);
  toNode(v);
}

static void boot() {
  if (!LittleFS.begin(true)) Serial.println("[BOOT] LittleFS mount failed");
  g_journal.power_on();
  g_valveLog.power_on();
  size_t before = g_journal.size();
  gwLoad();
  NodeConfig cfg;
  cfg.node_id = NODE_ID; cfg.critical_reserve = 512; cfg.ack_batch = 2;
  delete g_node; delete g_valve;
  g_node = new Node(g_journal, g_clock, cfg);
  g_valve = new Actuator(g_valveLog);
  g_node->boot();
  g_valve->boot();
  size_t after = g_node->journal_used();
  Serial.println();
  Serial.printf("[BOOT] epoch=%u journal %uB -> %uB, recovered unacked crit=%u ctrl=%u\n", (unsigned)g_node->epoch(),
                (unsigned)before, (unsigned)after,
                (unsigned)g_node->unacked(Cls::Critical), (unsigned)g_node->unacked(Cls::Control));
  if (g_journal.bytes_cut() > 0)
    Serial.printf("[BOOT] torn tail cut: %u incomplete bytes from the interrupted write were removed\n", (unsigned)g_journal.bytes_cut());
  std::vector<Record> p = g_node->pending_records(Cls::Critical);
  if (!p.empty()) Serial.printf("[BOOT] pending critical seq %u..%u survived the restart\n", (unsigned)p.front().seq, (unsigned)p.back().seq);
  g_link = LINK_UP_AT_BOOT; g_welcomed = false; g_alarm = false; g_dropAck = false;
  g_lastCritFrame.clear();
  printValve();
  applyValveOutput();
  if (readGas() >= GAS_TRIP) {  // hazard still present after a reboot: local response first, again
    g_alarm = true; localAlarm(true); g_valve->set_interlock(true); applyValveOutput();
    Serial.println("[BOOT] gas still high: local alarm re-armed immediately");
    if (g_node->unacked(Cls::Critical) == 0) { g_alarm = false; gasEvent(readGas()); }
  } else {
    localAlarm(false);
  }
}

static void floodCriticals() {
  int ok = 0, full = 0;
  for (int i = 0; i < 60; i++) {
    char p[24]; snprintf(p, sizeof p, "FLOOD %d", i);
    uint64_t s = 0;
    SubmitResult r = g_node->submit(Cls::Critical, 2, p, strlen(p), &s);
    if (r == SubmitResult::Accepted) ok++; else { full++; if (full == 1) Serial.printf("[FULL] journal full after %d accepted records\n", ok); }
  }
  Serial.printf("[FLOOD] accepted=%d rejected=%d (rejections are explicit; accepted ones are never dropped)\n", ok, full);
}

static void handleCommand(char c) {
  switch (c) {
    case 'g': Serial.println("> g (simulate gas threshold crossing)"); gasEvent(GAS_TRIP + 300); break;
    case 'd': g_link = false; g_welcomed = false; Serial.println("> d  LINK DOWN: node keeps working locally"); break;
    case 'u': g_link = true; g_welcomed = false; Serial.println("> u  LINK UP"); sendHello(); break;
    case 'r': Serial.println("> r  rebooting..."); delay(100); ESP.restart(); break;
    case 'x': g_journal.arm_torn(9); Serial.println("> x  next journal write will be cut after 9 bytes (modeled power loss)"); break;
    case 'e': Serial.println("> e"); submitReport(Cls::Control, 2, "MANUAL control record"); break;
    case '1': Serial.println("> 1"); issueCommand(1); break;
    case '0': Serial.println("> 0"); issueCommand(0); break;
    case 'z':
      Serial.println("> z  reset alarm");
      if (readGas() > GAS_CLEAR) Serial.println("[ALARM] cannot reset: gas still above the clear level");
      else { g_alarm = false; localAlarm(false); g_valve->set_interlock(false); applyValveOutput(); Serial.println("[ALARM] alarm reset, interlock released"); printValve(); }
      break;
    case 'l': g_dropAck = true; Serial.println("> l  the next ACK will be lost"); break;
    case 'p':
      Serial.println("> p  replay the last critical frame");
      if (g_lastCritFrame.empty()) Serial.println("(nothing sent yet)"); else toGateway(g_lastCritFrame);
      break;
    case 's': {
      Serial.println("> s  replay an OLD telemetry snapshot");
      Record r; r.node = NODE_ID; r.cls = Cls::Telemetry; r.key = 1;
      r.epoch = g_gw.tele_epoch[1]; r.seq = g_gw.tele_ver[1] > 1 ? g_gw.tele_ver[1] - 1 : 0;
      int16_t t = 9999; r.len = sizeof t; memcpy(r.payload, &t, sizeof t);
      if (g_gw.tele_ver[1] == 0) { Serial.println("(no telemetry delivered yet: bring the link up first)"); break; }
      std::vector<uint8_t> v; encode_msg(r, v); toGateway(v);
      break;
    }
    case 'k':
      Serial.println("> k  replay the previous command");
      if (g_gw.cmd_seq < 2) Serial.println("(need two commands first: press 1 then 0)");
      else { Cmd o; o.node = NODE_ID; o.term = g_gw.cmd_term; o.seq = g_gw.cmd_seq - 1; o.desired = g_gw.cmd_desired[o.seq & 1];
             std::vector<uint8_t> v; encode_msg(o, v); toNode(v); }
      break;
    case 'f': Serial.println("> f"); floodCriticals(); break;
    case 'i': Serial.println("> i"); printStatus(); break;
    case 'w': Serial.println("> w  wiping filesystem"); LittleFS.format(); delay(100); ESP.restart(); break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_VALVE_LED, OUTPUT);
  pinMode(PIN_GAS, INPUT);
#if defined(USE_DHT) && USE_DHT
  dht.begin();
#endif
#if defined(USE_OLED) && USE_OLED
  Wire.begin(21, 22);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
#endif
  boot();
  Serial.println("Commands: g d u r x e 1 0 z l p s k f i w   (link starts DOWN: press u)");
}

void loop() {
  unsigned long now = millis();
  while (Serial.available()) handleCommand((char)Serial.read());

  if (g_journal.dead()) {  // the modeled power loss happened: the device is off until it reboots
    Serial.println("[POWER LOSS] storage write was cut mid-way; device restarts");
    delay(300);
    ESP.restart();
  }

  if (now - g_lastGas >= 100) {  // 1. sensor -> local response, independent of the network
    g_lastGas = now;
    int gas = readGas();
    if (!g_alarm && gas >= GAS_TRIP) gasEvent(gas);
  }
  if (now - g_lastSample >= 2000) { g_lastSample = now; sampleTelemetry(); }

  if (g_link) {
    if (!g_welcomed && now - g_lastHello >= 1500) sendHello();
    if (g_welcomed && now - g_lastTx >= 150) {  // 2. strict priority: Critical, then Control, then Telemetry
      g_lastTx = now;
      Record r;
      if (g_node->poll_send(r)) {
        std::vector<uint8_t> v; encode_msg(r, v);
        if (r.cls != Cls::Telemetry) g_lastProgress = now;
        if (r.cls == Cls::Critical) g_lastCritFrame = v;
        if (r.cls == Cls::Telemetry) Serial.printf("[TX] TELE key=%u v=%u.%u\n", r.key, (unsigned)r.epoch, (unsigned)r.seq);
        else Serial.printf("[TX] %s seq=%u\n", clsName(r.cls), (unsigned)r.seq);
        toGateway(v);
      }
    }
    if (g_welcomed && g_node->unacked(Cls::Critical) + g_node->unacked(Cls::Control) > 0 &&
        g_node->unsent(Cls::Critical) + g_node->unsent(Cls::Control) == 0 && now - g_lastProgress > 3000) {
      Serial.println("[NODE] no ACK for 3 s: re-synchronising");  // lost-ACK recovery
      g_welcomed = false; g_lastProgress = now;
    }
    if (g_welcomed && now - g_lastCmd > 1000) { g_lastCmd = now; sendPendingCommand(); }
    if (g_welcomed && now - g_lastSample > 1000 && g_node->dirty_telemetry() > 0 && now % 5000 < 200) g_node->retry_telemetry();
  }

#if defined(USE_OLED) && USE_OLED
  if (now - g_lastOled >= 1000) {
    g_lastOled = now;
    oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(0, 0);
    oled.printf("LIFELINE  epoch %u\n", (unsigned)g_node->epoch());
    oled.printf("link %s  alarm %s\n", g_link ? "UP" : "DOWN", g_alarm ? "ON" : "off");
    oled.printf("valve %s\n", valveName(g_valve->output()));
    oled.printf("pending crit %u\n", (unsigned)g_node->unacked(Cls::Critical));
    oled.printf("journal %uB/%uB\n", (unsigned)g_node->journal_used(), (unsigned)JOURNAL_CAP);
    oled.printf("gw effects %u", (unsigned)g_gw.effects);
    oled.display();
  }
#endif
}
