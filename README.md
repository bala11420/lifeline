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
