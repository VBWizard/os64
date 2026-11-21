#!/usr/bin/env python3
"""One deliberate break at a time to yonder's task order
(userland/apps/yonder/task_order.c: which of a page's ready tasks runs next,
priority among the young and age among the old, and the ages the loop keeps
for the sources that say only whether they have a task), against
tools/test_task_order_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
break that changes nothing a rule promises is not in the list; the comment
after it says which and why. A build failure is neither.

The shape is test_frame_plan_mutants.py's: each mutant is applied to the
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/task-order-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

ORDER = 'userland/apps/yonder/task_order.c'
CASES = 'bash tools/test_task_order_host.sh'

OLD = '    return s->ready && now_ms >= s->since_ms && now_ms - s->since_ms >= TASK_ORDER_BOUND_MS;'

mutants = [
    # Who is old.
    ('old-only-past-the-bound', OLD, OLD.replace('>= TASK_ORDER_BOUND_MS', '> TASK_ORDER_BOUND_MS')),
    ('old-though-not-ready', OLD, OLD.replace('s->ready && ', '')),
    ('old-from-the-future', OLD, OLD.replace('now_ms >= s->since_ms && ', '')),
    ('never-old', OLD, OLD.replace('return s->ready', 'return false && s->ready')),
    # The order of the old.
    ('old-newest-first', '        while (at > 0 && sources[order[at - 1]].since_ms > sources[i].since_ms) {',
     '        while (at > 0 && sources[order[at - 1]].since_ms < sources[i].since_ms) {'),
    ('old-ties-reversed', '        while (at > 0 && sources[order[at - 1]].since_ms > sources[i].since_ms) {',
     '        while (at > 0 && sources[order[at - 1]].since_ms >= sources[i].since_ms) {'),
    ('old-left-out', '        if (!old(&sources[i], now_ms))\n            continue;',
     '        if (1)\n            continue;'),
    # The young.
    ('young-in-reverse', '    for (int i = 0; i < n; i++)\n        if (sources[i].ready && !old(&sources[i], now_ms))',
     '    for (int i = n - 1; i >= 0; i--)\n        if (sources[i].ready && !old(&sources[i], now_ms))'),
    ('young-not-ready-tried', '        if (sources[i].ready && !old(&sources[i], now_ms))',
     '        if (!old(&sources[i], now_ms))'),
    ('old-tried-twice', '        if (sources[i].ready && !old(&sources[i], now_ms))',
     '        if (sources[i].ready)'),
    # The ages.
    ('a-head-ages-from-every-turn', '    else if (age->since_ms == 0)\n        age->since_ms', '    else\n        age->since_ms'),
    ('none-ready-keeps-its-age', '    if (!ready)\n        age->since_ms = 0;\n    else if', '    if (0)\n        age->since_ms = 0;\n    else if'),
    ('found-at-zero-is-none', '        age->since_ms = now_ms != 0 ? now_ms : 1;\n    return', '        age->since_ms = now_ms;\n    return'),
    ('a-run-keeps-the-old-age', '    age->since_ms = ready ? (now_ms != 0 ? now_ms : 1) : 0;', '    (void)age; (void)ready; (void)now_ms;'),
]

# Not run, because it changes nothing a rule promises: task_age_ran's `: 0`
# for a source left empty. The next turn's task_age_observe answers the same
# (no head, no age), and a head that arrives before it is aged from that
# turn either way.

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / ORDER
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {ORDER}', file=sys.stderr)
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
            print(f'{name}: {ORDER} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\ntask_order mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
