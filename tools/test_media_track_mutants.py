#!/usr/bin/env python3
"""One deliberate break at a time to libmedia's track buffer
(userland/libmedia/media_track.c), against tools/test_media_track_host.sh:
a mutant is CAUGHT when the suite fails, MISSED when it passes, which means a
test is missing and gets written. A build failure is neither.

The shape is test_media_cores_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
through tools/mutants_parallel.py (it needs no --with: the buffer is in
userland/); the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/media-track-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

F = 'userland/libmedia/media_track.c'
CASES = 'bash tools/test_media_track_host.sh 3'

# Two are left out: the group's own-cut check (reached only when a cut has
# no memory for a kept stretch's range) and "a group removes its own" (equivalent: the cuts of
# [highest end, end) already cover the group's span).
mutants = [
    # Times.
    ('ticks-truncate-not-round', F, '(r * NS_PER_SECOND + (int64_t)(timescale / 2)) / (int64_t)timescale', '(r * NS_PER_SECOND) / (int64_t)timescale'),
    ('ticks-negative-remainder-kept', F, '    if (r < 0) {\n        r += timescale;\n        q--;\n    }\n    // r < 2^30', '    // r < 2^30'),
    ('ticks-finer-than-a-nanosecond-taken', F, 'if (timescale == 0 || timescale > OS64_MEDIA_MAX_TIMESCALE)\n        return false;\n    int64_t q = ticks', 'if (timescale == 0)\n        return false;\n    int64_t q = ticks'),
    ('seconds-truncated', F, '    if (frac >= 0.5)\n        n++;\n    else if (frac <= -0.5)\n        n--;\n', '    (void)frac;\n'),
    ('duration-rounded-on-its-own', F, '    duration = end0 - pts0;', '    if (!os64_media_ticks_to_ns((int64_t)f->duration, f->timescale, &duration))\n        return OS64_MEDIA_TRACK_TIME_OVERFLOW;'),
    ('init-finer-timescale-taken', F, '        if (init->track[i].timescale == 0 || init->track[i].timescale > OS64_MEDIA_MAX_TIMESCALE)\n            return OS64_MEDIA_TRACK_TIMESCALE;\n', ''),
    ('unknown-track-taken', F, '    os64_media_track_t *t = track_by_id(b, f->track_id);\n    if (t == 0)\n        return OS64_MEDIA_TRACK_UNKNOWN_TRACK;', '    os64_media_track_t *t = track_by_id(b, f->track_id);\n    if (t == 0)\n        t = &b->track[0];'),
    ('unknown-generation-taken', F, '    if (gen_find(t, f->generation, 0) == 0)\n        return OS64_MEDIA_TRACK_UNKNOWN_GENERATION;', '    if (0)\n        return OS64_MEDIA_TRACK_UNKNOWN_GENERATION;'),
    # Coded frame processing.
    ('sequence-offset-ignores-pts', F, 'if (__builtin_sub_overflow(b->group_start, pts0, &offset))', 'if (__builtin_sub_overflow(b->group_start, 0, &offset))'),
    ('the-microsecond-is-wider', F, 'pts < sat_add(at, ONE_MICROSECOND)))', 'pts < sat_add(at, 20000)))'),
    ('sound-starting-inside-a-frame-is-no-cut', F, 'if (found && (t->kind == OS64_MEDIA_FRAGMENT_AUDIO || pts < sat_add(', 'if (found && (pts < sat_add('),
    ('cfp-1-13-for-every-frame', F, '    if (t->last_dts == OS64_MEDIA_NO_TIME) {\n        // Every range', '    if (1) {\n        // Every range'),
    ('cfp-1-13-looks-in-one-range', F, '        for (uint32_t ri = 0; ri < t->range_count && !found; ri++) {\n            const os64_media_track_range_t *r = &t->range[ri];\n            if (pts < r->start || pts >= r->end)\n                continue;', '        for (uint32_t ri = 0; ri < t->range_count && !found; ri++) {\n            const os64_media_track_range_t *r = &t->range[ri];\n            if (pts < r->start || pts >= r->end)\n                continue;\n            ri = t->range_count;'),
    ('a-group-joins-a-range-behind-it-in-decode', F, 'if (last->dts <= dts && dts <= sat_add(sat_add(', 'if (dts <= sat_add(sat_add('),
    ('a-group-joins-across-a-hole-on-screen', F, ' && pts <= sat_add(p->end, join_of(t)) &&\n', ' && (pts | 1) &&\n'),
    ('a-group-joins-a-range-presented-after-it', F, '            pts >= sat_sub(p->end, join_of(t)) && pts <= sat_add(p->end, join_of(t)) &&\n', '            pts <= sat_add(p->end, join_of(t)) &&\n'),
    ('no-memory-leaves-the-group-running', F, '    if (rule == OS64_MEDIA_TRACK_NO_MEMORY) {\n        os64_media_track_t *t = track_by_id(b, f->track_id);', '    if (0) {\n        os64_media_track_t *t = track_by_id(b, f->track_id);'),
    ('sequence-needs-no-rap', F, '            b->group_end = b->group_start;\n            for (uint32_t k = 0; k < b->track_count; k++)\n                b->track[k].need_rap = true;', '            b->group_end = b->group_start;'),
    ('offset-not-added', F, '__builtin_add_overflow(pts0, b->offset, &pts)', '__builtin_add_overflow(pts0, 0, &pts)'),
    ('a-step-back-is-no-discontinuity', F, '(dts < t->last_dts || sat_sub(dts, t->last_dts)', '(sat_sub(dts, t->last_dts)'),
    ('discontinuity-at-three-durations', F, '> sat_add(t->last_duration, t->last_duration))) {', '> sat_add(t->last_duration, t->last_duration + t->last_duration))) {'),
    ('segments-group-end-not-set', F, '            if (b->mode == OS64_MEDIA_SEGMENTS)\n                b->group_end = pts;\n            else', '            if (b->mode == OS64_MEDIA_SEGMENTS)\n                (void)0;\n            else'),
    ('sequence-group-start-not-set', F, '            else\n                b->group_start = b->group_end;\n            for (uint32_t k', '            else\n                (void)0;\n            for (uint32_t k'),
    ('discontinuity-unsets-only-this-track', F, '            for (uint32_t k = 0; k < b->track_count; k++) {\n                track_unset(&b->track[k]);\n                preload_clear(&b->track[k]);\n            }\n            continue;', '            track_unset(t);\n            preload_clear(t);\n            continue;'),
    ('window-start-ignored', F, 'if (pts < b->window_start || end > b->window_end) {', 'if (end > b->window_end) {'),
    ('window-end-ignored', F, 'if (pts < b->window_start || end > b->window_end) {', 'if (pts < b->window_start) {'),
    ('pictures-get-a-preload', F, 'if (pts < b->window_start && t->kind == OS64_MEDIA_FRAGMENT_AUDIO) {', 'if (pts < b->window_start) {'),
    ('the-preload-is-never-kept', F, '    if (t->have_preload) {\n        t->preload.serial = ++t->next_serial;', '    if (0 && t->have_preload) {\n        t->preload.serial = ++t->next_serial;'),
    ('a-preload-from-anywhere-is-kept', F, '    if (t->have_preload && (t->preload.pts + t->preload.duration < sat_sub(pts, ONE_MICROSECOND) ||\n                            t->preload.pts + t->preload.duration > sat_add(pts, ONE_MICROSECOND) ||', '    if (t->have_preload && (0 ||'),
    ('need-rap-ignored', F, '    if (t->need_rap) {\n        if (!rap) {', '    if (t->need_rap) {\n        if (0) {'),
    ('sound-must-be-flagged-sync', F, 'bool rap = f->rap || t->kind == OS64_MEDIA_FRAGMENT_AUDIO;', 'bool rap = f->rap;'),
    ('a-group-start-removes-nothing', F, '    if (t->highest_end == OS64_MEDIA_NO_TIME)\n        cut_presented(t, pts, end);\n    else', '    if (t->highest_end == OS64_MEDIA_NO_TIME)\n        (void)0;\n    else'),
    ('dependents-left', F, '            } else if (is_unit_start(r, i)) {\n                removing = false;', '            } else if (1) {\n                removing = false;'),
    ('a-preload-outlives-its-frame', F, 'if (i > 0 && (r->frame[i - 1].flags & OS64_MEDIA_FRAME_PRELOAD) && !(r->frame[i - 1].flags & FRAME_CUT)) {', 'if (0) {'),
    ('a-cut-keeps-its-ranges-unsorted', F, '        range_extent(&t->range[ri]);\n        ri++;\n    }\n    ranges_sort(t);', '        range_extent(&t->range[ri]);\n        ri++;\n    }'),
    ('a-group-goes-on-after-its-last-was-removed', F, '        if (ar == 0 || !group_intact(t, ar)) {', '        if (ar == 0) {'),
    ('the-join-is-one-step', F, '    return sat_add(t->interbuffer, t->interbuffer);', '    return t->interbuffer;'),
    ('the-join-ignores-decode-steps', F, '    if (step > t->interbuffer)\n        t->interbuffer = step;\n', '    (void)step;\n'),
    ('ranges-not-resorted', F, 'for (uint32_t j = i; j > 0 && t->range[j].frame[0].dts < t->range[j - 1].frame[0].dts; j--) {', 'for (uint32_t j = i; 0; j--) {'),
    # Counting and generations.
    ('bytes-not-uncounted', F, '    t->bytes -= f->size;\n    uint32_t gi;', '    uint32_t gi;'),
    ('generation-frames-not-uncounted', F, '        t->gen[gi].frames--;\n', ''),
    ('the-newest-generation-is-freed', F, 'if (g->frames != 0 || g->holds != 0 || index == t->gen_count - 1)', 'if (g->frames != 0 || g->holds != 0)'),
    ('a-held-generation-is-freed', F, 'if (g->frames != 0 || g->holds != 0 || index == t->gen_count - 1)', 'if (g->frames != 0 || index == t->gen_count - 1)'),
    ('configurations-not-copied', F, '            g->entry[e].config = block + at;', '            g->entry[e].config = src->entry[e].config;'),
    ('an-init-needs-no-rap', F, '        t->need_rap = true;             // MSE', '        (void)0;             // MSE'),
    # Eviction and the quota.
    ('eviction-runs-past-the-play-position', F, 'uint32_t limit = is_play ? play_unit : r->count, cut = 0;', 'uint32_t limit = is_play ? r->count + 0 * play_unit : r->count, cut = 0;'),
    ('eviction-ahead-ignores-the-protection', F, '            if (unit_pts(r, u) < protect_end)\n                break;\n', '            if (unit_pts(r, u) < protect_end && 0)\n                break;\n'),
    ('the-quota-is-never-exceeded', F, '        if (t->bytes + share > t->quota)\n            fits = false;', '        (void)0;'),
    ('each-track-takes-the-whole-append', F, '        uint64_t share = total_quota == 0 ? incoming', '        uint64_t share = 1 ? incoming'),
    ('a-preload-with-a-frame-decoded-between-is-kept', F, '                            t->preload.dts > dts || decoded_between(t, t->preload.dts, dts)))', '                            t->preload.dts > dts || (decoded_between(t, t->preload.dts, dts) && 0)))'),
    ('a-preload-is-kept-without-room-for-its-frame', F, 'if (!range_reserve(t, r, r->count + (t->have_preload ? 2u : 1u))) {', 'if (!range_reserve(t, r, r->count + 1u)) {'),
    ('eviction-takes-a-unit-presented-ahead', F, '            if (pr && !unit_behind(r, cut, n, play))', '            if (pr && !unit_behind(r, cut, n, play) && 0)'),
    ('the-hop-measures-from-the-range-end', F, '    if (r->end > frontier)\n        frontier = r->end;\n    for', '    frontier = r->end;\n    for'),
    ('ranges-from-starts-the-walk-at-the-time', F, '    for (int64_t at = OS64_MEDIA_NO_TIME; next_union(t, at, &r); at = r.end) {\n        if (from != OS64_MEDIA_NO_TIME && r.end <= from)', '    for (int64_t at = from; next_union(t, at, &r); at = r.end) {\n        if (from != OS64_MEDIA_NO_TIME && r.end <= from)'),
    ('a-group-presented-earlier-joins', F, ' &&\n            pts >= unit_pts(p, unit_of(p, p->count - 1))) {', ') {'),
    ('the-highest-pts-is-the-first', F, '                if (!(f->flags & OS64_MEDIA_FRAME_PRELOAD) && f->pts > pts)\n                    pts = f->pts;', '                if (!(f->flags & OS64_MEDIA_FRAME_PRELOAD) && pts == OS64_MEDIA_NO_TIME)\n                    pts = f->pts;'),
    # Reading.
    ('a-joined-gap-is-not-held', F, '    if (before != OS64_MEDIA_NO_TIME && sat_sub(next->start, before) <= join_of(t))\n        return next;\n    return 0;', '    return 0;'),
    ('a-gap-is-measured-from-the-time', F, 'if (before != OS64_MEDIA_NO_TIME && sat_sub(next->start, before) <= join_of(t))', 'if (before != OS64_MEDIA_NO_TIME && sat_sub(next->start, time) <= join_of(t))'),
    ('ahead-starts-at-the-time', F, '    for (int64_t from = OS64_MEDIA_NO_TIME; next_union(t, from, &r); from = r.end) {\n        if (time < r.start)', '    for (int64_t from = time; next_union(t, from, &r); from = r.end) {\n        if (time < r.start)'),
    ('a-deleted-group-range-needs-no-rap', F, '        t->append_range = 0;\n        t->last_kept = 0;\n        t->need_rap = true;\n    }', '        t->append_range = 0;\n        t->last_kept = 0;\n    }'),
    ('a-cut-keeps-a-stretch-in-part', F, '            } else if (dest != 0) {\n                dest->frame[dest->count++] = f;   // reserved above', '            } else if (dest != 0 && (dest->count == 0 || (f.serial & 7) != 3)) {\n                dest->frame[dest->count++] = f;   // reserved above'),
    ('an-assigned-offset-starts-no-group', F, '    b->offset = offset;\n    if (b->mode == OS64_MEDIA_SEQUENCE)\n        b->group_start = offset;', '    b->offset = offset;'),
    ('ended-extends-nothing', F, '    if (out->end == last && extend_to > out->end)\n        out->end = extend_to;', '    (void)extend_to;'),
    ('ended-extends-every-interval', F, '    if (out->end == last && extend_to > out->end)', '    if (extend_to > out->end)'),
    ('the-highest-end-is-the-first-range-s', F, 'if (b->track[k].range[ri].start < b->track[k].range[ri].end && b->track[k].range[ri].end > end)', 'if (b->track[k].range[ri].start < b->track[k].range[ri].end && end == OS64_MEDIA_NO_TIME)'),
    ('buffered-joins-only-touching', F, 'r->start <= sat_add(out->end, join_of(t)) && r->end > out->end', 'r->start <= out->end && r->end > out->end'),
    ('buffered-is-the-union', F, 'int64_t s = a.start > c.start ? a.start : c.start, e = a.end < c.end ? a.end : c.end;', 'int64_t s = a.start < c.start ? a.start : c.start, e = a.end > c.end ? a.end : c.end;'),
    ('the-cursor-stops-at-a-range-end', F, '        const os64_media_track_range_t *n = range_after(t, r, cursor->frontier);\n        cursor->next_serial = 0;', '        const os64_media_track_range_t *n = range_after(t, r, cursor->frontier) ? (const os64_media_track_range_t *)0 : 0;\n        cursor->next_serial = 0;'),
    ('last-frame-gone-reads-on', F, '                return OS64_MEDIA_TRACK_RESYNC;     // the frame it returned last is gone', '                return OS64_MEDIA_TRACK_WAIT;     // the frame it returned last is gone'),
    ('next-frame-gone-reads-on', F, '            if (cursor->next_serial != 0 && (index >= r->count || r->frame[index].serial != cursor->next_serial))\n                return OS64_MEDIA_TRACK_RESYNC;', '            (void)0;'),
    ('every-edit-is-a-resync', F, '        // Frames moved: find the cursor\'s place again by the frames it knows.\n', '        if (cursor->last_serial != 0)\n            return OS64_MEDIA_TRACK_RESYNC;\n'),
    ('the-player-holds-nothing', F, '    *frame = r->frame[cursor->index++];\n    os64_media_fragment_slab_hold(frame->slab);', '    *frame = r->frame[cursor->index++];'),
    ('done-drops-nothing', F, '    if (frame->slab) {\n        os64_media_fragment_slab_drop(frame->slab);', '    if (frame->slab) {\n        (void)0;'),
    ('the-rap-is-the-range-start', F, '    *rap = r->frame[unit_for(r, time)];', '    *rap = r->frame[0];'),
    ('frame-at-or-after-includes-before', F, 'if ((f->flags & OS64_MEDIA_FRAME_PRELOAD) || f->pts < time)\n            continue;\n        if (!found || f->pts < frame->pts) {', 'if (f->flags & OS64_MEDIA_FRAME_PRELOAD)\n            continue;\n        if (!found || f->pts < frame->pts) {'),
    ('reset-keeps-the-sequence-group', F, '    if (b->mode == OS64_MEDIA_SEQUENCE)\n        b->group_start = b->group_end;\n    for (uint32_t k = 0; k < b->track_count; k++) {\n        track_unset(&b->track[k]);', '    for (uint32_t k = 0; k < b->track_count; k++) {\n        track_unset(&b->track[k]);'),
    ('sequence-mode-starts-no-group', F, '    b->mode = mode;\n    if (mode == OS64_MEDIA_SEQUENCE)\n        b->group_start = b->group_end;', '    b->mode = mode;'),
    ('remove-removes-nothing', F, '    for (uint32_t k = 0; k < b->track_count; k++)\n        cut_presented(&b->track[k], start, end);', '    (void)b;\n    (void)start;\n    (void)end;'),
]

caught, missed, unbuilt = [], [], []
for name, target, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / target
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {target}', file=sys.stderr)
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
            print(f'{name}: {target} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nmedia track mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
