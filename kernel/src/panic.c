#include <stdint.h>
#include "panic.h"
#include "BasicRenderer.h"
#include "stddef.h"
#include <stdarg.h>
#include "sprintf.h"
#include "shutdown.h"
#include "BasicRenderer.h"
#include "paging.h"

char sprintf_buf[2000];

void panic_no_shutdown(const char *format, ...)
{
    va_list args;
    va_start( args, format );
    printf("\n>>>panic at instruction prior to address 0x%08x<<<\n", __builtin_return_address(0));
    printf("  >>>");
    vsprintf(sprintf_buf, format, args);
	va_end(args);
	print(sprintf_buf);
    panicLoop: 
    __asm__("cli\nhlt\n");
    goto panicLoop;
}


void __attribute__((noreturn, noinline))panic(const char *format, ...)
{
    va_list args;
    va_start( args, format );
    printf("\n>>>panic at instruction prior to address 0x%08x<<<\n", __builtin_return_address(0));
    printf("  >>>");
    vsprintf(sprintf_buf, format, args);
	va_end(args);
	print(sprintf_buf);
#if SHUTOFF_ON_PANIC == 1	
	shutdown();
#endif 
    panicLoop: 
    __asm__("cli\nhlt\n");
    goto panicLoop;
}

void debug_print_mem(uint64_t address, uint64_t byteCount)
{
	uint64_t startAddr = ((uint64_t)address & 0xFFFFF000)-32;
	for (uint64_t cnt = startAddr; cnt < startAddr + byteCount+32; cnt += 32)
	{
		printf("%08lx:\t", cnt);  // Print the starting address of the row

		for (int i = 0; i < 32; i++)  // 32 values, each 2 bytes
		{
			uint8_t value = *(uint8_t *)PHYS_TO_VIRT(cnt + i);
			printf("%02x ", value);  // Print 2-byte value (4 hex digits)
			if (i%8 == 7)
				printf(" ");
		}

		printf("\n");  // New line for the next 32 values
	}

}