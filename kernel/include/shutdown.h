#ifndef SHUTDOWN_H
#define SHUTDOWN_H

// kernel_park: the kernel task's forever-home after boot (heartbeat widget,
// pool-slope reports). Named shutdown() from os32 days — a heritage misnomer
// this file wore until 2026-08-08, when an actual shutdown arrived and
// needed the name.
void kernel_park(void);

// shutdown_system: the real thing — the ordered descent behind
// SYSCALL_SHUTDOWN (see syscall_numbers.h for the contract and the lineage).
// Retires logd, sync_all's every open file, FLUSH CACHEs the drives, then
// powers off (hypervisor ports) or prints the 1995 liturgy and parks (bare
// metal, until we speak enough ACPI for a real S5). Runs in the calling
// task's context; never returns.
void shutdown_system(void) __attribute__((noreturn));

#endif
