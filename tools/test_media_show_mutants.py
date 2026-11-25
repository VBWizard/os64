#!/usr/bin/env python3
"""One deliberate break at a time to libmedia's show-thread core
(userland/libmedia/media_show.c: when a publish is owed, stale and ready, a
recreate, closing and its bound, a window gone), against
tools/test_media_show_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_bt_acl_mutants.py's: each mutant is applied to the tree's
copy of the file, the suite is run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it
through tools/mutants_parallel.py; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/media-show-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

COMPOSE = 'userland/libmedia/media_show.c'
CASES = 'bash tools/test_media_show_host.sh 4000'

mutants = [
    ('a-placed-plane-published-another-size', '            if (!same)\n                s->stale = true;\n',
     '            if (!same)\n                act.publish = true;\n'),
    ('a-hidden-plane-always-published', 'bool owed = s->pic_w == 0 || in->wanted || s->stale || !same;', 'bool owed = true;'),
    ('wanted-not-published', 'bool owed = s->pic_w == 0 || in->wanted || s->stale || !same;',
     'bool owed = s->pic_w == 0 || s->stale || !same;'),
    ('a-placed-plane-destroyed-closing', '        if (!in->placed) {\n            act.destroy = true;', '        if (1) {\n            act.destroy = true;'),
    ('an-end-before-the-bound', '} else if (in->now - s->closing_since >= OS64_MEDIA_SHOW_CLOSE_BOUND_US) {', '} else if (1) {'),
    ('no-end-after-the-bound', '} else if (in->now - s->closing_since >= OS64_MEDIA_SHOW_CLOSE_BOUND_US) {', '} else if (0) {'),
    ('the-bound-started-every-pass', '        if (!s->closing) {', '        if (1) {'),
    ('stale-not-raised', '                s->stale = true;', '                (void)0;'),
    ('ready-before-its-publish', '    s->ready = s->pic_w == in->target_w && s->pic_h == in->target_h;\n',
     '    s->ready = true;\n'),
    ('no-recreate-past-capacity', 'act.recreate = in->w > s->cap_w || in->h > s->cap_h;', 'act.recreate = false;'),
    ('a-recreate-only-when-both-sides-outgrow', 'act.recreate = in->w > s->cap_w || in->h > s->cap_h;',
     'act.recreate = in->w > s->cap_w && in->h > s->cap_h;'),
    ('ready-from-the-picks-size', '    s->ready = s->pic_w == in->target_w && s->pic_h == in->target_h;\n',
     '    s->ready = in->have && s->pic_w == in->w && s->pic_h == in->h;\n'),
    ('the-same-pick-published-again', 'if (owed && (fresh || !same)) {', 'if (owed) {'),
    ('the-caller-never-rung', 'act.look = s->stale != stale_was || s->ready != ready_was;',
     'act.look = stale_was && !stale_was && ready_was;'),
    ('a-gone-window-ignored', '    if (in->gone) {', '    if (0) {'),
    ('a-refused-publish-recorded', '    if (!done)\n        return false;\n', '    if (!done && done)\n        return false;\n'),
    ('a-new-plane-keeps-the-old-picture', '    s->pic_w = s->pic_h = 0;\n    s->published = 0;\n', ''),
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

print(f'\nmedia show mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
