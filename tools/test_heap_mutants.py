#!/usr/bin/env python3
"""One deliberate break at a time to os64_malloc_aligned and the release
path it shares with free (userland/libos64/heap.c), against
tools/test_heap_host.sh: a mutant is CAUGHT when the suite fails, MISSED when
it passes, which means a test is missing and gets written. A build failure
is neither.

The shape is test_bt_acl_mutants.py's: each mutant is applied to the tree's
copy of heap.c, the suite is run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it from
a clean tree, and never beside another harness that edits the same tree;
under tools/mutants_parallel.py each worker has its own.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/heap-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

HEAP = 'userland/libos64/heap.c'
CASES = 'bash tools/test_heap_host.sh'

mutants = [
    # The shape: a power of two from 16 to a page, nothing else.
    ('any-alignment-goes', '\t                (alignment & (alignment - 1)) == 0;', '\t                1;'),
    ('below-sixteen-goes', 'bool shape_ok = alignment >= HEAP_ALIGN &&', 'bool shape_ok = alignment >= 1 &&'),
    ('past-a-page-goes', 'alignment <= HEAP_ALIGNED_MAX &&', '1 &&'),
    # A big block is HEAP_DEDICATED_ALIGN-aligned; stricter is refused.
    ('big-strict-is-dedicated',
     '(size >= HEAP_BIG_BYTES && alignment <= HEAP_DEDICATED_ALIGN)', '(size >= HEAP_BIG_BYTES)'),
    # Refusals count as calls.
    ('refusals-uncounted', '\tgReport.calls_malloc++;\n\tif (!shape_ok || size >= HEAP_BIG_BYTES)',
     '\tif (!shape_ok || size >= HEAP_BIG_BYTES)'),
    # The payload lands on the alignment asked for.
    ('aligned-to-sixteen-only',
     'align_up((uint64_t)(uintptr_t)b + sizeof(heap_block_t), alignment)',
     'align_up((uint64_t)(uintptr_t)b + sizeof(heap_block_t), HEAP_ALIGN)'),
    # A front gap too small to be a block moves the header on.
    ('tiny-front-kept', 'if (gap != 0 && gap < HEAP_MIN_BLOCK)', 'if (0)'),
    # The block taken is big enough for the worst gap.
    ('taken-too-small', 'uint64_t over = need + alignment + HEAP_MIN_BLOCK;',
     'uint64_t over = need + HEAP_MIN_BLOCK;'),
    # The front piece is released, through free's own path.
    ('front-leaked', '\t\trelease_block_locked(&home->region, front, block_payload(front));\n', ''),
    # The front piece keeps its neighbour bit, so it merges with a free block below.
    ('front-forgets-a-free-neighbour', '\t\tblock_set(front, gap, keep | HEAP_IN_USE);',
     '\t\tblock_set(front, gap, (keep & HEAP_DIRTY) | HEAP_IN_USE);'),
    # Both pieces are counted live before one goes back.
    ('pieces-uncounted-in-the-pool', '\t\thome->live++;\n\t\tgReport.blocks_live++;',
     '\t\tgReport.blocks_live++;'),
    # A dedicated release goes back without touching the pool path.
    ('dedicated-falls-through', '\t\tregion_give_back(r);\n\t\treturn;\n\t}\n\n\theap_pool_t *p = pool_of(r);',
     '\t\tregion_give_back(r);\n\t}\n\n\theap_pool_t *p = pool_of(r);'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / HEAP
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {HEAP} ({text.count(old)})', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=180)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {HEAP} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h):\d+:\d+: error:', body) is None and 'undefined reference' not in body
    if not built:
        unbuilt.append(name)
        verdict = 'did not build'
    elif code != 0:
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nheap mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
sys.exit(0 if not missed and not unbuilt else 1)
