#!/usr/bin/env python3
"""One deliberate break at a time to the waited invalidation's rounds
(kernel/src/shootdown.c: how rounds are numbered, when a CPU's flush answers
a round, and the books), against tools/test_shootdown_host.sh: a mutant is
CAUGHT when the suite fails, MISSED when it passes, which means a test is
missing and gets written. A build failure is neither.

The order a flush handler keeps (read the round, flush, say so) is the
kernel's (inv_tlb_ISR) and the model's, not this core's, so no mutant here
can break it; the model is what shows that order is the one that works.

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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/shootdown-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CORE = 'kernel/src/shootdown.c'
CASES = 'bash tools/test_shootdown_host.sh 3000'

mutants = [
    # Numbering: a round must be newer than every round begun before it.
    ('begin-old-number', '__atomic_add_fetch(&s->round, 1, __ATOMIC_ACQ_REL)', '__atomic_fetch_add(&s->round, 1, __ATOMIC_ACQ_REL)'),
    ('begin-no-advance', '__atomic_add_fetch(&s->round, 1, __ATOMIC_ACQ_REL)', '__atomic_load_n(&s->round, __ATOMIC_ACQUIRE) + 1'),
    # A flush that claims more than it covers.
    ('covers-next-round', '\treturn __atomic_load_n(&s->round, __ATOMIC_ACQUIRE);\n}\n\n// One handler',
     '\treturn __atomic_load_n(&s->round, __ATOMIC_ACQUIRE) + 1;\n}\n\n// One handler'),
    ('flushed-not-said', '\t\t__atomic_store_n(&s->flushed[cpu], covers, __ATOMIC_RELEASE);\n', ''),
    ('flushed-wrong-cpu', '&s->flushed[cpu], covers', '&s->flushed[0], covers'),
    # Answered: at least as new, for that CPU.
    ('answered-strictly-newer', ' >= round;', ' > round;'),
    ('answered-older', ' >= round;', ' + 1 >= round;'),
    ('answered-any-cpu', '__atomic_load_n(&s->flushed[cpu], __ATOMIC_ACQUIRE) >= round', '__atomic_load_n(&s->flushed[1], __ATOMIC_ACQUIRE) >= round'),
    # The books.
    ('late-at-limit', 'bool late = waited_us > late_us;', 'bool late = waited_us >= late_us;'),
    ('total-rounds-uncounted', '\t__atomic_add_fetch(&s->rounds, 1, __ATOMIC_RELAXED);\n', ''),
    ('window-rounds-uncounted', '\t__atomic_add_fetch(&s->window_rounds, 1, __ATOMIC_RELAXED);\n', ''),
    ('total-late-uncounted', '\t\t__atomic_add_fetch(&s->late, 1, __ATOMIC_RELAXED);\n', ''),
    ('window-late-uncounted', '\t\t__atomic_add_fetch(&s->window_late, 1, __ATOMIC_RELAXED);\n', ''),
    ('total-most-not-raised', '\tif (raise_to(&s->wait_most_us, waited_us))', '\tif (false)'),
    ('window-most-not-raised', '\tif (raise_to(&s->window_wait_most_us, waited_us))', '\tif (false)'),
    ('raise-lowers', '\twhile (value > now)\n', '\twhile (value != now)\n'),
    ('most-cpu-kept', '\t\t__atomic_store_n(&s->wait_most_cpu, last_cpu, __ATOMIC_RELAXED);\n', ''),
    ('window-most-cpu-kept', '\t\t__atomic_store_n(&s->window_wait_most_cpu, last_cpu, __ATOMIC_RELAXED);\n', ''),
    ('most-cpu-any-round', '\tif (raise_to(&s->wait_most_us, waited_us))\n\t\t__atomic_store_n', '\traise_to(&s->wait_most_us, waited_us);\n\t\t__atomic_store_n'),
    ('read-window-kept', 'out->window_rounds = __atomic_exchange_n(&s->window_rounds, 0, __ATOMIC_RELAXED);',
     'out->window_rounds = __atomic_load_n(&s->window_rounds, __ATOMIC_RELAXED);'),
    ('read-window-late-kept', 'out->window_late = __atomic_exchange_n(&s->window_late, 0, __ATOMIC_RELAXED);',
     'out->window_late = __atomic_load_n(&s->window_late, __ATOMIC_RELAXED);'),
    ('read-window-most-kept', 'out->window_wait_most_us = __atomic_exchange_n(&s->window_wait_most_us, 0, __ATOMIC_RELAXED);',
     'out->window_wait_most_us = __atomic_load_n(&s->window_wait_most_us, __ATOMIC_RELAXED);'),
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

print(f'\nshootdown mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
