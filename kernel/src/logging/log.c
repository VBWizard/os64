#include "log.h"
#include "serial_logging.h"
#include "io.h"
#include "panic.h"
#include "kernel.h"
#include "smp.h"
#include "kmalloc.h"
#include "CONFIG.h"
#include "sprintf.h"
#include "memset.h"
#include "smp_core.h"
#include "task.h"
#include "scheduler.h"  //for scheduler_wake_task
#include "signals.h"
// TODO: Implement dump_log_buffer() to handle emergency log flushes when buffer is full in place of panicking

extern task_t* kLogDTask;
extern volatile uint64_t kTicksSinceStart;
log_buffer_t core_log_buffers[MAX_CPUS];
bool kLoggingInitialized = false;
extern struct limine_smp_response *kLimineSMPInfo;
// Ensures only one logd worker processes the buffers at a time
_Atomic uint32_t kLogDWorkLock = 0;
// Which core currently holds kLogDWorkLock (0xFFFFFFFF = nobody) — purely
// forensic, for the no-progress panic in log_store_entry.
volatile uint32_t kLogDDrainerCore = 0xFFFFFFFF;
// The ring-3 log sink's heartbeat (see log.h). Claimed by the first
// klog_dequeue; kept alive by every one after it.
volatile bool kLogSinkClaimed = false;
volatile uint64_t kLogSinkLastRead = 0;

// True while a userland log daemon is reading and hasn't gone quiet. The
// staleness test is what makes the hand-off safe: a daemon that crashes,
// hangs, or gets killed simply stops stamping, and serial comes back
// without anyone having to notice or clean up.
static bool log_sink_alive(void)
{
	return kLogSinkClaimed &&
	       (kTicksSinceStart - kLogSinkLastRead) <= LOG_SINK_TIMEOUT_TICKS;
}

// WHICH task holds the live claim (0 = nobody). The heartbeat alone can't
// tell two daemons apart: entries are CONSUMED by reading, so two readers
// would each get a random half of the log — each file individually looking
// plausible, jointly dropping nothing yet showing nobody everything. That
// failure mode arrived the day husk.rc made "logd &" typeable while the
// LOGD= cmdline flag was already launching one. Exclusivity with a loud
// refusal (tripwires over silence) beats discovering it in two half-files.
volatile uint64_t kLogSinkOwnerTask = 0;

// Claim the sink for `taskId`, or refuse. The owner may always re-claim
// (that IS the heartbeat); a stale claim (owner dead/hung past the timeout)
// lapses right here, so a restarted-by-hand daemon walks in without any
// cleanup step. The CAS settles the two-daemons-racing-at-boot case: one
// wins, the other is told no.
bool klog_sink_try_claim(uint64_t taskId)
{
	if (!log_sink_alive())
		kLogSinkOwnerTask = 0;   // the previous claimant went quiet — lapsed

	uint64_t current = kLogSinkOwnerTask;
	if (current == taskId)
		return true;
	if (current != 0)
		return false;            // a LIVE claim by somebody else: exclusive
	return __sync_bool_compare_and_swap(&kLogSinkOwnerTask, 0, taskId);
}

// ── The retire handshake (2026-08-08, the shutdown slice) ────────────────────
// shutdown_system() must not pull the power with log bytes still in the rings
// or the log file still open. It cannot reach into a ring-3 daemon, but it
// owns the one door the daemon knocks on every poll: klog_read. Setting this
// flag makes the NEXT empty poll answer OS64_KLOG_RETIRED instead of 0 — the
// daemon commits, closes, and exits; the claim is released kernel-side at the
// same moment so shutdown can watch kLogSinkOwnerTask for the all-clear.
// Never-drop-a-byte survives shutdown: RETIRED is only spoken when the
// dequeue came back empty, so every logged byte precedes the farewell.
volatile bool kKlogRetireRequested = false;

void klog_request_retire(void)
{
	kKlogRetireRequested = true;
}

