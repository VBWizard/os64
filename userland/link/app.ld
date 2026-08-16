/* app.ld — link script for os64 userland apps.
 *
 * Static, non-PIE for now (PIE + loader-assigned bias is the real fix — see
 * DEBTS). _start is the launch stub.
 *
 * APP_BASE is supplied per app by the build (--defsym APP_BASE=0x...), from
 * userland/tools/app_bases.py: EVERY APP GETS ITS OWN BASE ADDRESS. They used
 * to all link at 0x400000, which meant two programs running at once — i.e. any
 * pipeline — occupied the same linear addresses and GDB (one global symbol
 * table, keyed by address) could not tell them apart. Unique bases let GDB hold
 * every app's symbols simultaneously and make a breakpoint resolve to an
 * address only one program occupies.
 *
 * The default below is only a fallback for a hand-run ld; the build always
 * passes a real one. The window is 0x400000..0x6f000000 (below TASK_ARGV_VIRT);
 * the task heap at 0x70000000 is untouched and unaffected. */

ENTRY(_start)

SECTIONS
{
    . = DEFINED(APP_BASE) ? APP_BASE : 0x400000;

    .text : {
        *(.text._start)     /* launch first, so the ELF entry == _start */
        *(.text*)
    }

    /* PAGE-ALIGN EACH PERMISSION CHANGE (2026-08-16, the NX arc). Without
     * these, ld folds text+rodata+data into ONE RWX LOAD segment (the
     * "RWX permissions" warning every build printed) — and since the kernel
     * derives page protections from segment flags, every byte of every app
     * was writable AND executable. With the boundaries aligned, ld emits
     * separate LOAD segments (text R+X, rodata R, data/bss R+W), the loader's
     * honest PF_X-to-PROT_EXEC mapping finally has something to say, and the
     * fixed PTE bit 63 makes it stick. The cost is up to a page of padding
     * per boundary in the file — the price of W^X, at 1971 prices. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : { *(.rodata*) }
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data   : { *(.data*) }
    .bss    : {
        *(.bss*)
        *(COMMON)
    }
}
