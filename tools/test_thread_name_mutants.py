#!/usr/bin/env python3
"""One deliberate break at a time to the thread name rule
(abi/include/os64/thread_name.h: where a name must end, which bytes it may
hold), against tools/test_thread_name_host.sh: a mutant is CAUGHT when the
suite fails, MISSED when it passes, which means a test is missing and gets
written. A build failure is neither.

Each mutant is applied to the tree's copy of the file, the suite is run,
and the file is put back from the bytes read before the change, checked
byte for byte, whatever happens. Run it through tools/mutants_parallel.py;
the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/thread-name-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CORE = 'abi/include/os64/thread_name.h'
CASES = 'bash tools/test_thread_name_host.sh'

mutants = [
    # Where a name must end: within the limit, its terminator included.
    ('limit-one-more', 'i < OS64_THREAD_NAME_MAX; i++', 'i <= OS64_THREAD_NAME_MAX; i++'),
    ('limit-one-less', 'i < OS64_THREAD_NAME_MAX; i++', 'i < OS64_THREAD_NAME_MAX - 1; i++'),
    ('unended-taken', '    }\n    return false;\n}', '    }\n    return true;\n}'),
    ('empty-refused', "if (c == '\\0')\n            return true;", "if (c == '\\0')\n            return i > 0;"),
    # Which bytes: printable ASCII, both edges in.
    ('space-refused', 'c < 0x20 ||', 'c <= 0x20 ||'),
    ('control-taken', 'c < 0x20 ||', 'c < 0x1F ||'),
    ('tilde-refused', 'c > 0x7E)', 'c >= 0x7E)'),
    ('del-taken', 'c > 0x7E)', 'c > 0x7F)'),
    ('no-byte-check', 'if (c < 0x20 || c > 0x7E)\n            return false;', '(void)0;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / CORE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {CORE}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=120)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {CORE} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h):\d+:\d+: error:', body) is None and 'undefined reference' not in body
    if not built:
        unbuilt.append(name)
        verdict = 'did not build'
    elif code != 0:
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nthread name mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
