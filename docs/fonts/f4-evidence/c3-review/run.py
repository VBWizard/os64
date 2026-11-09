#!/usr/bin/env python3
"""Build consumer probes without editing the author's host suite.
The copied suite only renames main; its real fixtures/implementations are used.
Exit zero means observations completed, not that their semantics pass.
"""
import argparse, os, subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[4]
here=Path(__file__).resolve().parent
p=argparse.ArgumentParser()
p.add_argument('--baseline',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
objects=sorted(a.baseline.glob('ft*.o'))
if not objects:p.error('Pass fresh compatible backend objects from the widget host suite')
copied=a.output/'scribe-host.c'
copied.write_text((root/'tools/test_scribe_host.c').read_text().replace('int main(', 'int review_original_scribe_main('))
flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
       '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
       '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DUI_TEXT_REAL',
       '-DF4_C3_SCRIBE_TEST="'+str(copied)+'"']
flags+=['-I'+str(root/d) for d in ['tools','tools/fonts','userland/apps/scribe',
        'userland/libos64','userland/libos64/include','abi/include']]
sources=[root/('userland/libos64/'+n+'.c') for n in ['ui_font','ui','ui_controls',
         'ui_list','ui_text','font_provider','font_adopt','text','text_cache',
         'text_decode','text_bitmap','text_draw','draw','str']]
sources.append(root/'userland/apps/scribe/scribe_buf.c')
binary=a.output/'repro'
subprocess.run(['cc',*flags,str(here/'repro.c'),*map(str,sources+objects),'-o',str(binary)],check=True)
r=subprocess.run([str(binary),str(root/'userland/libfreetype/fixtures')],
                 env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),
                 stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
(a.output/'observations.txt').write_text(r.stdout);print(r.stdout,end='')
raise SystemExit(r.returncode)
