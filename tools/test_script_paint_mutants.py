#!/usr/bin/env python3
"""One deliberate break at a time to the paint that follows a script's change
(LAYOUT_NEEDS_N3.md): libflow's comparison of the shown tree with the next
(flow.c, flow_change_parts), the kept restyle's marks (restyle.inc) and
yonder's half (yonder.c: what the change log adds, the verdicts, the check),
against the scripts host's damage cases (tools/test_yonder_scripts_host.sh
with YONDER_DAMAGE_ONLY, test_yonder_damage.inc: each rebuild painted in
parts checked against the whole view to the pixel, and its verdict held to
the case's). A mutant is CAUGHT when the cases fail, MISSED when they pass,
which means a test is missing and gets written. A break that changes nothing
a rule promises is not in the list; the comment after it says which and
why. A build failure is neither.

The shape is test_cover_mutants.py's: each mutant is applied to the tree's
copy of its file, the cases are run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it from
a clean tree (or through tools/mutants_parallel.py), and never beside
another harness that edits the same tree; the per-mutant logs are in --logs.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/script-paint-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

FLOW = 'userland/libflow/flow.c'
RESTYLE = 'userland/libflow/restyle.inc'
YONDER = 'userland/apps/yonder/yonder.c'
CASES = 'YONDER_DAMAGE_ONLY=1 bash tools/test_yonder_scripts_host.sh'

mutants = [
    # libflow's comparison: what a pair draws.
    ('a-texts-bytes-not-compared', FLOW,
     '        return a->length == 0 || (a->text != NULL && b->text != NULL && os64_memcmp(a->text, b->text, a->length) == 0);',
     '        return true;'),
    ('a-style-not-compared', FLOW,
     '    if ((a->style == NULL) != (b->style == NULL) || (a->style != NULL && !f_style_equal(a->style, b->style)))\n        return false;',
     '    if ((a->style == NULL) != (b->style == NULL))\n        return false;'),
    # The moves inside a box.
    ('a-move-inside-a-box-not-marked', FLOW, '            if (!change_moved(&c, x, y))\n                break;',
     '            if (0 && !change_moved(&c, x, y))\n                break;'),
    ('a-positioned-box-inside-a-move-allowed', FLOW,
     '        if (++*visits > CHANGE_WALK_MAX || d->frame != p->frame || d->positioned || d->fixed || d->stacked)',
     '        if (++*visits > CHANGE_WALK_MAX || d->frame != p->frame || d->fixed)'),
    ('a-move-marked-by-its-box-alone', FLOW,
     '    *at = moved(l->box->overflow, o.x, o.y);\n    *at = grown(*at, l->box->ink);      // what it paints past it',
     '    *at = moved(l->box->rect, o.x, o.y);\n    *at = grown(*at, l->box->ink);      // what it paints past it'),
    # A group's change: everything inside it, not its own box (Fable's).
    ('an-opacity-marks-only-its-own-box', FLOW,
     '        os64_gui_rect_t in = bounds != NULL ? bounds[k] : group_bounds(t, k, scroll, &end);\n'
     '        os64_gui_rect_t r = space_rect(l->box->frame, in, scroll);\n'
     '        if (r.w > 0 && r.h > 0)\n            mark(ctx, l->box, r);\n'
     '        ok = f_map_put(&grouped, l->box->node, (void *)l);',
     '        (void)end;\n'
     '        os64_gui_rect_t r = flow_box_doc_rect(l->box, scroll);\n'
     '        if (r.w > 0 && r.h > 0)\n            mark(ctx, l->box, r);\n'
     '        ok = f_map_put(&grouped, l->box->node, (void *)l);'),
    # An element with no box, drawn by a replaced box.
    ('a-boxless-elements-style-not-compared', FLOW,
     '        if ((a == NULL) == (b == NULL) && (a == NULL || f_style_equal(&a->style, &b->style)))\n            continue;',
     '        if (a != b || 1)\n            continue;'),
    ('nothing-draws-a-boxless-element', FLOW,
     '        if (b->kind == FLOW_BOX_REPLACED || (b->kind == FLOW_BOX_ATOMIC && b->first == NULL))\n            *out = a;',
     '        if (b->kind == FLOW_BOX_REPLACED && 0)\n            *out = a;'),
    ('the-caller-elements-not-marked', FLOW,
     '        if (touched[i] != NULL && !change_drawn(&c, was, touched[i]))',
     '        if (touched[i] != NULL && 0 && !change_drawn(&c, was, touched[i]))'),
    ('the-canvas-not-refused-in-a-new-tree', FLOW, '    if (may_own_canvas(c->now, n)) {',
     '    if (c->now == NULL) {'),
    ('an-empty-box-given-children-not-compared', FLOW,
     '        bool down = x->first != NULL || y->first != NULL;', '        bool down = x->first != NULL;'),
    ('a-shape-another-svg-draws-marked-in-its-own', FLOW,
     '    if (n->ns == OS64_HTML_NS_SVG && svg_referable(doc, n))\n        return false;',
     '    if (n->ns == OS64_HTML_NS_SVG && svg_referable(doc, n) && 0)\n        return false;'),
    ('a-shape-in-a-hidden-svg-marks-nothing', FLOW, '    return *out != NULL || n->ns != OS64_HTML_NS_SVG;',
     '    return true;'),
    ('a-kept-transform-that-moves-the-page-kept', RESTYLE,
     '        if (marking && moved && marked == FLOW_PARTS_MARKED)', '        if (marking && moved && 0)'),
    # The kept restyle's marks: before the write and after it.
    ('a-kept-restyle-marks-nothing-before', RESTYLE,
     '    if (marking && marked == FLOW_PARTS_MARKED)\n'
     '        marked = parts_in(tree, &changed, marks->scroll, marks->mark, marks->ctx, NULL, 0);\n'
     '    for (int32_t i = 0; i < k.n; i++) {',
     '    for (int32_t i = 0; i < k.n; i++) {'),
    ('a-kept-restyle-marks-nothing-after', RESTYLE,
     '        pending_open(tree);\n'
     '        if (marking && marked == FLOW_PARTS_MARKED)\n'
     '            marked = parts_in(tree, &changed, marks->scroll, marks->mark, marks->ctx, NULL, 0);',
     '        pending_open(tree);'),
    ('a-kept-boxless-element-not-marked', RESTYLE,
     '        why = !drawn_by(tree, n, &by) ? FLOW_PARTS_NO_BOX : keep_change(tree, changed, by, all);',
     '        why = !drawn_by(tree, n, &by) ? FLOW_PARTS_NO_BOX : keep_change(tree, changed, NULL, all);'),
    # yonder's half: what the change log adds.
    ('an-unlisted-attribute-marks-nothing', YONDER,
     '        if (entry->kind == OS64_HTML_CHANGE_ATTR ? attr_styles_only(entry->attr_name) && !drawn : !drawn)',
     '        if (entry->kind == OS64_HTML_CHANGE_ATTR ? attr_styles_only(entry->attr_name) || 1 : !drawn)'),
    ('a-pictures-source-marks-nothing', YONDER,
     '            at->ns != OS64_HTML_NS_HTML || at->name == NULL                            ? NULL',
     '            at->ns != OS64_HTML_NS_HTML || at->name == NULL || 1                       ? NULL'),
    ('a-widget-over-the-view-compared', YONDER, '            if (!g.fw[i]->w->hidden)\n                left_out[nleft++] = g.fw[i]->w->bounds;',
     '            if (0 && !g.fw[i]->w->hidden)\n                left_out[nleft++] = g.fw[i]->w->bounds;'),
    ('a-kept-tree-not-compared-with-itself', YONDER,
     '    else if (next->tree == p->tree && s_change.kept_parts != FLOW_PARTS_MARKED)',
     '    else if (next->tree == p->tree)'),
    ('parts-past-half-the-view-kept', YONDER,
     '    if (why == NULL && (int64_t)reach.w * reach.h * 2 > (int64_t)s_change.view.w * s_change.view.h)',
     '    if (why == NULL && 0)'),
    # The svgs the cascade restyled, which yonder's adapter paints from it.
    ('the-svgs-the-cascade-restyled-not-marked', YONDER,
     '        else if (!change_svgs(p, next->tree, next->cascade, touched, &ntouched, CHANGE_TOUCHED))',
     '        else if (0 && !change_svgs(p, next->tree, next->cascade, touched, &ntouched, CHANGE_TOUCHED))'),
    ('a-restyled-definition-marks-only-its-own-svg', YONDER,
     '        all = definition && svg_restyled(c, e, since, fresh);', '        all = definition && 0 && svg_restyled(c, e, since, fresh);'),
    # The paint walk: a box is culled by all it paints, its ink too.
    ('a-shadow-past-its-box-culled', FLOW, '    return meets(grown(b->overflow, b->ink), view);',
     '    return meets(b->overflow, view);'),
    # The check: a rebuild that marked nothing is checked too.
    ('a-rebuild-that-marked-nothing-unchecked', YONDER,
     '        if (whole == NULL && s_damage_check != 0 && ++s_damage_counted % s_damage_check == 0) {',
     '        if (whole == NULL && s_change.n > 0 && s_damage_check != 0 && ++s_damage_counted % s_damage_check == 0) {'),
]

# Not run, because it changes nothing a rule promises, or nothing a case can
# tell apart:
# - the page's extent compared (flow_change_parts' layout->width/height):
#   in-flow content that grows the page grows the root's own box, which the
#   walk refuses as MOVED first, and out-of-flow content that reaches past
#   it moves a positioned box, which moved_within refuses; the compare is a
#   guard for a scroll bar's length, which no case reaches by itself.
# - an exempt attribute on a boxless element marked (change_touched's
#   `!drawn`): a class or style on an svg's shape reaches the paint through
#   its style, which the boxless elements' compare finds too.
# - the canvas refused in place (keep_change's may_own_canvas): every change
#   the cases make to the root's or the body's style is laid out afresh,
#   N1 keeping none of them, so the guard stands behind the new tree's,
#   which the-canvas-not-refused-in-a-new-tree breaks.
# - an svg's ancestors asked whether they were restyled (change_svgs): the
#   cascade restyles everything under an element whose matching changed,
#   so an svg under one is found by its own shapes too; the ancestors are
#   asked for a cascade that one day marks less.
# - the parts past the room (`over`): 64 parts take more than the fixtures
#   make, and a part past them refuses, the safe direction.
# - text or children changed in an element drawn from the document
#   (change_touched's `!drawn` for them): libsvg draws no text, and a shape
#   added to an svg is a new style record the boxless elements' compare
#   finds first; the rule is for what draws from the document later.
# - a restyle that wrote the tree and then declined (`s_change.wrote`): it
#   declines so only past an anonymous-box map's room, which no fixture can
#   fill without running the host out of memory.
# - the view scrolled with the change (script_rebuild's clamp): what moves
#   the page's extent is refused before it, in both trees and in place; the
#   clamp is the guard behind them.
# - the check run before the pane's canvas label is released: the label is
#   made only for a page with scripts but without the modern switch, which
#   the damage cases do not run.

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

print(f'\nscript paint mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
