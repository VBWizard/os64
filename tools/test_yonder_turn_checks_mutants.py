#!/usr/bin/env python3
"""One deliberate break at a time to yonder's half of the image check's quiet
key (canvas_source_answers, image_sources.inc's image_answers): the answers
must move when a selection completes, or the binding's image check skips
the pass that would tell a load; and a provider with no answers makes every
pass walk. With them, one of libdom's: a context method touches its canvas
after its body (canvas.c's d_canvas_method), since only yonder's forced
layout can run a damage visit inside an argument. Against the scripted
host's image slice (YONDER_IMAGES_ONLY: responsive_turn_check_cases in
tools/test_yonder_responsive_images.inc). libdom's half (the lists, its own
serial, the key's use) is test_dom_mutants.py --turn. A mutant is CAUGHT
when the cases fail, MISSED when they pass, which means a test is missing
and gets written. A build failure is neither.

The shape is test_yonder_data_pictures_mutants.py's: each mutant is applied
to the worktree's copy of its file, the cases are run, and the file is put
back from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/turn-checks-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

YONDER = 'userland/apps/yonder/yonder.c'
SOURCES = 'userland/apps/yonder/image_sources.inc'
CANVAS = 'userland/libdom/canvas.c'
CASES = 'YONDER_IMAGES_ONLY=1 bash tools/test_yonder_scripts_host.sh'
TIMEOUT = 1800

# (name, file, old, new). Each breaks one rule the design states.
mutants = [
    ('a-completion-moves-nothing', SOURCES,
     '            if(changed || s->current_generation!=s->generation)p->image_answers++;\n',
     ''),
    ('the-answers-leave-out-the-selections', YONDER,
     'p?p->modern:0,p?p->scripting:0,p?p->image_answers:0,',
     'p?p->modern:0,p?p->scripting:0,0,'),
    ('the-canvas-touched-before-the-drawing', CANVAS,
     '    JSValue result = body(ctx, dom, e, argc, argv, magic);\n    d_canvas_touch(e);\n',
     '    d_canvas_touch(e);\n    JSValue result = body(ctx, dom, e, argc, argv, magic);\n'),
    ('the-provider-gives-no-answers', YONDER,
     '.images={canvas_source,canvas_source_release,doc,image_sources_update,canvas_source_answers},',
     '.images={canvas_source,canvas_source_release,doc,image_sources_update,0?canvas_source_answers:NULL},'),
]

if args.only:
    mutants = [m for m in mutants if args.only in m[0]]

caught = missed = broken = 0
for name, rel, old, new in mutants:
    path = root / rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'ANCHOR {name}: {text.count(old)} matches', flush=True)
        broken += 1
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(CASES, shell=True, cwd=root, stdout=out, stderr=subprocess.STDOUT,
                                        timeout=TIMEOUT)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = None
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            sys.exit(f'RESTORE FAILED for {rel}; stop and repair the tree')
    excerpt = log.read_text(errors='replace')
    if code is None:
        print(f'TIMEOUT {name} (not counted)', flush=True)
        broken += 1
    elif 'error:' in excerpt and 'FAIL' not in excerpt and code != 0:
        print(f'BUILD {name} (neither)', flush=True)
        broken += 1
    elif code != 0:
        print(f'CAUGHT {name}', flush=True)
        caught += 1
    else:
        print(f'MISSED {name}', flush=True)
        missed += 1
print(f'turn checks mutants: {caught} caught, {missed} missed, {broken} neither, of {len(mutants)}; logs {args.logs}')
sys.exit(0 if missed == 0 and broken == 0 else 1)
