#!/usr/bin/env python3
"""One deliberate break at a time to the audio stream's rules
(kernel/src/audio/audio_stream.c: the ring and the split frame, generations,
the states, underruns, the gain, the position, sinks, the null sink's clock,
the tap), against tools/test_audio_stream_host.sh: a mutant is CAUGHT when
the suite fails, MISSED when it passes, which means a test is missing and
gets written. A build failure is neither.

The shape is test_xhci_service_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
from a clean tree, and never beside another harness that edits the same tree;
the per-mutant logs are in --logs. Under tools/mutants_parallel.py it needs
`--with kernel`: the runner's copies do not carry the kernel's sources
unless asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/audio-stream-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

STREAM = 'kernel/src/audio/audio_stream.c'
CASES = 'bash tools/test_audio_stream_host.sh 60'

mutants = [
    ('failed-does-not-outrank', '\tif (s->failed)\n\t\treturn OS64_AUDIO_FAILED_STATE;\n\tif (!s->open)',
     '\tif (!s->open)'),
    ('a-closed-stream-with-sound-is-idle', 'return s->fill > 0 ? OS64_AUDIO_DRAINING : OS64_AUDIO_IDLE;',
     'return OS64_AUDIO_IDLE;'),
    ('paused-is-not-a-state', '\tif (s->paused)\n\t\treturn OS64_AUDIO_PAUSED;\n\tif (s->fill > 0)',
     '\tif (s->fill > 0)'),
    ('an-unwritten-stream-is-starved', 'return s->written ? OS64_AUDIO_STARVED : OS64_AUDIO_WAITING;',
     'return OS64_AUDIO_STARVED;'),
    ('a-flush-keeps-the-ring', '\ts->head = 0;\n\ts->fill = 0;\n\ts->old_frames = 0;\n\ts->split_len = 0;\n\ts->generation++;',
     '\ts->old_frames = 0;\n\ts->split_len = 0;\n\ts->generation++;'),
    ('a-flush-keeps-the-split-frame', '\ts->old_frames = 0;\n\ts->split_len = 0;\n\ts->generation++;\n\ts->accepted = 0;',
     '\ts->old_frames = 0;\n\ts->generation++;\n\ts->accepted = 0;'),
    ('a-flush-keeps-the-generation', '\ts->split_len = 0;\n\ts->generation++;\n\ts->accepted = 0;\n\ts->taken = 0;\n\ts->underruns = 0;\n\ts->written = false;\n\ts->dry_counted = false;\n}\n\nbool audio_stream_open',
     '\ts->split_len = 0;\n\ts->accepted = 0;\n\ts->taken = 0;\n\ts->underruns = 0;\n\ts->written = false;\n\ts->dry_counted = false;\n}\n\nbool audio_stream_open'),
    ('a-flush-keeps-the-counts', '\ts->generation++;\n\ts->accepted = 0;\n\ts->taken = 0;\n\ts->underruns = 0;\n\ts->written = false;\n\ts->dry_counted = false;\n}\n\nbool',
     '\ts->generation++;\n\ts->underruns = 0;\n\ts->written = false;\n\ts->dry_counted = false;\n}\n\nbool'),
    ('a-flush-keeps-written', '\ts->underruns = 0;\n\ts->written = false;\n\ts->dry_counted = false;\n}\n\nbool audio_stream_open',
     '\ts->underruns = 0;\n\ts->dry_counted = false;\n}\n\nbool audio_stream_open'),
    ('a-second-open-is-granted', '\tif (s->open)\n\t\treturn false;\n\ts->open = true;',
     '\ts->open = true;'),
    ('an-open-forgets-the-old-frames', '\ts->old_frames = s->fill;\n',
     '\ts->old_frames = 0;\n'),
    ('an-open-names-the-old-frames-its-own', '\ts->old_generation = s->generation;\n\ts->old_frames = s->fill;\n\ts->generation++;',
     '\ts->old_frames = s->fill;\n\ts->generation++;\n\ts->old_generation = s->generation;'),
    ('an-open-keeps-the-generation', '\ts->old_frames = s->fill;\n\ts->generation++;',
     '\ts->old_frames = s->fill;'),
    ('an-open-keeps-the-counts', '\ts->generation++;\n\ts->accepted = 0;\n\ts->taken = 0;\n\ts->underruns = 0;\n\ts->written = false;\n\ts->dry_counted = false;\n\ts->split_len = 0;\n\treturn true;',
     '\ts->generation++;\n\ts->underruns = 0;\n\ts->written = false;\n\ts->dry_counted = false;\n\ts->split_len = 0;\n\treturn true;'),
    ('an-open-keeps-written', '\ts->underruns = 0;\n\ts->written = false;\n\ts->dry_counted = false;\n\ts->split_len = 0;\n\treturn true;',
     '\ts->underruns = 0;\n\ts->dry_counted = false;\n\ts->split_len = 0;\n\treturn true;'),
    ('a-paused-close-keeps-the-sound', '\tif (s->paused)\n\t{\n\t\tretire_everything(s);\n\t\ts->paused = false;\n\t}',
     '\tif (s->paused)\n\t{\n\t\ts->paused = false;\n\t}'),
    ('a-paused-close-stays-paused', '\t\tretire_everything(s);\n\t\ts->paused = false;\n',
     '\t\tretire_everything(s);\n'),
    ('a-close-keeps-the-split-frame', '\ts->open = false;\n\ts->split_len = 0;\n}',
     '\ts->open = false;\n}'),
    ('a-close-flushes', '\t// A pause belongs to an open: sound nobody is left to resume is not kept.\n\tif (s->paused)',
     '\t// A pause belongs to an open: sound nobody is left to resume is not kept.\n\tif (s->paused || s->fill > 0)'),
    ('a-put-ignores-the-wrap', '\tif (at >= s->capacity)\n\t\tat -= s->capacity;\n\ts->ring[at * 2]     =',
     '\ts->ring[at * 2]     ='),
    ('the-right-sample-is-the-left', '(int16_t)(uint16_t)(frame[2] | (frame[3] << 8));',
     '(int16_t)(uint16_t)(frame[0] | (frame[1] << 8));'),
    ('the-bytes-are-big-endian', '(int16_t)(uint16_t)(frame[0] | (frame[1] << 8));',
     '(int16_t)(uint16_t)(frame[1] | (frame[0] << 8));'),
    ('a-put-is-not-counted', '\ts->fill++;\n\ts->accepted++;\n',
     '\ts->fill++;\n'),
    ('a-put-does-not-mark-written', '\ts->written = true;\n\ts->dry_counted = false;\n}',
     '\ts->dry_counted = false;\n}'),
    ('a-put-does-not-end-the-dry-spell', '\ts->written = true;\n\ts->dry_counted = false;\n}',
     '\ts->written = true;\n}'),
    ('a-failed-sink-takes-writes', '\tif (s->failed)\n\t\treturn OS64_AUDIO_FAILED;\n',
     ''),
    ('a-stale-generation-is-written', '\tif (generation != s->generation)\n\t\treturn OS64_AUDIO_FLUSHED;',
     '\tif (generation != s->generation && 0)\n\t\treturn OS64_AUDIO_FLUSHED;'),
    ('flushed-outranks-failed', '\tif (s->failed)\n\t\treturn OS64_AUDIO_FAILED;\n\tif (generation != s->generation)\n\t\treturn OS64_AUDIO_FLUSHED;',
     '\tif (generation != s->generation)\n\t\treturn OS64_AUDIO_FLUSHED;\n\tif (s->failed)\n\t\treturn OS64_AUDIO_FAILED;'),
    ('a-finished-split-frame-overruns-a-full-ring', '\t\tif (s->fill == s->capacity)\n\t\t\treturn (long)used;\n\t\tring_put(s, s->split);',
     '\t\tring_put(s, s->split);'),
    ('a-placed-split-frame-stays', '\t\tring_put(s, s->split);\n\t\ts->split_len = 0;',
     '\t\tring_put(s, s->split);'),
    ('a-full-ring-takes-a-frame', '\t\tif (s->fill == s->capacity)\n\t\t\treturn (long)used;\n\t\tring_put(s, bytes + used);',
     '\t\tring_put(s, bytes + used);'),
    ('a-full-ring-is-one-early', '\t\tif (s->fill == s->capacity)\n\t\t\treturn (long)used;\n\t\tring_put(s, bytes + used);',
     '\t\tif (s->fill + 1 >= s->capacity)\n\t\t\treturn (long)used;\n\t\tring_put(s, bytes + used);'),
    ('the-tail-of-a-write-is-dropped', '\twhile (used < length)\n\t\ts->split[s->split_len++] = bytes[used++];\n\treturn (long)used;',
     '\tused = length;\n\treturn (long)used;'),
    ('a-record-that-does-not-fit-is-cut', '\tif (bytes > s->tap_capacity - s->tap_fill)\n\t{\n\t\ts->tap_dropped++;\n\t\ts->tap_dropped_total++;\n\t\treturn;\n\t}',
     '\tif (bytes > s->tap_capacity)\n\t{\n\t\ts->tap_dropped++;\n\t\ts->tap_dropped_total++;\n\t\treturn;\n\t}'),
    ('a-dropped-record-is-not-counted', '\t\ts->tap_dropped++;\n\t\ts->tap_dropped_total++;\n\t\treturn;',
     '\t\ts->tap_dropped_total++;\n\t\treturn;'),
    ('the-dropped-count-is-never-cleared', '\theader.dropped = s->tap_dropped;\n\ts->tap_dropped = 0;',
     '\theader.dropped = s->tap_dropped;'),
    ('a-record-is-not-stamped', '\theader.micros = now_us;',
     '\theader.micros = now_us * 0;'),
    ('every-record-is-sound', '\theader.silence = silence ? 1 : 0;',
     '\theader.silence = silence && 0 ? 1 : 0;'),
    ('records-are-not-cut-to-size', 'uint32_t piece = frames > OS64_AUDIO_TAP_RECORD_FRAMES ? OS64_AUDIO_TAP_RECORD_FRAMES : frames;',
     'uint32_t piece = frames;'),
    ('a-cut-record-repeats-its-index', '\t\tfirst += piece;\n\t\tpcm += piece * 2;',
     '\t\tpcm += piece * 2;'),
    ('a-cut-record-repeats-its-sound', '\t\tfirst += piece;\n\t\tpcm += piece * 2;',
     '\t\tfirst += piece;'),
    ('a-second-tap-is-granted', '\tif (s->tap != NULL)\n\t\treturn false;\n',
     ''),
    ('a-tap-read-cuts-a-record', '\t\tif (bytes > capacity - done)\n\t\t\tbreak;',
     '\t\tif (done >= capacity)\n\t\t\tbreak;\n\t\tif (bytes > capacity - done)\n\t\t\tbytes = (uint32_t)(capacity - done);'),
    ('a-tap-read-takes-one-record', '\t\ts->tap_fill -= bytes;\n\t}\n\treturn done;',
     '\t\ts->tap_fill -= bytes;\n\t\tbreak;\n\t}\n\treturn done;'),
    ('a-lost-record-is-not-counted', '\ts->tap_dropped += records;\n\ts->tap_dropped_total += records;',
     '\ts->tap_dropped_total += records;'),
    ('the-silence-is-the-old-generations', '\t\ttap_span(s, s->generation, s->total_given + n, now_us, true, out + n * 2, frames - n);',
     '\t\ttap_span(s, r.generation, s->total_given + n, now_us, true, out + n * 2, frames - n);'),
    ('the-silence-index-starts-at-the-sound', '\t\ttap_span(s, s->generation, s->total_given + n, now_us, true,',
     '\t\ttap_span(s, s->generation, s->total_given, now_us, true,'),
    ('unity-is-multiplied', '\tif (gain_q15 == GAIN_UNITY)\n\t\treturn sample;\n',
     '\tif (gain_q15 == GAIN_UNITY)\n\t\treturn (int16_t)(sample - (sample > 100));\n'),
    ('the-gain-rounds-down', '\treturn (int16_t)(((int32_t)sample * gain_q15) / GAIN_UNITY);',
     '\treturn (int16_t)(((int32_t)sample * gain_q15) >> 15);'),
    ('the-gain-is-linear', '\ts->gain_q15 = (int32_t)(volume * volume * (uint32_t)GAIN_UNITY / 10000u);',
     '\ts->gain_q15 = (int32_t)(volume * (uint32_t)GAIN_UNITY / 100u);'),
    ('a-volume-over-100-is-taken', '\tif (volume > 100)\n\t\treturn false;\n',
     ''),
    ('the-right-channel-has-no-gain', '\t\tout[i * 2 + 1] = apply_gain(s->ring[s->head * 2 + 1], s->gain_q15);',
     '\t\tout[i * 2 + 1] = s->ring[s->head * 2 + 1];'),
    ('a-stranger-pulls', '\tif (sink != s->sink)\n\t{\n\t\ts->refused_pulls++;\n\t\tr.yours = false;\n\t\treturn r;\n\t}',
     '\tif (sink != s->sink)\n\t{\n\t\ts->refused_pulls++;\n\t\tr.yours = false;\n\t}'),
    ('a-refused-pull-is-not-counted', '\t\ts->refused_pulls++;\n',
     ''),
    ('a-paused-stream-is-pulled', '\tif (s->paused)\n\t{\n\t\tr.paused = true;\n\t\treturn r;\n\t}',
     '\tif (s->paused)\n\t{\n\t\tr.paused = true;\n\t}'),
    ('a-pause-is-not-said', '\t\tr.paused = true;\n\t\treturn r;',
     '\t\treturn r;'),
    ('old-frames-count-as-taken', '\t\tn = frames < s->old_frames ? frames : s->old_frames;\n\t\ts->old_frames -= n;',
     '\t\tn = frames < s->old_frames ? frames : s->old_frames;\n\t\ts->old_frames -= n;\n\t\ts->taken += n;'),
    ('old-frames-carry-the-new-generation', '\t\tr.generation = s->old_generation;\n',
     ''),
    ('old-frames-are-never-finished', '\t\ts->old_frames -= n;\n',
     ''),
    ('a-pull-crosses-the-generation', '\t\tn = frames < s->old_frames ? frames : s->old_frames;',
     '\t\tn = frames < s->fill ? frames : s->fill;'),
    ('a-pull-takes-more-than-the-ring', '\t\tn = frames < s->fill ? frames : s->fill;\n\t\ts->taken += n;',
     '\t\tn = frames;\n\t\ts->taken += n;'),
    ('taken-is-not-counted', '\t\tn = frames < s->fill ? frames : s->fill;\n\t\ts->taken += n;',
     '\t\tn = frames < s->fill ? frames : s->fill;'),
    ('the-head-does-not-wrap', '\t\tif (++s->head == s->capacity)\n\t\t\ts->head = 0;\n\t}\n\ts->fill -= n;',
     '\t\t++s->head;\n\t}\n\ts->fill -= n;'),
    ('a-pull-leaves-the-fill', '\ts->fill -= n;\n\tfor (uint32_t i = n; i < frames; i++)',
     '\tfor (uint32_t i = n; i < frames; i++)'),
    ('the-shortfall-is-not-silenced', '\tfor (uint32_t i = n; i < frames; i++)\n\t{\n\t\tout[i * 2] = 0;\n\t\tout[i * 2 + 1] = 0;\n\t}',
     ''),
    ('a-pull-reports-what-was-asked', '\tr.frames = n;',
     '\tr.frames = frames;'),
    ('a-pull-is-not-stamped', '\ts->taken_micros = now_us;\n',
     ''),
    ('given-counts-only-sound', '\ts->total_given += frames;',
     '\ts->total_given += n;'),
    ('silence-is-not-counted', '\ts->total_silence += frames - n;',
     ''),
    ('an-exact-emptying-is-an-underrun', '\tif (n < frames && s->fill == 0 && s->open',
     '\tif (n <= frames && s->fill == 0 && s->open'),
    ('a-closed-stream-underruns', '\tif (n < frames && s->fill == 0 && s->open && s->written',
     '\tif (n < frames && s->fill == 0 && s->written'),
    ('an-unwritten-stream-underruns', '&& s->open && s->written && !s->failed && !s->dry_counted)',
     '&& s->open && !s->failed && !s->dry_counted)'),
    ('every-dry-pull-is-an-underrun', '&& s->written && !s->failed && !s->dry_counted)',
     '&& s->written && !s->failed)'),
    ('a-failed-stream-underruns', '&& s->written && !s->failed && !s->dry_counted)',
     '&& s->written && !s->dry_counted)'),
    ('an-underrun-is-not-counted-since-boot', '\t\ts->underruns++;\n\t\ts->total_underruns++;',
     '\t\ts->underruns++;'),
    ('pause-with-nothing-open', '\tif (!s->open || s->paused || s->failed)\n\t\treturn false;',
     '\tif (s->paused || s->failed)\n\t\treturn false;'),
    ('a-second-pause-is-accepted', '\tif (!s->open || s->paused || s->failed)',
     '\tif (!s->open || s->failed)'),
    ('a-failed-stream-pauses', '\tif (!s->open || s->paused || s->failed)',
     '\tif (!s->open || s->paused)'),
    ('resume-when-not-paused', '\tif (!s->paused)\n\t\treturn false;\n\ts->paused = false;',
     '\ts->paused = false;'),
    ('resume-does-nothing', '\tif (!s->paused)\n\t\treturn false;\n\ts->paused = false;',
     '\tif (!s->paused)\n\t\treturn false;'),
    ('an-idle-flush-moves-the-generation', '\tif (!s->open && s->fill == 0)\n\t\treturn;   // nothing queued and nobody to tell\n',
     ''),
    ('a-draining-flush-does-nothing', '\tif (!s->open && s->fill == 0)',
     '\tif (!s->open)'),
    ('an-attach-keeps-the-failure', '\ts->sink = sink;\n\ts->failed = false;',
     '\ts->sink = sink;'),
    ('an-attach-keeps-the-delay', '\ts->failed = false;\n\ts->delay_us = 0;\n\tretire_everything(s);',
     '\ts->failed = false;\n\tretire_everything(s);'),
    ('an-attach-keeps-the-ring', '\ts->delay_us = 0;\n\tretire_everything(s);',
     '\ts->delay_us = 0;'),
    ('a-stranger-detaches', 'void audio_stream_sink_detach(audio_stream_t *s, const void *sink)\n{\n\tif (s->sink != sink)',
     'void audio_stream_sink_detach(audio_stream_t *s, const void *sink)\n{\n\tif (s->sink != sink && 0)'),
    ('a-stranger-fails-the-sink', 'void audio_stream_sink_failed(audio_stream_t *s, const void *sink)\n{\n\tif (s->sink != sink)',
     'void audio_stream_sink_failed(audio_stream_t *s, const void *sink)\n{\n\tif (s->sink != sink && 0)'),
    ('a-failure-is-not-recorded', '\ts->failed = true;\n\ts->head = 0;',
     '\ts->head = 0;'),
    ('a-failure-keeps-the-ring', '\ts->failed = true;\n\ts->head = 0;\n\ts->fill = 0;',
     '\ts->failed = true;\n\ts->head = 0;'),
    ('a-failure-keeps-the-old-frames', '\ts->fill = 0;\n\ts->old_frames = 0;\n\ts->split_len = 0;\n}',
     '\ts->fill = 0;\n\ts->split_len = 0;\n}'),
    ('a-stranger-sets-the-delay', '\tif (s->sink == sink)\n\t\ts->delay_us = delay_us;',
     '\tif (s->sink == sink || 1)\n\t\ts->delay_us = delay_us;'),
    ('the-delay-is-in-milliseconds', '\tuint64_t behind = (uint64_t)s->delay_us * s->rate / 1000000u;',
     '\tuint64_t behind = (uint64_t)s->delay_us * s->rate / 1000u;'),
    ('heard-goes-below-zero', '\tout->heard = s->taken > behind ? s->taken - behind : 0;',
     '\tout->heard = s->taken - behind;'),
    ('heard-is-taken', '\tout->heard = s->taken > behind ? s->taken - behind : 0;',
     '\tout->heard = s->taken > behind ? s->taken : 0;'),
    ('the-position-is-accepted', '\tout->taken = s->taken;',
     '\tout->taken = s->accepted;'),
    ('the-position-stamp-is-missing', '\tout->micros = s->taken_micros;',
     '\tout->micros = 0;'),
    ('queued-leaves-out-the-old-frames', '\tout->queued = s->fill;',
     '\tout->queued = s->fill - s->old_frames;'),
    ('the-record-does-not-say-its-size', '\tout->size = (uint32_t)sizeof(*out);',
     '\tout->size = 0;'),
    ('the-writer-is-woken-for-any-room', '\treturn s->capacity - s->fill >= s->capacity / 4;',
     '\treturn s->capacity - s->fill >= 1;'),
    ('a-paused-stream-has-work', '\treturn !s->failed && !s->paused && (s->open || s->fill > 0);',
     '\treturn !s->failed && (s->open || s->fill > 0);'),
    ('a-draining-stream-has-no-work', '\treturn !s->failed && !s->paused && (s->open || s->fill > 0);',
     '\treturn !s->failed && !s->paused && s->open;'),
    ('an-open-empty-stream-has-no-work', '\treturn !s->failed && !s->paused && (s->open || s->fill > 0);',
     '\treturn !s->failed && !s->paused && s->fill > 0;'),
    ('a-failed-stream-has-work', '\treturn !s->failed && !s->paused && (s->open || s->fill > 0);',
     '\treturn !s->paused && (s->open || s->fill > 0);'),
    ('a-clock-behind-its-anchor-wraps', '\tif (now_us <= anchor_us)\n\t\treturn 0;\n',
     ''),
    ('the-clock-forgets-what-it-took', '\tuint64_t due = owed - taken;',
     '\tuint64_t due = owed;'),
    ('a-clock-ahead-wraps', '\tif (owed <= taken)\n\t\treturn 0;\n',
     ''),
    ('exactly-a-ring-is-a-slip', '\tif (due > capacity)\n\t{\n\t\t*slip = true;',
     '\tif (due >= capacity)\n\t{\n\t\t*slip = true;'),
    ('a-slip-is-not-said', '\t\t*slip = true;\n\t\treturn capacity;',
     '\t\treturn capacity;'),
    ('a-slip-takes-everything-owed', '\t\t*slip = true;\n\t\treturn capacity;',
     '\t\t*slip = true;\n\t\treturn (uint32_t)due;'),
    ('the-clock-rounds-each-wake', '\tuint64_t owed = (now_us - anchor_us) * rate / 1000000u;',
     '\tuint64_t owed = (now_us - anchor_us) / 1000u * rate / 1000u;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / STREAM
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {STREAM}', file=sys.stderr)
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
            print(f'{name}: {STREAM} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\naudio_stream mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
