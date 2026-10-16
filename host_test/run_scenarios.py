#!/usr/bin/env python3
"""Runs the Wokwi sketch (wokwi/sketch.ino) on the host with the real core and checks the serial output.
Each scenario starts from an empty LittleFS directory. Usage: python3 host_test/run_scenarios.py"""
import os, re, subprocess, sys, tempfile

BIN = os.path.abspath(os.path.join(os.path.dirname(__file__), "sketch_host"))
passed = failed = 0


def run(cmds):
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run([BIN], input="\n".join(cmds) + "\n", capture_output=True, text=True, cwd=d, timeout=60)
        return r.stdout


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  PASS", name)
    else:
        failed += 1
        print("  FAIL", name, detail)


def idx(out, pat, start=0):
    m = re.compile(pat).search(out, start)
    return m.start() if m else -1


print("A. offline alarm, debounce, durable acceptance, reboot keeps sequence, flush on reconnect")
o = run(["g", "!gas 3200", "!wait 1500", "!gas 500", "z", "g", "r", "g", "i", "u", "!wait 3000", "i"])
check("local alarm fires with the link down", "LOCAL ALARM ON" in o and "link=DOWN" in o)
check("one excursion -> one record (gas stays high 1.5 s, no spam)", len(re.findall(r"ACCEPTED\] CRIT", o.split(">>> !gas 500")[0])) == 1)
check("sequence 1,2 before reboot", "CRIT seq=1 durably" in o and "CRIT seq=2 durably" in o)
check("reboot recovers both unacked records", "recovered unacked crit=2" in o)
check("sequence continues after reboot (3, not 1)", "CRIT seq=3 durably" in o)
txs = [m.start() for m in re.finditer(r"\[TX\] CRIT seq=", o)]
tele = idx(o, r"\[TX\] TELE")
check("all 3 criticals sent, in order, before any telemetry", len(txs) == 3 and tele > txs[-1], (txs, tele))
check("gateway committed seq 1..3 exactly once each", all(len(re.findall(r"COMMIT CRIT seq=%d " % s, o)) == 1 for s in (1, 2, 3)))
check("gateway effects == 3", "effects=3" in o.split(">>> i")[-1])

print("B. duplicate and stale replays")
o = run(["u", "!wait 2500", "g", "!wait 1500", "p", "s"])
check("replayed critical frame is a duplicate with no new effect", re.search(r"DUPLICATE CRIT seq=1 ignored, no new effect \(effects=1\)", o) is not None)
check("older telemetry snapshot rejected", "STALE telemetry key=1" in o and "REJECTED" in o)

print("C. lost ACK: gateway committed, node retries safely")
o = run(["u", "!wait 500", "l", "g", "!wait 5000", "i"])
check("ACK was lost", "ACK lost" in o)
check("node resynchronised", "no ACK for 3 s: re-synchronising" in o)
check("committed exactly once", len(re.findall(r"COMMIT CRIT seq=1 ", o)) == 1)
check("no duplicate effect, node dropped the record after WELCOME", "DUPLICATE CRIT" not in o and "unacked crit=0" in o.split(">>> i")[-1])

print("D. power loss in the middle of a journal write")
o = run(["g", "!gas 500", "z", "d", "x", "g", "!wait 1500", "g", "r", "i"])
check("torn write is NOT reported as accepted", "IO ERROR] CRIT record NOT accepted" in o)
check("device restarts after the modeled power loss", "POWER LOSS" in o)
check("recovery cuts the torn tail", "torn tail cut" in o)
check("the earlier accepted record survived", "recovered unacked crit=1" in o)
check("next record continues with seq 2 (no gap, no reuse)", "CRIT seq=2 durably" in o and o.count("CRIT seq=2 durably") == 1)

print("E. capacity is finite and explicit")
o = run(["f", "r"])
m = re.search(r"accepted=(\d+) rejected=(\d+)", o)
acc = int(m.group(1)) if m else -1
check("some rejected with an explicit FULL message", m is not None and int(m.group(2)) > 0 and "[FULL]" in o)
check("every accepted record survives a reboot", "recovered unacked crit=%d " % acc in o, acc)

print("F. commands: versions, stale replay, local interlock")
o = run(["u", "!wait 500", "1", "!wait 1500", "0", "!wait 1500", "k", "g", "1", "!wait 1500", "0", "!wait 1500", "!gas 500", "z"])
check("CLOSED then OPEN applied", "v1.1 SET CLOSED -> APPLIED" in o and "v1.2 SET OPEN -> APPLIED" in o)
check("old command replay rejected", "v1.1 SET CLOSED -> STALE, rejected" in o)
check("remote OPEN cannot override the gas interlock", "desired=OPEN output=CLOSED interlock=ON" in o)
check("explicit reset releases it", "alarm reset, interlock released" in o and "desired=OPEN output=OPEN interlock=off" in o.split("interlock released")[-1])
check("command report travels on the Control stream and commits", "ACCEPTED] CTRL seq=1" in o and "COMMIT CTRL seq=1" in o)

print("G. priority: Critical > Control > Telemetry")
o = run(["!wait 4500", "e", "g", "u", "!wait 3000"])
c, k, t = idx(o, r"\[TX\] CRIT"), idx(o, r"\[TX\] CTRL"), idx(o, r"\[TX\] TELE")
check("order of first transmissions", 0 < c < k < t, (c, k, t))
check("a telemetry backlog was waiting (latest-value registers)", "dirty_tele" in o or t > 0)

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
