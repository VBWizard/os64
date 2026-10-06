#!/usr/bin/env python3
"""Break one frame-table rule (FRAMES.md) per isolated copy of frames.c and
require the host suite (test_frames_host.c, FRAMES_QUICK) to catch it.

Each mutant names the rule it breaks. A mutant that compiles and passes is a
rule the suite does not actually check."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "kernel/src/memory/frames.c"
MUTANTS = [
    # Coalescing: a freed run joins the free run on each side.
    ("coalesce-below", "if (first > 0 && kind_of(f->table[first - 1]) == FRAMES_FREE) {",
     "if (0) {"),
    ("coalesce-above", "if (end < f->nframes && kind_of(f->table[end]) == FRAMES_FREE) {",
     "if (0) {"),
    # A neighbour's boundary tag swallowed by a merge goes back to interior.
    ("merged-tail-to-interior",
     "\t\tif (len > 1)\n\t\t\tf->table[first - 1] = make(FRAMES_FREE, POS_INTERIOR, 0, 0);\n", ""),
    ("merged-head-to-interior",
     "\t\tif (len > 1)\n\t\t\tf->table[end] = make(FRAMES_FREE, POS_INTERIOR, 0, 0);\n", ""),
    # Boundary tags: both ends of every run carry its length.
    ("free-tail-tag",
     "\t\tf->table[first + count - 1] = make(FRAMES_FREE, POS_TAIL, count, FRAMES_NONE);\n", ""),
    ("allocated-tail-tag",
     "\tf->table[first + count - 1] = make(kind, POS_TAIL, count, 0);\n", ""),
    # Exact-size recycling: a repeating size gets its own hole back.
    ("exact-list-first",
     "\t\tif (f->exact_head[count] != FRAMES_NONE)\n\t\t\treturn f->exact_head[count];\n", ""),
    # TLSF: round up so the head fits; walk the skipped sub-bin before refusing.
    ("tlsf-round-up",
     "uint64_t rounded = count + ((uint64_t)1 << (floor_log2(count) - FRAMES_SL_LOG2)) - 1;",
     "uint64_t rounded = count;"),
    ("tlsf-fallback-walk",
     "\t\tif (run_length(f, h) >= count)\n\t\t\treturn h;\n", "\t\t(void)h;\n"),
    # The masks say which lists are non-empty.
    ("mask-cleared-when-empty",
     "\tif (*lh == FRAMES_NONE)\n\t\tlist_mark(f, len, false);\n", ""),
    ("largest-free-highest-sub-bin",
     "unsigned sl = floor_log2(f->sl_mask[fl]);",
     "unsigned sl = (unsigned)__builtin_ctz(f->sl_mask[fl]);"),
    # The links: a single free run keeps prev in b.
    ("single-prev-in-b",
     "\t\tf->table[head] = with_b(w, prev);\n\t\treturn;",
     "\t\tf->table[head] = with_a(w, prev);\n\t\treturn;"),
    # Allocation hands back the rest of the run.
    ("remainder-refiled",
     "\tif (len > count)\n\t\twrite_free(f, head + count, len - count);\n", ""),
    # Counters: free + run + ledger == usable, free runs counted.
    ("free-counter", "\tf->free_frames -= count;\n\tif (kind == FRAMES_RUN)\n\t\tf->run_frames += count;",
     "\tif (kind == FRAMES_RUN)\n\t\tf->run_frames += count;"),
    ("free-runs-counter", "\tlist_insert(f, first, count);\n\tf->free_runs++;",
     "\tlist_insert(f, first, count);"),
    # Tripwires: only a run's first frame frees it; pinned ledger runs stay.
    ("free-needs-a-start",
     "\tif (pos_of(w) != POS_SINGLE && pos_of(w) != POS_HEAD)\n\t\treturn FRAMES_NOT_A_START;\n", ""),
    ("pinned-ledger-stays", "\t\t\t\treturn FRAMES_LEDGER_PINNED;", "\t\t\t\tbreak;"),
    ("pin-never-below-zero",
     "\t\tif ((delta < 0 && pins == 0) || (delta > 0 && pins == FIELD_MASK))\n\t\t\treturn FRAMES_PIN_RANGE;\n",
     ""),
    ("pin-only-ledger", "\t\tif (kind_of(w) != FRAMES_LEDGER)\n\t\t\treturn FRAMES_NOT_LEDGER;\n", ""),
    # Liveness: a ledger frame is live only while pinned.
    ("ledger-live-needs-pins",
     "return kind == FRAMES_RUN || (kind == FRAMES_LEDGER && b_of(w) != 0);",
     "return kind == FRAMES_RUN || kind == FRAMES_LEDGER;"),
    # Page zero is never usable memory.
    ("page-zero-clipped", "\tif (first == 0 && count > 0) {", "\tif (0) {"),
    # Describing memory twice is refused.
    ("describe-once", "\t\t\treturn FRAMES_NOT_RESERVED;\n", "\t\t\t(void)0;\n"),
]


def build(source, output):
    command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-I", str(ROOT / "kernel/include/memory"),
               str(source), str(ROOT / "tools/test_frames_host.c"), "-o", str(output)]
    return subprocess.run(command, capture_output=True, text=True, timeout=180)


def main():
    original = SOURCE.read_text()
    environment = dict(os.environ, FRAMES_QUICK="1",
                       ASAN_OPTIONS="detect_leaks=0:abort_on_error=0")
    proposed = compiled = caught = 0
    survivors = []
    with tempfile.TemporaryDirectory(prefix="frames-mutants-") as temporary:
        work = pathlib.Path(temporary)
        for name, find, replace in MUTANTS:
            proposed += 1
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
            compiled += 1
            try:
                run = subprocess.run([str(exe)], capture_output=True, text=True,
                                     timeout=120, env=environment)
                if run.returncode != 0:
                    first = next((l for l in (run.stderr + run.stdout).splitlines()
                                  if "FAIL" in l or "ERROR" in l or "runtime error" in l), "nonzero exit")
                    print(f"{name}: CAUGHT ({first.strip()[:150]})")
                    caught += 1
                else:
                    print(f"{name}: SURVIVED")
                    survivors.append(name)
            except subprocess.TimeoutExpired:
                print(f"{name}: CAUGHT (hung past 120 seconds)")
                caught += 1
    print(f"frames mutants: {compiled} compiled, {caught} caught, {proposed} proposed")
    return 0 if caught == proposed else 1


if __name__ == "__main__":
    sys.exit(main())
