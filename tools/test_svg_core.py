#!/usr/bin/env python3
"""Build native SVG, production FreeType and pinned libmath under sanitizers."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

import test_freetype_host as ft

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    build = Path(tempfile.mkdtemp(prefix='build-', dir=output))
    cc = os.environ.get('CC', 'cc')
    common = ['-O2', '-g', '-std=c11', '-fcf-protection=none', '-fno-builtin',
              '-fno-tree-loop-distribute-patterns', '-fno-fast-math', '-ffp-contract=off',
              '-fexcess-precision=standard', '-mno-avx', '-mno-fma',
              '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror']
    includes = ['-I'+str(ROOT/'userland/libsvg/include'),
                '-I'+str(ROOT/'userland/libmath/include'),
                '-I'+str(ROOT/'userland/libos64/include')]
    entries = []
    # Read production membership; new source files cannot silently miss the harness.
    membership = (ROOT/'userland/libsvg/library.mk').read_text().replace('\\\n', ' ')
    membership = membership.split('LIBSVG_SRCS := ', 1)[1].splitlines()[0]
    entries += [(ROOT/'userland'/p, includes) for p in membership.split()]
    upstream, port = ft.sources()
    ft_flags = ['-DOS64_FREETYPE_HOSTED', '-I'+str(ft.LIB/'port'),
                '-I'+str(ft.LIB/'upstream/include'), *includes]
    entries += [(p, [*ft_flags, '-DFT2_BUILD_LIBRARY', '-Wno-unused-variable',
                    '-Wno-unused-but-set-variable']) for p in upstream]
    entries += [(p, ft_flags) for p in port]
    flags = subprocess.check_output(['make', '-s', '-C', str(ROOT/'userland'), 'math-host-flags'], text=True)
    math = dict(line.split('=', 1) for line in flags.splitlines() if line.startswith('MATH_'))
    math = {key: shlex.split(value)[0] for key, value in math.items()}
    for name in math['MATH_UPSTREAM_SRCS'].split():
        extra = ['-nostdinc', *shlex.split(math['MATH_UPSTREAM_FLAGS'])]
        if name.endswith('__rem_pio2_large.c'):
            extra += shlex.split(math['MATH_REM_PIO2_LARGE_FLAGS'])
        entries.append((ROOT/'userland'/name, extra))
    for name in math['MATH_PORT_SRCS'].split():
        entries.append((ROOT/'userland'/name, shlex.split(math['MATH_SQRT_FLAGS'])))
    entries.append((ROOT/'tools/test_svg_core.c', includes))

    def compile_one(entry):
        source, extra = entry
        obj = build/(str(source.relative_to(ROOT)).replace('/', '_')+'.o')
        subprocess.run([cc, *common, *extra, '-c', str(source), '-o', str(obj)],
                       check=True, cwd=ROOT/'userland')
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
        objects = list(workers.map(compile_one, entries))
    binary = build/'svg-core'
    subprocess.run([cc, *common, *map(str, objects), '-o', str(binary)], check=True)
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    run = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=120)
    (output/'results.txt').write_text(run.stdout+run.stderr)
    print(run.stdout+run.stderr, end='')
    print('Binary:', binary)
    return run.returncode


if __name__ == '__main__':
    raise SystemExit(main())
