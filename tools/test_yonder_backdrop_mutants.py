#!/usr/bin/env python3
"""One deliberate break at a time to yonder's moving-background layer
(docs/design/pending/BACKDROP_LAYER.md): the pixel's arithmetic and its
rounding, the runs of changed pixels, the compose's clips (the view, the
box's part, the skips), where tile copies fall, the damage it reports, and
every rule of the policy that keeps the layer from ever being dearer than a
paint — against tools/test_yonder_backdrop_host.sh. A mutant is CAUGHT when
the cases fail, MISSED when they pass, which means a test is missing and
gets written. A build failure is neither.

The shape is test_yonder_scratch_mutants.py's: each mutant is applied to the
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-backdrop-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
args.logs.mkdir(parents=True, exist_ok=True)

root = pathlib.Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], text=True).strip())
BACKDROP = 'userland/apps/yonder/backdrop.c'
YONDER = 'userland/apps/yonder/yonder.c'
SCALE = 'userland/apps/yonder/scale.c'
CASES = {
    BACKDROP: 'bash tools/test_yonder_backdrop_host.sh',
    # yonder's half: the scripted-page harness plays a moving body through
    # the real loop's order and holds every frame to an ordinary paint.
    YONDER: 'bash tools/test_yonder_scripts_host.sh',
    SCALE: 'bash tools/test_yonder_host.sh',
}

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The pixel: F = K + (W - K) b / 255, out = P + (F - P) a / 255, rounded.
    ('rounding-truncates', BACKDROP,
     '    return v >= 0 ? (v + 127) / 255 : -((-v + 127) / 255);',
     '    return v >= 0 ? v / 255 : -(-v / 255);'),
    ('negative-rounds-toward-minus', BACKDROP,
     '    return v >= 0 ? (v + 127) / 255 : -((-v + 127) / 255);',
     '    return v >= 0 ? (v + 127) / 255 : (v - 127) / 255 - 1;'),
    ('front-ignores-the-layer', BACKDROP,
     '        int32_t f = kc + div255((wc - kc) * bc);',
     '        int32_t f = kc + 0 * (wc + bc);'),
    ('front-from-white', BACKDROP,
     '        int32_t f = kc + div255((wc - kc) * bc);',
     '        int32_t f = wc - div255((wc - kc) * bc);'),
    ('alpha-ignored', BACKDROP,
     '        int32_t c = pc + div255((f - pc) * a);',
     '        int32_t c = pc + div255((f - pc) * (a | 255));'),
    ('what-shows-through-ignored', BACKDROP,
     '        int32_t c = pc + div255((f - pc) * a);',
     '        int32_t c = div255(f * a) + 0 * pc;'),
    ('channels-share-one', BACKDROP,
     '    for (int sh = 0; sh <= 16; sh += 8) {\n        int32_t kc',
     '    for (int sh = 0; sh <= 0; sh += 8) {\n        int32_t kc'),
    # The runs.
    ('runs-end-one-early', BACKDROP,
     '            while (x < w && a[x] != b[x])',
     '            while (x + 1 < w && a[x + 1] != b[x + 1])'),
    ('changed-not-counted', BACKDROP,
     '            px += x - from;',
     '            px += 1;'),
    ('overflow-not-said', BACKDROP,
     '    return n <= cap ? n : -1;',
     '    return n <= cap ? n : cap;'),
    ('overflow-said-one-early', BACKDROP,
     '    return n <= cap ? n : -1;',
     '    return n < cap ? n : -1;'),
    # Where the moving layer's pixels fall.
    ('wrap-can-go-negative', BACKDROP,
     '    return (int32_t)(r < 0 ? r + m : r);',
     '    return (int32_t)r;'),
    ('no-repeat-x-still-repeats', BACKDROP,
     '    if (!t->repeat_x && (u < 0 || u >= t->w))\n        return 0;',
     '    if (false)\n        return 0;'),
    ('no-repeat-y-still-repeats', BACKDROP,
     '    if (!t->repeat_y && (v < 0 || v >= t->h))\n        return NULL;',
     '    if (false)\n        return NULL;'),
    ('off-the-copy-is-opaque-black', BACKDROP,
     '    if (row == NULL)\n        return 0;',
     '    if (row == NULL)\n        return 0xff000000u;'),
    ('copies-start-at-the-part', BACKDROP,
     '    *first = (int64_t)from - wrap((int64_t)from - origin, size);',
     '    *first = from;'),
    ('one-copy-short', BACKDROP,
     '    *count = ((int64_t)from + len - *first + size - 1) / size;',
     '    *count = ((int64_t)from + len - *first) / size;'),
    ('runs-ignore-no-repeat', BACKDROP,
     '    if (!repeat) {\n        *first = origin;\n        *count = 1;\n        return;\n    }',
     '    (void)repeat;'),
    # The compose's clips.
    ('area-past-the-view', BACKDROP,
     '    os64_gui_rect_t a = meet(meet(area, part), (os64_gui_rect_t){0, 0, s->width, s->height});',
     '    os64_gui_rect_t a = meet(area, part);'),
    ('area-ignores-the-part', BACKDROP,
     '    os64_gui_rect_t a = meet(meet(area, part), (os64_gui_rect_t){0, 0, s->width, s->height});',
     '    (void)part;\n    os64_gui_rect_t a = meet(area, (os64_gui_rect_t){0, 0, s->width, s->height});'),
    ('runs-past-the-view', BACKDROP,
     '    part = meet(part, (os64_gui_rect_t){0, 0, s->width, s->height});\n',
     ''),
    ('runs-past-the-part-left', BACKDROP,
     '                if (x0 < part.x)\n                    x0 = part.x;',
     ''),
    ('runs-past-the-part-right', BACKDROP,
     '                if (x1 > px1)\n                    x1 = px1;',
     '                (void)px1;'),
    ('runs-past-the-part-rows', BACKDROP,
     '                if (y < part.y || y >= py1)\n                    continue;',
     '                if (y < 0 || y >= py1)\n                    continue;'),
    ('skips-ignored', BACKDROP,
     '        for (uint32_t i = 0; i < o->nskip; i++) {',
     '        for (uint32_t i = o->nskip; i < o->nskip; i++) {'),
    ('skip-rows-ignored', BACKDROP,
     '            if (r.w <= 0 || r.h <= 0 || y < r.y || (int64_t)y >= (int64_t)r.y + r.h)',
     '            if (r.w <= 0 || r.h <= 0)'),
    ('skip-ends-one-early', BACKDROP,
     '                x0 = (int32_t)(rx1 < x1 ? rx1 : x1);',
     '                x0 = (int32_t)(rx1 - 1 < x1 ? rx1 - 1 : x1);'),
    ('next-skip-not-stopped-at', BACKDROP,
     '            if (r.x > x0 && r.x < next)\n                next = r.x;',
     ''),
    # What it reports.
    ('written-not-counted', BACKDROP,
     '    *written += x1 - x0;',
     '    (void)written;'),
    ('damage-one-row-short', BACKDROP,
     '    box->y1 = max32(box->y1, y + 1);',
     '    box->y1 = max32(box->y1, y);'),
    ('damage-one-column-short', BACKDROP,
     '    box->x1 = max32(box->x1, x1);',
     '    box->x1 = max32(box->x1, x1 - 1);'),
    ('damage-not-reported', BACKDROP,
     '    (void)os64_damage_add(o->damage, &o->ndamage, o->cap, o->slack, r);',
     '    (void)r;'),
    ('runs-report-once-not-per-copy', BACKDROP,
     '            report(out, &box);\n        }\n    }\n}',
     '        }\n    }\n}'),
    # The policy.
    ('build-before-measured', BACKDROP,
     '    return !p->unprofitable && p->paint_cost > 0 && p->quiet >= p->need;',
     '    return !p->unprofitable && p->quiet >= p->need;'),
    ('build-when-given-up', BACKDROP,
     '    return !p->unprofitable && p->paint_cost > 0 && p->quiet >= p->need;',
     '    return p->paint_cost > 0 && p->quiet >= p->need;'),
    ('build-one-quiet-frame-early', BACKDROP,
     '    return !p->unprofitable && p->paint_cost > 0 && p->quiet >= p->need;',
     '    return !p->unprofitable && p->paint_cost > 0 && p->quiet + 1 >= p->need;'),
    ('ordinary-keeps-quiet', BACKDROP,
     '    p->quiet = 0;\n}',
     '    (void)p;\n}'),
    ('quiet-not-counted', BACKDROP,
     '    if (p->quiet < UINT32_MAX)\n        p->quiet++;',
     '    (void)p;'),
    ('rebuild-costs-one-paint', BACKDROP,
     '    double cost = (double)changed * p->compose_cost + 3.0 * (double)invalid * p->pass_cost;',
     '    double cost = (double)changed * p->compose_cost + (double)invalid * p->pass_cost;'),
    ('compose-whatever-it-costs', BACKDROP,
     '    bool cheaper = cost <= (double)part * p->paint_cost;',
     '    bool cheaper = cost == cost || part == 0;'),
    ('dearer-counted-while-invalid', BACKDROP,
     '    if (invalid == 0) {\n        p->dearer',
     '    if (true) {\n        p->dearer'),
    ('dearer-run-never-resets', BACKDROP,
     '        p->dearer = cheaper ? 0 : p->dearer + 1;',
     '        p->dearer = cheaper ? p->dearer : p->dearer + 1;'),
    ('never-given-up', BACKDROP,
     '        if (p->dearer >= YONDER_BACKDROP_DEARER)\n            p->unprofitable = true;',
     ''),
    ('rebuild-not-charged', BACKDROP,
     '    p->saved_cost += (double)part * p->paint_cost - spent - rebuild_cost;',
     '    p->saved_cost += (double)part * p->paint_cost - spent;'),
    ('paid-at-once', BACKDROP,
     '    if (!p->paid && p->saved_cost >= p->build_cost) {',
     '    if (!p->paid) {'),
    ('paying-keeps-the-back-off', BACKDROP,
     '        p->paid = true;\n        p->need = YONDER_BACKDROP_QUIET;',
     '        p->paid = true;'),
    ('a-new-build-keeps-old-savings', BACKDROP,
     '    p->build_cost = spent;\n    p->saved_cost = 0;',
     '    p->build_cost = spent;'),
    ('waste-never-backs-off', BACKDROP,
     '    if (!p->paid)\n        p->need = p->need * 2 < YONDER_BACKDROP_QUIET_MOST ? p->need * 2 : YONDER_BACKDROP_QUIET_MOST;',
     ''),
    ('back-off-unbounded', BACKDROP,
     'p->need * 2 < YONDER_BACKDROP_QUIET_MOST ? p->need * 2 : YONDER_BACKDROP_QUIET_MOST;',
     'p->need * 2;'),
    ('a-paid-loss-backs-off', BACKDROP,
     '    if (!p->paid)\n        p->need =',
     '    if (true)\n        p->need ='),
    # yonder: which backgrounds are held, and which refused.
    ('text-boxes-draw-backgrounds', YONDER,
     '    return b->kind != FLOW_BOX_TEXT && b->kind != FLOW_BOX_LINE && b->kind != FLOW_BOX_MARKER;',
     '    return b->kind == b->kind;'),
    ('small-boxes-held', YONDER,
     'if (s_layer.offer.area * BACKDROP_SHARE < rect_area(v) || g.page.tree == NULL)',
     'if (rect_area(v) < 0 || g.page.tree == NULL)'),
    ('a-layer-above-not-refused', YONDER,
     '            s_layer.refused = "a layer of its box drawn above it";',
     '            (void)0;'),
    ('a-sheet-not-refused', YONDER,
     '    if (gl != s_glass_root)\n        s_layer.through = true;',
     '    if (gl == NULL)\n        s_layer.through = true;'),
    # The three paintings.
    ('black-painted-white', YONDER,
     '        os64_draw_fill_rect(gl->surf, r, gl->pass == GLASS_BLACK ? 0xff000000u : 0xffffffffu);',
     '        os64_draw_fill_rect(gl->surf, r, 0xffffffffu);'),
    ('without-filled-too', YONDER,
     '        if (gl->pass == GLASS_WITHOUT)\n            return true;',
     '        if (false)\n            return true;'),
    ('copy-laid-from-the-view', YONDER,
     's_layer.at.y - g.sy',
     's_layer.at.y'),
    # The quiet.
    ('never-quiet-with-anything-in-front', YONDER,
     '    if (s_layer.disturbed <= rect_area(part) / BACKDROP_QUIET_SHARE)',
     '    if (s_layer.disturbed <= 0)'),
    ('a-disturbance-never-forgotten', YONDER,
     '    s_layer.disturbed = 0;',
     ''),
    # What is stale, and what is painted again for it.
    ('a-change-not-noted', YONDER,
     '(void)os64_damage_add(s_layer.invalid, &s_layer.ninvalid, BACKDROP_INVALID, OS64_UI_DAMAGE_SLACK, r);',
     '(void)r;'),
    ('the-frames-own-paint-noted', YONDER,
     'if (s_layer.node == NULL || s_layer.refused != NULL || s_layer.frame_painting)',
     'if (s_layer.node == NULL || s_layer.refused != NULL)'),
    ('others-not-noted-before-the-holes', YONDER,
     '    if (nholes > 0)\n        backdrop_others();',
     '    (void)0;'),
    ('others-not-noted-before-the-frame', YONDER,
     '    backdrop_others();\n    s_layer.measuring = rect_area(part);',
     '    s_layer.frame_painting = true;\n    s_layer.measuring = rect_area(part);'),
    ('invalid-not-painted-again', YONDER,
     '        backdrop_paintings(rebuilt[i]);',
     '        (void)rebuilt[i];'),
    ('painted-again-not-composed', YONDER,
     'yonder_backdrop_compose_area(dst, pitch, &sheets, &tile, part, rebuilt[i], &out);',
     '(void)rebuilt[i];'),
    ('invalid-never-taken', YONDER,
     '    s_layer.ninvalid = 0;\n    uint64_t began = cycles();',
     '    uint64_t began = cycles();'),
    # Not here: marking the layer built only after the build's compose. It
    # drops what the build frame notes as changed, but the build painted the
    # whole part after those changes, so nothing it drops is stale.
    # Memory.
    ('a-stopped-picture-keeps-the-layer', YONDER,
     '            !g.page.pics[s_layer.pic].playing)\n            backdrop_unbuild();',
     '            !g.page.pics[s_layer.pic].playing)\n            (void)0;'),
    # One copy of the tile as laid.
    ('a-copy-never-smoothed', SCALE,
     '            out[u] = smooth ? smooth_at(src, sw, sh, (uint32_t)u, tw, (uint32_t)v, th)',
     '            out[u] = false ? smooth_at(src, sw, sh, (uint32_t)u, tw, (uint32_t)v, th)'),
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
                result = subprocess.run(['bash', '-c', CASES[rel]], cwd=root, stdout=out, stderr=out,
                                        timeout=120)
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

print(f'\nyonder backdrop mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
