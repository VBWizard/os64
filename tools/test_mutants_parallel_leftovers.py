#!/usr/bin/env python3
"""What tools/mutants_parallel.py leaves running: the runner adopts the
orphans below it (a subreaper), every call is tagged in its environment, and
when a call returns, what it left is killed (sweep); stopping the run kills
the calls in flight and anything started after.

    python3 tools/test_mutants_parallel_leftovers.py

The runner's code is taken out of its source by its syntax tree, so the
runner itself never runs here; this process is the subreaper.
"""
import ast
import ctypes
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import threading
import time

SOURCE = pathlib.Path(__file__).resolve().parent / 'mutants_parallel.py'
FUNCTIONS = {'subreaper', 'adopted', 'sweep', 'call', 'stop'}
STATE = {'CALL_VAR', 'RUN_TAG', 'STOPPING', 'LIVE', 'LIVE_LOCK'}


def load_runner():
    tree = ast.parse(SOURCE.read_text())
    keep = [n for n in tree.body
            if (isinstance(n, ast.FunctionDef) and n.name in FUNCTIONS)
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in STATE for t in n.targets))]
    if len(keep) != len(FUNCTIONS) + len(STATE):
        sys.exit('test_mutants_parallel_leftovers: cannot find the leftover code in mutants_parallel.py')
    ns = {'os': os, 'signal': signal, 'subprocess': subprocess, 'threading': threading, 'time': time,
          'ctypes': ctypes}
    exec(compile(ast.Module(body=keep, type_ignores=[]), str(SOURCE), 'exec'), ns)
    return ns


def gone(pid):
    """Neither running nor a zombie: killed and reaped."""
    return not os.path.exists(f'/proc/{pid}')


def wait_for(path):
    for _ in range(200):
        if path.exists() and path.read_text().strip():
            return int(path.read_text())
        time.sleep(0.05)
    raise SystemExit(f'test_mutants_parallel_leftovers: {path} never written')


def main():
    ns = load_runner()
    call, sweep, tag0 = ns['call'], ns['sweep'], ns['RUN_TAG']
    failed = 0
    total = 0

    def check(ok, what):
        nonlocal failed, total
        total += 1
        print(('ok   ' if ok else 'FAIL ') + what)
        failed += not ok

    check(ns['subreaper'](), 'this process becomes the subreaper')
    with tempfile.TemporaryDirectory(prefix='leftovers-test.') as d:
        d = pathlib.Path(d)
        kw = dict(cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        stray = []

        # The shape a timed-out case leaves: the shell is gone, what it
        # started runs on, and the runner has adopted it.
        call(['bash', '-c', f'sleep 300 & echo $! > {d}/a; exit 0'], f'{tag0}:a', **kw)
        pid = wait_for(d / 'a')
        stray.append(pid)
        check(sweep(f'{tag0}:a') and gone(pid), 'what a call left behind is killed and reaped, and said')

        call(['bash', '-c', 'true'], f'{tag0}:clean', **kw)
        check(not sweep(f'{tag0}:clean'), 'a call that left nothing says nothing')

        call(['bash', '-c', f'setsid sleep 300 & echo $! > {d}/s; exit 0'], f'{tag0}:s', **kw)
        pid = wait_for(d / 's')
        stray.append(pid)
        check(sweep(f'{tag0}:s') and gone(pid), 'one that left the session is still the call\'s, and killed')

        # Two levels: killing the adopted subshell hands its child over too.
        call(['bash', '-c', f'( sleep 300 & echo $! > {d}/deep; wait ) & exit 0'], f'{tag0}:deep', **kw)
        pid = wait_for(d / 'deep')
        stray.append(pid)
        check(sweep(f'{tag0}:deep') and gone(pid), 'what a leftover started is swept with it')

        # Another call's leftovers are not this call's to kill.
        call(['bash', '-c', f'sleep 300 & echo $! > {d}/b; exit 0'], f'{tag0}:b', **kw)
        b = wait_for(d / 'b')
        stray.append(b)
        call(['bash', '-c', f'sleep 300 & echo $! > {d}/c; exit 0'], f'{tag0}:c', **kw)
        c = wait_for(d / 'c')
        stray.append(c)
        check(sweep(f'{tag0}:c') and gone(c) and not gone(b), 'sweeping one call leaves another call\'s alone')
        check(sweep(f'{tag0}:b') and gone(b), 'and that call\'s own sweep takes them')

        # A tag that is a prefix of another names only itself.
        call(['bash', '-c', f'sleep 300 & echo $! > {d}/x10; exit 0'], f'{tag0}:x10', **kw)
        x10 = wait_for(d / 'x10')
        stray.append(x10)
        check(not sweep(f'{tag0}:x1') and not gone(x10), 'a tag is matched whole, not as a prefix')

        # Stopping the run: the call in flight ends, and what it started is
        # the run's sweep's.
        done = []
        t = threading.Thread(target=lambda: done.append(call(
            ['bash', '-c', f'sleep 300 & echo $! > {d}/live; wait'], f'{tag0}:live', **kw)))
        t.start()
        pid = wait_for(d / 'live')
        stray.append(pid)
        ns['stop']()
        t.join(10)
        check(not t.is_alive() and sweep(f'{tag0}:') and gone(pid) and gone(x10),
              'stopping the run ends the call in flight, and the run\'s sweep takes everything it left')

        # A call started once the run is stopping is killed at once; one that
        # is not would sleep out its 300 s, so it is given 10 s and then said.
        began = time.monotonic()
        t = threading.Thread(target=call, daemon=True, args=(['bash', '-c', 'sleep 300'], f'{tag0}:late'),
                             kwargs=kw)
        t.start()
        t.join(10)
        check(not t.is_alive() and time.monotonic() - began < 10, 'a call started after the stop is killed at once')

        for pid in stray:       # a failed case must not leave its sleeper behind
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        sweep(f'{tag0}:')
    print(f'mutants_parallel leftovers: {total - failed} of {total} cases held')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
