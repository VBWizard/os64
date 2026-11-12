#!/usr/bin/env python3
"""Break realm ownership selections one at a time; compilation is not a kill.

Use tools/mutants_parallel.py test_js_realms_mutants.py:12 for isolated
workers, each with an unmodified passing baseline. Direct execution restores
the exact original runtime bytes after each case, including on interruption.
"""
import argparse
import pathlib
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--only')
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/js-realms-mutants'))
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)
RUNTIME = 'userland/libjs/runtime/runtime.c'
SUITE = 'tools/test_js_realms_host.sh'
mutants = [
    ('admission-ignored-headroom', 'if (required_headroom > os64_js_heap_headroom(runtime))',
     'if (false && required_headroom > os64_js_heap_headroom(runtime))'),
    ('admission-wrapper-unaccounted', 'return runtime->allocator.allocation_limit - runtime->allocator.charged_bytes;',
     'return runtime->limits.memory_bytes - runtime->allocator.charged_bytes;'),
    ('admission-create-no-recheck', 'os64_js_realm_preflight(runtime, caller_abi, required_headroom, outcome);',
     'os64_js_realm_preflight(runtime, caller_abi, required_headroom > 0 ? 1 : 0, outcome);'),
    ('admission-masks-allocator-failure', 'if (runtime->allocator.failures != 0)',
     'if (false && runtime->allocator.failures != 0)'),
    ('loader-primary', 'char *name = realm->loader.resolve != NULL\n        ? realm->loader.resolve(realm->loader.opaque, base, specifier) : NULL;',
     'char *name = realm->loader.resolve != NULL\n        ? primary(runtime)->loader.resolve(primary(runtime)->loader.opaque, base, specifier) : NULL;'),
    ('module-list-primary', 'for (struct os64_js_module *m = realm->modules; m != NULL; m = m->next)',
     'for (struct os64_js_module *m = primary(realm->runtime)->modules; m != NULL; m = m->next)'),
    ('watcher-then-primary', 'JS_Call(context, realm->promise_then, promise, 2, args)',
     'JS_Call(context, primary(realm->runtime)->promise_then, promise, 2, args)'),
    ('output-handle-primary', 'os64_write(realm->output_handle, bytes, size)',
     'os64_write(primary(runtime)->output_handle, bytes, size)'),
    ('console-depth-primary', 'write_indent(realm, realm->console_depth)',
     'write_indent(realm, primary(realm->runtime)->console_depth)'),
    ('exception-host-primary', 'os64_js_realm_t *realm = realm_for_context(runtime, context);\n    if (realm == NULL) jsport_fatal("exception context membership", __FILE__, __LINE__);',
     'os64_js_realm_t *realm = primary(runtime);\n    if (realm == NULL) jsport_fatal("exception context membership", __FILE__, __LINE__);'),
    ('late-module-owner-primary', 'caught(runtime, module->realm->context, reason);',
     'caught(runtime, primary(runtime)->context, reason);'),
    ('rejection-owner-primary', '*item = (Rejection){.realm_index = (uint32_t)(realm - runtime->realms),',
     '*item = (Rejection){.realm_index = 0,'),
    ('classic-run-primary', 'return complete_run(runtime, eval_text(realm, source, length, source_name, NULL, outcome), outcome);',
     'return complete_run(runtime, eval_text(primary(runtime), source, length, source_name, NULL, outcome), outcome);'),
    ('handled-removal-scoped', 'if (place != NULL) {\n        if (handled) {',
     'if (place != NULL) {\n        if (handled && &runtime->realms[(*place)->realm_index] == realm_for_context(runtime, context)) {'),
    ('nested-budget-reset', '.borrowed = borrowed, .inherited = runtime->turn, .nested = nested};',
     '.borrowed = borrowed, .inherited = false, .nested = nested};'),
    ('console-owner-caller', 'os64_js_realm_t *realm = data_realm(context, data[2]);',
     'os64_js_realm_t *realm = JS_GetContextOpaque(context);'),
]
source = root / RUNTIME
original = source.read_bytes()
failed = False
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    text = original.decode()
    if text.count(old) != 1:
        raise SystemExit(f'{name}: expected one mutation site, got {text.count(old)}')
    try:
        source.write_text(text.replace(old, new))
        with (args.logs / f'{name}.log').open('w') as log:
            try:
                result = subprocess.run(['bash', SUITE], cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=600)
            except subprocess.TimeoutExpired:
                print(f'{name}: timed out', flush=True)
                failed = True
                continue
        output = (args.logs / f'{name}.log').read_text()
        if re.search(r'(?<!runtime )error:|undefined reference|not found|No such file', output):
            verdict = 'did not build'
        elif result.returncode == 0:
            verdict = 'MISSED'
        elif re.search(r'FAIL:|ERROR: AddressSanitizer|runtime error:|libjs: engine invariant failure:', output):
            verdict = 'caught'
        else:
            verdict = 'unclassified failure'
        print(f'{name}: {verdict}', flush=True)
        failed |= verdict != 'caught'
    finally:
        source.write_bytes(original)
        if source.read_bytes() != original:
            raise SystemExit('runtime restoration failed')
raise SystemExit(failed)
