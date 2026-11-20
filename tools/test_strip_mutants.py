#!/usr/bin/env python3
"""One deliberate break at a time to yonder's strip states
(userland/apps/yonder/strip.c: the pins, the timed strip and its deadline,
buffering's hold, the mark and its delay, the next time, and what is drawn),
against tools/test_strip_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither. A mutant that loops forever is caught by the
timeout.

Each mutant is applied to the tree's copy of the file, the suite is run, and
the file is put back from the bytes read before the change, checked byte for
byte, whatever happens. Run it from a clean tree, and never beside another
harness that edits the same tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/strip-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

STRIP = 'userland/apps/yonder/strip.c'
CASES = 'bash tools/test_strip_host.sh 300'

mutants = [
    # The pins.
    ('paused-does-not-pin', 'return s->paused || s->over_strip', 'return s->over_strip'),
    ('the-strip-does-not-pin', 's->paused || s->over_strip || s->focused', 's->paused || s->focused'),
    ('focus-does-not-pin', 's->over_strip || s->focused || s->dragging', 's->over_strip || s->dragging'),
    ('a-drag-does-not-pin', '|| s->focused || s->dragging;', '|| s->focused;'),
    # The move.
    ('a-move-keeps-the-old-deadline', '    s->deadline = now + YONDER_STRIP_IDLE_MS;\n', '    if (s->deadline == 0) s->deadline = now + YONDER_STRIP_IDLE_MS;\n'),
    ('a-move-does-not-show-it', '    if (s->state == YONDER_STRIP_HIDDEN)\n        s->state = YONDER_STRIP_TIMED;', ''),
    # (A move that set PINNED would change nothing: every move meets a settle
    # before anything reads the state, and the settle decides PINNED itself.)
    # Timed.
    ('timed-ignores-the-deadline', 'if (!s->over_box || (!s->buffering && now >= s->deadline))', 'if (!s->over_box)'),
    ('timed-ignores-the-box', 'if (!s->over_box || (!s->buffering && now >= s->deadline))', 'if (!s->buffering && now >= s->deadline)'),
    ('the-deadline-is-exclusive', 'now >= s->deadline', 'now > s->deadline'),
    ('buffering-does-not-hold', '(!s->buffering && now >= s->deadline)', '(now >= s->deadline)'),
    ('buffering-holds-the-box', 'if (!s->over_box || (!s->buffering && now >= s->deadline))', 'if (!s->buffering && (!s->over_box || now >= s->deadline))'),
    ('hidden-comes-back-by-itself', '    } else if (s->state != YONDER_STRIP_HIDDEN) {', '    } else {'),
    # The mark.
    ('the-mark-outlives-buffering', '    if (!s->buffering) {\n        s->mark = YONDER_MARK_NONE;\n    } else if', '    if (!s->buffering) {\n    } else if'),
    ('the-mark-waits-no-time', 's->mark_due = now + YONDER_STRIP_MARK_MS;', 's->mark_due = now;'),
    ('the-mark-restarts-its-wait', '    } else if (s->mark == YONDER_MARK_NONE) {', '    } else if (s->mark != YONDER_MARK_SHOWN) {'),
    ('the-mark-due-is-exclusive', 'now >= s->mark_due', 'now > s->mark_due'),
    # Settle's answer.
    ('settle-says-nothing-of-the-strip', 'return was_drawn != yonder_strip_drawn(s) || ', '(void)was_drawn;\n    return '),
    ('settle-says-nothing-of-the-mark', ' || (was_mark == YONDER_MARK_SHOWN) != (s->mark == YONDER_MARK_SHOWN);', ';\n    (void)was_mark;'),
    # Next.
    ('next-arms-while-buffering', 'if (s->state == YONDER_STRIP_TIMED && !s->buffering && s->over_box)', 'if (s->state == YONDER_STRIP_TIMED && s->over_box)'),
    # (Dropping `over_box` from next's test changes nothing: a settle leaves
    # no TIMED strip with the pointer outside and the player not buffering.)
    ('next-forgets-the-mark', '    if (s->mark == YONDER_MARK_PENDING && s->mark_due < next)\n        next = s->mark_due;\n', ''),
    ('next-arms-a-shown-mark', 'if (s->mark == YONDER_MARK_PENDING && s->mark_due < next)', 'if (s->mark != YONDER_MARK_NONE && s->mark_due < next)'),
    # Drawn.
    ('timed-is-not-drawn', 'return s->state != YONDER_STRIP_HIDDEN;', 'return s->state == YONDER_STRIP_PINNED;'),
]

path = root / STRIP
caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {STRIP}', file=sys.stderr)
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
            print(f'{name}: {STRIP} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
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

print(f'\nstrip mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
