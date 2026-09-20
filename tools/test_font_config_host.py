#!/usr/bin/env python3
"""Exercise configuration and I/O against the production provider and FreeType."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser()
p.add_argument('--output', type=Path)
p.add_argument('--backend-objects', type=Path, help='Reuse compatible ASan/UBSan FreeType objects')
p.add_argument('-O', default='2')
p.add_argument('--settings', action='store_true', help='Run native-file installation/persistence suite')
a = p.parse_args()
out = a.output or Path(tempfile.mkdtemp(prefix='os64-font-config-'))
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=c11', '-O'+a.O, '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections']
if not a.settings: flags += ['-Wl,--wrap=os64_conf_find,--wrap=os64_conf_target']
if a.settings:
    include = out/'include/os64'
    include.mkdir(parents=True, exist_ok=True)
    (include/'syscall.h').write_text('#include <stdint.h>\n#include "os64/syscall_numbers.h"\nuint64_t os64_syscall2(uint64_t,uint64_t,uint64_t);\nuint64_t os64_syscall3(uint64_t,uint64_t,uint64_t,uint64_t);\nuint64_t os64_syscall6(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);\n')
    flags += ['-I'+str(out/'include')]
flags += ['-I'+str(root/d) for d in ['userland/libos64/include', 'userland/libos64', 'abi/include']]
objects = sorted(a.backend_objects.glob('ft*.o')) if a.backend_objects else []
if a.backend_objects and not objects:
    p.error('No backend objects found')
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
sources = ['tools/test_font_settings_host.c' if a.settings else 'tools/test_font_config_host.c']
if a.settings:
    sources += ['userland/libos64/'+n+'.c' for n in ['font_install','ui_session','ui_theme','ui_envelope','ui_palette','ui_saved','fmt']]
sources += ['userland/libos64/'+n+'.c' for n in ['font_config', 'font_discovery', 'font_provider',
            'conf', 'slurp', 'str', 'text', 'text_cache', 'text_decode', 'text_bitmap']]
binary = out/'test_font_config'
subprocess.run(['cc', *flags, *[str(root/s) for s in sources], *map(str, objects),
                '-o', str(binary)], check=True)
args = [str(binary)]
if a.settings:
    native = out/'native'
    native.mkdir(exist_ok=False)
    args.append(str(native))
args.append(str(root/'userland/libfreetype/fixtures'))
result = subprocess.run(args, env=os.environ)
print('Artifacts:', out)
raise SystemExit(result.returncode)
