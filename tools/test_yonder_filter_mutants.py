#!/usr/bin/env python3
"""One deliberate break at a time to a large blur made on a reduced copy
(userland/apps/yonder/filter.c § A large blur: the floor and the most, the
spread matched, the extended boxes, the copy's squares past the edges, each
pass kept inside the layer, the bilinear enlargement, the group's grid, what
the blur reads, and libflow's cull by it), against
tools/test_yonder_filter_host.sh (and tools/test_yonder_host.sh for the
cull): a mutant is
CAUGHT when the suite fails, MISSED when it passes, which means a test is
missing and gets written. A build failure is neither.

The shape is test_audio_stream_mutants.py's: each mutant is applied to the
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-filter-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

FILTER = 'userland/apps/yonder/filter.c'
CASES = 'bash tools/test_yonder_filter_host.sh'
FLOW = 'userland/libflow/flow.c'
PAINTER = 'bash tools/test_yonder_host.sh'

mutants = [
    # The floor and the most (the contract's table).
    ('the-floor-one-lower', 'if (k < BLUR_REDUCED_FLOOR)\n', 'if (k < BLUR_REDUCED_FLOOR - 1)\n'),
    ('the-floor-one-higher', 'if (k < BLUR_REDUCED_FLOOR)\n', 'if (k < BLUR_REDUCED_FLOOR + 1)\n'),
    ('the-most-sixteen', '#define BLUR_REDUCED_MOST 8\n', '#define BLUR_REDUCED_MOST 16\n'),
    # The spread matched.
    ('the-radius-not-divided', '/ ((double)s * s) / 3;', '/ 3;'),
    # (The reduction's own spread, (s^2 - 1) / 12, is not broken here: left
    # out it moves the deviation by a third of a percent, below what the
    # contract's 2 of 255 can see, so no suite of it could catch it.)
    ('the-enlargement-spread-forgotten', '(whole - (s * s - 1) / 12.0 - enlarge_spread(s))',
     '(whole - (s * s - 1) / 12.0 - 0 * enlarge_spread(s))'),
    ('the-end-weight-halved', 'long w = (long)floor(a * 256 + 0.5);', 'long w = (long)floor(a * 128 + 0.5);'),
    # The extended boxes.
    ('rows-without-the-end-weight',
     'uint64_t x = (uint64_t)in[c] * 256u + (uint64_t)(chan(below, 8 * c) + chan(above, 8 * c)) * a;',
     'uint64_t x = (uint64_t)in[c] * 256u + (uint64_t)(chan(below, 8 * c) + chan(above, 8 * c)) * 0;'),
    ('columns-without-the-end-weight',
     'uint64_t x = (uint64_t)in[q][c] * 256u + (uint64_t)(chan(pb, 8 * c) + chan(pa, 8 * c)) * a;',
     'uint64_t x = (uint64_t)in[q][c] * 256u + (uint64_t)(chan(pb, 8 * c) + chan(pa, 8 * c)) * 0;'),
    ('columns-forget-what-leaves', '                    if (leaving != NULL)\n                        in[q][c] -= chan(leaving[q], 8 * c);\n',
     '                    if (leaving != NULL && 0)\n                        in[q][c] -= chan(leaving[q], 8 * c);\n'),
    # The copy's squares past the edges, and each pass kept inside.
    ('no-square-before-the-edge', 'p0 = phase(grid.x, s) + s, q0 = phase(grid.y, s) + s;',
     'p0 = phase(grid.x, s), q0 = phase(grid.y, s);'),
    ('passes-not-kept-inside-the-rows', '        if (k < 2)\n            keep_inside(sm, rw, 1, rh, (size_t)rw, s, p0, w);\n', ''),
    ('passes-not-kept-inside-the-columns', '        if (k < 2)\n            keep_inside(sm, rh, (size_t)rw, rw, 1, s, q0, h);\n', ''),
    ('a-part-square-kept-whole', 'o |= ((chan(*p, c) * inside + (uint32_t)s / 2) / (uint32_t)s) << c;',
     'o |= chan(*p, c) << c;'),
    ('the-last-passes-kept-inside-too', '        if (k < 2)\n            keep_inside(sm, rw, 1,',
     '        if (k < 3)\n            keep_inside(sm, rw, 1,'),
    # The enlargement: bilinear, not the nearest square.
    ('widened-to-the-nearest', 'int32_t i0 = (at >> shift) - 1, f = at & (two - 1);',
     'int32_t i0 = (at >> shift) - 1, f = (at & (two - 1)) < s ? 0 : two;'),
    ('enlarged-to-the-nearest-row', 'int32_t j0 = (at >> shift) - 1, g = at & (two - 1);',
     'int32_t j0 = (at >> shift) - 1, g = (at & (two - 1)) < s ? 0 : two;'),
    # The grid is the group's.
    ('the-grid-is-the-layers', 'p0 = phase(grid.x, s) + s, q0 = phase(grid.y, s) + s;',
     'p0 = phase(grid.x * 0, s) + s, q0 = phase(grid.y * 0, s) + s;'),
    ('a-negative-grid-not-folded', 'return (at % s + s) % s;', 'return at % s < 0 ? 0 : at % s;'),
    # What it reads, which the painter grows a cut layer by.
    ('reads-no-farther-than-three-boxes', 'more += past > 0 ? past : 0;', 'more += past > 0 ? 0 : 0;'),
    ('reads-forget-the-enlargement', 'return d.s > 1 ? 3 * (d.k + 1) * d.s + 3 * d.s / 2 : 0;',
     'return d.s > 1 ? 3 * (d.k + 1) * d.s : 0;'),
    # libflow culls a filtered group's inside by what the blurs read.
    ('the-cull-without-the-reads', 'more += FLOW_FILTER_READS_MORE;', 'more += 0;', FLOW, PAINTER),
]

caught, missed, unbuilt = [], [], []
for mutant in mutants:
    name, old, new = mutant[:3]
    where, cases = mutant[3:] if len(mutant) > 3 else (FILTER, CASES)
    if args.only and args.only not in name:
        continue
    path = root / where
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {where}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', cases], cwd=root, stdout=out, stderr=out, timeout=300)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {where} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nyonder filter mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