void klog_sink_release(void)
{
	kLogSinkOwnerTask = 0;
	kLogSinkClaimed = false;
}

bool klog_sink_is_claimed(void)
{
	return log_sink_alive();
}

// Has a userland sink EVER attached this boot? Distinct from kLogSinkClaimed,
// which goes false again when a daemon dies: this one is a one-way latch, and
// it is what ends the initial wait for good.
static volatile bool kLogSinkEverClaimed = false;
// Set when the wait is given up on (deadline or ring pressure). Also one-way:
// having decided the daemon is not coming, the kernel does not re-decide every
// pass and re-print the explanation.
static volatile bool kLogSinkAwaitAbandoned = false;

void log_note_sink_claimed(void)
{
	kLogSinkEverClaimed = true;
}

// See log.h for who asks and why. Deliberately NOT "is logd running" — the
// question is strictly "will printd's bytes come out of COM1?", because the
// only caller is deciding whether its own direct serial write would be
// duplicated.
//
// SIDE-EFFECT FREE, and that is a requirement rather than a nicety: the pure
// condition is spelled out again here instead of calling
// log_awaiting_userland_sink(), which owns the two escape hatches and both
// SETS a flag and prints. A fault reporter must be able to ask this question
// without changing the answer for everybody else.
bool log_printd_reaches_serial(void)
{
	// A daemon owns the log: printd goes to its file, never to the wire.
	if (kLogSinkEverClaimed)
		return false;
	// LOGD= was requested and we are still holding the drainer off the wire
	// waiting for it. Entries queue now and reach the file when it attaches.
	if (kLogdPath[0] != '\0' && !kLogSinkAwaitAbandoned)
		return false;
	// Nobody is coming (or nobody was asked): the kernel drainer is putting
	// printd straight onto COM1.
	return true;
}

// Should the drainer stay silent because a log daemon is expected but has not
// attached yet? See the LOG_SINK_AWAIT_* commentary in log.h for the why; this
// is the where. Called once per drain pass, and it OWNS the two escape
// hatches — including printing the reason, because a kernel that silently
// stopped logging and silently started again is worse than either state.
static bool log_awaiting_userland_sink(void)
{
	if (kLogdPath[0] == '\0' || kLogSinkEverClaimed || kLogSinkAwaitAbandoned)
		return false;

	// Hatch 1: it's been too long. The daemon isn't coming.
	if (kTicksSinceStart > LOG_SINK_AWAIT_TIMEOUT_TICKS)
	{
		kLogSinkAwaitAbandoned = true;
		serial_print_string("[logd] LOGD= was set but no userland sink attached in time — kernel draining to serial\n");
		return false;
	}

	// Hatch 2: the rings are filling. This one outranks the deadline, because
	// a full ring is where entries actually die. Checked against the loudest
	// core, not the average — one core at capacity loses just as much.
	for (int c = 0; c < kMPCoreCount; c++)
	{
		log_buffer_t *b = &core_log_buffers[c];
		size_t used = (b->head >= b->tail)
		            ? b->head - b->tail
		            : b->capacity - b->tail + b->head;
		if (b->capacity && (used * 100) / b->capacity >= LOG_SINK_AWAIT_HIGH_WATER_PCT)
		{
			kLogSinkAwaitAbandoned = true;
			serial_print_string("[logd] log rings hit high water before a userland sink attached — kernel draining to serial\n");
			return false;
		}
	}

	return true;
}

