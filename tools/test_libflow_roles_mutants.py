#!/usr/bin/env python3
"""One deliberate break at a time to the rules that let a subtree holding
roles be copied (G1: reuse_match.inc's `inner`, `ordinary` and `escapes`,
reuse_graft.inc's staging; RELAYOUT_GRAFT.md § Roles inside a copy),
against the graft cases (test_libflow_host.sh --graft): a mutant is CAUGHT
when they fail, MISSED when they pass, which means a test is missing and
gets written. A build failure is neither. The shape is
test_libflow_intrinsic_mutants.py's.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/libflow-roles-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

MATCH = 'userland/libflow/reuse_match.inc'
GRAFT = 'userland/libflow/reuse_graft.inc'
CASES = 'bash tools/test_libflow_host.sh --graft'

# Staged with no mutant here, because nothing reads them after a copy until
# a copied container can be refit, which needs an item as a copy's root
# (G2): an item's flex_sized, flex_w, flex_ml and area_dx, and an
# out-of-flow box's placeholder (`place`, which absolute() reads, and which
# no fresh positioned_done reaches for a box whose containing block is in
# the copy).
#
# (name, [(file, old, new), ...]). Each breaks one rule the cases state.
mutants = [
    ('a-float-inside-is-copied', [
        (MATCH, '    p->inner = box->kind == FB_BLOCK && box->floated == FLOW_FLOAT_NONE && !box->homed;',
                '    p->inner = box->kind == FB_BLOCK && !box->homed;')]),
    ('a-block-level-picture-inside-is-copied', [
        (MATCH, '    p->inner = box->kind == FB_BLOCK && box->floated == FLOW_FLOAT_NONE && !box->homed;',
                '    p->inner = box->kind <= FB_REPLACED && box->floated == FLOW_FLOAT_NONE && !box->homed;')]),
    ('an-absolute-box-escaping-is-copied', [
        (MATCH, '    p->ordinary = p->ordinary && p->inner && p->escapes == 0;',
                '    p->ordinary = p->ordinary && p->inner;')]),
    ('a-containing-block-holds-nothing', [
        (MATCH, '    p->escapes -= p->cb_held;\n', '')]),
    ('an-atom-that-asked-the-floats-is-copied', [
        (MATCH, '        p->inner = false;\n        p->atom_floats = true;', '        p->atom_floats = false;')]),
    ('an-items-flex-line-is-not-staged', [
        (GRAFT, '            d->area_dx = s->area_dx; d->flex_line = s->flex_line;',
                '            d->area_dx = s->area_dx;')]),
    ('an-items-grid-row-is-not-staged', [
        (GRAFT, '            d->grid_row = s->grid_row; d->grid_col = s->grid_col;',
                '            d->grid_col = s->grid_col;')]),
    ('an-atoms-content-is-not-staged', [
        (GRAFT, '        if (it->content) graft_box(l, styles, it->content, staged, count, dx, dy, s);',
                '        (void)it;')]),
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

print(f'\nroles mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
