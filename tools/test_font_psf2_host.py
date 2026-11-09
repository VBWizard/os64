#!/usr/bin/env python3
"""Drive os64_font_render_psf2 on the host and hand what it writes to the
kernel's own PSF2 loader (kernel/src/psf2.c), under ASan and UBSan.

Default: a fake backend with a known ink pattern -- every pixel of every
glyph of both character sets is checked, at every size from 8 to 96.
--real links the FreeType port as well and renders the fixture faces.
--dump DIR (with --real) leaves the rendered .psf files there, for loading
into a guest by hand."""
import argparse,os,runpy,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('-O',default='1');p.add_argument('--real',action='store_true');p.add_argument('--dump',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='os64-font-psf2-'));out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O'+a.O,'-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
flags += ['-I'+str(ROOT/path) for path in ['userland/libos64/include','abi/include','kernel/include']]
objects=[]
if a.real:
    upstream,port=runpy.run_path(str(ROOT/'tools/test_freetype_host.py'))['sources']()
    for n,source in enumerate(upstream+port):
        obj=out/f'ft{n}.o'
        extra=['-DFT2_BUILD_LIBRARY','-Wno-unused-variable','-Wno-unused-but-set-variable'] if source in upstream else []
        subprocess.run(['cc',*flags,'-fcf-protection=none','-fno-builtin','-fno-tree-loop-distribute-patterns','-DOS64_FREETYPE_HOSTED','-I'+str(ROOT/'userland/libfreetype/port'),'-I'+str(ROOT/'userland/libfreetype/upstream/include'),*extra,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(str(obj))
    flags += ['-DPSF2_REAL']
sources=['tools/test_font_psf2_host.c','userland/libos64/font_psf2.c','kernel/src/psf2.c']
subprocess.run(['cc',*flags,*[str(ROOT/s) for s in sources],*objects,'-o',str(out/'test_font_psf2')],check=True)
args=[str(out/'test_font_psf2')]
if a.real:
    args.append(str(ROOT/'userland/libfreetype/fixtures'))
    if a.dump:
        a.dump.mkdir(parents=True,exist_ok=True);args.append(str(a.dump))
result=subprocess.run(args,env=os.environ)
print('Artifacts:',out);raise SystemExit(result.returncode)
