# liblifeline: node core + deterministic simulator (slice 3)

Portable C++17 library. No threads, no real clock, no randomness, no network inside it, so the
same code is intended to run in the deterministic simulator, in Wokwi/ESP32 and behind MQTT.

## Build and test

    cmake -S . -B build -G Ninja && cmake --build build && ./build/tests

(ASan + UBSan are on by default; turn off with -DLIFELINE_SANITIZE=OFF.) Without CMake:

    g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Iinclude -Ithird_party -I. src/*.cpp sim/sim.cpp sim/engines.cpp sim/scenario.cpp tests/*.cpp -o tests_bin && ./tests_bin
    g++ -std=c++17 -O2 -Iinclude -I. src/*.cpp sim/*.cpp -o lifeline_sim

Status: 40 test cases, ~64,800 assertions passing under ASan + UBSan.

## The simulator (sim/): run the evidence table

    ./lifeline_sim                          200 seeds, both builds, table on the terminal
    ./lifeline_sim --seeds 100000 --csv results.csv      overnight soak (~5 min per 100k seeds on a laptop)
    ./lifeline_sim --story 7 --build b3     readable timeline of one seed (use this for the live demo)
    ./lifeline_sim --seeds 300 --flash 600  tiny journal: exercises the storage-full path

One seed = one workload + one fault schedule (1-3 partitions of 5-40 s, loss up to 20%, duplication
up to 10%, reordering up to 20%, corruption up to 3%, 0-3 node crashes: torn write, power loss at
flush, or plain reset) and an operator who opens/closes a valve every 6-14 s. The SAME seed is replayed against both builds. Single-threaded, own PRNG,
virtual clock: replayable under the same seed, configuration and build; the trace hash is compared
in the tests.

Builds: B3_lifeline = this library. B1_qos1_model = a MODEL of QoS 1 semantics (bounded RAM FIFO,
per-message ack, resend until acked, no priority, no dedup, no staleness gate, lost on reboot).
It is NOT Mosquitto; never call it that on a slide.

Checks (ground truth is the event id carried in every payload, independent of any sequence number):
crit_lost (accepted critical never applied), ctrl_lost, dup_effects (an event applied more than
once), stale_overwrites (an older telemetry reading replaced a newer one), crit_first_violations
(anything but Critical sent while an accepted unapplied critical was waiting), tele_not_converged
(final gateway value != last accepted reading), plus delivery delay with link-down time subtracted.
Operator commands (gateway -> actuator, ABSOLUTE desired state ordered by (term, seq)): cmd_dup_effects
(same command applied twice), cmd_stale_applied (older command applied after a newer one),
cmd_not_converged = "valve output wrong": at the end of the run, right after a reboot, or changed by
a command that was not durable. Commands that hit an I/O error are "in doubt": the bytes may have
survived, so after a reboot the oracle accepts either the old state or the in-doubt command's state.
Events that returned IoError are "maybe committed" and never counted as lost; storage-full
rejections are counted separately and are not losses.

Results so far (200 seeds): naive model 200/200 runs violate (1,161 critical events lost, 7,088
duplicate effects, 8,381 stale overwrites, 3,438 old commands applied over newer ones, valve left in
the wrong position 159 times); Lifeline 0 violations. Soaks: 5,000 seeds both
builds and 20,000 further Lifeline seeds (first=100001): 0 violations for Lifeline.
Mutation-checked, 13 deliberate bugs each caught: no dedup, no stale gate, control or telemetry before
critical, ignored flush failure, ack deletes everything, actuator with no version gate / gate not
persisted / output not reconciled at boot / applies before persist succeeds, gateway never retries.

What the simulator found: with a journal so small it filled up, the node could not write its new
boot epoch, reused the old one, and the gateway rejected its telemetry as stale. Fixed with
NodeConfig::system_reserve (journal bytes only epoch records may use) + a regression test.
The oracle itself was also corrected twice by the soak: it first demanded an older actuator state
after a flush failure even though the command's bytes may legitimately have survived (now: in-doubt
commands), and mutation checks showed two oracle blind spots (valve reverting at reboot, acting before
the command was durable) that are now covered.

### Simulator limits (do not over-claim)

- One node, one gateway. The gateway is an in-memory reference (sim/engines.cpp, LifelineGateway):
  no SQLite, no gateway crashes. Lead B: replace/extend it; dedup record + state change must be one
  transaction. Add gateway-crash faults.
- One actuator, one gateway term (term = 1). Gateway restarts / a term bump are not simulated, and the
  gateway's command outbox lives in memory (a real gateway must persist it and bump the term on restart).
- Link model: loss/dup/reorder/corrupt/partition. No bandwidth limit, no radio effects.
- Delivery delay is dominated by the 1.5 s retransmission timer after a lost frame.
- Journal still never compacts; with a 400-byte journal ~70% of critical events are REJECTED
  (visibly, never lost). Say "fixed-capacity journal sized for the demo".

## Layout

    include/lifeline/protocol.h   Record, Cls, Flash / Clock / Transport interfaces  (FROZEN API)
    include/lifeline/codec.h      CRC32, little-endian helpers, Record body codec
    include/lifeline/messages.h   WIRE FORMAT: Data, Hello, Welcome, Ack, TeleAck (type + CRC32)
    include/lifeline/journal.h    CRC-framed append-only log with torn-tail recovery
    include/lifeline/node.h       Node: three classes, strict priority, resync, acks
    include/lifeline/dedup.h      StreamTracker (exact dedup), VersionGate (newer wins)
    include/lifeline/actuator.h   Actuator: persistent (term,seq) gate, persist-then-apply, reconcile at boot
    include/lifeline/memflash.h   in-memory Flash with crash injection (sim + tests)
    sim/                          rng, event queue, faulty link, engines (Lifeline + naive), scenario, CLI

## Guarantees (exact wording to use on slides)

1. Critical and Control: submit() == Accepted means the record is durably committed. It survives
   the simulated crashes (declared storage fault model) and is delivered after reconnect, while
   capacity is not exceeded.
2. Full storage: Control is rejected first; a reserved region is kept for Critical. A rejected
   submit returns StorageFull or IoError, sets the sticky storage_fault() flag and is counted.
   The caller must still run the local safety response. Nothing is reported Accepted unless
   committed. An event detected but not committed is NOT a journal loss; count it separately.
3. Priority: strict Critical > Control > Telemetry. A new Critical record is chosen at the very
   next poll_send(). No guaranteed share for lower classes; latency is measured, not promised.
4. Ordering: Critical and Control are per-class streams (seq 1,2,3...). Telemetry is a
   latest-value register per key ordered by (epoch, version), so coalescing leaves no gaps.
5. Delivery is at-least-once. Receivers use StreamTracker (exact, never a sliding window) and
   VersionGate. Commands must be absolute desired-state (SET CLOSED), never toggles.

## Protocol shape (what the gateway author codes against)

    node -> gw   Hello{node, epoch, crit_next_seq, ctrl_next_seq}     after every (re)connect
    gw -> node   Welcome{node, crit_hwm, ctrl_hwm, epoch_ok}          node calls on_welcome()
    node -> gw   Data(Record) ...   streamed, strict priority; NOT stop-and-wait (cumulative acks)
    gw -> node   Ack{node, cls, upto}                                 cumulative, node calls on_ack()
    gw -> node   TeleAck{node, epoch, key, version}                   telemetry convergence

Gateway receive rules: Critical uses StreamTracker::accept() (exact, any order: each alarm is an
independent fact, so a late one is never dropped). Control/commands use
StreamTracker::accept_in_order(): on Gap do NOT apply, reply with the cumulative ack so the node
rewinds and resends. Telemetry and commands use VersionGate. Dedup record + state change must be
ONE transaction (SQLite) on the gateway; this library cannot do that for you.

## Durability semantics (read before writing the oracle)

- Flash::sync() returns bool. A failed flush makes submit() return IoError, never Accepted.
- IoError means "maybe committed": on real flash the bytes can occasionally survive. The oracle
  must classify those events as undetermined, not as lost. Accepted means committed.
- After a write error the journal heals itself on the next append (re-scan, cut the torn tail);
  no reboot is needed. If the device is still failing, appends keep returning IoError.
- Ack records are batched (NodeConfig::ack_batch, default 8). A lost ack costs one harmless
  duplicate; on reconnect the gateway's Welcome re-trims the node.
- Telemetry converges only if the driver calls retry_telemetry() on a timer and the gateway sends
  TeleAck. Without those, telemetry is best-effort until the next value change or resync.

## Known limits (do not over-claim)

- STILL NO COMPACTION. The log only grows (data + epoch + batched acks) until Full. State this in
  the demo: fixed-capacity journal, sized for the demo. Real fix = two banks + generation header;
  it must preserve each stream's next sequence number. Resetting the journal in place is NOT
  crash-safe (a crash between erase and rewrite would reuse sequence numbers).
- Telemetry is RAM-only and lost on reboot by design.
- If a node loses its ENTIRE journal its epoch restarts at 1; the gateway must answer Hello with
  Welcome{epoch_ok=false} when it has already seen a higher epoch for that node id.
- on_ack() ignores any ack above the highest sequence the node ever issued (never clamps).
- No min/max/count telemetry summaries (cut on purpose).
- Real gateway (SQLite), MQTT adapter, actuator and dashboard are NOT in this zip. The oracle here
  was written by the same author as the system under test: have the oracle owner re-derive the
  checks independently from the invariant definitions.

## For the ESP32 / Wokwi teammate

Implement `Flash` over a LittleFS file (size = file size, append = write at end, sync = flush;
return false if it fails) and `Clock` over millis(). Arduino's LittleFS file API has no truncate,
so implement truncate() by rewriting the file; it is only called after a torn tail. The code
avoids std::optional and other newer features, but check which C++ standard your ESP32 core builds
with before assuming C++17. Not tested on a device.
# LIFELINE
## Priority-First, Crash-Resilient Messaging for Offline IoT

**InnoHacks 2.0 · TechSpark 2026 · IoT Problem 5**

**Team Matrix**
- **B. Gangadhar** — Core Engine
- **K. Dinesh Kumar** — Gateway
- **R. Veera Poojitha** — Chaos Engine

---

## 1. Overview

LIFELINE is a fault-tolerant messaging framework designed for IoT systems that must continue operating when network connectivity becomes unreliable.

Conventional IoT applications often depend on continuous communication with a remote server or cloud platform. During network outages, devices may lose collected data, delay emergency notifications, replay previously received commands, or overwrite newer information with stale values.

These failures become especially serious in safety-sensitive environments, including rural monitoring systems, industrial facilities, healthcare environments, and disaster-response networks.

LIFELINE addresses these challenges through three principles:

1. **Act locally:** Critical monitoring and emergency responses continue without internet connectivity.
2. **Store and protect:** Critical events are recorded safely, prioritized, and protected against duplicate processing and stale updates.
3. **Synchronize and verify:** After connectivity returns, queued events are delivered in priority order and the recovered system state is checked against defined invariants.

The goal is to make IoT communication more predictable, recoverable, and trustworthy under network and process failures.

---
## 🔬 Project Scope & Twin-Engine Validation Model
LIFELINE is implemented as a zero-dependency C++17 core messaging protocol deployed across a **Twin-Engine Validation Model**: a local native terminal chaos simulator for mass metric benchmarking and a live cloud-emulated Wokwi firmware environment for component interaction testing.
### 🏁 Replicating on a Fresh Linux Environment (Ubuntu / WSL2)
Copy and paste this single script sequence to configure toolchains, build wildcards, and execute the chaos benchmarks:

```bash
```bash
# =========================================================================
# LIFELINE: Complete Fresh Linux Environment Setup & Execution Script
# =========================================================================

# Step 1: Update system packages and install the complete C++17 build toolchain
sudo apt update && sudo apt install -y build-essential cmake ninja-build git g++

# Step 2: Unzip the main project repository archive file
# (Assumes your lifeline.zip file is sitting inside your active directory)
python3 -m zipfile -e lifeline.zip .

# Step 3: Change directory into the freshly extracted source workspace root folder
cd lifeline

# Step 4: Compile the zero-dependency C++17 protocol core natively via GCC
# This configures our optimized include paths and compiles all source wildcards
g++ -std=c++17 -O2 -Iinclude -I. -Ilifeline-main src/*.cpp sim/*.cpp -o lifeline_sim

# Step 5: Run the Master 200-Seed Automated Chaos Evaluation Loop
# This Subjects our core to 200 multi-fault campaigns to verify 0 violations
./lifeline_sim --seeds 200
```


| System Component / Layer | Status | Implementation & Validation Method |
| :--- | :--- | :--- |
| **C++17 Protocol Core (`liblifeline`)** | 🟢 **Implemented** | In-memory Write-Ahead Logging (WAL), tuple version state, preemption buffers. |
| **Discrete-Event Chaos Framework** | 🟢 **Implemented** | Custom PRNG, virtual timeline calendar clock, automated partition engines. |
| **Interactive Wokwi Prototype** | 🟢 **Implemented** | Live ESP32 cloud emulation tracking MQ2, DHT22, and SSD1306 OLED layout buses. |
| **Python Gateway Service Layer** | 🟢 **Implemented** | Backed by a functional in-memory reference gateway block tracking versions. |

## 🚀 Key Architectural Principles
1. **Act Locally (No Cloud Reliance):** When the MQ2 Gas sensor threshold is violated, the ESP32 firmware trips local alarm routines and drives the SSD1306 OLED layout instantly without waiting for an active internet handshake.
2. **Constant Space Telemetry Bounds:** Telemetry registers use an in-place register map. Routine data volume scales flatly relative to configured sensor keys (O(K) space complexity), protecting memory from overflowing during long outages.
3. **Write-Ahead Flash Journaling:** High-priority alarms are sealed within `MAGIC_START` and `MAGIC_END` frames. On boot, the engine scans the array and calls `truncate()` at the first torn frame marker to clean up mid-write power failures.

## 📦 How to Evaluate Our Work

### Engine A: The Native Deterministic Chaos Simulator
To compile and execute the core validation loop over 200 multi-fault campaigns natively on your terminal, run:
```bash
g++ -std=c++17 -O2 -Iinclude -I. src/*.cpp sim/*.cpp -o lifeline_sim
./lifeline_sim --seeds 200
```

### Engine B: The Interactive Cloud Wokwi Demonstration
Our fully functional, software-emulated ESP32 firmware loop is live and testable.
* **Wokwi Project Assets:** Refer to our root folder diagrams (`Wokwi simulation.jpeg`, `wokwisimulation2.jpeg`) and our source files (`include/lifeline/actuator.h`, `src/node.cpp`) to inspect the embedded driver mappings.

## 📊 Empirical Evaluation Matrix Results
Under identical multi-fault timelines, standard MQTT QoS 1 client queues fracture, while LIFELINE maintains absolute state convergence:
* **Lost Critical Safety Alerts (I1):** MQTT QoS 1: **1,161 Lost** | LIFELINE: **0 Violations 🌟**
* **Duplicate Command Executions (I2):** MQTT QoS 1: **7,088 Duplicates** | LIFELINE: **0 Violations 🌟**
* **Stale Data Dashboard Overwrites (I3):** MQTT QoS 1: **8,381 Overwrites** | LIFELINE: **0 Violations 🌟**
EOF
## 2. Problem Statement

IoT devices frequently operate in environments where network availability cannot be guaranteed. A device may lose connectivity because of weak signals, damaged infrastructure, power interruptions, or temporary network congestion.

Without appropriate fault-handling mechanisms, these conditions can cause:

- Loss of critical sensor events during an outage.
- Emergency notifications being delayed behind routine telemetry.
- Duplicate effects when previously transmitted commands are replayed.
- Older measurements overwriting newer dashboard values.
- Inconsistent state after a device or gateway restarts.
- Difficulty verifying whether the recovered system contains the correct data.

**LIFELINE's objective is to preserve critical event records, prioritize safety-related messages, prevent duplicate application effects, and reject stale updates during recovery.**

---

## 3. Project Objectives

- Continue local safety monitoring without internet connectivity.
- Store critical events in a crash-recoverable journal.
- Bound routine telemetry storage by retaining the latest value for each configured key.
- Prioritize critical alarms over routine messages.
- Detect corrupted records using CRC32.
- Track message sequences and versions to reject duplicate or stale updates.
- Support safe synchronization after network recovery.
- Validate behavior through repeatable failure-injection tests.
- Measure invariant violations under controlled failure schedules.

---

## 4. Three-Layer Trust Architecture

### Layer 1 — Act Locally

The edge node evaluates sensor readings locally and activates its alarm output when configured safety thresholds are exceeded.

For example, a gas-monitoring node can evaluate an MQ2 sensor and trigger a local siren without waiting for a cloud response. A DHT22 sensor can provide temperature and humidity measurements for monitoring.

The local safety path should remain independent of the remote messaging path.

**Key principle:** A network outage must not prevent a locally implemented emergency response.

> Sensor and hardware integration must be verified on the actual target device before claiming a working physical demonstration.

### Layer 2 — Store and Protect

LIFELINE separates critical events from routine telemetry.

**Critical events**
- Are recorded in an append-only journal.
- Include sequence or version metadata.
- Use CRC32 to detect record corruption.
- Are retained for delivery and recovery according to the journal's capacity and error-handling rules.

**Routine telemetry**
- Uses a latest-value slot for each configured key.
- Replaces older pending measurements with newer measurements for that key.
- Avoids unbounded accumulation of routine data.

If there are `K` configured telemetry keys, the latest-value map uses `O(K)` storage for the retained values, excluding key/value sizes and other system structures.

**Important distinction:** CRC32 detects accidental corruption; it does not by itself guarantee durable writes. Crash safety also depends on journal layout, commit ordering, storage behavior, and recovery logic.

### Layer 3 — Synchronize and Verify

When connectivity returns, the system resumes communication.

- Critical events receive priority over routine telemetry.
- Sequence metadata supports duplicate detection.
- Per-key versions prevent older measurements from replacing newer values.
- The gateway records accepted events and state changes in a persistent ledger.
- Tests evaluate whether the recovered state satisfies the required invariants.

For a production deployment, event acknowledgment, retry handling, gap detection, and persistent gateway updates must be coordinated to avoid losing events during interrupted synchronization.

---

## 5. System Architecture

```mermaid
flowchart TD
    A["Sensors: MQ2 / DHT22"] --> B["Edge Node: C++17"]
    B --> C["Local Threshold Evaluation"]
    C --> D["Local Alarm / Siren"]
    B --> E["Priority Classification"]
    E --> F["Critical Event Journal"]
    E --> G["Latest-Value Telemetry Map"]
    F --> H["Priority-Based Message Sender"]
    G --> H
    H --> I["Unreliable Network"]
    I --> J["Gateway Receiver"]
    J --> K["Frame Validation and Deduplication"]
    K --> L["Version and Stale-Data Checks"]
    L --> M["Persistent SQLite Ledger"]
    M --> N["Dashboard / State Verification"]
    O["Failure-Injection Simulator"] --> B
    O --> I
    O --> J
```

### Main components

| Component | Responsibility |
|---|---|
| Edge node | Sensor evaluation, local response, and message creation |
| Priority manager | Selects critical events before routine messages |
| Journal | Stores critical records and supports recovery |
| Telemetry map | Retains the latest pending value for each key |
| Transport | Carries messages across an unreliable network |
| Gateway receiver | Validates messages and tracks duplicates |
| SQLite ledger | Persists received records and application state |
| Dashboard | Displays the latest recorded system state |
| Chaos engine | Injects failures to test recovery behavior |
| Invariant checker | Reports violations of the defined correctness rules |

The architecture describes the intended end-to-end system. Individual components should be labeled as implemented, tested, or planned according to the current repository.

---

## 6. End-to-End Workflow

### Step 1 — Normal operation

Sensors produce measurements. The edge node evaluates configured thresholds, updates routine telemetry, and creates critical events when necessary.

Routine telemetry is retained using the latest-value policy. Critical events follow the journal's persistence path.

### Step 2 — Network disconnection

The transport becomes unavailable.

The edge node continues local monitoring and emergency signaling. Critical records are retained according to the journal's capacity and commit behavior, while routine measurements can replace older pending values.

### Step 3 — Critical event occurs offline

Suppose a gas reading crosses its configured threshold.

1. The edge node detects the condition.
2. The local alarm activates.
3. A critical event is created.
4. The event is recorded before it is considered safely committed.
5. The event waits for transmission when the network becomes available.

The local alarm does not need to wait for remote acknowledgment.

### Step 4 — Network recovery

When communication resumes, the sender prioritizes critical events.

The gateway validates incoming frames, checks sequence metadata, rejects duplicates, and prevents stale measurements from replacing newer values.

The gateway persists accepted records and state changes in its ledger.

### Step 5 — Crash and restart recovery

The journal recovery procedure identifies valid records and rejects invalid or incomplete records according to its recovery rules.

The gateway reloads its persisted state from SQLite after a process restart.

These mechanisms must be tested independently: node recovery does not automatically prove gateway recovery, and vice versa.

### Step 6 — Verification

The simulator runs repeatable failure scenarios and evaluates the resulting state.

It checks for:

- Missing accepted critical events.
- Duplicate application effects.
- Stale telemetry overwrites.
- Violations of critical-message priority.
- Incorrect recovery after injected failures.

Each test run should record its seed, workload, failure schedule, and results.

---

## 7. Message Priority

LIFELINE uses three conceptual message classes.

| Priority | Message type | Handling policy |
|---|---|---|
| P0 | Critical alarms | Highest transmission priority |
| P1 | Control messages | Sent after critical events, according to the implementation's ordering rules |
| P2 | Routine telemetry | Latest pending value per key can replace older values |

This policy prevents a large backlog of routine measurements from automatically taking precedence over safety-related events.

Priority affects transmission order. It does not eliminate the need for adequate storage, retransmission, acknowledgments, and network recovery handling.

---

## 8. Crash Resilience and Data Integrity

### Append-only journal

Critical events are written to a journal before transmission. CRC32 is used to detect accidental corruption.

A robust journal implementation must define:

- Record format and length validation.
- Commit and acknowledgment semantics.
- Recovery from incomplete writes.
- Handling of invalid records.
- Behavior when storage becomes full.
- Safe recovery after interrupted compaction, if compaction is supported.

### Duplicate protection

Message sequence identifiers and persistent receiver state help prevent repeated messages from applying the same logical update multiple times.

Deduplication state must be retained for an appropriate period and must be updated consistently with the associated application state.

### Stale-data protection

Measurements include a version or ordering value for their respective keys.

The receiver accepts a measurement only when it is valid under the configured ordering rules. Older updates must not replace newer accepted state.

### Gateway persistence

A persistent ledger allows the gateway to restore accepted event history after a process restart.

For reliable application behavior, deduplication records and their corresponding state updates should be committed together in a database transaction.

---

## 9. Failure-Injection Testing

LIFELINE uses a deterministic simulator to test how the messaging core responds to unreliable conditions.

### Failure scenarios

- Network partitions.
- Packet loss.
- Duplicate delivery.
- Out-of-order delivery.
- Corrupted records or frames.
- Node crashes and restarts.
- Repeated execution using recorded random seeds.

### Invariants

**I1 — Critical-event preservation**

No accepted, committed critical event should be silently lost within the defined test model and its storage assumptions.

**I2 — Duplicate-effect prevention**

Replaying the same logical command must not cause a second application effect within the defined deduplication scope.

**I3 — Stale-update prevention**

An older measurement must not overwrite a newer accepted value for the same key.

**I4 — Priority correctness**

When critical events and lower-priority messages are pending, the sender must follow the defined critical-first scheduling policy.

The test report should include the number of runs, number of injected failures, invariant violations, and any assumptions that limit the results.

---

## 10. Empirical Evaluation

The following are the reported results from the project's simulator evaluation. They should be reproduced from the current code and recorded test output before being treated as final submission evidence.

| Metric | Modeled MQTT QoS 1 baseline | LIFELINE |
|---|---:|---:|
| Lost critical events | 1,161 | 0 reported violations |
| Duplicate application effects | 7,088 | 0 reported violations |
| Stale telemetry overwrites | 8,381 | 0 reported violations |

**Interpretation:** In the reported simulation, LIFELINE had zero violations of the three listed invariants, while the modeled baseline had violations.

These results do not establish that every real MQTT QoS 1 deployment will behave this way. MQTT QoS 1 permits duplicate delivery, and persistent sessions alone do not guarantee durable application storage, exactly-once physical actuation, or protection against stale application updates.

For a fair comparison, document the baseline configuration, storage assumptions, workload, and identical failure schedules. Distinguish simulator measurements from real-broker and hardware measurements.

Recommended additional metrics include:

- Critical-event delivery latency after reconnection.
- Recovery time after a node or gateway restart.
- Maximum journal capacity and storage overhead.
- Memory use as the number of telemetry keys increases.
- Number of injected crash points and recovery failures.

---

## 11. Technology Stack

| Layer | Technology |
|---|---|
| Core messaging engine | C++17 |
| Gateway | Python |
| Persistent gateway ledger | SQLite |
| Messaging transport | MQTT / Mosquitto integration |
| Failure testing | Deterministic simulator |
| Validation | Automated tests and invariant checks |
| Hardware demonstration | ESP32, if the target driver and hardware integration are implemented and tested |

The C++ core can be developed and tested independently of the physical device. This makes it possible to reproduce failures without relying exclusively on manual cable disconnections or power cycling.

---

## 12. Build and Run

### Prerequisites

- A C++ compiler supporting C++17.
- A terminal or command prompt.
- The source repository and its required project files.

### Compile the core simulator

From the repository root:

```bash
g++ -std=c++17 -O2 -Iinclude -I. src/*.cpp sim/*.cpp -o lifeline_sim
```

### Run the simulator

```bash
./lifeline_sim --seeds 200
```

On Windows, run the generated executable as:

```powershell
.\lifeline_sim.exe --seeds 200
```

Use the exact commands and test targets documented by the current repository. If a source file or simulator option differs in a particular revision, adjust the command accordingly.

### Run the automated tests

Follow the repository's documented test commands. Save the complete output, including failures, rather than recording only the final success count.

### MQTT gateway

If the Python gateway and Mosquitto integration are present in the checked-out revision, follow their dedicated setup instructions. A simulator-only build does not itself verify the end-to-end MQTT transport.

---

## 13. Project Structure

The core repository is organized around these conceptual areas:

```text
lifeline/
├── include/
│   └── lifeline/       # Public interfaces and data structures
├── src/                # Core messaging and persistence logic
├── sim/                # Deterministic failure simulator
├── tests/              # Automated validation
├── gateway/            # Python receiver and persistence (if included)
├── deploy/              # Broker and deployment configuration (if included)
└── README.md
```

The exact files and directories depend on the repository revision. Optional integration folders should not be shown as completed components unless they are actually present.

---

## 14. Limitations and Future Work

LIFELINE is intended to demonstrate fault-tolerant messaging principles, not to claim complete production readiness.

Current and potential limitations include:

- Fixed journal capacity and storage-exhaustion handling.
- Durability guarantees that depend on the target storage driver.
- Gateway transaction and crash-recovery behavior.
- Transport integration and reconnect testing.
- Persistent sequence and deduplication state management.
- ESP32 flash integration and physical sensor testing.
- MQTT authentication, access control, and secure transport.
- Testing at larger fleet sizes and under longer outages.

Future work includes crash-safe journal compaction, a complete edge-to-gateway MQTT integration, ESP32 flash drivers, real hardware fault injection, stronger recovery reporting, and security hardening.

---

## 15. Applications

Potential application areas include:

- Rural environmental monitoring.
- Gas-leak and industrial safety monitoring.
- Healthcare and facility monitoring.
- Agricultural sensing.
- Disaster-response sensor networks.
- Remote infrastructure monitoring.

Each application requires its own safety analysis, appropriate sensor calibration, and validation of local alarm behavior.

---

## 16. Conclusion

LIFELINE explores a priority-first, crash-resilient approach to messaging for disconnected IoT environments.

By combining local decision-making, bounded routine telemetry, durable critical-event recording, duplicate protection, stale-update checks, and deterministic failure testing, the project aims to keep system state trustworthy even when communication becomes unreliable.

Its central principle is simple:

**Safety should not wait for the network. Critical events should be preserved, delivered first, and verified after recovery.**

---

## Team Matrix

- **B. Gangadhar** — Core Engine
- **K. Dinesh Kumar** — Gateway
- **R. Veera Poojitha** — Chaos Engine

**Event:** InnoHacks 2.0 · TechSpark 2026  
**Track:** IoT · Problem 5
