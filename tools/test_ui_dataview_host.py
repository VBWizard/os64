#!/usr/bin/env python3
"""Details public APIs with production renderer, real fonts and fault injection."""
import argparse, os, runpy, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser(); p.add_argument('--real',action='store_true'); p.add_argument('--output',type=Path); p.add_argument('-O',default='2'); a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='os64-e3-')); out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O'+a.O,'-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
flags+=['-I'+str(ROOT/d) for d in ['userland/libos64/include','userland/libimage/include','userland/libos64','abi/include','tools/fonts']]
objects=[]
if a.real:
    upstream,port=runpy.run_path(str(ROOT/'tools/test_freetype_host.py'))['sources']()
    for i,source in enumerate(upstream+port):
        obj=out/f'ft{i}.o'
        extra=['-DFT2_BUILD_LIBRARY','-Wno-unused-variable','-Wno-unused-but-set-variable'] if source in upstream else []
        subprocess.run(['cc',*flags,'-fcf-protection=none','-fno-builtin','-fno-tree-loop-distribute-patterns','-DOS64_FREETYPE_HOSTED','-I'+str(ROOT/'userland/libfreetype/port'),'-I'+str(ROOT/'userland/libfreetype/upstream/include'),*extra,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
    flags+=['-DUI_TEXT_REAL']
sources=['tools/test_ui_dataview_host.c']
if not a.real: sources+=['tools/fonts/fake_backend.c']
sources+=['userland/libos64/'+n+'.c' for n in ['ui_data','ui_dataview','ui_font','ui','ui_list','ui_controls','ui_text','ui_menu','ui_menu_widget','ui_dispatcher','ui_icon','ui_action','ui_status','font_provider','font_adopt','text','text_cache','text_decode','text_bitmap','text_draw','draw','str','fmt','ui_theme']]
subprocess.run(['cc',*flags,*[str(ROOT/s) for s in sources],*objects,'-o',str(out/'test_ui_dataview')],check=True)
print('Artifacts:',out,flush=True)
raise SystemExit(subprocess.run([str(out/'test_ui_dataview'),str(ROOT/'userland/libfreetype/fixtures')],env={**os.environ,'ASAN_OPTIONS':os.environ.get('ASAN_OPTIONS','detect_leaks=1')}).returncode)
