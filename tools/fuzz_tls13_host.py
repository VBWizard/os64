#!/usr/bin/env python3
"""Replay raw and authenticated TLS 1.3 flight mutations under ASan/UBSan/LSan.

Retains generated test keys, captured flights, native-host replay structs and
fuzz-current.txt. Replay directories are trusted local test artifacts, not an
input format for downloaded files. A failed case can be reproduced with
--replay DIRECTORY; its engine binary and all fixture files must be retained.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from test_tls13_engine_host import build, check

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--foundation', type=Path)
parser.add_argument('--output', type=Path)
parser.add_argument('--seconds', type=int, default=1800)
parser.add_argument('--replay', type=Path)
args = parser.parse_args()
os.environ.setdefault('ASAN_OPTIONS', 'detect_leaks=1')
if args.replay:
    work = args.replay.resolve()
    seed, iteration = (work/'fuzz-current.txt').read_text().split()
    subprocess.run([str(work/'engine'), str(work), '0', seed, iteration], check=True)
else:
    if args.seconds < 1:
        parser.error('--seconds must be positive')
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='tls13-fuzz-'))
    if args.output:
        work.mkdir()
    print(f'TLS 1.3 fuzz artifacts: {work}', flush=True)
    if args.foundation:
        archive = args.foundation.resolve()
    else:
        check(work)
        archive = work/'adapted/core.a'
    executable = build(archive, work, run_tests=False)
    with (work/'fuzz.log').open('w') as log:
        result = subprocess.run([str(executable), str(work), str(args.seconds)], stdout=log, stderr=subprocess.STDOUT)
    print((work/'fuzz.log').read_text(), end='')
    result.check_returncode()
