#!/usr/bin/env python3
"""Check callback lifetime oracles against retained matching runtime host objects.

Run JS_RUNTIME_KEEP=1 tools/test_js_runtime_host.sh first. This is negative
sensitivity evidence: each variant must reach a named behavioral assertion
before any sanitizer, engine invariant or process failure.
"""
import argparse
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
    artifacts, output = args.artifacts.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = root / 'userland/libjs/runtime/runtime.c'
    tickets = root / 'userland/libjs/runtime/owner_tickets.c'
    source = runtime.read_text()
    variants = {}

    def variant(name, old, new, path=runtime):
        original = path.read_text()
        if name in ('entry-follows-primary', 'incumbent-follows-entry', 'renew-deadline'):
            start = original.index('static os64_js_status_t owner_with_settings(')
            prefix, original = original[:start], original[start:]
        else:
            prefix = ''
        if original.count(old) != 1:
            raise RuntimeError('missing unique mutation anchor: ' + name)
        variants[name] = (path, prefix + original.replace(old, new))

    variant('capture-admitting-owner', 'facts.realm.owner, &incumbent);',
            'facts.admitting_owner, &incumbent);')
    variant('capture-borrowed-value', '.value=JS_DupValueRT(runtime->engine, callback),',
            '.value=callback,')
    variant('capture-raw-escape', '    JSOwnerSettingsSnapshot facts;\n',
            '    owner_legacy_escape(runtime);\n    JSOwnerSettingsSnapshot facts;\n')
    variant('capture-clears-raw-veto', '    JSOwnerSettingsSnapshot facts;\n',
            '    runtime->owner.metadata->table._unknown_escape = false;\n    JSOwnerSettingsSnapshot facts;\n')
    variant('capture-reuses-serial', '    uint64_t serial;\n    JSOwnerStatus result = js_owner_callback_serial_next',
            '    uint64_t serial;\n    metadata->callback_serial = 0;\n    JSOwnerStatus result = js_owner_callback_serial_next')
    variant('issuer-wrap', '    if (*issuer == UINT64_MAX) return JS_OWNER_EXHAUSTED;\n', '', tickets)
    variant('entry-follows-primary', 'bool admitted = JS_RunWithSettings(entry->context, incumbent ? incumbent->context : NULL,',
            'bool admitted = JS_RunWithSettings(primary(runtime)->context, incumbent ? incumbent->context : NULL,')
    variant('incumbent-follows-entry', 'bool admitted = JS_RunWithSettings(entry->context, incumbent ? incumbent->context : NULL,',
            'bool admitted = JS_RunWithSettings(entry->context, incumbent ? entry->context : NULL,')
    variant('incumbent-loan-pins-entry', '*incumbent_ticket, &incumbent_loan)',
            'entry_ticket, &incumbent_loan)')
    variant('early-entry-loan-end', '        JSValue value = JS_UNDEFINED;\n        bool admitted = JS_RunWithSettings',
            '        if (callback) js_owner_loan_end(&metadata->table, metadata->incarnation, invocation.call.scope.loan);\n'
            '        JSValue value = JS_UNDEFINED;\n        bool admitted = JS_RunWithSettings')
    variant('early-incumbent-loan-end', '        JSValue value = JS_UNDEFINED;\n        bool admitted = JS_RunWithSettings',
            '        if (callback) js_owner_loan_end(&metadata->table, metadata->incarnation, incumbent_loan);\n'
            '        JSValue value = JS_UNDEFINED;\n        bool admitted = JS_RunWithSettings')
    variant('borrowed-invocation-value', 'invocation.callback = JS_DupValueRT(runtime->engine, callback->value);',
            'invocation.callback = callback->value;')
    variant('snapshot-cleanup-without-frame', '        owner_callbacks_cleanup(runtime, invocation->callback);',
            '        runtime->settings_calls = NULL;\n        owner_callbacks_cleanup(runtime, invocation->callback);')
    variant('early-incumbent-holder-close', '            JS_FreeValueRT(runtime->engine, held);\n'
            '            if (js_owner_ticket_close(&metadata->table, metadata->incarnation, cell->incumbent) != JS_OWNER_OK)\n'
            '                jsport_fatal("callback incumbent cleanup", __FILE__, __LINE__);',
            '            if (js_owner_ticket_close(&metadata->table, metadata->incarnation, cell->incumbent) != JS_OWNER_OK)\n'
            '                jsport_fatal("callback incumbent cleanup", __FILE__, __LINE__);\n'
            '            JS_FreeValueRT(runtime->engine, held);')
    variant('cleanup-allows-reentry', '    metadata->callback_cleaning = true;',
            '    metadata->callback_cleaning = false;')
    variant('cleanup-stops-after-one', '            *cell = (OwnerCallback){.value=JS_UNDEFINED};',
            '            *cell = (OwnerCallback){.value=JS_UNDEFINED};\n            break;')
    variant('missing-root-drain', '    owner_callbacks_shutdown(runtime);',
            '    if (!runtime->metadata_selected) owner_callbacks_shutdown(runtime);')
    variant('renew-deadline', '    bool borrowed = runtime->active;\n    if (!runtime->turn',
            '    runtime->deadline = os64_micros() + (int64_t)(runtime->limits.execution_ms * 1000);\n'
            '    bool borrowed = runtime->active;\n    if (!runtime->turn')
    flags = ['cc', '-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
             '-ffreestanding', '-fno-builtin', '-fno-tree-loop-distribute-patterns',
             '-fno-stack-protector', '-DOS64_JS_TARGET', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all']
    for path in ['libmath/include', 'libjs/port', 'libjs/include', 'libjs/runtime', 'libos64/include']:
        flags += ['-I' + str(root / 'userland' / path)]
    flags += ['-I' + str(root / 'abi/include'), '-isystem', str(root / 'userland/obj/js/upstream')]
    observers = ['-DJS_Call=js_realm_observed_call',
                 '-Djs_owner_incarnation_next=js_owner_observed_incarnation_next',
                 '-Djs_owner_loan_begin=js_owner_observed_loan_begin',
                 '-Djs_owner_callback_serial_next=js_owner_observed_callback_serial_next',
                 '-Dos64_malloc=js_owner_observed_malloc', '-Dos64_free=js_owner_observed_free',
                 '-DJS_FreeContext=js_owner_observed_context_free']
    maths = []
    for line in subprocess.check_output(['make', '-s', '-C', str(root / 'userland'), 'math-host-flags'], text=True).splitlines():
        if line.startswith('MATH_OBJS='):
            maths = shlex.split(line.split('=', 1)[1])[0].split()
    if not maths:
        raise RuntimeError('missing target maths inventory')
    engines = {'target': [str(root / 'userland/obj/js/core.o')],
               'instrumented': [str(artifacts / (name + '.o')) for name in
                                ['quickjs', 'dtoa', 'libregexp', 'libunicode', 'cutils', 'allocator', 'platform', 'format']]}
    results = []
    for name, (path, mutated) in variants.items():
        unit = output / (name + '.c')
        unit.write_text(mutated.replace('#include "../intl/intl.h"',
                        '#include "' + str(root / 'userland/libjs/intl/intl.h') + '"'))
        obj = output / (name + '.o')
        subprocess.run(flags + (observers if path == runtime else []) + ['-c', str(unit), '-o', str(obj)], check=True)
        common = [str(obj) if path.stem == name else str(artifacts / (name + '.o')) for name in
                  ['runtime', 'owner_incarnation', 'owner_tickets', 'ledger', 'intl', 'str-local', 'fmt', 'test_js_port_calendar', 'test_js_runtime_host']]
        for mode, engine in engines.items():
            binary = output / (name + '-' + mode)
            subprocess.run(['cc', '-fsanitize=address,undefined', '-pthread', '-Wl,-z,noexecstack',
                            *engine, *maths, *common, '-o', str(binary)], check=True)
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1')
            run = subprocess.run([str(binary), '--owner-callbacks-controls'], capture_output=True,
                                 text=True, timeout=60, env=env)
            log = run.stdout + run.stderr
            (output / (name + '-' + mode + '.log')).write_text(log)
            failures = [line for line in log.splitlines() if line.startswith('FAIL: checked callbacks:')]
            if run.returncode != 1 or not failures or any(mark in log for mark in
                    ('ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'runtime error:', 'engine invariant failure')):
                raise RuntimeError('mutation escaped behavioral oracle: ' + name + '/' + mode + '\n' + log)
            results.append({'variant': name, 'mode': mode, 'oracle': failures[0]})
            print(name + '/' + mode + ': ' + failures[0], flush=True)
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(str(len(results)) + ' callback lifetime mutations rejected')


if __name__ == '__main__':
    main()
