#!/usr/bin/env python3
"""One deliberate break at a time to the mapping list's core
(kernel/src/memory/maplist.c: the list, each mapping's holds, and when a
mapping may be freed), against tools/test_maplist_host.sh: a mutant is
CAUGHT when the suite fails, MISSED when it passes, which means a test is
missing and gets written. A build failure is neither.

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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/maplist-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CORE = 'kernel/src/memory/maplist.c'
CASES = 'bash tools/test_maplist_host.sh 3000'

mutants = [
    # A free with a hold outstanding: unlink says to free a held node.
    ('unlink-frees-held', '    return n->holds == 0;\n}', '    return true;\n}'),
    # A lookup with no hold.
    ('find-hold-no-hold', '    if (n != NULL)\n        n->holds++;\n    return n;', '    return n;'),
    ('hold-no-hold', 'void maplist_hold(maplist_node_t *n)\n{\n    n->holds++;',
     'void maplist_hold(maplist_node_t *n)\n{\n    (void)n;'),
    # A delist that leaves the entry findable.
    ('unlink-keeps-links',
     '    if (n->prev != NULL)\n        n->prev->next = n->next;\n    else\n        l->head = n->next;\n'
     '    if (n->next != NULL)\n        n->next->prev = n->prev;\n', ''),
    ('unlink-keeps-head', '    else\n        l->head = n->next;\n', '\n'),
    ('unlink-keeps-back-link', '    if (n->next != NULL)\n        n->next->prev = n->prev;\n', ''),
    ('unlink-stays-listed', '    n->listed = false;\n    l->count--;', '    l->count--;'),
    ('unlink-twice', '    if (!n->listed)\n        return false;\n    if (n->prev', '    if (n->prev'),
    ('unlink-count', '    n->listed = false;\n    l->count--;', '    n->listed = false;'),
    # A release that frees twice, or frees a listed node.
    ('release-without-hold', '    if (n->holds == 0)\n        return false;\n    n->holds--;', '    n->holds--;'),
    ('release-frees-listed', '    return n->holds == 0 && !n->listed;', '    return n->holds == 0;'),
    ('release-frees-early', '    return n->holds == 0 && !n->listed;', '    return !n->listed;'),
    # A burial that misses one, or leaves the list.
    ('bury-misses-head', '    maplist_node_t *chain = l->head;\n',
     '    maplist_node_t *chain = l->head ? l->head->next : NULL;\n'),
    ('bury-leaves-listed', '        n->listed = false;\n        n->prev = NULL;', '        n->prev = NULL;'),
    ('bury-leaves-list', '    l->head = NULL;\n    l->count = 0;\n    return chain;', '    l->count = 0;\n    return chain;'),
    ('bury-holds-uncounted', '        *held += n->holds;\n', ''),
    # The head: a fresh node first.
    ('link-at-tail',
     '    n->prev = NULL;\n    n->next = l->head;\n    if (l->head != NULL)\n        l->head->prev = n;\n    l->head = n;',
     '    n->next = NULL;\n    n->prev = NULL;\n    if (l->head == NULL)\n        l->head = n;\n    else {\n'
     '        maplist_node_t *t = l->head;\n        while (t->next)\n            t = t->next;\n'
     '        t->next = n;\n        n->prev = t;\n    }'),
    ('link-no-back-link', '    if (l->head != NULL)\n        l->head->prev = n;\n', ''),
    ('link-keeps-holds', '    n->holds = 0;\n    n->listed = true;', '    n->listed = true;'),
    ('linked-count', '    l->head = n;\n    l->count++;', '    l->head = n;'),
    # The walk: [start, end), and its counts.
    ('range-end-inclusive', '(addr >= n->start && addr < n->end)', '(addr >= n->start && addr <= n->end)'),
    ('range-start-exclusive', '(addr >= n->start && addr < n->end)', '(addr > n->start && addr < n->end)'),
    ('find-start-in-range', 'start_only ? n->start == addr :', 'start_only ? (addr >= n->start && addr < n->end) :'),
    ('walk-lookups', '    l->lookups++;\n', ''),
    ('walk-steps', '    l->steps += steps;\n', ''),
    ('walk-longest', '    if (steps > l->steps_most)\n        l->steps_most = steps;\n', '    l->steps_most = steps;\n'),
    ('window-longest', '    if (steps > l->window_steps_most)\n        l->window_steps_most = steps;\n', ''),
    ('read-no-restart-lookups', '    l->read_lookups = l->lookups;\n', ''),
    ('read-no-restart-steps', '    l->read_steps = l->steps;\n', ''),
    ('read-no-restart-longest', '    l->window_steps_most = 0;\n}', '}'),
    ('walk-no-break', '            found = n;\n            break;', '            found = n;'),
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

print(f'\nmaplist mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