void log_store_entry(uint16_t core, uint64_t ticks, uint8_t priority, uint8_t category, bool continued, const char *message)
{
	core_local_storage_t *cls = get_core_local_storage();

	if (!kLoggingInitialized)
		return;
	if (core >= MAX_CPUS) panic("Invalid core ID in log_store_entry: %u", core);

	log_buffer_t *buffer = &core_log_buffers[core];

    log_entry_t *entry = &buffer->entries[buffer->head];
    entry->ticks = ticks;
    // TSC is valid for cross-core comparison on QEMU (single host TSC source).
    // On real hardware, requires invariant TSC (CPUID 0x80000007 EDX bit 8).
    __asm__ volatile("rdtsc" : "=a"(*(uint32_t*)&entry->tsc), "=d"(*((uint32_t*)&entry->tsc + 1)));
    entry->core_id = core;
    entry->log_level = priority;
    entry->category = category;
    entry->continued = continued;
	if (kSMPInitDone)
		entry->threadID = cls->currentThread->threadID;
	else
		entry->threadID = 0;
    snprintf(entry->message, MAX_LOG_MESSAGE_SIZE, "%s", message);
    entry->message[MAX_LOG_MESSAGE_SIZE-1] = '\0';
    buffer->head = (buffer->head + 1) % buffer->capacity;

    //If the log buffer is *full*: flush it ourselves, or wait for the drain
    //that is already running. NEVER drop a byte (house rule) — and never
    //panic while help is mid-flight: the old instant-panic on a failed
    //try-lock fired precisely when another context was ALREADY draining our
    //queue (the k-way merge covers every core), i.e. exactly when patience
    //would have won (2026-07-11, VBox DETAILED).
    //NOTE: Putting the thread to sleep is still *a bad idea* — the scheduler
    //itself calls printd(), and the scheduler cannot sleep.
    if ((buffer->head + 1) % buffer->capacity == buffer->tail)
    {
        // PERMANENT tripwire: if this prints, production outran logd and a
        // producer is paying drain costs — fine in a thread, brutal inside
        // the scheduler; see DEBTS for the never-drop remedies. printf ON
        // PURPOSE: it bypasses these queues, so it can't be drowned by the
        // flood it's reporting on.
        static volatile uint64_t forcedFlushCount = 0;
        printf("LOGFULL: core %u queue full at tick %lu (occurrence #%lu)\n",
               core, kTicksSinceStart, __sync_add_and_fetch(&forcedFlushCount, 1));

        uint64_t idleSpins = 0;
        size_t lastTail = buffer->tail;
        while ((buffer->head + 1) % buffer->capacity == buffer->tail)
        {
            //Try to run a drain chunk ourselves. False = someone else holds
            //kLogDWorkLock and is draining right now — our queue included.
            if (logd_thread(false))
            {
                lastTail = buffer->tail;
                idleSpins = 0;
                continue;
            }
            __builtin_ia32_pause();
            if (buffer->tail != lastTail)
            {
                //The other drainer is making progress — keep waiting.
                lastTail = buffer->tail;
                idleSpins = 0;
            }
            //ZERO progress for an eon means the drainer can never run again —
            //e.g. it lives on THIS core and we are spinning above it in
            //interrupt context. A loud forensic panic beats a silent wedge.
            else if (++idleSpins > 2000000000UL)
                panic("log_store_entry: core %u queue full with NO drain progress (drainer=core %u, head=%lu tail=%lu)\n",
                      core, kLogDDrainerCore, (unsigned long)buffer->head, (unsigned long)buffer->tail);
        }
    }
}

void logging_queueing_init() {
    for (int i = 0; i <  (int)kLimineSMPInfo->cpu_count; i++) {
        // Allocate memory for each core's log buffer
        core_log_buffers[i].entries = (log_entry_t *)kmalloc(LOG_BUFFER_SIZE); 
        core_log_buffers[i].capacity = LOG_BUFFER_SIZE / sizeof(log_entry_t);
        core_log_buffers[i].head = 0;  // Initialize head pointer
        core_log_buffers[i].tail = 0;  // Initialize tail pointer
        core_log_buffers[i].lock = 0;  // Initialize lock
    }
	kLoggingInitialized = true;
}

