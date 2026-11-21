#!/usr/bin/env python3
"""One deliberate break at a time to the paint beneath the picture (VIDEO.md
§ 5, V.7.1): libflow's walk that begins at a cover (flow.c,
flow_visit_groups_from and flow_paint_path) and yonder's cover, its split,
its canvas acknowledgment and its check (yonder.c), against libflow's cover
cases (tools/test_libflow_host.sh --cover, every box of three pages taken as
the cover) and the scripts host's (tools/test_yonder_scripts_host.sh with
YONDER_COVER_ONLY, test_yonder_cover.inc: each covered paint checked to the
pixel, and the window painted whole with covers and without). A mutant is
CAUGHT when the cases fail, MISSED when they pass, which means a test is
missing and gets written. A break that changes nothing a rule promises is
not in the list; the comment after it says which and why. A build failure is
neither.

The shape is test_restyle_mutants.py's: each mutant is applied to the tree's
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/cover-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

FLOW = 'userland/libflow/flow.c'
YONDER = 'userland/apps/yonder/yonder.c'
CASES = 'bash tools/test_libflow_host.sh --cover && YONDER_COVER_ONLY=1 bash tools/test_yonder_scripts_host.sh'

mutants = [
    # libflow's walk: what is before the cover is culled, and only that.
    ('nothing-before-the-cover-culled', FLOW, '        if (i < from_part && tree->layers[i].part != PART_CLOSE) {',
     '        if (0 && i < from_part && tree->layers[i].part != PART_CLOSE) {'),
    ('a-group-holding-the-cover-culled', FLOW, '            if (end < from_part) {',
     '            if (end < from_part || 1) {'),
    ('the-gate-lets-all-through', FLOW, '        if (box != g->from)\n            return;',
     '        if (0)\n            return;'),
    ('the-gate-not-on-the-cover-part', FLOW, '            v.visit = from_gate;', '            v.visit = i < 0 ? from_gate : visit;'),
    ('the-self-part-past-the-gate', FLOW, '                v.visit(v.ctx, b);', '                visit(ctx, b);'),
    ('the-box-itself-in-its-body', FLOW, '    LayerPart split = e == box ? PART_SELF : PART_BODY;',
     '    LayerPart split = e == box ? PART_BODY : PART_BODY;'),
    # yonder's cover: each condition.
    ('a-group-around-the-video-ignored', YONDER, '        flow_paint_path(g.page.tree, b, NULL, 0) != 0)',
     '        (flow_paint_path(g.page.tree, b, NULL, 0) != 0 && 0))'),
    ('a-frame-of-another-size-covers', YONDER,
     '    return fr->pixels != NULL && fr->w > 0 && fr->h > 0 && fr->w == (uint32_t)at->w && fr->h == (uint32_t)at->h;',
     '    return fr->pixels != NULL && fr->w > 0 && fr->h > 0;'),
    ('the-bars-counted-as-covered', YONDER,
     '    if (!video_fitted(v, b, content, &at) || !os64_rect_intersect(at, content, out))',
     '    if (!video_fitted(v, b, content, &at) || !os64_rect_intersect(content, content, out))'),
    ('the-clip-ignored', YONDER, '    return !b->clipped || os64_rect_intersect(*out, flow_box_doc_clip(b, scroll), out);',
     '    return true;'),
    # yonder's split, acknowledgment and check.
    ('a-piece-of-the-rest-left-unpainted', YONDER, '        if (rest[i].w <= 0 || rest[i].h <= 0)\n            continue;',
     '        if (rest[i].w <= 0 || rest[i].h <= 0 || i == 1)\n            continue;'),
    ('a-backdrop-reader-split-across', YONDER, '         backdrop_read_meets(view))) {',
     '         backdrop_read_meets(view) && 0)) {'),
    ('a-backdrop-reader-left-reading-an-old-frame', YONDER,
     '        os64_rect_intersect(covered, part, &covered) && !backdrop_read_meets(part))',
     '        os64_rect_intersect(covered, part, &covered) && (!backdrop_read_meets(part) || 1))'),
    ('a-piece-not-clipped-to-itself', YONDER, '    const yonder_dark_t *dark = s_dark ? &kDark : NULL;\n    bool found = true;',
     '    gl->clip = was;\n    const yonder_dark_t *dark = s_dark ? &kDark : NULL;\n    bool found = true;'),
    ('the-layers-budget-not-asked', YONDER,
     '    if (flow_layer_area_bound(g.page.tree, view, scroll) > (int64_t)YONDER_FILTER_AREA_VIEWS * whole.w * whole.h) {',
     '    if (flow_layer_area_bound(g.page.tree, view, scroll) > (int64_t)YONDER_FILTER_AREA_VIEWS * whole.w * whole.h && 0) {'),
    ('the-canvases-not-acknowledged', YONDER, '    canvases_acknowledge();\n    return refused;',
     '    (void)canvases_acknowledge;\n    return refused;'),
]

# Not run, because it changes nothing a rule promises:
# - the canvas's fill and the root's backgrounds drawn under a cover
#   (paint.c, `cover == NULL &&`): the cover draws over them, so the pixels
#   are the same and only the time is not.
# - a frame marking its box rather than its cover's part (videos_tick): the
#   paint of the box is split, the bars painted the ordinary way, the same
#   pixels again.
# - the line-cut refusal (an ellipsis cutting the video's line): no fixture
#   puts a video on a cut line, and the refusal is a guard on a case the
#   painter's own paint_box draws partly, which the check would show.
# - a canvas the paint drew acknowledged all the same (canvases_acknowledge's
#   `drawn` skip): it matters only for a draw the canvas verb did not
#   consume (no target, or a draw that failed for memory), which no fixture
#   can make; the skip keeps such a canvas's damage pending, as without a
#   cover.
# - the walk never handing the video over (view_paint_parts' painting again
#   whole): yonder's cover is a box the walk paints, so no fixture reaches
#   it; libflow's cover cases hold `found` false for a box no part paints.
# - the check finding no difference (view_paint_parts' compare): every break
#   it exists to see is seen too by the cases' counts or by the window
#   painted whole with covers and without, so no case fails for the check
#   alone; it was seen to find one by hand (the bars counted as covered, a
#   transparent letterbox: `differed 1`), and it is for the P5.

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

print(f'\ncover mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
