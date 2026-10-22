#!/usr/bin/env python3
"""One deliberate break at a time to the replay's rules
(docs/design/pending/REPLAY.md § 2-6), against the replay's whole proof,
tools/test_yonder_replay_host.sh: a mutant is CAUGHT when the proof fails,
MISSED when it passes, which means a case is missing and gets written. A
build failure is neither. REPLAY.md § 7: a check without a mutant is not a
check.

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
