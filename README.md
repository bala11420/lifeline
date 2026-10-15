# LIFELINE: Priority-First, Crash-Resilient Messaging for Offline IoT

**IoT Problem 5 Track · InnoHacks 2.0 (TechSpark 2026)**
**Team Matrix:** B. Gangadhar (Core Engine) · K. Dinesh Kumar (Gateway) · R. Veera Poojitha (Chaos Engine)

LIFELINE is a zero-dependency C++17 messaging framework designed to preserve data integrity and system trust during extensive network drops and host crashes.

## 🚀 The Three-Layer Trust Architecture
1. **Act Locally:** Edge nodes evaluate sensor parameters (MQ2 Gas, DHT22 Temp/Humidity) and fire emergency sirens immediately via local pins without waiting for a cloud connection.
2. **Store & Protect:** Routine telemetry is managed in a flat, single-slot map that overwrites old entries to bound storage at **Constant Space O(K) complexity**, completely avoiding memory crashes. High-priority alarms are stamped with a unique `(boot_epoch, per_key_version)` tuple and written to an append-only Write-Ahead flash journal with a trailing CRC32 check before transmission.
3. **Synchronize & Verify:** On link recovery, critical alarms skip the line via **Preemptive Priority**. The Python gateway receiver utilizes an atomic sequence validation block to drop duplicate frames, and references its SQLite ledger to permanently reject out-of-order, stale metrics.

## 📦 Build and Test

To compile and verify the core validation loops with your team's exact wildcards, execute:

```bash
g++ -std=c++17 -O2 -Iinclude -I. src/*.cpp sim/*.cpp -o lifeline_sim
./lifeline_sim --seeds 200
```

## 📊 Empirical Evaluation Matrix Results
Under identical failure schedules, standard MQTT QoS 1 queues completely fracture, while LIFELINE maintains absolute state convergence:

*   **Lost Critical Safety Alerts (I1):** MQTT QoS 1: **1,161 Lost** | LIFELINE: **0 Violations 🌟**
*   **Duplicate Command Executions (I2):** MQTT QoS 1: **7,088 Duplicates** | LIFELINE: **0 Violations 🌟**
*   **Stale Data Dashboard Overwrites (I3):** MQTT QoS 1: **8,381 Overwrites** | LIFELINE: **0 Violations 🌟**

