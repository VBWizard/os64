/* app.ld — link script for os64 userland apps.
 *
 * Static, non-PIE for now (PIE + loader-assigned bias is a later roadmap
 * item). Base 0x400000 matches the existing ring-3 fixtures and sits well
 * below the task heap (0x70000000) and the exit-trampoline VA. _start is the
 * launch stub. */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text._start)     /* launch first, so the ELF entry == _start */
        *(.text*)
    }

    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : {
        *(.bss*)
        *(COMMON)
    }
}
