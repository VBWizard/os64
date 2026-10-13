#!/usr/bin/env python3
"""Exercise Roam's real toolkit composition and application policy with real fonts."""
import argparse
import os
import runpy
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path)
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
out = args.output or Path(tempfile.mkdtemp(prefix='roam-ui-host-'))
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-D_POSIX_C_SOURCE=200809L', '-DUI_TEXT_REAL',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections']
flags += ['-I' + str(root / name) for name in
          ['userland/libos64/include', 'userland/libimage/include',
           'userland/libos64', 'abi/include', 'tools/fonts']]
upstream, port = runpy.run_path(str(root / 'tools/test_freetype_host.py'))['sources']()
objects = []
for index, source in enumerate(upstream + port):
    target = out / f'ft{index}.o'
    extra = (['-DFT2_BUILD_LIBRARY', '-Wno-unused-variable',
              '-Wno-unused-but-set-variable'] if source in upstream else [])
    if not target.exists():
        subprocess.run(['cc', *flags, '-fcf-protection=none', '-fno-builtin',
                        '-fno-tree-loop-distribute-patterns', '-DOS64_FREETYPE_HOSTED',
                        '-I' + str(root / 'userland/libfreetype/port'),
                        '-I' + str(root / 'userland/libfreetype/upstream/include'),
                        *extra, '-c', str(source), '-o', str(target)], check=True)
    objects.append(str(target))
sources = ['tools/test_roam_ui_host.c'] + ['userland/libos64/' + name + '.c' for name in
    ['ui_browser', 'ui_data', 'ui_dataview', 'ui_menu', 'ui_menu_widget',
     'ui_dispatcher', 'ui_icon', 'ui_action', 'ui_status', 'ui_font', 'ui',
     'ui_controls', 'ui_list', 'ui_text', 'font_provider', 'font_adopt', 'text',
     'text_cache', 'text_decode', 'text_bitmap', 'text_draw', 'draw', 'str',
     'fmt', 'ui_theme']]
binary = out / 'test_roam_ui'
subprocess.run(['cc', *flags, *[str(root / source) for source in sources],
                *objects, '-o', str(binary)], check=True)
print('Artifacts:', out, flush=True)
if args.build_only:
    raise SystemExit(0)
raise SystemExit(subprocess.run([str(binary), str(root / 'userland/libfreetype/fixtures')],
    env={**os.environ, 'ASAN_OPTIONS': os.environ.get('ASAN_OPTIONS', 'detect_leaks=1')}).returncode)
