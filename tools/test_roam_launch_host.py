#!/usr/bin/env python3
"""Run production launch/config code and Roam file-operation lifecycle on host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path)
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
out = args.output or Path(tempfile.mkdtemp(prefix='roam-launch-host-'))
out.mkdir(parents=True, exist_ok=True)
binary = out / 'test_roam_launch'
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections', '-include', str(root / 'tools/test_file_dialog_syscalls.h')]
flags += ['-I' + str(root / p) for p in
          ['userland/libos64/include', 'userland/libimage/include', 'abi/include']]
sources = ['tools/test_roam_launch_host.c'] + [
    'userland/libos64/' + name + '.c' for name in ['launch', 'conf', 'slurp', 'str', 'fmt']]
subprocess.run(['cc', *flags, *[str(root / s) for s in sources], '-o', str(binary)], check=True)
print('Artifacts:', out, flush=True)
if not args.build_only:
    raise SystemExit(subprocess.run([str(binary)], env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'}).returncode)
