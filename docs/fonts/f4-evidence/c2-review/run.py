#!/usr/bin/env python3
"""C2 review probes against real libui, FreeType, Scribe planner and buffer.
File I/O is an in-memory fixture; the GUI event loop is not launched.
Outputs are observations, not behavioral pass/fail verdicts.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
p = argparse.ArgumentParser()
p.add_argument('--output', type=Path)
p.add_argument('--baseline', type=Path, help='Reuse compatible ft*.o from this checkout')
a = p.parse_args()
out = a.output or Path(tempfile.mkdtemp(prefix='os64-f4-c2-review-'))
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
baseline = a.baseline or out/'baseline'
if a.baseline is None:
    with (out/'baseline-host.txt').open('w') as log:
        subprocess.run([sys.executable, str(ROOT/'tools/test_ui_text_host.py'),
                        '--real', '--output', str(baseline)], cwd=ROOT, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
else:
    with (out/'baseline-host.txt').open('w') as log:
        subprocess.run([str(baseline/'test_ui_text'),
                        str(ROOT/'userland/libfreetype/fixtures')], env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
objects = sorted(baseline.glob('ft*.o'))
if not objects:
    p.error('No backend objects in '+str(baseline))
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections', '-DUI_TEXT_REAL',
         '-DF4_C2_HOST_TEST="'+str(ROOT/'tools/test_ui_text_host.c')+'"',
         '-DF4_C2_SCRIBE="'+str(ROOT/'userland/apps/scribe/scribe.c')+'"']
flags += ['-I'+str(ROOT/d) for d in
          ['userland/libos64/include', 'userland/libos64', 'abi/include']]
sources = [ROOT/('userland/libos64/'+n+'.c') for n in
           ['ui_font', 'ui', 'ui_controls', 'ui_list', 'ui_text', 'font_provider',
            'font_adopt', 'text', 'text_cache', 'text_decode', 'text_bitmap',
            'text_draw', 'draw', 'str']]
sources.append(ROOT/'userland/apps/scribe/scribe_buf.c')
subprocess.run(['cc', *flags, str(HERE/'repro.c'), *map(str, sources+objects),
                '-o', str(out/'repro')], check=True)
receipt = []
failed = False
for mode in ['delete', 'field-oom', 'view-oom', 'field-adopt', 'commit', 'commit-oom', 'save']:
    r = subprocess.run([str(out/'repro'), mode,
                        str(ROOT/'userland/libfreetype/fixtures')], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out/(mode+'.txt')).write_text(r.stdout)
    receipt.append(f'{mode}: exit {r.returncode}\n{r.stdout}')
    print(receipt[-1], end='')
    failed |= r.returncode != 0
(out/'observations.txt').write_text('\n'.join(receipt))
raise SystemExit(1 if failed else 0)
