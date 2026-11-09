#!/usr/bin/env python3
"""Run the C1 follow-up boundary probes against the current worktree.

By default rebuilds the real-backend suite and original six probes first.
--baseline reuses an existing baseline directory's ft*.o files; only use it
with objects from this checkout and compatible ASan/UBSan compiler flags.
These probes print observations, not pass/fail verdicts; compare the review.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path)
parser.add_argument('--baseline', type=Path)
args = parser.parse_args()
out = args.output or Path(tempfile.mkdtemp(prefix='os64-f4-c1-r2-'))
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
if args.baseline is None:
    subprocess.run([sys.executable, str(HERE/'run.py'), '--output', str(out)],
                   cwd=ROOT, env=env, check=True)
baseline = args.baseline or out/'baseline'
objects = sorted(baseline.glob('ft*.o'))
if not objects:
    parser.error('No backend objects in ' + str(baseline))
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections', '-DUI_TEXT_REAL',
         '-DF4_C1_HOST_TEST="'+str(ROOT/'tools/test_ui_text_host.c')+'"']
flags += ['-I'+str(ROOT/p) for p in
          ['userland/libos64/include', 'userland/libos64', 'abi/include']]
sources = [ROOT/('userland/libos64/'+n+'.c') for n in
           ['ui_font', 'ui', 'ui_controls', 'ui_list', 'ui_text', 'font_provider', 'font_adopt',
            'text', 'text_cache', 'text_decode', 'text_bitmap', 'text_draw', 'draw', 'str']]
subprocess.run(['cc', *flags, str(HERE/'r2-repro.c'), *map(str, sources+objects),
                '-o', str(out/'r2-repro')], check=True)
receipt = []
failed = False
for mode in ['tabs', 'button', 'busy-state', 'list-release', 'list-resize']:
    result = subprocess.run([str(out/'r2-repro'), mode,
                             str(ROOT/'userland/libfreetype/fixtures')],
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (out/('r2-'+mode+'.txt')).write_text(result.stdout)
    receipt.append(f'{mode}: exit {result.returncode}\n{result.stdout}')
    print(receipt[-1], end='')
    failed |= result.returncode != 0
(out/'r2-observations.txt').write_text('\n'.join(receipt))
raise SystemExit(1 if failed else 0)
