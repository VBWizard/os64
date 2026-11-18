#!/usr/bin/env python3
"""js_bench_run.py — run the JavaScript engine benchmarks on one or more
engines, each benchmark alone, several times, and print the median score of
each (higher is faster: the suites' own score, which is work done per second).

    tools/js_bench_run.py [--suite DIR] [--runs N] [--cpu N] [--only NAMES]
                          [--write DIR] NAME=COMMAND...

NAME=COMMAND is an engine to time: the command that runs a script file,
which is appended to it, as in `stock=~/src/quickjs/qjs`, or
`os64="build/jsb-os64 -m 1024M -t 600000"`. The benchmarks are QuickJS's
pinned copies of bench-v8 and five of Octane, from the reference tree in
~/src/quickjs/tests (OS64_REFERENCE.txt there says what it is): each is
written as ONE file (the suite's base, the benchmark's own files, and a
harness printing a RESULT line), so an engine needs only `print` and no
`load`, which is what /bin/js offers. `--write DIR` writes those files for
copying to a machine that runs them itself (the P5's /home/jsbench) and
times nothing.

Each run is pinned to one CPU (`--cpu`, through taskset) so the scheduler
moving it between cores does not stand in the figures, and the engines take
turns run by run so a slow minute on the machine falls on each alike.
"""
import argparse
import os
import re
import shlex
import statistics
import subprocess
import sys
import time

V8 = ['richards', 'deltablue', 'crypto', 'raytrace', 'earley-boyer', 'regexp', 'splay', 'navier-stokes']
# Octane's five, chosen for what a heavy page does (JS_SPEED_PLAN.md § 1).
OCTANE = {
    'code-load': ['code-load.js'],
    'typescript': ['typescript.js', 'typescript-input.js', 'typescript-compiler.js'],
    'pdfjs': ['pdfjs.js'],
    'box2d': ['box2d.js'],
    'zlib': ['zlib.js', 'zlib-data.js'],
}
HARNESS = '''
var print = typeof print == "function" ? print : console.log;
BenchmarkSuite.RunSuites({
  NotifyResult: function (name, result) { print("RESULT " + name + " " + result); },
  NotifyError: function (name, error) { print("ERROR " + name + " " + error); },
  NotifyScore: function (score) { }
});
'''


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def benchmarks(suite):
    """Each benchmark as (name, its one-file source)."""
    v8 = os.path.join(suite, 'bench-v8')
    octane = os.path.join(suite, 'octane')
    out = []
    for name in V8:
        out.append(('v8/' + name, read(os.path.join(v8, 'base.js')) + read(os.path.join(v8, name + '.js')) + HARNESS))
    for name, files in OCTANE.items():
        # zlib's data file reads through `read`, which the suite's runner
        # stubs; the same stub here.
        body = 'var read = function () { };\n' + read(os.path.join(octane, 'base.js'))
        body += ''.join(read(os.path.join(octane, f)) for f in files)
        out.append(('octane/' + name, body + HARNESS))
    return out


def run_one(command, path, cpu):
    argv = (['taskset', '-c', str(cpu)] if cpu is not None else []) + shlex.split(command) + [path]
    began = time.monotonic()
    done = subprocess.run(argv, capture_output=True, text=True)
    elapsed = time.monotonic() - began
    scores = [float(m.group(1)) for m in re.finditer(r'^RESULT \S+ ([0-9.]+)$', done.stdout, re.M)]
    if done.returncode != 0 or not scores or 'ERROR' in done.stdout:
        tail = (done.stdout + done.stderr).strip().splitlines()[-3:]
        return None, elapsed, ' / '.join(tail)
    # A suite with several parts scores their geometric mean, as the suites do.
    product = 1.0
    for s in scores:
        product *= s
    return product ** (1.0 / len(scores)), elapsed, ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--suite', default=os.path.expanduser('~/src/quickjs/tests'))
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--cpu', type=int, default=2)
    ap.add_argument('--only', default='')
    ap.add_argument('--write', default='')
    ap.add_argument('--work', default='/tmp')
    ap.add_argument('engines', nargs='*')
    args = ap.parse_args()
    chosen = [b for b in benchmarks(args.suite)
              if not args.only or any(o in b[0] for o in args.only.split(','))]
    if args.write:
        os.makedirs(args.write, exist_ok=True)
        for name, body in chosen:
            path = os.path.join(args.write, name.replace('/', '-') + '.js')
            with open(path, 'w', encoding='utf-8') as f:
                f.write(body)
            print(path)
        return 0
    engines = [e.split('=', 1) for e in args.engines]
    if not engines or any(len(e) != 2 for e in engines):
        ap.error('name engines as NAME=COMMAND')
    os.makedirs(args.work, exist_ok=True)
    results = {}
    for name, body in chosen:
        path = os.path.join(args.work, 'jsbench-' + name.replace('/', '-') + '.js')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(body)
        for _ in range(args.runs):
            for ename, command in engines:
                score, elapsed, why = run_one(command, path, args.cpu)
                results.setdefault((name, ename), []).append((score, elapsed, why))
        os.unlink(path)
    width = max(len(n) for n, _ in chosen)
    head = ''.join('%14s' % e for e, _ in engines)
    ratio = '  ' + ' '.join('%s/%s' % (e, engines[0][0]) for e, _ in engines[1:])
    print('%-*s%s  (median of %d; spread is max/min)%s' % (width, 'benchmark', head, args.runs, ratio))
    for name, _ in chosen:
        row, medians = [], []
        for ename, _ in engines:
            runs = results[(name, ename)]
            good = [s for s, _, _ in runs if s is not None]
            if not good:
                row.append('%14s' % 'failed')
                medians.append(None)
                print('  %s on %s: %s' % (name, ename, runs[0][2]), file=sys.stderr)
                continue
            m = statistics.median(good)
            medians.append(m)
            row.append('%9.0f x%-4.2f' % (m, max(good) / min(good)))
        ratios = ['%5.2f' % (m / medians[0]) if m and medians[0] else '  -  ' for m in medians[1:]]
        print('%-*s%s  %s' % (width, name, ''.join(row), ' '.join(ratios)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
