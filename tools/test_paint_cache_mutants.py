#!/usr/bin/env python3
"""One deliberate break at a time to the paint cache (docs/design/pending/
YONDER_PERF.md § perf-2) — the store's list, table and budget, the keys of
a gradient's tile and a shadow's mask, the blocks, and yonder's half: the
cache emptied when another page is shown, its counts restarted and
written — against the cases that state its rules: yonder's painter
harness, whose oracle paints every page with and without the cache, and
yonder's scripted-page harness. A mutant is CAUGHT when they fail, MISSED
when they pass, which means a test is missing and gets written. A build
failure is neither.

The shape is test_cliptext_mutants.py's: each mutant is applied to the
worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.

Left out because nothing can tell them apart from the code: the blur and
the kind in a key (a mask's size and its shapes' places already say the
blur's reach, which is all of the blur a mask reads; a line has a
direction and a ray has none), and a released entry's stamp check in the
first paint (stamp 0 is the first paint's "one before"). The window making
its cache at its first paint is read off a QEMU record's paint line: no
host case paints the window's view.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/paint-cache-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

STORE = 'userland/apps/yonder/paint_cache.c'
PAINT = 'userland/apps/yonder/paint.c'
YONDER = 'userland/apps/yonder/yonder.c'
CASES = {
    STORE: 'bash tools/test_yonder_host.sh && bash tools/test_yonder_scripts_host.sh',
    PAINT: 'bash tools/test_yonder_host.sh',
    YONDER: 'bash tools/test_yonder_scripts_host.sh',
}

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The store: the last two paints' entries, the list, the table, the
    # budget, the room asked before making, the released.
    ('this-paint-is-not-kept', STORE,
     '    return e->stamp != 0 && e->stamp + 1 >= c->stamp;',
     '    return ((void)c, (void)e, false);'),
    ('the-last-paint-is-not-kept', STORE,
     '    return e->stamp != 0 && e->stamp + 1 >= c->stamp;',
     '    return e->stamp != 0 && e->stamp >= c->stamp;'),
    ('a-find-is-not-this-paints', STORE,
     '        e->stamp = c->stamp;\n        unlink_list(c, e);',
     '        unlink_list(c, e);'),
    ('a-find-is-not-the-newest', STORE,
     '        unlink_list(c, e);\n        push_newest(c, e);\n        c->counts.hits++;',
     '        c->counts.hits++;'),
    ('room-counts-what-it-may-not-let-go', STORE,
     '    for (const Entry *e = c->oldest; free < cost && e != NULL && !kept_now(c, e); e = e->newer)',
     '    for (const Entry *e = c->oldest; free < cost && e != NULL && !(kept_now(c, e) && false); e = e->newer)'),
    ('the-budget-is-not-kept', STORE,
     '    while (c->held + size > c->budget) {',
     '    while (false) {'),
    ('a-released-entry-keeps-its-paint', STORE,
     '    e->stamp = 0;\n    unlink_list(c, e);\n    push_oldest(c, e);',
     '    unlink_list(c, e);\n    push_oldest(c, e);'),
    ('a-grown-table-forgets', STORE,
     '            e->chain = b[e->hash & (n - 1)];\n            b[e->hash & (n - 1)] = e;\n',
     ''),
    ('emptied-it-keeps-its-entries', STORE,
     '    while (c->oldest != NULL)\n        let_go(c, c->oldest);\n    os64_free(c->buckets);',
     '    os64_free(c->buckets);'),
    ('the-counts-never-restart', STORE,
     '        os64_memset(&c->counts, 0, sizeof(c->counts));',
     '        (void)c;'),
    ('a-paint-is-not-begun', PAINT,
     '    if (cache != NULL)\n        paint_cache_begin(cache);',
     ''),
    # Kept whole or not at all.
    ('room-is-not-asked', PAINT,
     '    if (!paint_cache_room(p->cache, cost)) {',
     '    if (false) {'),
    ('a-refused-picture-holds-what-it-found', PAINT,
     '        if (bk->blocks[k] != NULL)\n            paint_cache_release(p->cache, bk->blocks[k]);',
     '        (void)p, (void)bk->blocks[k];'),
    ('room-refusals-are-not-counted', PAINT,
     '        blocks_release(p, bk);\n        paint_cache_refused(p->cache, false);',
     '        blocks_release(p, bk);'),
    ('memory-refusals-are-not-counted', PAINT,
     '                blocks_release(p, bk);\n                paint_cache_refused(p->cache, true);',
     '                blocks_release(p, bk);'),
    # A gradient's key.
    ('the-tile-forgets-a-stop-colour', PAINT,
     '        key_put(&bk->key, &line->stops.colour[i], sizeof(line->stops.colour[i]));',
     ''),
    ('the-tile-forgets-a-stop-place', PAINT,
     '        key_put(&bk->key, &line->stops.at[i], sizeof(line->stops.at[i]));',
     ''),
    ('the-tile-forgets-a-hint', PAINT,
     '        key_put(&bk->key, &line->stops.hint[i], 1);',
     ''),
    ('the-tile-forgets-the-line', PAINT,
     '    key_put(&bk->key, geometry, sizeof(geometry));',
     '    (void)geometry;'),
    ('the-tile-forgets-the-repeat', PAINT,
     '    key_put(&bk->key, &line->repeating, 1);',
     ''),
    ('the-tile-forgets-the-dark', PAINT,
     '    key_put(&bk->key, &dark, 1);',
     '    (void)dark;'),
    ('the-tile-forgets-its-width', PAINT,
     '    key_i64(&bk->key, bk->pw);\n    key_i64(&bk->key, bk->ph);\n    const double geometry',
     '    key_i64(&bk->key, bk->ph);\n    const double geometry'),
    ('the-tile-forgets-its-height', PAINT,
     '    key_i64(&bk->key, bk->pw);\n    key_i64(&bk->key, bk->ph);\n    const double geometry',
     '    key_i64(&bk->key, bk->pw);\n    const double geometry'),
    ('a-key-forgets-its-column', PAINT,
     '    key_i64(&bk->key, bk->bx[i]);\n',
     '    (void)i;\n'),
    ('a-key-forgets-its-row', PAINT,
     '    key_i64(&bk->key, bk->by[j]);\n',
     '    (void)j;\n'),
    # A shadow's key.
    ('the-mask-forgets-inset', PAINT,
     '    key_put(&bk.key, &sm.inset, 1);',
     ''),
    ('the-mask-forgets-its-shape', PAINT,
     '    key_round(&bk.key, &sm.shape);',
     ''),
    ('the-mask-forgets-what-cuts-it', PAINT,
     '    key_round(&bk.key, &sm.times);',
     ''),
    # The blocks.
    ('a-mask-block-is-cut-at-its-edges', PAINT,
     '    int64_t sc0 = max64(0, c0 - sm->reach), sc1 = min64(bk->pw, c1 + sm->reach);',
     '    int64_t sc0 = c0, sc1 = c1;'),
    ('a-mask-block-is-cut-at-its-rows', PAINT,
     '    int64_t sr0 = max64(0, r0 - sm->reach), sr1 = min64(bk->ph, r1 + sm->reach);',
     '    int64_t sr0 = r0, sr1 = r1;'),
    ('a-made-block-is-not-filled', PAINT,
     '            fill(p, bk, bk->bx[i], bk->by[j], w, h, *at);',
     '            (void)fill;'),
    ('a-mask-block-is-cut-from-the-wrong-place', PAINT,
     '        os64_memcpy(out + r * w, from + (by * BLOCK - bk->sy + r) * bk->sw + (bx * BLOCK - bk->sx),',
     '        os64_memcpy(out + r * w, from + (by * BLOCK + r) * bk->sw + (bx * BLOCK),'),
    ('a-failed-prepare-is-not-counted', PAINT,
     '    if (prepare != NULL && !prepare(p, bk, c0, r0, c1, r1)) {\n        blocks_release(p, bk);\n        paint_cache_refused(p->cache, true);',
     '    if (prepare != NULL && !prepare(p, bk, c0, r0, c1, r1)) {\n        blocks_release(p, bk);'),
    ('a-tile-pixel-is-asked-at-its-corner', PAINT,
     '            out[r * w + c] = line_at(p, line, bx * BLOCK + c + 0.5, by * BLOCK + r + 0.5);',
     '            out[r * w + c] = line_at(p, line, bx * BLOCK + c, by * BLOCK + r);'),
    ('a-tile-block-forgets-where-it-is', PAINT,
     '            out[r * w + c] = line_at(p, line, bx * BLOCK + c + 0.5, by * BLOCK + r + 0.5);',
     '            out[r * w + c] = line_at(p, line, ((void)bx, c + 0.5), ((void)by, r + 0.5));'),
    ('a-place-is-not-wrapped', PAINT,
     '            pos = floor_mod(pos, size);',
     '            pos = ((void)floor_mod, pos % size);'),
    ('an-edge-block-is-full-width', PAINT,
     '        bk->bw[i] = (int32_t)min64(BLOCK, bk->pw - bk->bx[i] * BLOCK);',
     '        bk->bw[i] = BLOCK;'),
    ('one-colour-is-kept', PAINT,
     '    bool kept = p->cache != NULL && !solid && tile_kept(p, &line, tile, x0, x1, y0, y1, &bk);',
     '    bool kept = p->cache != NULL && tile_kept(p, &line, tile, x0, x1, y0, y1, &bk);'),
    # yonder's half.
    ('another-page-keeps-the-last-ones', YONDER,
     '    // What the last page\'s paints kept is not this one\'s.\n    yonder_paint_cache_empty(g.paint_cache);\n',
     ''),
    ('the-cache-counts-run-on', YONDER,
     '    yonder_paint_cache_counts_restart(g.paint_cache);\n}',
     '}'),
    ('the-record-says-nothing-of-the-cache', YONDER,
     '    if (g.paint_cache != NULL)\n        dt_put(t, "; the paint cache:',
     '    if (false)\n        dt_put(t, "; the paint cache:'),
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
                result = subprocess.run(['bash', '-c', CASES[rel]], cwd=root, stdout=out, stderr=out,
                                        timeout=1800)
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

print(f'\npaint cache mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
