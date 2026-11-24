#!/usr/bin/env python3
"""One deliberate break at a time to the wrapper lifetime's object graph
(userland/libdom: d_node_mark and the VIEWS, the HOLDERS, the safe point),
against tools/test_yonder_scripts_host.sh run with DOM_GC_STRESS=1, which
collects at every safe point with the engine's assertions on: a mutant is
CAUGHT when the suite fails, MISSED when it passes, which means a test is
missing and gets written. A build failure is neither.

What these can break while the registry still owns every node wrapper (W0)
is a count reported that the object does not own, or a holder's count let
go of twice: each makes a collection or the teardown take a count that is
not there. A count owned and NOT reported cannot show yet, as nothing a
registry roots can die; that is W1's reachability oracle's to catch.

The shape is test_libflow_filter_mark_mutants.py's: each mutant is applied to
the tree's copy of its file, the suite is run, and the file is put back from
the bytes read before the change, checked byte for byte, whatever happens.
Run it from a clean tree, and never beside another harness that edits the
same tree; the per-mutant logs are in --logs.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/dom-wrapper-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CASES = 'bash tools/test_yonder_scripts_host.sh'
NODE = 'userland/libdom/node.c'
COLLECTION = 'userland/libdom/collection.c'
ATTR = 'userland/libdom/attr.c'
RANGE = 'userland/libdom/range.c'
OBSERVER = 'userland/libdom/observer.c'
EVENT = 'userland/libdom/event.c'

mutants = [
    ('a-listener-reported-twice', NODE,
     '        if (listener->engine) JS_MarkValue(rt, listener->callback, mark);\n',
     '        if (listener->engine) { JS_MarkValue(rt, listener->callback, mark);'
     ' JS_MarkValue(rt, listener->callback, mark); }\n'),
    ('a-registry-listener-reported', NODE,
     '        if (listener->engine) JS_MarkValue(rt, listener->callback, mark);\n',
     '        JS_MarkValue(rt, listener->callback, mark);\n'),
    ('a-kept-view-reported-twice', NODE,
     '        mark_kept(rt, records[i], mark);\n',
     '        { mark_kept(rt, records[i], mark); mark_kept(rt, records[i], mark); }\n'),
    ('a-kept-collection-reported-twice', NODE,
     '    mark_kept_query(rt, entry->child_nodes, mark);\n',
     '    mark_kept_query(rt, entry->child_nodes, mark);\n    mark_kept_query(rt, entry->child_nodes, mark);\n'),
    ('a-view-holder-reported-twice', NODE,
     '    if (view != NULL && view->holder != NULL) JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, view->holder), mark);\n',
     '    if (view != NULL && view->holder != NULL) { JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, view->holder), mark);'
     ' JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, view->holder), mark); }\n'),
    ('a-view-holder-not-taken', NODE,
     '    view->holder = JS_VALUE_GET_PTR(JS_DupValueRT(owner->dom->engine, owner->value));\n',
     '    view->holder = JS_VALUE_GET_PTR(owner->value);\n'),
    ('a-collection-holder-reported-twice', COLLECTION,
     '    if (query != NULL) JS_MarkValue(rt, query->holder, mark);\n',
     '    if (query != NULL) { JS_MarkValue(rt, query->holder, mark); JS_MarkValue(rt, query->holder, mark); }\n'),
    ('an-attr-reported-twice', ATTR,
     '                JS_MarkValue(rt, rec->entry->value, mark);\n',
     '                { JS_MarkValue(rt, rec->entry->value, mark); JS_MarkValue(rt, rec->entry->value, mark); }\n'),
    ('a-range-boundary-reported-twice', RANGE,
     '    for (int i = 0; i < 2; i++) JS_MarkValue(rt, r->wrapper[i], mark);\n',
     '    for (int i = 0; i < 2; i++) JS_MarkValue(rt, r->wrapper[i], mark);\n'
     '    JS_MarkValue(rt, r->wrapper[0], mark);\n'),
    ('a-record-wrapper-let-go-twice', OBSERVER,
     '    for (size_t i = 0; i < m->nwrappers; i++) JS_FreeValueRT(dom->engine, m->wrappers[i]);\n    d_free(dom, m);\n',
     '    for (size_t i = 0; i < m->nwrappers; i++) JS_FreeValueRT(dom->engine, m->wrappers[i]);\n'
     '    if (m->nwrappers > 0) JS_FreeValueRT(dom->engine, m->wrappers[0]);\n    d_free(dom, m);\n'),
    ('the-path-let-go-twice', EVENT,
     '    for (size_t i = 0; i < count; i++) JS_FreeValue(ctx, kept[i]);\n',
     '    for (size_t i = 0; i < count; i++) JS_FreeValue(ctx, kept[i]);\n'
     '    if (count > 0) JS_FreeValue(ctx, kept[0]);\n'),
]

caught, missed, unbuilt = [], [], []
env = dict(os.environ, DOM_GC_STRESS='1')
for name, relative, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / relative
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {relative}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, env=env,
                                        timeout=3000)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {relative} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\ndom wrapper mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
