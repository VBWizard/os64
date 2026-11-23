#!/usr/bin/env python3
"""One deliberate break at a time to a filtered group's mark
(userland/libflow/flow.c § A CHANGE INSIDE A FILTERED GROUP: what a box
paints, spread by each filtered group's filter out and cut to its bounds,
taken through each matrix), against tools/test_libflow_host.sh, whose
filter_mark_cases hold each rule to exact rects: a mutant is CAUGHT when the
suite fails, MISSED when it passes, which means a test is missing and gets
written. A build failure is neither.

The shape is test_yonder_scale_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
from a clean tree, and never beside another harness that edits the same
tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/libflow-filter-mark-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

FLOW = 'userland/libflow/flow.c'
CASES = 'bash tools/test_libflow_host.sh'

mutants = [
    ('the-reach-not-grown', 'r = meet(filter_local(a) ? filter_spread(a, r) : whole, whole);',
     'r = meet(filter_local(a) ? r : whole, whole);'),
    ('grown-by-the-inner-filter-only',
     '    for (const flow_box_t *a = b; a != NULL && r.w > 0 && r.h > 0; a = a->parent) {\n'
     '        if ((a != b || spread) && filtered_group(a)) {',
     '    int spreads = 0;\n'
     '    for (const flow_box_t *a = b; a != NULL && r.w > 0 && r.h > 0; a = a->parent) {\n'
     '        if ((a != b || spread) && filtered_group(a) && spreads++ == 0) {'),
    ('what-a-blur-reads-left-out',
     '        if (a->style->filter[i].fn == FLOW_FILTER_BLUR || a->style->filter[i].fn == FLOW_FILTER_DROP_SHADOW)\n'
     '            more += FLOW_FILTER_READS_MORE;',
     '        if (a->style->filter[i].fn == FLOW_FILTER_BLUR || a->style->filter[i].fn == FLOW_FILTER_DROP_SHADOW)\n'
     '            more += 0;'),
    ('not-cut-to-the-group', 'r = meet(filter_local(a) ? filter_spread(a, r) : whole, whole);',
     'r = filter_local(a) ? filter_spread(a, r) : whole;'),
    ('a-matrix-left-out', '        if (!nested_reach(a, r, scroll, &r))\n            return true;\n',
     '        if (r.w <= 0 || r.h <= 0)\n            return true;\n'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / FLOW
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {FLOW}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=600)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {FLOW} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nlibflow filter mark mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
