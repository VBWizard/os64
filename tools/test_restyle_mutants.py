#!/usr/bin/env python3
"""One deliberate break at a time to the restyle kept in the tree
(userland/libflow/restyle.inc, flow_restyle_in_place, the snapshot's
`structural` mark in reuse_snapshot.inc, ink_grown's outlines in flow.c, the
geometry check's scrolls in yonder.c, and a box that only moves: keep_move
in restyle.inc, move_box and the public build's relocation in flow.c, and
yonder telling the observers of it), against the scripts host's profile
cases (tools/test_yonder_scripts_host.sh with YONDER_PROFILE_ONLY, which run
restyle_kept_cases with every kept tree laid out whole beside it) and
libflow's own (tools/test_libflow_host.sh --restyle, restyle_in_place_cases,
which also hand the check trees that differ): a mutant
is CAUGHT when the cases fail, MISSED when they pass, which means a test is
missing and gets written. A break that changes nothing a rule promises is
not in the list; the comment after it says which and why. A build failure
is neither.

The shape is test_task_order_mutants.py's: each mutant is applied to the
tree's copy of its file, the cases are run, and the file is put back from
the bytes read before the change, checked byte for byte, whatever happens.
Run it from a clean tree, and never beside another harness that edits the
same tree; the per-mutant logs are in --logs.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/restyle-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

RESTYLE = 'userland/libflow/restyle.inc'
SNAPSHOT = 'userland/libflow/reuse_snapshot.inc'
FLOW = 'userland/libflow/flow.c'
YONDER = 'userland/apps/yonder/yonder.c'
CASES = 'bash tools/test_yonder_scripts_host.sh && bash tools/test_libflow_host.sh --restyle'

mutants = [
    # What changed since the tree.
    ('a-text-is-not-structure', SNAPSHOT, '            r->structural = true;', '            (void)0;'),
    ('structure-not-asked', RESTYLE, '    if (r->structural) {', '    if (0) {'),
    ('every-change-in-the-page', SNAPSHOT,
     '    return n == NULL || os64_html_shadow_including_root(n) == r->doc->document;',
     '    return n == NULL || os64_html_shadow_including_root(n) == r->doc->document || 1;'),
    ('no-change-in-the-page', SNAPSHOT,
     '    return n == NULL || os64_html_shadow_including_root(n) == r->doc->document;',
     '    return (n == NULL || os64_html_shadow_including_root(n) == r->doc->document) && 0;'),
    ('a-change-told-at-its-own-node', SNAPSHOT, '                r->structural_at = reuse_told_at(r, entry->target);',
     '                r->structural_at = reuse_told_at(r, entry->target) ? entry->target : entry->target;'),
    ('another-cascade-kept', RESTYLE, '                      : r->generation != garb_cascade_generation(env->cascade) ? "another cascade"',
     '                      : 0 ? "another cascade"'),
    # The verdict.
    ('geometry-kept', RESTYLE, '    } else if (!f_style_paint_only(&was->style, &is->style)) {', '    } else if (0) {'),
    # A box that only moves (N4: keep_move, move_box, public_box's relocation).
    ('a-relative-inline-taken-as-a-move', RESTYLE,
     '                    : box->kind == FLOW_BOX_SPAN || box->kind == FLOW_BOX_ATOMIC ? "a relative inline"\n',
     ''),
    ('a-stretched-box-taken', RESTYLE,
     '    if (why == NULL && out && across)\n', '    if (0 && across)\n'),
    ('sticky-taken', RESTYLE, '                    : b->position == FLOW_POSITION_STICKY ? "sticky"\n', ''),
    ('a-fixed-descendant-moved-with-it', FLOW,
     '    if (!root && b->out_of_flow && b->style->position == FLOW_POSITION_FIXED)\n        return "a fixed box inside";\n', ''),
    ('a-clip-moved-with-it', FLOW,
     '    if (f_box_clips(b, true) || f_box_clips(b, false) || b->style->legacy_clip_set)\n', '    if (0)\n'),
    ('the-move-skips-what-is-in-it', FLOW,
     '    for (const FBox *c = src->first; c != NULL && !bd->failed; c = c->next) {\n        if (!c->placed)\n            continue;\n',
     '    for (const FBox *c = src->first; c != NULL && !bd->failed; c = c->next) {\n        if (!c->placed || bd->relocate)\n            continue;\n'),
    ('the-moved-frame-s-anchor-kept', FLOW,
     '            f->box_x = bd->ox + src->x;\n            f->box_y = bd->oy + src->y;\n', ''),
    ('the-moved-box-s-matrix-not-rebuilt', FLOW,
     '            f->box_y = bd->oy + src->y;\n            transform_fill(f, src->style);\n',
     '            f->box_y = bd->oy + src->y;\n'),
    ('a-move-replaces-the-last', FLOW, '    m->dx += dx;\n    m->dy += dy;\n', '    m->dx = dx;\n    m->dy = dy;\n'),
    ('the-moved-boxes-round-it-forgotten', FLOW,
     '        if (pm != NULL) {\n            ox += pm->dx;\n            oy += pm->dy;\n        }\n', '        (void)pm;\n'),
    ('a-move-made-at-the-records-place', FLOW,
     '    if (moved != NULL) {\n        bd->ox += moved->dx;\n        bd->oy += moved->dy;\n    }\n', '    (void)moved;\n'),
    ('the-extent-not-reached-after-a-move', RESTYLE,
     '    bool reaching = transformed || moves > 0;', '    bool reaching = transformed;'),
    ('a-move-marks-only-where-it-is', RESTYLE,
     '    if (marking)\n        marked = keep_changed(tree, &k, &written, &changed, &all);\n',
     '    if (0 && marking)\n        marked = keep_changed(tree, &k, &written, &changed, &all);\n'),
    ('the-observers-not-told-of-a-move', YONDER,
     '    if (kept.moved > 0)\n        s_ticks_moved++;\n', ''),
    ('a-move-left-unmarked-in-the-snapshot', FLOW,
     '    if (t->reuse != NULL)\n        reuse_mark(t->reuse, node);\n', ''),
    ('a-moved-box-s-stamp-kept', FLOW,
     '    for (const flow_box_t *b = box; b != NULL; b = box_after_in(box, b))\n        drawn_changed(b, true, stamp);\n',
     '    (void)stamp;\n'),
    ('a-pseudo-element-anchored-to-itself-differs', RESTYLE, '    return o == n || (o == o_self && n == n_self);',
     '    return o == n || (o == o_self && n == n_self && 0);'),
    ('motion-kept', RESTYLE, '    if (was->motion != NULL || is->motion != NULL)\n', '    if (0)\n'),
    ('decoration-kept', RESTYLE, '    if (!same_decoration)\n', '    if (0)\n'),
    # The writes.
    ('the-style-not-written', RESTYLE, '        k.pairs[i].was->style = values[i];', '        (void)values;'),
    ('the-frame-not-filled', RESTYLE, '            transform_fill(f, box->style);', '            (void)0;'),
    ('the-ink-not-grown', RESTYLE, '        ink_grown(box);', '        (void)0;'),
    ('not-stamped-drawn', RESTYLE, '        drawn_changed(box, true, stamp);', '        (void)stamp;'),
    ('anonymous-boxes-not-taken-again', RESTYLE,
     '            f_style_anonymous(tree->styles, box->parent->style, box->style->display, (flow_style_t *)box->style);',
     '            (void)0;'),
    ('the-lists-left-in-the-build', RESTYLE, '        room = f_style_copy_lists(&values[i], &tree->restyle);',
     '        room = true;'),
    # Attributes, matrices and reach (the cold read's three).
    ('every-attribute-styled-only', SNAPSHOT, '            (entry->kind == OS64_HTML_CHANGE_ATTR && !reuse_attr_styled_only(entry)))',
     '            (entry->kind == OS64_HTML_CHANGE_ATTR && !reuse_attr_styled_only(entry) && 0))'),
    ('a-matrix-crossing-not-tried', RESTYLE, '            transform_fill(&tried, value);\n            tried.box = &tried_box;',
     '            tried.box = &tried_box;'),
    ('the-reach-not-worked-again', RESTYLE, '        reach_stacked(tree);\n        sticky_reach_scrolled(tree);\n        // A range',
     '        (void)0;\n        // A range'),
    # Codex's round 1.
    ('groups-and-contexts-not-asked', RESTYLE,
     '        bool layers_same = is_group(&tried_box) == is_group(box) && makes_context(&tried_box) == makes_context(box);',
     '        bool layers_same = is_group(&tried_box) == is_group(&tried_box) && makes_context(&tried_box) == makes_context(box);'),
    ('a-motion-base-not-asked', RESTYLE,
     '                                 !f_style_equal(&was->motion->base, &is->motion->base))))',
     '                                 0)))'),
    ('the-outline-not-handed-past-stacked', FLOW,
     '    for (; c->holds_outline && c->parent != NULL && !c->parent->holds_outline; c = c->parent)',
     '    for (; 0 && c->holds_outline && c->parent != NULL && !c->parent->holds_outline; c = c->parent)'),
    ('the-check-not-scrolled-alike', YONDER, '            flow_scroll_set(whole, s, flow_scroll_at(g.page.tree, i));',
     '            (void)0;'),
    ('the-check-ignores-outlines', RESTYLE, '                            : y->holds_outline && !x->holds_outline ? "an outline not held"',
     '                            : 0 ? "an outline not held"'),
    # The check (--geometry-check).
    ('the-check-ignores-styles', RESTYLE, '                            : x->style != NULL && !f_style_equal(x->style, y->style) ? "a style"',
     '                            : 0 ? "a style"'),
    ('the-check-ignores-border-boxes', RESTYLE, '                            : !same_rect(x->rect, y->rect) ? "a border box"',
     '                            : 0 ? "a border box"'),
]

# Not run, because it changes nothing a rule promises:
# - keep_collect's count of elements: a change of membership with no node
#   added or removed is an element's own display, which its style already
#   declines as geometry before the count matters.
# - `tree->styles->cascade_updates = b->cascade_updates`: without it the next
#   build restyles elements the cascade did not touch, which costs a build's
#   work and changes no answer.
# A break that does not build (a function left unused, which -Werror
# refuses) is written to keep the function called, `&& 0` after it.

env = dict(os.environ, YONDER_PROFILE_ONLY='1')
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
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
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

print(f'\nrestyle mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
