#!/usr/bin/env python3
"""Rebuild the baseline suite and reproduce Quinn's C1 boundary observations.
Writes artifacts to a temporary directory (or --output); edits no source files.
The release probe is expected to fail under ASan at reviewed commit d11608b.
Other probes print observations; their exit status alone is not a correctness test.
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
args = parser.parse_args()
out = args.output or Path(tempfile.mkdtemp(prefix='os64-f4-c1-review-'))
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
with (out/'baseline-host.txt').open('w') as log:
    subprocess.run([sys.executable, str(ROOT/'tools/test_ui_text_host.py'),
                    '--real', '--output', str(out/'baseline')],
                   cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-Wno-misleading-indentation', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
         '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
         '-DUI_TEXT_REAL', '-DF4_C1_HOST_TEST="'+str(ROOT/'tools/test_ui_text_host.c')+'"']
flags += ['-I'+str(ROOT/p) for p in ['userland/libos64/include', 'userland/libos64', 'abi/include']]
# ui.c and ui_controls.c joined the list when the height policy moved into
# the toolkit's constructors and layout; the host test they wrap links them.
sources = [ROOT/('userland/libos64/'+n+'.c') for n in
           ['ui_font', 'ui', 'ui_controls', 'font_provider', 'font_adopt', 'text',
            'text_cache', 'text_decode', 'text_bitmap', 'text_draw', 'draw', 'str']]
objects = sorted((out/'baseline').glob('ft*.o'))
subprocess.run(['cc', *flags, str(HERE/'repro.c'),
                *map(str, sources+objects), '-o', str(out/'repro')], check=True)
for mode in ['measure', 'register', 'clip', 'paint', 'cross', 'release']:
    with (out/(mode+'.txt')).open('w') as log:
        result = subprocess.run([str(out/'repro'), mode,
                                 str(ROOT/'userland/libfreetype/fixtures')],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
    print(f'{mode}: exit {result.returncode}; {out/(mode+".txt")}')
print('Compare outputs with the committed observations.txt; these are review probes.')
