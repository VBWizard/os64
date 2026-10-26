#!/usr/bin/env python3
"""Build the private SVG geometry proof against Os64's pinned FreeType port."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import subprocess
import tempfile
import test_freetype_host as ft
from svg_geometry.report import report

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='keep images, log and build here')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    build = Path(tempfile.mkdtemp(prefix='build-', dir=output))
    cc = os.environ.get('CC', 'cc')
    common = ['-O2', '-g', '-std=c11', '-fcf-protection=none', '-fno-builtin',
              '-fno-tree-loop-distribute-patterns', '-DOS64_FREETYPE_HOSTED',
              '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
              '-Wno-missing-field-initializers',
              '-I'+str(ft.LIB/'port'), '-I'+str(ft.LIB/'upstream/include'),
              '-I'+str(ROOT/'userland/libos64/include')]
    upstream, port = ft.sources()
    # ftstroke's glyph conveniences require ftglyph, which requires ftbitmap.
    extra = [ft.LIB/'upstream/src/base'/f for f in ('ftstroke.c', 'ftglyph.c', 'ftbitmap.c')]
    entries = [(p, ['-DFT2_BUILD_LIBRARY', '-Wno-unused-variable', '-Wno-unused-but-set-variable'])
               for p in upstream + extra]
    entries += [(p, []) for p in port]
    entries += [(ROOT/'tools/svg_geometry/proof.c', [])]
    def compile_one(entry):
        source, flags = entry
        obj = build/(source.name+'.o')
        subprocess.run([cc, *common, *flags, '-c', str(source), '-o', str(obj)], check=True)
        return obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
        objects = list(workers.map(compile_one, entries))
    binary = build/'geometry-proof'
    subprocess.run([cc, *common, *map(str, objects), '-lm', '-o', str(binary)], check=True)
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    result = subprocess.run([str(binary), str(output)], env=env, capture_output=True, text=True, timeout=120)
    (output/'results.txt').write_text(result.stdout+result.stderr)
    print(result.stdout+result.stderr, end='')
    if result.returncode == 0:
        report(output)
    print('Artifacts:', output)
    return result.returncode

if __name__ == '__main__':
    raise SystemExit(main())
