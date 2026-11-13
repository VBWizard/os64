#!/usr/bin/env python3
"""One deliberate break at a time to yonder's warp (userland/apps/yonder/
warp.c; PILE3.md § Transforms, the painter) — the fixed-point step along a
row, the stretch of a row that can reach the sheets, the skip of nothing
painted, the four pixels' weights and the lay — against its harness,
tools/test_yonder_warp_host.sh. A mutant is CAUGHT when the harness fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_cliptext_mutants.py's, which breaks the warp's masked lay:
each mutant is applied to the worktree's copy of the file, the cases are
run, and the file is put back from the bytes read before the change, checked
byte for byte, whatever happens. Run it from a clean tree, and never beside
another harness that edits the same tree; the per-mutant logs are in --logs.

Left out because nothing can tell them apart from the code: working every
row pixel by pixel instead of stepping it, and leaving a row's stretch
uncut — both are only slower — rounding the lay down instead of to the
nearest, which the contract's one level of slack absorbs, and a copy's
bounds on the sheets, which the pixels it is given — where the sheets land
— already keep it inside.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/warp-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

WARP = 'userland/apps/yonder/warp.c'
CASES = 'bash tools/test_yonder_warp_host.sh'

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The step along a row.
    ('the-step-is-held-to-16-bits', WARP,
     '    int64_t du = stepped ? fixed(inv[0]) : 0, dv = stepped ? fixed(inv[1]) : 0;',
     '    int64_t du = stepped ? fixed(inv[0]) & ~(int64_t)0xffff : 0, dv = stepped ? fixed(inv[1]) : 0;'),
    ('a-row-starts-where-it-was-not-cut', WARP,
     '            cx = from + 0.5L;',
     '            cx = ax0 + 0.5L;'),
    ('any-step-is-stepped', WARP,
     '    if (!(fabs(inv[0]) < 1048576.0 && fabs(inv[1]) < 1048576.0))\n        return false;',
     ''),
    ('a-far-centre-is-held-in-fixed-point', WARP,
     '            if (!near(u) || !near(v))\n                continue;',
     ''),
    # The stretch of a row that can reach the sheets.
    ('the-stretch-is-cut-too-close', WARP,
     '    double a = base + floor(k0) - 1, b = base + ceil(k1) + 2;',
     '    double a = base + floor(k0) + 1, b = base + ceil(k1) - 1;'),
    # What is nothing painted.
    ('black-alone-is-nothing-painted', WARP,
     '    return (b & 0xffffffu) == 0 && (w & 0xffffffu) == 0xffffffu;',
     '    (void)w;\n    return (b & 0xffffffu) == 0;'),
    # The four pixels and their weights.
    ('the-fourth-weight-is-dropped', WARP,
     '    uint32_t k0 = 65536 - k1 - k2 - k3;',
     '    uint32_t k0 = 65536 - k1 - k2;'),
    ('the-lower-pair-is-the-upper', WARP,
     '        pb[2] = s->black[next];',
     '        pb[2] = s->black[at];'),
    ('a-mixed-centre-is-not-moved-half-a-pixel', WARP,
     '    long double off = exact ? 0 : 0.5L;',
     '    long double off = 0;'),
    # The lay.
    ('a-stepped-row-ignores-the-opacity', WARP,
     '                if (exact ? take(s, U, V, B, W) : mix(s, U, V, B, W))\n'
     '                    row[x] = lay_px(B, W, row[x], alpha);\n            continue;',
     '                if (exact ? take(s, U, V, B, W) : mix(s, U, V, B, W))\n'
     '                    row[x] = lay_px(B, W, row[x], 255);\n            continue;'),
    ('the-backdrop-is-not-seen-through', WARP,
     '    uint64_t num = a * (255 * b + t * d) + (255 - a) * (255u * 65536u) * d;',
     '    uint64_t num = a * (255 * b + t * 0) + (255 - a) * (255u * 65536u) * d;'),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out,
                                        timeout=600)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {rel} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h|inc):\d+:\d+: error:', body) is None and 'undefined reference' not in body
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

print(f'\nwarp mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
