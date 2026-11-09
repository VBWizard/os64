#!/usr/bin/env python3
"""Regenerate frames/*.frame with the pinned production font and painter.

Use --check to compare regenerated assets without replacing them. --preview
selects the contact sheet path (PPM). --sanitize enables ASan and UBSan.
"""
import argparse
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--preview', type=Path, default=Path('/tmp/os64-frame-collection.ppm'))
    args = parser.parse_args()
    cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-ffunction-sections', '-fdata-sections', '-fcf-protection=none',
             '-fno-builtin', '-fno-tree-loop-distribute-patterns', '-DOS64_FREETYPE_HOSTED']
    flags += ['-I'+str(ROOT / p) for p in ['abi/include', 'userland/libos64/include',
              'userland/libos64', 'userland/libfreetype/port', 'userland/libfreetype/upstream/include']]
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    upstream, port = runpy.run_path(str(ROOT / 'tools/test_freetype_host.py'))['sources']()
    local = ['tools/frame_collection.c', 'shared/decoration.c',
             'userland/apps/framestudio/model.c', 'userland/apps/framestudio/storage.c']
    local += ['userland/libos64/'+n+'.c' for n in ['decoration_prepare', 'arena', 'font_provider',
              'text', 'text_cache', 'text_decode', 'text_bitmap', 'text_draw', 'str']]
    with tempfile.TemporaryDirectory(prefix='os64-frame-collection-') as tmp:
        work = Path(tmp)
        objects = []
        for i, source in enumerate(upstream + port + [ROOT / p for p in local]):
            extra = ['-DFT2_BUILD_LIBRARY', '-Wno-unused-variable', '-Wno-unused-but-set-variable'] if source in upstream else []
            obj = work / f'{i}.o'
            subprocess.run(cc + flags + extra + ['-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        binary = work / 'generate'
        subprocess.run(cc + flags + ['-Wl,--gc-sections', *objects, '-o', str(binary)], check=True)
        output = work / 'frames'
        output.mkdir()
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(binary), str(ROOT / 'userland/libfreetype/fixtures/DejaVuSans.ttf'),
                        str(output), str(args.preview.resolve())], check=True)
        target = ROOT / 'frames'
        generated = sorted(output.glob('*.frame'))
        if args.check:
            if {p.name for p in generated} != {p.name for p in target.glob('*.frame')}:
                raise SystemExit('Collection filenames differ; regenerate the collection.')
            for p in generated:
                if p.read_bytes() != (target / p.name).read_bytes():
                    raise SystemExit(f'Composition differs: {p.name}')
            print('Collection matches the checked-in assets.')
        else:
            target.mkdir(exist_ok=True)
            for p in generated:
                (target / p.name).write_bytes(p.read_bytes())
        print('Preview:', args.preview)


if __name__ == '__main__':
    main()
