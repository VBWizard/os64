#!/usr/bin/env python3
"""One deliberate break at a time to the two rules test_libflow_origin.inc
holds libflow to — a box laid out above its origin keeps its height, and a
line breaks beside an inline-block only where the white-space of the
nearest element holding both allows it — against those cases
(test_libflow_host.sh --origin): a mutant is CAUGHT when they fail, MISSED
when they pass, which means a test is missing and gets written. A build
failure is neither.

The shape is test_libflow_float_mutants.py's: each mutant is applied to the
worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/libflow-origin-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

LAYOUT = 'userland/libflow/layout.c'
CASES = 'bash tools/test_libflow_host.sh --origin'

# (name, file, old, new). Each breaks one rule the cases state.
mutants = [
    # A table's row group: whether it holds a row is not where its rows are.
    ('a-row-group-above-its-origin-is-empty', LAYOUT,
     '                    y0 = any ? min64(y0, row->y) : row->y;\n'
     '                    y1 = any ? max64(y1, row->y + row->h) : row->y + row->h;\n'
     '                    any = true;',
     '                    y0 = any && y0 >= 0 ? min64(y0, row->y) : row->y;\n'
     '                    y1 = any ? max64(y1, row->y + row->h) : row->y + row->h;\n'
     '                    any = row->y >= 0;'),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out,
                                        timeout=900)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {rel} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h|inc):\d+:\d+: error:', body) is None and 'undefined reference' not in body
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

print(f'\norigin mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
