#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define ENABLE_COM1

#define DISK_WRITING_ENABLED

//  Timing related configuration
#define TICKS_PER_SECOND 100
// Milliseconds per tick
#define MS_PER_TICK (uint64_t)(1000 / TICKS_PER_SECOND)
// Microseconds per tick
#define MIS_PER_TICK 1000000 / TICKS_PER_SECOND
// Memory related configuration

#define PAGE_SIZE 0x1000
#define KERNEL_PAGED_BASE_ADDRESS 0xFFFFFFFF80000000
#define INITIAL_MEMORY_STATUS_COUNT 100000
#define KERNEL_STACK_SIZE 20 * PAGE_SIZE

//Signal related
#define SIGNAL_PROCESS_TICK_FREQUENCY 1 //20 MS if TICKS_PER_SECOND = 100 (1 tick every 10 MS)

//Logging related
#define ENABLE_LOG_BUFFERING 1  // Set to 0 to disable buffering
// Wait for transmit-empty before each serial byte. 2026-07-11 finding: on
// VBox this LSR poll was the logd drain throttle (~11.5KB/s, VM-exit priced
// per byte) — with it OFF, drains run at port speed and DETAILED stays
// drained. Keep OFF for file/disconnected serial sinks. CAVEAT: with a REAL
// serial listener attached, unpaced writes can overrun the UART FIFO and
// drop bytes ON THE WIRE — turn this back on wherever a physical consumer
// sits (never-drop applies to the wire too).
#define SERIAL_WAIT_FOR_TRANSMIT 0

//Scheduler Related
// One scheduler pass per timer tick (10ms). This is also the SIGSLEEP wake
// granularity — every sleeping thread in the system wakes on this grid, so
// it caps animation frame rates (the GUI's bounce demo paces itself with
// 1-tick sleeps). Ran at 10/sec for years because nothing needed better;
// raised to match TICKS_PER_SECOND when the GUI gave sleeping threads a
// reason to wake quickly.
#define MP_SCHEDULER_RUNS_PER_SECOND 100
#define SCHEDULER_DEBUG 1

// Framebuffer related
#define FRAMEBUFFER_FONT "zap-ext-light16.psf"

// Linux defines so we have access to some header values that aren't accessible otherwise
#define __KERNEL__
#define __USE_MISC

// Debugging related configuration
#define SHUTOFF_ON_PANIC 0
//remark to disable
//#define DEBUG_FOCUS_APIC_ID 1

#define DEBUG_EVERYTHING ((__uint128_t)0xFFFFFFFFFFFFFFFFULL | ((__uint128_t)0xFFFFFFFFFFFFFFFFULL << 64))
#define DEBUG_NOTHING 0x0000000000000000
#define DEBUG_EXCEPTIONS 1 << 0
#define DEBUG_BOOT 1 << 1
#define DEBUG_SMP 1 << 2
#define DEBUG_PCI_DISCOVERY 1 << 3
#define DEBUG_PCI 1 << 4
#define DEBUG_HARDDRIVE 1 << 5
#define DEBUG_AHCI 1 << 6
#define DEBUG_MEMMAP (__uint128_t)1 << 7
#define DEBUG_ACPI (__uint128_t)1 << 8
#define DEBUG_PAGING (__uint128_t)1 << 9
#define DEBUG_ALLOCATOR (__uint128_t)1 << 10
#define DEBUG_KMALLOC (__uint128_t)1 << 10
// The demand pager's own channel — same doctrine as DEBUG_TASKSWITCH below:
// a RESOLVED page fault is not an exception, it's the pager doing its job on
// purpose, and logging routine success on the always-on EXCEPTIONS bit buries
// the faults that are actually news. Announce + resolution ride the base
// level here (opt-in from the cmdline, like DEBUG_PIPE); the per-fault VMA
// detail adds DEBUG_DETAILED. UNresolved faults — no VMA, protection
// violation, the lazy-HHDM tripwire — stay on DEBUG_EXCEPTIONS, because
// those ARE news (and are about to be a panic).
#define DEBUG_DEMAND_PAGING (__uint128_t)1 << 11
#define DEBUG_NVME (__uint128_t)1 << 12
#define DEBUG_VFS (__uint128_t)1 << 13
#define DEBUG_THREAD (__uint128_t)1 << 14
#define DEBUG_TASK (__uint128_t)1 << 15
#define DEBUG_SCHEDULER (__uint128_t)1 << 16
#define DEBUG_SIGNALS (__uint128_t)1 << 17
#define DEBUG_LOGGING (__uint128_t)1 << 18
#define DEBUG_TESTS (__uint128_t)1 << 19
#define DEBUG_SYSCALL (__uint128_t)1 << 20
#define DEBUG_GUI (__uint128_t)1 << 21
#define DEBUG_APPLICATION (__uint128_t)1 << 22
// One line, one bit: "which program is the CPU running right now?" — the
// task-switch trace. It rides in MINIMAL (i.e. always on) because when you're
// bringing up spawn/wait you always want to see husk -> hello -> husk, and you
// never want the whole DEBUG_SCHEDULER firehose to get it. It previously
// borrowed DEBUG_EXCEPTIONS purely because that bit happens to be always-on —
// which made the level lie about what the message is. The mask is 128 bits
// wide and 23 are spoken for; a bit is the cheapest thing in this kernel, and
// a level that means what it says is worth far more than the bit it costs.
#define DEBUG_TASKSWITCH (__uint128_t)1 << 23
// Pipes get their own channel, and NOT because of volume (though a busy shell
// pipes a lot, and these used to ride DEBUG_TASK — which lives in MINIMAL, so
// every boot printed pipe chatter forever).
//
// The real reason: a pipeline's characteristic failure is a HANG, and the only
// question that ever solves it is "who is parked waiting for what, and what are
// the refcounts?" So DEBUG_PIPE traces exactly that — lifecycle, every refcount
// change, every park and wake, and the two events that end a stream (EOF when
// the last writer leaves, EPIPE when the last reader does). Those are RARE
// events with high signal. The byte-by-byte data flow is a firehose and rides
// DEBUG_PIPE | DEBUG_DETAILED instead, so the base level stays readable.
//
// Deliberately NOT in MINIMAL: opt in from the cmdline when you're chasing a
// pipeline, stay silent otherwise.
#define DEBUG_PIPE (__uint128_t)1 << 24
// Was 1 << 13, silently aliasing DEBUG_VFS — turning on VFS logging dragged
// shutdown chatter along and vice versa. Own bit now. (DEBUG_KMALLOC still
// aliases DEBUG_ALLOCATOR above; those two at least are the same subsystem.)
#define DEBUG_SHUTDOWN (__uint128_t)1 << 25
#define DEBUG_USB (__uint128_t)1 << 26
#define DEBUG_SPECIAL (__uint128_t)1 << 125
#define DEBUG_DETAILED (__uint128_t)1 << 126
#define DEBUG_EXTRA_DETAILED (__uint128_t)1 << 127
#define DEBUG_MINIMAL_OPTIONS (__uint128_t)(DEBUG_EXCEPTIONS | DEBUG_BOOT | DEBUG_TESTS | DEBUG_APPLICATION | DEBUG_TASK | DEBUG_TASKSWITCH | DEBUG_DETAILED)
// | DEBUG_SPECIAL
#define DEBUG_OPTIONS (__uint128_t)(DEBUG_MINIMAL_OPTIONS)
//#define DEBUG_OPTIONS DEBUG_MINIMAL_OPTIONS
extern __uint128_t kDebugLevel;

#endif // CONFIG_H
