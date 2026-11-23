#!/usr/bin/env python3
"""One deliberate break at a time to the kept canvas pictures
(userland/apps/yonder/canvas_keep.c; canvas_keep.h says the rules), against
tools/test_yonder_host.sh, whose canvas_keep_cases hold every draw through
the keep to yonder_draw_surface's pixels and each rule to its counts: a
mutant is CAUGHT when the suite fails, MISSED when it passes, which means a
test is missing and gets written. A build failure is neither.

The shape is test_yonder_scale_mutants.py's: each mutant is applied to the
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-canvas-keep-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

KEEP = 'userland/apps/yonder/canvas_keep.c'
CASES = 'bash tools/test_yonder_host.sh'

mutants = [
    ('kept-in-the-first-paint', 'if (s->paint == k->paint)', 'if (s->paint == k->paint && 0)'),
    ('kept-on-the-first-draw',
     '        *s = (Seen){who, revision, k->paint, k->clock, box.w, box.h, premultiplied, false, 0, 0, 0, 0};\n'
     '        return yonder_draw_surface(dst, pitch, clip, box, smooth, source);\n',
     '        *s = (Seen){who, revision, k->paint - 1, k->clock, box.w, box.h, premultiplied, false, 0, 0, 0, 0};\n'),
    ('not-dropped-when-the-bitmap-changes', '            drop(k, cache, s);', '            (void)drop;'),
    ('found-at-another-revision', 'os64_memcpy(key + 4, &revision, 8);', 'os64_memset(key + 4, 0, 8);'),
    ('found-at-another-scale', 'os64_memcpy(key + 12, &w, 4);', 'os64_memset(key + 12, 0, 4);'),
    ('found-for-another-alpha-convention', 'key[2] = premultiplied;', 'key[2] = 0;'),
    ('kept-unsmoothed', "if (k == NULL || cache == NULL || who == NULL || revision == 0 || !smooth ||",
     "if (k == NULL || cache == NULL || who == NULL || revision == 0 ||"),
    ('laid-a-pixel-off', 'int32_t i = lx / KEEP_BLOCK - bx0, uin = lx % KEEP_BLOCK;',
     'int32_t i = lx / KEEP_BLOCK - bx0, uin = (lx + 1) % KEEP_BLOCK;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / KEEP
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {KEEP}', file=sys.stderr)
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
            print(f'{name}: {KEEP} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nyonder canvas keep mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
