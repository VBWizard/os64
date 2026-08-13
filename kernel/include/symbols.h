#ifndef SYMBOLS_H
#define SYMBOLS_H

#include <stdint.h>

// ── Names for kernel addresses ──────────────────────────────────────────────
//
// Built 2026-08-12, the day the NMI probe went live behind /sys/cpu/<n>/probe
// and every snapshot it took was sixteen hex addresses and a shrug. This is
// the missing half of that instrument — and of every panic, exception report
// and kernel call chain the reporting arc taught to print real registers.
//
// WHERE THE NAMES COME FROM. Limine hands us the kernel's own ELF file whole
// (kernel_file_request — we already used it for the cmdline), and the linked
// kernel retains .symtab and .strtab. symbols_init() walks that file ONCE at
// boot and copies what it needs into two kmalloc'd tables; nothing here ever
// touches the file again, the disk at all, or a lock of any kind.
//
// WHAT GETS A NAME. Every symbol in an executable section — sized FUNCs from
// C, and the zero-size FUNC/NOTYPE labels assembly leaves behind (exc_common,
// irq0_eoi_done, task_exit_with_retval...). Resolution is floor-to-next-
// symbol, the same rule Linux's kallsyms settled on, because a zero-size asm
// label still owns every byte down to the next label — so an RIP inside
// scheduler.S resolves to the nearest label instead of falling off the map.
//
// THE SAFETY CONTRACT (this is reporter-path code, so rule one applies:
// NEVER FAULT WHILE REPORTING A FAULT):
//   - symbols_for_address takes no lock, allocates nothing, and reads only
//     the two tables symbols_init built. Call it from a panic, an exception
//     reporter, or the NMI probe's asker side, under any CR3.
//   - If symbols_init never ran, failed, or the kernel was stripped, every
//     lookup returns NULL and callers print hex — the instrument degrades
//     to exactly what it was the day before this file existed, which is why
//     there is no cmdline kill switch: the fallback IS the old behavior.
//
// file:line is DELIBERATELY absent — that needs a DWARF .debug_line state
// machine and is its own future slice. The kernel file stays mapped after
// init partly for that day: .debug_line is in the same blob.

/// @brief Parse the kernel ELF (Limine's kernel_file) and build the tables.
/// Call once, early in kernel_init — needs kmalloc and the blob mapping that
/// init_os64_paging_tables carries over. Failure is announced and non-fatal.
void symbols_init(void);

/// @brief Name the kernel function (or asm label) containing `addr`.
/// @param offset Receives addr's offset into the symbol (the "+0x1c" part);
///               untouched when the answer is NULL. May be NULL if unwanted.
/// @return The name, or NULL if addr is outside every known symbol (or the
///         tables were never built). The pointer is stable for the life of
///         the kernel — safe to hold, never to free.
const char *symbols_for_address(uint64_t addr, uint64_t *offset);

#endif // SYMBOLS_H
