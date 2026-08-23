#include "elf_relocate.h"

int elf_relocation_value(const Elf64_Rela *rel, uintptr_t value_base,
                         const Elf64_Sym *symtab, size_t symtab_count,
                         const char *strtab, size_t strtab_size,
                         elf_symbol_resolver_t resolver, void *ctx,
                         uintptr_t *out_value)
{
    uint32_t type = (uint32_t)ELF64_R_TYPE(rel->r_info);

    switch (type) {
        case R_X86_64_RELATIVE:
            // Self-referential — no symbol lookup, just base + addend.
            *out_value = value_base + (uintptr_t)rel->r_addend;
            return 0;

        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_64: {
            if (symtab == NULL || strtab == NULL || resolver == NULL) {
                return -1;
            }
            // BOUNDS-CHECK THE METADATA (2026-08-22). This function is a
            // LINKER RUNNING IN RING 0 over a file on disk: an out-of-range
            // symbol index read whatever followed the symbol table, and an
            // out-of-range st_name pointed the resolver's strcmp at arbitrary
            // kernel memory. Both were listed as accepted risks while every
            // binary on the disk was built by our own Makefiles — a position
            // that expired the moment userland started loading real shared
            // libraries from a writable ext2 root. Refusing here turns "a
            // corrupt .so corrupts the kernel" into "that program will not
            // start", which is the same answer a typo already gets.
            uint32_t sym_idx = (uint32_t)ELF64_R_SYM(rel->r_info);
            if (sym_idx >= symtab_count) {
                return -1;
            }
            Elf64_Xword name_off = symtab[sym_idx].st_name;
            if (name_off >= strtab_size) {
                return -1;
            }
            const char *name = strtab + name_off;
            uintptr_t sym_addr = resolver(name, ctx);
            if (sym_addr == 0) {
                return -1;
            }
            // GLOB_DAT/JUMP_SLOT carry no addend in practice (always 0),
            // but R_X86_64_64 does — applying it uniformly is harmless.
            *out_value = sym_addr + (uintptr_t)rel->r_addend;
            return 0;
        }

        default:
            // Fail loudly rather than silently producing a broken image
            // — we only support the eager-bound relocation set above.
            return -1;
    }
}
