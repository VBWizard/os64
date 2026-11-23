#!/usr/bin/env python3
"""One deliberate break at a time to the canvas's revision
(userland/libcanvas/core.c, c_revised: a value no canvas has had, at every
reset and every draw that changes a pixel — the kept canvas pictures' key),
against tools/test_canvas_core.sh's revisions() and
tools/test_canvas_paths.sh's revisions_drawn(): a mutant is
CAUGHT when the suites fail, MISSED when they pass, which means a test is
missing and gets written. A build failure is neither.

The shape is test_yonder_scale_mutants.py's: each mutant is applied to the
tree's copy of the file, the suites are run, and the file is put back from
the bytes read before the change, checked byte for byte, whatever happens.
Run it from a clean tree, and never beside another harness that edits the
same tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/canvas-revision-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CORE = 'userland/libcanvas/core.c'
CASES = 'bash tools/test_canvas_core.sh && bash tools/test_canvas_paths.sh'

mutants = [
    ('not-revised-at-a-draw', '    if (d.x1 <= d.x0 || d.y1 <= d.y0) return;\n    c_revised(c);\n',
     '    if (d.x1 <= d.x0 || d.y1 <= d.y0) return;\n'),
    ('not-revised-at-a-reset', '    c->damage = w && h ? (draw2d_damage_t){0, 0, w, h} : (draw2d_damage_t){0};\n    c_revised(c);\n',
     '    c->damage = w && h ? (draw2d_damage_t){0, 0, w, h} : (draw2d_damage_t){0};\n'),
    ('one-count-a-canvas', 'c->revision = __atomic_add_fetch(&s_revisions, 1, __ATOMIC_RELAXED);',
     'c->revision = c->revision + 1 + 0 * s_revisions;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / CORE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {CORE}', file=sys.stderr)
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
            print(f'{name}: {CORE} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\ncanvas revision mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
