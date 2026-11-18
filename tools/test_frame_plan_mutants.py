#!/usr/bin/env python3
"""One deliberate break at a time to yonder's frame plan
(userland/apps/yonder/frame_plan.c: when a frame is asked for, a refusal,
an answer and the frame's time, skips, sampling, the close, the next blank,
and the cost rule's rests), against tools/test_frame_plan_host.sh: a mutant
is CAUGHT when the suite fails, MISSED when it passes, which means a test is
missing and gets written. A break that changes nothing a rule promises is
not in the list; the comment after it says which and why. A build failure is
neither.

The shape is test_frame_clock_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run
it from a clean tree, and never beside another harness that edits the same
tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/frame-plan-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PLAN = 'userland/apps/yonder/frame_plan.c'
CASES = 'bash tools/test_frame_plan_host.sh'

mutants = [
    # A look.
    ('work-due-soon-asks-now', '    if (due_ms > now_ms) {', '    if (due_ms > now_ms + 5) {'),
    ('work-due-later-wakes-nothing', '        step.wake_ms = due_ms;', '        step.wake_ms = FRAME_PLAN_NEVER;'),
    ('asks-while-one-is-out', '    if (p->asked || p->open)\n        return step;', '    if (p->open)\n        return step;'),
    ('asks-while-a-frame-is-open', '    if (p->asked || p->open)\n        return step;', '    if (p->asked)\n        return step;'),
    ('an-ask-is-not-out', '    step.ask = true;\n    p->asked = true;', '    step.ask = true;'),
    ('an-ask-is-not-counted', '    p->asks++;\n', ''),
    # (Each break keeps every variable used: the suite builds with -Werror.)
    ('never-chained', '    p->chained = closed_now;', '    p->chained = closed_now && false;'),
    ('always-chained', '    p->chained = closed_now;', '    p->chained = closed_now || true;'),
    ('a-look-keeps-the-close', '    p->closed_now = false;\n    if (due_ms == FRAME_PLAN_NEVER)', '    if (due_ms == FRAME_PLAN_NEVER)'),
    # A refusal.
    ('a-refusal-leaves-one-out', '    p->asked = false;\n    p->chained = false;\n    p->asks--;', '    p->chained = false;\n    p->asks--;'),
    ('a-refusal-is-an-ask', '    p->asks--;\n', ''),
    ('a-refusal-is-not-counted', '    p->refused++;\n', ''),
    # An answer and the frame's time.
    ('answers-what-nobody-asked', '    if (!p->asked)\n        return false;\n    // Refreshes', '    // Refreshes'),
    ('skips-unchained-answers', '    if (p->chained && number > p->number + 1)', '    if (number > p->number + 1)'),
    ('skips-count-the-answer', '        p->skipped += number - p->number - 1;', '        p->skipped += number - p->number;'),
    ('an-answer-leaves-it-asked', '    p->asked = false;\n    p->chained = false;\n    p->open = true;', '    p->chained = false;\n    p->open = true;'),
    ('an-answer-opens-nothing', '    p->open = true;\n    p->answers++;', '    p->answers++;'),
    ('an-answer-is-not-counted', '    p->answers++;\n', ''),
    ('the-period-truncates', '        p->period_us = ((uint64_t)period_ns + 500) / 1000;', '        p->period_us = (uint64_t)period_ns / 1000;'),
    ('no-period-is-a-period', '    if (period_ns != 0)\n        p->period_us', '    p->period_us'),
    ('the-frame-goes-back', '    p->time_us = p->sampled_us = later(blank_us, p->sampled_us);', '    p->time_us = p->sampled_us = blank_us;'),
    ('the-frame-is-forgotten', '    p->time_us = p->sampled_us = later(blank_us, p->sampled_us);', '    p->time_us = later(blank_us, p->sampled_us);'),
    # A sample.
    ('a-frame-samples-now', '    if (p->open)\n        return p->time_us;\n    p->sampled_us', '    p->sampled_us'),
    ('a-sample-goes-back', '    p->sampled_us = later(now_us, p->sampled_us);', '    p->sampled_us = now_us;'),
    # The close.
    ('a-close-of-nothing-closes', '    if (!p->open)\n        return;\n    p->open = false;', '    p->open = false;'),
    ('a-close-leaves-it-open', '    p->open = false;\n    p->closed_now = true;', '    p->closed_now = true;'),
    ('a-close-is-not-said', '    p->open = false;\n    p->closed_now = true;', '    p->open = false;'),
    # The next blank.
    ('no-guess-is-now', '        return sum(now_us, FRAME_PLAN_PERIOD_GUESS_US);', '        return now_us;'),
    ('the-blank-now-is-next', '    uint64_t periods = (now_us - p->blank_us) / p->period_us + 1;', '    uint64_t periods = (now_us - p->blank_us) / p->period_us;'),
    ('the-last-blank-is-passed-over', '    if (now_us < p->blank_us)\n        return p->blank_us;', '    if (now_us < p->blank_us)\n        return p->blank_us + p->period_us;'),
    # The cost rule's rests.
    ('the-limit-stops-nothing', '    if (due_ms == FRAME_PLAN_NEVER || cost_us > stop_us)', '    if (due_ms == FRAME_PLAN_NEVER || (cost_us > stop_us && false))'),
    ('the-limit-itself-stops', '    if (due_ms == FRAME_PLAN_NEVER || cost_us > stop_us)', '    if (due_ms == FRAME_PLAN_NEVER || cost_us >= stop_us)'),
    ('a-first-frame-rests', '    if (!had)\n        return due_ms;\n', '    if (!had && false)\n        return due_ms;\n'),
    ('the-rest-truncates', '    uint64_t cost_ms = cost_us / 1000 + (cost_us % 1000 != 0);', '    uint64_t cost_ms = cost_us / 1000;'),
    ('the-rest-is-one-cost', ': sum(ended_ms, cost_ms * rest);', ': sum(ended_ms, cost_ms);'),
    ('the-rest-overrides-the-due', '    return later(due_ms, rested);', '    return rested;'),
    ('the-due-overrides-the-rest', '    return later(due_ms, rested);', '    return rested != 0 ? due_ms : due_ms;'),
    ('the-rest-wraps', ': sum(ended_ms, cost_ms * rest);', ': ended_ms + cost_ms * rest;'),
]

# Not run, because it changes nothing a rule promises: frame_plan_next_blank_us's
# overflow guard (`periods > (UINT64_MAX - p->blank_us) / p->period_us`) and
# frame_plan_half_due's guard on `cost_ms * rest`: no microsecond clock since
# boot and no frame's cost under the limit reaches either.

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / PLAN
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {PLAN}', file=sys.stderr)
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
            print(f'{name}: {PLAN} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nframe_plan mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
