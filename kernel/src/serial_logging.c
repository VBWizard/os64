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

#define MAX_FIRST_MESSAGE_SIZE MAX_LOG_MESSAGE_SIZE - 10 // Leave space for prefix and null terminator

extern volatile uint64_t kUptime;
extern volatile uint64_t kTicksSinceStart;
extern volatile bool kFBInitDone;
extern bool kOverrideFileLogging;
extern bool kEnableSMP;
extern volatile bool kSchedulerInitialized;

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
    if (kLoggingInitialized)
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
	//TODO: FIX ME!  Worst case of duplicate code EVER
	//  Temporary justification is that the code it duplicates is hidden inside an #else which means it's disabled
	//  if this code is enabled. :-(
	{
    	char print_buf2[2048];
        sprintf(print_buf2, "%lu (0x%04lx) AP%u: %s", tick_count, threadID, core, print_buf);
    	serial_print_string(print_buf2);
	}
#else
    char print_buf2[2048];
    sprintf(print_buf2, "%lu (0x%04lx) AP%u: %s", tick_count, threadID, core, print_buf);
    serial_print_string(print_buf2);
#endif
}
