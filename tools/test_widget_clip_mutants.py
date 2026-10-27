#!/usr/bin/env python3
"""One deliberate break at a time to where yonder draws a page's controls:
POSITION.md ruling 9's amendment (libflow's flow_box_covered passes a box
that draws nothing at the control's centre unless it is the control or
inside it: draws_at) and the widgets
clipped to the page view (yonder's forms_place), against the cases that
state each rule: libflow's host and the scripted page's. libui's half,
the clipped widget's cut paint and hit, is test_ui_damage_mutants.py's. A
mutant is CAUGHT when its cases fail, MISSED when they pass, which means a
test is missing and gets written. A build failure is neither.

The shape is test_ui_damage_mutants.py's, with each mutant run against the
harness that holds its rule: each is applied to the worktree's copy of its
file, the cases are run, and the file is put back from the bytes read
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/widget-clip-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

FLOW = 'userland/libflow/flow.c'
YONDER = 'userland/apps/yonder/yonder.c'
LIBFLOW = 'bash tools/test_libflow_host.sh'
WINDOW = 'bash tools/test_yonder_scripts_host.sh'

# (name, file, old, new, cases). Each breaks one rule the design states.
mutants = [
    ('an-invisible-box-draws', FLOW, '    if (b->unpainted)\n        return false;\n    if (b->kind == FLOW_BOX_TEXT',
     '    if (b->kind == FLOW_BOX_TEXT', LIBFLOW),
    ('an-invisible-control-is-passed', FLOW,
     '    if (t->painted != NULL && !within(b, t->painted) && holds(b->rect, x, y) && !draws_at(b, x, y))',
     '    if (t->painted != NULL && (!within(b, t->painted) || true) && holds(b->rect, x, y) && !draws_at(b, x, y))',
     LIBFLOW),
    ('every-box-draws-everywhere', FLOW,
     '    return x < (int64_t)b->rect.x + left || y < (int64_t)b->rect.y + top ||',
     '    return true || x < (int64_t)b->rect.x + left || y < (int64_t)b->rect.y + top ||', LIBFLOW),
    ('a-border-draws-nothing', FLOW,
     '    return x < (int64_t)b->rect.x + left || y < (int64_t)b->rect.y + top ||',
     '    return false || (int64_t)b->rect.x + left < 0 || y < (int64_t)b->rect.y + top - (int64_t)1e12 ||',
     LIBFLOW),
    ('a-background-draws-nothing', FLOW, '    if (s->has_background || flow_background_has_image(s))',
     '    if (flow_background_has_image(s))', LIBFLOW),
    ('a-background-picture-draws-nothing', FLOW, '    if (s->has_background || flow_background_has_image(s))',
     '    if (s->has_background)', LIBFLOW),
    ('text-draws-nothing', FLOW,
     '    if (b->kind == FLOW_BOX_TEXT || b->kind == FLOW_BOX_MARKER || b->kind == FLOW_BOX_REPLACED ||',
     '    if (b->kind == FLOW_BOX_MARKER || b->kind == FLOW_BOX_REPLACED ||', LIBFLOW),
    ('covered-asks-the-pointer', FLOW, '                                 scroll, NULL, box);',
     '                                 scroll, NULL, NULL);', LIBFLOW),
    ('the-pointer-passes-invisible-boxes', FLOW, '    return hit_at(tree, x, y, scroll, cuts, NULL);',
     '    return hit_at(tree, x, y, scroll, cuts, (const flow_box_t *)(uintptr_t)1);', LIBFLOW),
    ('a-half-seen-control-is-hidden', YONDER,
     '        bool shown = x < (int64_t)v.x + v.w && y < (int64_t)v.y + v.h && x + at.w > v.x && y + at.h > v.y &&',
     '        bool shown = x >= v.x && y >= v.y && x + at.w <= (int64_t)v.x + v.w && y + at.h <= (int64_t)v.y + v.h &&',
     WINDOW),
    ('an-unseen-control-is-shown', YONDER,
     '        bool shown = x < (int64_t)v.x + v.w && y < (int64_t)v.y + v.h && x + at.w > v.x && y + at.h > v.y &&',
     '        bool shown = (x < (int64_t)v.x + v.w || true) && y < (int64_t)v.y + v.h + 1000 && x + at.w > v.x &&'
     ' y + at.h > v.y &&', WINDOW),
    ('a-widget-is-not-clipped', YONDER, '        fw->w->clipped = true;\n', '', WINDOW),
    ('a-widget-is-clipped-elsewhere', YONDER, '        fw->w->clip = v;\n', '', WINDOW),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, cases in mutants:
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
                result = subprocess.run(['bash', '-c', cases], cwd=root, stdout=out, stderr=out, timeout=900)
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

print(f'\nwidget clip mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
