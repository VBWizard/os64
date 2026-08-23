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
    /* NO PT_INTERP. Linking against a shared library makes ld helpfully name
     * an interpreter — "/lib/ld64.so.1", the userspace dynamic linker it
     * assumes will show up and do the work. os64 has no such program and
     * never will: THE KERNEL IS THE DYNAMIC LINKER (kernel/src/shared_object.c
     * resolves and relocates every page on first touch). A PT_INTERP naming a
     * file that does not exist is a statement the file makes about itself
     * that is simply false, and the day something starts honouring it — a
     * debugger, a future loader path, an `ldd`-alike — it would go looking
     * for a program that was never written. Discard it at the source.
     *
     * (Historical aside: PT_INTERP is how SunOS 4.0 introduced dynamic
     * linking in 1988 — put the linker in userspace, name it in the file,
     * let the kernel exec THAT and hand it the real program. It was the right
     * call for a system that wanted the linker replaceable. os64 wants the
     * opposite: one loader, in the kernel, sharing pages across tasks.) */
    /DISCARD/ : { *(.interp) }

    . = DEFINED(APP_BASE) ? APP_BASE : 0x400000;

    .text : {
        *(.text._start)     /* launch first, so the ELF entry == _start */
        *(.text*)
    }

    /* THE PLT, IN A SECTION ACTUALLY CALLED `.plt` (2026-08-22). Every call an
     * app makes into the library goes through a stub here, which jumps via a
     * .got.plt slot the loader fills in. The stubs are code, so they sit
     * adjacent to .text and share its executable segment.
     *
     * IT MUST BE ITS OWN OUTPUT SECTION, UNDER THIS NAME. The first version of
     * this script folded the stubs into .text with `*(.plt)` — which linked
     * fine, ran fine, and broke the debugger in a way that took a second
     * report to understand: `step` at a library call STEPPED OVER, unless a
     * breakpoint happened to be set inside the callee. The reason is that
     * `foo@plt` symbols do not exist in the file at all; BFD SYNTHESIZES them
     * by finding the PLT — and it finds it by SECTION NAME (".plt",
     * ".plt.sec", ".plt.got"). With the stubs hidden inside .text there was no
     * such section, so no synthetic symbols, so GDB saw the stub as an
     * anonymous stretch of .text with no line info and none of the markings
     * that say "trampoline — follow it through". Its rule for stepping into
     * code with no line info is to step back out, and that is precisely what
     * it did.
     *
     * (This is the same lesson as the dynamic sections below, one section
     * later: ld and BFD both look these up by their canonical names, and
     * giving them any other name loses machinery silently.)
     *
     * There is no lazy binding behind these stubs: os64's loader resolves
     * every relocation on a page the first time that page is touched, so a
     * .got.plt slot is already correct before the stub can ever run. That
     * sidesteps the single most fragile thing in a hand-rolled dynamic linker
     * — the _dl_runtime_resolve trampoline, which is where libChrisOS's
     * internal calls used to crash (LIBOS64.md's first fingerprint). */
    .plt      : { *(.plt) }
    .plt.got  : { *(.plt.got) }
    .plt.sec  : { *(.plt.sec) }
    .iplt     : { *(.iplt) }

    /* PAGE-ALIGN EACH PERMISSION CHANGE (2026-08-16, the NX arc). Without
     * these, ld folds text+rodata+data into ONE RWX LOAD segment (the
     * "RWX permissions" warning every build printed) — and since the kernel
     * derives page protections from segment flags, every byte of every app
     * was writable AND executable. With the boundaries aligned, ld emits
     * separate LOAD segments (text R+X, rodata R, data/bss R+W), the loader's
     * honest PF_X-to-PROT_EXEC mapping finally has something to say, and the
     * fixed PTE bit 63 makes it stick. The cost is up to a page of padding
     * per boundary in the file — the price of W^X, at 1971 prices. */
    /* THE DYNAMIC SECTIONS GET THEIR OWN CANONICAL NAMES (2026-08-22). The
     * first draft folded them into .rodata/.data with patterns like
     * `*(.dynsym)`. The link succeeded and silently produced an executable
     * with NO dynamic section at all — undefined library symbols left
     * undefined, no DT_NEEDED, no relocations, and no complaint from ld. It
     * would have faulted on its first call into libos64. ld places these
     * linker-created sections itself and wants them as output sections under
     * their own names; anything else and they evaporate. If you reorganize
     * this file, run `readelf -d` on an app and confirm you still see a
     * NEEDED line before believing the build. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    /* The dynamic linking metadata the kernel's loader READS: which library
     * this program needs, the symbols it imports, and the relocations that
     * wire them up. Pure input — nothing writes them — so they live in the
     * read-only segment, where a stray store faults instead of quietly
     * rewriting the program's own symbol table. */
    .hash     : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .rela.dyn : { *(.rela.dyn) }
    .rela.plt : { *(.rela.plt) }
    .rodata   : { *(.rodata*) }
    .eh_frame : { *(.eh_frame*) }

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    /* .dynamic is the table of contents the loader walks (DT_NEEDED,
     * DT_SYMTAB, DT_RELA…); .got/.got.plt are the slots relocation WRITES
     * the library's addresses into. Writable because they are written — and
     * being writable, they are copy-on-write per task like every other data
     * page, so each process ends up with its own privatized copy the first
     * time anything stores through them. */
    .dynamic  : { *(.dynamic) }
    .got      : { *(.got) *(.igot) }
    .got.plt  : { *(.got.plt) *(.igot.plt) }
    .data     : { *(.data*) }
    .bss      : {
        *(.bss*)
        *(COMMON)
    }

    /* THE SLOT CHECK (2026-08-22). Every app is linked into a fixed-size slot
     * carved out of the low 2GB by userland/tools/app_bases.py, and until now
     * NOTHING VERIFIED THAT THE APP FIT. An app that outgrew its slot would
     * simply overlap its neighbour's address range — it would build clean, run
     * clean (each app has its own address space, so nothing collides at
     * RUNTIME), and fail only in the debugger, where two programs' symbols now
     * claim the same addresses. That is the exact failure shape as the
     * --defsym-after-T bug this file already carries a warning about: correct
     * everywhere except the one place you go when something is already wrong.
     *
     * The slot shrank from 16MB to 4MB when libos64 became a shared object and
     * app images collapsed to their own code, so the margin went from ~1000x to
     * ~25x for a typical app — still enormous, but no longer so absurd that
     * "nobody could possibly hit it" is an argument. Hence the assert: three
     * lines that turn a debugger mystery into a build failure naming the app.
     *
     * THE ONE PAGE OF SLACK is deliberate margin, not arithmetic. While the
     * .interp discard above was not yet in place, ld mapped the ELF header
     * and program headers in a read-only page just BELOW APP_BASE (an
     * interpreter needs to find the phdrs, so they have to be mapped) — an
     * app's image quietly began one page before its own slot, in the tail of
     * its neighbour's. Discarding .interp removed the reason and the page
     * with it: every app's lowest PT_LOAD is now exactly APP_BASE, verified
     * across all 64. The page of margin stays anyway, because the next thing
     * that wants a mapped header — a core-dump writer, a symbol server, a
     * future loader that reads phdrs from memory — will bring it straight
     * back, and a slot check that is one page too generous costs nothing
     * while one that is one page too tight fails a build for no reason.
     *
     * APP_SLOT_SIZE is supplied by the build (--defsym, from app_bases.py
     * --slot-size, the same file that owns the number). The fallback matters
     * only for a hand-run ld. */
    ASSERT(. - (DEFINED(APP_BASE) ? APP_BASE : 0x400000)
             <= (DEFINED(APP_SLOT_SIZE) ? APP_SLOT_SIZE : 0x400000) - CONSTANT(MAXPAGESIZE),
           "app image is larger than its app_bases.py slot — it would overlap the next app's address range (see userland/tools/app_bases.py)")
}
