#!/usr/bin/env python3
"""One deliberate break at a time to the reap list's lifecycle
(kernel/src/reaplist.c: the list of ended threads whose stacks are still to
be released, each node's state, and its owner's count), against
tools/test_reaplist_host.sh: a mutant is CAUGHT when the suite fails, MISSED
when it passes, which means a test is missing and gets written. A build
failure is neither.

Each mutant is applied to the tree's copy of the file, the suite is run,
and the file is put back from the bytes read before the change, checked
byte for byte, whatever happens. Run it through tools/mutants_parallel.py;
the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/reaplist-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CORE = 'kernel/src/reaplist.c'
CASES = 'bash tools/test_reaplist_host.sh 3000'

mutants = [
    # A hand that takes a node already handed, or released.
    ('hand-twice', '\tif (n->state != REAP_NONE)\n\t{\n\t\tl->refused++;\n\t\treturn false;\n\t}\n\tn->state = REAP_PENDING;',
     '\tn->state = REAP_PENDING;'),
    ('hand-uncounted-owner', '\t__atomic_add_fetch(&owner->pending, 1, __ATOMIC_RELAXED);\n', ''),
    ('hand-no-owner', '\tn->owner = owner;\n', ''),
    ('hand-at-head', '\tif (l->tail != NULL)\n\t\tl->tail->next = n;\n\telse\n\t\tl->head = n;\n\tl->tail = n;',
     '\tn->next = l->head;\n\tl->head = n;\n\tif (l->tail == NULL)\n\t\tl->tail = n;'),
    ('hand-stale-link', '\tn->owner = owner;\n\tn->next = NULL;', '\tn->owner = owner;'),
    ('hand-queued', '\tl->tail = n;\n\tl->queued++;', '\tl->tail = n;'),
    ('hand-most', '\tif (l->queued > l->queued_most)\n\t\tl->queued_most = l->queued;\n', ''),
    ('hand-window-most', '\tif (l->queued > l->window_queued_most)\n\t\tl->window_queued_most = l->queued;\n', ''),
    ('hand-count', '\tl->handed++;\n', ''),
    ('hand-window-count', '\tl->window_handed++;\n', ''),
    ('hand-refusal-uncounted', '\tif (n->state != REAP_NONE)\n\t{\n\t\tl->refused++;', '\tif (n->state != REAP_NONE)\n\t{'),
    # Taking a node off: the links, the tail, the state, the count.
    ('unlink-keeps-head', '\telse\n\t\tl->head = n->next;\n\tif (l->tail == n)', '\tif (l->tail == n)'),
    ('unlink-keeps-middle', '\tif (prev != NULL)\n\t\tprev->next = n->next;\n\telse', '\tif (prev != NULL)\n\t\t;\n\telse'),
    ('unlink-keeps-tail', '\tif (l->tail == n)\n\t\tl->tail = prev;\n', ''),
    ('unlink-keeps-link', '\tl->tail = prev;\n\tn->next = NULL;', '\tl->tail = prev;'),
    ('unlink-stays-pending', '\tn->state = REAP_RUNNING;\n\tl->queued--;', '\tl->queued--;'),
    ('unlink-queued', '\tn->state = REAP_RUNNING;\n\tl->queued--;', '\tn->state = REAP_RUNNING;'),
    ('claim-next-newest', '\treap_node_t *n = l->head;\n\tif (n == NULL)\n\t\treturn NULL;\n\tunlink_after(l, NULL, n);',
     '\treap_node_t *n = l->head, *p = NULL;\n\tif (n == NULL)\n\t\treturn NULL;\n\twhile (n->next) { p = n; n = n->next; }\n\tunlink_after(l, p, n);'),
    ('claim-next-count', '\tl->claimed_next++;\n', ''),
    # A named claim of a node in the wrong state, or not unlinked.
    ('claim-any-state', '\tif (n->state != REAP_PENDING)\n\t{\n\t\tl->refused++;\n\t\treturn false;\n\t}\n\treap_node_t *prev',
     '\treap_node_t *prev'),
    ('claim-wrong-prev', '\t\t\tunlink_after(l, prev, n);\n\t\t\tl->claimed_named++;', '\t\t\tunlink_after(l, NULL, n);\n\t\t\tl->claimed_named++;'),
    ('claim-named-count', '\t\t\tl->claimed_named++;\n', ''),
    # Finishing: only a claimed node, once, and the owner's count.
    ('finish-any-state', '\tif (n->state != REAP_RUNNING)\n\t{\n\t\tl->refused++;\n\t\treturn false;\n\t}\n\tn->state = REAP_DONE;',
     '\tn->state = REAP_DONE;'),
    ('finish-not-done', '\tn->state = REAP_DONE;\n\t__atomic_sub', '\t__atomic_sub'),
    ('finish-owner-kept', '\t__atomic_sub_fetch(&n->owner->pending, 1, __ATOMIC_RELAXED);\n', ''),
    ('finish-count', '\tl->finished++;\n', ''),
    ('finish-window-count', '\tl->window_finished++;\n', ''),
    # The read and its window.
    ('read-no-window-most', '\tl->window_queued_most = l->queued;\n\tl->window_handed = 0;', '\tl->window_handed = 0;'),
    ('read-window-most-zero', '\tl->window_queued_most = l->queued;\n\tl->window_handed = 0;', '\tl->window_queued_most = 0;\n\tl->window_handed = 0;'),
    ('read-no-window-handed', '\tl->window_handed = 0;\n\tl->window_finished = 0;', '\tl->window_finished = 0;'),
    ('read-no-window-finished', '\tl->window_handed = 0;\n\tl->window_finished = 0;', '\tl->window_handed = 0;'),
    ('read-refused', '\tout->refused = l->refused;\n', '\tout->refused = 0;\n'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / CORE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {CORE}', file=sys.stderr)
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
            print(f'{name}: {CORE} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nreap list mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
