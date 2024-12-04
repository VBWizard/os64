#include "panic.h"
#include "BasicRenderer.h"
#include "stddef.h"
#include <stdarg.h>
#include "sprintf.h"

char sprintf_buf[2000];
void __attribute__((noinline))panic(const char *format, ...)
{
    va_list args;
    va_start( args, format );
    printf("\n>>>panic at instruction prior to address 0x%08x<<<\n", __builtin_return_address(0));
    printf("  >>>");
    vsprintf(sprintf_buf, format, args);
	va_end(args);
	print(sprintf_buf);
    //printDumpedRegs();
    panicLoop: 
    __asm__("cli\nhlt\n");
    goto panicLoop;
}
