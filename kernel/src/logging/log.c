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
                serial_print_string(stats);
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

