#!/usr/bin/env python3
"""One deliberate break at a time to Web Animations' slices A and B
(WEB_ANIMATIONS.md): libdom's Animation state machine, its promises,
events and holds, keyframe and timing processing, the effects it
publishes, KeyframeEffect as an object of its own, the removal of
replaced animations and commitStyles; yonder's keeping of a binding a tree may still read, and the
scripts a measured layout reads; libflow's script effects under
`display: none`. A mutant is CAUGHT when one of its harnesses fails, MISSED
when they all pass, which means a test is missing and gets written. A build
failure is neither.

Each mutant is applied to the worktree's copy of its file, its harnesses
are run, and the file is put back from the bytes read before the change —
checked byte for byte — whatever happens. Nothing else is touched. Run it
from a clean tree, one run at a time, or through tools/mutants_parallel.py.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/web-animations-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

WA_C = 'userland/libdom/webanimation.c'
SCRIPTS_C = 'userland/apps/yonder/scripts.c'
YONDER_C = 'userland/apps/yonder/yonder.c'
STYLE_C = 'userland/libflow/style.c'
DOM = 'tools/test_dom_host.sh --web-animations'
YON = 'tools/test_yonder_scripts_host.sh'
FLOW = 'tools/test_libflow_host.sh'

mutants = [
    # ── The state machine ───────────────────────────────────────────────
    ('seek-unconstrained-is-current', WA_C, 'double unconstrained = did_seek ? current_time(a)',
     'double unconstrained = false ? current_time(a)', [DOM]),
    ('finished-needs-start', WA_C,
     'if (!isnan(unconstrained) && !isnan(a->start) && !a->pending_play && !a->pending_pause) {',
     'if (!isnan(unconstrained) && !a->pending_play && !a->pending_pause) {', [DOM]),
    ('seek-completes-pause', WA_C, '    if (silently_set(ctx, a, seek) < 0) return -1;\n    if (a->pending_pause) {',
     '    if (silently_set(ctx, a, seek) < 0) return -1;\n    if (false) {', [DOM]),
    ('finish-notifies-at-once', WA_C, '    update_finished(ctx, a, true, true);\n    return 0;',
     '    update_finished(ctx, a, true, false);\n    return 0;', [DOM]),
    ('notify-still-finished', WA_C, '    if (play_state(a) != WA_FINISHED || a->finished_done) return;',
     '    if (a->finished_done) return;', [DOM]),
    ('unfinished-new-promise', WA_C, '    if (!finished && a->finished_done) {', '    if (false) {', [DOM]),
    ('cancel-rejects-ready', WA_C,
     '            promise_call(ctx, a->ready_reject, reason);\n            mark_handled(a->dom, ctx, a->ready);',
     '            mark_handled(a->dom, ctx, a->ready);', [DOM]),
    ('cancel-fires', WA_C, '        queue_event(ctx, a, "cancel", NAN, wa_now(a->dom), wa_now(a->dom));\n', '', [DOM]),
    ('reverse-seeks-end', WA_C, '        seek = end;\n    } else if (rate == 0 && isnan(t)) {',
     '        seek = 0;\n    } else if (rate == 0 && isnan(t)) {', [DOM]),
    ('hold-goes-without-seek', WA_C,
     '            if (did_seek && !isnan(a->held)) a->start = wa_now(a->dom) - a->held / a->rate;\n'
     '            a->held = NAN;',
     '            if (did_seek && !isnan(a->held)) a->start = wa_now(a->dom) - a->held / a->rate;\n'
     '            if (did_seek) a->held = NAN;', [DOM]),
    ('pause-of-paused-nothing', WA_C, '    if (a->pending_pause || play_state(a) == WA_PAUSED) return 0;',
     '    if (a->pending_pause) return 0;', [DOM]),
    ('publish-only-on-change', WA_C, '    if (a->published && same_time(a->pub_held, a->held)',
     '    if (false && a->published && same_time(a->pub_held, a->held)', [DOM]),
    ('relevant-rate-zero', WA_C, '    return a->rate > 0 ? k.phase == GARB_PHASE_BEFORE',
     '    return a->rate >= 0 ? k.phase == GARB_PHASE_BEFORE', [DOM]),
    ('time-not-negative-zero', WA_C, 'JS_NewFloat64(ctx, t + 0.0);', 'JS_NewFloat64(ctx, t);', [DOM]),
    # ── Events ──────────────────────────────────────────────────────────
    ('events-by-schedule', WA_C, '    return isnan(e->scheduled) ? -INFINITY : e->scheduled;',
     '    return isnan(e->scheduled) ? -INFINITY : 0;', [DOM]),
    ('finish-scheduled-at-end', WA_C,
     'double scheduled = !isnan(a->start) && a->rate != 0 ? a->start + end / a->rate : NAN;',
     'double scheduled = ((void)end, wa_now(a->dom));', [DOM]),
    # ── Holding and letting go ──────────────────────────────────────────
    ('live-while-running', WA_C,
     '    if (a->pending_play || a->pending_pause || play_state(a) == WA_RUNNING) return true;\n    return a->replace',
     '    if (a->pending_play || a->pending_pause) return true;\n    return a->replace', [DOM]),
    ('busy-while-running', WA_C,
     '        if (a->pending_play || a->pending_pause || play_state(a) == WA_RUNNING) return true;',
     '        if (a->pending_play || a->pending_pause) return true;', [DOM]),
    ('sweep-lets-go', WA_C, '            if (!wa_live(a)) {', '            if (false) {', [DOM]),
    ('hold-unmarked', WA_C, '    JS_MarkValue(rt, a->effect, mark);\n    JS_MarkValue(rt, a->ready, mark);',
     '    JS_MarkValue(rt, a->effect, mark);\n    JS_MarkValue(rt, a->hold, mark);\n    JS_MarkValue(rt, a->ready, mark);',
     [DOM]),
    ('release-by-generation', WA_C, '        if (d->died < oldest) {', '        if (d->died <= oldest) {', [DOM]),
    ('drain-buries', WA_C, '    data_bury(dom, e->data);\n    e->data = NULL;',
     '    data_free(dom, e->data);\n    e->data = NULL;', [YON]),
    # ── Keyframes and timing ────────────────────────────────────────────
    ('every-easing-parsed', WA_C,
     '        for (size_t i = 0; r == 0 && i < n[1]; i++) r = easing_of(ctx, items[1][i], &easings[i]);',
     '        for (size_t i = 0; r == 0 && i < n[1] && (int32_t)i < out->n; i++)\n'
     '            r = easing_of(ctx, items[1][i], &easings[i]);', [DOM]),
    ('easing-list-repeats', WA_C, '            out->v[i].easing = easings[(size_t)i % n[1]];',
     '            out->v[i].easing = easings[0];', [DOM]),
    ('places-are-no-offsets', WA_C,
     '        for (int32_t i = 0; i < out->n && (size_t)i < n[2]; i++) out->v[i].offset = offsets[i];',
     '        for (int32_t i = 0; i < out->n; i++) out->v[i].offset = out->v[i].place;\n'
     '        for (int32_t i = 0; i < out->n && (size_t)i < n[2]; i++) out->v[i].offset = offsets[i];', [DOM]),
    ('null-keyframe-empty', WA_C, '    bool empty = JS_IsNull(obj) || JS_IsUndefined(obj);', '    bool empty = false;',
     [DOM]),
    ('properties-by-name', WA_C, 'j > 0 && os64_strcmp(v[j - 1].idl, v[j].idl) > 0; j--) {',
     'j > 0 && os64_strcmp(v[j - 1].idl, v[j].idl) > 0 && false; j--) {', [DOM]),
    ('declarations-alone', WA_C, '        if (c == \'\\0\') return false;\n', '', [DOM]),
    ('lone-keyframe-at-end', WA_C, '    if (out->n == 1 && isnan(out->v[0].offset)) out->v[0].offset = 1;',
     '    if (out->n == 1 && isnan(out->v[0].offset)) out->v[0].offset = 0;', [DOM]),
    ('offsets-in-order', WA_C, '            if (out->v[i].offset < last) {',
     '            if (out->v[i].offset < last && false) {', [DOM]),
    ('members-webidl-order', WA_C,
     '"iterationStart", "iterations", "composite", "iterationComposite",\n                                           "pseudoElement", "id"};',
     '"iterationStart", "iterations", "composite", "iterationComposite",\n                                           "id", "pseudoElement"};',
     [DOM]),
    ('composite-checked', WA_C, '    if (!JS_IsUndefined(v) && enum_of(ctx, v, kComposites, 4, &w) < 0) return -1;',
     '    if (!JS_IsUndefined(v) && enum_of(ctx, v, kComposites, 4, &w) < 0) JS_FreeValue(ctx, JS_GetException(ctx));', [DOM]),
    ('pseudo-syntax-error', WA_C, '                    d_error(ctx, known ? "NotSupportedError" : "SyntaxError",',
     '                    d_error(ctx, known ? "NotSupportedError" : "NotSupportedError",', [DOM]),
    # ── What libflow is handed ──────────────────────────────────────────
    ('idle-not-handed', WA_C, 'a->replace != WA_REMOVED && play_state(a) != WA_IDLE;',
     'a->replace != WA_REMOVED;', [DOM]),
    ('start-on-face-clock', WA_C, '        fx->start_ms = a->start + a->dom->wa_face;',
     '        fx->start_ms = a->start;', [DOM]),
    ('change-touches', WA_C,
     '    if (wa_target(a) != NULL) (void)os64_html_touch(a->dom->document, (os64_html_node_t *)wa_target(a));\n}',
     '}', [YON]),
    ('frame-samples-timeline', 'userland/libdom/animation.c', '    d_web_animations_sample(dom,timestamp);\n', '',
     [DOM]),
    ('held-keeps-direction', 'userland/libflow/motion.c', '        return garb_clock_at(&x->timing, local, x->rate < 0);',
     '        return garb_clock_at(&x->timing, local, !x->held && x->rate < 0);', [FLOW]),
    ('eased-held-interval-now', 'userland/libflow/motion.c', '                       ? (isnan(base) || bent ? now',
     '                       ? (isnan(base) || (bent && false) ? now', [FLOW]),
    ('effect-easing-unbounds-shadow', STYLE_C,
     '        unbounded |= an->script != NULL && f_easing_overshoot(&an->script->timing.easing) > 0;\n', '', [FLOW]),
    ('sort-moves-the-entry', WA_C, 'j > 0 && os64_strcmp(v[j - 1].idl, v[j].idl) > 0; j--) {',
     'j > 0 && os64_strcmp(v[j - 1].idl, v[n].idl) > 0; j--) {', [DOM]),
    ('side-effects-by-direction', 'userland/libflow/motion.c',
     '        bool current = k.phase == GARB_PHASE_ACTIVE || (rate > 0 && k.phase == GARB_PHASE_BEFORE) ||\n'
     '                       (rate < 0 && k.phase == GARB_PHASE_AFTER);',
     '        bool current = ((void)rate, k.phase != GARB_PHASE_AFTER);', [FLOW]),
    ('unknown-properties-unread', WA_C,
     ' && garb_prop_known(p->css, os64_strlen(p->css), false))) {', ')) {', [DOM]),
    ('handled-through-intrinsic', WA_C,
     '    JSValue r = JS_Call(ctx, dom->wa_promise_then->value, promise, 2, args);',
     '    (void)dom;\n    JSValue then = JS_GetPropertyStr(ctx, promise, "then");\n'
     '    JSValue r = JS_IsFunction(ctx, then) ? JS_Call(ctx, then, promise, 2, args) : JS_UNDEFINED;\n'
     '    JS_FreeValue(ctx, then);', [DOM]),
    ('below-zero-first-frame', 'userland/libflow/motion.c', '    if (progress < 0 && zeros > 1) {',
     '    if (progress < 0 && zeros > 1 && false) {', [FLOW]),
    ('narrower-declaration-wins', WA_C, '                        if (k < ranks[had]) {',
     '                        if (k >= ranks[had]) {', [DOM]),
    # ── yonder ──────────────────────────────────────────────────────────
    ('orphan-waits-for-trees', SCRIPTS_C, '    if (!standing) {\n        os64_dom_free(s->retired_dom);',
     '    if (true) {\n        os64_dom_free(s->retired_dom);', [YON]),
    ('retire-keeps-held', SCRIPTS_C, '    if (os64_dom_effects_held(s->dom) && s->retired_dom == NULL)',
     '    if (false && s->retired_dom == NULL)', [YON]),
    ('measured-reads-stream-scripts', YONDER_C, '    s_measuring.scripts = page_scripts(p);\n', '', [YON]),
    # ── libflow ─────────────────────────────────────────────────────────
    ('script-effects-under-none', STYLE_C,
     '        if (!animate(c, n, &sp, &ua, parent, item, root, &motion))\n            return NULL;',
     '        if (sp.s.display != FLOW_DISPLAY_NONE && !animate(c, n, &sp, &ua, parent, item, root, &motion))\n'
     '            return NULL;', [FLOW]),
    ('css-animations-end-under-none', STYLE_C,
     '    int32_t count = tl != NULL && c->env->cascade != NULL && sp->s.display != FLOW_DISPLAY_NONE',
     '    int32_t count = tl != NULL && c->env->cascade != NULL', [FLOW]),
    # ── Slice B1: effects of their own, removal, commitStyles ──────────
    ('effect-stays-with-its-old-animation', WA_C,
     '    if (previous != NULL) set_effect(ctx, previous, JS_NULL);\n', '    (void)previous;\n', [DOM]),
    ('given-offset-is-the-computed-one', WA_C,
     'isnan(d->kept[i].given) ? JS_NULL : JS_NewFloat64(ctx, d->kept[i].given)',
     'JS_NewFloat64(ctx, d->frames[i].offset)', [DOM]),
    ('unparsed-value-given-back', WA_C,
     '                if (k > 0 || holds_var(piece)) accepted[this_piece] = true;',
     '                if (k > 0 || holds_var(piece) || true) accepted[this_piece] = true;', [DOM]),
    ('custom-property-not-given-back', WA_C,
     '                if (garb_decl_is_custom(&p->items[j].decl)) {\n                    accepted[this_piece] = true;',
     '                if (garb_decl_is_custom(&p->items[j].decl)) {', [DOM]),
    ('update-timing-not-atomic', WA_C,
     '        JS_FreeValue(ctx, v);\n        if (r < 0) {\n            js_free(ctx, ns.id);\n            return -1;\n        }',
     '        JS_FreeValue(ctx, v);\n        if (r == 0) *t = nt;\n        if (r < 0) {\n            js_free(ctx, ns.id);\n            return -1;\n        }', [DOM]),
    ('fill-auto-forgotten', WA_C, '                    ns.fill_auto = w == 4;', '                    ns.fill_auto = false;', [DOM]),
    ('number-duration-still-auto', WA_C,
     '        t->duration_ms = d;\n        spec->duration_auto = false;', '        t->duration_ms = d;', [DOM]),
    ('new-keyframes-lose-the-timing', WA_C, '        d->fx.timing = e->data->fx.timing;\n', '', [DOM]),
    ('removed-effect-still-drawn', WA_C,
     'wa_data(a) != NULL && a->replace != WA_REMOVED && play_state(a) != WA_IDLE;',
     'wa_data(a) != NULL && play_state(a) != WA_IDLE;', [DOM]),
    ('replacement-ignores-the-target', WA_C,
     '                if (wa_target(v[j]) != wa_target(a)) continue;\n', '', [DOM]),
    ('persisted-is-removed', WA_C,
     '        if (a->replace != WA_ACTIVE) continue;', '        if (a->replace == WA_REMOVED) continue;', [DOM]),
    ('remove-event-not-queued', WA_C,
     '        queue_event(ctx, a, "remove", current_time(a), wa_now(dom), wa_now(dom));\n', '', [DOM]),
    ('running-is-replaceable', WA_C,
     '           play_state(a) == WA_FINISHED && wa_clock(a).in_effect;', '           wa_clock(a).in_effect;', [DOM]),
    ('removed-not-in-get-animations', WA_C,
     '        if (target == NULL || a->replace == WA_REMOVED || !wa_relevant(a)) continue;',
     '        if (target == NULL || !wa_relevant(a)) continue;', [DOM]),
    ('persist-does-not-restyle', WA_C,
     '            a->replace = WA_PERSISTED;\n            wa_restyle(a);', '            a->replace = WA_PERSISTED;', [DOM]),
    ('commit-writes-nothing', WA_C,
     "        JSValue done = text[0] != '\\0' ? d_style_write", "        JSValue done = false ? d_style_write", [DOM]),
    ('commit-unrendered-allowed', WA_C,
     '    if (!rendered) {\n        d_error(ctx, "InvalidStateError"', '    if (!rendered && false) {\n        d_error(ctx, "InvalidStateError"', [DOM]),
    ('no-timeline-starts', WA_C, '            if (a->no_timeline) continue;\n', '', [DOM]),
    ('moved-effect-not-rechecked', WA_C,
     '    a->published = false;\n    /* Which effect, target or keyframes it has moves what replaces what. */\n    a->dom->wa_replace_check = true;',
     '    a->published = false;', [DOM]),
    ('two-arguments-copy', WA_C, '    DWaEffect *source = argc == 1 ? fx_state(argv[0]) : NULL;',
     '    DWaEffect *source = argc >= 1 ? fx_state(argv[0]) : NULL;', [DOM]),
    ('no-timeline-keeps-stepping', WA_C, '        if (a->no_timeline) continue;\n        if (a->pending_play', '        if (a->pending_play', [DOM]),
    ('target-set-not-republished', WA_C,
     '        e->element = element;\n        effect_changed(ctx, e, false);', '        e->element = element;', [DOM]),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, harnesses in mutants:
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
    codes = []
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            for harness in harnesses:
                script, *flags = harness.split()
                try:
                    result = subprocess.run(['bash', str(root / script), *flags], cwd=root, stdout=out, stderr=out,
                                            timeout=2400)
                    codes.append(result.returncode)
                except subprocess.TimeoutExpired:
                    codes.append('timeout')
                if codes[-1] != 0:
                    break
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
    elif any(code != 0 for code in codes):
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nweb animations mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
