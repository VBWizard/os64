#!/usr/bin/env python3
"""One deliberate break at a time to custom elements (CUSTOM_ELEMENTS.md):
libhtml's listener and what each verb and the parser tell it, the custom
state and the name it is read with, libgarb's `:defined` with the switch on
and off, yonder's census; and libdom's registry, upgrades, construction and
reactions, the reactions a change between tasks leaves owed, yonder's
checkpoint for them, and libjs's answer to whether a turn is open. A mutant is CAUGHT when one
of its harnesses fails, MISSED when they all pass, which means a test is
missing and gets written. A build failure is neither.

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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/custom-elements-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

DOM_C = 'userland/libhtml/dom.c'
TREE_C = 'userland/libhtml/tree.c'
FRAGMENT_C = 'userland/libhtml/fragment.c'
SELECT_C = 'userland/libgarb/select.c'
CASCADE_C = 'userland/libgarb/cascade.c'
COLLECTION_C = 'userland/libdom/collection.c'
YONDER_C = 'userland/apps/yonder/yonder.c'
CUSTOM_C = 'userland/libdom/custom.c'
NODE_C = 'userland/libdom/node.c'
IMPL_C = 'userland/libdom/implementation.c'
TIMER_C = 'userland/libdom/timer.c'
JS_C = 'userland/libjs/runtime/runtime.c'
HTML = 'tools/test_html_dom_host.sh'
GARB = 'tools/test_garb_host.sh'
DOM = 'tools/test_dom_host.sh'
# The DOM host's custom elements cases alone (its --custom mode).
DOMC = 'tools/test_dom_host.sh --custom'
WINDOW = 'tools/test_yonder_scripts_host.sh'
JSRT = 'tools/test_js_runtime_host.sh'

mutants = [
    # ── The listener ────────────────────────────────────────────────────
    ('observe-takes-away', DOM_C, '    d->changed = changed;\n',
     '    d->changed = changed ? changed : d->changed;\n', [HTML]),
    ('record-version', DOM_C, '    c->version = d->version;', '    c->version = 0;', [HTML]),
    ('seen-previous', DOM_C, '    seen->previous = before ? before->prev : parent->last_child;',
     '    seen->previous = before ? before->prev : parent->first_child;', [HTML]),
    ('seen-fragment-count', DOM_C, '            seen->count++;', '            ;', [HTML]),
    ('seen-move-parent', DOM_C, '        seen->from = node->parent;\n', '', [HTML]),
    ('seen-move-siblings', DOM_C, '        seen->from_prev = node->prev;', '        seen->from_prev = NULL;', [HTML]),
    ('drained-record', DOM_C, '            tell(d, &drained);', '            (void)drained;', [HTML]),
    ('replace-record-removed', DOM_C, '.nremoved = replaced != NULL,', '.nremoved = 0,', [HTML]),
    ('placed-next-sibling', DOM_C, '.previous_sibling = seen->previous, .next_sibling = before};',
     '.previous_sibling = seen->previous, .next_sibling = before ? before->next : NULL};', [HTML]),
    ('removed-siblings', DOM_C,
     '    os64_html_change_t c = {.kind = OS64_HTML_CHANGE_CHILDREN, .target = parent, .removed = node,\n'
     '                            .nremoved = 1, .previous_sibling = prev, .next_sibling = next};',
     '    os64_html_change_t c = {.kind = OS64_HTML_CHANGE_CHILDREN, .target = parent, .removed = node,\n'
     '                            .nremoved = 1, .previous_sibling = next, .next_sibling = prev};', [HTML]),
    ('attr-local-name', DOM_C, '            local = c + 1;', '            local = c;', [HTML]),
    ('attr-local-needs-namespace', DOM_C, '    for (const char *c = name; ns && *c; c++)',
     '    for (const char *c = name; *c; c++)', [HTML]),
    # ── What each verb tells, and when ──────────────────────────────────
    ('insert-tells', DOM_C,
     '    d_tell_placed(d, parent, node, before, NULL, &seen);\n    if (node->kind == FRAGMENT)',
     '    if (node->kind == FRAGMENT)', [HTML]),
    ('replace-self-tells', DOM_C,
     '        d_tell_placed(d, parent, node, old->next, NULL, &seen);\n        return OS64_HTML_OK;',
     '        return OS64_HTML_OK;', [HTML]),
    ('replace-self-removes-nothing', DOM_C, '        d_tell_placed(d, parent, node, old->next, NULL, &seen);',
     '        d_tell_placed(d, parent, node, old->next, old, &seen);', [HTML]),
    # The standard adopts the new node (its removal) before it takes `old` out.
    ('replace-seen-before-old-leaves', DOM_C,
     '    HSeen seen;\n    d_seen(d, parent, node, before, &seen);\n    seen.previous = old->prev;\n'
     '    moved(d);\n    bool touched = is_landmark_parent(d, parent);\n    take_out(d, old, NULL);\n',
     '    HNode *previous = old->prev;\n    moved(d);\n    bool touched = is_landmark_parent(d, parent);\n'
     '    take_out(d, old, NULL);\n    HSeen seen;\n    d_seen(d, parent, node, before, &seen);\n'
     '    seen.previous = previous;\n', [HTML]),
    ('replace-previous', DOM_C, '    seen.previous = old->prev;\n    moved(d);', '    moved(d);', [HTML]),
    ('remove-tells-before-reclaim', DOM_C,
     '    d_tell_removed(d, parent, node, prev, next);\n    d_detached(d, node);\n    d_collect(d);',
     '    d_detached(d, node);\n    d_collect(d);\n    d_tell_removed(d, parent, node, prev, next);', [HTML]),
    ('seat-html-tells', DOM_C,
     '        landmarks(d, NULL);\n    d_tell_placed(d, &d->root, &d->html, NULL, NULL, &seen);',
     '        landmarks(d, NULL);\n    (void)seen;', [HTML]),
    ('attr-add-tells', DOM_C, '    tell_attr(d, e, fresh->name, fresh->ns, NULL, 0);\n', '', [HTML]),
    ('set-attr-same-value-tells', DOM_C,
     '        tell_attr(d, e, name, existing->ns, existing->value, h_len(existing->value));\n', '', [HTML]),
    ('set-attr-tells-before-retire', DOM_C,
     '    tell_attr(d, e, name, fresh->ns, old ? old->value : NULL, old ? h_len(old->value) : 0);\n'
     '    d_retire(d, old);',
     '    d_retire(d, old);\n'
     '    tell_attr(d, e, name, fresh->ns, old ? old->value : NULL, old ? h_len(old->value) : 0);', [HTML]),
    ('remove-attr-tells-before-retire', DOM_C,
     '    tell_attr(d, e, name, old->ns, old->value, h_len(old->value));\n    d_retire(d, old);',
     '    d_retire(d, old);\n    tell_attr(d, e, name, old->ns, old->value, h_len(old->value));', [HTML]),
    ('batch-remove-clears', DOM_C, '                ns = value = NULL;', '                ns = NULL;', [HTML]),
    ('batch-set-value', DOM_C, '                value = changes[j].value ? changes[j].value : "";',
     '                value = value ? value : "";', [HTML]),
    ('batch-remove-absent-not-told', DOM_C, '        if (!c->remove || value)', '        if (c)', [HTML]),
    ('batch-unchanged-tells', DOM_C,
     '    if (!effective) {\n        tell_attrs(d, e, e->attrs, changes, count);',
     '    if (!effective) {', [HTML]),
    ('batch-tells-before-retire', DOM_C,
     '    tell_attrs(d, e, old, changes, count);\n    if (private)\n        while (old) {\n'
     '            HAttr *next = old->next;\n            d_retire(d, old);\n            old = next;\n        }',
     '    HAttr *had = old;\n    if (private)\n        while (old) {\n'
     '            HAttr *next = old->next;\n            d_retire(d, old);\n            old = next;\n        }\n'
     '    tell_attrs(d, e, had, changes, count);', [HTML]),
    ('set-text-same-tells', DOM_C, '        d_tell_text(d, n, n->text, n->text_len);\n        return OS64_HTML_OK;',
     '        return OS64_HTML_OK;', [HTML]),
    ('set-text-old', DOM_C, '    d_tell_text(d, n, was, was_len);', '    d_tell_text(d, n, was, was_len ? was_len - 1 : 0);', [HTML]),
    ('clone-copies-state', DOM_C, '    n->ns = from->ns;\n    n->tag = from->tag;\n',
     '    n->ns = from->ns;\n    n->tag = from->tag;\n    n->ce = from->ce;\n', [HTML]),
    # ── The parser ──────────────────────────────────────────────────────
    ('parser-attach-tells', TREE_C,
     '        h_attach(d, parent, before, n);\n        d_tell_placed(d, parent, n, before, NULL, &seen);',
     '        h_attach(d, parent, before, n);', [HTML]),
    ('parser-place-tells', TREE_C,
     '    if (verdict == OS64_HTML_OK)\n        d_tell_placed(d, parent, n, before, NULL, &seen);\n', '', [HTML]),
    ('parser-detach-tells', TREE_C, '    d_tell_removed(d, parent, n, prev, next);\n}',
     '    (void)prev;\n    (void)next;\n}', [HTML]),
    ('parser-text-before-link', TREE_C,
     '        if (h_text_put(p, n, c))\n            attach(p, parent, before, n);',
     '        if (attach(p, parent, before, n))\n            h_text_put(p, n, c);', [HTML]),
    ('parser-append-tells', TREE_C,
     '    if (h_text_put(p, n, c))\n        d_tell_text(p->d, n, n->text, had);',
     '    (void)had;\n    h_text_put(p, n, c);', [HTML]),
    ('parser-append-old-length', TREE_C, '        d_tell_text(p->d, n, n->text, had);',
     '        d_tell_text(p->d, n, n->text, had + 1);', [HTML]),
    ('fragment-parse-listens', FRAGMENT_C, '    d->id = owner->id;\n',
     '    d->id = owner->id;\n    d->changed = owner->changed;\n    d->changed_opaque = owner->changed_opaque;\n',
     [HTML]),
    # ── Custom state and the name ───────────────────────────────────────
    ('set-ce-version', DOM_C, '    if (e->ce != ce) {\n        d->version++;', '    if (e->ce != ce) {', [HTML]),
    ('set-ce-element-only', DOM_C,
     '!ours(d, e) || e->kind != ELEMENT || (unsigned)ce > OS64_HTML_CE_FAILED',
     '!ours(d, e) || (unsigned)ce > OS64_HTML_CE_FAILED', [HTML]),
    ('set-ce-range', DOM_C, '(unsigned)ce > OS64_HTML_CE_FAILED)', '(unsigned)ce > 3)', [HTML]),
    ('name-first-letter', DOM_C, "    if (!name || name[0] < 'a' || name[0] > 'z')", "    if (!name || name[0] < 'A')",
     [HTML]),
    ('name-characters', DOM_C, "*c == '-' ||\n              *c == '.' || *c == '_'))",
     "*c == '-' ||\n              *c == '_'))", [HTML]),
    ('name-reserved', DOM_C, '        if (h_eq(name, kReserved[i]))\n            return false;',
     '        if (h_eq(name, kReserved[i]) && i)\n            return false;', [HTML]),
    ('name-needs-hyphen', DOM_C, '    return hyphen;\n}', '    return true;\n}', [HTML]),
    # ── :defined ────────────────────────────────────────────────────────
    ('defined-switch-off', SELECT_C, '        return sel->modern_off || el->ce == OS64_HTML_CE_CUSTOM ||',
     '        return el->ce == OS64_HTML_CE_CUSTOM ||', [GARB]),
    ('defined-custom', SELECT_C, '        return sel->modern_off || el->ce == OS64_HTML_CE_CUSTOM ||',
     '        return sel->modern_off ||', [GARB]),
    ('defined-html-only', SELECT_C, '               !(html(el) && os64_html_custom_name(el->name));',
     '               !os64_html_custom_name(el->name);', [GARB]),
    ('selectors-keep-switch', SELECT_C, '    s->modern_off = modern_off;', '    s->modern_off = modern_off && !modern_off;',
     [GARB]),
    ('cascade-passes-switch', CASCADE_C, 'c->quirks,\n                                                 c->env.modern_off);',
     'c->quirks,\n                                                 false);', [GARB]),
    ('query-passes-switch', COLLECTION_C, '                               !dom->options.modern) : NULL;',
     '                               false) : NULL;', [DOM]),
    # ── yonder's census ─────────────────────────────────────────────────
    ('census-skips-custom', YONDER_C,
     '            if (n->ns == OS64_HTML_NS_HTML && n->ce != OS64_HTML_CE_CUSTOM &&',
     '            if (n->ns == OS64_HTML_NS_HTML &&', [WINDOW]),
    ('census-name-rule', YONDER_C, '                os64_html_custom_name(n->name)) {',
     "                os64_strchr(n->name, '-') != NULL) {", [WINDOW]),
    ('census-failed-line', YONDER_C, '        if (!custom[c].failed)\n', '        if (!custom[c].failed || c)\n',
     [WINDOW]),
    # ── Cut b1: the registry, upgrades and reactions (libdom custom.c) ──
    ('ce-attribute-custom-only', CUSTOM_C, '    if (element->ce != OS64_HTML_CE_CUSTOM) return;\n    const os64_html_attr_t *now',
     '    const os64_html_attr_t *now', [DOMC]),
    ('ce-observed-only', CUSTOM_C, '    if (kind == CB_ATTRIBUTE && !observes(d, strings[0])) return;\n',
     '    if (kind == CB_ATTRIBUTE && !observes(d, strings[0]) && false) return;\n', [DOMC]),
    ('ce-disconnect-needs-document', CUSTOM_C,
     '        if (d_connected(c->target)) {\n            const os64_html_node_t *n = c->removed;',
     '        if (true) {\n            const os64_html_node_t *n = c->removed;', [DOMC]),
    ('ce-connect-needs-document', CUSTOM_C, '        if (!connected) continue;\n',
     '        if (!connected && false) continue;\n', [DOMC]),
    ('ce-adopt-other-document-only', CUSTOM_C, 'if (from != NULL && from != document) {', 'if (from != NULL) {',
     [DOMC]),
    ('ce-upgrade-window-only', CUSTOM_C,
     '    if (element->ce != OS64_HTML_CE_PLAIN || definition_for(dom, element) == NULL ||\n'
     '        !d_window_document(dom, element))',
     '    if (element->ce != OS64_HTML_CE_PLAIN || definition_for(dom, element) == NULL)', [DOMC]),
    ('ce-define-upgrades', CUSTOM_C,
     '        if (at->kind == OS64_HTML_ELEMENT && at->ns == OS64_HTML_NS_HTML && os64_streq(at->name, d->name))\n'
     '            try_upgrade(dom, at);',
     '        if (at->kind == OS64_HTML_ELEMENT && at->ns == OS64_HTML_NS_HTML && os64_streq(at->name, d->name))\n'
     '            (void)at;', [DOMC]),
    ('ce-upgrade-marks-failed', CUSTOM_C,
     '    os64_html_set_ce(dom->document, (os64_html_node_t *)element, OS64_HTML_CE_FAILED);\n'
     '    for (const os64_html_attr_t *a', '    for (const os64_html_attr_t *a', [DOMC]),
    ('ce-upgrade-marks-custom', CUSTOM_C,
     '        os64_html_set_ce(dom->document, (os64_html_node_t *)element, OS64_HTML_CE_CUSTOM);\n        return 0;',
     '        return 0;', [DOMC]),
    ('ce-construction-stack', CUSTOM_C, '    d->stack[d->depth++] = element;', '    d->stack[d->depth++] = NULL;',
     [DOMC]),
    ('ce-marker', CUSTOM_C, '        if (!JS_IsException(result)) d->stack[d->depth - 1] = NULL;\n', '', [DOMC]),
    ('ce-same-value', CUSTOM_C,
     '    bool same = !JS_IsException(result) && JS_IsObject(result) &&\n'
     '                JS_VALUE_GET_PTR(result) == JS_VALUE_GET_PTR(self);',
     '    bool same = !JS_IsException(result) && JS_IsObject(result);', [DOMC]),
    ('ce-failed-empties-queue', CUSTOM_C,
     '    while (r->first != NULL) {\n        DReaction *next = r->first->next;\n        reaction_free(dom, r->first);\n'
     '        r->first = next;\n    }\n    r->last = NULL;\n    if (!d_exception_catchable(ctx)) return -1;',
     '    if (!d_exception_catchable(ctx)) return -1;', [DOMC]),
    ('ce-attribute-arguments', CUSTOM_C,
     'const char *strings[4] = {c->attr_local, was, now != NULL ? now->value : NULL, c->attr_ns};',
     'const char *strings[4] = {c->attr_local, now != NULL ? now->value : NULL, was, c->attr_ns};', [DOMC]),
    ('ce-pop-rethrows', CUSTOM_C,
     '        if (threw && status == 0) JS_Throw(ctx, pending);\n        else JS_FreeValue(ctx, pending);',
     '        JS_FreeValue(ctx, pending);', [DOMC]),
    ('ce-microtask-in-turn', CUSTOM_C, '    if (os64_js_turn_open(dom->runtime)) {\n        os64_js_outcome_t outcome;',
     '    if (false) {\n        os64_js_outcome_t outcome;', [DOMC]),
    ('ce-owed-cleared', CUSTOM_C, '    s->requested = false;\n    s->owed = false;\n    return status;',
     '    s->requested = false;\n    return status;', [DOMC]),
    ('ce-timer-next-owed', TIMER_C, '    if (os64_dom_reactions_owed(dom)) return 0;\n', '', [DOMC]),
    ('ce-timer-fire-owed', TIMER_C, '    if (os64_dom_reactions_run(dom, outcome)) return true;\n', '', [DOMC]),
    ('ce-when-defined-one-promise', CUSTOM_C,
     '    if (valid && d == NULL && w != NULL) {\n        d_string_free(dom, &name);\n'
     '        return JS_DupValue(ctx, w->promise->value);\n    }\n', '', [DOMC]),
    ('ce-when-defined-resolves', CUSTOM_C,
     '        JSValue resolved = JS_Call(ctx, w->resolve->value, JS_UNDEFINED, 1, &argv[1]);',
     '        JSValue resolved = JS_UNDEFINED;', [DOMC]),
    ('ce-observed-needs-callback', CUSTOM_C,
     '    if (!JS_IsUndefined(r->callbacks[CB_ATTRIBUTE])) {\n        JSValue observed',
     '    if (true) {\n        JSValue observed', [DOMC]),
    ('ce-create-window-only', CUSTOM_C, 'document == window_document(dom) && s->definitions != NULL',
     '(document == window_document(dom) || document != NULL) && s->definitions != NULL', [DOMC]),
    ('ce-created-name', CUSTOM_C, '!d_window_document(dom, node) || !os64_streq(node->name, name))',
     '!d_window_document(dom, node) || (!os64_streq(node->name, name) && false))', [DOMC]),
    ('ce-create-remembers-failure', CUSTOM_C,
     '    remember_failure(dom, ctx, d, error);\n    d_report_as(dom, ctx, error, d->constructor_script);\n'
     '    JS_FreeValue(ctx, error);\n    int64_t status;',
     '    d_report_as(dom, ctx, error, d->constructor_script);\n    JS_FreeValue(ctx, error);\n    int64_t status;',
     [DOMC]),
    ('ce-create-failed-unknown', CUSTOM_C,
     '    os64_html_set_ce(dom->document, element, OS64_HTML_CE_FAILED);\n    *out = d_wrap(dom, ctx, element);',
     '    *out = d_wrap(dom, ctx, element);', [DOMC]),
    ('ce-new-is-custom', CUSTOM_C,
     '            os64_html_set_ce(dom->document, element, OS64_HTML_CE_CUSTOM);\n'
     '            result = d_wrap_constructed(dom, ctx, element, prototype);',
     '            result = d_wrap_constructed(dom, ctx, element, prototype);', [DOMC]),
    ('ce-clone-upgrades', NODE_C, '            if (d_custom_cloned(dom, ctx, copy) < 0) {',
     '            if (false && d_custom_cloned(dom, ctx, copy) < 0) {', [DOMC]),
    ('ce-create-element-constructs', NODE_C,
     '        int constructed = d_custom_create(dom, ctx, node, string.data, &made);',
     '        int constructed = 0;', [DOMC]),
    ('ce-create-element-ns-constructs', IMPL_C,
     '            if (d_custom_create(dom, ctx, document, name.data, &made) != 0) result = made;',
     '            (void)made;', [DOMC]),
    ('ce-node-kind-failed', 'userland/libdom/node.c', ' && node->ce != OS64_HTML_CE_FAILED', '', [DOMC]),
    ('ce-drain-takes-listener', CUSTOM_C, '    if (dom->custom != NULL) os64_html_observe(dom->document, NULL, NULL);',
     '    (void)dom;', [DOMC]),
    ('ce-stream-checkpoint', YONDER_C, '            stream_sheets();\n            stream_reactions();\n            return r;',
     '            stream_sheets();\n            return r;', [WINDOW]),
    ('ce-census-message', YONDER_C,
     '        else if (yonder_scripts_custom_failure(scripts, custom[c].name, why, sizeof(why))) {',
     '        else if (false && yonder_scripts_custom_failure(scripts, custom[c].name, why, sizeof(why))) {', [WINDOW]),
    ('js-turn-open-task', JS_C, '    return runtime != NULL && (runtime->active || runtime->turn);',
     '    return runtime != NULL && runtime->active;', [JSRT]),
    # Codex #407's three: define's upgrades after its promise, a replace's
    # removed node not hiding the adopted one's source, and nothing queued
    # for an upgrade the construction stack cannot afford.
    ('ce-define-promise-first', CUSTOM_C,
     '    const os64_html_node_t *document = window_document(dom);\n'
     '    for (const os64_html_node_t *at = document; at != NULL; at = d_walk_next(document, at)) {\n'
     '        s->visits++;\n'
     '        if (at->kind == OS64_HTML_ELEMENT && at->ns == OS64_HTML_NS_HTML && os64_streq(at->name, d->name))\n'
     '            try_upgrade(dom, at);\n    }\n',
     '    if (d_reactions_push(dom, ctx) < 0) goto done;\n'
     '    const os64_html_node_t *document = window_document(dom);\n'
     '    for (const os64_html_node_t *at = document; at != NULL; at = d_walk_next(document, at)) {\n'
     '        s->visits++;\n'
     '        if (at->kind == OS64_HTML_ELEMENT && at->ns == OS64_HTML_NS_HTML && os64_streq(at->name, d->name))\n'
     '            try_upgrade(dom, at);\n    }\n'
     '    if (d_reactions_pop(dom, ctx) < 0) goto done;\n', [DOMC]),
    ('ce-replace-keeps-source', CUSTOM_C, 'if (dom->documents != 0 && c->nadded == 0) {', 'if (dom->documents != 0) {',
     [DOMC]),
    ('ce-stack-before-queue', CUSTOM_C,
     '        if (stack == NULL) {\n            JS_FreeValue(ctx, self);',
     '        if (stack == NULL) {\n            if (d_connected(element)) enqueue_callback(dom, element, CB_CONNECTED, NULL, NULL);\n'
     '            JS_FreeValue(ctx, self);', [DOMC]),
    ('census-failed-apart', YONDER_C,
     '                while (c < ncustom && !(custom[c].failed == (n->ce == OS64_HTML_CE_FAILED) &&\n'
     '                                        os64_streq(custom[c].name, n->name)))',
     '                while (c < ncustom && !os64_streq(custom[c].name, n->name))', [WINDOW]),
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
                script, *flags = harness.split()
                # tools/mutants_parallel.py gives a worker's whole baseline, every
                # suite named here in turn, this one deadline: the full DOM host
                # among them takes most of it.
                try:
                    result = subprocess.run(['bash', str(root / script), *flags], cwd=root, stdout=out, stderr=out,
                                            timeout=2400)
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

print(f'\ncustom elements mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
