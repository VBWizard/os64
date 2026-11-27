#!/usr/bin/env python3
"""One deliberate break at a time to the Fullscreen API's top layer in
yonder (VIDEO.md § 9, V8 slice 2): paint.c's top layer (the backdrop, then
the element's boxes alone), libflow's hit walk kept inside one box
(flow_hit_within), the user-agent rules that make the element cover the
view (style.c), its containing block (boxes.c, style.c's anchors and
flow_clip_parent) and its ancestors' opacity (style.c), yonder's pointer
and widgets through them (page_hit, page_covered_at), and its hidden rows
out of keyboard traversal (fs_rows_keys), against the scripts host's planes case for a
full-screen container inside a transformed, clipping wrapper
(tools/test_yonder_scripts_host.sh with YONDER_COVER_ONLY,
test_yonder_planes.inc, plane_fullscreen_container) and, where a mutant
names them, libflow's cases.
A mutant is CAUGHT
when the cases fail, MISSED when they pass, which means a test is missing
and gets written. A break that changes nothing a rule promises is not in
the list; the comment after it says which and why. A build failure is
neither.

The shape is test_cover_mutants.py's: each mutant is applied to the tree's
copy of its file, the cases are run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it
through tools/mutants_parallel.py with `--with kernel` (yonder's plane
blend builds the kernel's compositing source), never beside another
harness that edits the same tree; the per-mutant logs are in --logs.
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
BOXES = 'userland/libflow/boxes.c'
CASES = 'YONDER_COVER_ONLY=1 bash tools/test_yonder_scripts_host.sh'
# libflow's own cases (test_libflow_scroll.inc's top layer: the element
# under a transformed, filtered, clipping wrapper), for what the scripted
# page cannot see; a mutant that names no cases runs CASES.
FLOW_CASES = 'bash tools/test_libflow_host.sh'

mutants = [
    # The paint: the top layer at all, its backdrop, and only its boxes.
    ('the-top-layer-never-applies', PAINT, '    if (top_applies(tree)) {', '    if (0 && top_applies(tree)) {'),
    ('no-backdrop', PAINT, '        if (!cover_inside)\n            fill(', '        if (0)\n            fill('),
    ('every-box-painted', PAINT, '    if (in_top(t, b))\n        paint_box(t->p, b);', '    paint_box(t->p, b);'),
    ('culled-through-an-ancestors-matrix', PAINT, 'group_inside(t->p, b) : FLOW_INSIDE_ALL;',
     'group_inside(t->p, b) : FLOW_INSIDE_VIEW;'),
    # A video in the element is a cover whatever groups are round the element.
    ('an-outside-group-unseats-the-cover', YONDER, '        laid += inside;', '        laid++;'),
    ('a-cover-inside-not-seen', PAINT, '        bool cover_inside = cover != NULL && t.top != NULL && in_top(&t, cover);',
     '        bool cover_inside = false;'),
    # The pointer: kept inside the element.
    ('the-hit-not-kept-inside', FLOW, '        if (top != NULL && !within(b, top))\n            continue;',
     '        if (top != NULL && !within(b, top) && 0)\n            continue;'),
    ('a-widget-outside-not-covered', YONDER, '    if (!fs_layer())\n        return flow_box_covered_at(g.page.tree, b, x, y, scroll);',
     '    if (1)\n        return flow_box_covered_at(g.page.tree, b, x, y, scroll);'),
    ('yonder-hits-the-whole-page', YONDER, '    return top != NULL ? flow_hit_within(g.page.tree, x, y, scroll, top) : NULL;',
     '    return top != NULL ? flow_hit(g.page.tree, x, y, scroll) : NULL;'),
    # The top layer's containing block: the initial one, whatever is round it.
    ('anchored-by-its-ancestors', BOXES, 'up == NULL || f_top_layer(el) ? NULL', 'up == NULL ? NULL'),
    ('a-fixed-box-inside-anchored-outside', STYLE, 'up != NULL && !f_top_layer(n) ? up->fixed_anchor',
     'up != NULL ? up->fixed_anchor', FLOW_CASES),
    ('the-clip-parent-an-ancestor', FLOW, '    if (f_top_layer(node))\n        return NULL;\n', '', FLOW_CASES),
    # Painted whatever its ancestors' opacity.
    ('hidden-by-an-ancestors-opacity', STYLE, '(up != NULL && up->transparent && !f_top_layer(n))',
     '(up != NULL && up->transparent)', FLOW_CASES),
    # The hidden rows out of keyboard traversal.
    ('the-rows-stay-in-tab', YONDER, '    if (!staged)\n        fs_rows_keys(s_fs.active);', '    if (!staged)\n        fs_rows_keys(false);'),
    # The user-agent rules.
    ('no-user-agent-rules', STYLE, '        if (f_top_layer(n))\n            fullscreen_rules(c, n, &sp);',
     '        if (0)\n            fullscreen_rules(c, n, &sp);'),
]

# Not run, because no fixture can reach it or the pixels are the same:
# - top_applies' document test (a frame's or an SVG document's tree while
#   the page is full screen): no scripted fixture paints a frame's document
#   while its parent is full screen.
# - object-fit: contain for the element (style.c): the case's element is a
#   container, so the rule never meets a replaced element; the video itself
#   full screen is the player widget's slice.
# - the walk begun at the root rather than at the element: in_top hands
#   over the same boxes, so the pixels are the same and only the time is not.
# - top_open's and top_close's in_top guards, one at a time: each alone
#   leaves the painter's group opens and closes unpaired, a fault of its
#   own rather than the rule they keep together (top_inside's answer for
#   a group outside the element is run, above).
# - flow_box_covered_within's early answer for a box outside `top`: the hit
#   kept inside `top` gives the same answer wherever the element covers the
#   view, which its user-agent rules make it do.
# - yonder's leaving when the page goes or its scripts are retired
#   (fs_page_going, fs_turn's first check): no scripted fixture navigates or
#   retires a host under a full-screen element; QEMU's pages prove them.
# - libgarb's :fullscreen (select.c): held by the DOM host suite's
#   --fullscreen probe, which asserts `v.matches(':fullscreen')` and its
#   prefixed twin while v is full screen and neither after; that probe links
#   libgarb as an object the DOM harness does not mutate.

env = dict(os.environ)
caught, missed, unbuilt = [], [], []
for name, path_rel, old, new, *own in mutants:
    cases = own[0] if own else CASES
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
                result = subprocess.run(['bash', '-c', cases], cwd=root, stdout=out, stderr=out, env=env, timeout=1500)
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
