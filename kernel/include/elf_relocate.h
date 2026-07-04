#ifndef ELF_RELOCATE_H
#define ELF_RELOCATE_H

#include <stddef.h>
#include <stdint.h>

#include "elf.h"

/// @brief Resolve `name` to an absolute address, or 0 if not found. `ctx` is
/// whatever the caller passed to elf_relocation_value — typically a search
/// order over one or more loaded images' symbol tables.
typedef uintptr_t (*elf_symbol_resolver_t)(const char *name, void *ctx);

/// @brief Compute the 8-byte value a single Elf64_Rela relocation wants
/// stored at its r_offset, WITHOUT writing it anywhere.
///
/// This deliberately computes-but-does-not-write, unlike a classic
/// apply-in-place relocation loop. Under lazy per-page resolution
/// (shared_object.c) the destination is a single page-sized buffer, and a
/// relocation's 8-byte target can straddle a page boundary — start in the
/// last 7 bytes of one page and spill into the next. Only the caller knows
/// the page window, so only the caller can clip the write correctly (byte
/// slicing the value across two separately-resolved pages). Writing here
/// would either corrupt the heap past the buffer or silently lose the
/// spilled bytes.
///
/// `value_base` is the image's real, final virtual address (its chosen
/// load_bias) — used to compute the value baked into R_X86_64_RELATIVE
/// entries (value_base + addend), since that value is what other code will
/// later dereference through, at runtime, in a task's page table.
///
/// symtab/strtab are the *relocating* image's own tables (needed to look up
/// the name behind a symbol-relative entry); resolver is called with that
/// name to get the target address, so it can search this image, another
/// loaded shared object, or both, without this function knowing which.
///
/// We deliberately only implement the eager-bound relocation types actually
/// needed: RELATIVE (self-referential, no symbol lookup), and GLOB_DAT /
/// JUMP_SLOT / 64 (symbol-relative). There is no lazy PLT trampoline to
/// support — every relocation table is processed once, per page, at first
/// touch (see shared_object.c).
///
/// Returns 0 on success (*out_value filled in), -1 if a symbol couldn't be
/// resolved or an unsupported relocation type was encountered.
int elf_relocation_value(const Elf64_Rela *rel, uintptr_t value_base,
                         const Elf64_Sym *symtab, const char *strtab,
                         elf_symbol_resolver_t resolver, void *ctx,
                         uintptr_t *out_value);

#endif
