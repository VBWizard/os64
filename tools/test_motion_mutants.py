#!/usr/bin/env python3
"""One deliberate break at a time to the animation model (docs/design/
pending/PILE3.md § The animation model, slice S8a) — libgarb's grammars,
@keyframes and media answer, libflow's easing, clock, timeline, mixing and
side effects, and yonder's layouts for motion and its two switches —
against the cases that state its rules: libgarb's corpus, libflow's
--motion run, and the yonder window's host. A mutant is CAUGHT when they
fail, MISSED when they pass, which means a test is missing and gets
written. A build failure is neither.

The shape is test_cliptext_mutants.py's, with each mutant run against the
one harness that holds its rule: each is applied to the worktree's copy of
its file, the cases are run, and the file is put back from the bytes read
before the change, checked byte for byte, whatever happens. Run it from a
clean tree, and never beside another harness that edits the same tree; the
per-mutant logs are in --logs.

One break is left out because nothing can tell it apart from the code: a
measured layout handed the timeline of the page it copies (the pages a
measured layout copies are never shown, and have none).
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/motion-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
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

GARB = 'bash tools/test_garb_host.sh'
LIBFLOW = 'bash tools/test_libflow_host.sh --motion'
WINDOW = 'bash tools/test_yonder_scripts_host.sh'

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
     '    c->keyframes[at_] = (Keyframes){name, len, blocks, nblocks, pending, NULL, 0};',
     '    if (at_ == c->nkeyframes - 1)\n        c->keyframes[at_] = (Keyframes){name, len, blocks, nblocks, pending, NULL, 0};', GARB),
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
    ('names-match-first-to-last', MOTION,
     '        for (int32_t j = nold - 1; j >= 0 && r == NULL; j--)',
     '        for (int32_t j = 0; j < nold && r == NULL; j++)', LIBFLOW),
    ('a-paused-animation-does-not-hold', MOTION,
     '        if (an->timing.paused && !r->paused) {\n            r->hold = now - r->start;\n            r->paused = true;',
     '        if (an->timing.paused && !r->paused) {\n            r->hold = now - r->start;', LIBFLOW),
    ('a-resumed-animation-starts-over', MOTION,
     '            r->start = now - r->hold;\n            r->paused = false;',
     '            r->start = now;\n            r->paused = false;', LIBFLOW),
    ('an-incomplete-layout-drops-records', MOTION,
     '    if (t == NULL || !complete)\n        return;\n    // The map over',
     '    (void)complete;\n    if (t == NULL)\n        return;\n    // The map over', LIBFLOW),
    ('a-complete-layout-keeps-every-record', MOTION,
     '        if (r->seen == t->layout) {\n            r->next = next[i];',
     '        if (true) {\n            r->next = r->seen == t->layout ? next[i] : NULL;', LIBFLOW),
    ('no-room-for-the-map-drops-every-record', MOTION,
     '    if (!room) {\n        os64_free(next);\n        f_map_free(&map);\n        return;\n    }',
     '    if (!room) {\n        os64_free(next);\n        f_map_free(&map);\n        for (int32_t k = 0; k < t->n; k++)\n'
     '            record_free(t, t->records[k]);\n        t->n = 0;\n        f_map_free(&t->by_node);\n        return;\n    }', LIBFLOW),
    # libflow: transforms interpolated (Chromium's scheme, PILE3.md).
    ('the-turn-is-not-the-shorter-way', MOTION,
     '    if (c < 0) {\n        for (int i = 0; i < 4; i++)\n            from[i] = -from[i];\n        c = -c;\n    }',
     '', LIBFLOW),
    ('a-2d-shear-is-dropped', MOTION,
     '    d->sk[0] = shear / d->sc[1];',
     '    d->sk[0] = 0;', LIBFLOW),
    ('a-2d-flip-is-lost', MOTION,
     '    if (det < 0) {\n        if (m11 < m22)',
     '    if (false) {\n        if (m11 < m22)', LIBFLOW),
    # libflow: values mixed.
    ('colours-mix-unpremultiplied', MOTION,
     '        double ca = ((a >> shift) & 0xFF) * aa, cb = ((b >> shift) & 0xFF) * ab;\n        double c = lerp(ca, cb, t) / alpha;',
     '        double ca = ((a >> shift) & 0xFF), cb = ((b >> shift) & 0xFF);\n        double c = lerp(ca, cb, t);', LIBFLOW),
    ('integers-round-down', MOTION,
     '            d->z_index = (int32_t)floor(lerp(a->z_index, b->z_index, t) + 0.5);',
     '            d->z_index = (int32_t)floor(lerp(a->z_index, b->z_index, t));', LIBFLOW),
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
     '    if (!at0)\n        list[n++] = (Frame){&m->base, 0, own};',
     '    static flow_style_t initial;\n    if (!at0)\n        list[n++] = (Frame){&initial, 0, own};', LIBFLOW),
    ('a-keyframe-easing-is-ignored', MOTION,
     '            list[n++] = (Frame){&f->style, f->offset, f->has_easing ? &f->easing : own};',
     '            list[n++] = (Frame){&f->style, f->offset, own};', LIBFLOW),
    ('border-currentcolor-stays-put', MOTION,
     '        if (out->current_colours & FLOW_CURRENT_BORDER(i))\n            out->border_color[i] = out->color;',
     '', LIBFLOW),
    ('an-important-winner-is-animated', STYLE,
     '            if (!lay_sets(c, &k, ua, parent, kf[j].sets, NULL, kf[j].nsets, &important))',
     '            if (!lay_sets(c, &k, ua, parent, kf[j].sets, NULL, kf[j].nsets, NULL))', LIBFLOW),
    ('no-memory-for-a-list-is-a-whole-layout', STYLE,
     '    if (lists.no_memory)\n        return false;',
     '', LIBFLOW),
    # libflow: when the page next changes.
    ('a-step-edge-is-not-due', MOTION,
     '                    double at = base + at_progress(&k, &an->timing,\n                                                   s.offset + span * j / ease->steps);',
     '                    double at = INFINITY;', LIBFLOW),
    ('a-discrete-flip-is-not-due', MOTION,
     '                double at = base + at_progress(&k, &an->timing, s.offset + span * half_way(ease));',
     '                double at = INFINITY;\n                (void)half_way;', LIBFLOW),
    ('a-delayed-start-is-not-due', MOTION,
     '            next = fmin(next, base + k.before_end);\n            continue;',
     '            continue;', LIBFLOW),
    ('a-paused-animation-is-due', MOTION,
     '        if (an->record == NULL || an->record->paused)\n            continue;\n        double local = local_time(an->record, now), base',
     '        if (an->record == NULL)\n            continue;\n        double local = local_time(an->record, now), base', LIBFLOW),
    ('what-the-view-cannot-see-is-due', FLOW,
     '        if (!meets(flow_box_doc_rect(b, scroll), view) &&\n            !meets(moved(b->overflow, o.x, o.y), view))\n            continue;',
     '        (void)o;\n        (void)view;', LIBFLOW),
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
     '    if (!s_animations || g.covered || g.page.tree == NULL)\n        return YONDER_NEVER;',
     '    if (!s_animations || g.page.tree == NULL)\n        return YONDER_NEVER;', WINDOW),
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
     '    if (!s_animations || g.covered || g.page.tree == NULL)',
     '    if (g.covered || g.page.tree == NULL)', WINDOW),
    ('pages-are-never-told-to-reduce', YONDER,
     '                        .dark = s_dark, .reduced_motion = s_reduce_motion};',
     '                        .dark = s_dark};', WINDOW),
    ('the-cascade-is-kept-across-reduce', YONDER,
     '                                                  garb_cascade_env(cascade).reduced_motion !=\n'
     '                                                      view.reduced_motion))) {',
     '                                                  false))) {', WINDOW),
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
