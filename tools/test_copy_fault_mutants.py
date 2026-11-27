#!/usr/bin/env python3
"""One deliberate break at a time to the user copy's fault rule
(kernel/src/copy_fault.c: which ring-0 faults resume at a copy's failure
exit), against tools/test_copy_fault_host.sh: a mutant is CAUGHT when the
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/copy-fault-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CORE = 'kernel/src/copy_fault.c'
CASES = 'bash tools/test_copy_fault_host.sh'

mutants = [
    # Ring 3's faults are never a copy's.
    ('ring3-recovers', 'error_code & (COPY_FAULT_USER | COPY_FAULT_RESERVED)', 'error_code & (COPY_FAULT_RESERVED)'),
    # A reserved bit is a broken table, never a copy's race.
    ('reserved-recovers', 'error_code & (COPY_FAULT_USER | COPY_FAULT_RESERVED)', 'error_code & (COPY_FAULT_USER)'),
    ('any-error-refused', 'error_code & (COPY_FAULT_USER | COPY_FAULT_RESERVED)', 'error_code'),
    # The address must be one the check could have passed.
    ('page-zero-recovers', 'cr2 < page_size ||', 'cr2 < (page_size & 0) + 1 ||'),
    ('first-page-refused', 'cr2 < page_size ||', 'cr2 <= page_size ||'),
    ('ceiling-recovers', 'cr2 >= user_ceiling', 'cr2 > user_ceiling'),
    ('kernel-half-recovers', 'cr2 >= user_ceiling', '(cr2 >= user_ceiling && 0)'),
    # Only a table instruction, and its own exit.
    ('any-rip-recovers', '        if (table[i].at == rip)', '        if (table[i].at == rip || 1)'),
    ('last-entry-skipped', 'for (uint64_t i = 0; i < count; i++)', 'for (uint64_t i = 0; i + 1 < count; i++)'),
    ('first-entry-skipped', 'for (uint64_t i = 0; i < count; i++)', 'for (uint64_t i = 1; i < count; i++)'),
    ('wrong-exit', '            return table[i].resume;', '            return table[i].at;'),
    ('next-entry-exit', '            return table[i].resume;', '            return table[i + 1 < count ? i + 1 : i].resume;'),
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

print(f'\ncopy fault mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
