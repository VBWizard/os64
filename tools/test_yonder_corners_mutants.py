#!/usr/bin/env python3
"""One deliberate break at a time to a rounded box's background picture
(docs/design/pending/PILE3.md § Backgrounds: the picture follows the curve)
and to which boxes a moving background is counted behind (yonder.c's
box_draws_background) — against the cases that state their rules: the
painter's round_picture_cases and the scripted host's picture_text_cases
and round_picture_glass_cases. A mutant is CAUGHT when they fail, MISSED
when they pass, which means a test is missing and gets written. A build
failure is neither.

The shape is test_cliptext_mutants.py's: each mutant is applied to the
worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.

Left out, because nothing can tell them apart from the code: the corner
square's far edges rounded down instead of up (every radius reaching the
painter is whole pixels), and a band of no rows skipped or drawn (held
makes it empty).
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/corners-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PAINT = 'userland/apps/yonder/paint.c'
YONDER = 'userland/apps/yonder/yonder.c'
CASES = 'bash tools/test_yonder_host.sh && bash tools/test_yonder_scripts_host.sh'
TIMEOUT = 1800

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The picture is cut to the curve at all.
    ('the-picture-is-cut-square', PAINT,
     '        if (outer != NULL) {\n            round_backdrop(',
     '        if (false && outer != NULL) {\n            round_backdrop('),
    ('clips-are-made-with-no-picture', PAINT,
     '    if (!p->v->backdrop(p->v->ctx, b, layer, NULL, origin, corner.x, corner.y,\n'
     '                        on_page(p, p->view)))\n        return;\n',
     ''),
    # The corners' squares.
    ('a-right-corner-has-no-square', PAINT,
     'int64_t x0 = c == 0 || c == 3 ? s->l : s->r - rx,',
     'int64_t x0 = c == 0 || c == 3 ? s->l : s->r,'),
    ('a-bottom-corner-has-no-square', PAINT,
     'y0 = c < 2 ? s->t : s->b - ry;',
     'y0 = c < 2 ? s->t : s->b;'),
    ('a-corner-is-its-horizontal-radius-square', PAINT,
     '    int64_t rx = s->rad[c][0], ry = s->rad[c][1];\n    if (rx <= 0 || ry <= 0)\n'
     '        return (os64_gui_rect_t){0, 0, 0, 0};',
     '    int64_t rx = s->rad[c][0], ry = s->rad[c][0];\n    if (rx <= 0 || ry <= 0)\n'
     '        return (os64_gui_rect_t){0, 0, 0, 0};'),
    ('squares-that-meet-are-drawn-apart', PAINT,
     '            met = met || meets(sq[c], sq[d]);',
     '            met = met && meets(sq[c], sq[d]);'),
    # The bands between them.
    ('the-bands-are-not-sorted', PAINT,
     '    for (int i = 1; i < n; i++)\n        for (int j = i; j > 0 && at[j - 1] > at[j]; j--) {',
     '    for (int i = n; i < n; i++)\n        for (int j = i; j > 0 && at[j - 1] > at[j]; j--) {'),
    ('a-left-square-does-not-narrow-its-band', PAINT,
     '                x0 = max64(x0, (int64_t)sq[c].x + sq[c].w);',
     '                x0 = max64(x0, (int64_t)sq[c].x);'),
    ('a-right-square-does-not-narrow-its-band', PAINT,
     '                x1 = min64(x1, sq[c].x);',
     '                x1 = min64(x1, (int64_t)sq[c].x + sq[c].w);'),
    ('a-square-ending-with-the-band-is-left-out', PAINT,
     '                (int64_t)sq[c].y + sq[c].h < at[i + 1])',
     '                (int64_t)sq[c].y + sq[c].h <= at[i + 1])'),
    ('a-square-starting-with-the-band-is-left-out', PAINT,
     'sq[c].y > at[i] ||',
     'sq[c].y >= at[i] ||'),
    ('a-band-is-not-cut-to-the-view', PAINT,
     '        os64_gui_rect_t band = cut(held(x0, at[i], x1, at[i + 1]), p->view);',
     '        os64_gui_rect_t band = held(x0, at[i], x1, at[i + 1]);'),
    ('a-square-is-not-cut-to-the-view', PAINT,
     '        os64_gui_rect_t seen = cut(sq[c], p->view);',
     '        os64_gui_rect_t seen = sq[c];'),
    # An attribute's picture tiles from one corner.
    ('a-part-tiles-from-its-own-corner', PAINT,
     '    (void)p->v->backdrop(p->v->ctx, b, layer, &on, origin, ox, oy, on_page(p, p->view));',
     '    (void)ox, (void)oy;\n'
     '    (void)p->v->backdrop(p->v->ctx, b, layer, &on, origin, on.x, on.y, on_page(p, p->view));'),
    # A corner through its clip.
    ('the-mask-is-not-painted', PAINT,
     '        round_background(&mask, b, shape, 0x00ffffffu);\n',
     '        (void)mask;\n'),
    ('the-mask-is-the-whole-square', PAINT,
     '        round_background(&mask, b, shape, 0x00ffffffu);\n',
     '        fill(&mask, sq.x, sq.y, sq.w, sq.h, 0x00ffffffu);\n'),
    ('the-mask-reaches-past-its-square', PAINT,
     '        mask.view = sq;\n',
     ''),
    ('the-corner-is-drawn-outside-its-clip', PAINT,
     '        p->v->clip_fill(p->v->ctx);\n        backdrop_part(p, b, layer, sq, origin, ox, oy);\n'
     '        p->v->clip_close(p->v->ctx);',
     '        p->v->clip_fill(p->v->ctx);\n        p->v->clip_close(p->v->ctx);\n'
     '        backdrop_part(p, b, layer, sq, origin, ox, oy);'),
    # A corner with no clip to be had.
    ('a-refused-corner-is-drawn-square', PAINT,
     '        if (!round_row(shape, y * SUB + SUB / 2, &a, &e))\n            continue;\n'
     '        // Pixel x\'s middle',
     '        if (!round_row(shape, y * SUB + SUB / 2, &a, &e))\n            continue;\n'
     '        a = sq.x * SUB, e = ((int64_t)sq.x + sq.w) * SUB;\n        // Pixel x\'s middle'),
    ('a-refused-row-takes-pixels-it-touches', PAINT,
     '        int64_t x0 = max64(sq.x, -floor_div(SUB / 2 - a, SUB));',
     '        int64_t x0 = max64(sq.x, floor_div(a, SUB));'),
    ('a-refused-row-ends-a-pixel-late', PAINT,
     '        int64_t x1 = min64((int64_t)sq.x + sq.w, -floor_div(SUB / 2 - e, SUB));',
     '        int64_t x1 = min64((int64_t)sq.x + sq.w, -floor_div(-e, SUB));'),
    ('a-refused-row-is-taken-at-its-top', PAINT,
     '        if (!round_row(shape, y * SUB + SUB / 2, &a, &e))\n            continue;\n'
     '        // Pixel x\'s middle',
     '        if (!round_row(shape, y * SUB, &a, &e))\n            continue;\n'
     '        // Pixel x\'s middle'),
    # Which boxes a moving background is counted behind.
    ('text-is-counted', YONDER,
     '    return b->kind != FLOW_BOX_TEXT && b->kind != FLOW_BOX_LINE && b->kind != FLOW_BOX_MARKER;',
     '    return b->kind != FLOW_BOX_LINE && b->kind != FLOW_BOX_MARKER;'),
    ('a-marker-is-counted', YONDER,
     '    return b->kind != FLOW_BOX_TEXT && b->kind != FLOW_BOX_LINE && b->kind != FLOW_BOX_MARKER;',
     '    return b->kind != FLOW_BOX_TEXT && b->kind != FLOW_BOX_LINE;'),
    ('one-pass-counts-text', YONDER,
     '    if (b == look->owner || !box_draws_background(b) ||\n'
     '        !os64_rect_intersect(flow_box_doc_rect(b, scroll_now()), look->view, &meet))',
     '    if (b == look->owner ||\n'
     '        !os64_rect_intersect(flow_box_doc_rect(b, scroll_now()), look->view, &meet))'),
    ('asking-apart-counts-text', YONDER,
     '    if (b == look->owner || (look->seen && !look->mark) || !box_draws_background(b) ||',
     '    if (b == look->owner || (look->seen && !look->mark) ||'),
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
                                        timeout=TIMEOUT)
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

print(f'\ncorners mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
