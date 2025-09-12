#include "serial_logging.h"
#include <stdbool.h>
#include "CONFIG.h"
#include "sprintf.h"
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
#include "strcpy.h"

#define MAX_FIRST_MESSAGE_SIZE MAX_LOG_MESSAGE_SIZE - 10 // Leave space for prefix and null terminator

extern volatile uint64_t kUptime;
extern volatile uint64_t kTicksSinceStart;
extern volatile bool kFBInitDone;
extern bool kOverrideFileLogging;
extern bool kEnableSMP;
extern volatile bool kSchedulerInitialized;
char print_buf[2048];

void printd(__uint128_t debug_level, const char *fmt, ...) {
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
    uint8_t category = __builtin_ctz(debug_level & 0x3FFFFFFFFFFFFFFF); // First category set
    
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
            chunk[MAX_FIRST_MESSAGE_SIZE - 1] = '\0'; // Make sure the last character of the string is a null terminator

            //If the message will be split, the first message needs to be 10 characters less than the length of the string
            //  Additional messages can be the entire MAX_LOG_MESSAGE_SIZE
            if (offset == 0) {
                msg_continued = false;
                strncpy(chunk, print_buf, MAX_FIRST_MESSAGE_SIZE);
            } else {
                msg_continued = true;
                strncpy(chunk, print_buf + offset, MAX_LOG_MESSAGE_SIZE);
            }
            offset += strlen(chunk); // Only increment by the actual length of printed data
            log_store_entry(core, tick_count, priority, category, msg_continued, chunk);
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
