#!/usr/bin/env python3
"""One deliberate break at a time to transforms — the model and the
translations, and the painter (docs/design/pending/PILE3.md § Transforms,
slices S7a and S7b) — against the transform cases (test_libflow_host.sh
--transforms) and yonder's painter cases (test_yonder_host.sh): a mutant is
CAUGHT when they fail, MISSED when they pass, which means a test is missing
and gets written. A build failure is neither.

The shape is test_libflow_float_mutants.py's: each mutant is applied to the
worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/libflow-transform-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

INTERNAL = 'userland/libflow/internal.h'
STYLE = 'userland/libflow/style.c'
BOXES = 'userland/libflow/boxes.c'
PAINT = 'userland/apps/yonder/paint.c'
DUMP = 'userland/libflow/dump.c'
LAYOUT = 'userland/libflow/layout.c'
FLOW = 'userland/libflow/flow.c'
CASES = 'bash tools/test_libflow_host.sh --transforms && bash tools/test_yonder_host.sh'

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The style: which properties transform, and the matrix each makes.
    ('the-list-transforms-nothing', INTERNAL,
     '    return s->transform != NULL || s->translate != NULL',
     '    return s->translate != NULL'),
    ('a-plain-inline-is-transformed', INTERNAL,
     '    return f_transforms(s) && s->display != FLOW_DISPLAY_INLINE;',
     '    return f_transforms(s);'),
    ('the-root-keeps-its-inline-display', STYLE,
     '    if ((item || root) && !f_out_of_flow(s))',
     '    if ((item || (false && root)) && !f_out_of_flow(s))'),
    ('a-scale-of-calc-percent-is-nothing', STYLE,
     '        *out = r.number ? r.px : r.pct / 100;',
     '        *out = r.number ? r.px : 0;'),
    ('a-turn-is-anticlockwise', STYLE,
     '    m[1] = y * x * t + z * s;',
     '    m[1] = y * x * t - z * s;'),
    ('a-scale-of-one-number-is-across-only', STYLE,
     '        m[5] = k > 1 ? d[1] : d[0];',
     '        m[5] = k > 1 ? d[1] : 1;'),
    ('a-matrix-translates-in-26-6', STYLE,
     '        m[12] = d[4] * unit, m[13] = d[5] * unit;',
     '        m[12] = d[4], m[13] = d[5];'),
    # The containing block.
    ('a-transformed-box-is-no-fixed-anchor', STYLE,
     '    out->fixed_anchor = f_contains_absolute(&sp.s) && f_transformed(&sp.s) ? n',
     '    out->fixed_anchor = false ? n'),
    ('a-positioned-plain-inline-anchors-fixed-boxes', STYLE,
     '    out->fixed_anchor = f_contains_absolute(&sp.s) && f_transformed(&sp.s) ? n',
     '    out->fixed_anchor = f_contains_absolute(&sp.s) && f_transforms(&sp.s) ? n'),
    ('a-fixed-box-ignores-its-anchor', BOXES,
     '    const os64_html_node_t *anchor = up == NULL ? NULL : fixed ? up->fixed_anchor : up->anchor;',
     '    const os64_html_node_t *anchor = up == NULL || fixed ? NULL : up->anchor;'),
    ('a-fixed-box-in-a-transform-is-fixed', FLOW,
     '                src->pos != NULL && src->pos->cb == NULL;',
     '                src->pos != NULL;'),
    # The matrix.
    ('a-percentage-down-is-of-the-width', FLOW,
     '    mat_translate(m, (double)f_len(f->x, w), (double)f_len(f->y, h), 0);',
     '    mat_translate(m, (double)f_len(f->x, w), (double)f_len(f->y, w + 0 * h), 0);'),
    ('the-list-before-the-individual-properties', FLOW,
     '    if (s->translate != NULL)\n        mat_fn(m, s->translate, w, h);',
     '    for (int32_t i = 0; i < s->ntransform; i++)\n        mat_fn(m, &s->transform[i], w, h);\n'
     '    if (s->translate != NULL)\n        mat_fn(m, s->translate, w, h);\n    if (false)'),
    ('nothing-is-upright', FLOW,
     '    f->upright = !flat;',
     '    f->upright = false;'),
    # Stacking.
    ('a-transformed-block-is-not-stacked', FLOW,
     '    b->stacked = src->positioned || (parent != NULL && own_transform(src, b)) ||',
     '    b->stacked = src->positioned ||'),
    ('a-transformed-box-is-no-context', FLOW,
     '           (b->frame != NULL && b->frame->box == b && b->frame->kind == F_FRAME_TRANSFORM);',
     '           false;'),
    ('a-transformed-atom-is-not-transformed', FLOW,
     '    if (fr->kind == FF_ATOMIC && f_transforms(fr->style)) {',
     '    if (false) {'),
    ('a-stacked-atom-is-painted-on-its-line-too', FLOW,
     '                if (!f->stacked && !f->unpainted && meets(f->overflow, v->view))',
     '                if (!f->unpainted && meets(f->overflow, v->view))'),
    # Where it is drawn.
    ('a-transform-frame-moves-nothing', FLOW,
     '                                                  : s->by;',
     '                                                  : (flow_point_t){0, 0};'),
    ('the-clip-outside-moves-with-the-box', FLOW,
     '        clip = kNoClip;         // its frame\'s, where it stays as the box moves',
     '        (void)0;'),
    ('a-transformed-box-hands-on-its-outer-clip', FLOW,
     '    return inner_clip(b, is_sticky(b) || b->transformed ? kNoClip : outer_clip(b));',
     '    return inner_clip(b, is_sticky(b) ? kNoClip : outer_clip(b));'),
    # Where it reaches.
    ('the-page-is-not-reached', FLOW,
     '        } else if (transformed && !p->fixed) {',
     '        } else if (false) {'),
    ('a-transformed-root-is-not-reached', FLOW,
     '            page_reach(t, (int64_t)r.x + r.w, (int64_t)r.y + r.h);',
     '            (void)0;'),
    ('a-transformed-sticky-box-loses-its-scroll', FLOW,
     '            st = (flow_frame_t *)st->outer;',
     '            st = st;'),
    ('a-scroller-is-not-reached', FLOW,
     '            scroll_reach(t->scrollers[f->index], right, bottom);',
     '            (void)0;'),
    ('the-parent-keeps-where-it-was-laid-out', FLOW,
     '        if (child != NULL && !c->positioned && !c->transformed) {',
     '        if (child != NULL && !c->positioned) {'),
    ('layout-reaches-where-it-was-laid-out', LAYOUT,
     '    return c->positioned || c->transformed;',
     '    return c->positioned;'),
    # Drawn through (S7b): where it is painted, and where it is.
    ('a-turn-is-an-offset', FLOW,
     '    f->through = flat && !moves;',
     '    f->through = flat && !moves && false;'),
    ('paint-coordinates-run-past-a-turn', FLOW,
     '    if (s != NULL && s->kind == F_FRAME_TRANSFORM && s->through && !m->untransformed)\n        return (flow_point_t){0, 0};',
     '    if (false)\n        return (flow_point_t){0, 0};'),
    ('a-turned-box-is-where-it-was-laid-out', FLOW,
     '    if (inner_through(box->frame) == NULL)\n        return r;',
     '    if (true)\n        return r;'),
    ('nothing-is-taken-back-through-a-matrix', FLOW,
     '    const flow_frame_t *t = inner_through(f);\n    if (t == NULL)\n        return true;',
     '    const flow_frame_t *t = inner_through(f);\n    if (true)\n        return true;'),
    ('an-outside-clip-is-met-inside-the-turn', FLOW,
     '    const flow_frame_t *end = inner_through(box->frame);',
     '    const flow_frame_t *end = NULL;'),
    ('a-turn-is-no-group', FLOW,
     '    return (b->style->opacity < 1000 || own_through(b)) && !b->unpainted;',
     '    return (b->style->opacity < 1000) && !b->unpainted;'),
    ('a-nested-turn-counts-its-boxes-unturned', FLOW,
     '                at = meet(affine_bounds(m, at), clip);',
     '                (void)clip;'),
    ('the-walk-culls-in-page-coordinates', FLOW,
     '                    os64_gui_rect_t back = affine_back(m, view);',
     '                    os64_gui_rect_t back = view;'),
    ('reach-moves-a-turn-by-its-origin', FLOW,
     '                r = f->through ? affine_bounds(f->affine, r) : moved(r, f->by.x, f->by.y);',
     '                r = moved(r, f->by.x, f->by.y);'),
    ('a-refused-turn-is-drawn-unmoved', PAINT,
     '        p->shift.x += upright.x;\n        p->shift.y += upright.y;',
     '        (void)upright;'),
    ('nothing-showing-is-drawn-upright', PAINT,
     '        if (own.w <= 0 || own.h <= 0) {',
     '        if (false) {'),
    ('the-walk-culls-only-the-turned-view', FLOW,
     '                                 : back.w > 0 && back.h > 0 ? join(back, upright)',
     '                                 : back.w > 0 && back.h > 0 ? back'),
    ('a-refused-turn-escapes-its-clip', PAINT,
     '        p->view = clip;\n        p->shift.x += upright.x;',
     '        p->shift.x += upright.x;'),
    ('a-sheet-is-cut-at-the-clip', PAINT,
     '        own = cut(bounds, widened(back));',
     '        own = cut(bounds, back.w > 0 ? back : widened(back));'),
    ('offsets-move-with-a-transform', FLOW,
     '                   : m->untransformed || s->through ? (flow_point_t){0, 0}',
     '                   : s->through ? (flow_point_t){0, 0}'),
    ('group-bounds-count-clipped-overflow', FLOW,
     '            if (l->box->clipped)\n                at = meet(at, flow_box_doc_clip(l->box, scroll));\n',
     ''),
    ('a-turn-past-the-cap-is-painted-unmoved', PAINT,
     '        if (flow_box_through(box, p->scroll, (double[6]){0}, NULL, NULL))\n            gs->hidden = d + 1;\n        else if (opacity_verbs(p))',
     '        if (false)\n            gs->hidden = d + 1;\n        else if (!flow_box_through(box, p->scroll, (double[6]){0}, NULL, NULL) && opacity_verbs(p))'),
    ('the-sheet-edge-overflows', PAINT,
     '        own = cut(bounds, widened(back));',
     '        own = cut(bounds, (os64_gui_rect_t){back.x - 1, back.y - 1, back.w + 2, back.h + 2 + 0 * widened(back).w});'),
    ('a-huge-matrix-is-dumped-unheld', DUMP,
     '                v = v < -9e18 ? -9e18 : v > 9e18 ? 9e18 : v;\n',
     ''),
    ('inside-a-laid-turn-the-view-is-the-page', PAINT,
     '            p->view = own;',
     '            (void)own;'),
    ('the-page-extent-counts-it-unmoved', LAYOUT,
     '        (b->transformed && b->parent != NULL))',
     '        false)'),
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
                                        timeout=900)
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

print(f'\ntransform mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
