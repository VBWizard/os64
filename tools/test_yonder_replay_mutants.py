#!/usr/bin/env python3
"""One deliberate break at a time to the replay's rules
(docs/design/pending/REPLAY.md § 1-7, every rule a mutant), against the
replay's whole proof, tools/test_yonder_replay_host.sh, or, for a rule
yonder keeps when it writes a record, the scripted-page host,
tools/test_yonder_scripts_host.sh: a mutant is CAUGHT when its proof fails,
MISSED when it passes, which means a case is missing and gets written. A
build failure is neither. REPLAY.md's rule 4: a check without a mutant is
not a check.

The shape is test_yonder_diag_mutants.py's: each mutant is applied to the
worktree's copy of its file, the proof is run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, or through tools/mutants_parallel.py
(`tools/test_yonder_replay_host.sh --mutants` does); the per-mutant logs
are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-replay-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

RUNNER = 'tools/test_yonder_replay_host.c'
# Spelled in parts: tools/mutants_parallel.py takes a quoted tools/*.py
# path for a suite to run unmutated first, and this one is a module.
RECORDS = str(pathlib.PurePath('tools', 'yonder_records.py'))
PROOF = 'tools/test_yonder_replay_host.sh'
WINDOW = 'tools/test_yonder_scripts_host.sh'
DIAG = 'userland/apps/yonder/diag.c'
YONDER = 'userland/apps/yonder/yonder.c'
DIFF = str(pathlib.PurePath('tools', 'yonder_corpus_diff.py'))

# THE INPUTS A VERSION 2 RECORD MUST SAY (REPLAY.md § 3), the runner's
# kNeeded, each taken out of it in turn: the proof's case for that input
# must fail.
NEEDED = ('    static const int kNeeded[]={F_FINAL,F_BODY,F_KEPT_LINES,F_LAID,F_ZOOM,F_APPEARANCE,F_MOTION,F_ANIMATIONS,'
          'F_MODERN,\n        F_SCRIPTS,F_SCRIPT_SECONDS,F_AGENT,F_COOKIES,F_CLOCK,F_TZ,F_STATUS,F_TYPE,F_CHARSET,F_ENCRYPTED};')


def without(fact):
    """kNeeded without `fact`."""
    cut = NEEDED.replace(fact + ',', '', 1) if fact + ',' in NEEDED else NEEDED.replace(',' + fact, '', 1)
    assert cut != NEEDED, fact
    return cut


# (name, file, old, new, harness). Each breaks one rule REPLAY.md states.
mutants = [
    # § 3: every input a version 2 record must say.
    ('needed-final-address', RUNNER, NEEDED, without('F_FINAL'), PROOF),
    ('needed-body', RUNNER, NEEDED, without('F_BODY'), PROOF),
    ('needed-kept-lines', RUNNER, NEEDED, without('F_KEPT_LINES'), PROOF),
    ('needed-laid-out-at', RUNNER, NEEDED, without('F_LAID'), PROOF),
    ('needed-zoom', RUNNER, NEEDED, without('F_ZOOM'), PROOF),
    ('needed-appearance', RUNNER, NEEDED, without('F_APPEARANCE'), PROOF),
    ('needed-motion', RUNNER, NEEDED, without('F_MOTION'), PROOF),
    ('needed-animate', RUNNER, NEEDED, without('F_ANIMATIONS'), PROOF),
    ('needed-modern', RUNNER, NEEDED, without('F_MODERN'), PROOF),
    ('needed-scripts', RUNNER, NEEDED, without('F_SCRIPTS'), PROOF),
    ('needed-script-seconds', RUNNER, NEEDED, without('F_SCRIPT_SECONDS'), PROOF),
    ('needed-agent', RUNNER, NEEDED, without('F_AGENT'), PROOF),
    ('needed-cookies', RUNNER, NEEDED, without('F_COOKIES'), PROOF),
    ('needed-clock', RUNNER, NEEDED, without('F_CLOCK'), PROOF),
    ('needed-tz', RUNNER, NEEDED, without('F_TZ'), PROOF),
    ('needed-status', RUNNER, NEEDED, without('F_STATUS'), PROOF),
    ('needed-content-type', RUNNER, NEEDED, without('F_TYPE'), PROOF),
    ('needed-charset', RUNNER, NEEDED, without('F_CHARSET'), PROOF),
    ('needed-encrypted', RUNNER, NEEDED, without('F_ENCRYPTED'), PROOF),
    # § 3: a version 1 page names what it took from the harness.
    ('defaulted-line-unwritten', RUNNER,
     '        if(rp_any_taken(o)) {',
     '        if(false && rp_any_taken(o)) {', PROOF),
    # § 3 and § 4: versions.
    ('unknown-version-read', RUNNER,
     '            else if(l.n[0]!=1 && l.n[0]!=YONDER_DIAG_INDEX_VERSION) {',
     '            else if(false) {', PROOF),
    ('versions-need-not-agree', RUNNER,
     '    if(index.version!=0 && index.version!=rp_version(&r)) {',
     '    if(false && index.version!=rp_version(&r)) {', PROOF),
    # § 4: the index.
    ('identity-not-compared', RUNNER,
     '    return e->kind==kind && (e->any_id || e->id==id) && !strcmp(e->url,url);',
     '    return e->kind==kind && (e->any_id || ((void)id, true)) && !strcmp(e->url,url);', PROOF),
    ('numbering-not-checked', RUNNER,
     '            if(l.n[0]!=x->n+1) { bad=',
     '            if(false) { bad=', PROOF),
    # § 2: the grammar, in both readers.
    ('c-leading-zero-read', RUNNER,
     "    if(n>1 && s[0]=='0') return false;",
     "    if(false) return false;", PROOF),
    ('py-leading-zero-read', RECORDS,
     '            or (len(digits) > 1 and digits[0] == 0x30)):',
     '            or False):', PROOF),
    ('c-past-uint32-read', RUNNER,
     '    if(x>limit) return false;',
     '    if(false) return false;', PROOF),
    ('py-past-uint32-read', RECORDS,
     '    if n > limit:',
     '    if False:', PROOF),
    ('c-control-byte-read', RUNNER,
     "        if((unsigned char)line[i]<0x20 && line[i]!='\\t') { l.why=",
     "        if(false) { l.why=", PROOF),
    ('py-control-byte-read', RECORDS,
     '        if any(c < 0x20 for c in line if c != 0x09):',
     '        if False:', PROOF),
    ('c-end-not-required', RUNNER,
     '    return rp_version(r)>=2 && (r->facts[F_KEPT]!=NULL',
     '    return false && rp_version(r)>=2 && (r->facts[F_KEPT]!=NULL', PROOF),
    ('py-end-not-required', RECORDS,
     '            if not ended and kept and any(line.startswith(V2_FACTS) for line in body):',
     '            if False and any(line.startswith(V2_FACTS) for line in body):', PROOF),
    # § 6: one process per page, and what the parent trusts.
    ('agreement-assumed', RUNNER,
     '        if(says) {',
     '        if(says || true) {', PROOF),
    ('temporary-left', RUNNER,
     '        if(rp_fs_remove(temp,&fault)==RP_FS_FAULT) {',
     '        if(false && rp_fs_remove(temp,&fault)==RP_FS_FAULT) {', PROOF),
    ('clean-flag-trusted', RUNNER,
     'clean==(st==RP_VERDICT && reproduced==0) &&',
     '(clean==(st==RP_VERDICT && reproduced==0) || true) &&', PROOF),
    ('exit-table-unbounded', RUNNER,
     '} else if(code>=RP_CHILD_BASE && code<=RP_CHILD_TOP) {',
     '} else if(code>=RP_CHILD_BASE) {', PROOF),
    ('sigchld-not-reset', RUNNER,
     '    signal(SIGCHLD,SIG_DFL);',
     '    (void)0;', PROOF),
    ('wait-not-retried', RUNNER,
     'if(w>=0 || errno!=EINTR) break;',
     'if(w>=0 || true) break;', PROOF),
    ('time-limit-not-kept', RUNNER,
     'if(!*timed_out && ms>=rp_child_limit_ms) {',
     'if(false && ms>=rp_child_limit_ms) {', PROOF),
    # § 2 and § 3: what the reader of a kept record refuses.
    ('c-whole-record-utf8-unchecked', RUNNER,
     'if(!rp_utf8((const unsigned char *)text,size)) return "it is not a page file: it is not UTF-8";',
     'if(false) return "it is not a page file: it is not UTF-8";', PROOF),
    ('c-lines-past-end-read', RUNNER,
     'if(at<size) why="it is not a whole page file: lines past its `end`";',
     'if(false) why="it is not a whole page file: lines past its `end`";', PROOF),
    ('c-second-verdict-read', RUNNER,
     '} else if(!strcmp(l.kind,"verdict")) {',
     '} else if(false && !strcmp(l.kind,"verdict")) {', PROOF),
    ('placeholder-read', RUNNER,
     '} else if(l.unwritten) r->unjudged=',
     '} else if(false) r->unjudged=', PROOF),
    ('c-escaped-non-utf8-read', RUNNER,
     '    if(!utf8) { l.why=',
     '    if(!utf8 && false) { l.why=', PROOF),
    ('py-escaped-non-utf8-read', RECORDS,
     "            decoded.decode('utf-8')",
     '            pass', PROOF),
    ('budget-unwritten', RUNNER,
     '        rp_add(&t,budget,strlen(budget));',
     '        (void)0;', PROOF),
    ('py-budget-not-required', RECORDS,
     '        if rec.budget is None:',
     '        if False:', PROOF),
    ('diff-budgets-not-compared', DIFF,
     '        elif before[name].budget != after[name].budget:',
     '        elif False:', PROOF),
    ('c-nul-field-read', RUNNER,
     '    if(nul) { l.why="a NUL in a field"; return l; }',
     '    if(nul && false) { l.why="a NUL in a field"; return l; }', PROOF),
    ('py-nul-field-read', RECORDS,
     "        if b'\\0' in decoded:",
     '        if False:', PROOF),
    ('kept-lines-not-bound', RUNNER,
     '                                 want_lines!=index->lines)) {',
     '                                 (want_lines!=index->lines && false))) {', PROOF),
    # § 3: what a version 1 page takes from outside its record, never
    # silently: the person's statement, or nothing.
    ('stated-zoom-from-harness', RUNNER,
     '            f[k]=rp_stated[i];',
     '            f[k]=k==F_ZOOM ? "1000" : rp_stated[i];', PROOF),
    ('stated-agent-from-harness', RUNNER,
     '            f[k]=rp_stated[i];',
     '            f[k]=k==F_AGENT ? YONDER_AGENT : rp_stated[i];', PROOF),
    ('version1-empty-charset-unread', RUNNER,
     '        if(!local && f[F_CHARSET]==NULL) f[F_CHARSET]="";',
     '        (void)local;', PROOF),
    ('unstated-input-not-refused', RUNNER,
     '            if(rp_stated[i]==NULL) {',
     '            if(false) {', PROOF),
    ('stated-line-unwritten', RUNNER,
     '            if(!o->from.stated[k]) continue;',
     '            if(true) continue;', PROOF),
    ('assumed-line-unwritten', RUNNER,
     '        if(o->from.assumed) rp_add(',
     '        if(false) rp_add(', PROOF),
    ('version1-without-kept-not-kept', RUNNER,
     '    *kept=*assumed=got==RP_FS_OK && !dir;',
     '    *kept=*assumed=false && got==RP_FS_OK && !dir;', PROOF),
    ('window-split-at-every-comma', RUNNER,
     '            if(rp_statable_at(c+1)) { e=c; break; }',
     '            if(rp_statable_at(c+1) || true) { e=c; break; }', PROOF),
    ('window-view-without-height', RUNNER,
     "(laid && v[rp_digit_run(v,e)]!='x')",
     '(laid && false)', PROOF),
    ('py-stated-unchecked', RECORDS,
     '                if not eq or not value or key not in STATABLE or key in stated:',
     '                if False:', PROOF),
    ('diff-statements-not-compared', DIFF,
     '        elif before[name].stated != after[name].stated:',
     '        elif False:', PROOF),
    # § 4: each line answers one job.
    ('a-line-answers-twice', RUNNER,
     'if(x->e[i].used==0 && rp_entry_is(',
     'if(rp_entry_is(', PROOF),
    # § 3: every input set where the window reads it.
    ('record-inputs-not-read', RUNNER,
     '    load->agent=f[F_AGENT]; load->cookies=f[F_COOKIES]; load->tz=f[F_TZ];',
     '    rp_harness_inputs(load);', PROOF),
    ('inputs-not-applied', RUNNER,
     '    rp_apply_inputs(load);',
     '    (void)rp_apply_inputs;', PROOF),
    ('agent-not-applied', RUNNER,
     '    g.way.agent=load->agent;',
     '    (void)load->agent;', PROOF),
    ('cookies-not-applied', RUNNER,
     '    fake_cookie_read=load->cookies;',
     '    (void)load->cookies;', PROOF),
    ('clock-not-applied', RUNNER,
     '    js_test_clock.epoch=(int64_t)(load->clock_ms/1000);\n    js_test_clock.ticks_per_second=1000;\n'
     '    js_test_clock.ticks_into_second=(uint32_t)(load->clock_ms%1000);',
     '    (void)load->clock_ms;', PROOF),
    ('zone-not-applied', RUNNER,
     '    if(load->tz==NULL) return;',
     '    if(true) return;', PROOF),
    ('zoom-not-applied', RUNNER,
     'g.zoom=load->zoom;',
     '(void)load->zoom;', PROOF),
    ('appearance-not-applied', RUNNER,
     's_dark=load->dark;',
     '(void)load->dark;', PROOF),
    ('motion-not-applied', RUNNER,
     's_reduce_motion=load->reduce;',
     '(void)load->reduce;', PROOF),
    ('animate-not-applied', RUNNER,
     '    s_animations=!load->still;',
     '    (void)load->still;', PROOF),
    ('height-not-applied', RUNNER,
     '    g.root.bounds.h+=load->height-g.view.bounds.h;',
     '    (void)load->height;', PROOF),
    # § 1 and § 3, yonder's side: the claim proved, the inputs whole, a
    # window that changed marked.
    ('claim-not-proved', DIAG,
     '|| !claim_ours(c->record, path))',
     '|| (!claim_ours(c->record, path) && false))', WINDOW),
    ('claim-nonce-not-compared', DIAG,
     '        ours = diag_contains(text, mark);',
     '        ours = true;', WINDOW),
    ('cookies-written-cut', YONDER,
     '        if (cookies != NULL && whole)',
     '        if (cookies != NULL)', WINDOW),
    ('setting-change-unmarked', YONDER,
     '        if (then != NULL && !os64_streq(then, diag_setting(kDiagSettings[i], now, sizeof(now))))',
     '        if (false && then != NULL && !os64_streq(then, diag_setting(kDiagSettings[i], now, sizeof(now))))',
     WINDOW),
    ('scripts-switch-unrecorded', YONDER,
     '{"agent", "scripts", "modern",',
     '{"agent", "modern",', WINDOW),
    ('resize-before-arrival-unmarked', YONDER,
     '    if (!diag_same_view(yonder_diag_fact_value(p->diag, "laid out at"), p->laid_width, p->laid_height))',
     '    if (false && !diag_same_view(yonder_diag_fact_value(p->diag, "laid out at"), p->laid_width, p->laid_height))',
     WINDOW),
    # § 7: the diff's rules.
    ('diff-defaults-not-compared', DIFF,
     '        elif before[name].defaulted != after[name].defaulted:',
     '        elif False:', PROOF),
    ('diff-addresses-not-compared', DIFF,
     '        if before[name].address != after[name].address:',
     '        if False:', PROOF),
    ('diff-unfinished-not-broken', DIFF,
     '        if a == State.UNFINISHED:',
     '        if False:', PROOF),
    ('diff-regressions-pass', DIFF,
     "    return 1 if table['regressions'] or table['broken']",
     "    return 1 if table['broken']", PROOF),
    ('diff-broken-passes', DIFF,
     "table['regressions'] or table['broken'] or table['newly incomplete']",
     "table['regressions'] or table['newly incomplete']", PROOF),
    ('diff-newly-incomplete-passes', DIFF,
     "or table['newly incomplete'] or gone else 0",
     'or gone else 0', PROOF),
    ('diff-missing-from-after-passes', DIFF,
     "or table['newly incomplete'] or gone else 0",
     "or table['newly incomplete'] else 0", PROOF),
    ('diff-only-after-judged', DIFF,
     "        unjudged[name] = f'{name}: {args.before} did not replay this page'",
     '        continue', PROOF),
    ('diff-not-reproduced-compared', DIFF,
     '    skip = not_reproduced(before, after)',
     '    skip = set()', PROOF),
    ('diff-unjudged-passes', DIFF,
     '    if unjudged:\n        return 2\n',
     '    if False:\n        return 2\n', PROOF),
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
                result = subprocess.run(['bash', harness], cwd=root, stdout=out, stderr=out, timeout=900)
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

print(f'\nreplay mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
