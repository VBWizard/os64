#!/usr/bin/env python3
"""Build native SVG, production FreeType and pinned libmath under sanitizers."""
import argparse
import concurrent.futures
import hashlib
import json
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
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument('--paint', action='store_true', help='run scene painting tests and write fixtures')
    profile.add_argument('--yonder', action='store_true', help='run the real HTML/CSS/layout SVG adapter')
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
    if args.yonder:
        includes += ['-I'+str(ROOT/d) for d in ('abi/include', 'tools', 'userland',
            'userland/libflow/include', 'userland/libflow', 'userland/apps/yonder',
            'userland/libhtml/include', 'userland/libxml/include', 'userland/libpage/include', 'userland/libpage/upstream/ryu',
            'userland/libgarb/include', 'userland/libfetch/include', 'userland/libgzip/include',
            'userland/libtls/include', 'userland/libway/include')]
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
    if args.yonder:
        membership = (ROOT/'tools/test_yonder_host.sh').read_text().split('host_sources=(',1)[1].split(')',1)[0]
        entries += [(ROOT/p, includes) for p in shlex.split(membership) if p != 'tools/test_yonder_host.c']
        entries += [(ROOT/p, includes) for p in ('userland/apps/yonder/svg.c',
            'userland/apps/yonder/svg_document.c', 'userland/libhtml/xml.c', 'userland/libxml/xml.c')]
    harness = 'test_yonder_svg.c' if args.yonder else 'test_svg_paint.c' if args.paint else 'test_svg_core.c'
    entries.append((ROOT/'tools'/harness, includes))

    def compile_one(entry):
        source, extra = entry
        obj = build/(str(source.relative_to(ROOT)).replace('/', '_')+'.o')
        subprocess.run([cc, *common, *extra, '-c', str(source), '-o', str(obj)],
                       check=True, cwd=ROOT/'userland')
        if args.yonder and source == ROOT/'userland/libos64/str.c':
            subprocess.run(['objcopy', '--localize-symbol=memcpy', '--localize-symbol=memmove',
                            '--localize-symbol=memset', '--localize-symbol=memcmp', str(obj)], check=True)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
        objects = list(workers.map(compile_one, entries))
    binary = build/('svg-yonder' if args.yonder else 'svg-paint' if args.paint else 'svg-core')
    subprocess.run([cc, *common, *map(str, objects), '-o', str(binary)], check=True)
    inputs = {p for p, _ in entries}
    inputs.update([ROOT/'tools/test_svg_gradient_sampler.inc', ROOT/'tools/test_svg_radial_sampler.inc'])
    if args.paint:
        inputs.add(ROOT/'tools/test_svg_cache.inc')
    if args.yonder:
        inputs.update([ROOT/'tools/test_yonder_host.c', ROOT/'tools/test_yonder_host.sh', ROOT/'tools/test_yonder_svg_use.inc', ROOT/'tools/test_yonder_svg_viewports.inc', ROOT/'tools/test_yonder_svg_clips.inc', ROOT/'tools/test_yonder_svg_bounds.inc', ROOT/'tools/test_yonder_svg_document.inc', ROOT/'tools/test_yonder_svg_gradients.inc', ROOT/'tools/test_yonder_svg_radial.inc', ROOT/'tools/test_svg_gradient_sampler.inc'])
        for directory in ('abi/include', 'tools', 'userland/apps/yonder',
                          'userland/libflow', 'userland/libhtml', 'userland/libxml', 'userland/libpage',
                          'userland/libgarb', 'userland/libfetch/include',
                          'userland/libgzip/include', 'userland/libtls/include',
                          'userland/libway/include'):
            inputs.update((ROOT/directory).rglob('*.h'))
    for directory in ('userland/libsvg', 'userland/libfreetype', 'userland/libmath', 'userland/libos64/include'):
        inputs.update((ROOT/directory).rglob('*.h'))
    inputs.update((ft.LIB/'upstream').rglob('*.c'))
    inputs.update([ROOT/'userland/libsvg/library.mk', ft.LIB/'sources.mk',
                   ROOT/'userland/libmath/sources.mk', Path(__file__).resolve()])
    provenance = {'compiler': subprocess.check_output([cc, '--version'], text=True).splitlines()[0],
                  'flags': common, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                  'inputs': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in sorted(inputs)}}
    (output/'provenance.json').write_text(json.dumps(provenance, indent=2)+'\n')
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    run = subprocess.run([str(binary), str(output)], capture_output=True, text=True, env=env, timeout=120)
    (output/'results.txt').write_text(run.stdout+run.stderr)
    print(run.stdout+run.stderr, end='')
    print('Binary:', binary)
    return run.returncode


if __name__ == '__main__':
    raise SystemExit(main())
