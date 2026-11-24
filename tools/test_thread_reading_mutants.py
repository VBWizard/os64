#!/usr/bin/env python3
"""One deliberate break at a time to where the thread went per reading: the
pure reading (userland/apps/yonder/thread_reading.c, against
tools/test_thread_reading_host.sh), libdom's timer sites and list counts
(userland/libdom/timer.c, against the DOM events probe), libjs's collector
reader (userland/libjs/runtime/runtime.c, against the runtime host suite)
and yonder's wiring (userland/apps/yonder/yonder.c, against the profile's
cases in the scripted-page host suite). A mutant is CAUGHT when its suite
fails, MISSED when it passes, which means a test is missing and gets
written. A build failure is neither.

The engine's own counting (libjs/patches/0047-collector-counts.patch) is not
mutated here: a patch is pinned by its hash in the manifest, so a broken one
is refused before anything runs. The runtime host suite's collector case
reads every count it adds.

The shape is test_task_order_mutants.py's, with each mutant naming its file
and its suite: the file is put back from the bytes read before the change,
checked byte for byte, whatever happens. Run it from a clean tree, and never
beside another harness that edits the same tree; the per-mutant logs are in
--logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/thread-reading-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

READING = 'userland/apps/yonder/thread_reading.c'
TIMER = 'userland/libdom/timer.c'
RUNTIME = 'userland/libjs/runtime/runtime.c'
YONDER = 'userland/apps/yonder/yonder.c'
CASES_READING = 'bash tools/test_thread_reading_host.sh'
CASES_DOM = 'bash tools/test_dom_host.sh --events'
CASES_JS = 'bash tools/test_js_runtime_host.sh'
CASES_YONDER = 'YONDER_PROFILE_ONLY=1 bash tools/test_yonder_scripts_host.sh'
TIMEOUT = 2400

mutants = [
    # The reading's calls and steps.
    ('call-time-not-charged', READING, CASES_READING,
     '        t->call_ticks[t->call] += since(t->call_from, now);', '        (void)0;'),
    ('a-turn-at-every-call', READING, CASES_READING,
     '    if (t->call == YONDER_TR_CALLS || t->step != step)', '    if (1)'),
    ('no-turn-on-a-new-step', READING, CASES_READING,
     '    if (t->call == YONDER_TR_CALLS || t->step != step)', '    if (t->call == YONDER_TR_CALLS)'),
    ('reset-forgets-the-call-under-way', READING, CASES_READING,
     '    yonder_tr_call_t call = t->on ? t->call : YONDER_TR_CALLS;',
     '    yonder_tr_call_t call = YONDER_TR_CALLS;'),
    ('reset-carries-the-call-uncounted', READING, CASES_READING,
     '        t->call_n[call] = 1;\n        t->turns[step] = 1;', '        (void)step;'),
    ('the-line-leaves-out-the-call-under-way', READING, CASES_READING,
     '        calls[t->call] += since(t->call_from, now);', '        (void)0;'),
    ('inside-with-no-call', READING, CASES_READING,
     '    if (!t->on || t->call == YONDER_TR_CALLS || (unsigned)in >= YONDER_TR_INS)',
     '    if (!t->on || (unsigned)in >= YONDER_TR_INS)'),
    ('the-other-calls-unsaid', READING, CASES_READING,
     "        if (rest_n != 0 && *sep == ',') {", '        if (0) {'),
    ('busy-counts-the-wait', READING, CASES_READING,
     '    put_u(&o, all != 0 ? (all - steps[YONDER_LT_WAIT]) * 100 / all : 0);',
     '    put_u(&o, all != 0 ? 100 : 0);'),
    ('a-step-with-time-and-no-turn-hidden', READING, CASES_READING,
     '            if (!shown[k] && t->turns[k] != 0 && (s < 0 || steps[k] > steps[s]))',
     '            if (!shown[k] && t->turns[k] > 1 && (s < 0 || steps[k] > steps[s]))'),
    # The timers by site.
    ('the-table-ends-a-site-early', READING, CASES_READING,
     '    if (t->nsites == YONDER_TR_SITES) {', '    if (t->nsites == YONDER_TR_SITES - 1) {'),
    ('the-cheapest-site-first', READING, CASES_READING,
     '            if (!listed[i] && (k < 0 || t->sites[i].cost.ticks > t->sites[k].cost.ticks))',
     '            if (!listed[i] && (k < 0 || t->sites[i].cost.ticks < t->sites[k].cost.ticks))'),
    ('the-most-is-the-least', READING, CASES_READING,
     '    if (ticks > c->most)\n        c->most = ticks;', '    if (ticks < c->most || c->n == 1)\n        c->most = ticks;'),
    ('a-long-site-not-cut', READING, CASES_READING,
     '    while (site[n] != \'\\0\' && n + 1 < YONDER_TR_SITE_TEXT) {',
     '    while (site[n] != \'\\0\' && n + 1 < YONDER_TR_SITE_TEXT + 8) {'),
    # The collector and the walks.
    ('collector-runs-not-the-reading-s', READING, CASES_READING,
     '    uint64_t runs = b->runs - a->runs;', '    uint64_t runs = b->runs;'),
    ('collector-before-and-after-swapped', READING, CASES_READING,
     '        put_mib(&o, (b->bytes_before - a->bytes_before) / runs);',
     '        put_mib(&o, (b->bytes_after - a->bytes_after) / runs);'),
    ('intersection-targets-not-summed', READING, CASES_READING,
     '    t->observed += targets;', '    (void)targets;'),
    ('collector-behind-its-start-wraps', READING, CASES_READING,
     '        a = &kNone;\n', '        (void)kNone;\n'),
    ('timer-list-behind-its-start-wraps', READING, CASES_READING,
     '    bool behind = list_now->arms < t->list_from.arms || list_now->walked < t->list_from.walked;',
     '    bool behind = false;'),
    ('the-first-reading-spans-from-the-reset', READING, CASES_READING,
     '    t->from_first_call = call == YONDER_TR_CALLS;', '    t->from_first_call = false;'),
    ('timer-arms-not-the-reading-s', READING, CASES_READING,
     '    uint64_t arms = list_now->arms - t->list_from.arms,', '    uint64_t arms = list_now->arms,'),
    # libdom's sites and counts.
    ('a-timer-keeps-no-site', TIMER, CASES_DOM,
     '    timer->site = site_of(dom, ctx);', '    timer->site = 0;'),
    ('a-fire-that-ran-nothing-keeps-the-last-site', TIMER, CASES_DOM,
     '    if (dom != NULL) dom->timer_site_ran = 0;', '    (void)0;'),
    ('the-fired-timer-s-site-not-taken', TIMER, CASES_DOM,
     '    dom->timer_site_ran = timer->site;\n', ''),
    ('every-timer-a-new-site', TIMER, CASES_DOM,
     '            return (uint16_t)(i + 1);', '            break;'),
    ('sites-off-keeps-the-table', TIMER, CASES_DOM,
     '        d_free(dom, dom->timer_sites);\n        dom->timer_sites = NULL;\n        dom->timer_site_ran = 0;\n        for',
     '        for'),
    ('the-list-walk-not-counted', TIMER, CASES_DOM,
     '        dom->timer_walked += walked;\n', ''),
    ('the-list-counted-with-the-sites-off', TIMER, CASES_DOM,
     '    if (dom->timer_sites != NULL) {\n        dom->timer_arms++;', '    if (1) {\n        dom->timer_arms++;'),
    ('an-inline-script-by-the-engine-s-name', TIMER, CASES_YONDER,
     'address != NULL ? address : bindings_name ? "the page" : where.file',
     'address != NULL ? address : where.file'),
    # libjs's reader.
    ('the-collector-untimed', RUNTIME, CASES_JS,
     '        JS_SetCollectorClock(runtime->engine, os64_js_ledger_now);\n', ''),
    ('the-collector-not-read', RUNTIME, CASES_JS,
     '    JS_GetCollectorCounts(runtime->engine, &c);\n    *out =', '    return;\n    *out ='),
    # yonder's wiring.
    ('tasks-not-inside-the-step', YONDER, CASES_YONDER,
     '        yonder_tr_inside(&s_tr, YONDER_TR_IN_TASKS, spent);\n', ''),
    ('timers-not-by-site', YONDER, CASES_YONDER,
     '            yonder_tr_timer(&s_tr, yonder_scripts_timer_site(host), spent, forced.layouts,',
     '            yonder_tr_timer(&s_tr, NULL, spent, forced.layouts,'),
    ('a-reading-does-not-start-again', YONDER, CASES_YONDER,
     '    tr_restart();\n    effects_reset();', '    effects_reset();'),
    ('on-without-page-files', YONDER, CASES_YONDER,
     "    if (s_diag.dir[0] == '\\0') {\n        yonder_tr_off(&s_tr);", "    if (0) {\n        yonder_tr_off(&s_tr);"),
    ('intersection-steps-not-counted', YONDER, CASES_YONDER,
     '        yonder_tr_intersections(&s_tr, yonder_scripts_intersections_observed(s));',
     '        (void)0;'),
    ('timer-sites-not-asked', YONDER, CASES_YONDER,
     ".timer_sites = s_diag.dir[0] != '\\0'}", '.timer_sites = false}'),
]

# Not run, because they change nothing a rule promises: the call names'
# table (a name changed is a line that reads differently, which the LINE
# cases catch anyway) and tr_call's test of s_tr.on, which only saves the
# counter read while the reading is off (yonder_tr_call tests it again).

caught, missed, unbuilt = [], [], []
for name, rel, cases, old, new in mutants:
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
                result = subprocess.run(['bash', '-c', cases], cwd=root, stdout=out, stderr=out, timeout=TIMEOUT)
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

print(f'\nthread reading mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
