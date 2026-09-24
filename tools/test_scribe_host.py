#!/usr/bin/env python3
"""Drive the real Scribe on the host: scribe.c and its buffer, libui, the
provider and the real engine against the shipped faces. The window system,
the session theme and the disk are stubbed in tools/test_scribe_host.c."""
import argparse,os,runpy,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('-O',default='2');p.add_argument('--output',type=Path);a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='os64-scribe-'));out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O'+a.O,'-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
flags+=['-I'+str(ROOT/path) for path in ['userland/libos64/include','userland/libos64','abi/include','tools/fonts','userland/apps/scribe']]
objects=[]
upstream,port=runpy.run_path(str(ROOT/'tools/test_freetype_host.py'))['sources']()
for n,source in enumerate(upstream+port):
    obj=out/f'ft{n}.o'
    extra=['-DFT2_BUILD_LIBRARY','-Wno-unused-variable','-Wno-unused-but-set-variable'] if source in upstream else []
    subprocess.run(['cc',*flags,'-fcf-protection=none','-fno-builtin','-fno-tree-loop-distribute-patterns','-DOS64_FREETYPE_HOSTED','-I'+str(ROOT/'userland/libfreetype/port'),'-I'+str(ROOT/'userland/libfreetype/upstream/include'),*extra,'-c',str(source),'-o',str(obj)],check=True)
    objects.append(str(obj))
# The harness #includes the libui suite (for its allocator, face loader and
# CHECK) and scribe.c (whose statics are under test); scribe_buf.c is linked
# as the program links it. The libui sources are the suite's own list.
sources=['tools/test_scribe_host.c','userland/apps/scribe/scribe_buf.c']
sources+=['userland/libos64/'+name+'.c' for name in
          ['ui_font','ui','ui_controls','ui_list','ui_text','font_provider','font_adopt','text',
           'text_cache','text_decode','text_bitmap','text_draw','draw','str']]
subprocess.run(['cc',*flags,'-DUI_TEXT_REAL',*[str(ROOT/s) for s in sources],*objects,'-o',str(out/'test_scribe')],check=True)
result=subprocess.run([str(out/'test_scribe'),str(ROOT/'userland/libfreetype/fixtures')],env=os.environ)
print('Artifacts:',out);raise SystemExit(result.returncode)
