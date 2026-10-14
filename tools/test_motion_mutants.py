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
measured layout copies are never shown, and have none); a crossing whose
x rounds to an end of its interval dropped, which the schedule's own look
at the next millisecond places just as early; visibility's start read at
0 inclusively, where only a curve's first point is 0 itself and that look
places it too; and the probe's two looks given one set of lists between
them, where a list the second has no room for is not asked about — no
case is known of a list's change only the probe sees (300,000 fuzzed
cases with the probe off miss nothing but visibility, which is no list).
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
     '    d->sk[0] = (double)sk;',
     '    d->sk[0] = 0;', LIBFLOW),
    ('a-2d-flip-is-lost', MOTION,
     '    if (det < 0 && m11 < m22)\n        s0 = -s0;',
     '    if (false && m11 < m22)\n        s0 = -s0;', LIBFLOW),
    # libflow: values mixed.
    ('colours-mix-unpremultiplied', MOTION,
     '        double ca = ((a >> shift) & 0xFF) * aa, cb = ((b >> shift) & 0xFF) * ab;\n        double c = lerp(ca, cb, t) / alpha;',
     '        double ca = ((a >> shift) & 0xFF), cb = ((b >> shift) & 0xFF);\n        double c = lerp(ca, cb, t);', LIBFLOW),
    ('integers-round-down', MOTION,
     '    return round_i32(floor(v + 0.5));',
     '    return round_i32(floor(v));', LIBFLOW),
    ('a-discrete-value-flips-at-the-start', MOTION,
     '        copy_prop(p, d, t < 0.5 ? a : b);',
     '        copy_prop(p, d, t <= 0 ? a : b);', LIBFLOW),
    ('visibility-flips-half-way', MOTION,
     '            d->visibility = t <= 0 ? a->visibility : t >= 1 ? b->visibility : FLOW_VISIBLE;',
     '            d->visibility = t < 0.5 ? a->visibility : b->visibility;', LIBFLOW),
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
    ('a-step-edge-is-not-due', MOTION,
     '        if (j >= 1 && j <= (k->forwards ? n - 1 : n))\n            d->next = fmin(d->next, edge_due(k, &an->timing, base, s->offset + span * j / n, now));',
     '        (void)j;', LIBFLOW),
    ('a-reversed-top-step-is-not-due', MOTION,
     '        if (j >= 1 && j <= (k->forwards ? n - 1 : n))',
     '        if (j >= 1 && j <= n - 1)', LIBFLOW),
    ('a-crossing-is-not-due', MOTION,
     '                if (k->forwards ? xs[j] >= here : xs[j] <= here)\n'
     '                    d->next = fmin(d->next, edge_due(k, &an->timing, base, s->offset + span * xs[j], now));',
     '                (void)xs;', LIBFLOW),
    ('a-change-shows-only-the-millisecond-after', MOTION,
     '    return first > now ? first : after > now ? floor(now) + 1 : INFINITY;',
     '    return first > now ? floor(at + tol) + 1 : after > now ? floor(now) + 1 : INFINITY;', LIBFLOW),
    ('a-change-never-falls-past-its-millisecond', MOTION,
     '    return first > now ? first : after > now ? floor(now) + 1 : INFINITY;',
     '    (void)after;\n    return first > now ? first : INFINITY;', LIBFLOW),
    ('an-edge-behind-is-due', MOTION,
     '    d->next = fmin(d->next, edge_due(k, &an->timing, base, k->forwards ? e->offset : s->offset, now));\n    if (still)',
     '    d->next = fmin(d->next, edge_due(k, &an->timing, base, s->offset, now));\n'
     '    d->next = fmin(d->next, edge_due(k, &an->timing, base, e->offset, now));\n    if (still)', LIBFLOW),
    ('a-mix-cancels-at-a-huge-fraction', MOTION,
     '    if (t == 1)\n        return b;\n    return held(a + ((long double)b - a) * t);',
     '    return held((1 - (long double)t) * a + (long double)t * b);', LIBFLOW),
    ('an-easing-overflows', MOTION,
     '    long double u = 1 - (long double)t, v = t;\n    return held(3 * u * u * v * p1 + 3 * u * v * v * p2 + v * v * v);',
     '    return bezier(p1, p2, t);', LIBFLOW),
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
    ('a-step-edge-is-skipped', MOTION,
     '        double j = k->forwards ? floor(x) + 1 : floor(x);',
     '        double j = k->forwards ? floor(x) + 2 : floor(x);', LIBFLOW),
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
    ('only-the-first-crossing-is-found', MOTION,
     '    for (int32_t i = 0; i + 1 < nc && n < 3; i++) {',
     '    for (int32_t i = 0; i + 1 < nc && n < 1; i++) {', LIBFLOW),
    ('visibility-crosses-at-one-half', MOTION,
     '        for (int l = 0; l < nl; l++) {',
     '        for (int l = 0; l < (vis ? 0 : nl); l++) {', LIBFLOW),
    ('z-index-from-auto-stays-auto', MOTION,
     '            d->has_z_index = true;\n',
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
     '            next = fmin(next, base + k.before_end);\n            continue;',
     '            continue;', LIBFLOW),
    ('a-paused-animation-is-due', MOTION,
     '        if (an->record == NULL || an->record->paused || !anim_shows(an, a, last))\n            continue;\n'
     '        double base = an->record->start;',
     '        if (an->record == NULL || !anim_shows(an, a, last))\n            continue;\n'
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
     '    uint64_t cost_us = g.motion_laid ? g.motion_us : g.laid_ms * 1000;',
     '    uint64_t cost_us = g.motion_laid ? g.motion_us : 0;', WINDOW),
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
     '    g.motion_ended = yonder_now_ms();\n    g.motion_us = (uint64_t)(os64_micros() - g.motion_began_us);\n}',
     '}', WINDOW),
    ('a-paint-without-a-frame-costs', YONDER,
     '    if (!g.motion_painting)\n        return;\n    g.motion_painting = false;',
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
    # libflow: a curve no double holds, a failed layout's timeline, an inline
    # element's pieces, and what float, position, overflow and currentColor
    # make of each other across animations.
    ('crossing-coefficients-overflow-a-double', MOTION,
     '    long double lc = 3.0L * p[1], lb = 3.0L * ((long double)p[3] - p[1]) - lc, la = 1 - lc - lb;\n'
     '    long double la3 = 3 * la, lb2 = 2 * lb;\n'
     '    long double big = la3 < 0 ? -la3 : la3, mb = lb2 < 0 ? -lb2 : lb2, mc = lc < 0 ? -lc : lc;\n'
     '    big = mb > big ? mb : big;\n'
     '    big = mc > big ? mc : big;\n'
     '    long double qa = la3 / big, qb = lb2 / big, qc = lc / big;',
     '    double c = 3 * p[1], b = 3 * (p[3] - p[1]) - c, a = 1 - c - b;\n'
     '    double qa = 3 * a, qb = 2 * b, qc = c;', LIBFLOW),
    ('a-same-value-is-moving', MOTION,
     '    bool still = !unknown && same_value(p, s->style, e->style);',
     '    bool still = false;\n    (void)same_value;\n    (void)unknown;', LIBFLOW),
    ('transforms-are-compared-by-pointer', MOTION,
     '    for (int i = 0; i < 16; i++)\n        if (a->m[i] != b->m[i])\n            return false;\n    return true;',
     '    for (int i = 0; i < 16; i++)\n        if (a->m[i] != b->m[i])\n            return false;\n    return a == b;', LIBFLOW),
    ('shadows-are-compared-by-pointer', MOTION,
     '    if (na != nb)\n        return false;\n    for (int32_t i = 0; i < na; i++)\n        if (a[i].x != b[i].x',
     '    if (na != nb || a != b)\n        return false;\n    for (int32_t i = 0; i < na; i++)\n        if (a[i].x != b[i].x', LIBFLOW),
    ('a-still-interval-skips-its-far-end', MOTION,
     '    d->next = fmin(d->next, edge_due(k, &an->timing, base, k->forwards ? e->offset : s->offset, now));\n'
     '    if (still)\n        return true;',
     '    if (still)\n        return true;\n'
     '    d->next = fmin(d->next, edge_due(k, &an->timing, base, k->forwards ? e->offset : s->offset, now));', LIBFLOW),
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
    # libflow: what the schedule against the value found, and a side's
    # own width.
    ('the-curve-is-cubed-by-powers', MOTION,
     '    long double u = 1 - (long double)t, v = t;\n'
     '    return held(3 * u * u * v * p1 + 3 * u * v * v * p2 + v * v * v);',
     '    long double c = 3.0L * p1, b = 3.0L * ((long double)p2 - p1) - c, a = 1 - c - b;\n'
     '    return held(((a * t + b) * t + c) * t);', LIBFLOW),
    ('a-change-at-now-is-shown', MOTION,
     '    double first = ceil(at - tol), after = floor(at + tol) + 1;',
     '    double first = ceil(at - tol), after = floor(at) + 1;', LIBFLOW),
    ('visibility-reads-both-levels', MOTION,
     '        if (vis && s->style->visibility != FLOW_VISIBLE) {',
     '        if (vis) {', LIBFLOW),
    ('a-crossing-is-read-from-the-curve', MOTION,
     '            if (past_level(bezier_y(p[1], p[3], mid), level, strict) == past[i])',
     '            if ((bezier_y(p[1], p[3], mid) > level) == past[i])', LIBFLOW),
    ('the-next-millisecond-is-not-asked', MOTION,
     '    if (next > then && changes_by(m, now, then))',
     '    if (false && changes_by(m, now, then))', LIBFLOW),
    ('a-side-with-no-width-has-none', STYLE,
     '        return sp->border_px[side];\n    return css(c, 3);',
     '        return sp->border_px[side];\n    return css(c, 3) * 0;', LIBFLOW),
    ('a-keyframe-width-is-not-its-own', STYLE,
     '    if (sp->border_px[side] != 0 || author_declares(st, w) || author_declares(sets, w))',
     '    (void)sets;\n    if (sp->border_px[side] != 0 || author_declares(st, w))', LIBFLOW),
    # libflow: finite inputs whose arithmetic overflows, a list dropped for
    # the budget, and blocks that ease as the element does.
    ('a-row-length-is-not-scaled', MOTION,
     '    double a = (double)(x / k), b = (double)(y / k);\n    return k * (long double)sqrt(a * a + b * b);',
     '    (void)k;\n    double a = (double)x, b = (double)y;\n    return (long double)sqrt(a * a + b * b);', LIBFLOW),
    ('a-2d-determinant-in-doubles', MOTION,
     '    long double det = m11 * m22 - m12 * m21;\n    if (det == 0)\n        return false;           // cannot be undone',
     '    long double det = (double)m11 * (double)m22 - (double)m12 * (double)m21;\n    if (det == 0)\n        return false;           // cannot be undone', LIBFLOW),
    ('a-non-finite-composition-is-kept', MOTION,
     '    for (int i = 0; i < 16; i++)\n        if (!isfinite(out[i]))\n            return false;\n    return true;',
     '    return true;', LIBFLOW),
    ('axes-are-compared-by-length', MOTION,
     '        if (na[i] != nb[i])\n            return false;\n    os64_memcpy(axis, ua, sizeof(ua));',
     '        if (ua[i] / len3(ua) != ub[i] / len3(ub))\n            return false;\n    os64_memcpy(axis, ua, sizeof(ua));', LIBFLOW),
    ('angles-mix-in-degrees', MOTION,
     '    return per_turn(a) <= per_turn(b) ? a : b;',
     '    (void)a;\n    (void)b;\n    return FLOW_ANGLE_DEG;', LIBFLOW),
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
    # libflow: what is drawn moving, a 3D inverse at the extremes, an axis
    # of no length.
    ('a-replaced-value-is-moving', MOTION,
     '    if (k->phase != PHASE_ACTIVE || an->record->paused || d->last[p] > a)',
     '    if (k->phase != PHASE_ACTIVE || an->record->paused)', LIBFLOW),
    ('the-last-animation-is-the-first', MOTION,
     '            if (last[p] >= 0 || !f_props_has(&an->props, (garb_prop_t)p) ||',
     '            if (!f_props_has(&an->props, (garb_prop_t)p) ||', LIBFLOW),
    ('an-ended-animation-still-replaces', MOTION,
     '        if (!k.in_effect)\n            continue;\n        for (int p = 0; p < GARB_NPROPS; p++) {\n            if (last[p] >= 0',
     '        (void)k.in_effect;\n        for (int p = 0; p < GARB_NPROPS; p++) {\n            if (last[p] >= 0', LIBFLOW),
    ('a-flat-transform-is-moving', MOTION,
     '    return flips ? MIX_DISCRETE : MIX_CONTINUOUS;',
     '    (void)flips;\n    return MIX_CONTINUOUS;', LIBFLOW),
    ('a-blend-is-never-the-same', MOTION,
     '        return a->fn == b->fn && a->blend != NULL && b->blend != NULL && blend_same(a, b);',
     '        return false && blend_same(a, b);', LIBFLOW),
    ('a-3d-determinant-overflows', MOTION,
     '    if (det4(pm) == 0)\n        return false;',
     '    if ((double)det4(pm) == 0 || !isfinite((double)det4(pm)))\n        return false;', LIBFLOW),
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
    ('the-probe-asks-what-it-could-not-mix', MOTION,
     '                !f_props_has(&fa, (garb_prop_t)p) && !f_props_has(&fb, (garb_prop_t)p) &&\n',
     '', LIBFLOW),
    ('a-time-past-a-double-is-infinite', STYLE,
     '    return ms > DBL_MAX ? DBL_MAX : ms < -DBL_MAX ? -DBL_MAX : (double)ms;',
     '    return (double)ms;', LIBFLOW),
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
     '    if (!fits(s0) || !fits(s1) || !fits(sk))\n        return false;\n'
     '    d->sc[0] = (double)s0;\n    d->sc[1] = (double)s1;\n    d->sk[0] = (double)sk;',
     '    d->sc[0] = held(s0);\n    d->sc[1] = held(s1);\n    d->sk[0] = held(sk);', LIBFLOW),
    ('an-inverse-entry-past-a-double-is-held', MOTION,
     '            if (!fits(q))\n                return false;\n            out[col * 4 + row] = (double)q;',
     '            out[col * 4 + row] = held(q);', LIBFLOW),
    ('a-new-window-refuses-its-face', UI_SETTINGS,
     '    if(d->opening)grow(d);',
     '', UI_TEXT),
    ('a-live-window-grows-for-a-face', UI_SETTINGS,
     '    if(d->opening)grow(d);',
     '    grow(d);', UI_TEXT),
    # Round 10: animations of nothing that can show, and the canvas's
    # background.
    ('an-animation-of-nothing-is-due', MOTION,
     '        if (an->record == NULL || an->record->paused || !anim_shows(an, a, last))',
     '        if (an->record == NULL || an->record->paused || (false && !anim_shows(an, a, last)))', LIBFLOW),
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
     '                        interval(an, &under[i], (garb_prop_t)p, k[i].progress, &s[i], &e[i]);',
     '                        (void)under, interval(an, &m->base, (garb_prop_t)p, k[i].progress, &s[i], &e[i]);',
     LIBFLOW),
    ('an-implicit-interval-replaces', MOTION,
     '            if (!s.implicit && !e.implicit)\n                last[p] = a;',
     '            last[p] = a;', LIBFLOW),
    ('an-unknown-underlying-is-still', MOTION,
     '    bool still = !unknown && same_value(p, s->style, e->style);',
     '    bool still = same_value(p, s->style, e->style);\n    (void)unknown;', LIBFLOW),
    ('the-turning-roots-cancel', MOTION,
     '        long double sq = sqrt((double)disc), q = -0.5L * (qb + (qb < 0 ? -sq : sq));\n'
     '        double r0 = qa != 0 ? (double)(q / qa) : NAN, r1 = q != 0 ? (double)(qc / q) : NAN;',
     '        long double sq = sqrt((double)disc);\n'
     '        double r0 = qa != 0 ? (double)((-qb - sq) / (2 * qa)) : NAN,\n'
     '               r1 = qa != 0 ? (double)((-qb + sq) / (2 * qa)) : NAN;', LIBFLOW),
    ('crossings-are-halved-a-fixed-count', MOTION,
     '        for (int k = 0; k < 1100; k++) {\n            double mid = t0 + (t1 - t0) / 2;',
     '        for (int k = 0; k < 64; k++) {\n            double mid = t0 + (t1 - t0) / 2;', LIBFLOW),
    ('the-probe-ignores-the-value-underneath', MOTION,
     '    u->moved = true;\n    return false;',
     '    (void)u;\n    return true;', LIBFLOW),
    ('the-clock-tolerance-is-fixed', MOTION,
     '    return fmax(1e-6, 8 * DBL_EPSILON * (fabs(at) + fabs(at - base) + (forwards ? 0 : fabs(dur))));',
     '    (void)at;\n    (void)base;\n    (void)dur;\n    (void)forwards;\n    return 1e-6;', LIBFLOW),
    ('a-backward-run-is-charged-forwards', MOTION,
     '    return fmax(1e-6, 8 * DBL_EPSILON * (fabs(at) + fabs(at - base) + (forwards ? 0 : fabs(dur))));',
     '    return fmax(1e-6, 8 * DBL_EPSILON * (fabs(at) + fabs(at - base) + (forwards || true ? 0 : fabs(dur))));',
     LIBFLOW),
    ('the-window-waits-for-its-end', MOTION,
     '    return first > now ? first : after > now ? floor(now) + 1 : INFINITY;',
     '    return first > now ? first : after > now ? after : INFINITY;', LIBFLOW),
    ('a-crossing-is-placed-past-it', MOTION,
     '        xs[n++] = fmin(fmax(bezier(p[0], p[2], t0), 0), 1);',
     '        xs[n++] = fmin(fmax(bezier(p[0], p[2], t1), 0), 1);', LIBFLOW),
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
    ('skew-is-kept-in-degrees', STYLE,
     '            flow_angle_unit_t second = angle_unit(&v[1]), to = f_angle_coarser(out->unit, second);',
     '            flow_angle_unit_t second = angle_unit(&v[1]), to = FLOW_ANGLE_DEG;', LIBFLOW),
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
