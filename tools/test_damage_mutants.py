#!/usr/bin/env python3
"""One deliberate break at a time to the damage list's rule (shared/damage.c:
the merge test, first-fit, the append, the collapse and its answer, the empty
mark), against tools/test_damage_host.sh: a mutant is CAUGHT when the suite
fails, MISSED when it passes, which means a test is missing and gets written.
A build failure is neither.

The shape is test_str_mutants.py's: each mutant is applied to the tree's
copy of damage.c, the suite is run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it from
a clean tree, and never beside another harness that edits the same tree; the
per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/damage-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

DAMAGE = 'shared/damage.c'
CASES = 'bash tools/test_damage_host.sh'

mutants = [
    ('the-slack-is-ignored', '    return area(join(a, b)) <= area(a) + area(b) + slack;',
     '    return area(join(a, b)) <= area(a) + area(b) + 0 * slack;'),
    ('merging-is-always-cheap', '    return area(join(a, b)) <= area(a) + area(b) + slack;',
     '    return area(join(a, b)) + area(a) + area(b) + slack >= INT64_MIN;'),
    ('the-slack-is-counted-twice', '    return area(join(a, b)) <= area(a) + area(b) + slack;',
     '    return area(join(a, b)) <= area(a) + area(b) + 2 * slack;'),
    ('a-merge-keeps-the-old-entry', '            rects[i] = join(rects[i], r);\n', ''),
    ('a-merge-goes-to-the-last-fit', '            rects[i] = join(rects[i], r);\n            return false;',
     '            rects[i] = join(rects[i], r);\n            if (i + 1 == *count) return false;\n            continue;'),
    ('the-list-fills-one-short', '    if (*count < cap) {', '    if (*count + 1 < cap) {'),
    ('a-collapse-forgets-the-new-mark', '    os64_gui_rect_t all = r;', '    os64_gui_rect_t all = rects[0];'),
    ('a-collapse-keeps-its-count', '    rects[0] = all;\n    *count = 1;', '    rects[0] = all;'),
    ('a-collapse-is-not-said', '    *count = 1;\n    return true;', '    *count = 1;\n    return false;'),
    ('an-empty-mark-is-added', '    if (empty(r))\n        return false;\n\n', ''),
    ('a-negative-width-is-not-empty', '    return r.w <= 0 || r.h <= 0;', '    return r.w == 0 || r.h <= 0;'),
    ('the-union-loses-its-left-edge', '    int32_t x1 = a.x < b.x ? a.x : b.x;', '    int32_t x1 = a.x;'),
    ('the-union-loses-its-bottom', "    int32_t y2 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);",
     '    int32_t y2 = a.y + a.h;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / DAMAGE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {DAMAGE}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=300)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {DAMAGE} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\ndamage mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
