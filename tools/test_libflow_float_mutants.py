#!/usr/bin/env python3
"""One deliberate break at a time to the float layout (docs/design/pending/
FLOATS.md), against the float cases (test_libflow_host.sh --floats): a
mutant is CAUGHT when they fail, MISSED when they pass, which means a test
is missing and gets written. A build failure is neither.

The shape is test_yonder_diag_mutants.py's: each mutant is applied to the
worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/libflow-float-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

STYLE = 'userland/libflow/style.c'
BOXES = 'userland/libflow/boxes.c'
LAYOUT = 'userland/libflow/layout.c'
FLOW = 'userland/libflow/flow.c'
CASES = 'bash tools/test_libflow_host.sh --floats'

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # Pass 1.
    ('a-float-is-not-blockified', STYLE,
     '    if ((f_out_of_flow(s) || s->float_side != FLOW_FLOAT_NONE) && !blockify(s))',
     '    if (f_out_of_flow(s) && !blockify(s))'),
    ('positioning-off-keeps-floats', STYLE,
     '        s->position = FLOW_POSITION_STATIC;\n        s->float_side = FLOW_FLOAT_NONE;',
     '        s->position = FLOW_POSITION_STATIC;'),
    ('an-item-floats', STYLE,
     '    if (item)\n        s->float_side = FLOW_FLOAT_NONE;\n',
     ''),
    ('a-float-holds-a-block-in-the-flow', STYLE,
     '    if (f_leaves_flow(&child->style))',
     '    if (f_out_of_flow(&child->style))'),
    # Pass 2.
    ('a-float-box-is-no-float', BOXES,
     '    box->floated = parent != NULL && style != NULL ? style->float_side : FLOW_FLOAT_NONE;',
     '    box->floated = FLOW_FLOAT_NONE;'),
    ('an-inline-float-has-no-item', BOXES,
     '    if (f->target == NULL) {\n        element_box(f->b, f->container, el, s, scope);',
     '    if (true) {\n        element_box(f->b, f->container, el, s, scope);'),
    # Pass 3: the space.
    ('a-float-fits-beside-its-own-side-only', LAYOUT,
     '        band(sp, at, h, cx, cw, &x0, &x1);\n        bool alone',
     '        band(sp, at, h, cx, cw, &x0, &x1);\n'
     '        if (side == SIDE_LEFT)\n            x1 = cx + cw;\n        else\n            x0 = cx;\n'
     '        bool alone'),
    ('a-float-goes-above-an-earlier-one', LAYOUT,
     '    int64_t at = max64(y_min, sp->top);',
     '    int64_t at = y_min;'),
    ('a-float-alone-is-moved-down', LAYOUT,
     '        bool alone = x0 == cx && x1 == cx + cw;',
     '        bool alone = false;'),
    ('the-space-records-the-moved-float', LAYOUT,
     '    space_record(l, sp, side, x, y, w, h);\n    int64_t dx = 0, dy = 0;\n'
     '    if (f->positioned && !f->out_of_flow)\n'
     '        rel_offset(f->style, cw, definite_height(f->parent), &dx, &dy);\n',
     '    int64_t dx = 0, dy = 0;\n'
     '    if (f->positioned && !f->out_of_flow)\n'
     '        rel_offset(f->style, cw, definite_height(f->parent), &dx, &dy);\n'
     '    space_record(l, sp, side, x + dx, y + dy, w, h);\n'),
    ('a-relative-float-is-not-moved', LAYOUT,
     '    translate(f, x + ml + rel_x + dx - f->x, y + mt + rel_y + dy - f->y);',
     '    translate(f, x + ml + rel_x - f->x, y + mt + rel_y - f->y);'),
    ('a-relative-box-is-not-taken-off', LAYOUT,
     '    if (l->space != NULL) {\n        l->space->rel_x += dx;\n        l->space->rel_y += dy;\n    }\n'
     '    block_at(l, styles, b, cbx + dx, cbw, &moved);\n'
     '    if (l->space != NULL) {\n        l->space->rel_x -= dx;\n        l->space->rel_y -= dy;\n    }\n',
     '    block_at(l, styles, b, cbx + dx, cbw, &moved);\n'),
    ('a-relative-inline-leaves-its-float', LAYOUT,
     '                    float_at(l, styles, f, cx, cw, cur, rel_x, rel_y);',
     '                    float_at(l, styles, f, cx, cw, cur, 0, 0);'),
    ('a-waiting-float-keeps-the-moved-edge', LAYOUT,
     '        sp->wait[sp->nwait++] = (Wait){f, cx - sp->rel_x, cw,',
     '        sp->wait[sp->nwait++] = (Wait){f, cx, cw,'),
    # Pass 3: waiting for margins.
    ('a-float-never-waits', LAYOUT,
     '    if (at->first < 0) {\n        if (sp->nwait == sp->cap_wait) {',
     '    if (false) {\n        if (sp->nwait == sp->cap_wait) {'),
    ('a-border-edge-resolves-nothing', LAYOUT,
     '        if (in_flow(b))\n            settle(l, y_border);\n',
     ''),
    ('laying-a-float-out-resolves-margins', LAYOUT,
     '        if (in_flow(b))\n            settle(l, y_border);\n',
     '        settle(l, y_border);\n'),
    ('a-root-end-resolves-nothing', LAYOUT,
     '        if (!l->failed && !b->unfinished)\n            settle(l, in.y + margins_sum(in.pm));\n',
     ''),
    ('a-line-resolves-nothing', LAYOUT,
     '            if (floats_waiting(l)) {',
     '            if (false && floats_waiting(l)) {'),
    # Pass 3: the float's own layout.
    ('a-float-fills-the-line', LAYOUT,
     '               (b->floated != FLOW_FLOAT_NONE && s->width.kind == FLOW_LENGTH_AUTO)) {',
     '               false) {'),
    ('a-float-is-no-formatting-context', LAYOUT,
     '           b->floated != FLOW_FLOAT_NONE ||\n',
     ''),
    ('a-float-takes-its-item-marker', LAYOUT,
     '    l->marker_floor = l->nmarkers;\n    Cursor c = {0, kNoMargins, 0};',
     '    Cursor c = {0, kNoMargins, 0};'),
    # A float the whole layout would put somewhere else is not placed
    # (the relation sweep, LAYOUT.md § Proof relation b).
    ('a-float-that-cannot-wait-keeps-its-spot', LAYOUT,
     '                // It cannot wait, so it has no place.\n                unplace(f);\n',
     ''),
    ('a-float-waiting-at-a-stop-is-placed', LAYOUT,
     '        for (int32_t k = 0; k < own.nwait; k++)\n            unplace(own.wait[k].box);\n',
     ''),
    ('a-float-on-a-dropped-line-is-placed', LAYOUT,
     '            if (s.v[j].kind == SG_FLOAT)\n                unplace(s.v[j].item->floated);',
     '            (void)0;'),
    ('a-cut-box-resolves-at-its-end', LAYOUT,
     '    if (!root && (!bottom_open || !height_auto) && !b->unfinished)',
     '    if (!root && (!bottom_open || !height_auto))'),
    ('a-cut-box-resolves-at-its-top', LAYOUT,
     '        if (in.first < 0 && in_flow(b) && !b->unfinished)\n            settle(l, y_border);',
     '        if (in.first < 0 && in_flow(b))\n            settle(l, y_border);'),
    ('a-cut-root-resolves-at-its-end', LAYOUT,
     '        if (!l->failed && !b->unfinished)\n            settle(l, in.y + margins_sum(in.pm));',
     '        if (!l->failed)\n            settle(l, in.y + margins_sum(in.pm));'),
    # Lines beside floats (commit 2).
    ('a-line-ignores-the-floats', LAYOUT,
     '        line_room(l, y_at, ask_h, cx, cw, &lx, &lw);',
     '        lx = cx;\n        lw = cw;'),
    ('a-line-never-moves-down', LAYOUT,
     '        if ((lx > cx || lw < cw) && first_unit(&s, start, indent) > lw) {',
     '        if (false && first_unit(&s, start, indent) > lw) {'),
    ('a-tall-line-is-not-asked-again', LAYOUT,
     '        if (height > ask_h && !tall_asked) {',
     '        if (false && !tall_asked) {'),
    ('a-placed-float-breaks-nothing-again', LAYOUT,
     '                    float_put(l, f, y_at, cx, cw, rel_x, rel_y);\n                    rebreak = true;',
     '                    float_put(l, f, y_at, cx, cw, rel_x, rel_y);'),
    ('a-float-that-does-not-fit-stays-on-its-line', LAYOUT,
     '                if (!content || pen + float_outer_w(f, cw) <= lw) {',
     '                if (true || pen + float_outer_w(f, cw) <= lw) {'),
    ('a-float-that-fits-goes-below', LAYOUT,
     '                if (!content || pen + float_outer_w(f, cw) <= lw) {',
     '                if (!content || (false && pen + float_outer_w(f, cw) <= lw)) {'),
    ('a-float-below-its-line-is-lost', LAYOUT,
     '            float_put(l, later[k].f, cur->y, cx, cw, later[k].rel_x, later[k].rel_y);',
     '            (void)later[k];'),
    ('a-mid-line-float-waits', LAYOUT,
     '                if (cur->first < 0 && !content) {',
     '                if (cur->first < 0) {'),
    ('settled-floats-break-nothing-again', LAYOUT,
     '                settle(l, cur->y);\n                if (!l->failed)\n                    goto rebreak;',
     '                settle(l, cur->y);'),
    ('alignment-ignores-the-room', LAYOUT,
     '        int64_t shift = 0, extra = lw - used;',
     '        int64_t shift = 0, extra = cw - used;'),
    ('fragments-start-at-the-content-edge', LAYOUT,
     '            fr->x += lx + shift + per_gap * gaps_before(gap_at, gaps, fr->x) + fr->rel_x;',
     '            fr->x += cx + shift + per_gap * gaps_before(gap_at, gaps, fr->x) + fr->rel_x;'),
    ('a-rebreak-forgets-the-indent', LAYOUT,
     '        indent = indent0;\n        rel_x = rel_x0;',
     '        (void)indent0;\n        rel_x = rel_x0;'),
    ('a-rebreak-forgets-the-open-inlines', LAYOUT,
     '        open.n = open0;\n        for (size_t k = 0; k < open0; k++)\n            open.v[k] = open_save[k];\n',
     ''),
    ('a-rebreak-forgets-the-markers', LAYOUT,
     '        l->nmarkers = markers0;\n',
     '        (void)markers0;\n'),
    # clear, roots holding floats, boxes beside floats (commit 3).
    ('a-block-clears-nothing', LAYOUT,
     '    if (below <= at)\n        return;\n    cur->y += margins_sum(cur->pm);',
     '    if (below <= at || true)\n        return;\n    cur->y += margins_sum(cur->pm);'),
    ('clearance-waits-for-its-own-margin', LAYOUT,
     '    settle(l, cur->y + margins_sum(cur->pm));\n    int64_t at =',
     '    settle(l, cur->y + margins_sum(margins_join(cur->pm, margin_of(mt))));\n    int64_t at ='),
    ('clearance-moves-the-parent-down', LAYOUT,
     '    cur->y += margins_sum(cur->pm);\n    if (cur->first < 0)\n        cur->first = cur->y;\n    b->clearance',
     '    cur->y += margins_sum(cur->pm);\n    b->clearance'),
    ('a-float-clears-nothing', LAYOUT,
     '        y_min = max64(y_min, sp->bottom[SIDE_LEFT]);\n',
     ''),
    ('a-br-clears-nothing', LAYOUT,
     '        if (below > cur->y)\n            cur->y = below;',
     '        (void)below;'),
    ('a-root-does-not-grow', LAYOUT,
     '            content_h = max64(content_h, floats_end - content_top);',
     '            (void)0;'),
    ('a-cell-does-not-grow', LAYOUT,
     '        content = max64(content, floats_end - top);',
     '        (void)0;'),
    ('a-cell-shares-the-outer-space', LAYOUT,
     '    space_open(&own);\n    l->space = &own;\n    if (cell->ifc)',
     '    space_open(&own);\n    if (cell->ifc)'),
    ('a-flow-root-is-no-root', LAYOUT,
     ' || (b->node != NULL && s->flow_root) ||',
     ' ||'),
    ('flow-root-is-not-read', STYLE,
     '    *flow_root = os64_streq(words[i], "flow-root");',
     '    *flow_root = false;'),
    ('a-box-overlaps-floats', LAYOUT,
     '    if (avoids_floats(b) && (l->space',
     '    if (false && avoids_floats(b) && (l->space'),
    ('a-table-overlaps-floats', LAYOUT,
     '    if (avoids_floats(t) && l->space',
     '    if (false && avoids_floats(t) && l->space'),
    ('a-box-too-wide-stays-beside', LAYOUT,
     '        if (!alone && x1 - x0 < need)\n            next = min64(side_next(&sp->side[SIDE_LEFT], y), side_next(&sp->side[SIDE_RIGHT], y));',
     '        if (false && !alone && x1 - x0 < need)\n            next = min64(side_next(&sp->side[SIDE_LEFT], y), side_next(&sp->side[SIDE_RIGHT], y));'),
    ('a-float-is-its-box-alone', LAYOUT,
     '    f->float_h = c.y + margins_sum(c.pm);',
     '    f->float_h = float_margin(f, FLOW_TOP, cw) + f->h + float_margin(f, FLOW_BOTTOM, cw);'),
    # Paint and hit order (commit 4).
    ('blocks-paint-floats-in-step-4', FLOW,
     '        if (in_tree(c) && c->floated == FLOW_FLOAT_NONE)\n            visit_blocks(cv, c);\n}',
     '        if (in_tree(c))\n            visit_blocks(cv, c);\n}'),
    ('a-float-on-a-line-paints-twice', FLOW,
     '        } else if (in_tree(c) && c->floated == FLOW_FLOAT_NONE) {\n            visit_inline(v, c);',
     '        } else if (in_tree(c)) {\n            visit_inline(v, c);'),
    ('the-float-step-is-skipped', FLOW,
     '            visit_whole(cv, c);\n        else\n            visit_floats(cv, c);',
     '            (void)0;\n        else\n            visit_floats(cv, c);'),
    ('holds-float-is-never-set', FLOW,
     '            b->holds_float |= child->floated != FLOW_FLOAT_NONE || child->holds_float;',
     '            (void)0;'),
    ('a-later-block-takes-the-click', FLOW,
     '        if (h != NULL && r >= *rank) {',
     '        if (h != NULL) {'),
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

print(f'\nfloat mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
