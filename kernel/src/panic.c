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
#include "tty.h"   // tty_emergency_direct — the terminals' escape hatch
#include "exception_report.h"   // exception_wire_lock — one narrator per report

// SHARED between every panicking core — which is why both panic entry points
// take the wire lock BEFORE formatting into it: without that, two concurrent
// panics don't just braid on the wire, they overwrite each other's MESSAGE.
char sprintf_buf[2000];

// Shared body of panic/panic_no_shutdown: get the message onto EVERY sink in
// dying-breath order. The screen half was always right (bust the locks, then
// draw); the serial half was quietly broken for as long as logd has owned the
// wire: printf/print are FRAMEBUFFER-ONLY (their serial side left when logd
// arrived), so a panic's only serial copy sat in the printd queue — the
// NEWEST entry, i.e. the one a single bounded logd_thread(false) pass drains
// LAST or not at all (and drains NEVER if another core held the work lock, or
// if logd lived on this core when it cli/hlt'd). Result: panic on screen,
// nothing in the log — discovered 2026-07-18 when a root-fs panic left
// qemu_com1.log ending mid-boot, looking for all the world like a clean run.
//
// Order matters:
//   1. Banner + message DIRECT to serial (serial_print_string is lock-free
//      polled COM1 — cannot be lost, cannot wedge on a dead lock holder).
//      If everything after this hangs, the cause is already on the wire.
//   2. Emergency-flush the queued backlog — the printd trail that led here,
//      which is exactly what post-mortem debugging wants.
//   3. Repeat the message as the log's final line, so "tail the log" shows
//      the cause without scrolling back through the flushed backlog.
static void panic_broadcast(uintptr_t caller, const char *msg)
{
    char banner[96];

    sprintf(banner, "\n>>>panic at instruction prior to address 0x%016lx<<<\n  >>>", caller);

    // Screen first — it's already been made safe (sink detached, lock busted)
    // and it's the sink a human is most likely watching in the moment.
    print(banner);
    print(msg);

    // Serial, directly — steps 1 through 3 above.
    serial_print_string(banner);
    serial_print_string(msg);
    logd_emergency_flush();
    serial_print_string("\n>>>panic (repeated post-flush): ");
    serial_print_string(msg);
}

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

    // And one layer above: force every print onto the legacy direct-to-glass
    // path and bust the terminal locks too (a dead core may hold a tty grid
    // lock the same way it may hold the renderer's). Panic text lands on
    // whatever terminal is showing — exactly what you want from a dead system.
    tty_emergency_direct();

    // The wire lock guards BOTH the shared sprintf_buf and the output order.
    // Bounded + reentrant (exception_report.c), so a panic from inside a
    // locked report self-nests, and a dead holder gets barged, never waited
    // on forever. Abandoned (not merely unlocked) after the last line: this
    // core halts forever, and a dead core holds no locks.
    exception_wire_lock();

    va_list args;
    va_start( args, format );
    vsprintf(sprintf_buf, format, args);
    va_end(args);
    panic_broadcast((uintptr_t)__builtin_return_address(0), sprintf_buf);
    exception_wire_abandon();
panicLoop:
    __asm__("cli\nhlt\n");
    goto panicLoop;
}


void __attribute__((noreturn, noinline))panic(const char *format, ...)
{
    // FIRST: detach the GUI console sink, then bust the renderer lock, then
    // force the terminals into direct mode (see panic_no_shutdown for all
    // three — nothing stands between a panic and the framebuffer).
    gui_emergency_disable();
    renderer_bust_lock();
    tty_emergency_direct();

    // Same wire-lock discipline as panic_no_shutdown above: taken before the
    // shared buffer, abandoned after the last line.
    exception_wire_lock();

    va_list args;
    va_start( args, format );
    vsprintf(sprintf_buf, format, args);
    va_end(args);
    panic_broadcast((uintptr_t)__builtin_return_address(0), sprintf_buf);
    exception_wire_abandon();
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