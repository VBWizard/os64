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
// Poison every freed extent with 0xFE through its HHDM alias (allocator.c,
// free_memory). A use-after-free then reads as unmistakable 0xFE bytes —
// executing one faults fast and RECOGNIZABLY — instead of zeroed-then-
// recycled data masquerading as valid. Born as scribbled-text hunt
// instrumentation (2026-08-14); kept ON by ruling (Chris, 2026-08-15): the
// tripwire culture pays for its memsets. Set to 0 for a build that skips
// the per-free memset cost.
#define ALLOCATOR_POISON_ON_FREE 1

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

// ── Tickless preemption backstop (2026-08-13) ───────────────────────────────
// Under tickless scheduling an AP's LAPIC timer is normally silent — but a
// dispatched non-idle thread now carries a LEASE: a one-shot timer armed at
// dispatch for this many milliseconds. A thread that syscalls or blocks first
// re-enters the scheduler on its own and the lease is re-granted to whoever
// is dispatched next; a syscall-free hog gets preempted when the lease
// expires. Idle dispatch disarms the timer entirely, so an idle core stays
// truly tickless — the lease is a property of the DISPATCH, not of the clock
// (the periodic/deadline distinction; every mature scheduler converged here).
// 50ms = 20Hz worst-case interrupt load on a busy core, and the ceiling on
// Ctrl+C latency for a hog that never syscalls.
//
// This is the compiled DEFAULT; the live value is kSchedBackstopMS
// (kernel.c), overridable per boot with BACKSTOP=<ms> — the GUI entries
// pass 10, where a compositor sharing its core at 50ms granularity
// animates like a slideshow but at 10ms is smooth (his shakedown,
// 2026-08-13).
#define SCHED_BACKSTOP_MS 50

// The cache-home rule (scheduler_find_thread_to_run): a runnable thread whose
// last dispatch was on ANOTHER core is passed over — its caches are warm
// there, cold here — unless it has been waiting in qRunnable at least this
// many wall-clock ticks (at TICKS_PER_SECOND=100, 3 ticks = 30ms), at which
// point waiting longer costs more than migrating cold. Linux calls the same
// idea sched_migration_cost; ULE gets it structurally from per-CPU queues.
// Never-dispatched threads are exempt (cold everywhere = migration is free),
// as are idle threads (per-core by construction).
#define SCHED_MIGRATION_COST_TICKS 3

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
// DEBUG_EXCEPTIONS IS PERMANENTLY ON, and that is a ruling, not an accident
// (Chris, 2026-08-10: "I can't imagine running without it"). It is a member of
// DEBUG_MINIMAL_OPTIONS below, so EVERY configuration carries it — there is no
// supported build where a fault goes unlogged. Two consequences worth knowing
// before you touch it: other subsystems have deliberately borrowed this bit for
// must-never-be-silent messages precisely because it is always lit (see the
// note further down), and a fault report may therefore be written as a printd
// without hedging about whether anyone will see it. If you ever make this
// optional you are not saving a bit, you are making crashes invisible.
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
// DELIBERATE ALIAS, spelled as one so it can never read as an accident:
// kmalloc is built ON the allocator, so the two always want watching
// together and separating them would spend a bit to gain nothing. Written
// this way (rather than a second `1 << 10`) after a bit-collision scan
// flagged it as a duplicate — the VFS/shutdown pair below WAS a real
// accident, and the two cases should not look alike in the source.
#define DEBUG_KMALLOC DEBUG_ALLOCATOR
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
#define DEBUG_DIAG (__uint128_t)1 << 27
// The networking arc's bit (NETWORK.md). Covers the NIC drivers and, as the
// stack grows above them, the protocol layers — split into finer bits only
// when one subsystem's chatter starts drowning another's (the DEBUG_PIPE
// lesson: a bit earns independence when someone needs it alone).
// Bit 28, not 27: the userland branch minted DEBUG_DIAG at 27 in parallel
// with this arc, and the two files merge without a textual conflict — so
// the collision would have arrived silently and made "turn on net logging"
// also turn on the scheduler flight recorder. Exactly the accident the
// VFS/shutdown pair already taught this file once (2026-08-01).
// VINDICATED at the merge of 2026-08-05: 27 and 28 landed side by side with
// nothing to resolve, which is what choosing the free bit BOUGHT.
#define DEBUG_NET (__uint128_t)1 << 28
#define DEBUG_SPECIAL (__uint128_t)1 << 125
#define DEBUG_DETAILED (__uint128_t)1 << 126
#define DEBUG_EXTRA_DETAILED (__uint128_t)1 << 127
#define DEBUG_MINIMAL_OPTIONS (__uint128_t)(DEBUG_EXCEPTIONS | DEBUG_BOOT | DEBUG_TESTS)
// | DEBUG_SPECIAL
// The net branch's working default. DEBUG_SPECIAL is deliberately NOT here
// (Chris's ruling at the merge, 2026-08-05): it exists to watch his text
// utilities get scheduled in userland, and on THIS branch it printed 5,612
// of 5,829 serial lines — 96% — burying the very DEBUG_NET output this
// default exists to produce. That is exactly the "one subsystem's chatter
// drowning another's" the DEBUG_NET bit comment warned about, arriving on
// schedule. The bit still exists; a boot that wants it says so on the
// cmdline, which is what cmdline overrides are for.
#define DEBUG_OPTIONS (__uint128_t)(DEBUG_MINIMAL_OPTIONS | DEBUG_APPLICATION | DEBUG_TASK | DEBUG_DETAILED)
//#define DEBUG_OPTIONS DEBUG_MINIMAL_OPTIONS
extern __uint128_t kDebugLevel;

#endif // CONFIG_H
