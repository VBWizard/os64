#!/usr/bin/env python3
"""One deliberate break at a time to the restyle kept in the tree
(userland/libflow/restyle.inc, flow_restyle_in_place, and the snapshot's
`structural` mark in reuse_snapshot.inc), against the scripts host's profile
cases (tools/test_yonder_scripts_host.sh with YONDER_PROFILE_ONLY, which run
restyle_kept_cases with every kept tree laid out whole beside it) and
libflow's own (tools/test_libflow_host.sh --restyle, restyle_in_place_cases,
which also hand the check trees that differ): a mutant
is CAUGHT when the cases fail, MISSED when they pass, which means a test is
missing and gets written. A break that changes nothing a rule promises is
not in the list; the comment after it says which and why. A build failure
is neither.

The shape is test_task_order_mutants.py's: each mutant is applied to the
tree's copy of its file, the cases are run, and the file is put back from
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/restyle-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

RESTYLE = 'userland/libflow/restyle.inc'
SNAPSHOT = 'userland/libflow/reuse_snapshot.inc'
CASES = 'bash tools/test_yonder_scripts_host.sh && bash tools/test_libflow_host.sh --restyle'

mutants = [
    # What changed since the tree.
    ('a-text-is-not-structure', SNAPSHOT, '            r->structural = true;', '            (void)0;'),
    ('structure-not-asked', RESTYLE, '    if (r->structural)\n        return out;', '    if (0)\n        return out;'),
    # The verdict.
    ('geometry-kept', RESTYLE, '    if (!keep_derived_equal(was, is) || !f_style_paint_only(&was->style, &is->style)) {',
     '    if (!keep_derived_equal(was, is)) {'),
    ('stacking-kept', RESTYLE, '    if (!keep_stacking_same(&was->style, &is->style)) {',
     '    if (!keep_stacking_same(&was->style, &is->style) && 0) {'),
    ('opacity-across-one-kept', RESTYLE, '    return (a->opacity < 1000) == (b->opacity < 1000) &&\n',
     '    return\n'),
    ('motion-kept', RESTYLE, '    if (was->motion != NULL || is->motion != NULL) {', '    if (0) {'),
    ('decoration-kept', RESTYLE, '    if (!same_decoration) {', '    if (0) {'),
    # The writes.
    ('the-style-not-written', RESTYLE, '        k.pairs[i].was->style = values[i];', '        (void)values;'),
    ('the-frame-not-filled', RESTYLE, '            transform_fill(f, box->style);', '            (void)0;'),
    ('the-ink-not-grown', RESTYLE, '        ink_grown(box);', '        (void)0;'),
    ('not-stamped-drawn', RESTYLE, '        drawn_changed(box, true, stamp);', '        (void)stamp;'),
    ('anonymous-boxes-not-taken-again', RESTYLE,
     '            f_style_anonymous(tree->styles, box->parent->style, box->style->display, (flow_style_t *)box->style);',
     '            (void)0;'),
    ('the-lists-left-in-the-build', RESTYLE, '        room = f_style_copy_lists(&values[i], &tree->restyle);',
     '        room = true;'),
    # Attributes, matrices and reach (the cold read's three).
    ('every-attribute-styled-only', SNAPSHOT, '            (entry->kind == OS64_HTML_CHANGE_ATTR && !reuse_attr_styled_only(entry)))',
     '            (entry->kind == OS64_HTML_CHANGE_ATTR && !reuse_attr_styled_only(entry) && 0))'),
    ('a-matrix-crossing-kept', RESTYLE, '        if (tried.through != f->through || tried.upright != f->upright) {',
     '        if (0) {'),
    ('the-reach-not-worked-again', RESTYLE, '        reach_stacked(tree);\n        sticky_reach_scrolled(tree);\n        // A range',
     '        (void)0;\n        // A range'),
    # The check (--geometry-check).
    ('the-check-ignores-styles', RESTYLE, '                            : x->style != NULL && !f_style_equal(x->style, y->style) ? "a style"',
     '                            : 0 ? "a style"'),
    ('the-check-ignores-border-boxes', RESTYLE, '                            : !same_rect(x->rect, y->rect) ? "a border box"',
     '                            : 0 ? "a border box"'),
]

# Not run, because it changes nothing a rule promises:
# - keep_collect's count of elements: a change of membership with no node
#   added or removed is an element's own display, which its style already
#   declines as geometry before the count matters.
# - `tree->styles->cascade_updates = b->cascade_updates`: without it the next
#   build restyles elements the cascade did not touch, which costs a build's
#   work and changes no answer.
# A break that does not build (a function left unused, which -Werror
# refuses) is written to keep the function called, `&& 0` after it.

env = dict(os.environ, YONDER_PROFILE_ONLY='1')
caught, missed, unbuilt = [], [], []
for name, path_rel, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / path_rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {path_rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, env=env, timeout=1500)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {path_rel} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nrestyle mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
