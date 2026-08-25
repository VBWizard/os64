#include "log.h"
#include "os64/klog_format.h"   // the ONE renderer, shared with logd
#include "serial_logging.h"
#include "strings/strcmp.h"
#include "time.h"               // gmtime — %d/%t's calendar arithmetic
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
// ── the category-name tripwire ──────────────────────────────────────────────
// %g renders a name from a table in <os64/klog_format.h>, because logd has to
// print it too and cannot include CONFIG.h. Two files holding one truth is
// exactly how a log comes to label SCHEDULER lines "NET", so the build checks
// them against each other. Move a DEBUG_* bit without updating the table and
// this stops the compile with the flag's name in the message.
#define LOGCAT_CHECK(flag, idx) \
	_Static_assert((flag) == ((__uint128_t)1 << (idx)), \
	               #flag " changed bit position — update os64_logcat_name() in abi/include/os64/klog_format.h");
LOGCAT_CHECK(DEBUG_EXCEPTIONS,    OS64_LOGCAT_EXCEPTIONS)
LOGCAT_CHECK(DEBUG_BOOT,          OS64_LOGCAT_BOOT)
LOGCAT_CHECK(DEBUG_SMP,           OS64_LOGCAT_SMP)
LOGCAT_CHECK(DEBUG_PCI_DISCOVERY, OS64_LOGCAT_PCI_DISCOVERY)
LOGCAT_CHECK(DEBUG_PCI,           OS64_LOGCAT_PCI)
LOGCAT_CHECK(DEBUG_HARDDRIVE,     OS64_LOGCAT_HARDDRIVE)
LOGCAT_CHECK(DEBUG_AHCI,          OS64_LOGCAT_AHCI)
LOGCAT_CHECK(DEBUG_MEMMAP,        OS64_LOGCAT_MEMMAP)
LOGCAT_CHECK(DEBUG_ACPI,          OS64_LOGCAT_ACPI)
LOGCAT_CHECK(DEBUG_PAGING,        OS64_LOGCAT_PAGING)
LOGCAT_CHECK(DEBUG_ALLOCATOR,     OS64_LOGCAT_ALLOCATOR)
LOGCAT_CHECK(DEBUG_DEMAND_PAGING, OS64_LOGCAT_DEMAND_PAGING)
LOGCAT_CHECK(DEBUG_NVME,          OS64_LOGCAT_NVME)
LOGCAT_CHECK(DEBUG_VFS,           OS64_LOGCAT_VFS)
LOGCAT_CHECK(DEBUG_THREAD,        OS64_LOGCAT_THREAD)
LOGCAT_CHECK(DEBUG_TASK,          OS64_LOGCAT_TASK)
LOGCAT_CHECK(DEBUG_SCHEDULER,     OS64_LOGCAT_SCHEDULER)
LOGCAT_CHECK(DEBUG_SIGNALS,       OS64_LOGCAT_SIGNALS)
LOGCAT_CHECK(DEBUG_LOGGING,       OS64_LOGCAT_LOGGING)
LOGCAT_CHECK(DEBUG_TESTS,         OS64_LOGCAT_TESTS)
LOGCAT_CHECK(DEBUG_SYSCALL,       OS64_LOGCAT_SYSCALL)
LOGCAT_CHECK(DEBUG_GUI,           OS64_LOGCAT_GUI)
LOGCAT_CHECK(DEBUG_APPLICATION,   OS64_LOGCAT_APPLICATION)
LOGCAT_CHECK(DEBUG_TASKSWITCH,    OS64_LOGCAT_TASKSWITCH)
LOGCAT_CHECK(DEBUG_PIPE,          OS64_LOGCAT_PIPE)
LOGCAT_CHECK(DEBUG_SHUTDOWN,      OS64_LOGCAT_SHUTDOWN)
LOGCAT_CHECK(DEBUG_USB,           OS64_LOGCAT_USB)
LOGCAT_CHECK(DEBUG_DIAG,          OS64_LOGCAT_DIAG)
LOGCAT_CHECK(DEBUG_NET,           OS64_LOGCAT_NET)
LOGCAT_CHECK(DEBUG_SYSTEM,        OS64_LOGCAT_SYSTEM)
LOGCAT_CHECK(DEBUG_CLIPBOARD,     OS64_LOGCAT_CLIPBOARD)
LOGCAT_CHECK(DEBUG_SPECIAL,       OS64_LOGCAT_SPECIAL)

log_buffer_t core_log_buffers[MAX_CPUS];
bool kLoggingInitialized = false;
// Serial's line format (see log.h). CLASSIC by default, so a boot that passes
// no LOGFMT= prints exactly what os64 has always printed — a format change
// must never be able to cost someone the view they know.
const char *kLogFormat = OS64_LOGFMT_CLASSIC;
bool kLogFormatWantsClock = false;   // classic asks for no clock
// LOGFMT= named something we don't have. Set before there is any way to say
// so; reported in kernel_main once serial and the framebuffer exist.
bool kLogFormatBad = false;

bool log_set_format_by_name(const char *name)
{
	// The name table lives in <os64/klog_format.h> so logd resolves "daily"
	// through the identical code — see os64_logfmt_by_name's comment.
	const char *fmt = os64_logfmt_by_name(name);
	if (fmt == NULL)
		return false;
	kLogFormat = fmt;
	kLogFormatWantsClock = os64_logfmt_uses_clock(kLogFormat) != 0;
	return true;
}

void log_wallclock_now(void *out)
{
	os64_logtime_t *t = (os64_logtime_t *)out;
	// kSystemCurrentTime is seconds since the epoch, set from the RTC during
	// hardware_init. Zero means we are earlier than that — the first boot
	// lines genuinely predate the clock — and .valid = 0 makes the renderer
	// print placeholders instead of a confident 1970.
	if (kSystemCurrentTime == 0) {
		t->valid = 0;
		return;
	}
	time_t now = (time_t)kSystemCurrentTime;
	struct tm tm;
	gmtime(&now, &tm);
	t->year  = (uint16_t)(tm.tm_year + 1900);   // tm_year is years since 1900
	t->mon   = (uint8_t)(tm.tm_mon + 1);        // tm_mon is 0-based
	t->day   = (uint8_t)tm.tm_mday;
	t->hour  = (uint8_t)tm.tm_hour;
	t->min   = (uint8_t)tm.tm_min;
	t->sec   = (uint8_t)tm.tm_sec;
	t->valid = 1;
	// UTC, deliberately: the system clock is UTC by ruling (2026-08-08), and a
	// timezone is a human's display preference. logd applies TZ for the FILE
	// because it has an environment to read one from; serial has neither.
}
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
            //NO progress means nobody is draining and nobody can: the sink is
            //gone and serial either isn't there (the P5 has no UART at all) or
            //can't keep up. This used to PANIC after two billion spins — an
            //eon of frozen machine, then a dead one, to protect a logging
            //rule. The ruling (2026-08-18): "never drop a byte is
            //ASPIRATIONAL". Growth is impossible here, so the choice is not
            //WHETHER to lose lines but WHICH, and a panic loses all of them
            //plus the machine. So: keep the newest. Drop the oldest entry,
            //count it, and let the write proceed.
            //
            //Which end to keep is the whole question, and it is answered
            //differently one buffer over: the pre-allocator BSS log keeps the
            //FIRST lines (its job is the start of the boot), and these rings
            //keep the LAST (their job is the recent past, and the last line
            //before a freeze is the one you came for). Two policies because
            //two questions.
            //
            //The patience threshold is small now that the outcome is
            //survivable rather than fatal — a slow-but-progressing drainer
            //resets it on every entry it takes, so this only expires when
            //truly nothing is moving.
            else if (++idleSpins > LOG_FULL_PATIENCE_SPINS)
            {
                //This bump is UNLOCKED on purpose — do not "fix" it with
                //kLogDWorkLock: this path runs in interrupt context, possibly
                //ON TOP of the drainer it would be waiting for, and that is a
                //deadlock. The race it buys was traced (2026-08-18 review): a
                //drainer waking in this same instant advances tail too, both
                //sides read T and store T+1, and one update is lost. Worst
                //case is `lost` overcounting by one — the entry was in fact
                //drained — never corruption: this producer writes entries at
                //HEAD, and the full-ring gap slot keeps the slot being
                //drained and the slot being written apart. A CAS would close
                //the overcount if the number ever needs to be exact.
                buffer->tail = (buffer->tail + 1) % buffer->capacity;
                buffer->lost++;
                break;
            }
        }
    }
}

