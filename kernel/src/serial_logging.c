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

extern volatile uint64_t kUptime;
extern volatile uint64_t kTicksSinceStart;
extern volatile bool kFBInitDone;
extern bool kOverrideFileLogging;
extern bool kEnableSMP;
char print_buf[2048];
char print_buf2[2048];

void printd(__uint128_t debug_level, const char *fmt, ...)
{
	int printed;
	
	if ((kDebugLevel & debug_level) == debug_level)
	{
		uint64_t threadID = 0, apic_id = 0;
		va_list args;
		va_start(args, fmt);
		if (kEnableSMP && kCLSInitialized)
		{
			core_local_storage_t *cls = get_core_local_storage();
			threadID = cls->threadID;
			apic_id = cls->apic_id;
		}
		memset(print_buf,0,2048);
		printed = vsprintf(print_buf, fmt, args);
        printed = sprintf(print_buf2, "%u (0x%04x) AP%u: %s",kTicksSinceStart, threadID, apic_id, print_buf);

#ifdef ENABLE_COM1
	if (!kOverrideFileLogging)
		for (int cnt=0;cnt<printed;cnt++)
		{
			write_serial(COM1,print_buf2[cnt]);
		}
	else
		if (kFBInitDone)
			printf("%s",print_buf2);
#else
		if (kFBInitDone)
			printf("%s",print_buf2);
#endif
		va_end(args);
	}
}
