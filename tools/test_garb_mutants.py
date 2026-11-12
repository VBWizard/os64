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
FLOWC = 'userland/libflow/flow.c'
COPY = 'userland/libflow/style_copy.inc'
YONDER = 'userland/apps/yonder/yonder.c'
SCRIPTS = 'tools/test_yonder_scripts_host.sh'

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
    # Masks (MASKS.md): read, kept, written, grouped and drawn.
    ('a-mask-position-is-quirky', PROPS,
     '                      s->quirks && l->prop == GARB_BACKGROUND_POSITION_X, &s->a, out);',
     '                      s->quirks, &s->a, out);', GARB),
    ('the-mask-shorthand-is-quirky', PROPS,
     '            strict.quirks = false;\n            strict.a.short_of_memory = false;',
     '            strict.a.short_of_memory = false;', GARB),
    ('the-mask-position-shorthand-is-quirky', PROPS,
     '        strict.quirks = s->quirks && bg;',
     '        strict.quirks = s->quirks;', GARB),
    ('a-mask-position-takes-three-parts', PROPS,
     '            } else if (!have_pos && position(&strict, c, false, &x, &y)) {',
     '            } else if (!have_pos && position(&strict, c, true, &x, &y)) {', GARB),
    ('no-clip-takes-two-boxes-beside-it', PROPS,
     '            } else if (boxes < (no_clip ? 1 : 2) && (box[boxes] = vc_keyword(c, kBox)) != NULL) {',
     '            } else if (boxes < 2 && (box[boxes] = vc_keyword(c, kBox)) != NULL) {', GARB),
    ('one-box-sets-only-the-origin', PROPS,
     '        else if (boxes > 0)\n            l[M_CLIP] = kw(box[boxes - 1]);',
     '        else if (false)\n            l[M_CLIP] = kw(box[boxes - 1]);', GARB),
    ('a-mask-origin-starts-at-the-padding-box', PROPS,
     '                             percent(0),       auto_size(s),     kw("border-box"),',
     '                             percent(0),       auto_size(s),     kw("padding-box"),', GARB),
    ('a-mask-clip-takes-text', PROPS,
     'static const char *const kMaskClip[] = {"border-box", "padding-box", "content-box", "no-clip",\n',
     'static const char *const kMaskClip[] = {"border-box", "padding-box", "content-box", "no-clip", "text",\n', GARB),
    ('the-webkit-mask-is-unknown', PROPS,
     '    {"-webkit-mask", "mask", true},\n', '', GARB),
    ('a-mask-list-lands-in-the-backgrounds', STYLE,
     '    bool mask = p >= GARB_MASK_IMAGE && p <= GARB_MASK_CLIP;\n    flow_backgrounds_t *b',
     '    bool mask = false;\n    flow_backgrounds_t *b', FLOW_COMPUTED),
    ('the-initial-mask-origin-is-the-padding-box', STYLE,
     'static const flow_masks_t kMasks = {{&kNoImage, &kRepeat, &kAtStart, &kAtStart, &kOwnSize,\n'
     '                                     &kBorderBox, &kBorderBox,',
     'static const flow_masks_t kMasks = {{&kNoImage, &kRepeat, &kAtStart, &kAtStart, &kOwnSize,\n'
     '                                     &kPaddingBox, &kBorderBox,', FLOW_COMPUTED),
    ('inherit-takes-no-mask', STYLE,
     '    case GARB_MASK_COMPOSITE: case GARB_MASK_MODE:\n        f_take_mask(prop, d, s);',
     '    case GARB_MASK_COMPOSITE: case GARB_MASK_MODE:\n        (void)f_take_mask;', FLOW_COMPUTED),
    ('the-computed-mask-is-the-background', COMPUTED,
     '    const flow_backgrounds_t *bg = mask ? &s->masks.layers : &s->backgrounds;',
     '    const flow_backgrounds_t *bg = &s->backgrounds;', FLOW_COMPUTED),
    ('a-mask-makes-no-group', FLOWC,
     '            f_group_filter(b->style) || f_masked(b->style) ||',
     '            f_group_filter(b->style) ||', YON),
    ('a-mask-makes-no-stacked-box', FLOWC,
     '                   f_blended(src->style) || f_masked(src->style) || f_isolated(src->style)) &&',
     '                   f_blended(src->style) || f_isolated(src->style)) &&', YON),
    ('a-mask-cuts-no-bounds', FLOWC,
     '            done.r = mask_cut(tree->layers[done.layer].box,\n'
     '                              filter_grown(tree->layers[done.layer].box, done.r), scroll);',
     '            done.r = filter_grown(tree->layers[done.layer].box, done.r);', YON),
    ('no-clip-still-cuts', FLOWC,
     '        if (flow_mask_layer(box->style, i).layer.clip == FLOW_EDGE_NO_CLIP)\n            return r;',
     '        if (flow_mask_layer(box->style, i).layer.clip == FLOW_EDGE_NO_CLIP && false)\n            return r;', YON),
    ('a-copied-style-loses-its-mask', COPY,
     '    if(!style_copy_layers(c,&s->backgrounds) || !style_copy_layers(c,&s->masks.layers))return false;',
     '    if(!style_copy_layers(c,&s->backgrounds))return false;', FLOW),
    ('every-operator-adds', PAINT,
     '            yonder_mask_combine(run, one, n, m.composite);',
     '            yonder_mask_combine(run, one, n, FLOW_COMPOSITE_ADD);', YON),
    ('the-bottom-layer-is-composited', PAINT,
     '        if (i == layers - 1)\n            os64_memcpy(run, one, n);\n        else\n'
     '            yonder_mask_combine(run, one, n, m.composite);',
     '        if (i == layers - 1)\n            os64_memset(run, 0, n);\n'
     '        yonder_mask_combine(run, one, n, m.composite);', YON),
    ('luminance-is-read-as-alpha', PAINT,
     '            p->v->layer_take(p->v->ctx, one, m.mode == FLOW_MASK_LUMINANCE);',
     '            p->v->layer_take(p->v->ctx, one, false);', YON),
    ('a-mask-sized-to-nothing-shows', PAINT,
     '        if (nothing || (l.image == NULL && l.gradient == NULL))',
     '        if ((false && nothing) || (l.image == NULL && l.gradient == NULL))', YON),
    ('a-hiding-mask-is-drawn-plain-past-the-budget', PAINT,
     '    if (mask_hides_whole(box)) {\n        p->view = (os64_gui_rect_t){0, 0, 0, 0};',
     '    if (false && mask_hides_whole(box)) {\n        p->view = (os64_gui_rect_t){0, 0, 0, 0};', YON),
    ('a-mask-is-charged-no-captures', PAINT,
     '    int64_t area = (int64_t)shown.w * shown.h * (masked ? 1 + flow_mask_layers(box->style) : 1);',
     '    int64_t area = (int64_t)shown.w * shown.h * (masked ? 1 : 1);', YON),
    ('a-mask-picture-is-a-background-picture', PAINT,
     '            (void)p->v->backdrop(p->v->ctx, b, i | YONDER_MASK_LAYER, &area, origin, area.x,',
     '            (void)p->v->backdrop(p->v->ctx, b, i, &area, origin, area.x,', YON),
    ('a-mask-reference-is-drawn', PAINT,
     '        if (mask_reference(&l))\n            return false;',
     '        if (mask_reference(&l) && false)\n            return false;', YON),
    ('a-deep-hiding-mask-is-drawn', PAINT,
     '        if (flow_box_through(box, p->scroll, (double[6]){0}, NULL, NULL) || mask_hides_whole(box))',
     '        if (flow_box_through(box, p->scroll, (double[6]){0}, NULL, NULL))', YON),
    ('a-refused-position-keeps-its-tokens', PROPS,
     '    if (position_read(s, c, three, x, y))\n        return true;\n    c->i = at;',
     '    if (position_read(s, c, three, x, y))\n        return true;\n    (void)at;', GARB),
    ('a-filtered-mask-is-cut-before-its-filter', FLOWC,
     '    if (!f_masked(box->style) || f_group_filter(box->style))\n        return r;',
     '    if (!f_masked(box->style))\n        return r;', YON),
    # The atom group: an atom on a line with an effect is stacked whole.
    ('an-effect-stacks-no-atom', FLOWC,
     '    if (moved || (fr->kind == FF_ATOMIC && (f_opacity_context(s) || f_filtered(s) ||',
     '    if (moved || (false && fr->kind == FF_ATOMIC && (f_opacity_context(s) || f_filtered(s) ||', YON),
    ('a-picture-is-a-plain-inline', 'userland/libflow/internal.h',
     '    return b->kind == FLOW_BOX_ATOMIC;\n}',
     '    (void)b;\n    return false;\n}', FLOW),
    ('a-stacked-atom-leaves-its-line', FLOWC,
     '            if (pieces != NULL && !(fr->kind == FF_ATOMIC && f_transforms(fr->style)))',
     '            if (pieces != NULL && !pieces->stacked)', FLOW),
    ('a-mask-picture-is-never-sent-for', YONDER,
     '    return flow_background_layers(s) + flow_mask_layers(s);',
     '    return flow_background_layers(s) + 0 * flow_mask_layers(s);', SCRIPTS),
    ('a-mask-reference-is-sent-for', YONDER,
     '    if (l.image != NULL && l.image_len > 0 && l.image[0] == \'#\')\n        l.image = NULL;',
     '    if (false)\n        l.image = NULL;', SCRIPTS),
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
