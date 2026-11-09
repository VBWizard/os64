#!/usr/bin/env python3
"""One deliberate break at a time to the paint's scratch (docs/design/pending/
YONDER_PERF.md § perf-5): its slots, the floor, the spare eighth, the
smallest that fits, the keep, the paints a buffer may sit idle, another
page's drop, and every site in the painter that borrows from it, against
the cases that state its rules: yonder's painter harness, whose
scratch_cases walk the design's table a row at a time, whose oracle paints
every page through one scratch the pages before it left dirty, and whose
scratch_paint_cases require a page painted again to allocate no buffer of
the scratch's size; and the scripted-page harness, which shows another
page. A mutant is CAUGHT when they fail, MISSED when they pass, which
means a test is missing and gets written. A build failure is neither.

The shape is test_paint_cache_mutants.py's: each mutant is applied to the
worktree's copy of its file, the cases are run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.

Left out because no host case paints the window's view: the glass's
borrowing (the opacity group's copy of what is under it, the transform and
text-clip sheets), the paint's end in view_paint, and the window's scratch
made at its first paint and freed when it closes. Those are read off a
QEMU run, where the heap's own count of regions taken is the witness.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-scratch-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
args.logs.mkdir(parents=True, exist_ok=True)

root = pathlib.Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], text=True).strip())
SCRATCH = 'userland/apps/yonder/scratch.c'
PAINT = 'userland/apps/yonder/paint.c'
YONDER = 'userland/apps/yonder/yonder.c'
CASES = {
    SCRATCH: 'bash tools/test_yonder_host.sh',
    PAINT: 'bash tools/test_yonder_host.sh',
    YONDER: 'bash tools/test_yonder_scripts_host.sh',
}

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    # The scratch: which requests it pools, how big a buffer it makes, which
    # it lends, and what it keeps.
    ('a-small-request-is-pooled', SCRATCH,
     '    if (s == NULL || bytes < YONDER_SCRATCH_FLOOR || cap == 0 || cap > YONDER_SCRATCH_KEEP) {',
     '    if (s == NULL || cap == 0 || cap > YONDER_SCRATCH_KEEP) {'),
    ('a-request-past-the-keep-is-pooled', SCRATCH,
     '    if (s == NULL || bytes < YONDER_SCRATCH_FLOOR || cap == 0 || cap > YONDER_SCRATCH_KEEP) {',
     '    if (s == NULL || bytes < YONDER_SCRATCH_FLOOR || cap == 0) {'),
    ('no-eighth-to-spare', SCRATCH,
     '(bytes + bytes / 8 + PAGE - 1) / PAGE * PAGE',
     '(bytes + PAGE - 1) / PAGE * PAGE'),
    ('a-buffer-too-small-is-lent', SCRATCH,
     '            if (sl->cap >= bytes) {',
     '            if (true) {'),
    ('the-biggest-that-fits-is-lent', SCRATCH,
     '                if (fit == NULL || sl->cap < fit->cap)',
     '                if (fit == NULL || sl->cap > fit->cap)'),
    ('the-biggest-idle-buffer-is-remade', SCRATCH,
     '            } else if (small == NULL || sl->cap < small->cap) {',
     '            } else if (small == NULL || sl->cap > small->cap) {'),
    ('a-lent-buffer-is-not-marked', SCRATCH,
     '    sl->lent = true;\n    s->counts.lent++;',
     '    s->counts.lent++;'),
    ('a-reused-buffer-stays-counted-idle', SCRATCH,
     '        s->counts.idle--;\n        s->counts.idle_bytes -= fit->cap;\n        s->counts.reused++;',
     '        s->counts.reused++;'),
    ('every-slot-lent-gives-nothing', SCRATCH,
     '        return os64_malloc(bytes);   // every slot is lent',
     '        return NULL;   // every slot is lent'),
    ('a-buffer-no-slot-holds-is-kept', SCRATCH,
     '            return;\n        }\n    os64_free(p);\n}',
     '            return;\n        }\n    (void)p;\n}'),
    ('the-keep-is-not-kept', SCRATCH,
     '            if (s->counts.idle_bytes + sl->cap > YONDER_SCRATCH_KEEP) {',
     '            if (false) {'),
    ('a-given-buffer-keeps-its-old-paint', SCRATCH,
     '            sl->used = s->paint;\n',
     ''),
    ('an-idle-buffer-goes-a-paint-early', SCRATCH,
     '        if (sl->at != NULL && !sl->lent && s->paint - sl->used > YONDER_SCRATCH_IDLE_PAINTS)',
     '        if (sl->at != NULL && !sl->lent && s->paint - sl->used >= YONDER_SCRATCH_IDLE_PAINTS)'),
    ('a-paints-end-frees-a-lent-buffer', SCRATCH,
     '        if (sl->at != NULL && !sl->lent && s->paint - sl->used > YONDER_SCRATCH_IDLE_PAINTS)',
     '        if (sl->at != NULL && s->paint - sl->used > YONDER_SCRATCH_IDLE_PAINTS)'),
    ('another-page-frees-a-lent-buffer', SCRATCH,
     '        if (s->slot[i].at != NULL && !s->slot[i].lent)',
     '        if (s->slot[i].at != NULL)'),
    ('an-emptied-slot-stays-counted', SCRATCH,
     '        s->counts.idle--;\n        s->counts.idle_bytes -= sl->cap;\n    }\n    os64_free(sl->at);',
     '        (void)s;\n    }\n    os64_free(sl->at);'),
    # The painter: every site borrows from the scratch and gives back to it.
    ('a-gradient-is-the-heaps', PAINT,
     '    uint32_t *out = yonder_scratch_take(p->scratch, (size_t)((x1 - x0) * (y1 - y0)) * sizeof(*out));',
     '    uint32_t *out = os64_malloc((size_t)((x1 - x0) * (y1 - y0)) * sizeof(*out));'),
    ('a-gradient-is-not-given-back', PAINT,
     '    yonder_scratch_give(p->scratch, out);\n    if (kept)',
     '    if (kept)'),
    ('a-kept-shadow-is-the-heaps', PAINT,
     '    uint8_t *alpha = yonder_scratch_take(p->scratch, (size_t)((x1 - x0) * (y1 - y0)));\n    if (alpha != NULL) {',
     '    uint8_t *alpha = os64_malloc((size_t)((x1 - x0) * (y1 - y0)));\n    if (alpha != NULL) {'),
    ('a-kept-shadow-is-not-given-back', PAINT,
     '        yonder_scratch_give(p->scratch, alpha);\n    }\n    blocks_free(p, &bk);',
     '    }\n    blocks_free(p, &bk);'),
    ('a-cast-shadow-is-the-heaps', PAINT,
     '    Mask m = {x0, y0, x1 - x0, y1 - y0, NULL};\n    m.a = yonder_scratch_take(p->scratch, (size_t)(m.w * m.h) * sizeof(*m.a));',
     '    Mask m = {x0, y0, x1 - x0, y1 - y0, NULL};\n    m.a = os64_malloc((size_t)(m.w * m.h) * sizeof(*m.a));'),
    ('a-cast-shadow-is-freed-to-the-heap', PAINT,
     '        mask_emit(p, &m, colour);\n    }\n    yonder_scratch_give(p->scratch, m.a);',
     '        mask_emit(p, &m, colour);\n    }\n    os64_free(m.a);'),
    ('an-emitted-mask-is-the-heaps', PAINT,
     '    uint8_t *alpha = yonder_scratch_take(p->scratch, (size_t)((x1 - x0) * (y1 - y0)));\n    if (alpha == NULL)\n        return;',
     '    uint8_t *alpha = os64_malloc((size_t)((x1 - x0) * (y1 - y0)));\n    if (alpha == NULL)\n        return;'),
    ('a-prepared-mask-is-the-heaps', PAINT,
     '    m.a = yonder_scratch_take(p->scratch, (size_t)(m.w * m.h) * sizeof(*m.a));\n    if (m.a == NULL)\n        return false;',
     '    m.a = os64_malloc((size_t)(m.w * m.h) * sizeof(*m.a));\n    if (m.a == NULL)\n        return false;'),
    ('blocks-free-their-mask-to-the-heap', PAINT,
     '    yonder_scratch_give(p->scratch, bk->scratch);',
     '    (void)p;\n    os64_free(bk->scratch);'),
    ('the-painter-forgets-its-scratch', PAINT,
     '                 .cache = cache, .scratch = scratch};',
     '                 .cache = cache, .scratch = ((void)scratch, NULL)};'),
    # yonder's half.
    ('another-page-keeps-the-idle-buffers', YONDER,
     '    yonder_paint_cache_empty(g.paint_cache);\n    yonder_scratch_drop(g.paint_scratch);',
     '    yonder_paint_cache_empty(g.paint_cache);'),
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

print(f'\nyonder scratch mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
