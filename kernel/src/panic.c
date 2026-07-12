#include <stdint.h>
#include "panic.h"
#include "BasicRenderer.h"
#include "stddef.h"
#include <stdarg.h>
#include "sprintf.h"
#include "shutdown.h"
#include "BasicRenderer.h"
#include "paging.h"
#include "serial_logging.h"
#include "log.h"
#include "gui/compositor.h"

char sprintf_buf[2000];

void panic_no_shutdown(const char *format, ...)
{
    // FIRST: detach the GUI console sink so everything below renders raw on
    // the framebuffer — the GUI (or the lock some thread died holding) can
    // never stand between a panic and the screen.
    gui_emergency_disable();

    // Same reasoning, one layer down: if we faulted while holding the renderer
    // lock (or another core died holding it), every printf below would spin
    // forever with interrupts off and the panic would never reach the screen.
    // A panic nobody can read is worse than a garbled one — bust the lock.
    renderer_bust_lock();

    va_list args;
    va_start( args, format );
    printf("\n>>>panic at instruction prior to address 0x%08x<<<\n", __builtin_return_address(0));
    printf("  >>>");
    vsprintf(sprintf_buf, format, args);
	va_end(args);
	print(sprintf_buf);
    printd(DEBUG_EXCEPTIONS, sprintf_buf);
    logd_thread(false);
panicLoop:
    __asm__("cli\nhlt\n");
    goto panicLoop;
}


void __attribute__((noreturn, noinline))panic(const char *format, ...)
{
    // FIRST: detach the GUI console sink, then bust the renderer lock
    // (see panic_no_shutdown for both — nothing stands between a panic and
    // the framebuffer).
    gui_emergency_disable();
    renderer_bust_lock();

    va_list args;
    va_start( args, format );
    printf("\n>>>panic at instruction prior to address 0x%016lx<<<\n", __builtin_return_address(0));
    printf("  >>>");
    vsprintf(sprintf_buf, format, args);
	va_end(args);
	print(sprintf_buf);
    printd(DEBUG_EXCEPTIONS, sprintf_buf);
    logd_thread(false);
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