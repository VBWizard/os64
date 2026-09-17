#!/usr/bin/env python3
"""Drive libui's font binding on the host: the real ui_font.c, the real
provider, and — with --real — the real engine against the shipped faces."""
import argparse,os,runpy,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('-O',default='2');p.add_argument('--real',action='store_true');p.add_argument('--output',type=Path);a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='os64-ui-text-'));out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O'+a.O,'-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
flags+=['-I'+str(ROOT/path) for path in ['userland/libos64/include','userland/libos64','abi/include','tools/fonts']]
objects=[]
if a.real:
    upstream,port=runpy.run_path(str(ROOT/'tools/test_freetype_host.py'))['sources']()
    for n,source in enumerate(upstream+port):
        obj=out/f'ft{n}.o'
        extra=['-DFT2_BUILD_LIBRARY','-Wno-unused-variable','-Wno-unused-but-set-variable'] if source in upstream else []
        subprocess.run(['cc',*flags,'-fcf-protection=none','-fno-builtin','-fno-tree-loop-distribute-patterns','-DOS64_FREETYPE_HOSTED','-I'+str(ROOT/'userland/libfreetype/port'),'-I'+str(ROOT/'userland/libfreetype/upstream/include'),*extra,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
    flags+=['-DUI_TEXT_REAL']
# draw.c is the bitmap painter the identity check compares against; ui_font.c
# is the subject. The window system and the heap are stubbed in the test.
sources=['tools/test_ui_text_host.c']
if not a.real: sources.append('tools/fonts/fake_backend.c')
# ui.c and ui_controls.c come along because the height policy is a property
# of the TOOLKIT — the constructors that say a control sizes itself, and the
# layout that consumes that. draw.c is the bitmap painter the identity check
# compares against; ui_font.c is the subject.
sources+=['userland/libos64/'+name+'.c' for name in
          ['ui_font','ui','ui_controls','font_provider','font_adopt','text','text_cache',
           'text_decode','text_bitmap','text_draw','draw','str']]
subprocess.run(['cc',*flags,*[str(ROOT/s) for s in sources],*objects,'-o',str(out/'test_ui_text')],check=True)
result=subprocess.run([str(out/'test_ui_text'),str(ROOT/'userland/libfreetype/fixtures')],env=os.environ)
print('Artifacts:',out);raise SystemExit(result.returncode)
