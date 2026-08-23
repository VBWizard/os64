#!/usr/bin/env python3
"""app_bases.py — assign every userland app its own unique link base.

WHY THIS EXISTS
GDB has ONE global symbol table, keyed by address. Every os64 app used to link
at 0x400000, so two programs running at once (which is exactly what a pipeline
is — `hello | upper`) occupied the SAME linear addresses, and GDB could not tell
them apart: add-symbol-file both and address->symbol lookup becomes a coin flip
(a stop in one app gets labelled with the other app's function). The old
workaround was to keep only the most recently loaded app's symbols — fine when
debugging was one-app-at-a-time, useless the moment pipelines arrived.

Giving each program its own base fixes it at the root: the addresses no longer
overlap, so GDB can hold EVERY app's symbols simultaneously and a breakpoint
resolves to a linear address that only one program occupies. (This is Chris's
original 32-bit-OS trick, and it was right all along.)

THE SCHEME
  slot  = crc32(app name) % NSLOTS, linear-probed on collision
  base  = APP_BASE_START + slot * APP_SLOT_SIZE

Keyed on the NAME, not on build order or a hand-maintained table, so:
  * an app's address is a pure function of what it is called — stable across
    rebuilds, and adding a new app does not shove the others around;
  * the "just drop a directory and it builds" property is preserved. No table
    to edit, ever.

Apps are processed in sorted order so the probe sequence (and therefore the
whole map) is deterministic for a given set of apps.

THE WINDOW
  0x00400000 .. 0x6f000000, which is everything between the old fixed base and
  TASK_ARGV_VIRT (0x6f000000 — argv, env and the exit trampoline live there).
  The task heap is at 0x70000000 and is NOT affected by any of this: every task
  has its own address space, so all of them can share one heap VA. Only CODE
  needs unique addresses, because only code has symbols.

  THE CEILING IS REAL AND IT IS ~128 (Chris called it, 2026-08-22, before
  anyone had counted). It is not the slot count that binds — it is
  `-mcmodel=small`: a non-PIE app addresses its own symbols with 32-bit
  displacements, so every app must live in the low 2GB, and 2GB / 16MB is 127
  apps no matter how the window is drawn. The only ways past it are PIE (which
  fights shared text — see below) or making the debugger process-aware.

  4MB PER SLOT since 2026-08-22, 443 slots — and SHARED LIBRARIES ARE WHAT PAID
  FOR IT. The slot was 16MB because an app image was ~200KB, nearly all of it a
  private copy of libos64 statically linked into every single binary. With
  libos64 a real .so, an app's image is only its OWN code: 5-50KB for most,
  158KB for husk, ~1MB for the two that still carry a static megabyte buffer
  from the days before malloc existed. Quartering the slot quadrupled the app
  count, and the feature funded its own address space.

THE LIMIT (and why PIE is NOT the fix — superseded 2026-08-22, see DEBTS)
  Two instances of the SAME program in one pipeline (`upper | upper`) land at
  one base, so GDB cannot tell those two apart. They RUN fine — separate address
  spaces — it is purely a debugging ambiguity. This file used to say PIE with a
  per-TASK load bias was the proper fix. It is not, and cannot be: shared text
  REQUIRES one virtual address per image across every task that maps it, so the
  moment libos64 became a .so, per-task bias became incompatible with the thing
  it would have been serving. The real successor is what Linux does — a
  process-aware debugger (an in-OS debug server speaking the GDB remote
  protocol, reading /proc/<pid>/maps), at which point addresses stop mattering
  and this whole file retires.

  Historical note on what this scheme actually is: unique link addresses are a
  NO-MMU technique — DOS TSRs, ROM images, overlays; you place programs by hand
  because there is only one address space. Unix stopped needing it the moment
  the PDP-11 gave every process its own. os64 has proper per-task isolation and
  does not need this to RUN — only to be DEBUGGED, because GDB sits outside the
  MMU looking at QEMU's flat linear space with no idea what a task is. It is a
  no-MMU tax paid by a machine that has an MMU, on the debugger's behalf.

PRELINKED LIBRARIES (--libs), added 2026-08-22
  A shared library is position-independent and COULD be placed anywhere at
  load time — and at first it was, by a bump allocator in the kernel. That
  worked and was immediately, quietly hostile to debugging: the address a
  library lands at then depends on the ORDER objects happened to load in, so
  the build cannot tell GDB where its symbols went, and `step` into a library
  call silently degrades into `next` (no line info at that address = nothing
  to step into). Chris hit it within a day: "of course this is one of the
  first things I'd do in a shared library environment ... jump into the
  library."

  So libraries get build-time addresses too, by the same name hash: os64
  PRELINKS. `lib.ld` links each .so at its assigned base instead of 0, and
  the kernel honours a non-zero base rather than choosing one (load_bias 0 —
  the identical path a non-PIE executable takes, which is why this cost
  almost no kernel code). The address is then a pure function of the
  library's name: stable across boots, across load orders, and knowable by
  the build, which emits an add-symbol-file line for it.

  The lineage is older than Linux's prelink(8): every Windows DLL since 3.x
  has carried a "preferred base address" for exactly this reason — placement
  chosen once, by the builder, so the loader does not have to relocate and
  everyone agrees where the code lives.

  64MB slots in a 4GB region, 64 libraries. The kernel's bump allocator still
  exists and now starts ABOVE this region, serving anything that arrives
  without a base of its own (a PIE executable, or a .so built by hand).
"""

