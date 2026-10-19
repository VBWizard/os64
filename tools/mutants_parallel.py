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

NOTHING A MUTANT STARTED OUTLIVES IT. A harness times a case out by killing
the shell it started, and what that shell ran, a test driver looping on the
mutant, goes on running with no parent. So each harness call has a process
group of its own (harnessed()), and when it returns whatever is still in the
group is killed and named at the end as LEFT RUNNING. A group of its own is
also out of the terminal's reach, so stopping the run is the runner's job:
Ctrl-C, Ctrl-\\, a kill or a closed terminal (every signal that would end it
but SIGKILL, which nothing can catch) kills every group it started
(baselines included), no worker starts another, and each finishes moving its
logs and removing its copy before the run exits without a summary. Ctrl-Z
stops the groups with the runner, and continuing it continues them.

HOST_BUILD_JOBS (tools/host_build.sh) is set to --jobs in every worker, so
twenty workers compiling at once do not ask for twenty machines.

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
import threading

# What a suite builds from. The userland makefile reads ../abi and ../shared.
TREE_DIRS = ('userland', 'tools', 'abi', 'shared')
# The longest an unmutated suite may run when its harness sets no case limit.
BASELINE_SECONDS = 1800
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
LEFT = re.compile(r"^LEFT RUNNING: (\S+)'s cases", re.M)


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


# Every process group the runner has started and not yet finished with. A
# group of its own keeps a suite or a harness out of the terminal's reach, so
# Ctrl-C reaches the runner alone: stop_all() is what stops them, and a group
# started once the run is stopping is killed as soon as it exists.
LIVE = set()
LIVE_LOCK = threading.Lock()
STOPPING = threading.Event()


def group_start(cmd, **kw):
    """Popen in a process group of its own, known to stop_all()."""
    p = subprocess.Popen(cmd, start_new_session=True, **kw)
    with LIVE_LOCK:
        LIVE.add(p.pid)
    if STOPPING.is_set():
        group_kill(p.pid)
    return p


def group_kill(pgid):
    """SIGKILL a process group; False when nothing is left in it."""
    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        return False
    return True


def group_done(p):
    with LIVE_LOCK:
        LIVE.discard(p.pid)


def stop_all():
    """The run is stopping: no worker starts another call, and every group
    the runner started is killed."""
    STOPPING.set()
    with LIVE_LOCK:
        groups = list(LIVE)
    for g in groups:
        group_kill(g)


def bounded(cmd, cwd, env, log, seconds):
    """Run cmd in its own process group; past `seconds`, kill all of it."""
    p = group_start(['bash', '-c', cmd], cwd=cwd, env=env, stdout=log, stderr=log)
    try:
        return p.wait(timeout=max(seconds, 0))
    except subprocess.TimeoutExpired:
        group_kill(p.pid)
        p.wait()
        return 'timeout'
    finally:
        group_done(p)


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


def harnessed(cmd, cwd, env, out):
    """Run one harness call in a process group of its own, then kill whatever
    is still in that group. A harness that times a case out kills only the
    shell it started, and what the shell ran (a test driver looping on the
    mutant) goes on; one did, on a core, for half an hour after its verdict.
    True when something was left to kill."""
    p = group_start(cmd, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
    try:
        p.wait()
        return group_kill(p.pid)
    finally:
        group_done(p)


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
                # A baseline the stop killed failed nothing: no copy to keep.
                if STOPPING.is_set():
                    break
                if code != 0:
                    o.write(f'BASELINE FAILED ({code}): {suite} (see {logs / "baseline.log"}; '
                            f'the copy is kept)\n')
                    return harness, k, names, None, {}
        seconds = {}
        for name in names:
            if STOPPING.is_set():
                break
            o.flush()
            began = time.monotonic()
            if harnessed([sys.executable, f'tools/{harness}', '--only', name, '--logs', str(logs)],
                         root, env, o):
                o.write(f'LEFT RUNNING: {name}\'s cases left processes behind; killed\n')
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
print(f'mutants_parallel: {sum(len(j[2]) for j in jobs)} mutants, {len(jobs)} workers, in {out}',
      flush=True)
verdicts, broken, took, left = {}, [], {}, []


def interrupted(signum, frame):
    raise KeyboardInterrupt


def suspended(signum, frame):
    """Ctrl-Z: the groups are out of the terminal's reach, so the runner
    stops them before it stops itself, and continues them when it is
    continued."""
    with LIVE_LOCK:
        groups = list(LIVE)
    for g in groups:
        try:
            os.killpg(g, signal.SIGSTOP)
        except ProcessLookupError:
            pass
    os.kill(os.getpid(), signal.SIGSTOP)
    with LIVE_LOCK:
        groups = list(LIVE)
    for g in groups:
        try:
            os.killpg(g, signal.SIGCONT)
        except ProcessLookupError:
            pass


# Every signal that would end the runner stops the run the same way: every
# group it started is killed (stop_all), and the workers, which start nothing
# more, finish moving their logs and removing their copies. From before the
# first job is submitted, so no window lets the pool's shutdown wait out jobs
# nobody stopped. The set is every signal the platform has but these, so a
# signal this list never heard of is covered too:
NOT_STOPS = {
    'SIGKILL', 'SIGSTOP',                       # nothing can catch them; SIGKILL leaves the groups running
    'SIGCHLD', 'SIGCONT', 'SIGURG', 'SIGWINCH',  # their default ends nothing
    'SIGTSTP', 'SIGTTIN', 'SIGTTOU',            # job control: SIGTSTP is suspended()'s
    'SIGPIPE',                                  # Python ignores it (a closed pipe is an OSError)
    'SIGSEGV', 'SIGBUS', 'SIGFPE', 'SIGILL',    # the interpreter's own faults: no handler of
}                                               # Python's can run after one
for sig in signal.valid_signals():
    try:
        name = signal.Signals(sig).name
    except ValueError:      # a real-time signal between SIGRTMIN and SIGRTMAX has no name
        name = None
    if name not in NOT_STOPS:
        signal.signal(sig, interrupted)
signal.signal(signal.SIGTSTP, suspended)
results = []
with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as pool:
    try:
        futures = [pool.submit(work, job) for job in jobs]
        for f in futures:
            results.append(f.result())
    except KeyboardInterrupt:
        stop_all()
if STOPPING.is_set():
    sys.exit(f'mutants_parallel: interrupted; every process it started is stopped, and the logs so far '
             f'are in {out / "logs"}')
for harness, k, names, text, seconds in results:
    for name, t in seconds.items():
        took[harness, name] = t
    if text is not None:
        left += [f'{harness[:-3]} {n}' for n in LEFT.findall(text)]
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
for n in left:
    print(f'LEFT RUNNING, killed: {n}')
for b in broken:
    print(f'BASELINE FAILED, not run: {b}')
sys.exit(0 if caught == len(verdicts) else 1)