// Print one entry from a buffer and advance its tail.
// Always drains immediately-following continued entries from the same buffer
// so multi-chunk messages are never split by the interleave logic.
static void logd_drain_one(log_buffer_t *buffer)
{
    // Worst-case formatted output: 20 (%lu ticks) + 22 ((0x%016lx threadID))
    // + 9 (AP65535:) + 1 (space) + 255 (message) + NUL = 308 bytes.
    // Use 512 to stay well clear even if metadata is corrupted.
    char print_buf2[MAX_LOG_MESSAGE_SIZE + 256];
    log_entry_t *entry = &buffer->entries[buffer->tail];

    if (!entry->continued)
        snprintf(print_buf2, sizeof(print_buf2),
                 "%lu (0x%04lx) AP%u: %s",
                 entry->ticks,
                 entry->threadID,
                 entry->core_id,
                 entry->message);
    else
        snprintf(print_buf2, sizeof(print_buf2), "%s", entry->message);

    serial_print_string(print_buf2);
    entry->message[0] = '\0';
    buffer->tail = (buffer->tail + 1) % buffer->capacity;

    // Drain any continuation chunks that belong to this entry before yielding
    while (buffer->head != buffer->tail &&
           buffer->entries[buffer->tail].continued)
    {
        log_entry_t *cont = &buffer->entries[buffer->tail];
        snprintf(print_buf2, sizeof(print_buf2), "%s", cont->message);
        serial_print_string(print_buf2);
        cont->message[0] = '\0';
        buffer->tail = (buffer->tail + 1) % buffer->capacity;
    }
}

// ── The ring-3 sink's dequeue (SYSCALL_KLOG_READ) ──────────────────────────
// The same oldest-first k-way merge logd_thread runs, except the entries
// are COPIED OUT instead of printed. Taking kLogDWorkLock is what makes
// that safe: only the lock holder advances tail pointers, so the kernel
// daemon, the kworker drain, and this syscall can never disagree about
// what has been consumed.
uint32_t klog_dequeue(log_entry_t *out, uint32_t max)
{
    // Stamp the heartbeat FIRST — before any early return. A reader that
    // finds the queues empty is still a live reader, and if an empty poll
    // didn't count as a sign of life, a quiet system would hand serial
    // logging back and forth every three seconds.
    kLogSinkClaimed = true;
    kLogSinkLastRead = kTicksSinceStart;
    // One-way latch: ends the LOGD= startup wait for good. From here on the
    // ordinary heartbeat governs the hand-off, including a daemon that dies
    // and one that is restarted by hand later.
    log_note_sink_claimed();

    if (!kLoggingInitialized || out == NULL || max == 0)
        return 0;

    // Try-lock, never spin: this runs on a syscall from ring 3, and
    // blocking a user process behind a drain that another core is already
    // doing would trade a bounded wait for an unbounded one. Nothing is
    // lost by returning 0 — the caller polls again in 100ms and the
    // entries are still there.
    if (__sync_lock_test_and_set(&kLogDWorkLock, 1))
        return 0;
    kLogDDrainerCore = get_core_local_storage()->apic_id;

    uint32_t taken = 0;
    while (taken < max)
    {
        int best_core = -1;
        uint64_t best_tsc = UINT64_MAX;
        for (int c = 0; c < kMPCoreCount; c++)
        {
            log_buffer_t *buf = &core_log_buffers[c];
            if (!buf->entries || buf->head == buf->tail)
                continue;
            uint64_t t = buf->entries[buf->tail].tsc;
            if (t < best_tsc) { best_tsc = t; best_core = c; }
        }
        if (best_core < 0)
            break;   // every ring is empty

        log_buffer_t *buf = &core_log_buffers[best_core];
        out[taken++] = buf->entries[buf->tail];          // struct copy: the
                                                         // caller owns it now
        buf->entries[buf->tail].message[0] = '\0';
        buf->tail = (buf->tail + 1) % buf->capacity;
    }

    kLogDDrainerCore = 0xFFFFFFFF;
    __sync_lock_release(&kLogDWorkLock);
    return taken;
}

