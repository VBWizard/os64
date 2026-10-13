#!/usr/bin/env python3
"""One deliberate break at a time to the text clip (docs/design/pending/
PILE3.md § The text clip, slice S7c) — libgarb's names, libflow's fill
colour and clip set, the painter's pen and runs, and warp's masked lay —
against the cases that state its rules: libgarb's corpus, libflow's
--cliptext run, and yonder's painter and warp harnesses. A mutant is CAUGHT
when they fail, MISSED when they pass, which means a test is missing and
gets written. A build failure is neither.

The shape is test_libflow_transform_mutants.py's: each mutant is applied to
the worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.

Two breaks are left out because nothing can tell them apart from the code:
warp's skip of a pixel the mask does not cover (laid at coverage 0, a pixel
is what it was), and the pen's refusal of a box that is not text (the clip
set hands it text alone).
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/cliptext-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PROPS = 'userland/libgarb/props.c'
STYLE = 'userland/libflow/style.c'
FLOW = 'userland/libflow/flow.c'
PAINT = 'userland/apps/yonder/paint.c'
WARP = 'userland/apps/yonder/warp.c'
CASES = ('bash tools/test_yonder_warp_host.sh && bash tools/test_libflow_host.sh --cliptext && '
         'bash tools/test_yonder_host.sh && bash tools/test_garb_host.sh')

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # libgarb: the Compatibility Standard's names, and what @supports says.
    ('the-prefixed-clip-is-not-read', PROPS,
     '    {"-webkit-background-clip", GARB_BACKGROUND_CLIP},',
     '    {"-webkit-background-clipx", GARB_BACKGROUND_CLIP},'),
    ('a-clip-to-the-text-is-approximated', PROPS,
     '    {GARB_NPROPS, NULL},\n};',
     '    {GARB_BACKGROUND_CLIP, "text"},\n    {GARB_NPROPS, NULL},\n};'),
    ('the-fill-is-not-inherited', PROPS,
     '    [GARB_TEXT_FILL_COLOR] = {"-webkit-text-fill-color", true},',
     '    [GARB_TEXT_FILL_COLOR] = {"-webkit-text-fill-color", false},'),
    # libflow: the fill colour.
    ('the-fill-is-never-resolved', STYLE,
     '    if (s->current_colours & FLOW_CURRENT_FILL)\n        s->fill = s->color;',
     '    if (false && (s->current_colours & FLOW_CURRENT_FILL))\n        s->fill = s->color;'),
    ('currentcolor-is-not-inherited-as-a-keyword', STYLE,
     '                                  (parent->current_colours & FLOW_CURRENT_FILL));',
     '                                  FLOW_CURRENT_FILL);'),
    ('a-written-fill-is-not-inherited', STYLE,
     '    s.fill = parent->fill;\n',
     ''),
    ('inherit-takes-no-fill', STYLE,
     '        d->fill = s->fill;\n        d->current_colours = (uint8_t)((d->current_colours & ~FLOW_CURRENT_FILL) |\n'
     '                                       (s->current_colours & FLOW_CURRENT_FILL));\n',
     ''),
    # libflow: which text a clip holds.
    ('absolute-text-is-held', FLOW,
     '    if (d->positioned && (d->style->position == FLOW_POSITION_ABSOLUTE ||\n'
     '                          d->style->position == FLOW_POSITION_FIXED))\n        return true;',
     '    if (d->positioned && d->style->position == FLOW_POSITION_FIXED)\n        return true;'),
    ('fixed-text-is-held', FLOW,
     '    if (d->positioned && (d->style->position == FLOW_POSITION_ABSOLUTE ||\n'
     '                          d->style->position == FLOW_POSITION_FIXED))\n        return true;',
     '    if (d->positioned && d->style->position == FLOW_POSITION_ABSOLUTE)\n        return true;'),
    ('relative-text-leaves', FLOW,
     '    if (d->positioned && (d->style->position == FLOW_POSITION_ABSOLUTE ||\n'
     '                          d->style->position == FLOW_POSITION_FIXED))\n        return true;',
     '    if (d->positioned)\n        return true;'),
    ('transformed-text-is-held', FLOW,
     '    return d->frame != NULL && d->frame->kind == F_FRAME_TRANSFORM && d->frame->box == d;',
     '    return false;'),
    ('a-piece-holds-its-whole-line', FLOW,
     '        if (!node_within(c->node, box->node) || leaves_text_clip(c))',
     '        if (!node_within(c->node, c->node) || leaves_text_clip(c))'),
    ('a-piece-skips-its-atoms', FLOW,
     '        else if (c->kind == FLOW_BOX_ATOMIC)\n            visit_clip_subtree(c, visit, ctx);',
     '        else if (false)\n            visit_clip_subtree(c, visit, ctx);'),
    # The painter: the pen, the runs, the bounds, the refusal.
    ('the-mask-holds-the-shadows', PAINT,
     '        for (int32_t i = b->style->ntext_shadows - 1; b->run != NULL && !p->penned && i >= 0; i--) {',
     '        for (int32_t i = b->style->ntext_shadows - 1; b->run != NULL && i >= 0; i--) {'),
    ('the-mask-is-drawn-in-the-fill', PAINT,
     '                       p->penned ? p->pen : ink_of(p, b->style->fill));',
     '                       ink_of(p, b->style->fill));'),
    ('text-fills-with-its-colour', PAINT,
     '                       p->penned ? p->pen : ink_of(p, b->style->fill));',
     '                       p->penned ? p->pen : ink_of(p, b->style->color));'),
    ('a-marker-is-drawn-in-its-colour', PAINT,
     '    uint32_t colour = ink_of(p, b->style->fill);',
     '    uint32_t colour = ink_of(p, b->style->color);'),
    ('decorations-stay-out-of-the-mask', PAINT,
     '        fill(p, r.x, r.y, r.w, r.h, p->penned ? p->pen : ink_of(p, b->underline_color));',
     '        fill(p, r.x, r.y, r.w, r.h, ink_of(p, b->underline_color));'),
    ('each-layer-is-its-own-clip', PAINT,
     '        while (clipped_to_text(s, run.from, n) && run.to > 0 && clipped_to_text(s, run.to - 1, n))\n'
     '            run.to--;\n',
     ''),
    ('the-clip-is-not-cut-to-the-border-box', PAINT,
     '    os64_gui_rect_t bounds = cut(cut(held(ink.x0, ink.y0, ink.x1, ink.y1), on_page(p, run->b->rect)),\n'
     '                                 on_page(p, p->view));',
     '    os64_gui_rect_t bounds = cut(held(ink.x0, ink.y0, ink.x1, ink.y1), on_page(p, p->view));'),
    ('the-pen-is-not-cut-to-the-border-box', PAINT,
     '    pen.view = cut(on_page(p, p->view), on_page(p, run->b->rect));',
     '    pen.view = on_page(p, p->view);'),
    ('the-bounds-leave-out-the-ink', PAINT,
     '    int64_t x1 = ox + r.x + r.w + t->ink[FLOW_RIGHT];',
     '    int64_t x1 = ox + r.x + r.w;'),
    ('nothing-to-draw-is-rescued', PAINT,
     '    if (!run_paints(p, run))\n        return;\n    ClipInk',
     '    if (false && !run_paints(p, run))\n        return;\n    ClipInk'),
    ('a-refused-clip-draws-nothing', PAINT,
     '    pen.pen = clip_solid(p, run);\n    flow_visit_text_clip(run->b, pen_text, &pen);',
     '    pen.pen = clip_solid(p, run);'),
    ('a-spoiled-clip-is-not-rescued', PAINT,
     '        if (drawn)\n            return;\n    }',
     '        if (true || drawn)\n            return;\n    }'),
    ('a-starved-gradient-says-it-drew', PAINT,
     '    if (out == NULL)\n        return false;',
     '    if (out == NULL)\n        return true;'),
    ('a-piece-draws-no-picture', PAINT,
     '    if (l.image != NULL) {\n        os64_gui_rect_t area = on_page(p, b->rect);',
     '    if (false && l.image != NULL) {\n        os64_gui_rect_t area = on_page(p, b->rect);'),
    ('a-piece-picture-paints-nothing', PAINT,
     '        if ((l.image != NULL || (!run->piece && !sheet && i == 0)) &&',
     '        if ((!run->piece && (l.image != NULL || (!sheet && i == 0))) &&'),
    ('the-rescue-keeps-its-transparency', PAINT,
     '    return ink_of(p, c & 0xffffffu);',
     '    return ink_of(p, c);'),
    ('the-rescue-ignores-the-gradient', PAINT,
     '            if (g != NULL && stops_resolve(g, 100, s->color, &stops)) {',
     '            if (false && g != NULL && stops_resolve(g, 100, s->color, &stops)) {'),
    # warp: the masked lay.
    ('coverage-is-ignored', WARP,
     '                           c / 255.0) << (16 - 8 * k);',
     '                           1.0) << (16 - 8 * k);'),
    ('the-offset-is-not-applied', WARP,
     '            size_t at = j * (size_t)s->w + (size_t)(x - dx);',
     '            size_t at = j * (size_t)s->w + (size_t)(x - x0);'),
    ('the-clip-is-not-kept', WARP,
     '    if ((int64_t)clip.x + clip.w < x1)\n        x1 = (int64_t)clip.x + clip.w;',
     ''),
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

print(f'\ntext clip mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
