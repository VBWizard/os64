#include "log.h"
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

extern volatile uint64_t kUptime;
extern volatile uint64_t kTicksSinceStart;
extern volatile bool kFBInitDone;
extern bool kOverrideFileLogging;
extern bool kEnableSMP;
extern volatile bool kSchedulerInitialized;
char print_buf[2048];

void printd(__uint128_t debug_level, const char *fmt, ...) {
    if ((kDebugLevel & debug_level) != debug_level) return;
    
    uint16_t core = 0;  // Default core if SMP isn't initialized
	uint64_t threadID = 0;

    if (kSMPInitDone && kCLSInitialized) {
        core = read_apic_id();  // Get actual core ID if SMP is initialized
		threadID = get_core_local_storage()->currentThread->threadID;
    }
	uint64_t tick_count = kTicksSinceStart;
    uint8_t priority = (debug_level >> 126) & 0x3;  // Extract top 2 bits for priority
    uint8_t category = __builtin_ctz(debug_level & 0x3FFFFFFFFFFFFFFF); // First category set
    
    va_list args;
    va_start(args, fmt);
    int printed = vsprintf(print_buf, fmt, args);
    va_end(args);
    

#if ENABLE_LOG_BUFFERING == 1
    if (kLoggingInitialized)
    {
		size_t msg_len = strlen(print_buf);
		size_t offset = 0;
		while (offset < msg_len) {
			char chunk[MAX_LOG_MESSAGE_SIZE - 4];  // Leave space for prefix and null terminator
			size_t chunk_size = MAX_LOG_MESSAGE_SIZE - 5; // Ensuring room for \0
			
			strncpy(chunk, print_buf + offset, chunk_size);
			chunk[chunk_size] = '\0';
			
			if (offset == 0) {
				log_store_entry(core, tick_count, priority, category, false, chunk);
				offset += strlen(chunk); // Only increment by the actual length of printed data
				//if (kFBInitDone) printf("%s\n",chunk);
			} else {
				char cont_msg[MAX_LOG_MESSAGE_SIZE];
				snprintf(cont_msg, sizeof(cont_msg), "%s", chunk);
				log_store_entry(core, tick_count, priority, category, true, cont_msg);
				offset += MAX_LOG_MESSAGE_SIZE; // Only increment by the actual length of printed data
				//if (kFBInitDone) printf("%s\n",cont_msg);
			}
		}
	}
	else
	//TODO: FIX ME!  Worst case of duplicate code EVER
	//  Temporary justification is that the code it duplicates is hidden inside an #else which means it's disabled
	//  if this code is enabled. :-(
	{
    	char print_buf2[2048];
    	snprintf(print_buf2, sizeof(print_buf2), "%u (0x%04x) AP%u: %s", tick_count, threadID, core, print_buf);
    	serial_print_string(print_buf2);
	}	
#else
    char print_buf2[2048];
    snprintf(print_buf2, sizeof(print_buf2), "%u (0x%04x) AP%u: %s", tick_count, threadID, core, print_buf);
    serial_print_string(print_buf2);
#endif
}
