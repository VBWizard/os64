#!/usr/bin/env python3
"""Replay host-Promise counterexamples using matching retained runtime artifacts.

Run JS_RUNTIME_KEEP=1 tools/test_js_runtime_host.sh first. Reused objects must
belong to this checkout. Mutants use an ASan-instrumented O0 engine to keep this
sensitivity check small; they are not positive production or leak evidence.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    artifacts = args.artifacts.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = (root / 'userland/obj/js/upstream/quickjs.c').read_text()
    species = ('ctor = JS_SpeciesConstructor(ctx, this_val, JS_UNDEFINED);\n'
               '    if (JS_IsException(ctor))\n        return ctor;\n'
               '    result_promise = js_new_promise_capability')
    variants = {
        'wrapped-resolver-admitted': ('revoked_fallback && direct', 'revoked_fallback && (direct || TRUE)'),
        'missing-native-admission': ('if (host && intrinsic)', 'if (FALSE && host && intrinsic)'),
        'missing-forwarding-admission': (
            'if (argc == 4) js_prepare_host_resolving_functions(args);',
            'if (FALSE && argc == 4) js_prepare_host_resolving_functions(args);'),
        'author-then-admitted': ('if (host && intrinsic)', 'if (host && (intrinsic || TRUE))'),
        'species-bypassed': (species, species.replace('JS_SpeciesConstructor(ctx, this_val, JS_UNDEFINED)', 'JS_UNDEFINED')),
    }
    flags = shlex.split('-O0 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin '
        '-fno-tree-loop-distribute-patterns -fno-stack-protector -DOS64_JS_TARGET '
        '-U__linux__ -fsanitize=address -fno-sanitize-recover=all -Wno-unused-parameter '
        '-Wno-sign-compare -Wno-missing-field-initializers -Wno-implicit-fallthrough')
    flags += ['-DCONFIG_VERSION="2026-06-04"']
    for path in ['userland/libmath/include', 'userland/libjs/port', 'userland/libjs/include',
                 'userland/libos64/include', 'abi/include', 'userland/obj/js/upstream',
                 'userland/libjs/port/compat']:
        flags += ['-I' + str(root / path)]
    maths = []
    for line in subprocess.check_output(['make', '-s', '-C', str(root / 'userland'), 'math-host-flags'], text=True).splitlines():
        if line.startswith('MATH_OBJS='):
            maths = shlex.split(line.split('=', 1)[1])[0].split()
    if not maths:
        raise RuntimeError('missing target maths inventory')
    common = [str(artifacts / (name + '.o')) for name in [
        'dtoa', 'libregexp', 'libunicode', 'cutils', 'allocator', 'platform', 'format',
        'runtime', 'owner_incarnation', 'owner_tickets', 'intl', 'str-local', 'fmt',
        'test_js_port_calendar', 'test_js_runtime_host']]

    def run(item):
        name, (old, new) = item
        if source.count(old) != 1:
            raise RuntimeError('mutation must match once: ' + name)
        path = output / name
        path.mkdir()
        (path / 'quickjs.c').write_text(source.replace(old, new))
        with (path / 'build.log').open('w') as log:
            subprocess.run(['cc', *flags, '-c', str(path / 'quickjs.c'), '-o', str(path / 'quickjs.o')],
                           stdout=log, stderr=log, check=True, cwd=root)
            subprocess.run(['cc', '-fsanitize=address,undefined', '-pthread', '-Wl,-z,noexecstack',
                str(path / 'quickjs.o'), *common, *maths, '-o', str(path / 'probe')],
                stdout=log, stderr=log, check=True, cwd=root)
        result = subprocess.run([str(path / 'probe'), '--host-promise-capabilities'],
            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'}, capture_output=True,
            text=True, timeout=120, cwd=root)
        log = result.stdout + result.stderr
        (path / 'run.log').write_text(log)
        failures = [line for line in log.splitlines() if line.startswith('FAIL: host Promise:')]
        if result.returncode != 1 or not failures or 'Sanitizer' in log or 'runtime error:' in log:
            raise RuntimeError('mutation escaped normal behavioral oracle: ' + name + '\n' + log)
        print(name + ': ' + failures[0], flush=True)
        return {'variant': name, 'oracle': failures[0]}

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(run, variants.items()))
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(str(len(results)) + ' host Promise mutations rejected')


if __name__ == '__main__':
    main()
