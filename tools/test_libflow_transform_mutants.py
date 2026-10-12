#!/usr/bin/env python3
"""One deliberate break at a time to transforms, the model and the
translations (docs/design/pending/PILE3.md § Transforms, slice S7a),
against the transform cases (test_libflow_host.sh --transforms): a mutant
is CAUGHT when they fail, MISSED when they pass, which means a test is
missing and gets written. A build failure is neither.

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
LAYOUT = 'userland/libflow/layout.c'
FLOW = 'userland/libflow/flow.c'
CASES = 'bash tools/test_libflow_host.sh --transforms'

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The style: which properties transform, and the matrix each makes.
    ('the-list-transforms-nothing', INTERNAL,
     '    return s->transform != NULL || s->translate != NULL',
     '    return s->translate != NULL'),
    ('a-plain-inline-is-transformed', INTERNAL,
     '    return f_transforms(s) && s->display != FLOW_DISPLAY_INLINE;',
     '    return f_transforms(s);'),
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
     '    f->upright = !(',
     '    f->upright = false && !('),
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
     '                   : s->kind == F_FRAME_TRANSFORM ? s->by',
     '                   : s->kind == F_FRAME_TRANSFORM ? (flow_point_t){0, 0}'),
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
