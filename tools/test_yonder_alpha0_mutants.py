#!/usr/bin/env python3
"""One deliberate break at a time to the painter's groups laid at alpha 0
(userland/apps/yonder/paint.c, group_open and group_close: a group whose
alpha rounds to 0 paints nothing, open, inside and close, and the boxes
after it paint as ever), against tools/test_yonder_host.sh, whose
alpha0_group_paint_cases say what such a group paints: a mutant is CAUGHT
when the suite fails, MISSED when it passes, which means a test is missing
and gets written. A build failure is neither.

What the face owes a canvas inside such a group (yonder.c,
canvas_damage_mark) is proved on the glass by tools/pages/canvas-unseen.html
under --damage-check, not here: both sides of that check share the painter.

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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-alpha0-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PAINT = 'userland/apps/yonder/paint.c'
CASES = 'bash tools/test_yonder_host.sh'

mutants = [
    ('alpha-1-of-255-skipped', 'if (yonder_group_alpha(box) == 0) {', 'if (yonder_group_alpha(box) <= 1) {'),
    ('alpha-0-painted', 'if (yonder_group_alpha(box) == 0) {', 'if (yonder_group_alpha(box) == 0 && 0) {'),
    ('the-boxes-after-it-hidden-too', '        if (gs->hidden == d + 1)\n            gs->hidden = 0;\n',
     '        if (gs->hidden == d + 2)\n            gs->hidden = 0;\n'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / PAINT
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {PAINT}', file=sys.stderr)
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
            print(f'{name}: {PAINT} WAS NOT RESTORED', file=sys.stderr)
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
