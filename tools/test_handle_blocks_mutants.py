#!/usr/bin/env python3
"""One deliberate break at a time to the handle table's arithmetic and counts
(kernel/src/handle_blocks.c: where a number lives, the room after k blocks,
the counts /proc/<pid>/handles reports), against
tools/test_handle_blocks_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_bt_acl_mutants.py's: each mutant is applied to the tree's
copy of the file, the suite is run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it
through tools/mutants_parallel.py with `--with kernel` (the runner's copies
do not carry the kernel's sources unless asked); the per-mutant logs are in
--logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/handle-blocks-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

BLOCKS = 'kernel/src/handle_blocks.c'
CASES = 'bash tools/test_handle_blocks_host.sh'

mutants = [
    ('block-zero-sized-as-the-next', '\treturn k == 0 ? OS64_TASK_HANDLES : (uint32_t)OS64_TASK_HANDLES << (k - 1);',
     '\treturn k == 0 ? OS64_TASK_HANDLES * 2 : (uint32_t)OS64_TASK_HANDLES << (k - 1);'),
    ('blocks-do-not-double', '\treturn k == 0 ? OS64_TASK_HANDLES : (uint32_t)OS64_TASK_HANDLES << (k - 1);',
     '\treturn k == 0 ? OS64_TASK_HANDLES : (uint32_t)OS64_TASK_HANDLES << (k - 1 + (k > 2));'),
    ('a-block-starts-a-block-late', '\treturn k == 0 ? 0 : (uint32_t)OS64_TASK_HANDLES << (k - 1);',
     '\treturn k == 0 ? 0 : (uint32_t)OS64_TASK_HANDLES << k;'),
    ('the-room-one-block-short', '\treturn (uint32_t)OS64_TASK_HANDLES << added;',
     '\treturn added ? (uint32_t)OS64_TASK_HANDLES << (added - 1) : OS64_TASK_HANDLES;'),
    ('a-negative-number-has-a-place', '\tif (n < 0 || n >= (int64_t)TASK_HANDLES_CEILING)',
     '\tif (n >= (int64_t)TASK_HANDLES_CEILING)'),
    ('the-ceiling-has-a-place', '\tif (n < 0 || n >= (int64_t)TASK_HANDLES_CEILING)',
     '\tif (n < 0 || n > (int64_t)TASK_HANDLES_CEILING)'),
    ('block-zero-one-short', '\tif (n < OS64_TASK_HANDLES) {',
     '\tif (n < OS64_TASK_HANDLES - 1) {'),
    ('the-block-one-too-high', '\tunsigned k = 32u - (unsigned)__builtin_clz(ratio);',
     '\tunsigned k = 33u - (unsigned)__builtin_clz(ratio);'),
    ('the-slot-from-the-wrong-start', '\t*slot = (uint32_t)n - handle_block_first(k);',
     '\t*slot = (uint32_t)n - handle_block_first(k - 1);'),
    ('held-not-raised', '\tuint32_t held = __atomic_add_fetch(&c->held, 1, __ATOMIC_RELAXED);',
     '\tuint32_t held = __atomic_load_n(&c->held, __ATOMIC_RELAXED) + 1;'),
    ('most-never-moves', '\twhile (held > most &&',
     '\twhile (held > most && 0 &&'),
    ('most-follows-held-down', '\twhile (held > most &&\n\t       !__atomic_compare_exchange_n(&c->most, &most, held, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {\n\t}',
     '\t__atomic_store_n(&c->most, held, __ATOMIC_RELAXED);\n\t(void)most;'),
    ('a-free-entry-not-given-back', '\t__atomic_sub_fetch(&c->held, 1, __ATOMIC_RELAXED);',
     '\t(void)c;'),
    ('a-refusal-not-counted', '\t__atomic_add_fetch(&c->failed, 1, __ATOMIC_RELAXED);',
     '\t(void)c;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / BLOCKS
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {BLOCKS}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=120)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {BLOCKS} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nhandle blocks mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
