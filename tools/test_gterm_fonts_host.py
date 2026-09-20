#!/usr/bin/env python3
"""Build F3 against the pinned FreeType engine, with ASan and UBSan."""
import argparse, os, runpy, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(); p.add_argument('-O', default='2'); p.add_argument('--output', type=Path)
a = p.parse_args(); out = a.output or Path(tempfile.mkdtemp(prefix='os64-gterm-fonts-')); out.mkdir(parents=True, exist_ok=True)
runpy.run_path(str(ROOT/'tools/fonts/make_terminal_fixture.py'))['make_fixture'](ROOT/'userland/libfreetype/fixtures/DejaVuSansMono.ttf', out/'NoBoxes.ttf')
flags = ['-std=c11', '-O'+a.O, '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
flags += ['-I'+str(ROOT/path) for path in ['userland/libos64/include', 'userland/libos64', 'abi/include']]
upstream, port = runpy.run_path(str(ROOT/'tools/test_freetype_host.py'))['sources']()
objects = []
for n, source in enumerate(upstream+port):
    obj = out/f'ft{n}.o'
    extra = ['-DFT2_BUILD_LIBRARY', '-Wno-unused-variable', '-Wno-unused-but-set-variable'] if source in upstream else []
    subprocess.run(['cc', *flags, '-fcf-protection=none', '-fno-builtin', '-fno-tree-loop-distribute-patterns', '-DOS64_FREETYPE_HOSTED', '-I'+str(ROOT/'userland/libfreetype/port'), '-I'+str(ROOT/'userland/libfreetype/upstream/include'), *extra, '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
sources = ['tools/test_gterm_fonts_host.c', 'userland/apps/gterm/font_grid.c']
sources += ['userland/libos64/'+name+'.c' for name in ['font_provider', 'font_adopt', 'text', 'text_cache', 'text_decode', 'text_bitmap', 'text_draw', 'str', 'draw']]
subprocess.run(['cc', *flags, *[str(ROOT/s) for s in sources], *objects, '-o', str(out/'test_gterm')], check=True)
result = subprocess.run([str(out/'test_gterm'), str(ROOT/'userland/libfreetype/fixtures'), str(out/'NoBoxes.ttf')], env=os.environ)
print('Artifacts:', out); raise SystemExit(result.returncode)
