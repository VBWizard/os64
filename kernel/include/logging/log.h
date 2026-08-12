#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "smp.h"

//Set to sizeof(log_entry_t)*10 to enable buffer full processing

// 16MB/core (was 5): the DETAILED boot burst peaked at ~11.9k of 17.7k
// entries (~67% of 5MB) — and the burst happens BEFORE any filesystem
// exists, so only capacity can absorb it (the post-mount file sink handles
// steady state; see the klog plan). 3x headroom over worst observed; crank
// freely — never-drop is the house rule and RAM is cheap here.
#define LOG_BUFFER_SIZE_MB 16
#define LOG_BUFFER_SIZE (1024 * 1024 * LOG_BUFFER_SIZE_MB) // Size of buffer per core
#define MAX_LOG_MESSAGE_SIZE 256
// Drain twice a second: smaller accumulations per pass mean shorter serial
// bursts, which keeps QEMU/TCG from stalling the whole VM (and the guest
// clock) on one big once-a-second flush.
#define LOGD_SLEEP_TICKS (TICKS_PER_SECOND / 2)
// Bound on entries drained per kLogDWorkLock hold. The old until-empty merge
// never terminated under sustained production (DEBUG_DETAILED at 100 passes/
// sec): the daemon acquired the lock once and held it FOREVER — the stats
// heartbeat stopped printing, and the producers' forced-flush fallback could
// never take the lock, so its try-lock failure became the "buffer full"
// panic (2026-07-11, VBox). Bounded holds let the lock breathe; the daemon
// loops straight back (no sleep) while a backlog remains, so throughput is
// unchanged.
#define LOGD_DRAIN_CHUNK 64
// While a backlog persists, print the [logd] stats heartbeat at most once
// per this many passes (quiet passes always print) — the heartbeat must
// survive sustained load without becoming its own flood.
#define LOGD_STATS_EVERY 64
typedef struct log_entry {
    uint64_t ticks;     // kTicksSinceStart at log time — used for display
    uint64_t tsc;       // RDTSC at log time — used for cross-core sort order
    uint16_t core_id;
    uint8_t log_level;
    uint8_t category;
	uint64_t threadID;
    /// @brief message is the continuation of a previous entry
    bool continued;
    char message[MAX_LOG_MESSAGE_SIZE];
} log_entry_t;

typedef struct log_buffer {
    log_entry_t *entries;
    size_t head;
    size_t tail;
    size_t capacity;
    _Atomic uint32_t lock;
} log_buffer_t;

extern log_buffer_t core_log_buffers[MAX_CPUS];
extern bool kLoggingInitialized;
void logging_queueing_init();
void dump_log_buffer(uint16_t core);
void log_store_entry(uint16_t core, uint64_t ticks, uint8_t priority, uint8_t category, bool continued, const char *message);
bool logd_thread(bool daemon);

/// @brief Will printd output come out of the SERIAL PORT right now?
///
/// The exception reporter is the customer, and the question it is really
/// asking is "would a printd copy of this line be a SECOND copy on the wire?"
/// It writes serial directly (a fault report must survive the session), so it
/// adds the printd copy only when that copy goes somewhere ELSE — a LOGD= file
/// — rather than back onto the same wire.
///
/// Three states, only one of which is "yes": a daemon has claimed the log (no
/// — printd goes to its file), LOGD= was set and we are still holding the
/// drainer off the wire waiting (no — it queues, then reaches the file), or
/// nobody is coming (YES — the kernel drainer writes COM1 itself).
///
/// Guaranteed side-effect free — see the implementation for why that matters.
bool log_printd_reaches_serial(void);

// PANIC-ONLY: drain every core's queue to serial, right now, on THIS core.
// Busts kLogDWorkLock after a bounded grace wait (the holder may be halted or
// wedged — a possibly-garbled line beats a silently lost one, the
// renderer_bust_lock philosophy), then runs the oldest-first merge until the
// queues are empty or a total-capacity budget is spent (still-running cores
// can keep producing; we want the backlog, not the future). Never sleeps,
// never takes another lock — safe from any dying context.
void logd_emergency_flush(void);