// ── the pre-allocator log (see log.h for the why) ───────────────────────────
// 64 entries is ~20KB of BSS and about fifteen times what a default boot puts
// here (four lines). The margin is for a boot that turns on a chatty flag
// early; past it we keep the first 64 and count the rest, because the first
// lines of a boot are the ones this buffer exists for.
#define EARLY_LOG_ENTRIES 64
static log_entry_t kEarlyEntries[EARLY_LOG_ENTRIES];
static uint32_t kEarlyCount = 0;
static uint32_t kEarlyLost  = 0;

void log_store_early(uint16_t core, uint64_t ticks, uint8_t priority,
                     uint8_t category, const char *message)
{
	if (kEarlyCount >= EARLY_LOG_ENTRIES) {
		kEarlyLost++;
		return;
	}
	log_entry_t *e = &kEarlyEntries[kEarlyCount++];
	e->ticks = ticks;
	__asm__ volatile("rdtsc" : "=a"(*(uint32_t*)&e->tsc), "=d"(*((uint32_t*)&e->tsc + 1)));
	e->core_id   = core;
	e->log_level = priority;
	e->category  = category;
	e->continued = false;
	e->threadID  = 0;   // pre-scheduler: there is no thread to name yet
	snprintf(e->message, MAX_LOG_MESSAGE_SIZE, "%s", message);
	e->message[MAX_LOG_MESSAGE_SIZE - 1] = '\0';
}

