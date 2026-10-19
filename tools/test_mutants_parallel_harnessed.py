#!/usr/bin/env python3
"""What tools/mutants_parallel.py leaves running: harnessed() runs a harness
call in a process group of its own and kills what is still in it when the
call returns, so a test driver whose shell a timeout killed does not go on.

    python3 tools/test_mutants_parallel_harnessed.py

harnessed() is taken out of the runner's source by its syntax tree, so the
runner itself never runs here.
"""
import ast
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time

SOURCE = pathlib.Path(__file__).resolve().parent / 'mutants_parallel.py'


def load_harnessed():
    tree = ast.parse(SOURCE.read_text())
    keep = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'harnessed']
    if len(keep) != 1:
        sys.exit('test_mutants_parallel_harnessed: cannot find harnessed() in mutants_parallel.py')
    ns = {'os': os, 'signal': signal, 'subprocess': subprocess}
    exec(compile(ast.Module(body=keep, type_ignores=[]), str(SOURCE), 'exec'), ns)
    return ns['harnessed']


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
    harnessed = load_harnessed()
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
    print(f'mutants_parallel harnessed: {3 - failed} of 3 cases held')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
