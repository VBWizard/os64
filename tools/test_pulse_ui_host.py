#!/usr/bin/env python3
"""Exercise Pulse widgets or Instruments with the production font and drawing paths."""
import argparse,os,runpy,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('-O',default='2');p.add_argument('--instruments',action='store_true');p.add_argument('--build-only',action='store_true');p.add_argument('--output',type=Path);a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='os64-pulse-ui-'));out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O'+a.O,'-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
flags+=['-I'+str(ROOT/path) for path in ['userland/libos64/include','userland/libos64','abi/include','tools/fonts']]
objects=[]
upstream,port=runpy.run_path(str(ROOT/'tools/test_freetype_host.py'))['sources']()
for n,source in enumerate(upstream+port):
    obj=out/f'ft{n}.o'
    extra=['-DFT2_BUILD_LIBRARY','-Wno-unused-variable','-Wno-unused-but-set-variable'] if source in upstream else []
    subprocess.run(['cc',*flags,'-fcf-protection=none','-fno-builtin','-fno-tree-loop-distribute-patterns','-DOS64_FREETYPE_HOSTED','-I'+str(ROOT/'userland/libfreetype/port'),'-I'+str(ROOT/'userland/libfreetype/upstream/include'),*extra,'-c',str(source),'-o',str(obj)],check=True)
    objects.append(str(obj))
flags+=['-DUI_TEXT_REAL']
# Production widgets and Pulse are exercised by the harness; only guest I/O is stubbed.
harness='test_pulse_instruments' if a.instruments else 'test_pulse_ui'
sources=['tools/'+harness+'_host.c','userland/apps/pulse/pulse_data.c','userland/libos64/fmt.c']
sources+=['userland/libos64/'+name+'.c' for name in
          ['ui_dropdown','ui_action','ui_font','ui','ui_controls','ui_list','ui_text','font_provider','font_adopt','text',
           'text_cache','text_decode','text_bitmap','text_draw','draw','str']]
sources.append('userland/apps/pulse/pulse_instruments.c')
sources.append('shared/damage.c')   # ui.c keeps its dirty list by the shared rule
subprocess.run(['cc',*flags,*[str(ROOT/s) for s in sources],*objects,'-o',str(out/harness)],check=True)
if a.build_only:
    print('Artifacts:',out);raise SystemExit(0)
result=subprocess.run([str(out/harness),str(ROOT/'userland/libfreetype/fixtures'),str(out if a.instruments else out/'pulse.ppm')],env={**os.environ,'ASAN_OPTIONS':os.environ.get('ASAN_OPTIONS','detect_leaks=1')})
print('Artifacts:',out);raise SystemExit(result.returncode)
