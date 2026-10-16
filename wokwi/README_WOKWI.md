# LIFELINE in Wokwi (ESP32)

What runs: the **real Lifeline core** (journal, CRC32 frames, sequences, priority queue, actuator) on a
simulated ESP32, storing its journal in **LittleFS**. The gateway is a small stand-in inside the sketch
(same rules as `gateway/gateway.py`) whose state is also saved in LittleFS.

## Set up (Wokwi web)
1. New project -> ESP32 (Arduino). Replace `sketch.ino` with `wokwi/sketch.ino`.
2. Add every `ll_*.h`, `ll_*.cpp` and `esp_flash.h` from this folder as extra files (flat, no folders).
3. Paste `libraries.txt` into the Libraries tab. `diagram.json` is optional (pins at the top of the sketch).
4. Pins used: MQ2 AOUT=GPIO34, DHT22 SDA=GPIO15, OLED SDA=21/SCL=22, buzzer=GPIO25, valve LED=GPIO27.
   If your own diagram differs, change the constants at the top of `sketch.ino`.
5. Start the simulation and type one letter in the serial monitor (Enter after it). The link starts DOWN.

## Commands
| key | does | what it demonstrates |
|---|---|---|
| `g` | gas threshold crossing | local alarm first, then durable journal write, `ACCEPTED seq=N` |
| (slider) | move the MQ2 slider above/below the trip | same, with hysteresis: one excursion = one record |
| `d` / `u` | link down / up | offline operation; HELLO/WELCOME handshake and flush on reconnect |
| `r` | reboot | pending records and sequence numbers survive; next record continues the count |
| `x` | tear the next journal write | modeled power loss mid-write: not accepted, torn bytes cut on boot, no sequence reuse |
| `e` | control record | priority: CRIT before CTRL before TELE on reconnect |
| `1` / `0` | gateway issues SET CLOSED / OPEN | desired-state commands with persisted versions |
| `k` | replay previous command | rejected as STALE |
| `p` | replay last critical frame | gateway says DUPLICATE, no new effect |
| `s` | replay old telemetry | gateway says STALE, rejected |
| `l` | lose the next ACK | gateway committed, node re-syncs, no duplicate effect |
| `f` | flood critical records | finite journal: explicit FULL, accepted records untouched |
| `z` | reset alarm | allowed only when gas is below the clear level; releases the interlock |
| `i` / `w` | status / wipe flash | |

## 2-minute script
`g` -> `d` is already true at boot -> `g` again (debounced) -> `r` (note: recovered unacked crit=1, seq continues)
-> `x`, `g` (IO ERROR, POWER LOSS, torn tail cut) -> `u` (CRIT first, then TELE) -> `p`, `s` (duplicate/stale
rejected) -> `1`, `g`, `0` (interlock: desired=OPEN output=CLOSED) -> `k` (stale command).

## How to describe it honestly
"A Wokwi simulation of the Lifeline core on an ESP32 with LittleFS. Power loss mid-write is a modeled fault
injected in software, and the gateway is a stand-in inside the sketch. The full protocol is verified
separately in the C++ test suite." Do **not** say "tested on real ESP32 hardware", and do not call the modeled
fault a measured property of real flash.

## What is verified where
* `host_test/run_scenarios.py` compiles this exact sketch with a host Arduino/LittleFS stand-in (C++11) and
  checks 28 assertions over the 7 scenarios above. That checks the logic, not Wokwi itself.
* It has **not** been run inside Wokwi or compiled with the ESP32 toolchain from here. If the compiler
  reports a name clash or a missing library, send the error and it is a small fix.
* Assume stopping the simulation may reset the simulated flash; use `r` (software reboot) for persistence demos.
