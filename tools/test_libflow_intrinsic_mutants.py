#!/usr/bin/env python3
"""One deliberate break at a time to the rules a copy of an equal subtree's
intrinsic widths keeps (layout.c intrinsic_copy, RELAYOUT_GRAFT.md
§ Intrinsic widths have a separate memo), against the graft cases and the
intrinsic random walk (test_libflow_host.sh --graft): a mutant is CAUGHT
when they fail, MISSED when they pass, which means a test is missing and
gets written. A build failure is neither.

The shape is test_libflow_origin_mutants.py's, except that a mutant may make
several edits, because a key member is written where a measure is recorded
and read where a copy is checked, and breaking it takes both. Each edit is
applied to the worktree's copy of its file, the cases are run, and the file
is put back from the bytes read before the change, checked byte for byte,
whatever happens. Run it from a clean tree; the per-mutant logs are in
--logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/libflow-intrinsic-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

LAYOUT = 'userland/libflow/layout.c'
CASES = 'bash tools/test_libflow_host.sh --graft'

RECORD = '    b->intrinsic_def = definite_height(l, b);'
SAME = '    return was->intrinsic_def == definite_height(l, now) && was->intrinsic_cbh == intrinsic_cbh(now);'
CHECK = '    } else if (!intrinsic_heights_same(l, d, od)) {'

# Rules with no mutant here, because no layout today can tell them broken:
# the order rules, a box the source measured before the root, a box measured
# here already, and the window that bounds the copy to the boxes measured
# with the root. Each guards an asking order the layout does not make now
# (a box is first asked for its widths before its own layout, and only from
# above), and each is kept so the copy does not lean on that order.
#
# (name, [(file, old, new), ...]). Each breaks one rule the cases state.
mutants = [
    # The key: the definite height each box measured with the root reads.
    ('a-copy-ignores-the-heights', [
        (LAYOUT, CHECK, '    } else if (false && !intrinsic_heights_same(l, d, od)) {')]),
    ('the-key-forgets-an-anonymous-box-reads-the-named-one', [
        (LAYOUT, RECORD, '    b->intrinsic_def = b->def_known ? own_definite(b) : -1;'),
        (LAYOUT, SAME, '    (void)l;\n    return was->intrinsic_def == (now->def_known ? own_definite(now) : -1) && '
                       'was->intrinsic_cbh == intrinsic_cbh(now);')]),
    # The named box the walk reaches, keyed on its def_h alone.
    ('the-key-forgets-the-ratio-height', [
        (LAYOUT, RECORD, '    b->intrinsic_def = ({ const FBox *n = b; while (n && !n->node) n = n->parent; '
                         'n ? (n->def_known ? n->def_h : -1) : l->icb_h; });'),
        (LAYOUT, SAME, '    return was->intrinsic_def == ({ const FBox *n = now; while (n && !n->node) n = n->parent; '
                       'n ? (n->def_known ? n->def_h : -1) : l->icb_h; }) && '
                       'was->intrinsic_cbh == intrinsic_cbh(now);')]),
    ('the-key-forgets-a-pictures-containing-block', [
        (LAYOUT, '    return b->kind == FB_REPLACED ? box_cb_h(b) : -1;', '    (void)b;\n    return -1;')]),
    # The boxes the root's measure measured with it are copied with it.
    ('the-boxes-measured-with-the-root-are-not-copied', [
        (LAYOUT, '    for (FBox *c = d->first; c != NULL; c = c->next)\n        if (!intrinsic_walk(',
                 '    for (FBox *c = copy ? NULL : d->first; c != NULL; c = c->next)\n        if (!intrinsic_walk(')]),
    ('an-atoms-content-is-not-walked', [
        (LAYOUT, '        if (it->content != NULL && !intrinsic_walk(l, it->content, root, base, copy, why))',
                 '        if (false && it->content != NULL && !intrinsic_walk(l, it->content, root, base, copy, why))')]),
    ('a-measure-ends-at-itself', [
        (LAYOUT, '    b->intrinsic_end = l->measure_seq;', '    b->intrinsic_end = b->intrinsic_seq;')]),
    # The copy's numbers come after this layout's (any numbering that keeps
    # the source's order within the copy keeps its windows, so only the
    # count is a rule).
    ('a-copy-leaves-the-count-behind', [
        (LAYOUT, '    l->measure_seq = base + (old->intrinsic_end - old->intrinsic_seq);',
                 '    (void)base;')]),
    # The subtree and the cut.
    ('an-unequal-subtree-is-copied', [
        (LAYOUT, '    const FBox *old = pair && pair->equal ? pair->old : NULL;',
                 '    const FBox *old = pair ? pair->old : NULL;')]),
    ('a-table-is-copied', [
        (LAYOUT, '    } else if (d->kind != FB_BLOCK && d->kind != FB_REPLACED) {',
                 '    } else if (false) {')]),
]

caught, missed, unbuilt = [], [], []
for name, edits in mutants:
    if args.only and args.only not in name:
        continue
    originals = {}
    texts = {}
    ok = True
    for rel, old, new in edits:
        path = root / rel
        if rel not in originals:
            originals[rel] = path.read_bytes()
            texts[rel] = originals[rel].decode()
        if texts[rel].count(old) != 1:
            print(f'{name}: the text to break is not found once in {rel}', file=sys.stderr)
            ok = False
            break
        texts[rel] = texts[rel].replace(old, new)
    if not ok:
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        for rel, text in texts.items():
            (root / rel).write_text(text)
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out,
                                        timeout=1800)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        for rel, original in originals.items():
            path = root / rel
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

print(f'\nintrinsic mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
