#!/usr/bin/env python3
"""One deliberate break at a time to libmedia's fragment parser
(userland/libmedia/media_fragment.c), against tools/test_media_fragment_host.sh:
a mutant is CAUGHT when the suite fails, MISSED when it passes, which means a
test is missing and gets written. A build failure is neither.

The shape is test_media_cores_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
through tools/mutants_parallel.py (it needs no --with: the parser is in
userland/); the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/media-fragment-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

F = 'userland/libmedia/media_fragment.c'
CASES = 'bash tools/test_media_fragment_host.sh'

mutants = [
    # The framer.
    ('a-partial-box-is-read', F,
     '            if (p->box_have == p->box_size && end_box(p) < 0)\n                return -1;\n            break;\n        case IN_SKIP:',
     '            if (end_box(p) < 0)\n                return -1;\n            break;\n        case IN_SKIP:'),
    ('a-box-smaller-than-its-header-is-taken', F, 'if (size < p->header_size)          // zero', 'if (0 && size < p->header_size)          // zero'),
    ('a-64-bit-size-is-not-read', F, 'p->header_size = large ? 16 : 8;', 'p->header_size = large ? 8 : 8;'),
    ('max-box-is-not-held-to', F,
     '        if (size > cap)\n            return fail(p, OS64_MEDIA_FRAGMENT_BOX_TOO_LARGE);', '        if (0 && size > cap)\n            return fail(p, OS64_MEDIA_FRAGMENT_BOX_TOO_LARGE);'),
    ('max-mdat-is-not-held-to', F,
     'if (payload > cap || payload > SIZE_MAX', 'if ((0 && payload > cap) || payload > SIZE_MAX'),
    ('an-mdat-holding-no-frame-is-held-to-max-mdat', F,
     '        if (!samples_pending(p)) {\n            p->skip_left = payload;',
     '        if (!samples_pending(p)) {\n            if (payload > p->memory.max_mdat && p->memory.max_mdat)\n                return fail(p, OS64_MEDIA_FRAGMENT_MDAT_TOO_LARGE);\n            p->skip_left = payload;'),
    ('an-error-is-not-sticky', F,
     '    if (p->error != OS64_MEDIA_FRAGMENT_OK)\n        return -1;\n    // A sink', '    // A sink'),
    # A media segment before its init; frames left unserved.
    ('a-moof-before-any-init-is-taken', F,
     '        if (type == BOX_MOOF && !p->have_init)\n            return fail(p, OS64_MEDIA_FRAGMENT_MEDIA_BEFORE_INIT);', ''),
    ('frames-left-at-the-next-moof-are-forgotten', F,
     '        if (samples_pending(p))\n            return fail(p, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT);\n        if (type == BOX_MOOF', '        if (type == BOX_MOOF'),
    ('a-frame-before-its-mdat-is-taken', F,
     '        if (next->start < p->mdat_start)\n            return fail(p, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT);', ''),
    ('a-frame-past-its-mdat-end-is-forgotten', F,
     'if (samples_pending(p) && in_this_mdat(p, &p->sample[p->sample_next]))',
     'if (0 && samples_pending(p) && in_this_mdat(p, &p->sample[p->sample_next]))'),
    ('a-frame-goes-before-its-bytes', F, 'if (s->start > have || s->size > have - s->start)', 'if (s->size > have - s->start)'),
    ('a-zero-size-frame-at-the-end-is-a-later-mdats', F,
     'return s->start < p->mdat_end || (s->start == p->mdat_end && s->size == 0);', 'return s->start < p->mdat_end;'),
    ('an-mdat-of-zero-size-frames-waits-for-a-later-byte', F,
     '        return mdat_progress(p, resets);    // zero-size', '        return (int)(resets * 0);    // zero-size'),
    ('a-frame-before-its-moof-is-taken', F,
     '            if (position < 0)\n                return OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT;', ''),
    # The init segment.
    ('no-mvex-is-taken', F, '    if (found == 0)\n        return OS64_MEDIA_FRAGMENT_NO_MVEX;', ''),
    ('a-sample-table-entry-is-ignored', F, '    return count == 0;\n}', '    (void)count;\n    return true;\n}'),
    ('co64-is-not-checked', F,
     '{ BOX_STTS, BOX_STSC, BOX_STCO, BOX_CO64, BOX_STSZ }', '{ BOX_STTS, BOX_STSC, BOX_STCO, BOX_STSZ }'),
    ('stsz-sample-size-read-as-its-count', F,
     '    if (type == BOX_STSZ)\n        rd_skip(&b.body, 4);            // sample_size; sample_count follows\n', ''),
    ('no-tracks-is-taken', F,
     '    if (next.track_count == 0)\n        return OS64_MEDIA_FRAGMENT_NO_TRACKS;', ''),
    ('two-tracks-of-a-kind-are-taken', F,
     '            if (next.track[i].kind == kind)\n                return OS64_MEDIA_FRAGMENT_TOO_MANY_TRACKS;', ''),
    ('a-duplicate-id-is-taken', F, '            if (ids[i] == ids[j])', '            if (0 && ids[i] == ids[j])'),
    ('an-ignored-track-id-is-not-compared', F,
     '    for (uint32_t i = 0; i < ignored_count; i++)\n        ids[n++] = ignored[i];', ''),
    ('a-changed-track-count-is-taken', F, '            if (before != after)', '            if (0)'),
    ('a-later-init-keeps-generation-one', F,
     'next.generation = p->init.generation + 1;', 'next.generation = 1;'),
    ('the-init-buffer-is-not-kept', F,
     '        p->box = p->init_box;\n        p->box_capacity = p->init_capacity;\n        p->init_box = held;\n        p->init_capacity = held_capacity;', '        (void)held;\n        (void)held_capacity;'),
    ('any-aac-object-type-is-taken', F,
     'if (object_type != 0x40 && object_type != 0x66 && object_type != 0x67 && object_type != 0x68)',
     'if (0)'),
    ('a-sound-entry-in-a-picture-track-is-taken', F,
     '        else if (entry.type == BOX_MP4A && t->kind == OS64_MEDIA_FRAGMENT_AUDIO)',
     '        else if (entry.type == BOX_MP4A)'),
    ('an-unknown-entry-is-taken', F,
     '        else\n            rule = OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC;',
     '        else\n            rule = OS64_MEDIA_FRAGMENT_OK;'),
    ('avcc-is-not-required', F,
     '    if (found <= 0)\n        return OS64_MEDIA_FRAGMENT_MALFORMED;\n    // version',
     '    if (found < 0)\n        return OS64_MEDIA_FRAGMENT_MALFORMED;\n    if (found == 0)\n        return OS64_MEDIA_FRAGMENT_OK;\n    // version'),
    ('a-short-avcc-is-taken', F, 'if (avcc.body.left < 7 || avcc.body.p[0] != 1)', 'if (avcc.body.left < 4 || avcc.body.p[0] != 1)'),
    ('any-mpeg-4-audio-is-taken-as-aac', F,
     '    if (aot != 1 && aot != 2 && aot != 4 && aot != 5 && aot != 29)\n        return OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC;\n', ''),
    ('he-aac-is-refused', F, 'aot != 4 && aot != 5 && aot != 29', 'aot != 4 && aot != 29'),
    ('one-entry-too-many-is-taken', F,
     'if (count > OS64_MEDIA_FRAGMENT_MAX_ENTRIES)', 'if (count > OS64_MEDIA_FRAGMENT_MAX_ENTRIES + 1)'),
    ('a-timescale-of-zero-is-taken', F, '    if (t->timescale == 0)', '    if (0)'),
    ('an-empty-edit-in-movie-timescale-zero-divides', F, '        if (movie_scale == 0)', '        if (0)'),
    # The edit list.
    ('the-edit-is-not-applied-to-dts', F,
     '__builtin_sub_overflow(time, edit, &sample.dts)', '__builtin_sub_overflow(time, 0, &sample.dts)'),
    ('the-edit-is-not-applied-to-pts', F,
     '__builtin_sub_overflow(pts, edit, &sample.pts)', '__builtin_sub_overflow(pts, 0, &sample.pts)'),
    ('an-empty-edit-is-not-added-back', F,
     'if (__builtin_sub_overflow(media_time, shift, offset))', 'if (__builtin_sub_overflow(media_time, shift * 0, offset))'),
    ('later-edits-are-followed', F, '        break;                          // later edits are not followed', ''),
    ('a-v1-edit-is-read-as-v0', F,
     'int64_t time = version == 1 ? (int64_t)rd_n(&r, 8) :', 'int64_t time = version == 9 ? (int64_t)rd_n(&r, 8) :'),
    # The moof.
    ('a-traf-for-an-unknown-track-is-taken', F, '    if (t == 0 && !ignored)', '    if (0)'),
    ('an-ignored-track-is-unknown', F, '            *ignored = true;\n            return 0;', '            return 0;'),
    ('an-ignored-track-is-handed-out', F, '        if (!s->ignored) {', '        if (1) {'),
    ('a-base-data-offset-is-taken', F,
     '    if (flags & TFHD_BASE_DATA_OFFSET)\n        return OS64_MEDIA_FRAGMENT_BASE_DATA_OFFSET;', ''),
    ('a-first-trun-without-data-offset-is-taken', F,
     '        } else if (first_trun) {\n            return OS64_MEDIA_FRAGMENT_NO_DATA_OFFSET;',
     '        } else if (0 && first_trun) {\n            return OS64_MEDIA_FRAGMENT_NO_DATA_OFFSET;'),
    ('no-tfdt-is-taken', F,
     '    if (found == 0)\n        return OS64_MEDIA_FRAGMENT_NO_TFDT;', '    if (found == 0)\n        return OS64_MEDIA_FRAGMENT_OK;'),
    ('a-sample-description-index-of-zero-is-taken', F,
     '(entry < 1 || entry > t->entry_count)', '(entry > t->entry_count)'),
    ('a-sample-description-index-past-the-entries-is-taken', F,
     '(entry < 1 || entry > t->entry_count)', '(entry < 1)'),
    ('the-frame-names-the-first-entry', F, '.entry = entry ? entry - 1 : 0,', '.entry = 0,'),
    ('an-empty-fragment-may-name-frames', F,
     'if (count != 0 && (flags & TFHD_DURATION_IS_EMPTY))', 'if (0 && (flags & TFHD_DURATION_IS_EMPTY))'),
    ('too-many-samples-are-taken', F,
     'if (count > OS64_MEDIA_FRAGMENT_MAX_SAMPLES - p->sample_count)', 'if (0)'),
    ('trex-duration-is-not-the-default', F,
     'uint32_t entry = t ? t->trex_entry : 1, duration = t ? t->trex_duration : 0;',
     'uint32_t entry = t ? t->trex_entry : 1, duration = 0;'),
    ('trex-size-is-not-kept', F, '                next.track[i].trex_size = size;\n', '                next.track[i].trex_size = size * 0;\n'),
    ('the-first-sample-flags-are-ignored', F,
     'f = (i == 0 && have_first) ? first_flags : sample_flags;', 'f = (i == 0 && have_first && 0) ? first_flags : sample_flags;'),
    ('depends-on-alone-is-a-random-access-point', F,
     '.rap = !(f & SAMPLE_NON_SYNC) && SAMPLE_DEPENDS_ON(f) != 1,', '.rap = !(f & SAMPLE_NON_SYNC),'),
    ('non-sync-alone-is-a-random-access-point', F,
     '.rap = !(f & SAMPLE_NON_SYNC) && SAMPLE_DEPENDS_ON(f) != 1,', '.rap = SAMPLE_DEPENDS_ON(f) != 1,'),
    ('a-chained-traf-counts-from-the-moof', F,
     '            int64_t base = (flags & TFHD_DEFAULT_BASE_IS_MOOF) || first_traf ? 0 : *chain_end;',
     '            int64_t base = 0;'),
    ('the-chain-does-not-move', F, '    *chain_end = position;\n', ''),
    ('a-tfdt-past-63-bits-is-taken', F,
     '    if (base_time > (uint64_t)INT64_MAX)\n        return OS64_MEDIA_FRAGMENT_TIME_OVERFLOW;', ''),
    ('times-overflow-unchecked', F,
     '__builtin_add_overflow(time, (int64_t)d, &time)', '(time = (int64_t)((uint64_t)time + d), 0)'),
    ('samples-are-not-sorted', F,
     '    if (sorted)\n        return OS64_MEDIA_FRAGMENT_OK;  // the usual case', '    if (1)\n        return OS64_MEDIA_FRAGMENT_OK;  // the usual case'),
    ('a-moof-with-no-traf-is-taken', F, '    if (walk.bad || first)              // a moof with no traf', '    if (walk.bad)              // a moof with no traf'),
    # The slab's holds, and reset.
    ('the-parser-keeps-its-hold', F,
     '        p->slab = 0;\n        os64_media_fragment_slab_drop(slab);\n        p->state = WAIT_HEADER;', '        p->slab = 0;\n        p->state = WAIT_HEADER;'),
    # A sink that resets the parser.
    ('a-sinks-reset-goes-unseen-by-append', F, 'while (length > 0 && p->resets == resets) {', 'while (length > 0) {'),
    ('a-sinks-reset-goes-unseen-by-progress', F,
     '    if (p->resets != resets)\n        return 0;                       // the slab', '    if (0 && p->resets != resets)\n        return 0;                       // the slab'),
    ('a-reset-is-not-counted', F, '{\n    p->resets++;\n', '{\n'),
    ('a-slab-is-freed-at-one-hold', F, '    if (--slab->holds == 0)', '    if (--slab->holds <= 1)'),
    ('reset-keeps-the-unfinished-slab', F,
     '    if (p->slab != 0) {\n        os64_media_fragment_slab_t *slab = p->slab;\n        p->slab = 0;\n        os64_media_fragment_slab_drop(slab);\n    }', ''),
    ('reset-forgets-the-init', F,
     '    p->error = OS64_MEDIA_FRAGMENT_OK;\n}\n\nvoid os64_media_fragment_destroy',
     '    p->error = OS64_MEDIA_FRAGMENT_OK;\n    p->have_init = false;\n}\n\nvoid os64_media_fragment_destroy'),
    # The reader.
    ('a-child-box-may-claim-more-than-its-parent', F,
     'if (c->bad || size < head || size - head > c->left)', 'if (c->bad || size < head)'),
    ('a-descriptor-may-claim-more-than-its-box', F,
     '        if (r->bad || length > r->left) {', '        if (r->bad) {'),
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

print(f'\nmedia fragment mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
