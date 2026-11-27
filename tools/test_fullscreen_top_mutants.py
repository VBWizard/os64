#!/usr/bin/env python3
"""One deliberate break at a time to the Fullscreen API's top layer in
yonder (VIDEO.md § 9, V8 slice 2): paint.c's top layer (the backdrop, then
the element's boxes alone), libflow's hit walk kept inside one box
(flow_hit_within), the user-agent rules that make the element cover the
view (style.c), and yonder's pointer through them (page_hit), against the
scripts host's planes case for a full-screen container
(tools/test_yonder_scripts_host.sh with YONDER_COVER_ONLY,
test_yonder_planes.inc, plane_fullscreen_container). A mutant is CAUGHT
when the cases fail, MISSED when they pass, which means a test is missing
and gets written. A break that changes nothing a rule promises is not in
the list; the comment after it says which and why. A build failure is
neither.

The shape is test_cover_mutants.py's: each mutant is applied to the tree's
copy of its file, the cases are run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it
through tools/mutants_parallel.py, never beside another harness that edits
the same tree; the per-mutant logs are in --logs.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/fullscreen-top-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PAINT = 'userland/apps/yonder/paint.c'
YONDER = 'userland/apps/yonder/yonder.c'
FLOW = 'userland/libflow/flow.c'
STYLE = 'userland/libflow/style.c'
CASES = 'YONDER_COVER_ONLY=1 bash tools/test_yonder_scripts_host.sh'

mutants = [
    # The paint: the top layer at all, its backdrop, and only its boxes.
    ('the-top-layer-never-applies', PAINT, '    if (top_applies(tree)) {', '    if (0 && top_applies(tree)) {'),
    ('an-anonymous-root-hides-the-document', PAINT, '    while (b != NULL && b->node == NULL)\n        b = b->first;\n',
     '    (void)0;\n'),
    ('no-backdrop', PAINT, '        if (!cover_inside)\n            fill(', '        if (0)\n            fill('),
    ('every-box-painted', PAINT, '    if (in_top(t, b))\n        paint_box(t->p, b);', '    paint_box(t->p, b);'),
    ('the-walk-from-the-root', PAINT, '&v, cover_inside ? cover : t.top, &found);',
     '&v, cover_inside ? cover : flow_root(tree), &found);'),
    ('a-cover-inside-not-seen', PAINT, '        bool cover_inside = cover != NULL && t.top != NULL && in_top(&t, cover);',
     '        bool cover_inside = false;'),
    # The pointer: kept inside the element.
    ('the-hit-not-kept-inside', FLOW, '        if (top != NULL && !within(b, top))\n            continue;',
     '        if (0)\n            continue;'),
    ('yonder-hits-the-whole-page', YONDER, '    return top != NULL ? flow_hit_within(g.page.tree, x, y, scroll, top) : NULL;',
     '    return flow_hit(g.page.tree, x, y, scroll);'),
    # The user-agent rules.
    ('no-user-agent-rules', STYLE, '        if (fullscreen(n, root))\n            fullscreen_rules(c, n, &sp);',
     '        if (0)\n            fullscreen_rules(c, n, &sp);'),
]

# Not run, because no fixture can reach it or the pixels are the same:
# - top_applies' document test (a frame's or an SVG document's tree while
#   the page is full screen): no scripted fixture paints a frame's document
#   while its parent is full screen.
# - object-fit: contain for the element (style.c): the case's element is a
#   container, so the rule never meets a replaced element; the video itself
#   full screen is the player widget's slice.
# - the group verbs' in_top guards (top_open, top_close, top_inside): an
#   open and its close come for the same box, so a guard broken on one
#   pairs nothing new; the container case has no group outside the element.
# - libgarb's :fullscreen (select.c): held by the DOM host suite's
#   --fullscreen probe (`:fullscreen matches v alone`), which links libgarb
#   as an object the DOM harness does not mutate; it was broken by hand
#   once and the probe failed.

env = dict(os.environ)
caught, missed, unbuilt = [], [], []
for name, path_rel, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / path_rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {path_rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    changed = text.replace(old, new)
    log = args.logs / f'{name}.log'
    try:
        path.write_text(changed)
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, env=env, timeout=1500)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {path_rel} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nfullscreen top-layer mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
