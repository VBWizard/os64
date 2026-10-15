#!/usr/bin/env python3
"""One deliberate break at a time to the animation model (docs/design/
pending/PILE3.md § The animation model, slice S8a) — libgarb's grammars,
@keyframes and media answer, libflow's easing, clock, timeline, mixing and
side effects, yonder's layouts for motion and its two switches, and the
settings window those switches live in — against the cases that state
its rules: libgarb's corpus, libflow's --motion run, the yonder window's
host, and the toolkit's text host. A mutant is CAUGHT when they
fail, MISSED when they pass, which means a test is missing and gets
written. A build failure is neither.

The shape is test_cliptext_mutants.py's, with each mutant run against the
one harness that holds its rule: each is applied to the worktree's copy of
its file, the cases are run, and the file is put back from the bytes read
before the change, checked byte for byte, whatever happens. Run it from a
clean tree, and never beside another harness that edits the same tree; the
per-mutant logs are in --logs.

Breaks are left out where nothing can tell them apart from the code: a
measured layout handed the timeline of the page it copies (the pages a
measured layout copies are never shown, and have none); rotation axes
compared by their lengths, which an axis held to the domain (values.h)
takes as unit3 does, by its largest part; and f_matrix_mix's door for a
matrix holding a non-finite entry, which the decompositions' own guards
refuse further in: the door is there so that no NaN comparison decides.
The curve's power form, whose rounding at y controls of 1e6 is some 2e-10,
below every grid a value lands on, is left out too: cubic() returns the
curve's ends itself, and beside them the solved parameter's own last bit
moves y as far.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/motion-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this, or any of these, comma-separated')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PROPS = 'userland/libgarb/props.c'
VALUES = 'userland/libgarb/values.c'
VALUES_H = 'userland/libgarb/include/garb/values.h'
CASCADE = 'userland/libgarb/cascade.c'
MEDIA = 'userland/libgarb/media.c'
MOTION = 'userland/libflow/motion.c'
STYLE = 'userland/libflow/style.c'
FLOW = 'userland/libflow/flow.c'
INTERNAL = 'userland/libflow/internal.h'
YONDER = 'userland/apps/yonder/yonder.c'
SETTINGS = 'userland/apps/yonder/settings.c'
UI_SETTINGS = 'userland/libos64/ui_settings.c'

GARB = 'bash tools/test_garb_host.sh'
LIBFLOW = 'bash tools/test_libflow_host.sh --motion'
WINDOW = 'bash tools/test_yonder_scripts_host.sh'
UI_TEXT = 'python3 tools/test_ui_text_host.py --real'

# (name, file, old, new, cases). Each breaks one rule the design states.
mutants = [
    # libgarb: the grammars and the Compatibility Standard's names.
    ('the-prefixed-shorthand-is-not-read', PROPS,
     '    {"-webkit-animation", "animation"},',
     '    {"-webkit-animationx", "animation"},', GARB),
    ('a-time-takes-any-unit', VALUES,
     '    else if (ieq(t->unit, t->unit_len, "ms"))\n        out->unit = GARB_U_MS;\n    else\n        return false;',
     '    else\n        out->unit = GARB_U_MS;', GARB),
    ('a-negative-duration-is-read', VALUES,
     '    if (t == NULL || t->kind != GARB_DIMENSION || (!negative && t->number < 0))',
     '    (void)negative;\n    if (t == NULL || t->kind != GARB_DIMENSION)', GARB),
    ('a-bezier-x-may-leave-the-unit', PROPS,
     '        if (!(a[0].number >= 0 && a[0].number <= 1 && a[2].number >= 0 && a[2].number <= 1))',
     '        if (false)', GARB),
    ('jump-none-takes-one-step', PROPS,
     '                (os64_streq(k, "jump-none") && count->number < 2))',
     '                false)', GARB),
    # libgarb: @keyframes.
    ('the-prefixed-rule-is-not-read', CASCADE,
     '        else if (at(r, "keyframes") || at(r, "-webkit-keyframes"))',
     '        else if (at(r, "keyframes"))', GARB),
    ('an-earlier-rule-of-a-name-wins', CASCADE,
     '        if (i + 1 < n && keyframes_cmp(&c->keyframes[i], &c->keyframes[i + 1]) == 0)',
     '        if (i > 0 && keyframes_cmp(&c->keyframes[i], &c->keyframes[i - 1]) == 0)', GARB),
    ('an-important-keyframe-declaration-is-kept', CASCADE,
     '            if (items[j].kind != GARB_ITEM_DECL || d->important || garb_decl_is_custom(d) ||',
     '            if (items[j].kind != GARB_ITEM_DECL || garb_decl_is_custom(d) ||', GARB),
    ('a-keyframe-keeps-animation-longhands', CASCADE,
     '        if (props[k] >= GARB_ANIMATION_NAME && props[k] <= GARB_ANIMATION_FILL_MODE)\n            return true;',
     '        if (false)\n            return true;', GARB),
    ('from-is-not-the-start', CASCADE,
     '        if (t->kind == GARB_IDENT && ieq(t->text, t->len, "from"))\n            at_[n] = 0;',
     '        if (t->kind == GARB_IDENT && ieq(t->text, t->len, "from"))\n            at_[n] = 0.5;', GARB),
    ('an-offset-past-100-is-kept', CASCADE,
     '        else if (t->kind == GARB_PERCENTAGE && t->number >= 0 && t->number <= 100)',
     '        else if (t->kind == GARB_PERCENTAGE)', GARB),
    # libgarb: what pages are told.
    ('motion-is-always-reduced', MEDIA,
     '    if (os64_streq(f->name, "prefers-reduced-motion") && env.reduced_motion)',
     '    if (os64_streq(f->name, "prefers-reduced-motion"))', GARB),
    ('reduce-is-never-answered', MEDIA,
     '    if (os64_streq(f->name, "prefers-reduced-motion") && env.reduced_motion)',
     '    if (os64_streq(f->name, "prefers-reduced-motion") && false)', GARB),

    # libflow: easing (CSS Easing 1).
    ('a-bezier-has-no-tangent-below', MOTION,
     '        if (x < 0) {\n            if (p[0] > 0)',
     '        if (x < -1e300) {\n            if (p[0] > 0)', LIBFLOW),
    ('a-bezier-has-no-tangent-above', MOTION,
     '        if (x > 1) {\n            if (p[2] < 1)',
     '        if (x > 1e300) {\n            if (p[2] < 1)', LIBFLOW),
    ('the-before-flag-is-ignored', MOTION,
     '    if (before && fmod(x * n, 1.0) == 0)\n        step -= 1;',
     '    (void)before;', LIBFLOW),
    ('jump-start-does-not-jump', MOTION,
     '    if (e->jump == FLOW_JUMP_START || e->jump == FLOW_JUMP_BOTH)\n        step += 1;',
     '    if (e->jump == FLOW_JUMP_BOTH)\n        step += 1;', LIBFLOW),
    # libflow: the clock (Web Animations 1).
    ('backwards-fill-is-ignored', MOTION,
     '        if (a->fill != FLOW_FILL_BACKWARDS && a->fill != FLOW_FILL_BOTH)\n            return k;\n        at = fmax(local - delay, 0);',
     '        return k;', LIBFLOW),
    ('forwards-fill-is-ignored', MOTION,
     '        if (a->fill != FLOW_FILL_FORWARDS && a->fill != FLOW_FILL_BOTH)\n            return k;',
     '        return k;', LIBFLOW),
    ('alternate-does-not-alternate', MOTION,
     '        return isinf(d) || fmod(d, 2) == 0;',
     '        (void)d;\n        return true;', LIBFLOW),
    ('an-iteration-ends-at-zero', MOTION,
     '    if (simple == 0 && k.phase != PHASE_BEFORE && at == active && count != 0)\n        simple = 1;',
     '', LIBFLOW),
    # libflow: the timeline.
    ('a-paused-animation-does-not-hold', MOTION,
     '        if (an->timing.paused && !r->paused) {\n            r->hold = now - r->start;\n            r->paused = true;',
     '        if (an->timing.paused && !r->paused) {\n            r->hold = now - r->start;', LIBFLOW),
    ('a-resumed-animation-starts-over', MOTION,
     '            r->start = now - r->hold;\n            r->paused = false;',
     '            r->start = now;\n            r->paused = false;', LIBFLOW),
    ('an-incomplete-layout-drops-records', MOTION,
     '    for (int32_t i = 0; ok && !complete && i < t->from->n; i++) {',
     '    for (int32_t i = 0; ok && !complete && i < 0 * t->from->n; i++) {', LIBFLOW),
    ('a-complete-layout-keeps-every-record', MOTION,
     '    for (int32_t i = 0; ok && !complete && i < t->from->n; i++) {',
     '    for (int32_t i = 0; ok && (complete || !complete) && i < t->from->n; i++) {', LIBFLOW),
    ('a-carried-record-joins-no-chain', MOTION,
     '            r->next = f_map_get(&t->by_node, was->node);\n            ok = f_map_put(&t->by_node, was->node, r);',
     '            r->next = NULL;', LIBFLOW),
    ('a-carried-record-forgets-its-place', MOTION,
     '            r->place = was->place;\n',
     '', LIBFLOW),
    ('no-room-to-carry-is-ignored', MOTION,
     '        ok = r != NULL;\n        if (ok) {',
     '        if (r != NULL) {', LIBFLOW),
    ('a-matched-record-is-not-met', MOTION,
     '            t->met[was->index] = true;',
     '            (void)was->index;', LIBFLOW),
    ('a-matched-name-starts-over', MOTION,
     '    if (was != NULL) {\n        r->start = was->start;',
     '    if (false) {\n        r->start = was->start;', LIBFLOW),
    # libflow: transforms interpolated (Chromium's scheme, PILE3.md).
    ('the-turn-is-not-the-shorter-way', MOTION,
     '    if (c < 0) {\n        for (int i = 0; i < 4; i++)\n            from[i] = -from[i];\n        c = -c;\n    }',
     '', LIBFLOW),
    ('a-2d-shear-is-dropped', MOTION,
     '    d->sk[0] = sk;',
     '    d->sk[0] = 0;', LIBFLOW),
    ('a-2d-flip-is-lost', MOTION,
     '    if (flip && m11 < m22)\n        s0 = -s0;',
     '    if (false && m11 < m22)\n        s0 = -s0;', LIBFLOW),
    # libflow: values mixed.
    ('colours-mix-unpremultiplied', MOTION,
     '        double ca = ((a >> shift) & 0xFF) * aa, cb = ((b >> shift) & 0xFF) * ab;\n        double c = lerp(ca, cb, t) / alpha;',
     '        double ca = ((a >> shift) & 0xFF), cb = ((b >> shift) & 0xFF);\n        double c = lerp(ca, cb, t);', LIBFLOW),
    ('integers-round-down', MOTION,
     '    return round_i32(floor(v + 0.5));',
     '    return round_i32(floor(v));', LIBFLOW),
    ('a-discrete-value-flips-at-the-start', MOTION,
     '    copy_prop(p, d, t < 0.5 ? a : b);',
     '    copy_prop(p, d, t <= 0 ? a : b);', LIBFLOW),
    ('visibility-flips-half-way', MOTION,
     '        d->visibility = t <= 0 ? a->visibility : t >= 1 ? b->visibility : FLOW_VISIBLE;',
     '        d->visibility = t < 0.5 ? a->visibility : b->visibility;', LIBFLOW),
    ('shadow-lists-are-not-padded', MOTION,
     '    int32_t n = na > nb ? na : nb;\n    for (int32_t i = 0; i < n && i < na && i < nb; i++)\n        if (a[i].inset != b[i].inset)\n            return false;',
     '    int32_t n = na > nb ? na : nb;\n    if (na != nb)\n        return false;\n    for (int32_t i = 0; i < n && i < na && i < nb; i++)\n        if (a[i].inset != b[i].inset)\n            return false;', LIBFLOW),
    # libflow: keyframes.
    ('an-implicit-start-is-the-initial-value', MOTION,
     '        if (i == 0)\n            return (Frame){l->under, 0, own, true};',
     '        static flow_style_t initial;\n        if (i == 0)\n            return (Frame){&initial, 0, own, true};', LIBFLOW),
    ('a-keyframe-easing-is-ignored', MOTION,
     '            return (Frame){&f->style, f->offset, f->has_easing ? &f->easing : own, false};',
     '            return (Frame){&f->style, f->offset, own, false};', LIBFLOW),
    ('border-currentcolor-stays-put', MOTION,
     '        if (out->current_colours & FLOW_CURRENT_BORDER(i))\n            out->border_color[i] = out->color;',
     '', LIBFLOW),
    ('an-important-winner-is-animated', STYLE,
     '            if (!lay_sets(c, &k, ua, parent, kf[j].sets, NULL, kf[j].sheet, kf[j].nsets, &important))',
     '            if (!lay_sets(c, &k, ua, parent, kf[j].sets, NULL, kf[j].sheet, kf[j].nsets, NULL))', LIBFLOW),
    ('no-memory-for-a-list-is-a-whole-layout', STYLE,
     '    if (lists.no_memory)\n        return false;',
     '', LIBFLOW),
    # libflow: when the page next changes.
    ('a-change-shows-only-the-millisecond-after', MOTION,
     '    return first > now ? first : after > now ? after : INFINITY;',
     '    return first > now ? after : after > now ? after : INFINITY;', LIBFLOW),
    ('a-change-never-falls-past-its-millisecond', MOTION,
     '    return first > now ? first : after > now ? after : INFINITY;',
     '    (void)after;\n    return first > now ? first : INFINITY;', LIBFLOW),
    ('a-mix-cancels-at-a-huge-fraction', MOTION,
     '    if (t == 1)\n        return b;\n    return a + (b - a) * t;',
     '    return (1 - t) * a + t * b;', LIBFLOW),
    ('records-chain-without-their-new-ones', MOTION,
     '        m->anims[i].record->next = head;',
     '        m->anims[i].record->next = NULL;', LIBFLOW),
    ('names-match-first-record-of-their-name', MOTION,
     '            names[b].record = old[a].record;\n            a--;\n            b--;',
     '            int32_t f = a;\n            while (f > 0 && name_cmp(&old[f - 1], &names[b]) == 0)\n                f--;\n            names[b].record = old[f].record;\n            a--;\n            b--;', LIBFLOW),
    ('a-held-length-goes-negative', MOTION,
     '    if (l->kind == FLOW_LENGTH_PX && l->value < 0)\n        l->value = 0;\n    else if',
     '    if (false)\n        l->value = 0;\n    else if', LIBFLOW),
    ('a-percent-is-not-held', MOTION,
     '    else if (l->kind == FLOW_LENGTH_PERCENT && l->value <= 0 && l->offset <= 0)',
     '    else if (false)', LIBFLOW),
    ('a-box-drawn-outside-is-not-seen', FLOW,
     '    for (int32_t i = 0; i < tree->nstacked && next > now; i++) {',
     '    for (int32_t i = 0; false && i < tree->nstacked && next > now; i++) {', LIBFLOW),
    # The wake through a box drawn outside asks only what is handed down.
    ('the-wake-asks-the-whole-motion', FLOW,
     '                next = fmin(next, f_motion_next(st->motion, now, through ? NULL : &ask));',
     '                next = fmin(next, f_motion_next(st->motion, now, false && through ? &ask : NULL));', LIBFLOW),
    ('a-fade-is-not-handed-down', FLOW,
     '        if (garb_prop_inherited((garb_prop_t)p) || p == GARB_OPACITY)',
     '        if (garb_prop_inherited((garb_prop_t)p))', LIBFLOW),
    ('nothing-inherited-is-handed-down', FLOW,
     '        if (garb_prop_inherited((garb_prop_t)p) || p == GARB_OPACITY)',
     '        if (p == GARB_OPACITY)', LIBFLOW),
    ('the-walk-judges-what-is-not-asked', MOTION,
     '    if (d->only != NULL && !f_props_has(d->only, p))\n        return true;\n',
     '', LIBFLOW),
    ('nothing-is-taken-by-keyword', STYLE,
     '            if (inherits)\n                f_props_add(&sp->taken, set->prop);',
     '            if (false)\n                f_props_add(&sp->taken, set->prop);', LIBFLOW),
    ('unset-is-taken-on-anything', STYLE,
     '            if (inherits)\n                f_props_add(&sp->taken, set->prop);',
     '            if (inherits || os64_streq(k, "unset"))\n                f_props_add(&sp->taken, set->prop);', LIBFLOW),
    ('the-wake-asks-nothing-taken', FLOW,
     '                    ask.w[w] = handed.w[w] | took.w[w];',
     '                    ask.w[w] = handed.w[w];', LIBFLOW),
    ('what-is-taken-is-not-cut-between', FLOW,
     '                    took.w[w] &= st->taken.w[w];',
     '                    took.w[w] |= st->taken.w[w];', LIBFLOW),
    ('a-keyframe-takes-nothing', STYLE,
     '            for (int w = 0; w < F_PROP_WORDS; w++)\n                sp->taken.w[w] |= k.taken.w[w];',
     '', LIBFLOW),
    ('a-text-clip-is-asked-what-is-handed', FLOW,
     '                bool through = in_clip && clips_to_text(&st->style);',
     '                bool through = false && in_clip && clips_to_text(&st->style);', LIBFLOW),
    ('a-box-out-of-the-clip-is-in-it', FLOW,
     '                bool through = in_clip && clips_to_text(&st->style);',
     '                bool through = clips_to_text(&st->style);\n                (void)in_clip;', LIBFLOW),
    ('a-box-between-keeps-the-clip', FLOW,
     '            if (ab != NULL && leaves_text_clip(ab))\n                in_clip = false;',
     '            (void)ab;', LIBFLOW),
    ('no-background-clips-to-text', FLOW,
     '        if (flow_background_layer(s, i).clip == FLOW_EDGE_TEXT)',
     '        if (flow_background_layer(s, i).clip == FLOW_EDGE_TEXT && false)', LIBFLOW),
    ('the-edges-count-what-is-not-asked', MOTION,
     '            (only == NULL || f_props_has(only, (garb_prop_t)p)) &&',
     '            (only == NULL || only != NULL) &&', LIBFLOW),
    ('an-animation-that-moves-waits-out-of-view', FLOW,
     '            (st->motion->in_place && !may_own_canvas(tree, n) && !motion_seen(tree, n, view, scroll)))',
     '            (!may_own_canvas(tree, n) && !motion_seen(tree, n, view, scroll)))', LIBFLOW),
    ('a-transform-repaints-in-place', MOTION,
     '    case GARB_COLOR: case GARB_TEXT_FILL_COLOR: case GARB_BACKGROUND_COLOR:',
     '    case GARB_COLOR: case GARB_TEXT_FILL_COLOR: case GARB_BACKGROUND_COLOR: case GARB_TRANSFORM:', LIBFLOW),
    ('a-length-overflows-its-squares', MOTION,
     '    double k = fmax(fabs(v[0]), fmax(fabs(v[1]), fabs(v[2])));\n    if (!(k > 0) || !isfinite(k))\n        return k;',
     '    return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);\n    double k = fmax(fabs(v[0]), fmax(fabs(v[1]), fabs(v[2])));\n    if (!(k > 0) || !isfinite(k))\n        return k;', LIBFLOW),
    ('a-style-carries-its-width', MOTION,
     '        d->border_style[i] = s->border_style[i];\n        break;',
     '        d->border_style[i] = s->border_style[i];\n        d->border_width[i] = s->border_width[i];\n        break;', LIBFLOW),
    ('the-base-loses-a-hidden-width', STYLE,
     '    for (int side = 0; side < 4; side++)\n        m->base.border_width[side] = own_width(c, &base, st, none, side);',
     '    (void)none;', LIBFLOW),
    ('a-keyframe-loses-a-hidden-width', STYLE,
     '            for (int side = 0; side < 4; side++)\n                k.s.border_width[side] =\n'
     '                    own_width(c, &k, st, (garb_style_t){kf[j].sets, NULL, NULL, kf[j].nsets}, side);',
     '', LIBFLOW),
    ('z-index-from-auto-stays-auto', MOTION,
     '        d->has_z_index = true;\n',
     '', LIBFLOW),
    ('an-overshooting-integer-is-cast-unheld', MOTION,
     '    return round_i32(floor(v + 0.5));',
     '    return (int32_t)floor(v + 0.5);', LIBFLOW),
    ('an-element-without-a-box-is-never-seen', FLOW,
     '    if (b != NULL)\n        return pieces_seen(b, view, scroll);\n    for (',
     '    if (true)\n        return b != NULL && pieces_seen(b, view, scroll);\n    for (', LIBFLOW),
    ('an-inherited-name-shares-the-frames', CASCADE,
     '    while (v != NULL && v->kind == GARB_V_WIDE && os64_streq(v->keyword, "inherit")) {',
     '    while (false && v != NULL && v->kind == GARB_V_WIDE && os64_streq(v->keyword, "inherit")) {', LIBFLOW),
    ('a-delayed-start-is-not-due', MOTION,
     '            next = fmin(next, change_due(base + k.before_end, now));\n            continue;',
     '            continue;', LIBFLOW),
    ('a-paused-animation-is-due', MOTION,
     '        if (an->record == NULL || an->record->paused || !anim_shows(an, a, last, only))\n            continue;\n'
     '        double base = an->record->start;',
     '        if (an->record == NULL || !anim_shows(an, a, last, only))\n            continue;\n'
     '        double base = an->record->start;', LIBFLOW),
    ('what-the-view-cannot-see-is-due', FLOW,
     '    return meets(doc_rect_of(b, inked, scroll), view);',
     '    return true || meets(doc_rect_of(b, inked, scroll), view);', LIBFLOW),
    # libflow: the side effects (Web Animations 1 § 5.6).
    ('an-animated-transform-makes-no-frame', INTERNAL,
     '           s->scale != NULL || ((s->will_change | s->animating) & FLOW_WILL_TRANSFORM);',
     '           s->scale != NULL || (s->will_change & FLOW_WILL_TRANSFORM);', LIBFLOW),
    ('an-animated-opacity-makes-no-context', INTERNAL,
     '    return s->opacity < 1000 || ((s->will_change | s->animating) & FLOW_WILL_OPACITY);',
     '    return s->opacity < 1000 || (s->will_change & FLOW_WILL_OPACITY);', LIBFLOW),
    ('an-animated-box-is-no-group', FLOW,
     '    return (b->style->opacity < 1000 || own_through(b) || b->style->animating != 0) &&',
     '    return (b->style->opacity < 1000 || own_through(b)) &&', LIBFLOW),
    ('a-box-fading-from-nothing-is-skipped', STYLE,
     '    out->transparent = (sp.s.opacity == 0 && !(sp.s.animating & FLOW_WILL_OPACITY)) ||',
     '    out->transparent = sp.s.opacity == 0 ||', LIBFLOW),

    # yonder: the layouts for motion.
    ('a-covered-window-animates', YONDER,
     '    if (!s_animations || g.covered || g.page.tree == NULL || page_rebuild_owed(&g.page))\n        return YONDER_NEVER;',
     '    if (!s_animations || g.page.tree == NULL || page_rebuild_owed(&g.page))\n        return YONDER_NEVER;', WINDOW),
    ('a-slow-page-animates', YONDER,
     '    if (cost_us > (uint64_t)MOTION_COST_MS * 1000)\n        return YONDER_NEVER;',
     '', WINDOW),
    ('the-page-s-own-cost-does-not-stand-in', YONDER,
     '    return g.motion_laid ? g.motion_us : g.laid_ms * 1000;',
     '    return g.motion_laid ? g.motion_us : 0;', WINDOW),
    # yonder: the animations the cost rule stopped are said.
    ('a-pause-is-not-said', YONDER,
     '                  paused);',
     '                  "");', WINDOW),
    ('a-pause-is-not-recorded', YONDER,
     '            yonder_diag_fact(g.page.diag, "animations", fact);',
     '            (void)fact;', WINDOW),
    ('a-resume-is-not-recorded', YONDER,
     '                                                  : "running");',
     '                                                  : "off");', WINDOW),
    ('turning-off-is-recorded-as-running', YONDER,
     '        yonder_diag_fact(g.page.diag, "animations", !s_animations ? "off"',
     '        yonder_diag_fact(g.page.diag, "animations", false ? "off"', WINDOW),
    ('the-frame-that-pauses-is-not-said', YONDER,
     '    say_laid_out(g.laid_ms);\n    return true;',
     '    return true;', WINDOW),
    ('the-said-pause-asks-no-paint', YONDER,
     '    say_laid_out(g.laid_ms);\n    return true;',
     '    say_laid_out(g.laid_ms);\n    return false;', WINDOW),
    ('a-page-with-no-animations-is-paused', YONDER,
     '    if (!s_animations || !flow_animated(g.page.tree) || cost_us == UINT64_MAX ||',
     '    if (!s_animations || g.page.tree == NULL || cost_us == UINT64_MAX ||', WINDOW),
    ('animations-off-are-paused', YONDER,
     '    if (!s_animations || !flow_animated(g.page.tree) || cost_us == UINT64_MAX ||',
     '    if (!flow_animated(g.page.tree) || cost_us == UINT64_MAX ||', WINDOW),
    ('a-failed-frame-is-paused', YONDER,
     '    if (!s_animations || !flow_animated(g.page.tree) || cost_us == UINT64_MAX ||',
     '    if (!s_animations || !flow_animated(g.page.tree) ||', WINDOW),
    ('every-tree-is-animated', FLOW,
     '    return tree != NULL && tree->styles != NULL && tree->styles->nanimated > 0;',
     '    return tree != NULL && tree->styles != NULL;', WINDOW),
    ('no-rest-after-a-costly-layout', YONDER,
     '        if (due < rested)\n            due = rested;',
     '        (void)rested;', WINDOW),
    ('no-frame-floor', YONDER,
     '        if (due < frame)\n            due = frame;',
     '        (void)frame;', WINDOW),
    ('a-layout-for-motion-is-not-quiet', YONDER,
     '    if (!for_motion)\n        say_laid_out(ms_between(&t0, &t1));',
     '    say_laid_out(ms_between(&t0, &t1));', WINDOW),
    ('a-frame-s-paint-is-free', YONDER,
     '    g.motion_ended = yonder_now_ms();\n    g.motion_us = (uint64_t)(os64_micros() - g.motion_began_us);\n    char why[64];',
     '    char why[64];', WINDOW),
    ('a-paint-without-a-frame-costs', YONDER,
     '    if (!g.motion_painting)\n        return false;\n    g.motion_painting = false;',
     '    g.motion_painting = false;', WINDOW),
    ('a-failed-layout-for-motion-is-not-the-last', YONDER,
     '        g.motion_us = laid ? (uint64_t)(os64_micros() - began_us) : UINT64_MAX;',
     '        g.motion_us = (uint64_t)(os64_micros() - began_us);', WINDOW),
    ('a-tick-lays-out-whenever-it-rings', YONDER,
     '    if (motion_next() <= yonder_now_ms())\n        (void)lay_out_shown(true, true);',
     '    (void)lay_out_shown(true, true);', WINDOW),
    ('the-ticker-is-not-told', YONDER,
     '    uint64_t motion = motion_next();\n    if (motion < next)\n        next = motion;',
     '', WINDOW),
    ('an-arriving-page-inherits-the-last-cost', YONDER,
     '    g.motion_laid = false;             // a new page\'s own layout stands for its cost',
     '', WINDOW),
    ('an-arriving-page-has-no-timeline', YONDER,
     '    page_timeline(fresh);\n    laid = laid && lay_out_page(fresh, width, height);',
     '    laid = laid && lay_out_page(fresh, width, height);', WINDOW),
    ('a-relayout-makes-no-timeline', YONDER,
     '    page_timeline(&g.page);\n    bool laid = lay_out_page(&g.page, width, height);',
     '    bool laid = lay_out_page(&g.page, width, height);', WINDOW),
    ('a-rebuild-makes-no-timeline', YONDER,
     '        page_timeline(p);\n        Page next = *p;',
     '        Page next = *p;', WINDOW),
    # yonder: the switches.
    ('animations-off-still-run', YONDER,
     '    s_env.timeline = s_animations ? p->timeline : NULL;',
     '    s_env.timeline = p->timeline;', WINDOW),
    ('animations-off-are-still-due', YONDER,
     '    if (!s_animations || g.covered || g.page.tree == NULL || page_rebuild_owed(&g.page))',
     '    if (g.covered || g.page.tree == NULL || page_rebuild_owed(&g.page))', WINDOW),
    ('a-frame-is-due-while-a-rebuild-is-owed', YONDER,
     '    if (!s_animations || g.covered || g.page.tree == NULL || page_rebuild_owed(&g.page))',
     '    if (!s_animations || g.covered || g.page.tree == NULL)', WINDOW),
    ('pages-are-never-told-to-reduce', YONDER,
     '                        .dark = s_dark, .reduced_motion = s_reduce_motion};',
     '                        .dark = s_dark};', WINDOW),
    ('the-cascade-is-kept-across-reduce', YONDER,
     '                                                  garb_cascade_env(cascade).reduced_motion !=\n'
     '                                                      view.reduced_motion))) {',
     '                                                  false))) {', WINDOW),
    # libgarb: @keyframes at any scale, and Animations 2's duration keyword.
    ('keyframes-group-across-easings', CASCADE,
     '            while (j < n && at[j].offset == at[i].offset && easing_cmp(at[j].easing, at[i].easing) == 0)',
     '            while (j < n && at[j].offset == at[i].offset)', GARB),
    ('a-tie-at-one-offset-goes-later-first', CASCADE,
     '    return (x->order > y->order) - (x->order < y->order);\n}\n\n// A rule\'s keyframes',
     '    return (x->order < y->order) - (x->order > y->order);\n}\n\n// A rule\'s keyframes', GARB),
    ('keyframes-sort-one-at-a-time', CASCADE,
     '        merge_sort(at, at + n, n, sizeof(*at), key_at_cmp);',
     '        for (int32_t q = 1; q < n; q++) {\n'
     '            KeyAt x = at[q];\n'
     '            int32_t j = q - 1;\n'
     '            while (j >= 0 && key_at_cmp(&at[j], &x) > 0) {\n'
     '                at[j + 1] = at[j];\n'
     '                j--;\n'
     '            }\n'
     '            at[j + 1] = x;\n'
     '        }', GARB),
    ('the-shorthand-auto-is-a-name', PROPS,
     '        } else if (!have[A_DURATION] && vc_keyword(c, kAuto) != NULL) {',
     '        } else if (false && vc_keyword(c, kAuto) != NULL) {', GARB),
    # libflow: a failed layout's timeline, an inline element's pieces, and
    # what float, position, overflow and currentColor make of each other
    # across animations.
    ('a-same-value-is-moving', MOTION,
     '    bool still = (own || !f_props_has(&d->failed, p)) && same_value(p, s->style, e->style);',
     '    bool still = false;\n    (void)same_value;', LIBFLOW),
    ('transforms-are-compared-by-pointer', MOTION,
     '    for (int i = 0; i < 16; i++)\n        if (a->m[i] != b->m[i])\n            return false;\n    return true;',
     '    for (int i = 0; i < 16; i++)\n        if (a->m[i] != b->m[i])\n            return false;\n    return a == b;', LIBFLOW),
    ('shadows-are-compared-by-pointer', MOTION,
     '    if (na != nb)\n        return false;\n    for (int32_t i = 0; i < na; i++)\n        if (a[i].x != b[i].x',
     '    if (na != nb || a != b)\n        return false;\n    for (int32_t i = 0; i < na; i++)\n        if (a[i].x != b[i].x', LIBFLOW),
    ('a-still-interval-skips-its-far-end', MOTION,
     '    d->next = fmin(d->next, edge_due(k, &an->timing, an->record->start,\n'
     '                                     k->forwards ? e->offset : s->offset, d->now));\n',
     '    (void)edge_due;\n', LIBFLOW),
    ('near-quaternions-are-the-start', MOTION,
     '    if (s < 1e-5) {\n        double len = 0;',
     '    if (s < 1e-5) {\n        os64_memcpy(out, a, 4 * sizeof(double));\n        return;\n        double len = 0;', LIBFLOW),
    ('a-keyframe-url-is-the-documents', STYLE,
     'kf[j].sets, NULL, kf[j].sheet, kf[j].nsets, &important)',
     'kf[j].sets, NULL, -1, kf[j].nsets, &important)', LIBFLOW),
    ('a-keyframe-forgets-its-sheet', CASCADE,
     '                                      .nsets = kept != NULL ? np : 0, .sheet = k->sheet};',
     '                                      .nsets = kept != NULL ? np : 0, .sheet = -1};', LIBFLOW),
    ('a-failed-layout-keeps-its-timeline', STYLE,
     '    os64_free(styles->animated);\n    flow_timeline_free(styles->timeline);',
     '    os64_free(styles->animated);', LIBFLOW),
    ('no-room-to-end-is-ignored', FLOW,
     '    if (!f_timeline_end(tree->styles->timeline, !tree->incomplete)) {\n        flow_free(tree);\n        return NULL;\n    }',
     '    (void)f_timeline_end(tree->styles->timeline, !tree->incomplete);', LIBFLOW),
    ('a-commit-is-skipped', FLOW,
     '    flow_timeline_free(*shown);\n    *shown = tree->styles->timeline;\n    tree->styles->timeline = NULL;',
     '    (void)shown;', LIBFLOW),
    ('a-commit-keeps-the-tree-owning-it', FLOW,
     '    *shown = tree->styles->timeline;\n    tree->styles->timeline = NULL;',
     '    *shown = tree->styles->timeline;', LIBFLOW),
    ('a-freed-timeline-keeps-its-holds', MOTION,
     '        os64_html_release(t->doc, t->records[i]->node);\n',
     '', LIBFLOW),
    ('a-record-takes-no-hold', MOTION,
     '    os64_html_hold(t->doc, n);\n    t->records[t->n++] = r;',
     '    t->records[t->n++] = r;', LIBFLOW),
    ('an-inline-element-is-its-first-piece', FLOW,
     '    if (b->kind == FLOW_BOX_SPAN)\n        while',
     '    if (false)\n        while', LIBFLOW),
    ('an-inline-element-is-its-anonymous-block', FLOW,
     '        while (b->parent != NULL && !(block_level(b->kind) && b->node != NULL))',
     '        while (b->parent != NULL && !block_level(b->kind))', LIBFLOW),
    ('the-base-is-unsettled', STYLE,
     '        m->base.border_width[side] = own_width(c, &base, st, none, side);\n    for (int32_t i = 0; i < count; i++) {',
     '        m->base.border_width[side] = own_width(c, &base, st, none, side);\n    unsettled(&m->base, &sp->s);\n'
     '    for (int32_t i = 0; i < count; i++) {', LIBFLOW),
    ('a-keyframe-is-settled-alone', STYLE,
     '            unsettled(&k.s, &specified);',
     '            (void)specified;\n            (void)unsettled;', LIBFLOW),
    ('the-mix-is-not-settled', STYLE,
     '    if (lists.no_memory)\n        return false;\n    settle_box(c, &s, item, root);',
     '    if (lists.no_memory)\n        return false;', LIBFLOW),
    ('a-current-shadow-is-resolved', MOTION,
     '    r.current = a->current && b->current;',
     '    r.current = false;', LIBFLOW),
    ('a-current-colour-is-resolved', MOTION,
     '    if (a->current_colours & b->current_colours & bit)\n        d->current_colours |= bit;',
     '    (void)a;\n    (void)b;\n    if (false)\n        d->current_colours |= bit;', LIBFLOW),
    # libflow: a side's own width.
    ('a-side-with-no-width-has-none', STYLE,
     '        return sp->border_px[side];\n    return css(c, 3);',
     '        return sp->border_px[side];\n    return css(c, 3) * 0;', LIBFLOW),
    ('a-keyframe-width-is-not-its-own', STYLE,
     '    if (sp->border_px[side] != 0 || author_declares(st, w) || author_declares(sets, w))',
     '    (void)sets;\n    if (sp->border_px[side] != 0 || author_declares(st, w))', LIBFLOW),
    # libflow: matrices a list multiplies out past a double, a list dropped
    # for the budget, and blocks that ease as the element does.
    ('a-row-length-is-not-scaled', MOTION,
     '    double a = x / k, b = y / k;\n    return k * sqrt(a * a + b * b);',
     '    (void)k;\n    return sqrt(x * x + y * y);', LIBFLOW),
    ('a-non-finite-composition-is-kept', MOTION,
     '    for (int i = 0; i < 16; i++)\n        if (!isfinite(out[i]))\n            return false;\n    return true;',
     '    return true;', LIBFLOW),
    ('two-units-mix-in-the-first', MOTION,
     '    return a == b ? a : FLOW_ANGLE_DEG;',
     '    (void)b;\n    return a;', LIBFLOW),
    ('a-dropped-list-is-walked', MOTION,
     '                if (at != pass || !f_props_has(&an->props, (garb_prop_t)p) || !animatable((garb_prop_t)p) ||\n'
     '                    f_props_has(&an->dropped, (garb_prop_t)p))',
     '                if (at != pass || !f_props_has(&an->props, (garb_prop_t)p) || !animatable((garb_prop_t)p))',
     LIBFLOW),
    ('the-style-names-nothing-dropped', STYLE,
     '    (void)f_motion_style(m, now, &s, motion_alloc, &lists, NULL, true);',
     '    (void)f_motion_style(m, now, &s, motion_alloc, &lists, NULL, false);', LIBFLOW),
    ('the-own-easing-joins-nothing', STYLE,
     '    if (joins == 0)\n        return true;\n    garb_keyframe_t *out',
     '    if (joins >= 0)\n        return true;\n    garb_keyframe_t *out', LIBFLOW),
    ('joined-blocks-keep-the-first-written', STYLE,
     '                    } else if (at <= written[p]) {',
     '                    } else {', LIBFLOW),
    ('garb-forgets-where-it-wrote', CASCADE,
     '                    written[p] = (uint64_t)(uint32_t)at[i].order << 32 | (uint32_t)d;',
     '                    written[p] = 0;\n                    (void)d;', LIBFLOW),
    # libflow: what is drawn moving, a 3D determinant past a double, an
    # axis of no length.
    ('a-replaced-value-is-moving', MOTION,
     '    if (!active || d->last[p] > a)\n        return true;',
     '    if (!active)\n        return true;', LIBFLOW),
    ('the-last-animation-is-the-first', MOTION,
     '            if (last[p] >= 0 || !f_props_has(&an->props, (garb_prop_t)p) ||',
     '            if (!f_props_has(&an->props, (garb_prop_t)p) ||', LIBFLOW),
    ('an-ended-animation-still-replaces', MOTION,
     '        if (!k.in_effect)\n            continue;\n        for (int p = 0; p < GARB_NPROPS; p++) {\n            if (last[p] >= 0',
     '        (void)k.in_effect;\n        for (int p = 0; p < GARB_NPROPS; p++) {\n            if (last[p] >= 0', LIBFLOW),
    # The middle ground (A2): the frame rate while an animation runs, two
    # equal ends held still to their far end, and what rests on an unmade
    # value moving with it, until two ends of an animation's own replace it.
    ('equal-ends-do-not-hold', MOTION,
     '    bool still = (own || !f_props_has(&d->failed, p)) && same_value(p, s->style, e->style);',
     '    bool still = (own || !f_props_has(&d->failed, p)) && false && same_value(p, s->style, e->style);', LIBFLOW),
    ('a-replaced-value-is-still-unmade', MOTION,
     '    if (own)\n        f_props_remove(&d->failed, p);',
     '', LIBFLOW),
    ('a-paused-animation-runs', MOTION,
     '    bool active = k->phase == PHASE_ACTIVE && !an->record->paused;',
     '    bool active = k->phase == PHASE_ACTIVE;', LIBFLOW),
    ('blends-of-two-fractions-are-one', MOTION,
     '    if (x->t != y->t || x->nfrom != y->nfrom || x->nto != y->nto)',
     '    if (x->nfrom != y->nfrom || x->nto != y->nto)', LIBFLOW),
    ('blends-compare-their-fractions-alone', MOTION,
     '    for (int32_t i = 0; i < x->nfrom; i++)\n        if (!function_same(&x->from[i], &y->from[i]))\n            return false;',
     '', LIBFLOW),
    ('a-blend-is-never-the-same', MOTION,
     '        return a->fn == b->fn && a->blend != NULL && b->blend != NULL && blend_same(a, b);',
     '        return false && blend_same(a, b);', LIBFLOW),
    ('a-3d-determinant-past-a-double-is-refused', MOTION,
     '    double dp = det4(pm);\n    if (!isfinite(dp)) {',
     '    double dp = det4(pm);\n    if (!isfinite(dp))\n        return false;\n    if (!isfinite(dp)) {', LIBFLOW),
    ('a-zero-axis-keeps-its-angle', MOTION,
     '        double ta = has_axis(a) ? a->a[3] : 0, tb = has_axis(b) ? b->a[3] : 0;',
     '        double ta = a->a[3], tb = b->a[3];\n        (void)has_axis;', LIBFLOW),
    # libflow: the curve solved to the last bit, a crossing still due after
    # a late layout, currentColor's used value, and what a box paints.
    ('the-easing-stops-at-a-near-x', MOTION,
     '        if (!(mid > lo && mid < hi))\n            break;',
     '        if (!(mid > lo && mid < hi) || fabs(bezier(p[0], p[2], mid) - x) <= 1e-7)\n            break;', LIBFLOW),
    ('the-easing-ends-are-halved', MOTION,
     '    if (x <= 0)\n        return 0;\n    if (x >= 1)\n        return 1;\n    double lo = 0, hi = 1;',
     '    double lo = 0, hi = 1;', LIBFLOW),
    ('colour-is-composed-in-order', MOTION,
     '                int at = p == GARB_COLOR ? 0 : width ? 2 : 1;',
     '                int at = width ? 2 : 1;', LIBFLOW),
    ('a-shadow-mixes-the-keyframe-colour', MOTION,
     '        if (!mix_shadows(sa, na, sb, nb, d->color, d->color, t, &list, &n, alloc, ctx))',
     '        if (!mix_shadows(sa, na, sb, nb, a->color, b->color, t, &list, &n, alloc, ctx))', LIBFLOW),
    ('a-colour-end-is-the-keyframes', MOTION,
     '    return (s->current_colours & bit) && d != NULL ? d->color : own;',
     '    (void)s;\n    (void)bit;\n    (void)d;\n    return own;', LIBFLOW),
    ('ink-is-not-seen', FLOW,
     '        rect_from((int64_t)o.x - b->ink[FLOW_LEFT], (int64_t)o.y - b->ink[FLOW_TOP],\n'
     '                  (int64_t)o.x + o.w + b->ink[FLOW_RIGHT], (int64_t)o.y + o.h + b->ink[FLOW_BOTTOM]);',
     '        rect_from((int64_t)o.x, (int64_t)o.y, (int64_t)o.x + o.w, (int64_t)o.y + o.h);', LIBFLOW),
    # Round 9: what one animation dropped, a time past a double, easings
    # grouped by what they compute to, a decomposition a double cannot
    # hold, and a settings window's first face.
    ('a-dropped-list-replaces-the-earlier', MOTION,
     '            if (last[p] >= 0 || !f_props_has(&an->props, (garb_prop_t)p) ||\n'
     '                f_props_has(&an->dropped, (garb_prop_t)p))',
     '            if (last[p] >= 0 || !f_props_has(&an->props, (garb_prop_t)p))', LIBFLOW),
    # The input domain (values.h): one break for each clamp.
    ('a-time-is-not-held', VALUES,
     '    out->number = garb_hold(t->number, out->unit == GARB_U_S ? GARB_TIME_MS_MAX / 1000\n'
     '                                                             : GARB_TIME_MS_MAX);',
     '    out->number = t->number;', GARB),
    ('seconds-are-held-as-milliseconds', VALUES,
     '    out->number = garb_hold(t->number, out->unit == GARB_U_S ? GARB_TIME_MS_MAX / 1000\n'
     '                                                             : GARB_TIME_MS_MAX);',
     '    out->number = garb_hold(t->number, GARB_TIME_MS_MAX);', GARB),
    ('an-iteration-count-is-not-held', PROPS,
     '    out->number = garb_hold(out->number, GARB_ITERATIONS_MAX);  // the domain (values.h)\n',
     '', GARB),
    ('a-calc-leaf-is-not-held', STYLE,
     '            *out = (Lp){garb_hold(k->leaf.number, GARB_TRANSFORM_NUMBER_MAX), 0, true};',
     '            *out = (Lp){k->leaf.number, 0, true};', LIBFLOW),
    ('a-settled-leaf-is-not-held', PROPS,
     '        *v = garb_hold(k->leaf.number, *number ? GARB_TRANSFORM_NUMBER_MAX\n'
     '                                               : GARB_TRANSFORM_NUMBER_MAX * 100);',
     '        *v = k->leaf.number;', GARB),
    ('a-settled-number-is-not-held', PROPS,
     '    *out = garb_hold(*out, GARB_TRANSFORM_NUMBER_MAX);\n    return true;\n}',
     '    return true;\n}', GARB),
    ('a-nan-is-kept', VALUES_H,
     '    return v > limit ? limit : v < -limit ? -limit : v == v ? v : 0;',
     '    return v > limit ? limit : v < -limit ? -limit : v;', LIBFLOW),
    ('a-length-nan-is-cast', STYLE,
     '    if (!(v == v))\n        return 0;\n    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);',
     '    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);', LIBFLOW),
    ('a-subnormal-shift-in-one-power', FLOW,
     '    long double by = power_of_two(-shift / 2), by2 = power_of_two(-shift + shift / 2);',
     '    long double by = power_of_two(-shift), by2 = 1;', LIBFLOW),
    ('a-2d-nan-determinant-is-compared', MOTION,
     '    if (!isfinite(det)) {\n        double r0 = len2(m11, m12), r1 = len2(m21, m22);',
     '    if (false) {\n        double r0 = len2(m11, m12), r1 = len2(m21, m22);', LIBFLOW),
    ('a-3d-nan-determinant-is-compared', MOTION,
     '    double dp = det4(pm);\n    if (!isfinite(dp)) {',
     '    double dp = det4(pm);\n    if (false) {', LIBFLOW),
    ('the-first-y-is-not-held', PROPS,
     '        a[1].number = garb_hold(a[1].number, GARB_EASING_Y_MAX);\n',
     '', GARB),
    ('the-second-y-is-not-held', PROPS,
     '        a[3].number = garb_hold(a[3].number, GARB_EASING_Y_MAX);\n',
     '', GARB),
    ('a-transform-number-is-not-held', STYLE,
     '    *out = garb_hold(*out, GARB_TRANSFORM_NUMBER_MAX);\n',
     '', LIBFLOW),
    ('an-angle-is-not-held', STYLE,
     '    *out = garb_hold(v->number, f_angle_in_unit(GARB_ANGLE_DEG_MAX, FLOW_ANGLE_DEG, *unit));',
     '    *out = v->number;', LIBFLOW),
    ('an-angle-is-held-in-its-own-number', STYLE,
     '    *out = garb_hold(v->number, f_angle_in_unit(GARB_ANGLE_DEG_MAX, FLOW_ANGLE_DEG, *unit));',
     '    *out = garb_hold(v->number, GARB_ANGLE_DEG_MAX);', LIBFLOW),
    ('easings-compare-as-printed', CASCADE,
     '            e.n[i] = v->items[i].number;',
     '            e.n[i] = (double)(int64_t)(v->items[i].number * 1e6);', GARB),
    ('jump-end-is-not-end', CASCADE,
     '        if (os64_streq(e.jump, "jump-start") || os64_streq(e.jump, "jump-end"))\n            e.jump += 5;',
     '', GARB),
    ('bare-steps-jump-nowhere', CASCADE,
     '        e.jump = v->nitems > 1 && v->items[1].keyword != NULL ? v->items[1].keyword : "end";',
     '        e.jump = v->nitems > 1 && v->items[1].keyword != NULL ? v->items[1].keyword : "";', GARB),
    ('a-step-keyword-is-not-steps', CASCADE,
     '    if (v->kind == GARB_V_KEYWORD && (os64_streq(e.word, "step-start") || os64_streq(e.word, "step-end"))) {',
     '    if (false) {', GARB),
    ('a-keyword-curve-is-its-points', STYLE,
     '        return a->named == b->named && a->p[0] == b->p[0]',
     '        return a->p[0] == b->p[0]', LIBFLOW),
    ('the-initial-easing-is-unnamed', MOTION,
     '                              : (flow_easing_t){FLOW_EASE_CUBIC, {0.25, 0.1, 0.25, 1}, 0, 0, true};',
     '                              : (flow_easing_t){FLOW_EASE_CUBIC, {0.25, 0.1, 0.25, 1}, 0, 0, false};', LIBFLOW),
    ('a-2d-length-past-a-double-is-held', MOTION,
     '    if (!isfinite(s0) || !isfinite(s1) || !isfinite(sk))\n        return false;\n'
     '    d->sc[0] = s0;\n    d->sc[1] = s1;\n    d->sk[0] = sk;',
     '    d->sc[0] = fmin(fmax(s0, -DBL_MAX), DBL_MAX);\n    d->sc[1] = fmin(fmax(s1, -DBL_MAX), DBL_MAX);\n'
     '    d->sk[0] = fmin(fmax(sk, -DBL_MAX), DBL_MAX);', LIBFLOW),
    ('an-infinity-is-divided-down', FLOW,
     '        big = a - a == 0 && a > big ? a : big;',
     '        big = a > big ? a : big;', LIBFLOW),
    ('an-inverse-entry-past-a-double-is-held', MOTION,
     '            if (!isfinite(q))\n                return false;\n            out[col * 4 + row] = q;',
     '            out[col * 4 + row] = fmin(fmax(q, -DBL_MAX), DBL_MAX);', LIBFLOW),
    ('a-new-window-refuses-its-face', UI_SETTINGS,
     '    if(d->opening)grow(d);',
     '', UI_TEXT),
    ('a-live-window-grows-for-a-face', UI_SETTINGS,
     '    if(d->opening)grow(d);',
     '    grow(d);', UI_TEXT),
    # Round 10: animations of nothing that can show, and the canvas's
    # background.
    ('an-animation-of-nothing-is-due', MOTION,
     '        if (an->record == NULL || an->record->paused || !anim_shows(an, a, last, only))',
     '        if (an->record == NULL || an->record->paused || (false && !anim_shows(an, a, last, only)))', LIBFLOW),
    ('a-replaced-animation-shows', MOTION,
     '        if (f_props_has(&an->props, (garb_prop_t)p) && !(last[p] > a) &&',
     '        if (f_props_has(&an->props, (garb_prop_t)p) && (true || !(last[p] > a)) &&', LIBFLOW),
    ('a-dropped-side-effect-is-not-due', MOTION,
     '             will_bit((garb_prop_t)p) != 0))',
     '             false))', LIBFLOW),
    ('a-dropped-list-shows', MOTION,
     '            ((animatable((garb_prop_t)p) && !f_props_has(&an->dropped, (garb_prop_t)p)) ||',
     '            (animatable((garb_prop_t)p) ||', LIBFLOW),
    ('an-opacity-animation-has-no-side-effect', MOTION,
     '           : p == GARB_OPACITY ? FLOW_WILL_OPACITY : 0;',
     '           : 0;', LIBFLOW),
    ('the-canvas-is-its-owners-box', FLOW,
     '            (st->motion->in_place && !may_own_canvas(tree, n) && !motion_seen(tree, n, view, scroll)))',
     '            (st->motion->in_place && (true || !may_own_canvas(tree, n)) && !motion_seen(tree, n, view, scroll)))',
     LIBFLOW),
    ('only-the-root-owns-the-canvas', FLOW,
     '           (n == root || (n->parent == root && n->kind == OS64_HTML_ELEMENT && n->tag == OS64_HTML_TAG_BODY));',
     '           n == root;', LIBFLOW),
    # Round 11: the value underneath, a curve's turns and crossings to the
    # last bit, and a skew's unit.
    ('an-implicit-frame-is-the-base', MOTION,
     '                interval(an, &under, (garb_prop_t)p, k.progress, &s, &e);',
     '                (void)under;\n                interval(an, &m->base, (garb_prop_t)p, k.progress, &s, &e);', LIBFLOW),
    ('an-implicit-interval-replaces', MOTION,
     '            if (!s.implicit && !e.implicit)\n                last[p] = a;',
     '            last[p] = a;', LIBFLOW),
    ('an-unknown-underlying-is-still', MOTION,
     '    bool still = (own || !f_props_has(&d->failed, p)) && same_value(p, s->style, e->style);',
     '    bool still = same_value(p, s->style, e->style);', LIBFLOW),
    # Round 12: a blend under a blend; and the timeline built and swapped —
    # a layout discarded unshown, a tree shown, a staged one published.
    ('a-nested-blend-is-its-placeholder', FLOW,
     '        if (f[i].fn == FLOW_FN_BLEND && f[i].blend != NULL) {',
     '        if (false && f[i].fn == FLOW_FN_BLEND && f[i].blend != NULL) {', LIBFLOW),
    ('blends-nest-without-end', MOTION,
     '        if (depth > F_BLEND_DEPTH_MAX)\n            return false;',
     '', LIBFLOW),
    ('a-discarded-layout-is-shown', YONDER,
     '        flow_free(fresh);\n        flow_free(reclaim->tree);',
     '        flow_timeline_commit(&p->timeline, fresh);\n        flow_free(fresh);\n        flow_free(reclaim->tree);', WINDOW),
    ('a-shown-tree-keeps-the-old-timeline', YONDER,
     '    p->tree = tree;\n    flow_timeline_commit(&p->timeline, tree);',
     '    p->tree = tree;', WINDOW),
    ('a-staged-tree-commits-the-borrowed-timeline', YONDER,
     '    if (reclaim != NULL && reclaim != p) {\n        flow_free(p->tree);\n        p->tree = fresh;\n    } else {\n        page_show(p, fresh);\n    }',
     '    page_show(p, fresh);', WINDOW),
    ('a-published-rebuild-keeps-the-old-timeline', YONDER,
     '        page_show(p, next.tree);',
     '        flow_free(p->tree);\n        p->tree = next.tree;', WINDOW),
    ('a-skew-keeps-its-first-unit', STYLE,
     '            to = f_angle_common(out->unit, second);',
     '            to = out->unit;', LIBFLOW),
    ('the-switches-are-not-applied', SETTINGS,
     '    s.motion_use(s.run.checked, s.reduce.checked);',
     '', WINDOW),
    ('animations-default-off', SETTINGS,
     '    return !(os64_conf_get(CONF_NAME, CONF_ANIMATIONS, value, sizeof(value)) == 0 &&\n'
     '             os64_streq_nocase(value, "off"));',
     '    return os64_conf_get(CONF_NAME, CONF_ANIMATIONS, value, sizeof(value)) == 0 &&\n'
     '           os64_streq_nocase(value, "on");', WINDOW),
    ('reduce-is-not-saved', SETTINGS,
     '                                          s.reduce.checked ? "reduce" : "no-preference") == 0 &&',
     '                                          "no-preference") == 0 &&', WINDOW),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, cases in mutants:
    if args.only and not any(o in name for o in args.only.split(',')):
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
                result = subprocess.run(['bash', '-c', cases], cwd=root, stdout=out, stderr=out,
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

print(f'\nmotion mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
