#!/usr/bin/env python3
"""Run mutant harnesses across many workers, each in its own new copy of the
tree, and say which mutants were caught.

    tools/mutants_parallel.py test_motion_mutants.py:7 test_fonts_mutants.py:4
    tools/mutants_parallel.py test_motion_mutants.py:2 --only name-one,name-two

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
suites are what the harness names: `bash tools/<suite>.sh` or `python3
tools/<suite>.py` with any --flags, or a bare quoted 'tools/<suite>.sh' or
'.py' it runs by path; a harness that names none is refused. A worker's
suites run in turn against ONE deadline, the shortest timeout the harness
gives its own case runs, because a harness may run several of them under one
case timeout. A `timeout=` names a number or a variable; for a variable, the
numbers assigned to it count. A harness whose timeout cannot be read that way
is refused, and one that sets none gets BASELINE_SECONDS. A baseline that fails or runs past it fails the worker, its process
group is killed, and its mutants are summarized as "baseline failed": a
baseline more patient than the harness would pass suites whose cases the
harness then times out and counts as caught.

A harness's mutants are its `mutants = [` list, split into as many balanced
shares as there are workers, and each is run with `--only <name>`. The
harnesses match --only as a substring, so a name inside another runs both;
a worker reports verdicts for its own share only, so a mutant another
worker ran by accident never stands in for one its own worker did not.

--only takes exact mutant names, from any of the harnesses named, and runs
those alone: a review round's few through the same copies, baselines and
cleanup as a full run. A name no harness has stops the run before it starts.

A RUN CLEANS UP AFTER ITSELF. A suite keeps its work directory when it fails,
so a person can look at the failure, and under a harness every caught mutant
is a failure: a day of runs once left tens of gigabytes in /tmp. So each
worker's TMPDIR is a directory inside its own copy (the suites make theirs
with `mktemp`, which honours it), and when a worker is done its logs move to
`<out>/logs/` and the copy goes, TMPDIR and all. The copy is removed only through discard(), which
refuses any path that is not directly inside the run's directory or does not
carry the mark the worker wrote when it made the copy. A worker whose
baseline failed keeps its copy, said with its path, because that failure is
the one someone has to look at.

HOST_BUILD_JOBS (tools/host_build.sh) is set to --jobs in every worker, so
twenty workers compiling at once do not ask for twenty machines.

A MUTANT CANNOT TAKE THE MACHINE WITH IT. Everything a worker runs, baseline
and mutants alike, runs with a ceiling on any one file it writes (--file-mib)
and, where `systemd-run --user --scope` works, in a control group with a
ceiling on memory and no swap (--memory-gib, for each worker). A mutant whose
broken loop prints without end is ended when its log reaches the first, and
one that allocates without end, or a harness reading back a log, is ended at
the second; either reads as a failed case or as "no verdict", and the run
goes on. The first line of a run says which ceilings it has.
tools/test_mutants_parallel_caps.py runs a mutant of each kind.

Each mutant's harness call is timed, and summary.txt gives the seconds
beside its verdict. A mutant that took more than SLOW_TIMES its harness's
median, and more than SLOW_SECONDS, is named at the end as SLOW: a stall
or a test that grew is said the run it happens, not found later in the
tail. The time is the whole `--only` call, so a name that is inside
another's counts both.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import shutil
import signal
import subprocess
import time
import sys
import tempfile

# What a suite builds from. The userland makefile reads ../abi and ../shared.
TREE_DIRS = ('userland', 'tools', 'abi', 'shared')
# The longest an unmutated suite may run when its harness sets no case limit.
BASELINE_SECONDS = 1800
# What one file may grow to, and what one worker's processes may hold in
# memory together, unless the command line says otherwise. A mutant is a
# program with a rule broken, and a broken loop can print or allocate without
# end: its log filled a disk, and the harness reading that log back filled
# memory, before either had a ceiling. No build here writes a file a sixth
# this size.
FILE_MIB = 256
MEMORY_GIB = 8
# A mutant is SLOW past this many times its harness's median, and past this
# many seconds (a fast harness's small jitter is not news).
SLOW_TIMES = 5
SLOW_SECONDS = 60

parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
parser.add_argument('harnesses', nargs='+', help='HARNESS.py[:WORKERS], from tools/')
parser.add_argument('--src', type=pathlib.Path, help='the tree to copy (default: this one)')
parser.add_argument('--out', type=pathlib.Path, help='where the copies and logs go (default: a new temp directory)')
parser.add_argument('--with', dest='extra', action='append', default=[],
                    help='another top-level directory to copy')
parser.add_argument('--jobs', type=int, default=2, help='compiles at once in each worker (HOST_BUILD_JOBS)')
parser.add_argument('--only', help='exact mutant names, comma-separated: run these alone')
parser.add_argument('--file-mib', type=int, default=FILE_MIB,
                    help='the largest file a suite or a mutant may write (default %(default)s)')
parser.add_argument('--memory-gib', type=int, default=MEMORY_GIB,
                    help="the most memory one worker's processes may hold (default %(default)s)")
args = parser.parse_args()
only = set(filter(None, (args.only or '').split(','))) or None

src = args.src or pathlib.Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'],
                                                       text=True).strip())
# Absolute, because every worker runs from inside its own copy.
out = (args.out or pathlib.Path(tempfile.mkdtemp(prefix='mutants.'))).resolve()
out.mkdir(parents=True, exist_ok=True)
dirs = TREE_DIRS + tuple(args.extra)
for d in dirs:
    if not (src / d).is_dir():
        sys.exit(f'mutants_parallel: {src / d} is not a directory')

NAME = re.compile(r"^\s{4}\('([^']+)',", re.M)
# A harness is not a suite, so a name ending _mutants.py is never one.
SUITE = re.compile(r"\b((?:bash|python3) tools/(?!\w*_mutants\.py)\w+\.(?:sh|py)(?: --[\w-]+)*)")
BARE = re.compile(r"""['"](tools/(?!\w*_mutants\.py)\w+\.(sh|py)(?: --[\w-]+)*)['"]""")
VERDICT = re.compile(r'^([a-z0-9][a-z0-9-]*): (caught|MISSED|did not build)$', re.M)
TIMEOUT = re.compile(r'\btimeout=(\w+)')


