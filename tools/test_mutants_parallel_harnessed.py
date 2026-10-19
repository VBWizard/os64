#!/usr/bin/env python3
"""What tools/mutants_parallel.py leaves running: harnessed() runs a harness
call in a process group of its own and kills what is still in it when the
call returns, so a test driver whose shell a timeout killed does not go on;
and stopping the run (stop_all) ends every call it started, and any it starts
after.

    python3 tools/test_mutants_parallel_harnessed.py

The process-group code is taken out of the runner's source by its syntax
tree, so the runner itself never runs here.
"""
import ast
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import threading
import time

SOURCE = pathlib.Path(__file__).resolve().parent / 'mutants_parallel.py'


FUNCTIONS = {'harnessed', 'group_start', 'group_kill', 'group_done', 'stop_all'}
STATE = {'LIVE', 'LIVE_LOCK', 'STOPPING'}


def load_runner():
    tree = ast.parse(SOURCE.read_text())
    keep = [n for n in tree.body
            if (isinstance(n, ast.FunctionDef) and n.name in FUNCTIONS)
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in STATE for t in n.targets))]
    if len(keep) != len(FUNCTIONS) + len(STATE):
        sys.exit('test_mutants_parallel_harnessed: cannot find the process-group code in mutants_parallel.py')
    ns = {'os': os, 'signal': signal, 'subprocess': subprocess, 'threading': threading}
    exec(compile(ast.Module(body=keep, type_ignores=[]), str(SOURCE), 'exec'), ns)
    return ns


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A child reaped by nobody is a zombie: gone for this test's purposes.
    try:
        with open(f'/proc/{pid}/stat') as f:
            return f.read().split(') ')[-1].split()[0] != 'Z'
    except OSError:
        return True


def main():
    ns = load_runner()
    harnessed = ns['harnessed']
    failed = 0

    def check(ok, what):
        nonlocal failed
        print(('ok   ' if ok else 'FAIL ') + what)
        failed += not ok

    with tempfile.TemporaryDirectory(prefix='harnessed-test.') as d:
        pidfile = pathlib.Path(d) / 'pid'
        with open(os.devnull, 'w') as out:
            # The shape a timed-out case leaves: the shell is gone, and what it
            # started is still running under no parent.
            left = harnessed(['bash', '-c', f'sleep 300 & echo $! > {pidfile}; exit 0'], d, os.environ, out)
            pid = int(pidfile.read_text())
            time.sleep(0.2)
            check(left and not alive(pid), 'a process a harness call left behind is killed, and said')

            clean = harnessed(['bash', '-c', 'true'], d, os.environ, out)
            check(not clean, 'a harness call that leaves nothing says nothing')

            # A process that leaves its group is not the call's to kill.
            free = harnessed(['bash', '-c', f'setsid sleep 300 & echo $! > {pidfile}; sleep 0.2'], d, os.environ, out)
            pid = int(pidfile.read_text())
            check(alive(pid), 'a process in a session of its own is left alone')
            os.kill(pid, signal.SIGKILL)
            del free

            # The run is stopped while a call is running: the call ends, and
            # what it started dies with it.
            pidfile.unlink()
            done = []
            t = threading.Thread(target=lambda: done.append(harnessed(
                ['bash', '-c', f'sleep 300 & echo $! > {pidfile}; wait'], d, os.environ, out)))
            t.start()
            for _ in range(100):
                if pidfile.exists() and pidfile.read_text().strip():
                    break
                time.sleep(0.05)
            pid = int(pidfile.read_text())
            ns['stop_all']()
            t.join(10)
            time.sleep(0.2)
            check(not t.is_alive() and not alive(pid) and not ns['LIVE'],
                  'stopping the run ends a running call and kills what it started')

            # A call started once the run is stopping is killed as it starts;
            # one that is not would sleep out its 300 s, so it is waited for
            # 10 s and then said, not waited out.
            pidfile.unlink()
            t = threading.Thread(target=harnessed, daemon=True, args=(
                ['bash', '-c', f'echo $$ > {pidfile}; exec sleep 300'], d, os.environ, out))
            t.start()
            t.join(10)
            check(not t.is_alive() and not ns['LIVE'], 'a call started after the stop is killed at once')
            if t.is_alive() and pidfile.exists():
                os.killpg(int(pidfile.read_text()), signal.SIGKILL)
    print(f'mutants_parallel harnessed: {5 - failed} of 5 cases held')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
