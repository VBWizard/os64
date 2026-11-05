#!/usr/bin/env python3
"""test262 through libjs, held to tools/test262/expected/ (INTL.md § 8).

Usage (tools/test262_host.sh builds the runner and calls this):
  run.py --runner <bin> [--checkout <dir>] [--update] [--only <substring>] [dir ...]

A dir is a test262 directory under test/ (intl402/DateTimeFormat); with
none, every one that has an expectation file. Each test runs as test262's
INTERPRETING.md says: the harness's assert.js and sta.js, then the files
its `includes` names (doneprintHandle.js first for an async test), then
the test, once as written and once strict ("use strict" first) unless its
flags say onlyStrict, noStrict or raw. A module test is not run (libjs's
classic runner has no module host) and fails as such. A negative test
passes on an exception of its type in any phase: the runner does not tell
a parse-phase SyntaxError from a runtime one (intl402 has no negative
tests; a directory that does needs the phase told apart first).

THE GATE: every test of a directory passes, except those its expectation
file lists under [excluded], each with the reason it cannot pass here (a
locale, calendar or historical zone the data does not hold, a constructor
not shipped, a host hook the runner does not have). An excluded test that
passes is a failure too, so the list cannot go stale. --update rewrites
[excluded] from the run: what passes leaves, a new failure joins with the
reason `untriaged`, which no run accepts; reasons already written stay.
Exit 1 when the gate fails, 2 when the run could not be judged.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = os.path.join(HERE, 'expected')


def frontmatter(text):
    """The test's metadata: includes, flags, features, negative {phase, type}."""
    m = re.search(r'/\*---(.*?)---\*/', text, re.S)
    meta = {'includes': [], 'flags': [], 'features': [], 'negative': None}
    if not m:
        return meta
    lines = m.group(1).splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        key = re.match(r'^(includes|flags|features):\s*(.*)$', line)
        if key:
            name, rest = key.group(1), key.group(2).strip()
            if rest.startswith('['):
                meta[name] = [x.strip() for x in rest.strip('[]').split(',') if x.strip()]
            else:
                items = []
                while i + 1 < len(lines) and re.match(r'^\s+-\s', lines[i + 1]):
                    i += 1
                    items.append(lines[i].strip()[1:].strip())
                meta[name] = items
        elif re.match(r'^negative:', line):
            negative = {}
            while i + 1 < len(lines) and re.match(r'^\s+\w+:', lines[i + 1]):
                i += 1
                k, v = lines[i].strip().split(':', 1)
                negative[k.strip()] = v.strip()
            meta['negative'] = negative
        i += 1
    return meta


def tests_of(checkout, directory):
    root = os.path.join(checkout, 'test', directory)
    out = []
    for base, _, files in os.walk(root):
        for name in files:
            if name.endswith('.js') and '_FIXTURE' not in name:
                out.append(os.path.relpath(os.path.join(base, name), os.path.join(checkout, 'test')))
    return sorted(out)