// Pour the early ring into the real one. Called by logging_queueing_init AFTER
// the buffers exist and BEFORE kLoggingInitialized goes true, so no concurrent
// writer can be walking these entries — and in any case this is still the BSP
// alone.
static void log_drain_early_into_rings(void)
{
	log_buffer_t *b = &core_log_buffers[0];
	for (uint32_t i = 0; i < kEarlyCount; i++) {
		b->entries[b->head] = kEarlyEntries[i];        // struct copy
		b->head = (b->head + 1) % b->capacity;
	}
	if (kEarlyLost > 0) {
		// Say it IN THE LOG, at the point of the hole, so a reader of the file
		// sees the gap rather than inferring it from a boot that starts oddly.
		log_entry_t *e = &b->entries[b->head];
		memset(e, 0, sizeof(*e));
		e->ticks = kEarlyEntries[EARLY_LOG_ENTRIES - 1].ticks;
		snprintf(e->message, MAX_LOG_MESSAGE_SIZE,
		         "*** %u early boot line(s) reached serial but not this file "
		         "(pre-allocator buffer full) ***\n", kEarlyLost);
		b->head = (b->head + 1) % b->capacity;
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
	// The boot's first lines, which have been waiting in BSS since before
	// there was an allocator, take their place at the head of core 0's ring —
	// in order, with their original ticks and TSC, so the k-way merge that
	// feeds logd puts them exactly where they belong: first.
	log_drain_early_into_rings();
	kLoggingInitialized = true;

	// The legend the retired attach banner used to carry. Ticks are the unit
	// every log line is stamped in, and "%k" means nothing without the rate —
	// so state it once, in the log, on DEBUG_BOOT (which survives even the
	// Ctrl+~ suppression, precisely so a suppressed machine still explains
	// itself).
	printd(DEBUG_BOOT, "log: %d ring(s) x %d MB, %d ticks/sec\n",
	       (int)kLimineSMPInfo->cpu_count, LOG_BUFFER_SIZE_MB, TICKS_PER_SECOND);
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

    // A continuation chunk gets NO prefix — it is the tail of a line that was
    // already introduced, and re-stamping it would break the line in half.
    if (!entry->continued) {
        os64_logline_t line = {
            .ticks         = entry->ticks,
            .threadID      = entry->threadID,
            .core          = entry->core_id,
            .level         = entry->log_level,
            .category      = entry->category,
            .message       = entry->message,
            .category_name = os64_logcat_name(entry->category),
        };
        os64_logtime_t now = {0};
        if (kLogFormatWantsClock)
            log_wallclock_now(&now);
        os64_logfmt_render(print_buf2, sizeof(print_buf2), kLogFormat, &line, &now);
    } else {
        snprintf(print_buf2, sizeof(print_buf2), "%s", entry->message);
    }

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
    // A RE-attach — a daemon claiming the log after a previous one died — is
    // worth a line; a FIRST attach is not (the boot banner already separates
    // boots, and logd's own "===== attached =====" was retired for saying the
    // same thing twice). Only the kernel can tell these apart: a restarted
    // logd is a brand-new process whose "have I attached yet?" is always no.
    //
    // The notice is stored DIRECTLY rather than through printd, on purpose —
    // it must not be filterable by kDebugLevel. It is the counterpart of
    // "userland log sink went quiet" and the pair reads as one story.
    if (!kLogSinkClaimed && kLogSinkEverClaimed)
        log_store_entry(get_core_local_storage()->apic_id, kTicksSinceStart, 0,
                        OS64_LOGCAT_LOGGING, false,
                        "[logd] userland log sink re-attached — kernel off the wire again\n");

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
            signal_raise(SIGSLEEP, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
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
            signal_raise(SIGSLEEP, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
            continue;
        }
        if (was_claimed)
        {
            was_claimed = false;
            kLogSinkClaimed = false;
            serial_print_string("[logd] userland log sink went quiet — kernel resuming serial drain\n");
        }

        // NO SERIAL PORT = NO DRAINING. Draining means CONSUMING: entries are
        // removed from the rings on the way out. Aim that at a UART which does
        // not exist and the kernel is not logging, it is shredding — as fast
        // as it can, exactly when the log daemon has just died and the log is
        // the only evidence of why. That is the P5's situation permanently:
        // no serial port at all (found 2026-08-18, when the probe that was
        // supposed to detect this turned out never to have worked).
        //
        // So we retain instead. The rings hold the recent past, circular, and
        // a logd restarted by hand recovers everything still in them —
        // "at least if I can manage to restart the ring 3 logd, we haven't
        // lost everything" (Chris's ruling, and the reason the ring-full path
        // above overwrites rather than panics: without that, retention here
        // would fill the rings and kill the machine).
        if (!kSerialPresent)
        {
            static bool saidSo = false;
            if (!saidSo)
            {
                saidSo = true;
                // printf, not printd: this must reach the GLASS, which on a
                // serial-less machine is the only place anything can be said.
                printf("[logd] no serial port on this machine — kernel log RETAINED in memory, "
                       "not drained (restart a log daemon to collect it)\n");
            }
            if (!daemon)
                return false;
            signal_raise(SIGSLEEP, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
            continue;
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
        signal_raise(SIGSLEEP, kTicksSinceStart + LOGD_SLEEP_TICKS, self);
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

