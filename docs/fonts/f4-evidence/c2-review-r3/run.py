#!/usr/bin/env python3
"""Build supplemental review probes using fresh compatible backend objects.
Exit zero means probes completed; their printed observations need review.
"""
import argparse, os, subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[4]
here = Path(__file__).resolve().parent
p = argparse.ArgumentParser()
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
objects = sorted(a.baseline.glob('ft*.o'))
if not objects:
    p.error('Build tools/test_ui_text_host.py --real first; pass its output directory')
flags = ['-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
         '-fsanitize=address,undefined','-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections',
         '-Wl,--gc-sections','-DUI_TEXT_REAL',
         '-DF4_C2_HOST_TEST="'+str(root/'tools/test_ui_text_host.c')+'"']
flags += ['-I'+str(root/d) for d in
          ['userland/libos64/include','userland/libos64','abi/include']]
sources = [root/('userland/libos64/'+n+'.c') for n in
           ['ui_font','ui','ui_controls','ui_list','ui_text','font_provider',
            'font_adopt','text','text_cache','text_decode','text_bitmap','text_draw','draw','str']]
binary = a.output/'repro'
subprocess.run(['cc',*flags,str(here/'repro.c'),*map(str,sources+objects),'-o',str(binary)],check=True)
r = subprocess.run([str(binary)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),
                   stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
(a.output/'observations.txt').write_text(r.stdout)
print(r.stdout,end='')
raise SystemExit(r.returncode)