import sys
import zlib

APP_BASE_START = 0x00400000
APP_SLOT_SIZE  = 0x00400000     # 4MB of image room per app
# 0x400000 + 443*4MB == 0x6f000000 == TASK_ARGV_VIRT exactly: the window is
# filled to its last usable byte, and app.ld's ASSERT (fed APP_SLOT_SIZE by the
# makefile) fails the BUILD if any single app outgrows its slot — the check
# that did not exist while the slot was 16MB, when an oversized app would have
# silently overlapped its neighbour and failed only in the debugger.
NSLOTS         = 443

# The PRELINK region, carved off the FRONT of the kernel's shared-library
# window (TASK_SHLIB_VIRT_BASE in kernel/include/shared_object.h — these two
# numbers must agree, and the kernel range-checks every prelinked base against
# it, so a disagreement is a loud refusal at load rather than a mystery).
# Everything above LIB_BASE_START + LIB_NSLOTS*LIB_SLOT_SIZE belongs to the
# kernel's bump allocator, for images that arrive without a base of their own.
LIB_BASE_START = 0x00007F0000000000
LIB_SLOT_SIZE  = 0x04000000     # 64MB per library — libos64.so is ~110KB mapped
LIB_NSLOTS     = 64


def assign(names, base_start=APP_BASE_START, slot_size=APP_SLOT_SIZE,
           nslots=NSLOTS, what="app"):
    """Assign each name a unique base: crc32(name) % nslots, linear-probed.

    Parameterized (rather than duplicated into a second script) because apps
    and libraries want the SAME scheme with different constants — one hash,
    one probe, one place to reason about collisions."""
    used = {}
    result = []

    for name in sorted(names):
        slot = zlib.crc32(name.encode()) % nslots
        probed = 0
        while slot in used:
            slot = (slot + 1) % nslots
            probed += 1
            if probed > nslots:
                raise SystemExit("app_bases: out of %s slots (%d %ss, %d slots)"
                                 % (what, len(names), what, nslots))
        used[slot] = name
        result.append((name, base_start + slot * slot_size))

    return result


def assign_libs(names):
    return assign(names, LIB_BASE_START, LIB_SLOT_SIZE, LIB_NSLOTS, "library")


if __name__ == "__main__":
    args = sys.argv[1:]

    # `--slot-size` reports the slot in hex and exits: the makefile passes it to
    # ld as --defsym APP_SLOT_SIZE so the link script can ASSERT that the app
    # actually fits. One definition of the number, in this file, used by both
    # the allocator and the check.
    if "--slot-size" in args:
        print("0x%x" % APP_SLOT_SIZE)
        raise SystemExit(0)
    if "--lib-slot-size" in args:
        print("0x%x" % LIB_SLOT_SIZE)
        raise SystemExit(0)

    # `--libs` switches to the prelink region — same hash, different window.
    # 16 hex digits, because a library base is a full 64-bit address.
    if "--libs" in args:
        for name, base in assign_libs([a for a in args if a != "--libs"]):
            print("%s=0x%016x" % (name, base))
        raise SystemExit(0)

    for name, base in assign(args):
        print("%s=0x%08x" % (name, base))
