#include "serial_logging.h"
#include <stdbool.h>
#include "CONFIG.h"
#include "sprintf.h"
#include "memcpy.h"
#include "memset.h"
#include "io.h"
#include "printd.h"
#include "BasicRenderer.h"
#include "smp.h"
#include "smp_core.h"
#include "panic.h"
#include "x86_64.h"
#include "log.h"
#include "strlen.h"
#include "os64/klog_format.h"   // the ONE renderer, shared with logd

#define MAX_FIRST_MESSAGE_SIZE MAX_LOG_MESSAGE_SIZE - 10 // Leave space for prefix and null terminator

extern volatile uint64_t kUptime;
extern volatile uint64_t kTicksSinceStart;
extern volatile bool kFBInitDone;
extern bool kOverrideFileLogging;
extern bool kDirectLog;   // DIRECTLOG: bypass the queues, write COM1 polled (kernel_commandline.c)
extern bool kEnableSMP;
extern volatile bool kSchedulerInitialized;

// printd's two direct-to-serial paths, rendered in one place. Serial takes its
// format from the kernel cmdline (LOGFMT=) because that is the only config
// channel that exists before a filesystem does — logd reads /etc/logd.conf for
// the file's format, and the two are deliberately allowed to differ.
//
// The wall clock is computed only when the active format actually asks for it
// (kLogFormatWantsClock). This function runs in interrupt context on every
// core, so paying for an epoch→calendar breakdown that `classic` never prints
// would be a tax on the common path for a field nobody rendered.
static void printd_emit_serial(uint64_t tick_count, uint64_t threadID,
                               uint16_t core, uint8_t priority, uint8_t category,
                               const char *message)
{
	os64_logline_t line = {
		.ticks         = tick_count,
		.threadID      = threadID,
		.core          = core,
		.level         = priority,
		.category      = category,
		.message       = message,
		.category_name = os64_logcat_name(category),
	};
	os64_logtime_t now = {0};
	if (kLogFormatWantsClock)
		log_wallclock_now(&now);
	char out[2048];
	os64_logfmt_render(out, sizeof(out), kLogFormat, &line, &now);
	serial_print_string(out);
}

void printd(__uint128_t debug_level, const char *fmt, ...) {
    // Formatting scratch buffer. MUST be a local (per-call, per-stack) and never
    // a shared global: printd runs concurrently on every core, and a single
    // shared buffer gets clobbered mid-format when two cores log at once — which
    // corrupts the message BEFORE it is copied into the per-core log queue,
    // producing garbled log entries that logd then faithfully prints. A local
    // also makes printd re-entrancy-safe (e.g. a fault handler logging while an
    // outer printd is mid-format on the same core). The sibling print_buf2 below
    // is already a local for the same reason.
    char print_buf[2048];
    bool msg_continued = false;

    if ((kDebugLevel & debug_level) != debug_level) return;
    
    uint16_t core = 0;  // Default core if SMP isn't initialized
	uint64_t threadID = 0;

    core = read_apic_id(); // Get actual core ID if SMP is initialized
    if (kSMPInitDone && kCLSInitialized)
    {
        core_local_storage_t *cls = get_core_local_storage();
        if (cls->currentThread)
            threadID = cls->currentThread->threadID;
    }
    uint64_t tick_count = kTicksSinceStart;
    uint8_t priority = (debug_level >> 126) & 0x3;  // Extract top 2 bits for priority
    // Which category this line belongs to: the index of the LOWEST set
    // category bit. Two traps, both fixed 2026-08-01 (Chris green-lit it
    // while noting category isn't consumed yet — it's earmarked for
    // printing the flag's name on each log line, which only works if the
    // number is right):
    //
    //   * The old mask was a 64-bit literal against a __uint128_t, so
    //     every bit above 63 was silently truncated away. DEBUG_SPECIAL
    //     (bit 125) therefore masked to ZERO...
    //   * ...and __builtin_ctz(0) is UNDEFINED BEHAVIOR. It happened to
    //     return something plausible; that is the worst kind of working.
    //
    // Now: strip only the two PRIORITY bits (126/127), search the full
    // 128, and guard the all-zero case explicitly.
    __uint128_t category_bits = debug_level & ~(((__uint128_t)3) << 126);
    uint8_t category = 0;
    if (category_bits != 0)
    {
        uint64_t low = (uint64_t)category_bits;
        category = low ? (uint8_t)__builtin_ctzll(low)
                       : (uint8_t)(64 + __builtin_ctzll((uint64_t)(category_bits >> 64)));
    }

    va_list args;
    va_start(args, fmt);
    vsprintf(print_buf, fmt, args);
    va_end(args);

#if ENABLE_LOG_BUFFERING == 1
    // kDirectLog (DIRECTLOG on the cmdline) forces the `else` branch below for
    // the whole boot — see kernel_commandline.c for why that branch is worth
    // having as a MODE and not just as a bootstrap leftover: logd is a task, so
    // the queue cannot drain until the scheduler runs, and a boot that dies
    // before then takes its own explanation with it.
    if (kLoggingInitialized && !kDirectLog)
    {
		size_t msg_len = strlen(print_buf);
		size_t offset = 0;
		while (offset < msg_len) {
            char chunk[MAX_LOG_MESSAGE_SIZE];
            size_t chunk_capacity = (offset == 0) ? MAX_FIRST_MESSAGE_SIZE : MAX_LOG_MESSAGE_SIZE;
            size_t max_payload = chunk_capacity - 1; // leave room for null terminator
            size_t remaining = msg_len - offset;
            size_t copy_len = remaining < max_payload ? remaining : max_payload;

            if (copy_len == 0)
                break; // Safety: should not happen, but avoid infinite loop

            memcpy(chunk, print_buf + offset, copy_len);
            chunk[copy_len] = '\0';

            msg_continued = (offset != 0);
            log_store_entry(core, tick_count, priority, category, msg_continued, chunk);

            offset += copy_len;
        }
    }
	else
	{
		// Before the rings exist, park a copy in the pre-allocator BSS log so
		// these lines can reach the FILE too — on the P5 there is no serial
		// port, so "it went to the wire" means it went nowhere. DIRECTLOG is
		// excluded on purpose: it exists to bypass every queue for a boot that
		// dies early, and a bypass that quietly buffered would not be one.
		// The !kLoggingInitialized half is belt-and-braces: reaching this arm
		// with the rings already up means DIRECTLOG, and storing then would
		// fill a buffer whose one drainer has already run.
		if (!kLoggingInitialized && !kDirectLog)
			log_store_early(core, tick_count, priority, category, print_buf);

		// (The "Worst case of duplicate code EVER" TODO that lived here is
		// PAID, 2026-08-18: both arms and the two drainers now render through
		// the one formatter in <os64/klog_format.h>. There were FOUR copies of
		// this layout — here, the #else below, log.c's drainer, and logd's
		// file writer — which is a drift waiting for its first edit.)
		printd_emit_serial(tick_count, threadID, core, priority, category, print_buf);
	}
#else
	printd_emit_serial(tick_count, threadID, core, priority, category, print_buf);
#endif
}
