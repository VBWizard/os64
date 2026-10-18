#!/usr/bin/env python3
"""Run mutant harnesses across many workers, each in its own new copy of the
tree, and say which mutants were caught.

    tools/mutants_parallel.py test_motion_mutants.py:7 test_fonts_mutants.py:4

A harness edits its sources in place, so two of them must never share a tree.
Each worker gets a copy of the directories the suites build from, made a git
tree of its own (the harnesses find their root with `git rev-parse`), and
runs its share of one harness's mutants there one at a time.

BEFORE ITS FIRST MUTANT, EVERY WORKER RUNS ITS HARNESS'S SUITES UNMUTATED,
and a worker whose suites fail runs no mutants at all. A harness calls any
failure of its cases a catch unless it sees a compiler's error, so a copy
missing something the build reads fails every mutant for a reason that has
nothing to do with it, and every one of them reads as caught. That happened:
copies without shared/, which the userland makefile reads, turned hundreds of
mutants whose cases build the JS runtime into catches that ran no check. The
suites are the `bash tools/<suite>.sh` commands the harness names; a harness
that names none is refused.

A harness's mutants are its `mutants = [` list, and each is run with
`--only <name>`. The harnesses match --only as a substring, so a name inside
another runs both; the summary keeps one verdict per name.

HOST_BUILD_JOBS (tools/host_build.sh) is set to --jobs in every worker, so
twenty workers compiling at once do not ask for twenty machines.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

# What a suite builds from. The userland makefile reads ../abi and ../shared.
TREE_DIRS = ('userland', 'tools', 'abi', 'shared')

parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
parser.add_argument('harnesses', nargs='+', help='HARNESS.py[:WORKERS], from tools/')
parser.add_argument('--src', type=pathlib.Path, help='the tree to copy (default: this one)')
parser.add_argument('--out', type=pathlib.Path, help='where the copies and logs go (default: a new temp directory)')
parser.add_argument('--with', dest='extra', action='append', default=[],
                    help='another top-level directory to copy')
parser.add_argument('--jobs', type=int, default=2, help='compiles at once in each worker (HOST_BUILD_JOBS)')
args = parser.parse_args()

src = args.src or pathlib.Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'],
                                                       text=True).strip())
out = args.out or pathlib.Path(tempfile.mkdtemp(prefix='mutants.'))
out.mkdir(parents=True, exist_ok=True)
dirs = TREE_DIRS + tuple(args.extra)
for d in dirs:
    if not (src / d).is_dir():
        sys.exit(f'mutants_parallel: {src / d} is not a directory')

NAME = re.compile(r"^\s{4}\('([^']+)',", re.M)
SUITE = re.compile(r"bash (tools/[\w]+\.sh(?: --[\w-]+)*)")
VERDICT = re.compile(r'^([a-z0-9][a-z0-9-]*): (caught|MISSED|did not build)$', re.M)


def plan(spec):
    harness, _, n = spec.partition(':')
    text = (src / 'tools' / harness).read_text()
    at = text.find('mutants = [')
    if at < 0:
        sys.exit(f'mutants_parallel: {harness} has no `mutants = [` list')
    names = NAME.findall(text[at:])
    suites = sorted(set(SUITE.findall(text)))
    if not names or not suites:
        sys.exit(f'mutants_parallel: {harness} names no mutants or no suites to check first')
    workers = max(1, min(int(n or 1), len(names)))
    per = -(-len(names) // workers)
    return [(harness, k, names[k * per:(k + 1) * per], suites) for k in range(workers)
            if names[k * per:(k + 1) * per]]


def work(job):
    harness, k, names, suites = job
    root = out / f'{harness[:-3]}-{k}'
    for d in dirs:
        shutil.copytree(src / d, root / d, symlinks=True)
    subprocess.run(['git', 'init', '-q', '.'], cwd=root, check=True)
    env = dict(os.environ, HOST_BUILD_JOBS=str(args.jobs))
    logs = root / 'logs'
    logs.mkdir()
    with (root / 'out.txt').open('w') as o:
        for suite in suites:
            with (logs / 'baseline.log').open('a') as b:
                b.write(f'$ bash {suite}\n')
                b.flush()
                if subprocess.run(['bash', '-c', f'bash {suite}'], cwd=root, env=env,
                                  stdout=b, stderr=b).returncode != 0:
                    o.write(f'BASELINE FAILED: bash {suite} (see {logs / "baseline.log"})\n')
                    return harness, k, names, None
        for name in names:
            o.flush()
            subprocess.run([sys.executable, f'tools/{harness}', '--only', name, '--logs', str(logs)],
                           cwd=root, env=env, stdout=o, stderr=subprocess.STDOUT)
    return harness, k, names, (root / 'out.txt').read_text()


jobs = [job for spec in args.harnesses for job in plan(spec)]
print(f'mutants_parallel: {sum(len(j[2]) for j in jobs)} mutants, {len(jobs)} workers, in {out}',
      flush=True)
verdicts, broken = {}, []
with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as pool:
    for harness, k, names, text in pool.map(work, jobs):
        if text is None:
            broken.append(f'{harness} worker {k} ({len(names)} mutants not run)')
            continue
        for name, verdict in VERDICT.findall(text):
            verdicts[name] = verdict
        for name in names:
            verdicts.setdefault(name, 'no verdict')

summary = sorted(f'{n}: {v}' for n, v in verdicts.items())
(out / 'summary.txt').write_text('\n'.join(summary) + '\n')
caught = sum(v == 'caught' for v in verdicts.values())
print(f'caught {caught} of {len(verdicts)}')
for line in summary:
    if not line.endswith(': caught'):
        print(line)
for b in broken:
    print(f'BASELINE FAILED, not run: {b}')
sys.exit(0 if caught == len(verdicts) and not broken else 1)
