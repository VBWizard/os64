#!/usr/bin/env python3
"""One deliberate break at a time to libmedia's three cores for a video
player (userland/libmedia/media_pictures.c, media_clock.c, media_pace.c),
against tools/test_media_cores_host.sh: a mutant is CAUGHT when the suite
fails, MISSED when it passes, which means a test is missing and gets written.
A build failure is neither.

The shape is test_media_keys_mutants.py's, with the file named per mutant:
each is applied to the tree's copy of its file, the suite is run, and the file
is put back from the bytes read before the change, checked byte for byte,
whatever happens. Run it from a clean tree, and never beside another harness
that edits the same tree; the per-mutant logs are in --logs. Under
tools/mutants_parallel.py it needs no --with: the cores are in userland/.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/media-cores-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

Q = 'userland/libmedia/media_pictures.c'
C = 'userland/libmedia/media_clock.c'
P = 'userland/libmedia/media_pace.c'
CASES = 'bash tools/test_media_cores_host.sh'

mutants = [
    # The picture queue.
    ('q-a-stale-push-is-taken', Q, 'if (generation != q->generation || q->ended || handle == 0)',
     'if ((generation != q->generation && 0) || q->ended || handle == 0)'),
    ('q-a-push-after-the-end-is-taken', Q, 'if (generation != q->generation || q->ended || handle == 0)',
     'if (generation != q->generation || handle == 0)'),
    ('q-a-null-handle-is-taken', Q, 'if (generation != q->generation || q->ended || handle == 0)',
     'if (generation != q->generation || q->ended)'),
    ('q-a-full-queue-overwrites', Q, '    if (q->count == q->capacity)\n        return OS64_MEDIA_PICTURES_FULL;\n', ''),
    ('q-a-push-lands-on-the-front', Q, 'q->slot[(q->head + q->count) % q->capacity] =',
     'q->slot[q->head % q->capacity] ='),
    ('q-peek-reads-past-the-back', Q, 'if (index >= q->count)', 'if (index > q->count)'),
    ('q-peek-ignores-the-head', Q, 'return &q->slot[(q->head + index) % q->capacity];',
     'return &q->slot[index % q->capacity];'),
    ('q-newest-is-the-front', Q, 'return q->slot[(q->head + q->count - 1) % q->capacity].at;',
     'return q->slot[q->head].at;'),
    ('q-take-does-not-advance', Q, 'q->head = (q->head + 1) % q->capacity;', 'q->head = q->head % q->capacity;'),
    ('q-a-flush-releases-nothing', Q, '            release(context, e.handle);', '            (void)context;'),
    ('q-a-flush-keeps-the-end', Q, '    q->head = 0;\n    q->ended = false;\n', '    q->head = 0;\n'),
    ('q-a-flush-keeps-the-generation', Q, 'return ++q->generation;', 'return q->generation;'),
    ('q-an-end-ignores-its-generation', Q, '    if (generation != q->generation)\n        return false;\n    q->ended = true;',
     '    (void)generation;\n    q->ended = true;'),
    ('q-finished-before-taken', Q, 'return q->ended && q->count == 0;', 'return q->ended;'),
    ('q-full-one-early', Q, 'return q->count == q->capacity;', 'return q->count + 1 >= q->capacity;'),
    # The clock.
    ('c-the-rate-is-wrong', C, '#define UNITS_PER_US 120 ', '#define UNITS_PER_US 125 '),
    ('c-the-slew-is-a-tenth', C, '(UNITS_PER_US / 20)', '(UNITS_PER_US / 10)'),
    ('c-the-slew-is-a-120th', C, '(UNITS_PER_US / 20)', '(1)'),
    ('c-negative-drift-stays', C, 'return d < 0 ? -worked_size(-d, us) : 0;', 'return d < 0 ? d : 0;'),
    ('c-negative-drift-is-dropped', C, 'return d < 0 ? -worked_size(-d, us) : 0;', 'return 0;'),
    ('c-drift-overshoots', C, '    return left > 0 ? left : 0;', '    return left;'),
    ('c-no-near-rate', C, '#define NEAR_UNITS (480 * UNITS_PER_FRAME)', '#define NEAR_UNITS 0'),
    ('c-near-is-5-ms', C, '#define NEAR_UNITS (480 * UNITS_PER_FRAME)', '#define NEAR_UNITS (240 * UNITS_PER_FRAME)'),
    ('c-near-rate-a-240th', C, '#define NEAR_US_PER_UNIT 4', '#define NEAR_US_PER_UNIT 2'),
    ('c-near-forgets-the-fast-part', C, '- fast_us * SLEW_UNITS_PER_US - (us - fast_us) / NEAR_US_PER_UNIT',
     '- (us - fast_us) / NEAR_US_PER_UNIT'),
    ('c-near-counts-the-fast-time-too', C, '- (us - fast_us) / NEAR_US_PER_UNIT', '- us / NEAR_US_PER_UNIT'),
    ('c-frames-round-toward-zero', C, 'return units % UNITS_PER_FRAME < 0 ? q - 1 : q;', 'return q;'),
    ('c-a-time-before-the-event-is-read', C, 'return t < c->since ? c->since : t;', 'return t < c->since ? t : t;'),
    ('c-a-start-does-not-hold', C, 'return t < c->hold_until ? c->base : c->base + run(t - c->hold_until);',
     'return c->base + run(t - c->since);'),
    ('c-a-start-holds-for-ever', C, 'return t < c->hold_until ? c->base : c->base + run(t - c->hold_until);',
     'return c->base;'),
    ('c-following-forgets-the-drift', C, 'return heard_view(c, f) + slewed(c->drift, f - c->slew_since);',
     'return heard_view(c, f) + (slewed(c->drift, f - c->slew_since) & 0);'),
    ('c-the-slew-starts-at-the-report', C, 'return heard_view(c, f) + slewed(c->drift, f - c->slew_since);',
     'return heard_view(c, f) + slewed(c->drift, f - c->since);'),
    # The still ear.
    ('c-no-still', C, '    if (t > until)\n        t = until;\n', '    (void)until;\n'),
    ('c-a-stopped-ear-reads-following', C,
     'return ear_stopped(c, at_or_after(c, t)) ? OS64_MEDIA_CLOCK_STILL : OS64_MEDIA_CLOCK_FOLLOWING;',
     'return OS64_MEDIA_CLOCK_FOLLOWING;'),
    ('c-the-trust-is-ignored', C, '    c->still_next = us > 0 ? us : 1;', '    (void)c;\n    (void)us;'),
    ('c-a-trust-below-one-is-taken', C, '    c->still_next = us > 0 ? us : 1;', '    c->still_next = us;'),
    ('c-a-new-trust-applies-at-once', C, '    c->still_next = us > 0 ? us : 1;',
     '    c->still_us = c->still_next = us > 0 ? us : 1;'),
    ('c-a-new-trust-never-taken', C, '    c->still_us = c->still_next;', '    (void)0;'),
    ('c-a-still-ear-reads-starved', C,
     'return ear_stopped(c, at_or_after(c, t)) ? OS64_MEDIA_CLOCK_STILL : OS64_MEDIA_CLOCK_FOLLOWING;',
     'return ear_stopped(c, at_or_after(c, t)) ? OS64_MEDIA_CLOCK_STARVED : OS64_MEDIA_CLOCK_FOLLOWING;'),
    ('c-stills-not-counted', C, '            c->stills++;', '            (void)0;'),
    ('c-the-start-wait-not-kept', C, '        c->start_wait_us = now - c->since;', '        (void)0;'),
    ('c-ages-not-counted', C, '    c->report_ages[i]++;', '    (void)c;\n    (void)i;'),
    ('c-drift-ignores-the-still', C, '    return frames(reading(c, t) - heard_view(c, follow_t(c, t)));',
     '    return frames(reading(c, t) - heard_view(c, t));'),
    # The bound.
    ('c-no-hold-ahead', C, '        if (f < c->slew_since)\n            return c->base;\n', ''),
    ('c-ahead-unbounded', C, '    if (d > BOUND_UNITS) {', '    if (false) {'),
    ('c-behind-unbounded', C, '    } else if (d < -BOUND_UNITS) {', '    } else if (false) {'),
    ('c-the-bound-doubled', C, '#define BOUND_UNITS ((int64_t)OS64_MEDIA_CLOCK_BOUND_US * UNITS_PER_US)',
     '#define BOUND_UNITS ((int64_t)OS64_MEDIA_CLOCK_BOUND_US * 2 * UNITS_PER_US)'),
    ('c-the-hold-ends-early', C, '(d - BOUND_UNITS + UNITS_PER_US - 1) / UNITS_PER_US',
     '(d - BOUND_UNITS + UNITS_PER_US - 1) / (2 * UNITS_PER_US)'),
    ('c-a-hold-counted-each-report', C, '        if (!holding)\n            c->held_ahead++;',
     '        (void)holding;\n        c->held_ahead++;'),
    ('c-a-step-behind-not-counted', C, '        c->jumped_behind++;', '        ;'),
    ('c-the-hold-has-no-length', C, 'c->hold_until = now + c->hold_us;', 'c->hold_until = now;'),
    ('c-the-hold-ignores-its-bound', C, 'c->hold_until = now + c->hold_us;',
     'c->hold_until = now + OS64_MEDIA_CLOCK_START_HOLD_US;'),
    ('c-a-bound-below-one-is-taken', C, 'c->hold_us = us > 0 ? us : 1;', 'c->hold_us = us;'),
    ('c-a-heard-start-runs', C, '    if (c->source == OS64_MEDIA_CLOCK_HEARD) {\n        c->mode = MODE_STARTING;',
     '    if (0) {\n        c->mode = MODE_STARTING;'),
    ('c-a-late-start-is-not-counted', C, '        c->starts_late++;', '        (void)0;'),
    ('c-play-while-playing-restarts', C, '    if (c->mode != MODE_PAUSED)\n        return;\n    start(', '    start('),
    ('c-a-paused-seek-plays', C, '    if (c->mode == MODE_PAUSED) {\n        c->base = value;',
     '    if (0) {\n        c->base = value;'),
    ('c-a-seek-keeps-the-generation', C, 'return ++c->generation;', 'return c->generation;'),
    ('c-the-same-source-starts-over', C, '    if (source == c->source)\n        return;\n', ''),
    ('c-unmute-takes-no-reports', C, 'c->mode = source == OS64_MEDIA_CLOCK_HEARD ? MODE_AWAITING : MODE_RUNNING;',
     'c->mode = MODE_RUNNING;'),
    ('c-a-paused-source-change-plays', C,
     '    c->source = source;\n    if (c->mode == MODE_PAUSED)\n        return;\n', '    c->source = source;\n'),
    ('c-a-stale-report-is-taken', C, '    if (generation != c->generation) {', '    if (generation != c->generation && 0) {'),
    ('c-a-report-while-paused-is-taken', C, 'if (c->mode == MODE_PAUSED || c->mode == MODE_RUNNING)',
     'if (c->mode == MODE_RUNNING)'),
    ('c-not-advancing-ends-a-wait', C, '    if (waiting && (!advancing || frame_wall < c->since))',
     '    if (waiting && (frame_wall < c->since))'),
    ('c-an-old-report-ends-a-wait', C, '    if (waiting && (!advancing || frame_wall < c->since))',
     '    if (waiting && (!advancing))'),
    ('c-starved-does-not-hold', C, '            c->mode = MODE_STARVED;', '            c->mode = MODE_FOLLOWING;'),
    ('c-a-starve-is-not-counted', C, '            c->starves++;', '            (void)0;'),
    ('c-a-report-steps', C, '    int64_t d = was - heard_view(c, now);', '    int64_t d = 0;'),
    ('c-the-ear-runs-from-the-report', C, 'return c->heard + run(t - c->heard_wall);',
     'return c->heard + run(t - c->since);'),
    ('c-drift-reads-backward', C, '    return frames(reading(c, t) - heard_view(c, follow_t(c, t)));',
     '    return frames(heard_view(c, follow_t(c, t)) - reading(c, t));'),
    ('c-drift-rounds-toward-zero', C, '    return frames(reading(c, t) - heard_view(c, follow_t(c, t)));',
     '    return (reading(c, t) - heard_view(c, follow_t(c, t))) / UNITS_PER_FRAME;'),
    ('c-never-late', C, '? OS64_MEDIA_CLOCK_STARTING : OS64_MEDIA_CLOCK_STARTING_LATE;',
     '? OS64_MEDIA_CLOCK_STARTING : OS64_MEDIA_CLOCK_STARTING;'),
    # The pace.
    ('p-the-first-due-is-shown', P, 'pick.drop = due - 1;', 'pick.drop = 0;'),
    ('p-a-picture-at-the-blank-waits', P, '!= 0 && e->at <= clock)', '!= 0 && e->at < clock)'),
    ('p-drops-are-not-counted', P, '        p->dropped += pick.drop;\n', ''),
    ('p-starved-while-not-judging', P, '} else if (judging && os64_media_pictures_count(ready) == 0 && shown != 0) {',
     '} else if ((judging || 1) && os64_media_pictures_count(ready) == 0 && shown != 0) {'),
    ('p-starved-with-a-picture-ready', P,
     '} else if (judging && os64_media_pictures_count(ready) == 0 && shown != 0) {',
     '} else if (judging && shown != 0) {'),
    ('p-starved-a-frame-late', P, 'clock >= shown->at + lasts', 'clock > shown->at + lasts'),
    ('p-no-period-for-a-picture-without-time', P, 'int64_t lasts = shown->duration > 0 ? shown->duration : p->period;',
     'int64_t lasts = shown->duration;'),
    ('p-a-timeless-picture-starves-at-once', P, 'if (lasts > 0 && clock', 'if (clock'),
    ('p-the-reach-rounds-down', P, '    if (k * period_us < gap)\n        k++;\n', ''),
    ('p-the-reach-ignores-the-cost', P, 'int64_t target = now + (cost > 0 ? cost : 0);', 'int64_t target = now + 0 * cost;'),
    ('p-a-negative-cost-counts', P, 'int64_t target = now + (cost > 0 ? cost : 0);', 'int64_t target = now + cost;'),
    ('p-skip-by-the-picture-itself', P, 'os64_media_pictures_peek(decoded, 1);', 'os64_media_pictures_peek(decoded, 0);'),
    ('p-skip-a-blank-early', P, 'if (next == 0 || next->at > clock)', 'if (next == 0 || next->at >= clock)'),
    ('p-judged-while-not-judging', P, '    if (!m->judging) {', '    if (!m->judging && 0) {'),
    ('p-judged-without-a-period', P, 'bool lead_known = p->period > 0 && m->newest >= 0;',
     'bool lead_known = m->newest >= 0;'),
    ('p-judged-with-nothing-decoded', P, 'bool lead_known = p->period > 0 && m->newest >= 0;',
     'bool lead_known = p->period > 0;'),
    ('p-not-judging-keeps-the-holds', P,
     '    if (!m->judging) {\n        p->in_since = p->out_since = -1;\n        return p->state;\n    }',
     '    if (!m->judging) {\n        return p->state;\n    }'),
    ('p-the-sound-door-never-disarms', P, '            p->sound_armed = false;\n', ''),
    ('p-the-sound-door-never-rearms', P, '    if (!short_of_sound)\n        p->sound_armed = true;\n', ''),
    ('p-the-sound-door-needs-the-lead', P, 'if (by_sound || (lead_known &&', 'if ((by_sound && lead_known) || (lead_known &&'),
    ('p-a-reset-keeps-the-ask', P, '    p->resync_asked = false;\n    p->sound_armed = true;\n', '    p->sound_armed = true;\n'),
    ('p-a-reset-keeps-the-sound-disarmed', P, '    p->resync_asked = false;\n    p->sound_armed = true;\n',
     '    p->resync_asked = false;\n'),
    ('p-taking-keeps-the-ask', P, '    p->resync_asked = false;\n    return asked;', '    return asked;'),
    ('p-far-behind-asks-again', P, '    if (p->state == OS64_MEDIA_PACE_FAR_BEHIND)\n        return p->state;\n    bool by_sound',
     '    bool by_sound'),
    ('p-far-at-thirty', P, 'p->lead < -(int64_t)OS64_MEDIA_PACE_FAR_PERIODS * p->period)',
     'p->lead <= -(int64_t)OS64_MEDIA_PACE_FAR_PERIODS * p->period)'),
    ('p-the-sound-protects-nothing', P, 'bool by_sound = short_of_sound && p->sound_armed;',
     'bool by_sound = short_of_sound && p->sound_armed && 0;'),
    ('p-the-floor-is-far', P, 'm->sound_frames < m->sound_floor;', 'm->sound_frames <= m->sound_floor;'),
    ('p-far-by-sound-is-not-counted', P, '            p->far_by_sound++;', '            (void)0;'),
    ('p-the-in-door-includes-its-edge', P, 'if (lead * 8 < OS64_MEDIA_PACE_IN_EIGHTHS * depth) {',
     'if (lead * 8 <= OS64_MEDIA_PACE_IN_EIGHTHS * depth) {'),
    ('p-the-in-door-does-not-hold', P, 'if (m->now - p->in_since >= OS64_MEDIA_PACE_IN_HOLD_US)', 'if (1)'),
    ('p-the-in-hold-is-not-broken', P, '        } else {\n            p->in_since = -1;\n        }\n', '        }\n'),
    ('p-the-in-hold-restarts-each-paint', P, '            if (p->in_since < 0)\n                p->in_since = m->now;',
     '            p->in_since = m->now;'),
    ('p-the-out-door-excludes-its-edge', P, 'if (lead * 8 >= OS64_MEDIA_PACE_OUT_EIGHTHS * depth) {',
     'if (lead * 8 > OS64_MEDIA_PACE_OUT_EIGHTHS * depth) {'),
    ('p-the-out-door-does-not-hold', P, 'if (m->now - p->out_since >= OS64_MEDIA_PACE_OUT_HOLD_US)', 'if (1)'),
    ('p-the-out-hold-is-not-broken', P, '        } else {\n            p->out_since = -1;\n        }\n', '        }\n'),
    ('p-the-doors-ignore-the-depth', P, 'int64_t depth = (int64_t)p->depth * p->period;', 'int64_t depth = 8 * p->period;'),
    ('p-an-entry-keeps-the-holds', P, '    p->entered[state]++;\n    p->in_since = p->out_since = -1;\n',
     '    p->entered[state]++;\n'),
    ('p-on-time-skips-too', P, 'p->skip_unused = state != OS64_MEDIA_PACE_ON_TIME;', 'p->skip_unused = true;'),
    ('p-no-resync-is-asked', P, '        p->resync_asked = true;', '        (void)0;'),
    ('p-resyncs-are-not-counted', P, '        p->resyncs++;', '        (void)0;'),
    ('p-far-time-is-not-counted', P,
     'static void enter(os64_media_pace_t *p, int state, int64_t now)\n{\n'
     '    if (p->state == OS64_MEDIA_PACE_FAR_BEHIND && now > p->far_since)\n'
     '        p->far_behind_us += (uint64_t)(now - p->far_since);\n',
     'static void enter(os64_media_pace_t *p, int state, int64_t now)\n{\n'),
    ('p-a-reset-loses-far-time', P,
     'void os64_media_pace_reset(os64_media_pace_t *p, int64_t now)\n{\n'
     '    if (p->state == OS64_MEDIA_PACE_FAR_BEHIND && now > p->far_since)\n'
     '        p->far_behind_us += (uint64_t)(now - p->far_since);\n',
     'void os64_media_pace_reset(os64_media_pace_t *p, int64_t now)\n{\n    (void)now;\n'),
    ('p-a-resync-from-anywhere', P,
     '    if (p->state == OS64_MEDIA_PACE_FAR_BEHIND)\n        enter(p, OS64_MEDIA_PACE_DECODING_LATE, now);',
     '    enter(p, OS64_MEDIA_PACE_DECODING_LATE, now);'),
    ('p-a-reset-keeps-the-state', P, '    p->state = OS64_MEDIA_PACE_ON_TIME;\n    p->skip_unused = false;\n',
     '    p->skip_unused = false;\n'),
    ('p-a-reset-keeps-skipping', P, '    p->state = OS64_MEDIA_PACE_ON_TIME;\n    p->skip_unused = false;\n',
     '    p->state = OS64_MEDIA_PACE_ON_TIME;\n'),
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

print(f'\nmedia cores mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
