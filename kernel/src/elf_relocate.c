#include "elf_relocate.h"

int elf_relocation_value(const Elf64_Rela *rel, uintptr_t value_base,
                         const Elf64_Sym *symtab, const char *strtab,
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
            uint32_t sym_idx = (uint32_t)ELF64_R_SYM(rel->r_info);
            const char *name = strtab + symtab[sym_idx].st_name;
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