// ── The ring-3 log sink (SYSCALL_KLOG_READ) ────────────────────────────────
// Dequeue up to `max` entries, oldest-first across every core (the same
// k-way TSC merge logd uses to print), into a caller-owned array. Returns
// the count taken. Entries are CONSUMED — the caller has accepted
// responsibility for them, which is exactly why the kernel may then stop
// writing them to serial.
uint32_t klog_dequeue(log_entry_t *out, uint32_t max);

// How long a claim survives without a read before the kernel takes serial
// logging back. Three seconds is far longer than any healthy daemon's poll
// interval (~100ms) and far shorter than a human notices — and it covers
// the HUNG daemon as well as the crashed one, which a task-liveness check
// would not.
#define LOG_SINK_TIMEOUT_TICKS (3 * TICKS_PER_SECOND)

// Set by klog_dequeue on every call; read by logd_thread to decide whether
// userland is still holding up its end. Not a lock — a heartbeat.
extern volatile bool kLogSinkClaimed;
extern volatile uint64_t kLogSinkLastRead;

// The claim is EXCLUSIVE while its holder is alive: entries are consumed by
// reading, so two concurrent readers would silently deal the log out between
// two files. syscall_klog_read calls this before dequeuing; a refusal means
// another daemon holds a live claim and the caller should say so and exit,
// not retry. A stale claim (heartbeat past LOG_SINK_TIMEOUT_TICKS) lapses on
// the next attempt, so daemon restarts need no hand-off ceremony.
bool klog_sink_try_claim(uint64_t taskId);

// ── The retire handshake (the shutdown slice, 2026-08-08) ──────────────────
// shutdown_system() → klog_request_retire(): the next EMPTY klog_read
// answers OS64_KLOG_RETIRED (klog.h) instead of 0, releasing the claim
// kernel-side in the same breath — the daemon's contract is commit, close,
// exit. klog_sink_is_claimed() is what shutdown polls to know the daemon
// got the word (a bounded wait: a dead daemon's claim lapses by heartbeat,
// so this can never wedge the descent).
extern volatile bool kKlogRetireRequested;
void klog_request_retire(void);
void klog_sink_release(void);
bool klog_sink_is_claimed(void);

// ── Waiting for the ring-3 sink (the LOGD= cmdline flag) ───────────────────
// When a log daemon is COMING but has not attached yet, draining to serial is
// pure waste: those same entries are about to be claimed and written to a
// file, and the serial copy costs a VM exit per byte for the loudest stretch
// of the whole boot. So with LOGD= set, the kernel drainer holds its fire
// from the first log line and lets the rings simply accumulate — they hold
// ~56,000 entries per core, which is a great deal of boot.
//
// Two escape hatches, because "never drop a byte" outranks "boot fast":
//
//   DEADLINE — if the daemon never attaches (missing binary, unwritable path,
//   crash on startup), waiting forever would mean a boot with NO log at all
//   and no clue why. After this long, serial comes back and says so.
//
//   HIGH WATER — if the rings fill faster than the daemon arrives, serial
//   comes back immediately regardless of the deadline. A full ring is the one
//   thing that actually loses entries.
//
// Either hatch abandons the wait PERMANENTLY for the rest of the boot: once
// the kernel has decided the daemon isn't coming, a late arrival still claims
// the sink through the normal heartbeat path, and nothing is lost either way.
#define LOG_SINK_AWAIT_TIMEOUT_TICKS (30 * TICKS_PER_SECOND)
#define LOG_SINK_AWAIT_HIGH_WATER_PCT 75

// The LOGD= path from the kernel commandline ("" = no userland sink expected).
// Its mere presence is what arms the wait above — the kernel does not need to
// know whether the daemon has been spawned yet, only that one is expected.
extern char kLogdPath[128];

#endif // LOG_H
