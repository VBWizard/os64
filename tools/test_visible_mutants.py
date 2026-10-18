#!/usr/bin/env python3
"""Break one rule of the occlusion geometry (OCCLUSION.md) per copy of
kernel/src/gui/visible.c and require the host oracle (test_visible_host.c)
to catch it. A mutant that compiles and passes is a rule the oracle does not
check.

Each copy is written to a temporary directory and built there, so the tree
is never edited and this runs beside anything.

Left out because no input can tell them from the original: the guard's
four-piece allowance written as three (a cut makes four only when the frame
lies inside one piece and so meets no other; the set then holds at most
about three quarters of the cap, far below the arrays), the guard's `>`
written `>=` (stopping one piece early is still a partition), `break`
written `continue` there (the frame is discarded either way), and `!fits`
dropped from the final test (a guard trips only when claims and pieces
already exceed the cap).
"""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "kernel/src/gui/visible.c"
MUTANTS = [
    # The cut: four bands, each only when it has area, each its own place.
    ("top-band-when-empty", "if (top > 0)", "if (top >= 0)"),
    ("bottom-band-lost", "if (bottom > 0)", "if (bottom > 1)"),
    ("left-band-lost", "if (left > 0)", "if (left > 1)"),
    ("right-band-lost", "if (right > 0)", "if (right > 1)"),
    ("top-band-full-height", "{r.x, r.y, r.w, top}", "{r.x, r.y, r.w, top + 1}"),
    ("bottom-band-starts-early", "{r.x, hole.y + hole.h, r.w, bottom}",
     "{r.x, hole.y + hole.h - 1, r.w, bottom}"),
    ("left-band-short", "{r.x, hole.y, left, hole.h}", "{r.x, hole.y, left, hole.h - 1}"),
    ("right-band-overlaps", "{hole.x + hole.w, hole.y, right, hole.h}",
     "{hole.x + hole.w - 1, hole.y, right, hole.h}"),
    # The intersection: touching is not meeting, and edges are 64-bit.
    ("touching-meets", "if (x1 <= x0 || y1 <= y0)", "if (x1 < x0 || y1 < y0)"),
    ("frame-edge-in-32-bits", "bx1 = (int64_t)b.x + b.w", "bx1 = b.x + b.w"),
    ("max-for-min", "int64_t x1 = ax1 < bx1 ? ax1 : bx1;", "int64_t x1 = ax1 > bx1 ? ax1 : bx1;"),
    # The walk: a missed piece moves across, the claim goes to its frame.
    ("a-missed-piece-needs-no-room", "(met ? 4 : 1)", "(met ? 4 : 0)"),
    ("a-missed-piece-is-dropped", "                next[nnext++] = left[i];\n", ""),
    ("claim-owned-by-the-next-frame", "(visible_piece_t){hole, f}", "(visible_piece_t){hole, f + 1}"),
    ("the-sets-do-not-swap", "        left = next;", "        left = left;"),
    # The cap: all of the pieces must fit, and no more than the arrays.
    ("the-cap-forgets-the-unclaimed", "nclaimed + nmade + nnext > cap", "nclaimed + nmade > cap"),
    ("the-cap-off-by-one", "nclaimed + nmade + nnext > cap", "nclaimed + nmade + nnext > cap + 1"),
    ("a-large-cap-is-believed", "        cap = VISIBLE_PIECES_MAX;", "        cap = VISIBLE_PIECES_MAX + 8;"),
    # What a walk consumed, and what an empty rectangle is.
    ("a-stopped-walk-consumed-all", "*consumed = nleft == 0 ? nframes : f;", "*consumed = nframes;"),
    ("an-emptied-walk-consumed-only-so-far", "*consumed = nleft == 0 ? nframes : f;", "*consumed = f;"),
    ("an-empty-rectangle-has-a-piece", "if (!empty(d) && cap > 0)", "if (cap > 0)"),
    ("height-not-checked", "return r.w <= 0 || r.h <= 0;", "return r.w <= 0;"),
    ("the-unclaimed-are-not-written",
     "        out[nclaimed + i] = (visible_piece_t){left[i], VISIBLE_UNCLAIMED};", "        (void)i;"),
]


def build(source, output):
    command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter",
               "-Wno-self-assign",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-I", str(ROOT / "kernel/include"),
               str(source), str(ROOT / "tools/test_visible_host.c"), "-o", str(output)]
    return subprocess.run(command, capture_output=True, text=True, timeout=180)


def main():
    original = SOURCE.read_text()
    environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0")
    caught = 0
    survivors = []
    with tempfile.TemporaryDirectory(prefix="visible-mutants-") as temporary:
        work = pathlib.Path(temporary)
        for name, find, replace in MUTANTS:
            if original.count(find) != 1:
                print(f"{name}: PATTERN NOT FOUND EXACTLY ONCE ({original.count(find)})")
                survivors.append(name)
                continue
            source = work / f"{name}.c"
            source.write_text(original.replace(find, replace))
            exe = work / name
            built = build(source, exe)
            if built.returncode != 0:
                print(f"{name}: BUILD FAILED\n{built.stderr[-800:]}")
                survivors.append(name)
                continue
            try:
                run = subprocess.run([str(exe)], capture_output=True, text=True,
                                     timeout=120, env=environment)
                failed = run.returncode != 0
                output = run.stderr + run.stdout
            except subprocess.TimeoutExpired:
                failed, output = True, "timeout"
            if failed:
                first = next((l for l in output.splitlines()
                              if "FAIL" in l or "ERROR" in l or "runtime error" in l), output[:150] or "nonzero exit")
                print(f"{name}: CAUGHT ({first.strip()[:150]})")
                caught += 1
            else:
                print(f"{name}: SURVIVED")
                survivors.append(name)
    print(f"\nvisible mutants: {caught} of {len(MUTANTS)} caught")
    if survivors:
        print("not caught: " + ", ".join(survivors))
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