bool logd_thread(bool daemon) {
    thread_t *self = get_core_local_storage()->currentThread;
    bool nonDaemonRunSuccess = false;
    static uint32_t drain_pass = 0;
    static bool was_claimed = false;

    while (1) {
        int processed_logs = 0;
        bool backlog = false;

        // HAND-OFF: while a userland log daemon is reading, the kernel
        // does NOT drain to serial — those entries belong to the file now,
        // and printing them too would both duplicate the log and pay the
        // 115200-baud tax the hand-off exists to escape. The moment the
        // sink goes quiet (crash, hang, kill), this test fails and serial
        // draining resumes on the very next pass — which is why a dead log
        // daemon's death is always visible SOMEWHERE.
        if (log_sink_alive())
        {
            was_claimed = true;
            if (!daemon)
                return false;
            sigaction(SIGSLEEP, NULL, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
            continue;
        }

        // The same hand-off, one step earlier: LOGD= says a daemon is coming,
        // so don't spend the boot pushing bytes out a serial port that the
        // file is about to own anyway. Deliberately placed AFTER the
        // sink-alive test and BEFORE was_claimed's "resuming serial" notice,
        // so this quiet stretch is never mistaken for a daemon that died.
        if (log_awaiting_userland_sink())
        {
            if (!daemon)
                return false;
            sigaction(SIGSLEEP, NULL, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
            continue;
        }
        if (was_claimed)
        {
            was_claimed = false;
            kLogSinkClaimed = false;
            serial_print_string("[logd] userland log sink went quiet — kernel resuming serial drain\n");
        }

        // Try-lock: if another CPU is already flushing, skip this wakeup
        if (!__sync_lock_test_and_set(&kLogDWorkLock, 1))
        {
            kLogDDrainerCore = get_core_local_storage()->apic_id;
            if (kLoggingInitialized)
            {
                // k-way merge: on each step pick the core whose oldest entry
                // has the lowest TSC, print it, repeat — BOUNDED at
                // LOGD_DRAIN_CHUNK entries per lock hold (see log.h: the old
                // until-empty loop never terminated under sustained
                // production, welding the lock shut forever).
                // Only logd updates tail pointers so no per-buffer lock needed.
                bool any;
                do {
                    any = false;
                    int   best_core = -1;
                    uint64_t best_tsc  = UINT64_MAX;

                    for (int c = 0; c < kMPCoreCount; c++)
                    {
                        log_buffer_t *buf = &core_log_buffers[c];
                        if (!buf->entries || buf->head == buf->tail)
                            continue;
                        uint64_t t = buf->entries[buf->tail].tsc;
                        if (t < best_tsc) {
                            best_tsc  = t;
                            best_core = c;
                            any = true;
                        }
                    }

                    if (best_core >= 0) {
                        logd_drain_one(&core_log_buffers[best_core]);
                        processed_logs++;
                    }
                } while (any && processed_logs < LOGD_DRAIN_CHUNK);
                backlog = any;
            }
            nonDaemonRunSuccess = processed_logs > 0;
            drain_pass++;

            // Print queue depths directly to serial, so we can monitor buffer
            // pressure without adding to the ring buffers themselves (which
            // would skew the numbers). Quiet passes always print; while a
            // backlog persists the heartbeat is rate-limited to every
            // LOGD_STATS_EVERY-th pass so it can't flood the wire it reports
            // on. Per-pass visibility cracked the 2026-07 slow-walk, and the
            // 2026-07-11 eternal-drain bug was spotted precisely because this
            // line STOPPED printing — keep it alive under all conditions. AP
            // is the core the pass ran on — kworker drains from AP1 every 2s
            // through this same code, so without it the stats can't tell the
            // two drainers apart.
            if (!backlog || (drain_pass % LOGD_STATS_EVERY) == 0)
            {
                char stats[128];
                int pos = snprintf(stats, sizeof(stats),
                    "[logd] AP%u tick=%lu pass=%u drained=%d",
                    get_core_local_storage()->apic_id, kTicksSinceStart, drain_pass, processed_logs);
                for (int c = 0; c < kMPCoreCount && pos < (int)sizeof(stats) - 32; c++) {
                    log_buffer_t *b = &core_log_buffers[c];
                    size_t used = (b->head >= b->tail)
                        ? b->head - b->tail
                        : b->capacity - b->tail + b->head;
                    pos += snprintf(stats + pos, sizeof(stats) - pos,
                        " AP%d=%lu/%lu", c, (unsigned long)used, (unsigned long)b->capacity);
                }
                snprintf(stats + pos, sizeof(stats) - pos, "\n");
                // This needs to be controlled by a config setting ... was useful when testing logd issues, isn't now
                //serial_print_string(stats);
            }

            kLogDDrainerCore = 0xFFFFFFFF;
            __sync_lock_release(&kLogDWorkLock);
        }
        if (!daemon)
            return nonDaemonRunSuccess;
        // Backlog remaining (chunk bound hit): come straight back for more —
        // sleeping now would cap throughput at CHUNK/LOGD_SLEEP_TICKS.
        if (backlog)
            continue;
        // Queues drained dry: sleep, wake up periodically regardless.
        sigaction(SIGSLEEP, NULL, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
    }
}

// See log.h. The one caller is panic — this is the "never drop a byte" rule
// applied to the byte that matters most: a panic that dies with the evidence
// still queued in RAM is worse than one that garbles a line getting it out.
//
// Exists because logd_thread(false) is NOT a flush: it's a bounded try-lock
// drain — one LOGD_DRAIN_CHUNK if the lock is free, NOTHING if it isn't —
// and the panic message (newest TSC, drained last in the oldest-first merge)
// is precisely the entry a bounded pass leaves behind. Then cli/hlt orphans
// the queue forever if the daemon lived on the dying core.
void logd_emergency_flush(void)
{
    if (!kLoggingInitialized)
        return;

    // Give a live drainer a moment to finish — a clean handoff beats a mid-
    // line bust when the holder is actually making progress (~a few ms).
    bool locked = false;
    for (uint64_t spins = 0; spins < 20000000UL; spins++)
    {
        if (!__sync_lock_test_and_set(&kLogDWorkLock, 1))
        {
            locked = true;
            break;
        }
        __builtin_ia32_pause();
    }
    if (!locked)
    {
        // Holder is halted or wedged (it may be the reason we're panicking).
        // Bust the lock and take over its drain mid-line; the interleave is
        // the price of completeness.
        kLogDWorkLock = 1;
    }
    kLogDDrainerCore = get_core_local_storage()->apic_id;

    // Budget = every slot in every queue, so cores that are still alive and
    // producing can't hold us hostage: we flush the backlog that existed when
    // the panic hit (plus whatever slips in while we drain), then stop.
    uint64_t budget = 0;
    for (int c = 0; c < kMPCoreCount; c++)
        budget += core_log_buffers[c].capacity;

    while (budget--)
    {
        // Same oldest-first k-way merge as logd_thread, unbounded by chunks.
        int best_core = -1;
        uint64_t best_tsc = UINT64_MAX;
        for (int c = 0; c < kMPCoreCount; c++)
        {
            log_buffer_t *buf = &core_log_buffers[c];
            if (!buf->entries || buf->head == buf->tail)
                continue;
            uint64_t t = buf->entries[buf->tail].tsc;
            if (t < best_tsc)
            {
                best_tsc = t;
                best_core = c;
            }
        }
        if (best_core < 0)
            break;   // every queue empty: the backlog is on the wire
        logd_drain_one(&core_log_buffers[best_core]);
    }

    // Release so surviving cores' logd can keep draining whatever they
    // produce after this point — the panic'd core is done, they may not be.
    kLogDDrainerCore = 0xFFFFFFFF;
    __sync_lock_release(&kLogDWorkLock);
}

