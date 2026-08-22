#ifndef SHUTDOWN_H
#define SHUTDOWN_H

// kernel_park: the kernel task's forever-home after boot (heartbeat widget,
// pool-slope reports). Named shutdown() from os32 days — a heritage misnomer
// this file wore until 2026-08-08, when an actual shutdown arrived and
// needed the name.
void kernel_park(void);

#include "os64/syscall_numbers.h"   // os64_shutdown_mode_t — the verb, defined ONCE

// How the descent ends. ONE descent, two doors — everything above the last
// step is identical, because "stop the machine safely" is the same job either
// way and only the final instruction differs. (A separate reboot_system()
// would have been a second copy of the sync-and-flush ladder, i.e. a second
// place to forget a step.)
//
// The TYPE lives in the ABI, not here: it is a syscall argument, so the
// kernel, the library wrapper and the utility must all mean the same thing by
// it. A kernel-side copy of the same two values would be a second definition
// to keep in step, which is the bug the log.c/klog_format.h static asserts
// exist to catch elsewhere — better to have nothing to catch.
//   OS64_SHUTDOWN_POWEROFF — hypervisor ports, else the 1995 liturgy + park
//   OS64_SHUTDOWN_REBOOT   — 0xCF9 → 8042 pulse → triple fault

// shutdown_system: the real thing — the ordered descent behind
// SYSCALL_SHUTDOWN (see syscall_numbers.h for the contract and the lineage).
// Asks every task to stop (SIGTERM, grace, SIGKILL), retires logd, sync_all's
// every open file, FLUSH CACHEs the drives, then powers off or reboots. Runs
// in the calling task's context; never returns.
void shutdown_system(os64_shutdown_mode_t mode) __attribute__((noreturn));

// TRUE from the first line of the descent onward. One reader today, and it
// exists because of a bug worth remembering (found on the P5, 2026-08-21):
// the descent's SIGTERM sweep kills the shell that launched `shutdown`, the
// dying shell hangs up its terminal, and the hangup sweep then SIGHUPs
// everything still seated there — INCLUDING THE SHUTDOWN TASK ITSELF, which
// is halfway through running the descent. The machine stayed up with its
// undertaker shot, and how far the descent got before dying was a race (which
// is exactly what it looked like: a different number of lines each run).
//
// During a descent, a shell exiting is a CONSEQUENCE of the shutdown, not a
// terminal hangup — everyone has already been asked to stop, in order, by the
// ladder. So the hangup sweep stands down.
extern volatile bool kShuttingDown;

#endif
