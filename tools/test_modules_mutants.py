#!/usr/bin/env python3
"""One deliberate break at a time to slice 3's ES modules — libjs's module
entry, yonder's module map, the script host's ordering and its error
reporting, the module preloads and the relList a loader asks before it
preloads — and to the import maps after them, against the harnesses that
hold each rule: a mutant is CAUGHT
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
IMPORTMAP_C = 'userland/apps/yonder/importmap.c'
LIBDOM_MAP_C = 'userland/libdom/importmap.c'
JS = 'tools/test_js_runtime_host.sh'
SCRIPTS = 'tools/test_yonder_scripts_host.sh'
DOM = 'tools/test_dom_host.sh'
IMPORTMAP = 'tools/test_importmap_host.sh'

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
     '    if (g.stream.scripting && kind == OS64_DOM_SCRIPT_IMPORTMAP && !g.stream.modern)',
     '    if (false)', (SCRIPTS,)),
    ('stream-skips-modules', YONDER_C,
     '        (kind != OS64_DOM_SCRIPT_CLASSIC && !g.stream.modern))',
     '        kind != OS64_DOM_SCRIPT_CLASSIC)', (SCRIPTS,)),
    # 4. libdom's two exports
    ('mime-any', WINDOW_C, '    return javascript_type(value + start, essence - start, false);',
     '    return javascript_type(value + start, essence - start, false) || length != (size_t)-1;',
     (SCRIPTS,)),
    ('connected-skips-modules', WINDOW_C,
     '        if (os64_dom_script_kind(script) == OS64_DOM_SCRIPT_NONE) continue;',
     '        if (os64_dom_script_kind(script) != OS64_DOM_SCRIPT_CLASSIC) continue;', (SCRIPTS,)),
    ('connected-skips-import-maps', WINDOW_C,
     '        if (os64_dom_script_kind(script) == OS64_DOM_SCRIPT_NONE) continue;',
     '        if (os64_dom_script_kind(script) == OS64_DOM_SCRIPT_NONE ||\n'
     '            os64_dom_script_kind(script) == OS64_DOM_SCRIPT_IMPORTMAP) continue;', (SCRIPTS,)),
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
    # 6. import maps (MODERN_WEB.md § Import maps, as built): the map's
    # rules, the parse's JSON and errors, and the host's registration. The import map
    # host needs the WPT checkout: run with WPT_DIR set where it is not
    # .vendor/wpt.
    ('rules-ascending', IMPORTMAP_C, '        int order = os64_strcmp(list->rules[mid].key, key);',
     '        int order = os64_strcmp(key, list->rules[mid].key);', (IMPORTMAP,)),
    ('scopes-ascending', IMPORTMAP_C, '        int order = os64_strcmp(map->scopes[mid].prefix, prefix);',
     '        int order = os64_strcmp(prefix, map->scopes[mid].prefix);', (IMPORTMAP,)),
    ('first-duplicate-kept', IMPORTMAP_C, '    return rule_put(map, list, normalized, valid ? address : NULL, true);',
     '    return rule_put(map, list, normalized, valid ? address : NULL, false);', (IMPORTMAP,)),
    ('key-as-written', IMPORTMAP_C, '    if (!url_like(key, base, normalized, sizeof(normalized))) {',
     '    if (true) {', (IMPORTMAP,)),
    ('slash-key-any-address', IMPORTMAP_C,
     '                 (!ends_with_slash(key) || ends_with_slash(address));', '                 true;', (IMPORTMAP,)),
    ('scope-prefix-as-written', IMPORTMAP_C, '        if (!url_parse(scope, base, prefix, sizeof(prefix)))\n            return true;',
     '        if (os64_strcopy(prefix, sizeof(prefix), scope) >= sizeof(prefix))\n            return true;', (IMPORTMAP,)),
    ('merge-new-wins', IMPORTMAP_C,
     '        if (!rule_put(map, into, incoming->rules[i].key, incoming->rules[i].url, false))',
     '        if (!rule_put(map, into, incoming->rules[i].key, incoming->rules[i].url, true))', (IMPORTMAP,)),
    ('merge-scopes-ignore-records', IMPORTMAP_C, '            if (!scope_applies(scope->prefix, record->base))\n                continue;',
     '            if (true)\n                continue;', (IMPORTMAP,)),
    ('merge-imports-ignore-records', IMPORTMAP_C,
     '            if (starts_with(incoming->imports.rules[i].key, window->records[r].specifier))',
     '            if (false)', (IMPORTMAP,)),
    # The merge's "specifier as a URL is null or special" has no mutant: a
    # prefix rule matches only a bare specifier or a special URL, so one
    # kept over a non-special record can change no resolution.
    ('prefix-on-any-url', IMPORTMAP_C, '            (as_url == NULL || special(as_url))) {',
     '            (as_url == NULL || special(as_url) || true)) {', (IMPORTMAP,)),
    ('backtrack-allowed', IMPORTMAP_C, '            return starts_with(out, rule->url) ? MATCH_FOUND : MATCH_BLOCKED;',
     '            return MATCH_FOUND;', (IMPORTMAP,)),
    ('null-rule-falls-back', IMPORTMAP_C,
     '        if (os64_streq(rule->key, normalized)) {\n            if (rule->url == NULL)\n                return MATCH_BLOCKED;',
     '        if (os64_streq(rule->key, normalized)) {\n            if (rule->url == NULL)\n                return MATCH_NONE;',
     (IMPORTMAP,)),
    ('scope-exact-only', IMPORTMAP_C,
     '    return os64_streq(prefix, base) || (ends_with_slash(prefix) && starts_with(base, prefix));',
     '    return os64_streq(prefix, base);', (IMPORTMAP,)),
    ('resolution-unrecorded', IMPORTMAP_C,
     '        record_add(window, serialized, normalized, !url || special(as_url));', '        (void)record_add;',
     (IMPORTMAP,)),
    ('scopes-unread', IMPORTMAP_C,
     '        for (uint32_t s = 0; match == MATCH_NONE && s < window->nscopes; s++)',
     '        for (uint32_t s = 0; match == MATCH_NONE && s < window->nscopes && false; s++)', (IMPORTMAP,)),
    ('parse-reads-inherited', LIBDOM_MAP_C, '    int found = JS_GetOwnProperty(ctx, &desc, object, atom);',
     '    JSValue inherited = JS_GetProperty(ctx, object, atom);\n'
     '    int found = JS_IsUndefined(inherited) ? 0 : 1;\n'
     '    desc = (JSPropertyDescriptor){0, inherited, JS_UNDEFINED, JS_UNDEFINED};', (DOM,)),
    ('parse-array-is-a-map', LIBDOM_MAP_C, '    return JS_IsObject(value) && JS_IsArray(ctx, value) == 0;',
     '    return JS_IsObject(value) && (ctx != NULL || ctx == NULL);', (DOM,)),
    ('parse-scopes-unchecked', LIBDOM_MAP_C,
     '    if (result == 0 && has_scopes) result = each_entry(ctx, scopes, scope_is_map, NULL);',
     '    if (result == 0 && has_scopes && false) result = each_entry(ctx, scopes, scope_is_map, NULL);', (DOM,)),
    ('parse-integrity-unchecked', LIBDOM_MAP_C,
     '    if (result == 0 && has_integrity && !is_map(ctx, integrity)) {',
     '    if (result == 0 && has_integrity && false) {', (DOM,)),
    ('parse-any-value-a-string', LIBDOM_MAP_C, '    if (JS_IsString(value)) {', '    if (!JS_IsUndefined(value)) {', (DOM,)),
    ('parse-refusal-kept', LIBDOM_MAP_C, '    if (kept) return 0;\n    JS_ThrowRangeError',
     '    if (kept || !kept) return 0;\n    JS_ThrowRangeError', (DOM,)),
    ('supports-no-importmap', LIBDOM_MAP_C,
     '(os64_streq(type, "classic") || os64_streq(type, "module") || os64_streq(type, "importmap"));',
     '(os64_streq(type, "classic") || os64_streq(type, "module"));', (DOM,)),
    ('importmap-kind-none', WINDOW_C,
     '        if (type_named(value, length, "importmap")) return OS64_DOM_SCRIPT_IMPORTMAP;\n', '', (SCRIPTS,)),
    ('script-type-trimmed-first', WINDOW_C,
     '        if (type_named(value, length, "module")) return OS64_DOM_SCRIPT_MODULE;\n'
     '        if (type_named(value, length, "importmap")) return OS64_DOM_SCRIPT_IMPORTMAP;\n'
     '        while (length != 0 && ascii_space((unsigned char)*value)) { value++; length--; }\n'
     '        while (length != 0 && ascii_space((unsigned char)value[length - 1])) length--;\n',
     '        while (length != 0 && ascii_space((unsigned char)*value)) { value++; length--; }\n'
     '        while (length != 0 && ascii_space((unsigned char)value[length - 1])) length--;\n'
     '        if (type_named(value, length, "module")) return OS64_DOM_SCRIPT_MODULE;\n'
     '        if (type_named(value, length, "importmap")) return OS64_DOM_SCRIPT_IMPORTMAP;\n', (DOM,)),
    ('parser-map-never-stops', SCRIPTS_C,
     '        s->blocking = (int32_t)(map - s->items);\n        return YONDER_STOP_BLOCK;',
     '        return YONDER_STOP_RESUME;', (SCRIPTS,)),
    ('map-never-merged', SCRIPTS_C, '    if (!yonder_modules_import_map(s->modules, walk.parsed)) {',
     '    yonder_import_map_free(walk.parsed);\n    if (false) {', (SCRIPTS,)),
    ('map-base-the-documents-url', SCRIPTS_C,
     '    else if (!os64_page_base_in(s->doc, s->url, base, sizeof(base)) || (it->base = keep(base)) == NULL)',
     '    else if ((void)base, (it->base = keep(s->url)) == NULL)', (SCRIPTS,)),
    ('connected-after-the-parse', YONDER_C,
     '            if (yonder_scripts_connected_pending(host))\n                return true;', '', (SCRIPTS,)),
    ('resolution-past-the-map', MODULES_C, '    return yonder_import_map_resolve(m->imports, base, specifier, out, cap);',
     '    return yonder_import_map_resolve(NULL, base, specifier, out, cap);', (SCRIPTS,)),
    ('inline-meta-the-documents', MODULES_C, '    compile(m, i, text, length, m->mods[i].base);',
     '    compile(m, i, text, length, m->mods[i].name);', (SCRIPTS,)),
    ('unmapped-says-no-map', MODULES_C, '                      : m->mapped ? "the import', '                      : false ? "the import',
     (SCRIPTS,)),
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
