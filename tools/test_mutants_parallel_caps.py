#!/usr/bin/env python3
"""The ceilings tools/mutants_parallel.py puts on what it runs: a mutant that
prints without end is stopped at the file ceiling, one that allocates without
end at the memory ceiling, and a mutant beside them is judged as usual.

    python3 tools/test_mutants_parallel_caps.py

The runner itself is run, with small ceilings, on a tree this test makes with
mkdtemp and removes at the end: a harness of three mutants and the one suite
it names. Where `systemd-run --user --scope` does not work there is no memory
ceiling to test, and the mutant that allocates is left out and said so.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

RUNNER = pathlib.Path(__file__).resolve().parent / 'mutants_parallel.py'
FILE_MIB = 2
MEMORY_GIB = 1

HARNESS = r'''#!/usr/bin/env python3
import argparse, pathlib, subprocess, sys
parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path)
parser.add_argument('--only')
args = parser.parse_args()
SUITE = 'tools/fake_suite.sh'
# What each mutant makes the suite do.
mutants = [
    ('prints-without-end', 'yes a line of failure output'),
    ('allocates-without-end', "python3 -c 'x = [bytes([1]) * (1 << 20) * 64 for i in range(1 << 20)]'"),
    ('fails-quietly', 'false'),
    ('passes', 'true'),
]
for name, command in mutants:
    if args.only and args.only not in name:
        continue
    log = args.logs / f'{name}.log'
    with log.open('w') as out:
        code = subprocess.run(['bash', SUITE, command], stdout=out, stderr=out, timeout=120).returncode
    body = log.read_text(errors='replace')
    print(f'{name}: {"caught" if code != 0 else "MISSED"}', flush=True)
'''
SUITE = '#!/bin/bash\n# With no argument, the unmutated suite: it passes.\n[ $# -eq 0 ] && exit 0\neval "$1"\n'


def main():
    base = pathlib.Path(tempfile.mkdtemp(prefix='caps-test.'))
    failed = 0

    def check(ok, what):
        nonlocal failed
        print(('ok   ' if ok else 'FAIL ') + what)
        failed += not ok

    try:
        tree, out = base / 'tree', base / 'out'
        for d in ('userland', 'tools', 'abi', 'shared'):
            (tree / d).mkdir(parents=True)
        (tree / 'tools' / 'fake_mutants.py').write_text(HARNESS)
        (tree / 'tools' / 'fake_suite.sh').write_text(SUITE)
        scope = subprocess.run(['systemd-run', '--user', '--scope', '--quiet', '--collect', 'true'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0 \
            if shutil.which('systemd-run') else False
        names = ['prints-without-end', 'fails-quietly', 'passes'] + (['allocates-without-end'] if scope else [])
        run = subprocess.run([sys.executable, str(RUNNER), 'fake_mutants.py:2', '--src', str(tree), '--out', str(out),
                              '--file-mib', str(FILE_MIB), '--memory-gib', str(MEMORY_GIB), '--only', ','.join(names)],
                             capture_output=True, text=True, timeout=600)
        said = run.stdout
        check(f'no file past {FILE_MIB} MiB' in said, 'the run says its file ceiling')
        check(('no worker past' in said) == scope and ('NO MEMORY CEILING' in said) != scope,
              'the run says whether it has a memory ceiling')
        logs = list(out.glob('logs/*/prints-without-end.log'))
        check(len(logs) == 1 and 0 < logs[0].stat().st_size <= FILE_MIB << 20,
              'the mutant that prints without end wrote a log, and no more than the ceiling')
        check('fake_mutants prints-without-end: MISSED' not in said and 'prints-without-end: no verdict' not in said,
              'and is counted as caught')
        check('fake_mutants fails-quietly:' not in said, 'a mutant that fails is caught beside it')
        check('fake_mutants passes: MISSED' in said, 'a mutant that passes is missed, as ever')
        if scope:
            check('fake_mutants allocates-without-end: MISSED' not in said,
                  'the mutant that allocates without end was ended, not passed')
            check('\ncaught ' in said and ' of 4\n' in said, 'and the run reached its summary after it')
        else:
            print('skip the mutant that allocates without end: systemd-run --user --scope does not work here')
        biggest = max((f.stat().st_size for f in out.rglob('*') if f.is_file()), default=0)
        check(biggest <= FILE_MIB << 20, 'nothing the run left is larger than the ceiling')
    finally:
        shutil.rmtree(base)
    print(f'mutants_parallel caps: {"all cases held" if not failed else f"{failed} cases FAILED"}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
