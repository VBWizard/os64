#!/usr/bin/env python3
"""One deliberate break at a time to yonder's pictures kept at the size they
are drawn (docs/design/pending/PICTURE_SIZES.md): the kept size and the
reduction (scale.c), the worker that keeps a picture smaller and lets the
whole decode go (picture.c), and the page that keeps a picture's natural size
apart from its kept one, finds each picture's need after a layout, judges it
by the design's two rules, and swaps a resample in — against the scripted
host's picture_sizes_cases (tools/test_yonder_picture_sizes.inc). A mutant is
CAUGHT when they fail, MISSED when they pass, which means a test is missing
and gets written. A build failure is neither.

The shape is test_yonder_data_pictures_mutants.py's: each mutant is applied
to the worktree's copy of its file, the cases are run, and the file is put
back from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs. --check only says
whether every mutant's text is found exactly once.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/picture-sizes-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
parser.add_argument('--check', action='store_true', help='only find each mutant\'s text')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent

YONDER = 'userland/apps/yonder/yonder.c'
PICTURE = 'userland/apps/yonder/picture.c'
SCALE = 'userland/apps/yonder/scale.c'
CASES = 'bash tools/test_yonder_scripts_host.sh'
TIMEOUT = 1800

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The kept size: one factor, the axis that asks more, never past whole.
    ('the-kept-size-meets-the-wrong-axis', SCALE,
     '    if ((uint64_t)dw * nh >= (uint64_t)dh * nw) {',
     '    if ((uint64_t)dw * nh < (uint64_t)dh * nw) {'),
    ('the-kept-size-rounds-down', SCALE,
     '        h = ((uint64_t)nh * dw + nw - 1) / nw;',
     '        h = ((uint64_t)nh * dw) / nw;'),
    ('the-kept-size-passes-whole', SCALE,
     '    *kw = w < nw ? (uint32_t)w : nw;',
     '    *kw = (uint32_t)w;'),
    ('no-rectangle-keeps-nothing', SCALE,
     '    if (nw == 0 || nh == 0 || dw == 0 || dh == 0) {\n        *kw = nw;',
     '    if (nw == 0 || nh == 0) {\n        *kw = nw;'),
    # The reduction: every source pixel under an output pixel, by area and alpha.
    ('the-reduction-ignores-alpha', SCALE,
     '                    uint64_t wa = (right - left) * wy * (p >> 24);',
     '                    uint64_t wa = (right - left) * wy * 255u;'),
    ('the-reduction-weighs-rows-alike', SCALE,
     '            uint64_t wy = bottom - top;',
     '            uint64_t wy = dh;'),
    ('the-reduction-truncates', SCALE,
     '                                   (uint32_t)((s[1] + a / 2) / a) << 16 |',
     '                                   (uint32_t)(s[1] / a) << 16 |'),
    # The worker.
    ('the-worker-keeps-it-whole', PICTURE,
     '        keep_smaller(job, cancelled, context, p);',
     '        (void)keep_smaller;'),
    ('the-natural-size-is-the-kept-one', PICTURE,
     '    p->image.height = kh;\n}',
     '    p->image.height = kh;\n    p->width = kw;\n    p->height = kh;\n}'),
    ('the-cost-is-whole', PICTURE,
     '    p->cost = (size_t)p->image.width * p->image.height * 4u;',
     '    p->cost = (size_t)p->width * p->height * 4u;'),
    ('the-whole-decode-is-kept', PICTURE,
     '    os64_image_free(&p->image);\n    p->image.pixels = small;',
     '    p->image.pixels = small;'),
    ('a-failed-reduction-leaks', PICTURE,
     '        os64_free(small);\n        return;',
     '        return;'),
    # Natural size for layout, kept size for the painters.
    ('layout-reads-the-kept-size', YONDER,
     '    picture_natural(&p->pics[p->pic_of[i]], &pw, &ph);\n    if (pw == 0 || ph == 0)',
     '    (void)picture_pixels(&p->pics[p->pic_of[i]], &pw, &ph);\n    if (pw == 0 || ph == 0)'),
    ('object-fit-reads-the-kept-size', YONDER,
     '    picture_natural(pic, &w, &h);\n    return yonder_object_rect(',
     '    (void)picture_pixels(pic, &w, &h);\n    return yonder_object_rect('),
    ('a-tile-reads-the-kept-size', YONDER,
     '    picture_natural(pic, &w, &h);\n    uint32_t zw = zoomed_px(w, p->laid_zoom)',
     '    (void)picture_pixels(pic, &w, &h);\n    uint32_t zw = zoomed_px(w, p->laid_zoom)'),
    ('a-picture-past-the-memory-has-no-size', YONDER,
     '        (p->pics[p->pic_of[i]].state != PIC_SHOWN && p->pics[p->pic_of[i]].state != PIC_NOT_KEPT))',
     '        (p->pics[p->pic_of[i]].state != PIC_SHOWN))'),
    # The first ask.
    ('the-first-ask-is-whole', YONDER,
     '        pic->id = picture_submit(p, index, pic->cover_whole ? 0 : pic->cover_w,\n'
     '                                 pic->cover_whole ? 0 : pic->cover_h, false, &why);',
     '        pic->id = picture_submit(p, index, 0,\n'
     '                                 0, false, &why);'),
    ('a-waiting-box-is-asked-small', YONDER,
     '            if (c.w <= 0 || c.h <= 0 || b->style->pixelated ||\n'
     '                (fit != FLOW_OBJECT_FILL && fit != FLOW_OBJECT_CONTAIN && fit != FLOW_OBJECT_COVER)) {\n'
     '                p->pics[k].cover_whole = true;',
     '            if (c.w <= 0 || c.h <= 0 || b->style->pixelated ||\n'
     '                (fit != FLOW_OBJECT_FILL && fit != FLOW_OBJECT_CONTAIN && fit != FLOW_OBJECT_COVER)) {\n'
     '                (void)0;'),
    ('rd1-a-pixelated-box-is-asked-small', YONDER,
     '            if (c.w <= 0 || c.h <= 0 || b->style->pixelated ||',
     '            if (c.w <= 0 || c.h <= 0 ||'),
    ('the-first-ask-takes-the-last-box', YONDER,
     '                if ((uint32_t)c.w > p->pics[k].cover_w)\n                    p->pics[k].cover_w = (uint32_t)c.w;',
     '                p->pics[k].cover_w = (uint32_t)c.w;'),
    # The need.
    ('the-need-is-the-last-use', YONDER,
     '    if (kw > pic->need_w)\n        pic->need_w = kw;',
     '    pic->need_w = kw;'),
    ('the-need-outlives-its-layout', YONDER,
     '        p->pics[k].need_w = p->pics[k].need_h = 0;\n    bool fresh',
     '        (void)0;\n    bool fresh'),
    ('an-svg-has-a-need', YONDER,
     '    if (pic->kind != YONDER_PICTURE_RASTER || (pic->state != PIC_SHOWN && pic->state != PIC_NOT_KEPT) ||',
     '    if ((pic->state != PIC_SHOWN && pic->state != PIC_NOT_KEPT) ||'),
    ('the-canvas-is-tiled-in-its-own-box', YONDER,
     '        const flow_box_t *in = b == walk->owner && walk->root != NULL ? walk->root : b;',
     '        const flow_box_t *in = b;'),
    # The judge.
    ('it-shrinks-at-any-size', YONDER,
     '                owed = have >= 2 * need ||',
     '                owed = have > need ||'),
    ('it-never-shrinks', YONDER,
     '                owed = have >= 2 * need ||',
     '                owed = false ||'),
    ('it-never-grows', YONDER,
     '                       (grow && bytes <= PICTURES_KEPT_MAX - p->kept_bytes + pic->cost);',
     '                       (false && grow && bytes <= PICTURES_KEPT_MAX - p->kept_bytes + pic->cost);'),
    ('it-grows-past-the-memory', YONDER,
     '                       (grow && bytes <= PICTURES_KEPT_MAX - p->kept_bytes + pic->cost);',
     '                       grow;'),
    ('a-picture-past-the-memory-stays-there', YONDER,
     '                owed = bytes <= PICTURES_KEPT_MAX - p->kept_bytes;',
     '                owed = false;'),
    ('a-refused-size-is-asked-again', YONDER,
     '            (pic->need_w != pic->refused_w || pic->need_h != pic->refused_h)) {',
     '            true) {'),
    ('an-owed-resample-is-never-withdrawn', YONDER,
     '        } else if (pic->resample_owed) {\n            pic->resample_owed = false;\n            p->resample_owed--;',
     '        } else if (false) {\n            pic->resample_owed = false;\n            p->resample_owed--;'),
    # The feed and the arrival.
    ('a-resample-is-asked-as-new', YONDER,
     '        pic->resample_id = picture_submit(p, k, pic->want_w, pic->want_h, true, &why);',
     '        pic->resample_id = picture_submit(p, k, pic->want_w, pic->want_h, false, &why);'),
    ('a-resample-arrives-as-new', YONDER,
     '    if (job->resample) {\n        picture_resampled(job, product);',
     '    if (false && job->resample) {\n        picture_resampled(job, product);'),
    ('a-resample-keeps-its-slot', YONDER,
     '    pic->resampling = false;\n    g.page.in_flight--;',
     '    pic->resampling = false;'),
    ('a-failed-resample-is-not-remembered', YONDER,
     '        picture_fact(pic, "could not be had again at the size it is drawn");\n'
     '        pic->refused_w = job->cover_w;\n        pic->refused_h = job->cover_h;',
     '        picture_fact(pic, "could not be had again at the size it is drawn");'),
    ('a-failed-resample-drops-the-picture', YONDER,
     '        picture_fact(pic, "could not be had again at the size it is drawn");',
     '        picture_fact(pic, "could not be had again at the size it is drawn");\n'
     '        os64_image_free(&pic->image);'),
    ('a-resample-past-the-memory-is-taken', YONDER,
     '    } else if (product->cost > PICTURES_KEPT_MAX - (g.page.kept_bytes - old)) {',
     '    } else if (false) {'),
    ('a-resample-leaks-the-old-pixels', YONDER,
     '        os64_image_free(&pic->image);\n        pic->image = product->image;',
     '        pic->image = product->image;'),
    ('a-resample-shares-the-product', YONDER,
     '        pic->image = product->image;\n        os64_memset(&product->image, 0, sizeof(product->image));\n'
     '        g.page.kept_bytes = g.page.kept_bytes - old + product->cost;',
     '        pic->image = product->image;\n'
     '        g.page.kept_bytes = g.page.kept_bytes - old + product->cost;'),
    ('a-resample-adds-to-the-account', YONDER,
     '        g.page.kept_bytes = g.page.kept_bytes - old + product->cost;',
     '        g.page.kept_bytes += product->cost;'),
    ('a-recovered-picture-stays-counted-past-the-memory', YONDER,
     '            pic->state = PIC_SHOWN;\n            g.page.not_kept--;',
     '            pic->state = PIC_SHOWN;'),
    ('leaving-forgets-a-resample', YONDER,
     '        if (p->pics[i].resampling && g.pool != NULL)\n'
     '            os64_work_cancel(g.pool, p->pics[i].resample_id);\n',
     ''),
    # Codex #444 round 1.
    ('rd1-a-resample-target-is-worked-out-again', PICTURE,
     '    if (job->resample && job->cover_w > 0 && job->cover_h > 0) {',
     '    if (false && job->resample && job->cover_w > 0 && job->cover_h > 0) {'),
    ('rd1-a-resample-no-different-is-asked-again', YONDER,
     '    } else if (shown && product->image.width == pic->image.width &&\n'
     '               product->image.height == pic->image.height) {',
     '    } else if (false) {'),
    ('rd1-an-arrival-moving-nothing-is-never-judged', YONDER,
     '    if (g.pictures_judge_owed && !g.pictures_moved && !s_web.moved)\n        css_pictures(&g.page);',
     '    (void)0;'),
    ('rd1-a-pixelated-use-is-kept-small', YONDER,
     '    if (!pixelated)\n        yonder_picture_kept_size(',
     '    (void)pixelated;\n        yonder_picture_kept_size('),
    ('rd1-leaving-does-not-wait-for-a-resample', YONDER,
     '           g.page.in_flight == 0 && g.page.resample_owed == 0 && !g.pictures_judge_owed &&',
     '           g.page.resample_owed == 0 && !g.pictures_judge_owed &&'),
    # The record.
    ('the-record-counts-whole-pictures', YONDER,
     '            (pic->image.width >= pic->width && pic->image.height >= pic->height))',
     '            false)'),
]

if args.check:
    bad = 0
    for name, rel, old, new in mutants:
        n = (root / rel).read_text().count(old)
        if n != 1:
            print(f'{name}: found {n} times in {rel}')
            bad += 1
    print(f'{len(mutants)} mutants, {bad} not found once')
    sys.exit(1 if bad else 0)

args.logs.mkdir(parents=True, exist_ok=True)
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

print(f'\npicture sizes mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
