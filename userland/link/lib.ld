/* lib.ld — link script for os64 shared libraries (libos64.so, and any that
 * follow).
 *
 * A library is ET_DYN and position-independent, but os64 PRELINKS it: the
 * BUILD assigns its address (LIB_BASE, from userland/tools/app_bases.py
 * --libs) and the kernel honours it rather than choosing one. Every task maps
 * it there. That "same address everywhere" property is what lets a page be
 * relocated ONCE, cached, and then shared physically by every process — the
 * whole point of the exercise.
 *
 * WHY PRELINK RATHER THAN LET THE KERNEL PLACE IT (2026-08-22, the day after
 * the .so landed): the kernel's bump allocator placed libraries in LOAD ORDER,
 * so a library's address was stable only by accident and unknowable to the
 * build. That is fine for running and hostile to debugging — GDB had no
 * symbols at that address, so `step` into any library call silently became
 * `next`. Chris hit it immediately, stepping into os64_read from cp.c. With
 * the address decided here, the build emits an add-symbol-file line for it
 * and stepping into the library works like stepping into anything else.
 *
 * (Windows DLLs have carried a "preferred base address" since 3.x for the
 * same reason, and Linux's prelink(8) bolted the idea on later. Placement
 * chosen once, by the builder, so everyone agrees where the code lives.)
 *
 * No ENTRY(): a library has no entry point. (e_entry of 0 is exactly right
 * here, and nothing reads it — the kernel takes e_entry only from the main
 * executable.)
 *
 * WHY A SCRIPT AT ALL, rather than ld's built-in default: the default folds
 * .rodata in with .text and hands out ONE read+execute segment for both. os64
 * derives page protections straight from segment flags, so that would make
 * every constant in the library executable — the exact thing the NX arc
 * (2026-08-16) went to the trouble of fixing for apps. This script keeps the
 * same three zones app.ld uses: read-only, read+execute, read+write.
 *
 * EVERY DYNAMIC SECTION IS NAMED EXPLICITLY, and that is not stylistic. The
 * first draft of this file folded them into .rodata/.data by pattern
 * (`*(.dynsym)` inside the .rodata block, and so on). The link SUCCEEDED and
 * silently produced a library with no .dynamic, no .dynsym and no PT_DYNAMIC
 * at all — a shared object with nothing shareable in it, which then linked
 * into apps that had no dynamic section either and would have faulted on
 * their first library call. ld places these linker-created sections itself
 * and wants them as output sections under their own canonical names; give
 * them anything else and they quietly evaporate. Cost: one confusing hour.
 * If you reorganize this file, re-run `readelf -d` on the result and confirm
 * you still see SONEEDED/SONAME/HASH entries before you believe it.
 */

SECTIONS
{
    /* The prelinked base, supplied by the build (--defsym LIB_BASE=...). The
     * fallback is only for a hand-run ld; the build always passes a real one.
     * SIZEOF_HEADERS leaves room for the ELF header and program headers.
     *
     * NOTE the same trap app.ld documents: --defsym MUST come BEFORE -T on
     * the ld command line, or the symbol is not defined yet when this script
     * is read and the fallback silently wins. */
    . = (DEFINED(LIB_BASE) ? LIB_BASE : 0x00007F0000000000) + SIZEOF_HEADERS;

    /* ── R only ───────────────────────────────────────────────────────────
     * The dynamic-linking metadata the KERNEL reads: the symbol table it
     * searches, the strings behind the names, the hash table it takes the
     * symbol COUNT from (DT_HASH's nchain — ELF gives no DT_SYMTABSZ, so
     * that word is the only way to size the table), and the relocations
     * themselves. Pure input to relocation; nothing ever writes them, so a
     * stray store here faults instead of quietly rewriting the library's own
     * symbol table. */
    .hash     : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .rela.dyn : { *(.rela.dyn) }
    .rela.plt : { *(.rela.plt) }
    .rodata   : { *(.rodata*) }
    .eh_frame : { *(.eh_frame*) }

    /* ── R + X ────────────────────────────────────────────────────────────
     * Code, and the PLT stubs for any call not bound internally. With
     * -Bsymbolic-functions the library's calls to its OWN functions are bound
     * direct at link time, so this PLT should be empty — measured at zero
     * JUMP_SLOT relocations on the day it was built. If it ever grows,
     * something is being resolved through the dynamic symbol table that did
     * not need to be. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    /* Each PLT flavour in its own output section under its canonical name —
     * BFD synthesizes the `foo@plt` symbols a debugger needs to step THROUGH
     * a stub by finding these sections BY NAME, and folding them into .text
     * makes those symbols silently cease to exist (see app.ld for the full
     * story; it cost a second bug report). Should be empty here anyway:
     * -Bsymbolic-functions binds the library's own calls directly. */
    .plt      : { *(.plt) }
    .plt.got  : { *(.plt.got) }
    .plt.sec  : { *(.plt.sec) }
    .iplt     : { *(.iplt) }
    .text     : { *(.text*) }

    /* ── R + W ────────────────────────────────────────────────────────────
     * Mutable data, and the indirection tables relocation WRITES: .dynamic,
     * .got and .got.plt are all filled in by the loader.
     *
     * Every page here is copy-on-write per task (task.c marks writable
     * shared-object segments cow). So a data page's life is: first TOUCH by
     * anyone resolves it and applies its relocations into the one shared
     * physical page; first WRITE by a given task privatizes that task's own
     * copy. Two independent triggers — which is why a libos64 global is
     * shared with nobody, and every process gets its own. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic  : { *(.dynamic) }
    .got      : { *(.got) *(.igot) }
    .got.plt  : { *(.got.plt) *(.igot.plt) }
    .data     : { *(.data*) }
    .bss      : { *(.bss*) *(COMMON) }
}
