#!/usr/bin/env python3
"""One deliberate break at a time to libui's dirty list (userland/libos64/
ui.c: the swallow, the hand clear, covers, the per-rectangle walk, the
panel's part, the box, the collapse count), against
tools/test_ui_damage_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_damage_mutants.py's: each mutant is applied to the tree's
copy of ui.c, the suite is run, and the file is put back from the bytes read
before the change, checked byte for byte, whatever happens. Run it from a
clean tree, and never beside another harness that edits the same tree; the
per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/ui-damage-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

UI = 'userland/libos64/ui.c'
CASES = 'bash tools/test_ui_damage_host.sh'

mutants = [
    ('a-mark-swallows-nothing', '\t\tif (!rect_holds(r, ui->damage[i]))\n', '\t\tif (true)\n'),
    ('a-hand-clear-keeps-the-list', '\tif (!ui->any_dirty)\n\t\tui->damage_count = 0;\n', ''),
    ('covers-reads-a-clean-list', 'ui->any_dirty && i < ui->damage_count', 'i < ui->damage_count'),
    ('a-part-class-is-painted-once', '\t\tif (w->cls->paints_part) {', '\t\tif (false) {'),
    ('every-class-is-painted-per-rectangle', '\t\tif (w->cls->paints_part) {', '\t\tif (true) {'),
    ('a-whole-paint-keeps-the-last-dirty', '\t\t\tui->dirty = met;\n', ''),
    ('the-panel-fills-its-bounds', '\tos64_draw_fill_rect(&ctx->surf, part, t->panel_bg);',
     '\tos64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);'),
    ('the-panel-outlines-its-bounds', '\t\tif (os64_rect_intersect(edges[i], part, &edge))',
     '\t\tif ((edge = edges[i]), true)'),
    ('the-panel-paints-whole', '\t0, 0, 0, 0, 0, 0, true };', '\t0, 0, 0, 0, 0, 0, false };'),
    ('the-box-is-the-first-rectangle', '\t\t\t*damage = os64_rect_union(*damage, list[i]);',
     '\t\t\t(void)list[i];'),
    ('a-collapse-is-not-counted', '\t\tui->damage_collapses++;', '\t\tui->damage_collapses += 0;'),
    ('a-render-leaves-the-list', '\tclean(ui);\n\treturn count;', '\treturn count;'),
    ('an-empty-mark-is-dirty', '\tif (w == (os64_ui_widget_t *)0 || os64_rect_is_empty(w->bounds))',
     '\tif (w == (os64_ui_widget_t *)0)'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / UI
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {UI}', file=sys.stderr)
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
            print(f'{name}: {UI} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nui damage mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
