#!/usr/bin/env python3
"""One deliberate break at a time to the kept drawings (PILE3.md §
Transforms, the painter: Kept drawings) — the store's rules
(userland/apps/yonder/keep.c), the painter's half of the protocol and what
spoils a keeping (userland/apps/yonder/paint.c), and libflow's `drawn`
stamps and the walk's `inside` (userland/libflow/flow.c) — against their
harnesses: tools/test_yonder_keep_host.sh and tools/test_yonder_host.sh for
yonder's files, tools/test_libflow_host.sh for libflow's. A mutant is
CAUGHT when its harness fails, MISSED when it passes, which means a test is
missing and gets written. A build failure is neither.

The shape is test_yonder_warp_mutants.py's: each mutant is applied to the
worktree's copy of the file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.

The face's half (yonder.c: pane_keep, pane_through_open and close, what
owns the sheets, the scratch's refusals read at a making's close) is built
by no host suite; QEMU's probe page checks it, with yonder's record.

Left out because nothing can tell them apart from the code: the walk asked
to cull nothing inside a group whose kept drawing is laid (the painter's
view there is empty, so it paints nothing either way, only slower), and
the store letting go of the oldest drawing first rather than another this
paint has not asked for (which it lets go is a matter of what is asked
again, not of what is drawn).
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/keep-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

KEEP = 'userland/apps/yonder/keep.c'
SCRATCH = 'userland/apps/yonder/scratch.c'
PAINT = 'userland/apps/yonder/paint.c'
FLOW = 'userland/libflow/flow.c'
YONDER_CASES = 'bash tools/test_yonder_keep_host.sh && bash tools/test_yonder_host.sh'
FLOW_CASES = 'bash tools/test_libflow_host.sh'

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The store.
    ('a-group-is-made-at-its-first-ask', KEEP,
     '        k->seen++;\n        return YONDER_KEEP_NO;',
     '        k->seen++;\n        return YONDER_KEEP_MAKE;'),
    ('two-parts-of-one-paint-are-two-paints', KEEP,
     '    if (r->state == K_NO || !earlier)',
     '    if (r->state == K_NO || (!earlier && false))'),
    ('the-face-stamp-is-not-the-key', KEEP,
     '           a->scroll.x == key->scroll.x && a->scroll.y == key->scroll.y && r->face == face;',
     '           a->scroll.x == key->scroll.x && a->scroll.y == key->scroll.y && (r->face == face || true);'),
    ('drawn-is-not-the-key', KEEP,
     '    return a->tree == key->tree && a->box == key->box && a->drawn == key->drawn &&',
     '    return a->tree == key->tree && a->box == key->box &&'),
    ('a-spoiled-making-is-made-again', KEEP,
     '    r->state = K_NO;\n    k->spoiled++;',
     '    k->spoiled++;'),
    ('room-counts-what-this-paint-asked-for', KEEP,
     '        if (r != keep && r->state == K_MADE && r->asked <= k->paint)\n            could += r->bytes;',
     '        if (r != keep && r->state == K_MADE)\n            could += r->bytes;'),
    ('another-tree-keeps-the-last-ones', KEEP,
     '    if (tree != k->tree) {',
     '    if (false && tree != k->tree) {'),
    ('a-drawing-let-go-is-still-counted', KEEP,
     '        k->bytes -= r->bytes;\n        k->held--;',
     '        k->held--;'),
    ('a-row-this-paint-asked-for-is-taken', KEEP,
     '            if (c->asked <= k->paint && (r == NULL || c->asked < r->asked))',
     '            if ((r == NULL || c->asked < r->asked))'),
    ('a-put-takes-a-row-already-held', KEEP,
     '    if (r == NULL || r->state != K_SEEN || !same(r, key, face) || black == NULL || white == NULL)',
     '    if (r == NULL || !same(r, key, face) || black == NULL || white == NULL)'),
    ('laid-is-counted-at-the-ask', KEEP,
     '    if (r->state == K_MADE)\n        return YONDER_KEEP_HAVE;',
     '    if (r->state == K_MADE) {\n        k->laid++;\n        return YONDER_KEEP_HAVE;\n    }'),
    ('sheets-are-handed-out-under-another-key', KEEP,
     '    if (r == NULL || r->state != K_MADE || !same(r, key, face))\n        return false;\n    *black',
     '    if (r == NULL || r->state != K_MADE || (!same(r, key, face) && false))\n        return false;\n    *black'),
    # The painter's half.
    ('a-making-is-painted-cut', PAINT,
     '                      : keep == YONDER_KEEP_MAKE ? bounds : own;',
     '                      : keep == YONDER_KEEP_MAKE ? own : own;'),
    ('a-making-opens-cut', PAINT,
     '                    p->v->through_open(p->v->ctx, keep == YONDER_KEEP_NO ? own : bounds, m, clip, keep);',
     '                    p->v->through_open(p->v->ctx, own, m, clip, keep);'),
    ('a-refused-have-is-not-asked-again', PAINT,
     '        if (!open && keep != YONDER_KEEP_NO) {',
     '        if (false) {'),
    ('a-making-is-culled-by-the-walk', PAINT,
     '    return gs->open[d].keep == YONDER_KEEP_MAKE ? FLOW_INSIDE_ALL',
     '    return gs->open[d].keep == YONDER_KEEP_MAKE ? FLOW_INSIDE_VIEW'),
    ('a-refused-layer-inside-is-kept', PAINT,
     '                            gs->open[d].keep == YONDER_KEEP_MAKE && !gs->open[d].spoiled &&\n'
     '                                yonder_paint_layers_refused == gs->open[d].refused);',
     '                            gs->open[d].keep == YONDER_KEEP_MAKE && !gs->open[d].spoiled);'),
    ('what-spoils-does-not-reach-the-close', PAINT,
     '                            gs->open[d].keep == YONDER_KEEP_MAKE && !gs->open[d].spoiled &&',
     '                            gs->open[d].keep == YONDER_KEEP_MAKE &&'),
    ('a-group-drawn-upright-inside-is-kept', PAINT,
     '        // and not as a group around it being kept would draw it again.\n        spoil(p);',
     '        // and not as a group around it being kept would draw it again.'),
    ('a-control-inside-is-kept', PAINT,
     '            spoil(p);\n            p->v->control(',
     '            p->v->control('),
    ('a-picture-inside-is-kept', PAINT,
     '            spoil(p);\n            p->v->image(',
     '            p->v->image('),
    ('a-frame-inside-is-kept', PAINT,
     '            spoil(p);\n            p->v->stand_in(',
     '            p->v->stand_in('),
    ('a-background-picture-inside-is-kept', PAINT,
     '        if (l.image != NULL ||\n            (making(p)',
     '        if (false ||\n            (making(p)'),
    ('an-inline-picture-inside-is-kept', PAINT,
     '    if (l.image != NULL) {\n        spoil(p);\n        os64_gui_rect_t area',
     '    if (l.image != NULL) {\n        os64_gui_rect_t area'),
    ('a-canvas-inside-is-kept', PAINT,
     '            spoil(p);\n            p->v->canvas(',
     '            p->v->canvas('),
    ('a-background-attribute-inside-is-kept', PAINT,
     '            (making(p) && p->v->backdrop(p->v->ctx, b, -1, NULL, (os64_gui_rect_t){0}, 0, 0,',
     '            (false && p->v->backdrop(p->v->ctx, b, -1, NULL, (os64_gui_rect_t){0}, 0, 0,'),
    ('an-empty-view-opens-a-layer', PAINT,
     '    if (p->view.w <= 0 || p->view.h <= 0)\n        return true;\n    // A blend asked',
     '    // A blend asked'),
    ('a-refused-making-is-not-asked-again', PAINT,
     '        if (!open && keep != YONDER_KEEP_NO) {',
     '        if (!open && keep == YONDER_KEEP_HAVE) {'),
    ('a-scratch-refusal-is-not-counted', SCRATCH,
     '    if (p == NULL && s != NULL && bytes != 0)\n        s->counts.refused++;',
     ''),
    ('a-take-of-nothing-is-a-refusal', SCRATCH,
     '    if (p == NULL && s != NULL && bytes != 0)',
     '    if (p == NULL && s != NULL)'),
    ('a-mask-picture-inside-is-kept', PAINT,
     '    } else if (l.image != NULL) {\n        spoil(p);\n        os64_gui_rect_t origin',
     '    } else if (l.image != NULL) {\n        os64_gui_rect_t origin'),
    ('a-text-clip-in-one-colour-is-kept', PAINT,
     '    // Drawn in one colour for want of a clip, which a later paint may have.\n    spoil(p);',
     ''),
    # libflow's stamps and walk.
    ('a-turn-stamps-its-own-box', FLOW,
     '        f_props_remove(&props, GARB_TRANSFORM);',
     ''),
    ('translate-stamps-its-own-box', FLOW,
     '        f_props_remove(&props, GARB_TRANSLATE);',
     ''),
    ('rotate-stamps-its-own-box', FLOW,
     '        f_props_remove(&props, GARB_ROTATE);',
     ''),
    ('scale-stamps-its-own-box', FLOW,
     '        f_props_remove(&props, GARB_SCALE);',
     ''),
    ('transform-origin-stamps-its-own-box', FLOW,
     '        f_props_remove(&props, GARB_TRANSFORM_ORIGIN);',
     ''),
    ('a-fade-stamps-its-own-box', FLOW,
     '        f_props_remove(&props, GARB_OPACITY);',
     ''),
    ('a-colour-does-not-stamp-its-own-box', FLOW,
     '    if (own)\n        ((flow_box_t *)b)->drawn = stamp;',
     '    if (own && false)\n        ((flow_box_t *)b)->drawn = stamp;'),
    ('the-boxes-around-are-not-stamped', FLOW,
     '    for (const flow_box_t *a = b->parent; a != NULL && a->drawn != stamp; a = a->parent)',
     '    for (const flow_box_t *a = b->parent; a != NULL && false; a = a->parent)'),
    ('a-later-tick-stamps-alike', FLOW,
     '        if (stamp == 0)\n            stamp = ++tree->drawn;',
     '        if (stamp == 0)\n            stamp = tree->drawn + 1;'),
    ('a-scroll-stamps-nothing', FLOW,
     '    if (f->at.x != was.x || f->at.y != was.y)\n        drawn_changed(f->box, true, ++tree->drawn);',
     '    if (false && (f->at.x != was.x || f->at.y != was.y))\n        drawn_changed(f->box, true, ++tree->drawn);'),
    ('a-held-scroll-stamps', FLOW,
     '    if (f->at.x != was.x || f->at.y != was.y)\n        drawn_changed(',
     '    if (true || f->at.x != was.x || f->at.y != was.y)\n        drawn_changed('),
    ('trees-share-a-serial', FLOW,
     '    tree->serial = __atomic_add_fetch(&s_tree_serials, 1, __ATOMIC_RELAXED);',
     '    tree->serial = 1 + 0 * __atomic_add_fetch(&s_tree_serials, 1, __ATOMIC_RELAXED);'),
    ('the-walk-culls-what-it-is-asked-for-whole', FLOW,
     '                    } else if (view.w == kWholePlane.w || asked == FLOW_INSIDE_ALL) {',
     '                    } else if (view.w == kWholePlane.w) {'),
    ('a-group-inside-none-gets-a-view', FLOW,
     '                    if (view.w <= 0 || view.h <= 0 || asked == FLOW_INSIDE_NONE) {',
     '                    if (asked == FLOW_INSIDE_NONE) {'),
    ('the-walk-hands-over-what-it-is-asked-for-none-of', FLOW,
     '                    if (view.w <= 0 || view.h <= 0 || asked == FLOW_INSIDE_NONE) {',
     '                    if (view.w <= 0 || view.h <= 0) {'),
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
    cases = FLOW_CASES if rel == FLOW else YONDER_CASES
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', cases], cwd=root, stdout=out, stderr=out,
                                        timeout=1800)
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

print(f'\nkeep mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
