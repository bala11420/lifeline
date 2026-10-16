// Plain C interface so the Python harness and demo can drive the REAL core (liblifeline) with
// simulated flash. One "device" = a node journal on one MemFlash + an actuator on another.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ll_dev ll_dev;

ll_dev* ll_new(uint32_t node_id, size_t flash_capacity, size_t critical_reserve, size_t ack_batch,
               size_t act_flash_capacity);
void ll_free(ll_dev*);

// Simulated reboot: RAM state is lost, flash is power-cycled (optionally losing unsynced bytes),
// a new Node/Actuator are constructed over the same flash and booted. Returns 1 if boot() ok.
int ll_reboot(ll_dev*, int lose_unsynced);
void ll_set_time(ll_dev*, uint64_t ms);

// cls: 0 Critical, 1 Control, 2 Telemetry.  Returns SubmitResult (0 Accepted, 1 Full, 2 IoError,
// 3 TooLarge) and the assigned sequence/version in *seq.
int ll_submit(ll_dev*, int cls, uint16_t key, const uint8_t* data, size_t len, uint64_t* seq);
// Next encoded Data frame in strict priority order; returns its length, 0 if nothing to send.
size_t ll_poll_send(ll_dev*, uint8_t* buf, size_t cap);
size_t ll_hello(ll_dev*, uint8_t* buf, size_t cap);
// Feed any gateway->node frame. Returns MsgType (2..7), or -1 if invalid. For Cmd, an encoded
// CmdAck is written to out/*outlen and *detail = CmdResult. For Welcome, *detail = epoch_ok.
int ll_on_frame(ll_dev*, const uint8_t* in, size_t n, uint8_t* out, size_t outcap, size_t* outlen,
                int* detail);
size_t ll_retry_telemetry(ll_dev*);

uint32_t ll_epoch(ll_dev*);
size_t ll_unacked(ll_dev*, int cls);
size_t ll_unsent(ll_dev*, int cls);
size_t ll_journal_used(ll_dev*);
size_t ll_flash_capacity(ll_dev*);
int ll_storage_fault(ll_dev*);
int ll_flash_dead(ll_dev*);
// Copies up to max (cls, seq, first-8-payload-bytes-as-u64) triples of unacked durable records.
size_t ll_pending(ll_dev*, int cls, uint64_t* seqs, uint64_t* oids, size_t max);

// Storage fault injection on the node journal.
void ll_arm_torn(ll_dev*, size_t bytes);
void ll_arm_sync_fail(ll_dev*);
void ll_power_cycle(ll_dev*, int lose_unsynced);  // device healthy again, node NOT rebooted

// Actuator.
void ll_set_interlock(ll_dev*, int on);
// out: have, term, seq, desired, output, interlock
void ll_act_state(ll_dev*, uint32_t* term, uint64_t* seq, int* have, int* desired, int* output,
                  int* interlock);

#ifdef __cplusplus
}
#endif
