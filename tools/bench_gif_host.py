#!/usr/bin/env python3
"""Compare GIF decoder builds without drawing, file I/O or sanitizer overhead."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(args, work):
    includes = ['tools', 'userland/libimage', 'userland/libimage/include',
                'userland/libos64/include', 'userland/libpng/include',
                'userland/libjpeg/include', 'abi/include']
    flags = ['cc', '-std=c11', '-g', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
             '-fPIC', *['-I'+str(ROOT/p) for p in includes]]
    objects = []
    for name, source in [('bench', 'tools/bench_gif_host.c'),
                         ('image', 'userland/libimage/image.c')]:
        obj = work/(name+'.o')
        subprocess.run([*flags, '-O2', '-c', str(ROOT/source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    builds = []
    if args.baseline_ref:
        baseline = subprocess.check_output(['git', 'show',
            args.baseline_ref+':userland/libimage/gif.c'], cwd=ROOT)
        source = work/'baseline.c'
        source.write_bytes(baseline)
        builds.extend([('baseline-O0', source, '-O0'), ('baseline-O2', source, '-O2')])
    builds.append(('current-O2', ROOT/'userland/libimage/gif.c', '-O2'))
    hashes = {}
    for name, source, optimization in builds:
        obj = work/(name+'.o')
        hashes[name] = hashlib.sha256(source.read_bytes()).hexdigest()
        subprocess.run([*flags, optimization, '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run(['cc', *objects, str(obj), '-o', str(work/name)], check=True)
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    result = {'platform': platform.platform(), 'cpu': args.cpu,
              'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
              'baseline_ref': args.baseline_ref, 'decoder_sha256': hashes,
              'seconds_per_sample': args.seconds, 'repeats': args.repeats, 'files': []}
    names = [build[0] for build in builds]
    for path in args.files:
        samples = {name: [] for name in names}
        dimensions = None
        # Rotate build order to limit systematic warm-up/frequency bias.
        for rep in range(args.repeats):
            offset = rep % len(names)
            for name in names[offset:]+names[:offset]:
                fields = subprocess.check_output([str(work/name), str(path.resolve()),
                                                 str(args.seconds)], text=True).strip().split(',')
                metadata = [int(value) for value in fields[:4]]
                if dimensions is None:
                    dimensions = metadata
                assert metadata == dimensions, 'Dimensions, frames or owned bytes changed'
                samples[name].append(float(fields[4]))
        row = {'file': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
               'width': dimensions[0], 'height': dimensions[1], 'frames': dimensions[2],
               'owned_bytes': dimensions[3], 'samples_us': samples,
               'median_us': {name: statistics.median(values) for name, values in samples.items()}}
        result['files'].append(row)
        print(path.name+': '+', '.join(f'{name} {value:.3f} us/frame'
              for name, value in row['median_us'].items()), flush=True)
        if args.output:
            args.output.write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', type=Path, nargs='+', help='Valid multi-frame GIFs')
    parser.add_argument('--baseline-ref', help='Git revision to compare at -O0 and -O2')
    parser.add_argument('--seconds', type=float, default=0.25, help='CPU seconds per sample')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--cpu', type=int, help='Pin benchmark children to this host CPU')
    parser.add_argument('--output', type=Path, help='Write metadata and individual samples as JSON')
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0 or args.repeats < 1:
        parser.error('seconds and repeats must be positive and finite')
    if args.cpu is not None and (not hasattr(os, 'sched_getaffinity') or
                               args.cpu not in os.sched_getaffinity(0)):
        parser.error('cpu is outside the permitted host affinity set')
    with tempfile.TemporaryDirectory(prefix='gif-bench-') as directory:
        run(args, Path(directory))
