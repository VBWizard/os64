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
is refused, and one that sets none gets BASELINE_SECONDS. A baseline that
fails or runs past it fails the worker; one that runs past it is killed with
what it left (sweep), and its mutants are summarized as "baseline failed": a
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
mutant, goes on running with no parent. So the runner is a subreaper (Linux):
an orphan below it is adopted by the runner, and every call is tagged in its
environment, so when a call returns, what it left is killed (sweep) and
named at the end as LEFT RUNNING. The calls stay in the runner's process
group, so the terminal's Ctrl-C, Ctrl-Z and Ctrl-\\ reach them as they reach
the runner. Every signal that would end the runner (SIGKILL, which nothing
can catch, aside) stops the run: the calls in flight are killed, no worker
starts another, each finishes moving its logs and removing its copy, what
the calls left is swept, and the run exits without a summary.

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
import ctypes
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


# What a call leaves behind is the runner's to stop. The runner is a
# subreaper (Linux's PR_SET_CHILD_SUBREAPER), so a process orphaned anywhere
# below it is adopted by the runner instead of by init, and every call
# carries CALL_VAR in its environment, which what it starts inherits: once a
# call returns, its leftovers are the runner's adopted children with its tag,
# and another worker's live calls, which carry their own, are never touched.
# A process that clears its environment carries no tag, and is not swept.
# The calls stay in the runner's process group, so the terminal's Ctrl-C,
# Ctrl-Z and Ctrl-\ reach them as they reach the runner.
CALL_VAR = 'MUTANTS_PARALLEL_CALL'
RUN_TAG = f'{os.getpid()}.{time.time_ns()}'
STOPPING = threading.Event()
LIVE = set()            # the calls in flight, as Popen objects
LIVE_LOCK = threading.Lock()


def subreaper():
    """Make this process the adopter of orphans below it; False where the
    system has no such thing (then a timed-out case's leftovers outlive it)."""
    try:
        return ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0   # PR_SET_CHILD_SUBREAPER
    except (OSError, AttributeError):
        return False


def adopted(tag):
    """This process's children that carry CALL_VAR `tag` (or, for a tag
    ending in ':', any tag it begins) and that no call in flight is: the
    processes a call left behind."""
    me, found = os.getpid(), []
    want = f'{CALL_VAR}={tag}'.encode()
    with LIVE_LOCK:
        live = {p.pid for p in LIVE}
    try:
        names = os.listdir('/proc')
    except OSError:
        return found
    for d in names:
        if not d.isdigit() or int(d) in live:
            continue
        try:
            with open(f'/proc/{d}/stat') as f:
                ppid = int(f.read().rsplit(')', 1)[1].split()[1])
            if ppid != me:
                continue
            with open(f'/proc/{d}/environ', 'rb') as f:
                env = f.read().split(b'\0')
        except (OSError, ValueError, IndexError):
            continue
        if any(e == want or (tag.endswith(':') and e.startswith(want)) for e in env):
            found.append(int(d))
    return found


def sweep(tag):
    """Kill and reap what the call tagged `tag` left behind. Killing one
    hands its children to the runner too, so it goes until none are left.
    True when there was something."""
    left = False
    for _ in range(100):
        pids = adopted(tag)
        if not pids:
            break
        left = True
        for pid in pids:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for pid in pids:
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
    return left


def call(cmd, tag, timeout=None, **kw):
    """Run one call tagged `tag`, in the runner's process group; past
    `timeout`, kill it. Its exit code, or 'timeout'."""
    env = dict(kw.pop('env', os.environ), **{CALL_VAR: tag})
    p = subprocess.Popen(cmd, env=env, **kw)
    with LIVE_LOCK:
        LIVE.add(p)
    if STOPPING.is_set():
        p.kill()
    try:
        return p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return 'timeout'
    finally:
        with LIVE_LOCK:
            LIVE.discard(p)


def stop():
    """The run is stopping: no worker starts another call, the calls in
    flight are killed, and what they leave is swept when the run ends."""
    STOPPING.set()
    with LIVE_LOCK:
        live = list(LIVE)
    for p in live:
        p.kill()


def bounded(cmd, tag, cwd, env, log, seconds):
    """Run cmd as call `tag`; past `seconds`, kill it and what it left."""
    code = call(['bash', '-c', cmd], tag, timeout=max(seconds, 0), cwd=cwd, env=env, stdout=log,
                stderr=log)
    sweep(tag)
    return code


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
        for i, suite in enumerate(suites):
            with (logs / 'baseline.log').open('a') as b:
                b.write(f'$ {suite}\n')
                b.flush()
                code = bounded(suite, f'{RUN_TAG}:{root.name}:baseline{i}', root, env, b,
                               deadline - time.monotonic())
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
            tag = f'{RUN_TAG}:{root.name}:{name}'
            call([sys.executable, f'tools/{harness}', '--only', name, '--logs', str(logs)], tag,
                 cwd=root, env=env, stdout=o, stderr=subprocess.STDOUT)
            if sweep(tag):
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


# Every signal that would end the runner stops the run the same way (stop):
# the calls in flight are killed, the workers start nothing more and finish
# moving their logs and removing their copies, and what the calls left is
# swept. From before the first job is submitted, so no window lets the pool's
# shutdown wait out jobs nobody stopped. The set is every signal the platform
# has but these, so a signal this list never heard of is covered too:
NOT_STOPS = {
    'SIGKILL', 'SIGSTOP',                       # nothing can catch them; SIGKILL leaves the calls running
    'SIGCHLD', 'SIGCONT', 'SIGURG', 'SIGWINCH',  # their default ends nothing
    'SIGTSTP', 'SIGTTIN', 'SIGTTOU',            # job control, the terminal's: the calls share its group
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
if not subreaper():
    print('mutants_parallel: this system cannot hand orphans to the runner: a case a harness '
          'times out may leave its processes running', flush=True)
results = []
try:
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as pool:
        try:
            futures = [pool.submit(work, job) for job in jobs]
            for f in futures:
                results.append(f.result())
        except KeyboardInterrupt:
            stop()
finally:
    sweep(f'{RUN_TAG}:')
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
