#!/usr/bin/env python3
"""One deliberate break at a time to yonder's plane core
(userland/apps/yonder/plane_place.c: the reasons and their order, whether
each wants the plane kept current, and the map of what lies over a
picture), against tools/test_plane_place_host.sh: a mutant is CAUGHT when
the suite fails, MISSED when it passes, which means a test is missing and
gets written. A build failure is neither.

The shape is test_media_show_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from
the bytes read before the change, checked byte for byte, whatever happens.
Run it through tools/mutants_parallel.py; the per-mutant logs are in
--logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/plane-place-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CORE = 'userland/apps/yonder/plane_place.c'
CASES = 'bash tools/test_plane_place_host.sh'

mutants = [
    ('out-of-view-wanted', '        v.reason = YONDER_PLANE_OUT_OF_VIEW;\n        v.want = false;',
     '        v.reason = YONDER_PLANE_OUT_OF_VIEW;'),
    ('out-of-view-placed', '    if (!in->on_view) {', '    if (0) {'),
    ('a-group-placed', '    } else if (!in->cover) {', '    } else if (0) {'),
    ('a-backdrop-reader-placed', '    } else if (in->backdrop) {', '    } else if (0) {'),
    ('meeting-clips-placed', '    } else if (in->clip_meets) {', '    } else if (0) {'),
    ('no-layer-placed', '    } else if (in->not_layer) {', '    } else if (0) {'),
    ('over-budget-placed', '    } else if (in->over_budget) {', '    } else if (0) {'),
    ('unpainted-placed', '    } else if (in->unpainted) {', '    } else if (0) {'),
    ('placed-before-ready', '    } else if (!in->ready || !in->shown) {', '    } else if (!in->shown) {'),
    ('placed-on-an-old-picture', '    } else if (!in->ready || !in->shown) {', '    } else if (!in->ready) {'),
    ('a-refusal-not-held-off', '    } else if (in->held_off) {', '    } else if (0) {'),
    ('no-layer-asked-before-the-backdrop', '    } else if (in->backdrop) {\n        v.reason = YONDER_PLANE_BACKDROP;',
     '    } else if (in->backdrop && !in->not_layer) {\n        v.reason = YONDER_PLANE_BACKDROP;'),
    ('a-reason-misnamed', '"not a layer"', '"no layer"'),
    # The layer.
    ('coverage-from-red', '        if (a[1] > 0)\n            counts->inked++;\n        out[i] = (uint32_t)a[1] << 24',
     '        if (a[0] > 0)\n            counts->inked++;\n        out[i] = (uint32_t)a[0] << 24'),
    ('disagreement-not-counted', '        if (hi - lo > 1) {', '        if (hi - lo > 255) {'),
    ('disagreement-too-strict', '        if (hi - lo > 1) {', '        if (hi - lo > 0) {'),
    ('colour-from-the-white-pass', '    out[i] = (uint32_t)a[1] << 24 | (b[i] & 0x00ffffffu);', '    out[i] = (uint32_t)a[1] << 24 | (w[i] & 0x00ffffffu);'),
    ('coverage-not-clamped-high', '            a[c] = v < 0 ? 0 : v > 255 ? 255 : v;', '            a[c] = v < 0 ? 0 : v;'),
    # The overlays.
    ('runs-not-grown-down', '                if (runs[k].x == run.x && runs[k].w == run.w && runs[k].y + runs[k].h == run.y) {',
     '                if (0) {'),
    ('runs-grown-across-a-gap', '                if (runs[k].x == run.x && runs[k].w == run.w && runs[k].y + runs[k].h == run.y) {',
     '                if (runs[k].x == run.x && runs[k].w == run.w) {'),
    ('the-cap-ignored', '    while (n > max) {', '    while (n > max + 4) {'),
    ('joined-past-the-cap', '    while (n > max) {', '    while (n > 1) {'),
    ('the-area-a-bounding-box', '    *area = yonder_plane_union_area(out, n);\n    return n;',
     '    *area = n ? (uint64_t)all.w * (uint64_t)all.h : 0;\n    return n;'),
    ('forget-forgets-nothing', '            m->tiles[(size_t)r * m->cols + c] = YONDER_PLANE_TILE_UNKNOWN;',
     '            (void)0;'),
    ('join-past-the-cap', '    while (n > max) {\n        uint32_t bi = 0, bj = 1;', '    while (n > max + 1) {\n        uint32_t bi = 0, bj = 1;'),
    ('join-the-dearest', '                if (cost < best) {', '                if (cost > best || best == INT64_MAX) {'),
    ('spread-off-by-one', '            counts->spread[hi - lo >= 5 ? 3 : hi - lo - 2]++;', '            counts->spread[hi - lo >= 5 ? 3 : hi - lo - 1 > 3 ? 3 : hi - lo - 1]++;'),
    ('first-not-the-first', '            if (counts->disagree == 0) {', '            if (1) {'),
    ('a-cell-counted-twice', '                    area += (uint64_t)(xs[i + 1] - xs[i]) * (uint64_t)(ys[j + 1] - ys[j]);\n                    break;',
     '                    area += (uint64_t)(xs[i + 1] - xs[i]) * (uint64_t)(ys[j + 1] - ys[j]);'),
    ('a-partial-paint-clears', '            else if (whole)\n', '            else if (whole || 1)\n'),
    ('a-whole-clean-paint-keeps-ink', '            else if (whole)\n                *t = YONDER_PLANE_TILE_CLEAR;',
     '            else if (whole && *t != YONDER_PLANE_TILE_INKED)\n                *t = YONDER_PLANE_TILE_CLEAR;'),
    ('ink-not-recorded', '                *t = YONDER_PLANE_TILE_INKED;\n            else', '                (void)0;\n            else'),
    ('a-tile-not-cut-to-the-clip', '    return cut(tile, m->origin, &in_origin) && cut(in_origin, clip, out);',
     '    return cut(tile, m->origin, &in_origin) && ((void)clip, *out = in_origin, true);'),
    ('the-last-column-dropped', '    *c1 = (uint32_t)(in.x + in.w - m->origin.x + YONDER_PLANE_TILE - 1) / YONDER_PLANE_TILE;',
     '    *c1 = (uint32_t)(in.x + in.w - m->origin.x) / YONDER_PLANE_TILE;'),
    ('the-last-row-dropped', '    *r1 = (uint32_t)(in.y + in.h - m->origin.y + YONDER_PLANE_TILE - 1) / YONDER_PLANE_TILE;',
     '    *r1 = (uint32_t)(in.y + in.h - m->origin.y) / YONDER_PLANE_TILE;'),
    ('known-ignores-the-unknown', '            if (inked ? t == YONDER_PLANE_TILE_INKED : t == YONDER_PLANE_TILE_UNKNOWN)',
     '            if (inked ? t == YONDER_PLANE_TILE_INKED : 0)'),
    ('inked-sees-nothing', '            if (inked ? t == YONDER_PLANE_TILE_INKED : t == YONDER_PLANE_TILE_UNKNOWN)',
     '            if (inked ? 0 : t == YONDER_PLANE_TILE_UNKNOWN)'),
    ('a-reset-keeps-old-tiles', '    for (size_t i = 0; i < n; i++)\n        tiles[i] = YONDER_PLANE_TILE_UNKNOWN;', '    (void)0;'),
    ('a-small-buffer-accepted', '    if (n == 0 || n > cap || tiles == NULL)', '    if (n == 0 || n > cap + 4096 || tiles == NULL)'),
    ('a-clip-off-the-picture-known', '    return m->tiles != NULL && cut(clip, m->origin, &in) && !any_tile(m, clip, false);',
     '    (void)in;\n    return m->tiles != NULL && !any_tile(m, clip, false);'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / CORE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {CORE}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=120)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {CORE} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h):\d+:\d+: error:', body) is None and 'undefined reference' not in body
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

print(f'\nplane place mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
