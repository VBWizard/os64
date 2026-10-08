#!/usr/bin/env python3
"""One deliberate break at a time to the page files and the badge
(docs/design/pending/YONDER_DIAGNOSTICS.md), against the scripted-page
harness: a mutant is CAUGHT when the harness fails, MISSED when it passes,
which means a test is missing and gets written. A build failure is neither.

The shape is test_yonder_loop_mutants.py's: each mutant is applied to the
worktree's copy of its file, the harness is run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-diag-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

DIAG = 'userland/apps/yonder/diag.c'
SCRIPTS = 'userland/apps/yonder/scripts.c'
YONDER = 'userland/apps/yonder/yonder.c'
WINDOW = 'tools/test_yonder_scripts_host.sh'

# (name, file, old, new, harness). Each breaks one rule the design states.
mutants = [
    # The brief's three.
    ('departure-rewrite-skipped', YONDER,
     '    diag_observe(p, "when the page was left");\n    diag_write(p->diag);',
     '    diag_observe(p, "when the page was left");', WINDOW),
    ('miss-counted-twice', SCRIPTS,
     '    yonder_diag_missing(s->options.diag, "global", name, 1);',
     '    yonder_diag_missing(s->options.diag, "global", name, 2);', WINDOW),
    ('tokens-leak-into-a-plain-line', DIAG,
     '        put_text(&o, d->facts.lines[i].b, true);',
     '        put_text(&o, d->facts.lines[i].b, false);', WINDOW),
    # And the rules around them.
    ('a-look-counts-again', YONDER,
     '            yonder_diag_missing_seen(d, "element", kNothing[k], nothing[k]);',
     '            yonder_diag_missing(d, "element", kNothing[k], nothing[k]);', WINDOW),
    ('arrival-write-skipped', YONDER,
     '    diag_observe(p, "when the page arrived");\n    diag_write(p->diag);',
     '    diag_observe(p, "when the page arrived");', WINDOW),
    ('a-failure-is-not-recorded', YONDER,
     '    yonder_diag_failed(diag, key, failure);',
     '    (void)failure;', WINDOW),
    ('a-lost-load-writes-nothing', YONDER,
     'static void stream_failed(const char *line)\n{\n    yonder_diag_failed(g.stream.diag, "page", line);',
     'static void stream_failed(const char *line)\n{\n    (void)line;', WINDOW),
    ('the-badge-stays-blank', YONDER,
     '    g.badge_missing = missing;\n    g.badge_failed = failed;',
     '    g.badge_missing = missing;\n    g.badge_failed = failed;\n    return;', WINDOW),
    ('the-hook-is-not-installed', SCRIPTS,
     '    if (s->options.diag != NULL)\n        os64_dom_set_global_miss(s->dom, global_missed, s);',
     '    (void)global_missed;', WINDOW),
    ('module-not-recorded', YONDER,
     '        yonder_diag_missing(g.stream.diag, "script", "module", 1);',
     '        (void)0;', WINDOW),
    ('cascade-not-looked-at', YONDER,
     '    if (p->cascade != NULL)\n        diag_cascade(d, p->cascade);',
     '    (void)diag_cascade;', WINDOW),
    ('sequence-restarts', DIAG,
     '    return high < UINT32_MAX ? high + 1 : high;',
     '    return 1;', WINDOW),
    ('a-line-breaker-is-written-raw', DIAG,
     '        } else if (c < 0x20 || c == 0x7F) {',
     '        } else if (c == 0x7F) {', WINDOW),
    ('the-missing-parent-is-not-said', YONDER,
     '        why = os64_mkdir(dir) == 0 ? NULL : "cannot be made (does its parent exist?)";',
     '        why = ((void)os64_mkdir(dir), NULL);', WINDOW),
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
                result = subprocess.run(['bash', '-c', harness if harness.endswith('.sh') else f'python3 {harness}'],
                                        cwd=root, stdout=out, stderr=out, timeout=900)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
                subprocess.run(['pkill', '-f', 'yonder-script[s]'])
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

print(f'\ndiagnostics mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
