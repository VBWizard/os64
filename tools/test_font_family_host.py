#!/usr/bin/env python3
"""Exercise the font-family cache lifetime and resource bounds."""
import argparse,os,runpy,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('-O',default='2');p.add_argument('--real',action='store_true');p.add_argument('--output',type=Path);a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='os64-font-provider-'));out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O'+a.O,'-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
flags += ['-I'+str(ROOT/path) for path in ['userland/libos64/include','userland/libos64','abi/include','tools/fonts']]
objects=[]
if a.real:
    upstream,port=runpy.run_path(str(ROOT/'tools/test_freetype_host.py'))['sources']()
    for n,source in enumerate(upstream+port):
        obj=out/f'ft{n}.o'
        extra=['-DFT2_BUILD_LIBRARY','-Wno-unused-variable','-Wno-unused-but-set-variable'] if source in upstream else []
        subprocess.run(['cc',*flags,'-fcf-protection=none','-fno-builtin','-fno-tree-loop-distribute-patterns','-DOS64_FREETYPE_HOSTED','-I'+str(ROOT/'userland/libfreetype/port'),'-I'+str(ROOT/'userland/libfreetype/upstream/include'),*extra,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
    flags += ['-DFAMILY_REAL']
sources=['tools/test_font_family_host.c','tools/fonts/fake_backend.c']
sources += ['userland/libos64/'+name+'.c' for name in ['font_provider','font_family','text','text_cache','text_decode','text_bitmap','str']]
subprocess.run(['cc',*flags,*[str(ROOT/s) for s in sources],*objects,'-o',str(out/'test_family')],check=True)
result=subprocess.run([str(out/'test_family'),str(ROOT/'userland/libfreetype/fixtures')],env=os.environ)
if result.returncode: raise SystemExit(result.returncode)
if not a.real:
    bad = subprocess.run([str(out/'test_family'),str(ROOT/'userland/libfreetype/fixtures'),'stale'],env=os.environ,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    (out/'stale-list-asan.txt').write_bytes(bad.stderr)
    if bad.returncode == 0 or b'heap-use-after-free' not in bad.stderr:
        raise SystemExit('ASan did not diagnose expired font list')
    print('Expired list: expected ASan heap-use-after-free verified')
print('Artifacts:',out)
