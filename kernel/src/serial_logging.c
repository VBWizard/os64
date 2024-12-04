#include "serial_logging.h"
#include "CONFIG.h"
#include "sprintf.h"
#include "memset.h"
#include "io.h"
#include "CONFIG.h"
#include "BasicRenderer.h"

extern volatile uint64_t kUptime;
extern volatile uint64_t kTicksSinceStart;
extern volatile bool kFBInitDone;
extern uint64_t kDebugLevel;

char print_buf[2048];
char print_buf2[2048];

void printd(uint64_t debug_level, const char *fmt, ...)
{
	int printed;

	if ((kDebugLevel & debug_level) == debug_level)
	{
		va_list args;
		va_start(args, fmt);
        int taskNum=0;
        __asm__("str eax\nshr eax,3\n":"=a" (taskNum));
		memset(print_buf,0,2048);
		printed = vsprintf(print_buf, fmt, args);
        printed = sprintf(print_buf2, "%u (0x%04x) AP%u: %s",kTicksSinceStart, taskNum, 0, print_buf);

#ifdef ENABLE_COM1
		for (int cnt=0;cnt<printed;cnt++)
		{
			write_serial(COM1,print_buf2[cnt]);
		}
#else
		if (kFBInitDone)
		printf("%s",print_buf2);
#endif
		va_end(args);
	}
}
