#include "serial_logging.h"
#include "CONFIG.h"
#include "sprintf.h"
#include "memset.h"
#include "io.h"

void printd(uint64_t debug_level, const char *fmt, ...)
{
	char print_buf[2048];
	int printed;

	if ((DEBUG_OPTIONS & debug_level) == debug_level)
	{
		va_list args;
		va_start(args, fmt);
		memset(print_buf,0,2048);
		printed = vsprintf(print_buf, fmt, args);

		for (int cnt=0;cnt<printed;cnt++)
		{
			write_serial(COM1,print_buf[cnt]);
		}
		va_end(args);
	}
}