def read_expected(directory):
    path = os.path.join(EXPECTED, directory + '.txt')
    excluded, header = {}, []
    if not os.path.exists(path):
        return path, excluded, header
    section = None
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if section is None and line.startswith('#'):
            header.append(line)
            continue
        if line == '[excluded]':
            section = 'excluded'
            continue
        if not line or line.startswith('#'):
            continue
        if section == 'excluded':
            test, _, reason = line.partition('\t')
            excluded[test] = reason
    return path, excluded, header


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', required=True)
    parser.add_argument('--checkout', default=os.environ.get('TEST262_DIR', '.vendor/test262'))
    parser.add_argument('--update', action='store_true')
    parser.add_argument('--only')
    parser.add_argument('dirs', nargs='*')
    args = parser.parse_args()
    if not os.path.exists(os.path.join(args.checkout, 'harness', 'assert.js')):
        sys.exit('test262: no checkout in %s; run tools/test262_fetch.sh' % args.checkout)
    dirs = args.dirs
    if not dirs:
        for base, _, files in os.walk(EXPECTED):
            for name in files:
                if name.endswith('.txt'):
                    dirs.append(os.path.relpath(os.path.join(base, name), EXPECTED)[:-4])
        dirs.sort()
    harness = {}

    def include(name):
        if name not in harness:
            harness[name] = open(os.path.join(args.checkout, 'harness', name), encoding='utf-8').read()
        return harness[name]

    scratch = tempfile.TemporaryDirectory(prefix='os64-test262.')   # removed at exit
    work = scratch.name
    failed = False
    for directory in dirs:
        listing, kinds = [], {}
        for test in tests_of(args.checkout, directory):
            if args.only and args.only not in test:
                continue
            text = open(os.path.join(args.checkout, 'test', test), encoding='utf-8').read()
            meta = frontmatter(text)
            flags = meta['flags']
            negative = meta['negative'] or {}
            if 'module' in flags:
                kinds[test] = 'module'
                continue
            if 'raw' in flags:
                body = text
            else:
                parts = ['assert.js', 'sta.js'] + (['doneprintHandle.js'] if 'async' in flags else []) + meta['includes']
                body = ''.join(include(p) + '\n' for p in parts) + text
            variants = []
            if 'onlyStrict' in flags:
                variants = [(test + ' (strict)', True)]
            elif 'noStrict' in flags or 'raw' in flags:
                variants = [(test, False)]
            else:
                variants = [(test, False), (test + ' (strict)', True)]
            for ident, strict in variants:
                path = os.path.join(work, '%d.js' % len(listing))
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(('"use strict";\n' if strict else '') + body)
                listing.append('%s\t%s\t%d\t%s' % (ident, path, 1 if 'async' in flags else 0,
                                                   negative.get('type', '-')))
        list_path = os.path.join(work, 'list')
        with open(list_path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(listing) + '\n')
        env = dict(os.environ, TZ='EST5EDT')
        run = subprocess.run([args.runner, list_path], capture_output=True, text=True, env=env)
        if run.returncode != 0:
            sys.stderr.write(run.stderr)
            print('test262: the runner stopped (%d) in %s' % (run.returncode, directory))
            sys.exit(2)
        results = {}
        for line in run.stdout.splitlines():
            ident, verdict, *why = line.split('\t')
            results[ident] = (verdict, why[0] if why else '')
        for test, kind in kinds.items():
            results[test] = ('FAIL', 'a %s test (no module host)' % kind)
        path, excluded, header = read_expected(directory)
        problems = []
        for ident, (verdict, why) in sorted(results.items()):
            if ident in excluded:
                if verdict == 'PASS':
                    problems.append('PASSES (excluded: %s): %s' % (excluded[ident], ident))
                elif excluded[ident].startswith('untriaged'):
                    problems.append('UNTRIAGED: %s: %s' % (ident, why))
            elif verdict != 'PASS':
                problems.append('FAIL: %s: %s' % (ident, why))
        for ident in sorted(set(excluded) - set(results)):
            if not args.only:
                problems.append('GONE (excluded, no such test): %s' % ident)
        passed = sum(1 for v, _ in results.values() if v == 'PASS')
        print('test262 %s: %d of %d pass, %d excluded' % (directory, passed, len(results), len(excluded)))
        for p in problems:
            print('  ' + p)
        if args.update and not args.only:
            keep = {}
            for ident, (verdict, why) in results.items():
                if verdict != 'PASS':
                    keep[ident] = excluded.get(ident, 'untriaged: ' + why)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'w', encoding='utf-8') as f:
                for line in header or ['# test262 %s, held by tools/test262/run.py (its docstring says how).' % directory]:
                    f.write(line + '\n')
                f.write('[excluded]\n')
                for ident in sorted(keep):
                    f.write('%s\t%s\n' % (ident, keep[ident]))
            print('  wrote %s' % path)
        elif problems:
            failed = True
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