def case_limit(harness, text):
    """The shortest timeout the harness gives a case run, or BASELINE_SECONDS."""
    limits = []
    for t in TIMEOUT.findall(text):
        if t.isdigit():
            limits.append(int(t))
            continue
        assigned = [int(n) for line in re.findall(rf'^\s*{t}\s*=\s*(.*)$', text, re.M)
                    for n in re.findall(r'\b\d+\b', line)]
        if not assigned:
            sys.exit(f'mutants_parallel: cannot read {harness}\'s case timeout `timeout={t}`')
        limits.extend(assigned)
    return min(limits, default=BASELINE_SECONDS)


def plan(spec):
    harness, _, n = spec.partition(':')
    text = (src / 'tools' / harness).read_text()
    at = text.find('mutants = [')
    if at < 0:
        sys.exit(f'mutants_parallel: {harness} has no `mutants = [` list')
    names = NAME.findall(text[at:])
    if only is not None:
        names = [n for n in names if n in only]
        if not names:
            return []
    suites = set(SUITE.findall(text))
    suites |= {('bash ' if kind == 'sh' else 'python3 ') + path for path, kind in BARE.findall(text)}
    suites = sorted(suites)
    if not names or not suites:
        sys.exit(f'mutants_parallel: {harness} names no mutants or no suites to check first')
    limit = case_limit(harness, text)
    workers = max(1, min(int(n or 1), len(names)))
    share, extra = divmod(len(names), workers)
    jobs, at = [], 0
    for k in range(workers):
        size = share + (k < extra)
        jobs.append((harness, k, names[at:at + size], suites, limit))
        at += size
    return jobs


# Put before a command, this runs it with a ceiling on each file it or its
# children write (the writer is ended by SIGXFSZ at the ceiling) and with no
# core dumps. A program of its own and not a preexec_fn, which may not be
# used from the threads the workers are.
FILE_CAP = [sys.executable, '-c',
            'import os, resource, sys\n'
            'n = int(sys.argv[1])\n'
            'resource.setrlimit(resource.RLIMIT_FSIZE, (n, n))\n'
            'resource.setrlimit(resource.RLIMIT_CORE, (0, 0))\n'
            'os.execvp(sys.argv[2], sys.argv[2:])',
            str(args.file_mib << 20)]
# Put before that, this runs the command in a control group of its own that
# may hold so much memory and no swap; past it the kernel ends a process
# inside the group and nothing outside it. An address-space limit would do
# for less, but the sanitizers reserve terabytes of address space they never
# touch, and every suite here is built with one.
MEMORY_CAP = ['systemd-run', '--user', '--scope', '--quiet', '--collect',
              '-p', f'MemoryMax={args.memory_gib}G', '-p', 'MemorySwapMax=0', '--']


