#!/usr/bin/env python3
"""One deliberate break at a time to slice 3's ES modules — libjs's module
entry, yonder's module map, the script host's ordering and its error
reporting, the module preloads and the relList a loader asks before it
preloads — against the harnesses that hold each rule: a mutant is CAUGHT
when one of its harnesses fails, MISSED when they all pass, which means a
test is missing and gets written. A build failure is neither.

Each mutant is applied to the worktree's copy of its file, its harnesses
are run, and the file is put back from the bytes read before the change —
checked byte for byte — whatever happens. Nothing else is touched. Run it
from a clean tree, one run at a time, or through tools/mutants_parallel.py.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/modules-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

RUNTIME_C = 'userland/libjs/runtime/runtime.c'
MODULES_C = 'userland/apps/yonder/modules.c'
SCRIPTS_C = 'userland/apps/yonder/scripts.c'
YONDER_C = 'userland/apps/yonder/yonder.c'
WINDOW_C = 'userland/libdom/window.c'
REPORT_C = 'userland/libdom/report.c'
MIXIN_C = 'userland/libdom/mixin.c'
JS = 'tools/test_js_runtime_host.sh'
SCRIPTS = 'tools/test_yonder_scripts_host.sh'
DOM = 'tools/test_dom_host.sh'

# (name, file, old, new, harnesses). Each breaks one rule the slice
# states; none supplies a replacement expectation.
mutants = [
    # 1. libjs's module entry
    ('compile-resolves', RUNTIME_C,
     'JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_RESOLVE);',
     'JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);', (JS,)),
    ('meta-is-the-name', RUNTIME_C, 'module_meta(runtime->context, JS_VALUE_GET_PTR(value), url != NULL ? url : name) < 0',
     'module_meta(runtime->context, JS_VALUE_GET_PTR(value), url != NULL ? name : name) < 0', (JS,)),
    ('module-rejection-unhandled', RUNTIME_C, '    struct os64_js_module *module = handled ? NULL : module_of(runtime, promise);',
     '    struct os64_js_module *module = handled ? NULL : module_of(runtime, promise) != NULL ? NULL : NULL;', (JS,)),
    ('module-failure-unreported', RUNTIME_C,
     '        copy_text(module->failure.message, sizeof(module->failure.message), "module evaluation failed");\n    caught(runtime, context, reason);',
     '        copy_text(module->failure.message, sizeof(module->failure.message), "module evaluation failed");\n    (void)context;', (JS,)),
    ('sync-rejection-kept', RUNTIME_C, '        if (place != NULL && *place != NULL && *place != &rejection_removed) {',
     '        if (place != NULL && *place == NULL) {', (JS,)),
    ('state-never-pending', RUNTIME_C, '    default: return OS64_JS_MODULE_PENDING;', '    default: return OS64_JS_MODULE_FULFILLED;', (JS,)),
    ('compile-reports-to-host', RUNTIME_C,
     '            JSValue error = JS_GetException(runtime->context);\n            outcome->status = OS64_JS_EXCEPTION;',
     '            JSValue error = JS_GetException(runtime->context);\n            caught(runtime, runtime->context, error);\n            outcome->status = OS64_JS_EXCEPTION;',
     (JS,)),
    ('destroy-keeps-handles', RUNTIME_C, '    while (runtime->modules != NULL)\n        module_free(runtime, runtime->modules);',
     '    (void)module_free;', (JS,)),
    # 2. yonder's module map
    ('map-finds-nothing', MODULES_C, '        if (os64_streq(m->mods[i].name, name))\n            return (int32_t)i;',
     '        if (os64_streq(m->mods[i].name, name) && false)\n            return (int32_t)i;', (SCRIPTS,)),
    ('graph-ignores-fetching', MODULES_C, '        if (mod->state == MOD_FETCHING || mod->state == MOD_PENDING) {\n            result = YONDER_GRAPH_FETCHING;',
     '        if (mod->state == MOD_PENDING) {\n            result = YONDER_GRAPH_FETCHING;', (SCRIPTS,)),
    ('any-type-is-javascript', MODULES_C, '            typed = os64_dom_javascript_mime(h.value, h.value_len);',
     '            typed = true;', (SCRIPTS,)),
    ('bare-fails-the-graph', MODULES_C,
     '        record(m, from, why);\n        return;\n    }\n    int32_t dep = find(m, url);',
     '        fail(m, from, why);\n        return;\n    }\n    int32_t dep = find(m, url);', (SCRIPTS,)),
    ('syntax-error-fails-the-graph', MODULES_C, '        m->mods[i].state = MOD_BROKEN;', '        m->mods[i].state = MOD_FAILED;', (SCRIPTS,)),
    ('busy-compile-fails', MODULES_C, '        m->mods[i].state = MOD_PENDING;\n        return;',
     '        fail(m, i, "busy");\n        return;', (SCRIPTS,)),
    ('classic-import-from-page', MODULES_C,
     "    const char *script = from < 0 && m->net.script_base != NULL ? m->net.script_base(m->net.opaque, base) : NULL;",
     "    const char *script = NULL;", (SCRIPTS,)),
    ('broken-entry-not-rethrown', MODULES_C,
     '    if (m->mods[entry].handle == NULL && m->mods[entry].state == MOD_BROKEN)\n        m->mods[entry].handle = rethrower(m, entry);',
     '    (void)rethrower;', (SCRIPTS,)),
    ('dynamic-import-not-fetched', MODULES_C, '    if (m->net.run == NULL || !m->net.run(m->net.opaque, name, &response)) {',
     '    if (true) {', (SCRIPTS,)),
    ('takeover-unwalked', MODULES_C, '        m->taken = true;\n', '', (SCRIPTS,)),
    # 3. the script host's order and wiring
    ('parser-module-not-deferred', SCRIPTS_C,
     'parser && os64_html_attr(script, "async") == NULL ? MODE_DEFER : MODE_ASYNC',
     'parser ? MODE_ASYNC : MODE_ASYNC', (SCRIPTS,)),
    ('nomodule-runs', SCRIPTS_C, '    if (!s->options.modern || os64_html_attr(script, "nomodule") == NULL)\n        return false;',
     '    if (true)\n        return false;', (SCRIPTS,)),
    ('answer-progresses-nothing', SCRIPTS_C,
     '        yonder_modules_answered(s->modules, token, response);\n        modules_progress(s);',
     '        yonder_modules_answered(s->modules, token, response);', (SCRIPTS,)),
    ('step-never-settles', SCRIPTS_C, '    if (yonder_modules_settle(s->modules)) {', '    if (false) {', (SCRIPTS,)),
    ('modules-missing-with-modern', SCRIPTS_C, '    else if (kind == OS64_DOM_SCRIPT_MODULE)\n        yonder_diag_missing',
     '    if (kind == OS64_DOM_SCRIPT_MODULE)\n        yonder_diag_missing', (SCRIPTS,)),
    ('importmap-unrecorded', YONDER_C,
     '    if (g.stream.scripting && kind == OS64_DOM_SCRIPT_NONE && yonder_scripts_importmap(script))',
     '    if (false)', (SCRIPTS,)),
    ('stream-skips-modules', YONDER_C,
     '        (kind != OS64_DOM_SCRIPT_CLASSIC && (kind != OS64_DOM_SCRIPT_MODULE || !g.stream.modern)))',
     '        kind != OS64_DOM_SCRIPT_CLASSIC)', (SCRIPTS,)),
    # 4. libdom's two exports
    ('mime-any', WINDOW_C, '    return javascript_type(value + start, essence - start, false);',
     '    return javascript_type(value + start, essence - start, false) || length != (size_t)-1;',
     (SCRIPTS,)),
    ('connected-skips-modules', WINDOW_C,
     '        if (kind != OS64_DOM_SCRIPT_CLASSIC && kind != OS64_DOM_SCRIPT_MODULE) continue;',
     '        if (kind != OS64_DOM_SCRIPT_CLASSIC) continue;', (SCRIPTS,)),
    ('type-unreflected', MIXIN_C, '        {"type", R_TYPE, true, 1u << D_PROTO_SCRIPT}, ',
     '        ', (SCRIPTS,)),
    ('script-address-none', REPORT_C, '    return script != D_NO_SCRIPT ? dom->script_names[script].address : NULL;',
     '    return script != D_NO_SCRIPT && name == NULL ? dom->script_names[script].address : NULL;', (SCRIPTS,)),
    # 5. module preloads, and the relList a loader asks before it preloads
    ('preload-unwalked', YONDER_C, '    yonder_scripts_preload(g.stream.scripts);\n', '', (SCRIPTS,)),
    ('preload-any-as', SCRIPTS_C,
     '           (as == NULL || as->value[0] == \'\\0\' || os64_streq_nocase(as->value, "script"));',
     '           (as == NULL || as != NULL);', (SCRIPTS,)),
    ('preload-rel-case-kept', SCRIPTS_C,
     "            same = (start[i] >= 'A' && start[i] <= 'Z' ? start[i] - 'A' + 'a' : start[i]) == token[i];",
     '            same = start[i] == token[i];', (SCRIPTS,)),
    ('rel-list-is-class', MIXIN_C, '    return entry->rel_tokens ? "rel" : "class";', '    return entry->rel_tokens ? "class" : "class";',
     (DOM,)),
    ('rel-supports-ungated', MIXIN_C, '        if (k == 1 && !dom->options.modern) continue;',
     '        if (k == 1 && dom == NULL) continue;', (DOM,)),
    ('rel-supports-case-kept', MIXIN_C,
     "               (token[n] >= 'A' && token[n] <= 'Z' ? token[n] - 'A' + 'a' : token[n]) == kinds[k][n])",
     '               token[n] == kinds[k][n])', (DOM,)),
    ('class-supports-answers', MIXIN_C,
     '                                       : JS_ThrowTypeError(ctx, "DOMTokenList has no supported tokens");',
     '                                       : JS_NewBool(ctx, false);', (DOM,)),
    ('supports-judges-token', MIXIN_C,
     '        ok = magic == T_CONTAINS || magic == T_SUPPORTS ? d_string(dom, ctx, argv[converted], &given[converted])',
     '        ok = magic == T_CONTAINS ? d_string(dom, ctx, argv[converted], &given[converted])', (DOM,)),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, harnesses in mutants:
    if args.only and args.only not in name:
        continue
    path = root / rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    codes = []
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            for harness in harnesses:
                try:
                    result = subprocess.run(['bash', str(root / harness)], cwd=root, stdout=out, stderr=out,
                                            timeout=900)
                    codes.append(result.returncode)
                except subprocess.TimeoutExpired:
                    codes.append('timeout')
                if codes[-1] != 0:
                    break
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {rel} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h|inc):\d+:\d+: error:', body) is None and 'undefined reference' not in body
    if not built:
        unbuilt.append(name)
        verdict = 'did not build'
    elif any(code != 0 for code in codes):
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nmodules mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
