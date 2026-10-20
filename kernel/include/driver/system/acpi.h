// acpi.h — the kernel's side of ACPI: walk the firmware's tables once at
// boot, take what the kernel uses, and keep the S5 descent.
//
// The walking and decoding is acpi_tables.c (pure, host-tested); this side
// owns the mappings it reads through and the globals it fills. /sys/acpi
// renders everything the walk found, used or not.

#ifndef ACPI_H
#define ACPI_H

#include <stdbool.h>
#include <stdint.h>
#include "acpi_tables.h"

// Per-ISA-IRQ routing from the MADT's Interrupt Source Overrides:
// kISAIrqToGSI[n] is the GSI that ISA IRQ n is wired to, or -1 for no
// override (identity, per the spec); kISAIrqOverrideFlags[n] holds the
// override's MPS INTI flags (polarity, trigger). The PIT is the classic
// case — IRQ 0 -> GSI 2 on VirtualBox and most real hardware — and routing
// it by number instead lands the tick on a masked IOAPIC pin.
extern int16_t kISAIrqToGSI[16];
extern uint16_t kISAIrqOverrideFlags[16];

// Walk everything reachable from Limine's RSDP and set what the kernel runs
// on: kIOAPICAddress (the IOAPIC that serves GSI 0, where the ISA IRQs
// live), kISAIrqToGSI, kPCIBaseAddress and the PCI bus range (MCFG segment
// 0, mapped), and the S5 numbers acpi_poweroff spends. Runs on the boot
// core before any other core is started. Every page it maps to READ the
// tables is unmapped again before it returns; the PCI configuration window
// stays.
void acpi_init(void);

// The walk's record, for /sys/acpi. Written once by acpi_init and never
// again, which is why reading it needs no lock.
const acpi_info_t *acpi_info(void);

// Which of the walk's entries the kernel took — indices into
// acpi_info()->madt.ioapics and ->mcfg, or -1 for none. Set by acpi_init.
typedef struct { int ioapic; int mcfg; } acpi_used_t;
const acpi_used_t *acpi_used(void);

// Ask the machine to power off through ACPI's soft-off (S5). Returns FALSE
// if it could not even try (no FADT, no \_S5, no PM1a control block) — the
// caller then falls back. On success it does not return.
bool acpi_poweroff(void);

#endif // ACPI_H
