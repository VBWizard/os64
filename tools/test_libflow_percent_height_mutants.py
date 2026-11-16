#!/usr/bin/env python3
"""One deliberate break at a time to the rules test_libflow_percent_height.inc
holds libflow to — a percentage height, and a percentage height limit, is
of the containing block's height where that is definite (CSS 2.1 § 10.5,
§ 10.7) and `auto` where it is not — against those cases and the flex,
grid, positioned and grafted layouts they reach (test_libflow_host.sh
--percent-height): a mutant is CAUGHT when they fail, MISSED when they
pass, which means a test is missing and gets written. A build failure is
neither.

The shape is test_libflow_origin_mutants.py's: each mutant is applied to the
worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it through tools/mutants_parallel.py, or alone from a clean
tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/libflow-percent-height-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

LAYOUT = 'userland/libflow/layout.c'
PROBE = 'userland/libflow/reuse_probe.inc'
GRAFT = 'userland/libflow/reuse_graft.inc'
CASES = 'bash tools/test_libflow_host.sh --percent-height'

# (name, file, old, new). Each breaks one rule the cases state.
mutants = [
    # Above the root: the viewport.
    ('the-root-has-no-containing-height', LAYOUT,
     '    if (b == NULL)\n        return l->icb_h;',
     '    (void)l;\n    if (b == NULL)\n        return -1;'),
    # An anonymous box is passed over.
    ('an-anonymous-box-is-a-containing-block', LAYOUT,
     '    while (b != NULL && b->node == NULL)\n        b = b->parent;\n    if (b == NULL)\n        return l->icb_h;',
     '    if (b == NULL)\n        return l->icb_h;'),
    # A grid item's containing block is its area.
    ('a-grid-item-resolves-against-its-container', LAYOUT,
     '    if (b->parent != NULL && b->parent->grid)\n        return -1;\n',
     ''),
    # The parent's USED height: its limits applied.
    ('the-parent-height-is-unclamped', LAYOUT,
     '    if (b->def_h >= 0 && !b->abs_h_set)\n        b->def_h = clamp_height(b->style, b->def_h, vf, b->cb_h);',
     '    (void)clamp_height;'),
    # box-sizing: the frame comes off a border box's percentage.
    ('a-border-box-percentage-keeps-its-frame', LAYOUT,
     '        return content_of(s, s->height, max64(0, cb_h), vf);\n    return -1;',
     '        return s->height.kind == FLOW_LENGTH_PX ? content_of(s, s->height, 0, vf) : len(s->height, max64(0, cb_h));\n    return -1;'),
    # A percentage limit binds against a definite height.
    ('a-percentage-limit-binds-nothing', LAYOUT,
     '    return limit.kind == FLOW_LENGTH_PX || (limit.kind == FLOW_LENGTH_PERCENT && base >= 0);',
     '    (void)base;\n    return limit.kind == FLOW_LENGTH_PX;'),
    ('a-percentage-limit-of-nothing-binds', LAYOUT,
     '    return limit.kind == FLOW_LENGTH_PX || (limit.kind == FLOW_LENGTH_PERCENT && base >= 0);',
     '    (void)base;\n    return limit.kind == FLOW_LENGTH_PX || limit.kind == FLOW_LENGTH_PERCENT;'),
    # A percentage of nothing definite is auto.
    ('a-percentage-of-nothing-is-zero', LAYOUT,
     '    if (s->height.kind == FLOW_LENGTH_PX || (s->height.kind == FLOW_LENGTH_PERCENT && cb_h >= 0))',
     '    if (s->height.kind == FLOW_LENGTH_PX || s->height.kind == FLOW_LENGTH_PERCENT)'),
    # A resolved min-height holds margins in, as a length's does.
    ('a-percentage-min-height-lets-margins-through', LAYOUT,
     '    bool min_height = binds_h(s->min_height, b->cb_h) && len(s->min_height, max64(0, b->cb_h)) > 0;',
     '    bool min_height = s->min_height.kind == FLOW_LENGTH_PX && s->min_height.value > 0;'),
    # A flex container's own resolved height.
    ('a-flex-container-reads-lengths-alone', LAYOUT,
     '    int64_t h_given = own_definite(b);\n    bool h_def = h_given >= 0;\n    for (FBox *c = b->first; c != NULL; c = c->next)\n        if (!c->out_of_flow)\n            seed_heights(l, c, cw);',
     '    int64_t h_given = own_definite(b);\n    bool h_def = s->height.kind == FLOW_LENGTH_PX;\n    for (FBox *c = b->first; c != NULL; c = c->next)\n        if (!c->out_of_flow)\n            seed_heights(l, c, cw);'),
    # Items are seeded before they are measured.
    ('flex-items-are-measured-unseeded', LAYOUT,
     '    for (FBox *c = b->first; c != NULL; c = c->next)\n        if (!c->out_of_flow)\n            seed_heights(l, c, cw);',
     ''),
    # A resolved percentage is a size the page gave across: no stretch.
    ('a-resolved-item-percentage-stretches', LAYOUT,
     '    bool size_given = row ? fixed_height(c)',
     '    bool size_given = row ? c->style->height.kind == FLOW_LENGTH_PX'),
    ('a-placed-row-stretches-a-resolved-percentage', LAYOUT,
     '                    if (!fixed_height(c) && c->kind != FB_TABLE)\n                        h = max64(vf, clamp_height(cs, max64(0, line - mt - mb - vf), vf, box_cb_h(c)) + vf);',
     '                    if (cs->height.kind != FLOW_LENGTH_PX && c->kind != FB_TABLE)\n                        h = max64(vf, clamp_height(cs, max64(0, line - mt - mb - vf), vf, box_cb_h(c)) + vf);'),
    # A picture's percentage height.
    ('a-picture-height-has-no-containing-height', LAYOUT,
     '        rep = replaced_size(l, b->node, s, cbw, b->cb_h);',
     '        rep = replaced_size(l, b->node, s, cbw, -1);'),
    # A block's own resolved height.
    ('a-block-reads-lengths-alone', LAYOUT,
     '    bool height_auto = b->def_h < 0;',
     '    bool height_auto = b->style->height.kind != FLOW_LENGTH_PX && !b->abs_h_set;'),
    # An absolute box's height is definite for its children.
    ('an-absolute-height-is-not-definite', LAYOUT,
     '    if (b->abs_h_set)\n        return b->abs_h;\n    const flow_style_t *s = b->style;\n    if (s->height.kind == FLOW_LENGTH_PX || (s->height.kind == FLOW_LENGTH_PERCENT && cb_h >= 0))',
     '    const flow_style_t *s = b->style;\n    if (s->height.kind == FLOW_LENGTH_PX || (s->height.kind == FLOW_LENGTH_PERCENT && cb_h >= 0))'),
    # An absolute box's containing block is the block it is placed in.
    ('an-absolute-box-takes-its-parent-height', LAYOUT,
     '    if (!b->abs_sized)\n        b->cb_h = percent_base_h(l, b);',
     '    b->cb_h = percent_base_h(l, b);'),
    # A picture is measured against the height it was seeded with.
    ('a-picture-is-measured-without-a-containing-height', LAYOUT,
     '    Replaced r = replaced_size(l, b->node, &own, cbw, box_cb_h(b));\n    return axis == 0 ? r.w : r.h;',
     '    Replaced r = replaced_size(l, b->node, &own, cbw, -1);\n    return axis == 0 ? r.w : r.h;'),
    # An inline picture and an inline-block are of the block their line is in.
    ('an-inline-picture-has-no-containing-height', LAYOUT,
     '            if (it->content == NULL)\n                r = replaced_size(l, it->node, st, cw, cbh);',
     '            if (it->content == NULL)\n                r = replaced_size(l, it->node, st, cw, -1);'),
    ('an-inline-block-ratio-reads-lengths-alone', LAYOUT,
     '                int64_t own_h = own_height(it->content, cbh, fh);',
     '                int64_t own_h = own_height(it->content, -1, fh);'),
    # A column item's flex base is its height before its limits.
    ('a-column-base-is-held-to-its-limits', LAYOUT,
     '            it->base = own_height(c, c->cb_h, it->frame) + it->frame;',
     '            it->base = c->def_h + it->frame;'),
    # A cell its table lays out keeps a length for its percentage offsets.
    ('a-cell-has-no-offset-height', LAYOUT,
     '    if (s->height.kind != FLOW_LENGTH_PX)\n        return -1;\n    int64_t frame',
     '    if (s->height.kind != FLOW_LENGTH_PX || true)\n        return -1;\n    int64_t frame'),
    # A border box's height takes off the frame it is laid out with.
    ('def-h-keeps-the-offered-frame', LAYOUT,
     '    if (b->def_h >= 0 && !b->abs_h_set)\n        b->def_h = clamp_height(s, own_height(b, b->cb_h, vframe_px), vframe_px, b->cb_h);\n',
     ''),
    # Reused widths were measured against the same height.
    ('reused-widths-ignore-the-height-they-were-measured-with', LAYOUT,
     '    if (pair && pair->equal && pair->ordinary && pair->old->intrinsic_known &&\n        pair->old->intrinsic_def == (b->def_known ? b->def_h : -2)) {',
     '    if (pair && pair->equal && pair->ordinary && pair->old->intrinsic_known) {'),
    # A copy is keyed on the height it was placed in.
    ('a-copy-ignores-the-containing-height', PROBE,
     '.cbh = offset_height(l, b->parent),',
     '.cbh = -1,'),
    # A height a ratio gives from the width is definite (FBox.ratio_h).
    ('a-ratio-height-is-not-definite', LAYOUT,
     '               ? clamp_height(s, ratio_height(s, cw, b->w - cw, vframe_px), vframe_px, b->cb_h)\n',
     '               ? -1\n'),
    ('a-ratio-height-is-not-held', LAYOUT,
     '               ? clamp_height(s, ratio_height(s, cw, b->w - cw, vframe_px), vframe_px, b->cb_h)\n',
     '               ? ratio_height(s, cw, b->w - cw, vframe_px)\n'),
    ('a-child-reads-the-style-height-alone', LAYOUT,
     '    return b->def_known ? own_definite(b) : -1;',
     '    return b->def_known ? b->def_h : -1;'),
    ('a-flex-container-ignores-its-ratio', LAYOUT,
     '    // measures before laying any out.\n    int64_t h_given = own_definite(b);',
     '    // measures before laying any out.\n    int64_t h_given = b->def_h;'),
    ('a-grid-ignores-its-ratio', LAYOUT,
     "    // (own_definite: its style's or its ratio's).\n    int64_t h_given = own_definite(b);",
     "    // (own_definite: its style's or its ratio's).\n    int64_t h_given = b->def_h;"),
    ('a-container-that-grows-its-items-keeps-their-ratio', LAYOUT,
     '    b->ratio_h = by_ratio && !height_set_later(b)',
     '    b->ratio_h = by_ratio && (true || !height_set_later(b))'),
    ('a-grid-item-under-normal-counts-as-stretched', LAYOUT,
     '    return written != FLOW_PLACE_NORMAL && written != FLOW_PLACE_AUTO &&\n           self_align(',
     '    (void)written;\n    return true &&\n           self_align('),
    ('a-column-item-keeps-its-ratio', LAYOUT,
     '        return !row || (stretches_across(ps, b) &&',
     '        return row && (stretches_across(ps, b) &&'),
    ('a-stretched-row-item-keeps-its-ratio', LAYOUT,
     '        return !row || (stretches_across(ps, b) &&',
     '        return !row || (false &&'),
    ('a-lone-row-item-counts-as-stretched', LAYOUT,
     '(own_definite(b->parent) >= 0 || limited || !lone_item(b)));',
     '(own_definite(b->parent) >= 0 || limited || (true || !lone_item(b))));'),
    ('a-row-item-beside-others-keeps-its-ratio', LAYOUT,
     '(own_definite(b->parent) >= 0 || limited || !lone_item(b)));',
     '(own_definite(b->parent) >= 0 || limited || (false && !lone_item(b))));'),
    ('a-row-max-height-does-not-bind-a-lone-item', LAYOUT,
     "        bool limited = ps->min_height.kind != FLOW_LENGTH_AUTO || ps->max_height.kind == FLOW_LENGTH_PX ||",
     "        bool limited = ps->min_height.kind != FLOW_LENGTH_AUTO || false ||"),
    ('a-flex-box-with-a-ratio-does-not-grow', LAYOUT,
     '        // and placed again against that, before anything is placed in them.\n        int64_t grown = ratio_grown(b, cw, *natural);',
     '        // and placed again against that, before anything is placed in them.\n        int64_t grown = -1;'),
    ('a-grid-with-a-ratio-does-not-grow', LAYOUT,
     '        // gives, its rows sized and its items placed again against that.\n        int64_t grown = ratio_grown(b, cw, *natural);',
     '        // gives, its rows sized and its items placed again against that.\n        int64_t grown = -1;'),
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

print(f'\npercent height mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
