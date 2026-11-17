#!/usr/bin/env python3
"""One deliberate break at a time to the frame clock's rules
(kernel/src/gui/frame_clock.c: the frame's facts, the next blank, the
bands, the verdict on a copy, the lead, the strips, when the clock runs, and
measuring the period), against tools/test_frame_clock_host.sh: a mutant is
CAUGHT when the suite fails, MISSED when it passes, which means a test is
missing and gets written (or the mutant changes nothing the rule promises,
and is said so in EXPLAINED below). A build failure is neither. A mutant that
loops forever is caught by the timeout.

The shape is test_xhci_service_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run
it from a clean tree, and never beside another harness that edits the same
tree; the per-mutant logs are in --logs. Under tools/mutants_parallel.py it
needs `--with kernel`: the runner's copies do not carry the kernel's sources
unless asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/frame-clock-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CLOCK = 'kernel/src/gui/frame_clock.c'
CASES = 'bash tools/test_frame_clock_host.sh 1500'

mutants = [
    # The frame's facts.
    ('a-frame-may-be-huge', 'f->lines <= 32768', 'f->lines <= 65536'),
    ('a-blank-may-be-empty', 'f->blank_start != f->blank_end &&\n', '\n'),
    ('a-frame-needs-no-period', '	       f->period_ns > 0 && f->period_ns <= 1000000000ull;', '	       f->period_ns <= 1000000000ull;'),
    ('a-period-may-be-endless', '	       f->period_ns > 0 && f->period_ns <= 1000000000ull;', '	       f->period_ns > 0;'),
    ('the-shown-rows-are-the-blank', 'return (f->blank_start + f->lines - f->blank_end) % f->lines;',
     'return (f->blank_end + f->lines - f->blank_start) % f->lines;'),
    ('the-sweep-counts-from-the-blank-start', 'return (line % f->lines + f->lines - f->blank_end) % f->lines;',
     'return (line % f->lines + f->lines - f->blank_start) % f->lines;'),
    ('the-blank-misses-its-first-line', 'return frame_clock_sweep(f, line) >= frame_clock_active_rows(f);',
     'return frame_clock_sweep(f, line) > frame_clock_active_rows(f);'),
    ('the-frame-has-one-more-line', 'return frame_clock_frame_valid(f) && raw < f->lines;',
     'return frame_clock_frame_valid(f) && raw <= f->lines;'),
    # The next blank.
    ('the-next-blank-rounds-up', '	return now_us + lines_to * f->period_ns / f->lines / 1000;',
     '	return now_us + (lines_to * f->period_ns / f->lines + 999) / 1000;'),
    ('the-next-blank-is-its-end', 'uint64_t lines_to = (f->blank_start + f->lines - line % f->lines) % f->lines;',
     'uint64_t lines_to = (f->blank_end + f->lines - line % f->lines) % f->lines;'),
    # The bands.
    ('the-last-shown-row-is-the-blank', '	if (active == 0 || s >= active)\n		return FC_BAND_BLANK;',
     '	if (active == 0 || s + 1 >= active)\n		return FC_BAND_BLANK;'),
    ('the-screen-has-seven-bands', 'return (uint32_t)((uint64_t)s * 8 / active);', 'return (uint32_t)((uint64_t)s * 7 / active);'),
    # The verdict.
    ('a-begin-beyond-the-frame-is-judged', '	if (!frame_clock_frame_valid(f) || begin_line >= f->lines || end_line >= f->lines)\n		return v;\n	v.band',
     '	if (!frame_clock_frame_valid(f) || end_line >= f->lines)\n		return v;\n	v.band'),
    ('an-end-beyond-the-frame-is-judged', '	if (!frame_clock_frame_valid(f) || begin_line >= f->lines || end_line >= f->lines)\n		return v;\n	v.band',
     '	if (!frame_clock_frame_valid(f) || begin_line >= f->lines)\n		return v;\n	v.band'),
    ('the-band-is-not-said', '	v.band = frame_clock_band(f, begin_line);\n', ''),
    ('the-blank-is-not-said', '	v.began_in_blank = v.band == FC_BAND_BLANK;', '	v.began_in_blank = false;'),
    ('huge-pixel-counts-are-not-scaled', '	while ((sum >> shift) >= FC_PIXELS_MAX)', '	while (0 && (sum >> shift) >= FC_PIXELS_MAX)'),
    ('the-time-never-adds-a-frame', '		for (int64_t round = 1; round <= 2; round++) {', '		for (int64_t round = 1; round <= 0; round++) {'),
    ('the-time-picks-the-farthest', '			if (off < best_off)', '			if (off > best_off)'),
    ('a-frame-exactly-is-not-a-frame', '		outlasted = d >= lines;', '		outlasted = d > lines;'),
    ('an-enormous-time-is-not-guarded', '	if (elapsed_us >= 2 * f->period_ns / 1000 + 1) {', '	if (0) {'),
    ('a-copy-outlasting-a-frame-is-judged', '		if (outlasted) {', '		if (outlasted && 0) {'),
    ('the-pass-after-is-not-looked-at', '		for (int64_t pass = 0; pass <= 1 && !v.torn; pass++)', '		for (int64_t pass = 0; pass <= 0 && !v.torn; pass++)'),
    ('the-pass-begun-in-is-not-looked-at', '		for (int64_t pass = 0; pass <= 1 && !v.torn; pass++)', '		for (int64_t pass = 1; pass <= 1 && !v.torn; pass++)'),
    ('rows-below-the-picture-are-judged', '		int64_t lo = top < 0 ? 0 : top, hi = pieces[i].bottom > active ? active : pieces[i].bottom;',
     '		int64_t lo = top < 0 ? 0 : top, hi = pieces[i].bottom + 0 * active;'),
    ('rows-above-the-picture-are-judged', '		int64_t lo = top < 0 ? 0 : top, hi = pieces[i].bottom > active ? active : pieces[i].bottom;',
     '		int64_t lo = top, hi = pieces[i].bottom > active ? active : pieces[i].bottom;'),
    ('a-piece-of-no-pixels-is-judged', '		if (pieces[i].pixels == 0 || pieces[i].top >= pieces[i].bottom)\n			continue;',
     '		if (pieces[i].top >= pieces[i].bottom)\n			continue;'),
    ('a-piece-takes-no-share-of-the-time', '		done += px;\n', '		(void)px;\n'),
    ('a-read-as-the-copy-begins-is-new', '	if (tr <= ta)\n		return false;', '	if (tr < ta)\n		return false;'),
    ('a-read-as-the-copy-ends-is-old', '	if (tr >= tb)\n		return true;', '	if (tr > tb)\n		return true;'),
    ('a-row-is-new-when-the-front-reaches-it', '(__int128)(r + 1 - top) * (tb - ta)', '(__int128)(r - top) * (tb - ta)'),
    ('a-row-is-new-only-after-the-front-passes', '<= (__int128)height * (tr - ta);', '< (__int128)height * (tr - ta);'),
    ('the-first-row-during-is-one-early', 'int64_t first_during = ta / total + 1 - pass * lines;', 'int64_t first_during = ta / total - pass * lines;'),
    ('the-first-row-after-is-rounded-down', 'int64_t first_after = (tb + total - 1) / total - pass * lines;', 'int64_t first_after = tb / total - pass * lines;'),
    ('only-the-ends-are-looked-at', '{ lo, hi - 1, first_during - 1, first_during, first_after - 1, first_after }',
     '{ lo, hi - 1, lo + 0 * first_during, lo + 0 * first_after, lo, lo }'),
    ('the-stretches-ends-are-not-clamped-low', 'int64_t r = rows[i] < lo ? lo : rows[i] > hi - 1 ? hi - 1 : rows[i];',
     'int64_t r = rows[i] > hi - 1 ? hi - 1 : rows[i];'),
    ('the-stretches-ends-are-not-clamped-high', 'int64_t r = rows[i] < lo ? lo : rows[i] > hi - 1 ? hi - 1 : rows[i];',
     'int64_t r = rows[i] < lo ? lo : rows[i];'),
    # The lead.
    ('no-passes-is-the-least-lead', '	if (l->count == 0)\n		return FC_LEAD_MAX_US;', '	if (l->count == 0)\n		return FC_LEAD_MIN_US;'),
    ('the-margin-is-fixed', 'uint64_t margin = worst / 4 > FC_LEAD_MARGIN_US ? worst / 4 : FC_LEAD_MARGIN_US;', 'uint64_t margin = FC_LEAD_MARGIN_US;'),
    ('the-margin-is-a-quarter-only', 'uint64_t margin = worst / 4 > FC_LEAD_MARGIN_US ? worst / 4 : FC_LEAD_MARGIN_US;', 'uint64_t margin = worst / 4;'),
    ('the-lead-has-no-floor', '	if (lead < FC_LEAD_MIN_US)\n		return FC_LEAD_MIN_US;\n', ''),
    ('the-lead-has-no-ceiling', '	if (lead > FC_LEAD_MAX_US)\n		return FC_LEAD_MAX_US;\n', ''),
    ('the-lead-follows-the-last-pass', '		if (l->cost_us[i] > worst)', '		if (1)'),
    ('the-window-never-forgets', '	l->next = (l->next + 1) % FC_LEAD_PASSES;', '	l->next = l->next + 1 < FC_LEAD_PASSES ? l->next + 1 : FC_LEAD_PASSES - 1;'),
    ('an-absurd-pass-wraps', 'pass_us > UINT32_MAX ? UINT32_MAX : (uint32_t)pass_us', '(uint32_t)pass_us'),
    # The strips.
    ('a-part-starts-above-its-strip', '		int64_t y0 = r->y > strip_top ? r->y : strip_top;', '		int64_t y0 = r->y;'),
    ('a-part-ends-below-its-strip', '		int64_t y1 = (int64_t)r->y + r->h < strip_end ? (int64_t)r->y + r->h : strip_end;', '		int64_t y1 = (int64_t)r->y + r->h + 0 * strip_end;'),
    ('an-empty-rectangle-is-a-part', '		if (r->w <= 0 || y0 >= y1)', '		if (y0 >= y1)'),
    ('the-parts-are-not-sorted', '		while (at > 0 && out[at - 1].x > piece.x) {', '		while (0 && at > 0 && out[at - 1].x > piece.x) {'),
    ('overlapping-parts-are-not-merged', '			if (out[i].x < box_right) {', '			if (0) {'),
    ('touching-parts-are-merged', '			if (out[i].x < box_right) {', '			if (out[i].x <= box_right) {'),
    ('a-merged-box-keeps-its-height', '				box->y = (int32_t)y0;\n				box->h = (int32_t)(y1 - y0);\n', '				(void)y0; (void)y1;\n'),
    ('a-merged-box-ends-where-the-part-does', '? (int64_t)out[i].x + out[i].w : box_right;', '? (int64_t)out[i].x + out[i].w : (int64_t)out[i].x + out[i].w;'),
    # When the clock runs.
    ('the-linger-is-a-frame-short', '	return r->idle_frames <= FC_LINGER_FRAMES;', '	return r->idle_frames < FC_LINGER_FRAMES;'),
    ('a-waiter-does-not-run-it', '	if (damage || waiting > 0) {', '	if (damage || (waiting > 0 && 0)) {'),
    ('activity-does-not-restart-the-linger', '		r->idle_frames = 0;\n		return true;', '		return true;'),
    ('the-idle-count-wraps', '	if (r->idle_frames <= FC_LINGER_FRAMES)\n		r->idle_frames++;', '	r->idle_frames++;'),
    # Measuring the period.
    ('a-reading-beyond-the-frame-is-a-line', '	if (m->lines == 0 || raw >= m->lines) {', '	if (m->lines == 0) {'),
    ('a-wrap-must-drop-a-whole-frame', '		if (m->prev - raw > m->lines / 2)', '		if (m->prev - raw > m->lines - 1)'),
    ('the-longest-gap-is-not-kept', '	if (gap > m->gap_max_us)\n		m->gap_max_us = gap;\n', ''),
    ('a-step-back-across-a-gap-is-backward', '		else if (gap * 2 >= 1000000 / FC_MEASURE_HZ_MAX)\n			m->hidden++;', '		else if (0)\n			m->hidden++;'),
    ('a-hidden-wrap-is-no-reason', '	if (m->hidden > 0 || (m->done && m->gap_max_us * 2000 >= period))', '	if (m->done && m->gap_max_us * 2000 >= period)'),
    ('a-change-across-a-stall-is-an-edge', '	if (raw != m->prev && gap <= FC_MEASURE_EDGE_US) {', '	if (raw != m->prev) {'),
    ('the-window-needs-no-frames', ' &&\n		           pos - m->anchor_pos >= (uint64_t)FC_MEASURE_WRAPS * m->lines) {', ') {'),
    ('a-gap-may-be-a-whole-frame', 'm->gap_max_us * 2000 >= period', 'm->gap_max_us * 1000 >= period'),
    ('the-rate-has-no-floor', '	if (period < 1000000000ull / FC_MEASURE_HZ_MAX || period > 1000000000ull / FC_MEASURE_HZ_MIN)',
     '	if (period < 1000000000ull / FC_MEASURE_HZ_MAX)'),
    ('the-rate-has-no-ceiling', '	if (period < 1000000000ull / FC_MEASURE_HZ_MAX || period > 1000000000ull / FC_MEASURE_HZ_MIN)',
     '	if (period > 1000000000ull / FC_MEASURE_HZ_MIN)'),
    ('the-period-counts-a-line-short', '		period = (m->end_us - m->anchor_us) * 1000 * m->lines / (m->end_pos - m->anchor_pos);',
     '		period = (m->end_us - m->anchor_us) * 1000 * (m->lines - 1) / (m->end_pos - m->anchor_pos);'),
    ('a-still-counter-is-too-few-wraps', '	if (!m->anchored)\n		return FC_MEASURE_STILL;', '	if (!m->anchored)\n		return FC_MEASURE_TOO_FEW_WRAPS;'),
    ('a-step-back-is-no-reason', '	if (m->backward > 0)\n		return FC_MEASURE_BACKWARD;\n', ''),
]

# Mutants that change nothing a rule promises, and why. Run, and reported
# as explained rather than missed.
EXPLAINED = {
    'a-read-as-the-copy-ends-is-old':
        'at the instant a piece\'s copy ends its front has finished every row, so the linear test answers new there too',
    'the-first-row-during-is-one-early':
        'the first row read during a copy is never the only one that differs: with the front slower than the sweep it '
        'is old like the rows read before the copy; faster, it is the oldest of the copy\'s stretch, so either rows read '
        'before the copy exist (old, and the piece\'s first row stands for them) or it is the piece\'s first row itself',
    'the-first-row-after-is-rounded-down':
        'a row read after the copy is new like every row after it, and the piece\'s last row stands for them; rounding '
        'down only moves the evaluated pair onto the copy\'s last row, which is evaluated either way',
}

caught, missed, unbuilt, explained = [], [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / CLOCK
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {CLOCK}', file=sys.stderr)
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
            print(f'{name}: {CLOCK} WAS NOT RESTORED', file=sys.stderr)
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
    elif name in EXPLAINED:
        explained.append(name)
        verdict = 'explained: ' + EXPLAINED[name]
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nframe_clock mutants: {len(caught)} caught, {len(explained)} explained, {len(missed)} missed, '
      f'{len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
