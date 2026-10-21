// cpu_roster.h — who each CPU is: the number a person and the kernel use for
// it, and the APIC ID the hardware addresses it by.
//
// PURE: stdint and stdbool only, so tools/test_cpu_roster_host.sh drives it
// under ASan with plain cc. The kernel half (smp.c, cpu_roster_init) owns the
// globals and hands this file Limine's list.
//
// A CPU NUMBER is dense, 0..count-1, and it is what the kernel means by "a
// CPU": every per-CPU array, the core-local storage slot, a thread's pin, the
// log stamp, /proc, /sys/cpu/<n>. An APIC ID is a hardware ADDRESS — where an
// IPI or an interrupt is aimed — and on many machines it is not dense: a
// Zen 2 CCX has four ID slots and a 3900X uses three of them, so its 24
// threads are APIC 0-5, 8-13, 16-21, 24-29. Using one number for both is what
// overran every MAX_CPUS array on that machine. CPU_NUMBERS.md is the design.
//
// THE NUMBERING: CPU 0 is the bootstrap processor, always; the rest follow
// in ascending APIC ID order. An APIC ID is a structured topology number
// (CPUID 0xB / 0x8000001E: thread bits lowest, then core, then die), so the
// sort puts SMT siblings next to each other on every firmware, and on a
// machine whose IDs are already 0..N-1 it makes every CPU number equal its
// APIC ID. Firmware order (the MADT's, which Limine reports) would do
// neither. A cap — nosmp, MAXCPUS=, MAX_CPUS — takes the first N of this
// order, so the BSP is always among them.

#ifndef CPU_ROSTER_H
#define CPU_ROSTER_H

#include <stdbool.h>
#include <stdint.h>

// A local APIC's hardware ID. A struct and not an integer so that the
// compiler refuses to index an array with one, compare one with a CPU number,
// or pass one where a CPU number belongs. The single array an APIC ID may
// index is the roster's own inverse table, which is what it exists to do.
typedef struct { uint32_t id; } apic_id_t;

// xAPIC destinations are eight bits and 0xFF is the broadcast address, so
// 0..254 are the IDs an IPI can name. A CPU above that is left off the roster.
#define CPU_ROSTER_APIC_IDS 256u
#define CPU_ROSTER_APIC_MAX 254u

// The inverse table's "no CPU has this APIC ID". It must never be a CPU
// number, so the kernel static-asserts MAX_CPUS below it.
#define CPU_ROSTER_NO_CPU 0xFFu

typedef struct
{
	uint32_t count;            // CPUs numbered, the BSP included
	uint32_t listed;           // entries the bootloader offered
	uint32_t too_wide;         // IDs above CPU_ROSTER_APIC_MAX, left off
	uint32_t duplicates;       // an ID listed again, left off after its first
	uint32_t parked;           // usable CPUs beyond the limit, never numbered
	bool     parked_by_max;    // ...and the limit was the table size, not the cap
	bool     bsp_not_lowest;   // a numbered CPU's APIC ID is below the BSP's
	bool     dense;            // the numbered IDs are exactly 0..count-1
	uint32_t apic_lowest;      // the numbered IDs' range, for the boot line
	uint32_t apic_highest;
} cpu_roster_report_t;

// Number the CPUs. `lapic_ids` is the bootloader's list in its own order;
// `cap` is the requested count (0 = no cap) and `max_cpus` the size of the
// caller's per-CPU tables, which must be below CPU_ROSTER_NO_CPU.
//
// On success, for every cpu < report->count, `slot_of_cpu[cpu]` is the index
// into `lapic_ids` of CPU `cpu`'s entry — the caller reads that CPU's ID and
// its bootloader record through it — and `apic_to_cpu[id]` is the CPU number
// for each numbered ID, CPU_ROSTER_NO_CPU for every other. `slot_of_cpu`
// must have room for `max_cpus` entries.
//
// Fails, writing nothing it promises, when the BSP's ID is not on the list
// or is one the xAPIC cannot address, or when `max_cpus` is zero or not
// below the sentinel.
bool cpu_roster_build(const uint32_t *lapic_ids, uint32_t listed,
                      uint32_t bsp_lapic_id, uint32_t cap, uint32_t max_cpus,
                      uint32_t *slot_of_cpu,
                      uint8_t apic_to_cpu[CPU_ROSTER_APIC_IDS],
                      cpu_roster_report_t *report);

#endif
