#!/usr/bin/env python3
"""One deliberate break at a time to the scaler's columns and rows
(userland/apps/yonder/scale.c § THE COLUMNS AND THE ROWS: a column's and a
row's source position worked out once, the runs of columns, the paired
halves of a premultiplied mix, the tile's wrap), against
tools/test_yonder_host.sh, whose scale_reference_cases hold it to the
scaler as it was, to the bit: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_yonder_filter_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
from a clean tree, and never beside another harness that edits the same
tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-scale-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

SCALE = 'userland/apps/yonder/scale.c'
CASES = 'bash tools/test_yonder_host.sh'

mutants = [
    ('the-column-weight-one-unit-off', '    t.next = t.i + 1 < n ? t.i + 1 : t.i;\n',
     '    t.next = t.i + 1 < n ? t.i + 1 : t.i;\n    t.f += t.f != 0 && t.f < 255;\n'),
    ('the-last-tap-clamped-wrong', 't.next = t.i + 1 < n ? t.i + 1 : t.i;',
     't.next = t.i + 1 < n ? t.i + 1 : (t.i > 0 ? t.i - 1 : 0);'),
    ('the-row-from-the-next-row', 'Tap r = tap_at(v, sh, box.h);', 'Tap r = tap_at(v + 1, sh, box.h);'),
    ('the-tiled-row-from-the-next-row', 'Tap r = tap_at(v, sh, tile.h);', 'Tap r = tap_at(v + 1, sh, tile.h);'),
    ('a-run-counted-from-the-first', 'uint32_t u = (uint32_t)((int64_t)cx + k - box.x);',
     'uint32_t u = (uint32_t)((int64_t)x0 + k - box.x);'),
    ('the-halves-carry', 'ar += ((x >> 24) << 32 | ((x >> 16) & 0xff)) * w[k];',
     'ar += ((x >> 24) << 16 | ((x >> 16) & 0xff)) * w[k];'),
    ('the-tile-never-wraps', '            if (++u == (uint32_t)tile.w)\n                u = 0;\n',
     '            if (++u == (uint32_t)tile.w + 1)\n                u = 0;\n'),
    ('the-nearest-column-not-its-middle', 'at[k] = (uint32_t)(((int64_t)u * 2 + 1) * sw',
     'at[k] = (uint32_t)(((int64_t)u * 2) * sw'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / SCALE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {SCALE}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=600)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {SCALE} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nyonder scale mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
