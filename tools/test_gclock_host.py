#!/usr/bin/env python3
"""Run gclock callbacks with the production fonts/UI/config code under sanitizers."""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path)
parser.add_argument('--backend-objects', type=Path)
args = parser.parse_args()
out = args.output or Path(tempfile.mkdtemp(prefix='os64-gclock-'))
out.mkdir(parents=True, exist_ok=True)
include = out / 'include/os64'
include.mkdir(parents=True, exist_ok=True)
(include / 'syscall.h').write_text('''#include <stdint.h>
#include "os64/syscall_numbers.h"
uint64_t os64_syscall2(uint64_t,uint64_t,uint64_t);
uint64_t os64_syscall3(uint64_t,uint64_t,uint64_t,uint64_t);
uint64_t os64_syscall6(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
''')
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections', '-I'+str(out/'include')]
flags += ['-I'+str(root/d) for d in ['userland/libos64/include', 'userland/libos64', 'abi/include']]
objects = sorted(args.backend_objects.glob('ft*.o')) if args.backend_objects else []
if args.backend_objects and not objects:
    parser.error('No backend objects found')
if not objects:
    upstream, port = runpy.run_path(str(root/'tools/test_freetype_host.py'))['sources']()
    for i, source in enumerate(upstream+port):
        obj = out/f'ft{i}.o'
        extra = ['-DFT2_BUILD_LIBRARY', '-Wno-unused-variable', '-Wno-unused-but-set-variable'] if source in upstream else []
        subprocess.run(['cc', *flags, '-fcf-protection=none', '-fno-builtin',
                        '-fno-tree-loop-distribute-patterns', '-DOS64_FREETYPE_HOSTED',
                        '-I'+str(root/'userland/libfreetype/port'),
                        '-I'+str(root/'userland/libfreetype/upstream/include'), *extra,
                        '-c', str(source), '-o', str(obj)], check=True)
        objects.append(obj)
sources = ['tools/test_gclock_host.c'] + ['userland/libos64/'+n+'.c' for n in
    ['ui', 'ui_font', 'ui_controls', 'ui_list', 'ui_text', 'font_provider', 'font_adopt',
     'font_config', 'conf', 'slurp', 'str', 'fmt', 'text', 'text_cache', 'text_decode',
     'text_bitmap', 'text_draw', 'draw']]
binary = out/'test_gclock'
subprocess.run(['cc', *flags, *[str(root/s) for s in sources], *map(str, objects), '-o', str(binary)], check=True)
native = Path(tempfile.mkdtemp(prefix='native-', dir=out))
subprocess.run([str(binary), str(native), str(root/'userland/libfreetype/fixtures/DejaVuSans.ttf')], check=True)
print('Artifacts:', out)
