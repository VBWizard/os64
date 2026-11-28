#!/usr/bin/env python3
"""One deliberate break at a time to libmedia's fed sound placement
(userland/libmedia/media_fed_sound.c), against tools/test_media_fed_sound_host.sh:
a mutant is CAUGHT when the suite fails, MISSED when it passes, which means a
test is missing and gets written. A build failure is neither.

The shape is test_media_cores_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
through tools/mutants_parallel.py (it needs no --with: the core is in
userland/); the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/media-fed-sound-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

F = 'userland/libmedia/media_fed_sound.c'
CASES = 'bash tools/test_media_fed_sound_host.sh 2'

mutants = [
    ('samples-truncate', F, '(uint64_t)r * rate + 500000000u)', '(uint64_t)r * rate + 0u)'),
    ('negative-remainder-kept', F, '    if (r < 0) {\n        r += 1000000000;\n        q--;\n    }\n', ''),
    ('no-tolerance', F, '    if (s->tolerance < 1)\n        s->tolerance = 1;', '    s->tolerance = 0;'),
    ('a-tenth-of-the-tolerance', F, 's->tolerance = (int64_t)rate * OS64_MEDIA_FED_SOUND_TOLERANCE_US / 1000000;', 's->tolerance = (int64_t)rate * OS64_MEDIA_FED_SOUND_TOLERANCE_US / 10000000;'),
    ('no-bound', F, '    s->bound = join_ns > 0 ? os64_media_fed_sound_samples(join_ns, rate) : 0;', '    s->bound = INT64_MAX;'),
    ('the-target-ignored', F, '        s->count = os64_media_fed_sound_samples(target_ns, rate);\n        s->placed = true;', '        (void)target_ns;'),
    ('the-first-frame-at-zero', F, '    if (!s->placed) {\n        s->count = place;', '    if (!s->placed) {\n        s->count = 0;'),
    ('a-gap-of-the-bound-restarts', F, '        if (d <= s->bound) {', '        if (d < s->bound) {'),
    ('a-gap-never-filled', F, '            out->silence = d;\n', ''),
    ('a-restart-not-said', F, '            out->restarted = true;\n', ''),
    ('a-restart-keeps-the-count', F, '            out->restarted = true;\n            s->count = place;', '            out->restarted = true;'),
    ('a-gap-plays-on-by-count', F, '            out->silence = d;\n            s->count = place;', '            out->silence = d;'),
    ('an-overlap-never-cut', F, '    } else if (d < -s->tolerance) {', '    } else if (0 && d < -s->tolerance) {'),
    ('a-cut-past-the-frame', F, '        out->skip = over >= samples ? samples : (uint32_t)over;', '        out->skip = (uint32_t)over;'),
    ('the-kept-not-reduced', F, '        out->keep = samples - out->skip;\n', ''),
    ('the-count-moves-the-whole-frame', F, '    s->count += out->keep;', '    s->count += samples;'),
    ('the-tolerance-one-sided', F, '    if (d > s->tolerance) {', '    if (d > 0) {'),
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

print(f'\nmedia fed sound mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
