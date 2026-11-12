#!/usr/bin/env python3
"""One deliberate break at a time to libgarb, against its host suite
(tools/test_garb_host.sh): a mutant is CAUGHT when the suite fails, MISSED
when it passes, which means a test is missing and gets written. A build
failure is neither.

The first are the kept parse scratch's (HEAP_BINS.md § Part A): a block
read borrows its owner's scratch and hands it back, and whoever holds a
scratch frees it.

The shape is test_yonder_diag_mutants.py's: each mutant is applied to the
worktree's copy of its file, the suite is run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/garb-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PARSE = 'userland/libgarb/parse.c'
CASCADE = 'userland/libgarb/cascade.c'
GARB = 'tools/test_garb_host.sh'
PROPS = 'userland/libgarb/props.c'
STYLE = 'userland/libflow/style.c'
COMPUTED = 'userland/libflow/computed.c'
PAINT = 'userland/apps/yonder/paint.c'
FLOW = 'tools/test_libflow_host.sh'
FLOW_COMPUTED = 'tools/test_libflow_host.sh --computed'
YON = 'tools/test_yonder_host.sh'

# (name, file, old, new, harness). Each breaks one rule the design states.
mutants = [
    # The kept scratch.
    ('a-read-frees-its-scratch', PARSE,
     '    owner->scratch = p.scratch;\n    owner->scratch_cap = p.scratch_cap;',
     '    os64_free(p.scratch);\n    owner->scratch = NULL;\n    owner->scratch_cap = 0;', GARB),
    ('a-read-keeps-the-grown-scratch', PARSE,
     '    owner->scratch = p.scratch;\n    owner->scratch_cap = p.scratch_cap;',
     '', GARB),
    ('a-read-starts-its-own-scratch', PARSE,
     '               .scratch = owner->scratch, .scratch_cap = owner->scratch_cap};',
     '               .scratch = NULL, .scratch_cap = 0};', GARB),
    ('a-free-keeps-the-scratch', PARSE,
     '    os64_arena_destroy(parsed->arena);\n    os64_free(parsed->scratch);',
     '    os64_arena_destroy(parsed->arena);', GARB),
    ('a-text-parse-keeps-its-scratch', PARSE,
     '    os64_free(p.scratch);\n    return finish(out, &p, &tz, st);',
     '    return finish(out, &p, &tz, st);', GARB),
    ('the-cascade-frees-only-its-arena', CASCADE,
     '    os64_free(c->slots);\n    garb_free(&c->self);',
     '    os64_free(c->slots);\n    os64_arena_destroy(c->self.arena);', GARB),
    # CSS Images: a stop with two positions, and conic gradients.
    ('a-two-position-stop-counts-once', PROPS,
     '        colours += hint ? 0 : positions == 2 ? 2 : 1;',
     '        colours += !hint;', GARB),
    ('a-conic-stop-reads-a-length', PROPS,
     '    return kind == GRADIENT_CONIC ? ap_val(s, c, &v) : length_percentage(s, c);',
     '    (void)v;\n    (void)kind;\n    return length_percentage(s, c);', GARB),
    ('a-conic-has-no-prelude', PROPS,
     '         : kind == GRADIENT_CONIC  ? conic_prelude(s, g)',
     '         : kind == GRADIENT_CONIC  ? (false && conic_prelude(s, g))', GARB),
    ('an-angle-calc-costs-the-declaration', PROPS,
     '                !(angle_calc(t->children, t->nchildren, false) || writes_interpolation(t)))',
     '                !((false && angle_calc(t->children, t->nchildren, false)) || writes_interpolation(t)))', GARB),
    ('any-invalid-conic-is-taken-by-name', PROPS,
     '                !(angle_calc(t->children, t->nchildren, false) || writes_interpolation(t)))',
     '                ((void)angle_calc(t->children, t->nchildren, false), (void)writes_interpolation(t), false))', GARB),
    ('a-conic-is-read-as-radial', STYLE,
     '    } else if (is_word(geo, "conic") && geo->nitems == 3) {',
     '    } else if (false && is_word(geo, "conic") && geo->nitems == 3) {', FLOW),
    ('a-conic-stop-in-degrees-is-unwritten', STYLE,
     '    if (kind != FLOW_GRADIENT_CONIC || v->kind != GARB_V_ANGLE)',
     '    if (((void)kind, true) || v->kind != GARB_V_ANGLE)', FLOW),
    ('a-conic-is-written-as-radial', COMPUTED,
     '    } else if (g->kind == FLOW_GRADIENT_CONIC) {\n        // What the page left',
     '    } else if (false) {\n        // What the page left', FLOW_COMPUTED),
    ('a-conic-turns-anticlockwise', PAINT,
     '    double t = (atan2(u - l->cx, l->cy - v) - l->turn0)',
     '    double t = (atan2(l->cx - u, l->cy - v) - l->turn0)', YON),
    ('a-conic-ignores-its-from', PAINT,
     '        turn0 = g->angle / 1000.0 * 3.14159265358979323846 / 180;',
     '        turn0 = 0;', YON),
    ('a-conic-start-is-not-in-the-tile-key', PAINT,
     '                                line->turn0};',
     '                                0};', YON),
    ('an-interpolating-conic-costs-the-declaration', PROPS,
     '                !(angle_calc(t->children, t->nchildren, false) || writes_interpolation(t)))',
     '                !(angle_calc(t->children, t->nchildren, false) || (false && writes_interpolation(t))))', GARB),
    ('a-negative-conic-percentage-is-refused', PROPS,
     '    return vc_angle(c, true, out) || vc_dim(c, ACCEPT_PERCENT, true, false, &s->a, out);',
     '    return vc_angle(c, true, out) || vc_dim(c, ACCEPT_PERCENT, false, false, &s->a, out);', GARB),
    ('a-conic-centre-takes-the-far-side', PAINT,
     '    double t = (atan2(u - l->cx, l->cy - v) - l->turn0)',
     '    double t = (atan2(u - l->cx, -(v - l->cy)) - l->turn0)', YON),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, harness in mutants:
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
                result = subprocess.run(['bash', '-c', harness], cwd=root, stdout=out, stderr=out, timeout=900)
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

print(f'\nlibgarb mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