def memory_cap_works():
    try:
        return subprocess.run(MEMORY_CAP + ['true'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def capped(command):
    return (MEMORY_CAP if MEMORY_CAPPED else []) + FILE_CAP + command


def bounded(cmd, cwd, env, log, seconds):
    """Run cmd in its own process group; past `seconds`, kill all of it."""
    p = subprocess.Popen(capped(['bash', '-c', cmd]), cwd=cwd, env=env, stdout=log, stderr=log,
                         start_new_session=True)
    try:
        return p.wait(timeout=max(seconds, 0))
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        return 'timeout'


# The mark a worker writes in the copy it makes: this run's own words, so
# discard() deletes nothing it did not make.
MARK = '.mutants_parallel_copy'
RUN = f'mutants_parallel {os.getpid()} {time.time_ns()} {out}\n'


def discard(root):
    """Delete a worker's copy: a real directory directly inside `out` that
    carries this run's mark, and nothing else."""
    mark = root / MARK
    if (root.parent != out or root.is_symlink() or not root.is_dir() or mark.is_symlink()
            or not mark.is_file() or mark.read_text() != RUN):
        print(f'mutants_parallel: not deleting {root}: not a copy this run made', flush=True)
        return
    shutil.rmtree(root)


def work(job):
    harness, k, names, suites, limit = job
    root = out / f'{harness[:-3]}-{k}'
    root.mkdir()
    (root / MARK).write_text(RUN)
    for d in dirs:
        shutil.copytree(src / d, root / d, symlinks=True)
    subprocess.run(['git', 'init', '-q', '.'], cwd=root, check=True)
    tmp = root / 'tmp'
    tmp.mkdir()
    env = dict(os.environ, HOST_BUILD_JOBS=str(args.jobs), TMPDIR=str(tmp))
    logs = root / 'logs'
    logs.mkdir()
    with (root / 'out.txt').open('w') as o:
        deadline = time.monotonic() + limit
        for suite in suites:
            with (logs / 'baseline.log').open('a') as b:
                b.write(f'$ {suite}\n')
                b.flush()
                code = bounded(suite, root, env, b, deadline - time.monotonic())
                if code != 0:
                    o.write(f'BASELINE FAILED ({code}): {suite} (see {logs / "baseline.log"}; '
                            f'the copy is kept)\n')
                    return harness, k, names, None, {}
        seconds = {}
        for name in names:
            o.flush()
            began = time.monotonic()
            subprocess.run(capped([sys.executable, f'tools/{harness}', '--only', name, '--logs', str(logs)]),
                           cwd=root, env=env, stdout=o, stderr=subprocess.STDOUT)
            seconds[name] = time.monotonic() - began
    text = (root / 'out.txt').read_text()
    kept = out / 'logs' / root.name
    kept.parent.mkdir(exist_ok=True)
    shutil.move(str(logs), str(kept))
    shutil.move(str(root / 'out.txt'), str(kept / 'out.txt'))
    discard(root)
    return harness, k, names, text, seconds


jobs = [job for spec in args.harnesses for job in plan(spec)]
if only is not None:
    unknown = only - {n for job in jobs for n in job[2]}
    if unknown:
        sys.exit(f'mutants_parallel: no harness named has {", ".join(sorted(unknown))}')
MEMORY_CAPPED = memory_cap_works()
print(f'mutants_parallel: {sum(len(j[2]) for j in jobs)} mutants, {len(jobs)} workers, in {out}',
      flush=True)
print(f'mutants_parallel: no file past {args.file_mib} MiB; '
      + (f'no worker past {args.memory_gib} GiB of memory' if MEMORY_CAPPED else
         'NO MEMORY CEILING (systemd-run --user --scope does not work here)'), flush=True)
verdicts, broken, took = {}, [], {}
with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as pool:
    for harness, k, names, text, seconds in pool.map(work, jobs):
        for name, t in seconds.items():
            took[harness, name] = t
        if text is None:
            broken.append(f'{harness} worker {k} ({len(names)} mutants not run)')
            for name in names:
                verdicts[harness, name] = 'baseline failed'
            continue
        mine = set(names)
        for name, verdict in VERDICT.findall(text):
            if name in mine:
                verdicts[harness, name] = verdict
        for name in names:
            verdicts.setdefault((harness, name), 'no verdict')

def line(key):
    h, n = key
    t = f' ({took[key]:.0f} s)' if key in took else ''
    return f'{h[:-3]} {n}: {verdicts[key]}{t}'


summary = sorted(line(key) for key in verdicts)
(out / 'summary.txt').write_text('\n'.join(summary) + '\n')
caught = sum(v == 'caught' for v in verdicts.values())
print(f'caught {caught} of {len(verdicts)}')
for key in sorted(verdicts):
    if verdicts[key] != 'caught':
        print(line(key))
for harness in sorted({h for h, _ in took}):
    times = sorted(t for (h, _), t in took.items() if h == harness)
    median = times[len(times) // 2]
    for (h, n), t in sorted(took.items()):
        if h == harness and t > SLOW_SECONDS and t > SLOW_TIMES * median:
            print(f'SLOW: {h[:-3]} {n}: {t:.0f} s, {t / median:.0f}x the harness median of {median:.0f} s')
for b in broken:
    print(f'BASELINE FAILED, not run: {b}')
sys.exit(0 if caught == len(verdicts) else 1)
