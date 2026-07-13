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

  16MB per slot: the apps are ~17KB today, so that is a ~1000x margin. 104 slots.

THE LIMIT (and why PIE is still the real fix — see DEBTS)
  Two instances of the SAME program in one pipeline (`upper | upper`) still land
  at one base, so GDB cannot tell those two apart. They RUN fine — separate
  address spaces — it is purely a debugging ambiguity. The proper fix is PIE with
  a loader-assigned per-TASK bias, which this scheme is a stopgap for.
"""

import sys
import zlib

APP_BASE_START = 0x00400000
APP_SLOT_SIZE  = 0x01000000     # 16MB of image room per app
NSLOTS         = 104            # 0x400000 + 103*16MB + 16MB = 0x68400000 < 0x6f000000


def assign(apps):
    used = {}
    result = []

    for app in sorted(apps):
        slot = zlib.crc32(app.encode()) % NSLOTS
        probed = 0
        while slot in used:
            slot = (slot + 1) % NSLOTS
            probed += 1
            if probed > NSLOTS:
                raise SystemExit("app_bases: out of slots (%d apps, %d slots)"
                                 % (len(apps), NSLOTS))
        used[slot] = app
        result.append((app, APP_BASE_START + slot * APP_SLOT_SIZE))

    return result


if __name__ == "__main__":
    for name, base in assign(sys.argv[1:]):
        print("%s=0x%08x" % (name, base))
