#!/usr/bin/env python3
"""One deliberate break at a time to the plane composer
(kernel/src/gui/plane_compose.c: the blend, the overlay union, the row walk,
the picture's place, the place checks), against
tools/test_plane_compose_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_bt_acl_mutants.py's: each mutant is applied to the tree's
copy of the file, the suite is run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it
through tools/mutants_parallel.py with `--with kernel`; the per-mutant logs
are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/plane-compose-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

COMPOSE = 'kernel/src/gui/plane_compose.c'
CASES = 'bash tools/test_plane_compose_host.sh 3000'

mutants = [
    ('the-blend-rounded-down', '(255 - a) + 0x0080008000800080ull;', '(255 - a) + 0x007F007F007F007Full;'),
    ('the-blend-rounded-up', '(255 - a) + 0x0080008000800080ull;', '(255 - a) + 0x0081008100810081ull;'),
    ('the-division-without-its-correction', 'uint64_t v = (((t + ((t >> 8) & LANES_LOW)) >> 8) & LANES_LOW) + spread(content);',
     'uint64_t v = ((t >> 8) & LANES_LOW) + spread(content);'),
    ('a-lane-not-held-at-255', 'uint64_t over = v & 0x0100010001000100ull;', 'uint64_t over = v & 0;'),
    ('the-spread-drops-green-and-the-top-byte', 'return (uint64_t)(x & 0x00FF00FFu) | ((uint64_t)(x & 0xFF00FF00u) << 24);',
     'return (uint64_t)(x & 0x00FF00FFu);'),
    ('the-gather-drops-green-and-the-top-byte', 'return (uint32_t)(v & 0x00FF00FFu) | (uint32_t)((v >> 24) & 0xFF00FF00u);',
     'return (uint32_t)(v & 0x00FF00FFu);'),
    ('an-overlay-run-counted-short', 'counts->blended += (uint64_t)n;', 'counts->blended += (uint64_t)n - 1;'),
    ('a-see-through-pixel-blended', '\tif (a == 0)\n\t\treturn picture;\n', ''),
    ('an-overlay-a-column-wider', 'int32_t x0 = max32(o->x, cx0), x1 = min32(o->x + o->w, cx1);',
     'int32_t x0 = max32(o->x, cx0), x1 = min32(o->x + o->w + 1, cx1);'),
    ('an-overlay-a-row-taller', 'y < o->y || y >= o->y + o->h)', 'y < o->y || y > o->y + o->h)'),
    ('the-picture-blended-outside-the-overlays',
     '\t\tif (x < in1)\n\t\t\tpicture_run(drow, content, picture, place, y, x, in1, ax0, counts);',
     '\t\tif (x < in1)\n\t\t\toverlay_run(drow, content, picture, place, y, x, in1, ax0, counts);'),
    ('overlapping-overlays-not-merged', 'if (merged > 0 && out[i].x0 <= out[merged - 1].x1) {', 'if (0) {'),
    ('the-picture-placed-at-the-clips-corner',
     'return picture->pixels + (size_t)py * picture->pitch_px + ((int64_t)*px0 - ox);',
     'return picture->pixels + (size_t)py * picture->pitch_px + ((int64_t)*px0 - place->clip.x);'),
    ('the-picture-read-past-its-width', '*px1 = (int32_t)max64(min64(x1, ox + picture->w), *px0);',
     '*px1 = x1;'),
    ('the-clips-last-row-past-it', 'if (clip_empty || y < cy0 || y >= cy1 || ax1 <= cx0 || ax0 >= cx1) {',
     'if (clip_empty || y < cy0 || y > cy1 || ax1 <= cx0 || ax0 >= cx1) {'),
    ('the-content-left-of-the-clip-not-drawn', '\t\tif (in0 > ax0)\n\t\t\tcopy_px(drow, crow + ax0, in0 - ax0);\n', ''),
    ('the-union-a-row-short', 'y1 = max32(y1, min32(o->y + o->h, place->clip.y + place->clip.h));',
     'y1 = max32(y1, min32(o->y + o->h, place->clip.y + place->clip.h) - 1);'),
    ('an-overlay-outside-its-clip-accepted', 'if (!rect_sane(&place->overlay[i]) || !rect_inside(&place->overlay[i], &place->clip))',
     'if (!rect_sane(&place->overlay[i]))'),
    ('the-budget-one-short', 'return plane_overlay_area(place) <= overlay_budget;', 'return plane_overlay_area(place) < overlay_budget;'),
    ('a-negative-height-sane', 'return r->w >= 0 && r->h >= 0 &&', 'return r->w >= 0 &&'),
    ('an-overflowing-edge-sane', '(int64_t)r->x + r->w <= INT32_MAX && (int64_t)r->y + r->h <= INT32_MAX', '1'),
    ('a-clip-past-the-content-accepted', '!rect_inside(&place->clip, &content))', 'content.w < 0)'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / COMPOSE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {COMPOSE}', file=sys.stderr)
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
            print(f'{name}: {COMPOSE} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nplane compose mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
