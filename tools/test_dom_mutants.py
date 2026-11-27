#!/usr/bin/env python3
"""Compile contract mutants against retained fully sanitized host objects.

Each mutant is a copy of one libdom (or libpage) source with one rule broken,
compiled and linked against the DOM host suite's own sanitized objects and
run in the probe its set names; the worktree is never mutated. A mutant
counts only when it compiles and the host assertions or sanitizers fail; a
timeout or a signal alone is not a catch.

Run it through tools/mutants_parallel.py (test_dom_mutants.py:N, and --only
for a review round's few). Each worker's copy builds the objects once, with
SUITE under DOM_HOST_KEEP=1 in its own TMPDIR, and every mutant that worker
runs links against them. A path to objects kept by an earlier run may be
given instead. Without --only, a set flag picks the set; with --only, a name
is looked for in every set.

THE SETS, each with the probe it runs in: the D5 contracts (`base`, the
host's --mutants mode); D6 binding ownership (`reclaim`); D7a events, timers
and asks (`events`); F2 cluster ownership (`realms`); and the loop's
every-turn checks, their lists and the image check's quiet key (`turn`,
against the --canvas probe, test_dom_turn_checks.inc); and the Fullscreen
API's rules (`fullscreen`, against the --fullscreen probe,
test_dom_fullscreen.inc).
"""
import argparse
import os
import pathlib
import shlex
import subprocess
import tempfile
import resource

parser=argparse.ArgumentParser()
parser.add_argument('objects',type=pathlib.Path,nargs='?',help='objects a DOM_HOST_KEEP=1 run kept (default: build them, once per TMPDIR)')
parser.add_argument('--logs',type=pathlib.Path,default=pathlib.Path('/tmp/dom-d5a-mutants'))
parser.add_argument('--reclaim',action='store_true',help='run D6 binding ownership mutants')
parser.add_argument('--events',action='store_true',help='run D7a event, timer and ask mutants')
parser.add_argument('--realms',action='store_true',help='run F2 cluster ownership mutants')
parser.add_argument('--turn',action='store_true',help="run the every-turn checks' list and quiet-key mutants")
parser.add_argument('--fullscreen',action='store_true',help="run the Fullscreen API's mutants")
parser.add_argument('--only',default='',help='run only the mutants whose names hold this text, from every set')
parser.add_argument('--probe',default='',help='select an existing host probe, e.g. --probe=--clone')
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True,exist_ok=True)

# The suite whose objects the mutants link against, and the patience its
# build is given. A probe's run is held to PROBE_SECONDS by `timeout`.
SUITE='bash tools/test_dom_host.sh --core'
SUITE_TIMEOUT=2400
PROBE_SECONDS='45'
PROBES={'base':'--mutants','reclaim':'--reclaim','events':'--events','realms':'--realms','turn':'--canvas','fullscreen':'--fullscreen'}

# Each mutation breaks a separately specified visible rule; it does not supply
# replacement expectations derived from the library's implementation.
mutants = [
    ('clone-state-routing', 'base', 'node', 'os64_page_node_clone(dom->state, node, deep != 0, &status)', 'os64_html_clone(dom->document, node, deep != 0, &status)'),
    ('character-data-state-routing', 'base', 'content', 'os64_page_node_set_text(d_owner(dom, node)->state, (os64_html_node_t *)node, string.data, string.length)', 'os64_html_set_text(d_owner(dom, node)->document, (os64_html_node_t *)node, string.data, string.length)'),
    ('clone-checked-dirty', 'base', 'page-state', 'fresh->on_dirty = old != NULL && old->on_dirty;', 'fresh->on_dirty = false;'),
    ('clone-clean-textarea', 'base', 'page-state', 'fresh->text_clean = textarea && !dirty;', 'fresh->text_clean = false;'),
    ('clone-template-descendants', 'base', 'page-state', 'if (source->template_contents != NULL && target->template_contents != NULL &&', 'if (false && source->template_contents != NULL && target->template_contents != NULL &&'),
    ('comment-parent-notification', 'base', 'page-state', 'tree_plan_textarea(&reserve, node->parent);\n    uint64_t version = os64_html_version(state->doc);', 'tree_plan_textarea(&reserve, node->kind == OS64_HTML_COMMENT ? NULL : node->parent);\n    uint64_t version = os64_html_version(state->doc);'),
    ('character-data-null', 'base', 'content', 'if (JS_IsNull(value) ||', 'if ((JS_IsNull(value) && property != D_DATA) ||'),
    ('generic-element-control-property', 'base', 'node', 'return D_KIND(D_PROTO_INPUT);', 'return D_KIND(D_PROTO_INPUT) | D_KIND(D_PROTO_ELEMENT);'),
    ('text-element-property', 'base', 'node', 'return D_PROTO_CHARACTER_DATA;', 'return D_PROTO_ELEMENT;'),
    ('wrapper-identity', 'base', 'node', 'if (entry != NULL) return JS_DupValue(ctx, entry->value);', 'if (entry != NULL && false) return JS_DupValue(ctx, entry->value);'),
    ('closed-callback', 'base', 'core', 'if (dom == NULL || dom->closed || (dom->cluster && (dom->cluster->poisoned || dom->cluster->drained))) {', 'if (dom == NULL) {'),
    ('binding-quota', 'base', 'core', 'if (total > dom->options.max_bytes - dom->bytes ||', 'if ((false && total > dom->options.max_bytes - dom->bytes) ||'),
    ('html-quota-name', 'base', 'core', 'else if (status <= OS64_HTML_TOO_LARGE && status >= OS64_HTML_WORK_EXHAUSTED)', 'else if (false && status <= OS64_HTML_TOO_LARGE && status >= OS64_HTML_WORK_EXHAUSTED)'),
    ('state-quota-name', 'base', 'core', 'status == -OS64_PAGE_REASON_NO_MEMORY ? "QuotaExceededError" : "TypeError"', 'status == -OS64_PAGE_REASON_NO_MEMORY ? "TypeError" : "TypeError"'),
    ('same-child-collection', 'base', 'collection', 'if (query != NULL) return JS_DupValue(ctx, query->entry->value);', 'if (query != NULL && false) return JS_DupValue(ctx, query->entry->value);'),
    ('live-query-version', 'base', 'collection', 'if (query->version == version) return true;', 'if (query->version != 0) return true;'),
    ('elements-only', 'base', 'collection', 'if (query->elements && node->kind != OS64_HTML_ELEMENT) return false;', 'if (false && query->elements && node->kind != OS64_HTML_ELEMENT) return false;'),
    ('html-query-case', 'base', 'collection', 'node->ns == OS64_HTML_NS_HTML ? query->folded : query->name', 'node->ns == OS64_HTML_NS_HTML ? query->name : query->name'),
    ('numeric-enumeration', 'base', 'collection', 'desc->flags = JS_PROP_CONFIGURABLE | (numeric ? JS_PROP_ENUMERABLE : 0);', 'desc->flags = JS_PROP_CONFIGURABLE;'),
    ('nul-conversion', 'base', 'content', 'if (c == 0) {', 'if (c == 0 && false) {'),
    ('surrogate-conversion', 'base', 'content', '} else if (c == 0xed && i + 2 < length &&', '} else if (c == 0xee && i + 2 < length &&'),
    ('attribute-state-routing', 'base', 'attr', 'os64_page_node_set_attr(d_owner(dom, node)->state, node, name, value, length, remove)', '(remove ? os64_html_remove_attr(d_owner(dom, node)->document, (os64_html_node_t *)node, name) : os64_html_set_attr(d_owner(dom, node)->document, (os64_html_node_t *)node, name, value, length))'),
    ('structural-state-routing', 'base', 'node', 'int64_t status = os64_page_node_insert(dom->state, parent, node, before);', 'int64_t status = os64_html_insert(dom->document, parent, node, before);'),
    ('registry-drain', 'base', 'core', 'dom->retained = 0;', 'dom->retained += 0;'),
    ('fragment-scripting-mode', 'base', 'content', 'string.length, d_scripting(dom, node), &status);', 'string.length, true, &status);'),
    ('numeric-readonly-hook', 'base', 'collection', 'return JS_DefineProperty(ctx, query->dom->index_guard->value, atom,', 'return JS_DefineProperty(ctx, object, atom,'),
    ('undefined-nullable-content', 'base', 'content', '(JS_IsUndefined(value) && (property == D_NODE_VALUE || property == D_TEXT_CONTENT))', '(false && JS_IsUndefined(value) && (property == D_NODE_VALUE || property == D_TEXT_CONTENT))'),
    ('attribute-name-equals', 'base', 'content', " || *at == '='", ''),
    ('error-prototype-setter', 'base', 'core', 'JS_DefinePropertyValueStr(ctx, error, "name", text, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)', 'JS_SetPropertyStr(ctx, error, "name", text)'),
    ('selector-scope', 'base', 'collection', 'garb_selector_matches_in(list, i, element, scope)) return true;', 'garb_selector_matches_in(list, i, element, ((void)scope, NULL))) return true;'),
    ('selector-pseudo-elements', 'base', 'collection', 'if (garb_selector_pseudo(list, i) == GARB_PSEUDO_NONE &&', 'if (true &&'),
    ('static-list-length', 'base', 'collection', 'JS_NewUint32(ctx, found), 0) < 0) {', 'JS_NewUint32(ctx, 0), 0) < 0) {'),
    ('class-query-every-class', 'base', 'collection', 'if (attr == NULL || !d_has_token(attr->value, at, (size_t)(end - at), fold)) return false;', 'if (attr == NULL || ((void)fold, false)) return false;'),
    ('closest-walks-up', 'base', 'collection', 'for (const os64_html_node_t *at = element; at != NULL && at->kind == OS64_HTML_ELEMENT; at = at->parent)', 'for (const os64_html_node_t *at = element; at != NULL && at->kind == OS64_HTML_ELEMENT; at = NULL)'),
    ('before-skips-its-nodes', 'base', 'mixin', 'while (magic == D_BEFORE && previous != NULL && among(previous, args, count)) previous = previous->prev;', '(void)0;'),
    ('replacement-judged-before-it-moves', 'base', 'mixin', 'if (verdict < 0) { d_html_error(ctx, verdict); return -1; }', '(void)verdict;'),
    ('adjacent-text-is-text', 'base', 'mixin', 'os64_html_node_t *node = os64_html_create_text(dom->document, text->data, text->length, &status);\n    if (node == NULL) return d_html_error(ctx, status);', 'os64_html_node_t *node = os64_html_create_text(dom->document, text->data, 0, &status);\n    if (node == NULL) return d_html_error(ctx, status);'),
    ('replace-with-in-place', 'base', 'mixin', 'placed = self->parent == parent ? d_tree_change', 'placed = false ? d_tree_change'),
    ('adjacent-html-born-started', 'base', 'mixin', 'int placed = d_script_mark_tree(dom, fragment) < 0', 'int placed = false'),
    ('contains-walks-up', 'base', 'mixin', 'while (other != NULL && other != self) other = other->parent;', 'if (other != NULL && other != self) other = other->parent;'),
    ('toggle-attribute-force', 'base', 'mixin', '} else if (present && force != 1) {', '} else if (present) {'),
    ('tokens-once', 'base', 'mixin', 'if (!seen) (*out)[(*count)++]', 'if (true) (*out)[(*count)++]'),
    ('tokens-no-empty-attribute', 'base', 'mixin', 'result = changed && !(attr == NULL && n == 0) ?', 'result = changed ?'),
    ('dataset-camel-case', 'base', 'mixin', "out[used++] = (char)(at[1] - ('a' - 'A')); at++;", 'out[used++] = *at;'),
    ('tabindex-focusable', 'base', 'mixin', 'return JS_NewInt32(ctx, focusable(self) ? 0 : -1);', 'return JS_NewInt32(ctx, ((void)focusable, -1));'),
    ('impl-same-object', 'base', 'implementation', 'if (entry->implementation != NULL) return JS_DupValue(ctx, entry->implementation->value);', 'if (false) return JS_DupValue(ctx, entry->implementation->value);'),
    ('impl-title-only-when-given', 'base', 'implementation', 'bool titled = argc > 0 && !JS_IsUndefined(argv[0]);', 'bool titled = argc > 0;'),
    ('impl-doctype-name-rule', 'base', 'implementation', "if (c == '\\0' || c == '>' || c == ' '", "if (c == '\\0' || c == ' '"),
    ('impl-owner-record-read', 'base', 'implementation', 'return os64_html_node_document(node);', 'return node->kind == OS64_HTML_DOCUMENT ? node : window_document(dom);'),
    ('impl-adoption-document', 'base', 'implementation', 'int64_t status = os64_page_node_adopt(dom->state, (os64_html_node_t *)document, (os64_html_node_t *)node);', 'int64_t status = OS64_HTML_OK;'),
    ('impl-made-in-records', 'base', 'implementation', 'if (os64_html_node_document(node) != document)', 'if (false && os64_html_node_document(node) != document)'),
    ('impl-scripting-per-document', 'base', 'implementation', 'return dom->options.scripting && d_window_document(dom, node);', 'return ((void)node, dom->options.scripting);'),
    ('impl-xml-keeps-case', 'base', 'node', 'if (type == D_DOCUMENT_HTML) d_fold(string.data);', 'd_fold(string.data);'),
    ('impl-control-type-default', 'base', 'node', 'return JS_NewString(ctx, keywords == kInput ? "text" : "submit");', 'return JS_NewString(ctx, "submit");'),
    ('impl-reflected-window-only', 'base', 'event', 'return d_window_document(dom, node) ? 1 : -1;', 'return ((void)dom, 1);'),
    ('impl-windowless-handler-compile', 'base', 'event', '!d_window_document(dom, entry->node))\n        return JS_NULL;', 'false)\n        return JS_NULL;'),
    ('impl-windowless-script-marked', 'base', 'window', '} else if (d_connected(script)) {\n            (void)d_script_mark(dom, script);', '} else if (false) {\n            (void)d_script_mark(dom, script);'),
    ('impl-windowless-link', 'base', 'window', 'if (d_window_document(dom, node)) d_navigate(dom, OS64_DOM_NAVIGATE_FOLLOW', 'if (true) d_navigate(dom, OS64_DOM_NAVIGATE_FOLLOW'),
    ('impl-windowless-submit', 'base', 'window', 'if (!d_window_document(dom, form) || !d_connected(form)) return 1;', 'if (!d_connected(form)) return 1;'),
    ('impl-windowless-notify', 'base', 'window', 'if (dom->options.activate != NULL && d_window_document(dom, node))', 'if (dom->options.activate != NULL)'),
    ('impl-windowless-write', 'base', 'window', '        if (windowless(dom, self)) return d_error(ctx, "InvalidStateError", WRITE_UNOPENED);\n        return document_write', '        return document_write'),
    ('impl-windowless-location', 'base', 'window', '    if (windowless(dom, self))\n        return magic & D_SET', '    if (((void)self, false))\n        return magic & D_SET'),
    ('impl-windowless-cookie', 'base', 'classic', 'if (!d_window_document(dom, node)) return set ? JS_UNDEFINED : JS_NewString(ctx, "");', '(void)0;'),
    ('impl-connected-any-document', 'base', 'window', 'return tree_root(node)->kind == OS64_HTML_DOCUMENT;', 'return ((void)node, false);'),
    ('impl-form-lookup-own-tree', 'base', 'collection', 'if (root->kind != OS64_HTML_DOCUMENT) root = query->owner;', 'root = query->owner;'),
    ('impl-checked-every-input', 'base', 'page-state', 'if (on == NULL || !node_kind(state, node, &element, &input) || element != OS64_PAGE_EL_INPUT)', 'if (on == NULL || !node_kind(state, node, &element, &input) || element != OS64_PAGE_EL_INPUT ||\n        (input != OS64_PAGE_INPUT_CHECKBOX && input != OS64_PAGE_INPUT_RADIO))'),
    ('attr-same-object', 'base', 'attr', 'DAttr *rec = record_of(dom, element, a->name);\n    if (rec == NULL) {', 'DAttr *rec = NULL;\n    if (rec == NULL) {'),
    ('attr-value-live', 'base', 'attr', 'if (element != NULL) return JS_NewString(ctx, os64_html_attr(element, rec->name)->value);', 'if (element != NULL) return JS_NewString(ctx, "");'),
    ('attr-written-through', 'base', 'attr', '    if (element != NULL) {\n        int64_t status = d_set_attr(', '    if (false) {\n        int64_t status = d_set_attr('),
    ('attr-detached-own-value', 'base', 'attr', '    rec->value = text->data;\n    rec->length = text->length;\n    text->data = NULL;', '    (void)0;'),
    ('attr-detached-at-door', 'base', 'attr', 'if (rec != NULL) detach(dom, rec, kept, kept_length);', 'd_free(dom, kept);'),
    ('attr-last-value-kept', 'base', 'attr', 'if (rec != NULL) detach(dom, rec, kept, kept_length);', 'if (rec != NULL) { d_free(dom, kept); detach(dom, rec, NULL, 0); }'),
    ('attr-removal-refused-whole', 'base', 'attr', 'if (a != NULL && kept == NULL) return OS64_HTML_NO_MEMORY;', '(void)0;'),
    ('attr-detached-document', 'base', 'attr', 'rec->document = document;\n    rec->entry->target = NULL;', 'rec->document = ((void)document, NULL);\n    rec->entry->target = NULL;'),
    ('attr-in-use', 'base', 'attr', 'if (at != NULL && at != element)\n        return d_error(ctx, "InUseAttributeError"', 'if (false)\n        return d_error(ctx, "InUseAttributeError"'),
    ('attr-own-element-answers-itself', 'base', 'attr', 'if (at == element) return JS_DupValue(ctx, value);', 'if (at == element) return JS_NULL;'),
    ('attr-replaced-detached', 'base', 'attr', 'if (gone != NULL) detach(dom, gone, kept, kept_length);', 'd_free(dom, kept);'),
    ('attr-namespace-kept-or-refused', 'base', 'attr', 'if (old != os64_html_attr(element, rec->name) || (old == NULL && rec->ns != NULL))', 'if (false)'),
    ('attr-handler-slot', 'base', 'attr', 'if (d_handler_attribute_set(dom, ctx, element, rec->name) < 0) {', 'if (false) {'),
    ('attr-remove-node-own-only', 'base', 'attr', 'if (rec->entry->target != element) return d_error(ctx, "NotFoundError"', 'if (rec->entry->target == NULL) return d_error(ctx, "NotFoundError"'),
    ('attr-create-folds', 'base', 'attr', 'else if (d_document_type(dom, document) == D_DOCUMENT_HTML) d_fold(name.data);', 'else (void)0;'),
    ('attr-prefix-only-in-namespace', 'base', 'attr', "const char *colon = ns != NULL ? os64_strchr(name, ':') : NULL;", "const char *colon = ((void)ns, os64_strchr(name, ':'));"),
    ('attr-names-unenumerable', 'base', 'attr', '.flags = JS_PROP_CONFIGURABLE | (numeric ? JS_PROP_ENUMERABLE : 0),', '.flags = JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE,'),
    ('attr-uppercase-unlisted', 'base', 'attr', 'for (size_t i = 0; folds(element) && i < length; i++)', 'for (size_t i = 0; false && i < length; i++)'),
    ('attr-cursor-checked', 'base', 'attr', '    return c->element == element && c->head == element->attrs &&', '    return c->element == element; return c->head == element->attrs &&'),
    ('attr-names-behind-prototype', 'base', 'attr', '    int shown = visible(dom, ctx, object, atom);\n    *found =', '    int shown = 1;\n    *found ='),
    ('attr-looked-up-again', 'base', 'attr', '    int shown = visible(dom, ctx, object, atom);\n    *found = shown > 0 ? listed(dom, element, text, length) : NULL;', '    *found = listed(dom, element, text, length);\n    int shown = visible(dom, ctx, object, atom);\n    if (shown <= 0) *found = NULL;'),
    ('attr-own-property-hides-name', 'base', 'attr', '    if (own != 0) return own < 0 ? -1 : 0;', '    (void)own;'),
    ('attr-index-read-only', 'base', 'attr', 'return JS_DefineProperty(ctx, refused ? entry->dom->index_guard->value : object, atom,', 'return JS_DefineProperty(ctx, object, atom,'),
    ('attr-shadow-whole', 'base', 'attr', '        {"parentNode", A_NO_NODE, false}, {"parentElement", A_NO_NODE, false},', '        {"parentElement", A_NO_NODE, false},'),
    ('attr-an-event-target', 'base', 'attr', 'if (d_event_target(dom, ctx, attr, dom->attr_class, NULL) < 0) return -1;', '(void)0;'),
    ('attr-backstop-told', 'base', 'attr', 'if (dom->options.broken != NULL) dom->options.broken(dom->options.host_opaque, kBroken);', '(void)kBroken;'),
    ('attr-node-names-attr', 'base', 'node', '    if (node == NULL && d_is_attr(dom, value)) {', '    if (false) {'),
    ('attr-tree-arguments', 'base', 'node', 'if (d_is_attr(dom, argv[0]) || (needed == 2 && d_is_attr(dom, argv[1]))) {', 'if (false) {'),
    ('attr-door-remove-attribute', 'base', 'node', 'int64_t status = d_set_attr(dom, node, name.data, value.data, value.length, magic == D_REMOVE_ATTR);', 'int64_t status = os64_page_node_set_attr(dom->state, node, name.data, value.data, value.length, magic == D_REMOVE_ATTR);'),
    ('attr-convert-refused', 'base', 'mixin', '        if (args[i].attr) {\n            d_error(ctx, "HierarchyRequestError"', '        if (false) {\n            d_error(ctx, "HierarchyRequestError"'),
    ('attr-contained-by-none', 'base', 'mixin', 'if (argc < 1 || JS_IsNull(argv[0]) || d_is_attr(dom, argv[0])) return JS_FALSE;', 'if (argc < 1 || JS_IsNull(argv[0])) return JS_FALSE;'),
    ('wrapper-hold', 'reclaim', 'node', 'os64_html_hold(dom->document, node);', '(void)node;'),
    ('wrapper-release', 'reclaim', 'core', 'os64_html_release(dom->document, entry->node);', '(void)entry->node;'),
    ('query-root-hold', 'reclaim', 'collection', 'os64_html_hold(dom->document, root);', '(void)root;'),
    ('query-root-release', 'reclaim', 'collection', 'os64_html_release(dom->document, query->root);', '(void)query->root;'),
    ('query-answer-hold', 'reclaim', 'collection', 'os64_html_hold(query->dom->document, at);', '(void)at;'),
    ('query-answer-release', 'reclaim', 'collection', 'os64_html_release(query->dom->document, old[i]);', '(void)old[i];'),
    ('empty-content-consumption', 'reclaim', 'page-state', 'return node != NULL ? os64_html_insert(state->doc, (os64_html_node_t *)parent,\n            (os64_html_node_t *)node, NULL) : OS64_HTML_OK;', 'return OS64_HTML_OK;'),
    ('capture-top-down', 'events', 'event', 'for (size_t i = count; result == 0 && i > 1; i--)\n        result = invoke_target(d, path[i - 1], PHASE_CAPTURING, true);', 'for (size_t i = 2; result == 0 && i <= count; i++)\n        result = invoke_target(d, path[i - 1], PHASE_CAPTURING, true);'),
    ('target-capture-first', 'events', 'event', 'if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, true);\n    if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, false);', 'if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, false);\n    if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, true);'),
    ('bubbles-flag', 'events', 'event', 'result == 0 && event->bubbles && i < count', 'result == 0 && i < count'),
    ('window-in-path', 'events', 'event', 'bool to_window = node == NULL || (top == document_node(dom) && kind != D_EVENT_LOAD);', 'bool to_window = node == NULL || (top == NULL && kind != D_EVENT_LOAD);'),
    ('once-removed', 'events', 'event', 'if (listener->once) remove_listener(dom, entry, listener);', '(void)0;'),
    ('list-cut-at-reach', 'events', 'event', 'bool final = listener == last;', 'bool final = false;'),
    ('stop-immediate', 'events', 'event', 'if (event->stop_now) break;', '(void)0;'),
    ('stop-propagation', 'events', 'event', 'if (entry == NULL || event->stop) return 0;', 'if (entry == NULL) return 0;'),
    ('handler-false-cancels', 'events', 'event', 'if (state != NULL) cancel(state);', '(void)state;'),
    ('listener-false-ignored', 'events', 'event', 'if (listener->handler != D_LISTENER && JS_VALUE_GET_TAG(result) == JS_TAG_BOOL &&', 'if (JS_VALUE_GET_TAG(result) == JS_TAG_BOOL &&'),
    ('uncancelable', 'events', 'event', '    if (event->cancelable) event->canceled = true;', '    event->canceled = true;'),
    ('return-value-false', 'events', 'event', 'if (magic == E_RETURN_VALUE && !flag) cancel(event);', 'if (magic == E_RETURN_VALUE && flag) cancel(event);'),
    ('checkpoint-per-listener', 'events', 'event', 'os64_js_checkpoint(dom->runtime, OS64_JS_ABI_ID, &point);', 'os64_memset(&point, 0, sizeof(point));'),
    ('throwing-listener-next', 'events', 'event', 'if (!sticky(call.status)) {', 'if (call.status == OS64_JS_OK) {'),
    ('nested-error-reported', 'events', 'event', 'if (!d_exception_catchable(d->ctx)) {\n                        result = -1;', 'if (true) {\n                        result = -1;'),
    ('listener-dedupe', 'events', 'event', 'os64_strcmp(listener->type, type.data) == 0) found = listener;', 'os64_strcmp(listener->type, type.data) == 0 && magic == M_REMOVE) found = listener;'),
    ('document-scope', 'events', 'event', 'd_wrap(dom, ctx, document_node(dom)),\n        form != NULL', 'JS_NewObject(ctx),\n        form != NULL'),
    ('form-scope', 'events', 'event', 'form != NULL ? d_wrap(dom, ctx, form) : JS_NewObject(ctx),', 'form != NULL && false ? d_wrap(dom, ctx, form) : JS_NewObject(ctx),'),
    ('attribute-change-seen', 'events', 'event', '    if (text == NULL ? slot->seen == NULL : slot->seen != NULL && os64_strcmp(text, slot->seen) == 0)\n        return 0;', '    if (slot->handler != D_SLOT_EMPTY || text == NULL) return 0;'),
    ('failed-handler-absent', 'events', 'event', 'if (slot->handler == D_SLOT_UNCOMPILED && slot_compile(dom, ctx, entry, slot) < 0)', 'if ((slot->handler == D_SLOT_UNCOMPILED || slot->handler == D_SLOT_ERROR) && slot_compile(dom, ctx, entry, slot) < 0)'),
    ('body-reflects-window', 'events', 'event', 'return kind == D_EVENT_LOAD || kind == D_EVENT_FOCUS || kind == D_EVENT_BLUR;', 'return kind < 0;'),
    ('load-reads-document', 'events', 'event', 'node == NULL && d_event_kind(ev->type) == D_EVENT_LOAD};', 'false};'),
    ('parser-attribute-first', 'events', 'event', 'return slot_for(dom, ctx, entry, kind, entry != dom->window) != NULL ? 0 : -1;', 'return slot_for(dom, ctx, entry, kind, false) != NULL ? 0 : -1;'),
    ('window-attribute-before-a-listener', 'events', 'event', '        for (int kind = 0; entry == dom->window && kind < D_EVENT_COUNT; kind++)', '        for (int kind = 0; entry == dom->window && kind < 0; kind++)'),
    ('window-late-attribute-last', 'events', 'event', 'return slot_for(dom, ctx, entry, kind, entry != dom->window) != NULL ? 0 : -1;', 'return slot_for(dom, ctx, entry, kind, true) != NULL ? 0 : -1;'),
    ('ancestor-attribute-wrapped', 'events', 'event', 'if (entry == NULL && element_has_handler(at, kind)) {', 'if (entry == NULL && element_has_handler(at, kind) && false) {'),
    ('silent-without-listeners', 'events', 'event', '    if (!os64_dom_listens(dom, ev->type)) return OS64_JS_OK;\n', '\n'),
    ('attribute-count-version', 'events', 'event', 'if (dom->attribute_scanned && dom->attribute_version == version) return dom->attribute_mask;', 'if (dom->attribute_scanned) return dom->attribute_mask;'),
    ('report-folded', 'events', 'event', '    if (outcome->status == OS64_JS_OK && dom->report.status != OS64_JS_OK) {\n        *outcome = dom->report;', '    if (false) {\n        *outcome = dom->report;'),
    ('listener-drain', 'events', 'event', '            set_callback(dom, listener, JS_UNDEFINED);', '            listener->callback = JS_UNDEFINED;'),
    ('timer-order', 'events', 'timer', '((*link)->due == timer->due && (*link)->id < timer->id)', '((*link)->due == timer->due && (*link)->id > timer->id)'),
    ('timer-negative-zero', 'events', 'timer', '    if (delay < 0) delay = 0;\n', '\n'),
    ('timer-cap', 'events', 'timer', 'if (dom->timer_count >= OS64_DOM_TIMERS_MAX', 'if (dom->timer_count > OS64_DOM_TIMERS_MAX'),
    ('timer-arguments', 'events', 'timer', 'timer->argc = extra;', 'timer->argc = 0;'),
    ('timer-clear', 'events', 'timer', 'if (timer != NULL) timer_free(dom, timer);', 'if (timer != NULL && false) timer_free(dom, timer);'),
    ('timer-drain', 'events', 'timer', '    for (DTimer *timer = dom->timers; timer != NULL; timer = timer->next)\n        release_values(dom, timer);', '    (void)0;'),
    ('navigation-last-wins', 'events', 'window', '    /* The last ask of a task wins, as the last location= does in a browser. */', '    if (dom->navigation.kind != OS64_DOM_NAVIGATE_NONE && kind != OS64_DOM_NAVIGATE_NONE) return;'),
    ('navigation-resolved', 'events', 'window', '    return os64_url_absolute(&base, reference, out, cap);', '    return os64_strcopy(out, cap, reference) < cap;'),
    ('hash-fragment', 'events', 'window', 'return set ? ask_url(dom, ctx, OS64_DOM_NAVIGATE_URL, value, true) : location_part(dom, ctx, magic);', 'return set ? ask_url(dom, ctx, OS64_DOM_NAVIGATE_URL, value, false) : location_part(dom, ctx, magic);'),
    ('checkbox-put-back', 'events', 'window', '        if (result == 0) {\n            int64_t status = os64_page_node_set_checked(dom->state, node, was);', '        if (false) {\n            int64_t status = os64_page_node_set_checked(dom->state, node, was);'),
    ('submit-event', 'events', 'window', 'return submit(dom, ctx, form, node, true);', 'return submit(dom, ctx, form, node, false);'),
    ('write-no-insertion-point-throws', 'events', 'window', '    if (status == OS64_HTML_BAD_ARGUMENT) return d_error(ctx, "InvalidStateError", WRITE_ENDED);\n    return JS_UNDEFINED;', '    if (false && status == OS64_HTML_BAD_ARGUMENT) return d_error(ctx, "InvalidStateError", WRITE_ENDED);\n    return JS_UNDEFINED;'),
    ('write-refusal-is-the-pages', 'events', 'window', '    if (status == OS64_HTML_BAD_ARGUMENT) return d_error(ctx, "InvalidStateError", WRITE_ENDED);\n    return JS_UNDEFINED;', '    if (status < 0) return d_error(ctx, "InvalidStateError", WRITE_ENDED);\n    return JS_UNDEFINED;'),
    ('write-joins-every-argument', 'events', 'window', '    for (; done < argc && ok; done++) {', '    for (; done < argc && done < 1 && ok; done++) {'),
    ('writeln-newline', 'events', 'window', "        if (line) text[at++] = '\\n';", '        (void)line;'),
    ('write-budget-before-host', 'events', 'window', '    if (ok && text == NULL) {\n        d_error(ctx, "QuotaExceededError", "document.write quota exceeded");\n        ok = false;\n    }', '    if (ok && text == NULL) return JS_UNDEFINED;'),
    ('open-asks-the-host', 'events', 'window', '        if (host_write(dom, "", 0) == OS64_HTML_BAD_ARGUMENT)\n            return d_error(ctx, "InvalidStateError", WRITE_ENDED);', '        (void)host_write;'),
    ('no-host-no-insertion-point', 'events', 'window', ': OS64_HTML_BAD_ARGUMENT;\n}', ': OS64_HTML_OK;\n}'),
    ('module-not-classic', 'events', 'window', 'if (i == 6) return OS64_DOM_SCRIPT_MODULE;', 'if (i == 6) return OS64_DOM_SCRIPT_CLASSIC;'),
    ('connected-once', 'events', 'window', 'if (d_script_mark(dom, script) == 1) dom->options.script_connected', 'if (d_script_mark(dom, script) >= 0) dom->options.script_connected'),
    ('innerhtml-born-started', 'events', 'content', 'if (fragment != NULL && d_script_mark_tree(dom, fragment) < 0) status = OS64_HTML_NO_MEMORY;\n        else if', 'if (false) status = OS64_HTML_NO_MEMORY;\n        else if'),
    ('clone-carries-started', 'events', 'node', 'if (copy != NULL && d_script_copy_marks(dom, node, copy) >= 0 &&', 'if (copy != NULL &&'),
    ('script-attribute-slot', 'events', 'node', '} else if (d_handler_attribute_set(dom, ctx, node, name.data) < 0) {', '} else if (false) {'),
    ('script-mixins-connect', 'events', 'node', '    if (status >= 0) d_script_connected(dom, scripts, found);', '    (void)0;'),
    ('round9-intersection-root-owner', 'realms', 'intersection', '    dom = d_owner(dom, o->root);\n    if (!d_window_document(dom, o->root)) return NULL;', '    if (!d_window_document(dom, o->root)) return NULL;'),
    ('round9-intersection-provider', 'realms', 'intersection', 'owner->intersections_provider != NULL &&\n            owner->intersections_provider(owner->intersections_opaque, s->batch + groups[i].at, count, &cost)', 'dom->intersections_provider != NULL &&\n            dom->intersections_provider(dom->intersections_opaque, s->batch + groups[i].at, count, &cost)'),
    ('round9-intersection-viewport', 'realms', 'intersection', 'if (owner->view.read != NULL) owner->view.read(owner->view.opaque, &view);', 'if (dom->view.read != NULL) dom->view.read(dom->view.opaque, &view);'),
    ('round9-intersection-target-document', 'realms', 'intersection', 'if (d_window_document(owner, t->node))\n                    s->batch[n++]', 'if (true)\n                    s->batch[n++]'),
    ('round9-intersection-source-provider', 'realms', 'intersection', 'if (s->observed == 0) return false;\n    s->undone = true;', 'if (s->observed == 0 || dom->intersections_provider == NULL) return false;\n    s->undone = true;'),
    ('round9-intersection-retry', 'realms', 'intersection', '(s->observed != 0 && s->undone) || s->owed != 0', 's->owed != 0'),
    ('round8-view-owner', 'realms', 'mixin', '    dom = owner->dom;\n    os64_js_outcome_t access;', '    os64_js_outcome_t access;'),
    ('round8-view-context', 'realms', 'mixin', '    os64_js_outcome_t access;\n    ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);\n    if (ctx == NULL) return JS_EXCEPTION;', '    (void)0;'),
    ('round8-sheet-document', 'realms', 'adopted', 'd_node_document(dom, root) != sheet->dom->document->document', 'd_node_document(dom, root) != dom->document->document'),
    ('round8-sheet-array-owner', 'realms', 'adopted', '    dom = owner->dom;\n    os64_js_outcome_t access;', '    os64_js_outcome_t access;'),
    ('round8-sheet-copy-owner', 'realms', 'adopted', '    dom = entry->dom;\n    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;', '    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;'),
    ('round8-sheet-adoption-scan', 'realms', 'adopted', 'size_t members = dom->cluster != NULL ? dom->cluster->count : 1;', 'size_t members = 1;'),
    ('round8-sheet-array-touch', 'realms', 'adopted', 'os64_html_touch(d_owner(dom, root)->document, (os64_html_node_t *)root)', 'os64_html_touch(dom->document, (os64_html_node_t *)root)'),
    ('round8-resize-partition', 'realms', 'resize', 'os64_dom_t *owner = dom->cluster != NULL ? dom->cluster->members[i] : dom;', 'os64_dom_t *owner = dom;'),
    ('round8-resize-source-provider', 'realms', 'resize', 'if (out == NULL || s == NULL) return false;', 'if (out == NULL || s == NULL || dom->resizes_provider == NULL) return false;'),
    ('round7-report-owner', 'realms', 'validity', 'static int report(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)\n{\n    dom = d_owner(dom, node);', 'static int report(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)\n{'),
    ('round7-selection-owner', 'realms', 'selection', '    dom = d_owner(dom, node);\n    uint32_t now_start, now_end;', '    uint32_t now_start, now_end;'),
    ('round7-resize-owner', 'realms', 'resize', '    dom = o->dom;\n    struct DResizes *s = dom->resizes;', '    struct DResizes *s = dom->resizes;'),
    ('round7-intersection-owner', 'realms', 'intersection', '    dom = o->dom;\n    switch (magic) {', '    switch (magic) {'),
    ('round7-performance-owner', 'realms', 'performance', '    dom = d_callback(ctx, &o->anchor, OS64_JS_ABI_ID);\n    if (dom == NULL) return JS_EXCEPTION;', '    (void)0;'),
    ('round6-pointer-owner', 'realms', 'pointer', '    dom = d_owner(dom, node);\n    DPointer *p = &dom->pointer;', '    DPointer *p = &dom->pointer;'),
    ('round6-attr-clone-owner', 'realms', 'attr', 'static JSValue attr_clone(os64_dom_t *dom, JSContext *ctx, DAttr *rec, const os64_html_node_t *document)\n{\n    dom = d_owner(dom, document);', 'static JSValue attr_clone(os64_dom_t *dom, JSContext *ctx, DAttr *rec, const os64_html_node_t *document)\n{'),
    ('round6-attr-context', 'realms', 'attr', 'static DAttr *attr_make(os64_dom_t *dom, JSContext *ctx, const char *name, const char *ns)\n{\n    os64_js_outcome_t access;\n    ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);\n    if (ctx == NULL) return NULL;', 'static DAttr *attr_make(os64_dom_t *dom, JSContext *ctx, const char *name, const char *ns)\n{'),
    ('round6-attr-factory-owner', 'realms', 'attr', '    dom = d_owner(dom, document);\n    if (argc < (magic == M_CREATE ? 1 : 2))', '    if (argc < (magic == M_CREATE ? 1 : 2))'),
    ('round6-attr-view-owner', 'realms', 'attr', '    dom = d_creation_owner(dom, element);\n    DAttr *rec = record_for(dom, element, a);', '    DAttr *rec = record_for(dom, element, a);'),
    ('round6-map-owner', 'realms', 'attr', '    dom = owner->dom;\n    os64_js_outcome_t access;', '    os64_js_outcome_t access;'),
    ('round6-attr-child-list', 'realms', 'attr', 'static JSValue empty_child_nodes(os64_dom_t *dom, JSContext *ctx, DAttr *rec)\n{\n    dom = rec->entry->dom;', 'static JSValue empty_child_nodes(os64_dom_t *dom, JSContext *ctx, DAttr *rec)\n{'),
    ('round5-scroll-owner', 'realms', 'view', '    dom = d_owner(dom, element);\n    int owner = scroll_owner(dom, ctx, element);', '    int owner = scroll_owner(dom, ctx, element);'),
    ('round5-activation-after-owner', 'realms', 'window', '                       const DActivation *saved)\n{\n    dom = d_owner(dom, node);', '                       const DActivation *saved)\n{'),
    ('round5-submit-after-owner', 'realms', 'window', '    dom = d_owner(dom, form);\n    if (valid > 0 && result > 0', '    if (valid > 0 && result > 0'),
    ('round5-submit-after-connected', 'realms', 'window', 'valid > 0 && result > 0 && d_window_document(dom, form) && d_connected(form)', 'valid > 0 && result > 0 && d_window_document(dom, form)'),
    ('round5-notify-owner', 'realms', 'window', 'static void notify(os64_dom_t *dom, const os64_html_node_t *node, os64_dom_activation_t what)\n{\n    dom = d_owner(dom, node);', 'static void notify(os64_dom_t *dom, const os64_html_node_t *node, os64_dom_activation_t what)\n{'),
    ('round5-replacement-current-target', 'realms', 'webanimation', 'd_in_window_tree(d_owner(a->dom, wa_target(a)), wa_target(a)) && wa_data(a) != NULL &&', 'd_in_window_tree(a->dom, wa_target(a)) && wa_data(a) != NULL &&'),
    ('round5-replacement-order', 'realms', 'webanimation', 'heads[i]->serial < heads[first]->serial', 'heads[i]->serial > heads[first]->serial'),
    ('round5-replacement-cluster', 'realms', 'webanimation', 'size_t n = 0, members = animation_heads(dom, heads);\n    for (DWaAnimation *a; (a = animation_next(heads, members)) != NULL;) n += wa_replaceable(a);', 'size_t n = 0, members = 1; heads[0] = dom->wa_first;\n    for (DWaAnimation *a; (a = animation_next(heads, members)) != NULL;) n += wa_replaceable(a);'),
    ('round4-gradient-registry', 'realms', 'canvas', 'os64_dom_t *owner = dom->cluster ? dom->cluster->members[i] : dom;', 'os64_dom_t *owner = dom;'),
    ('round4-click-guard', 'realms', 'window', 'os64_dom_t *guard = d_creation_owner(dom, node);', 'os64_dom_t *guard = dom;'),
    ('round4-submit-guard', 'realms', 'window', 'os64_dom_t *guard = d_creation_owner(dom, form);', 'os64_dom_t *guard = dom;'),
    ('round4-host-submit-guard', 'realms', 'event', 'os64_dom_t *guard = submitting ? d_creation_owner(dom, node) : dom;', 'os64_dom_t *guard = dom;'),
    ('round4-host-validation-guard', 'realms', 'validity', 'os64_dom_t *guard = d_creation_owner(dom, form);', 'os64_dom_t *guard = dom;'),
    ('round4-event-receiver', 'realms', 'event', '    if (entry != NULL) dom = entry->node ? d_owner(dom, entry->node) : entry->dom;\n    if (magic == M_DISPATCH) {', '    if (magic == M_DISPATCH) {'),
    ('round4-window-registration', 'realms', 'event', 'for (int kind = 0; entry == dom->window && kind < D_EVENT_COUNT; kind++)', 'for (int kind = 0; entry == registration->window && kind < D_EVENT_COUNT; kind++)'),
    ('round4-window-compilation', 'realms', 'event', '    ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &outcome);\n    if (!ctx) return JS_EXCEPTION;\n    if (slot_refresh', '    if (entry->node) ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &outcome);\n    if (!ctx) return JS_EXCEPTION;\n    if (slot_refresh'),
    ('round3-base-uri-owner', 'realms', 'mixin', 'JSValue d_base_uri(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *self)\n{\n    dom = d_owner(dom, self);', 'JSValue d_base_uri(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *self)\n{'),
    ('round3-rendered-owner', 'realms', 'webanimation', 'static int target_rendered(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *target)\n{\n    dom = d_owner(dom, target);', 'static int target_rendered(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *target)\n{'),
    ('round3-handler-receiver', 'realms', 'event', '    dom = entry->node != NULL ? d_owner(dom, entry->node) : entry->dom;', '    (void)entry->dom;'),
    ('round3-handler-attribute', 'realms', 'event', 'int d_handler_attribute_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,\n                            const char *name)\n{\n    dom = d_owner(dom, node);', 'int d_handler_attribute_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,\n                            const char *name)\n{'),
    ('round2-onerror-target-window', 'realms', 'event', 'entry == entry->dom->window && state != NULL &&', 'entry == dom->window && state != NULL &&'),
    ('round2-animation-local-query', 'realms', 'webanimation', 'size_t members = animation_heads(dom, heads);', 'size_t members = 1; heads[0] = dom->wa_first;'),
    ('round2-animation-document-filter', 'realms', 'webanimation', 'return d_connected(target) && os64_html_node_document(target) == root;', 'return d_connected(target);'),
    ('round2-animation-composite-order', 'realms', 'webanimation', 'heads[i]->serial < heads[first]->serial', 'heads[i]->serial > heads[first]->serial'),
    ('review-options-length-owner', 'realms', 'options', '    dom = d_owner(dom, select);\n    uint32_t count = option_count(select);\n    while (count > wanted)', '    uint32_t count = option_count(select);\n    while (count > wanted)'),
    ('review-options-index-owner', 'realms', 'options', '    dom = d_owner(dom, select);\n    uint32_t count = option_count(select);\n    if (index >= count)', '    uint32_t count = option_count(select);\n    if (index >= count)'),
    ('review-class-query-owner', 'realms', 'collection', 'has_classes(d_owner(query->dom, query->root), node, query->named)', 'has_classes(query->dom, node, query->named)'),
    ('review-started-script-identity', 'realms', 'window', 'static bool script_marked(os64_dom_t *dom, const os64_html_node_t *node)\n{\n    dom = d_creation_owner(dom, node);', 'static bool script_marked(os64_dom_t *dom, const os64_html_node_t *node)\n{'),
    ('review-callback-realm', 'realms', 'event', 'if (!dom->cluster || !JS_IsFunction(ctx, callback)) return dom;', 'if (dom || !JS_IsFunction(ctx, callback)) return dom;'),
    ('realm-from-current-document', 'realms', 'node', 'uint8_t slot = os64_html_node_creation_realm(node);', 'uint8_t slot = os64_html_node_creation_realm(os64_html_node_document(node));'),
    ('receiver-uses-method-owner', 'realms', 'geometry', '    dom = d_owner(dom, element);', '    (void)element;'),
    ('binding-local-wrapper-cache', 'realms', 'node', 'DValue **table = dom->cluster ? dom->cluster->buckets : dom->buckets;\n    for (DValue *entry', 'DValue **table = dom->buckets;\n    for (DValue *entry'),
    ('release-to-caller-ledger', 'realms', 'core', '    dom = block->owner;', '    (void)block->owner;'),
    ('destination-custom-definition', 'realms', 'custom', 'if (recorded || element->ce == OS64_HTML_CE_CUSTOM) return recorded;', 'if (!dom->cluster && (recorded || element->ce == OS64_HTML_CE_CUSTOM)) return recorded;'),
    ('child-timer-primary-task', 'realms', 'timer', 'os64_js_realm_call(dom->realm, OS64_JS_ABI_ID, held,', 'os64_js_call(dom->runtime, OS64_JS_ABI_ID, held,'),
    ('child-report-primary-task', 'realms', 'report', 'os64_js_realm_call(dom->realm, OS64_JS_ABI_ID, dom->report_fire->value,', 'os64_js_call(dom->runtime, OS64_JS_ABI_ID, dom->report_fire->value,'),
    ('aggregate-dom-cap-ignored', 'realms', 'core', '(dom->cluster && total > dom->cluster->limits.dom_bytes - dom->cluster->bytes)', '(false && dom->cluster && total > dom->cluster->limits.dom_bytes - dom->cluster->bytes)'),
    ('turn-image-unlisted', 'turn', 'node', '        dom->images[dom->nimages++] = entry;\n', '        (void)0;\n'),
    ('turn-image-room-unmade', 'turn', 'node', '        !d_turn_list_room(dom, &dom->images, dom->nimages, &dom->images_cap, dom->nimages + 1)) {', '        false) {'),
    ('turn-image-made-unmoved', 'turn', 'node', '        d_images_moved(dom);\n    }\n    return JS_DupValue(ctx, entry->value);', '    }\n    return JS_DupValue(ctx, entry->value);'),
    ('turn-source-change-unmoved', 'turn', 'canvas_image', '    if(!e->image_generation)e->image_reported=false;\n    d_images_moved(dom);', '    if(!e->image_generation)e->image_reported=false;'),
    ('turn-insertion-across-documents-unmoved', 'turn', 'node', '    d_documents_moved(dom, parent, node);\n    DAdopting scope;\n    d_adopting_begin(dom, &scope, node, parent);\n    int64_t status = os64_page_node_insert(', '    DAdopting scope;\n    d_adopting_begin(dom, &scope, node, parent);\n    int64_t status = os64_page_node_insert('),
    ('turn-documents-moved-never', 'turn', 'node', '    if (!node || !parent || os64_html_node_document(node) == os64_html_node_document(parent)) return;', '    if (parent || !parent) return;'),
    ('turn-replace-across-documents-unmoved', 'turn', 'node', '    d_documents_moved(dom, parent, node);\n    DAdopting scope;\n    d_adopting_begin(dom, &scope, node, parent);\n    int64_t status = os64_page_node_replace(', '    DAdopting scope;\n    d_adopting_begin(dom, &scope, node, parent);\n    int64_t status = os64_page_node_replace('),
    ('turn-replace-children-across-documents-unmoved', 'turn', 'content', '    d_documents_moved(dom, parent, replacement);\n', ''),
    ('turn-adoption-unmoved', 'turn', 'implementation', '        d_images_moved(dom);\n        d_custom_adopted(dom, node, from, document);', '        d_custom_adopted(dom, node, from, document);'),
    ('turn-key-ignores-answers', 'turn', 'canvas_image', 'uint64_t key=(answers^(serial*0x9E3779B97F4A7C15ull))*0xBF58476D1CE4E5B9ull;', 'uint64_t key=(1^(serial*0x9E3779B97F4A7C15ull))*0xBF58476D1CE4E5B9ull;'),
    ('turn-key-ignores-serial', 'turn', 'canvas_image', 'uint64_t key=(answers^(serial*0x9E3779B97F4A7C15ull))*0xBF58476D1CE4E5B9ull;', 'uint64_t key=(answers^(0*serial*0x9E3779B97F4A7C15ull))*0xBF58476D1CE4E5B9ull;'),
    ('turn-quiet-after-a-find', 'turn', 'canvas_image', '    if(owed||stop)return owed;', '    if(owed||stop){dom->image_quiet=quiet;return owed;}'),
    ('turn-quiet-unchecked', 'turn', 'canvas_image', '    if(quiet && quiet==dom->image_quiet)return false;', '    (void)0;'),
    ('turn-images-oldest-first', 'turn', 'canvas_image', 'owed=image_owed(dom,storage->images[k-1],node,failed,consume,&stop);', 'owed=image_owed(dom,storage->images[storage->nimages-k],node,failed,consume,&stop);'),
    ('turn-image-visits-uncounted', 'turn', 'canvas_image', '    dom->checks_visited+=visited;\n    if(owed||stop)return owed;', '    if(owed||stop)return owed;'),
    ('turn-canvas-untouched-by-a-method', 'turn', 'canvas', '    JSValue result = body(ctx, dom, e, argc, argv, magic);\n    d_canvas_touch(e);', '    JSValue result = body(ctx, dom, e, argc, argv, magic);'),
    ('turn-canvas-untouched-by-a-new-size', 'turn', 'canvas', '        e->more->canvas_reset_pending = true;\n        d_canvas_touch(e);', '        e->more->canvas_reset_pending = true;'),
    ('turn-canvas-never-leaves', 'turn', 'canvas', '            if (tell) {\n                storage->canvases[kept++] = e;', '            if (true) {\n                storage->canvases[kept++] = e;'),
    ('turn-canvas-away-stays-listed', 'turn', 'canvas', '    if(!d_in_window_tree(dom,e->node)){*tell=false;*away=true;return true;}', '    if(!d_in_window_tree(dom,e->node))return true;'),
    ('turn-canvas-never-back', 'turn', 'canvas', 'for (size_t i = 0; i < c->nadded && n != NULL; i++, n = n->next) canvases_back(dom, n);', 'if (0) canvases_back(dom, n);'),
    ('turn-canvas-back-from-a-detached-tree', 'turn', 'canvas', '*canvases_away(dom) != 0 && d_in_window_tree(dom, c->target)) {', '*canvases_away(dom) != 0) {'),
    ('turn-canvas-room-unmade', 'turn', 'canvas', '    if (supported && !d_turn_list_room(storage, &storage->canvases, storage->ncanvases, &storage->canvases_cap,\n                                       storage->contexts + 1))', '    if (false)'),
    ('turn-canvas-visits-uncounted', 'turn', 'canvas', '    dom->checks_visited+=visited;\n    d_provider_leave(dom);', '    (void)visited;\n    d_provider_leave(dom);'),
    ('turn-sweep-unlisted', 'turn', 'event', '                storage->sweeps[storage->nsweeps++] = entry;', '                (void)0;'),
    ('turn-sweep-flag-kept', 'turn', 'event', '        member->sweeps[i]->sweep_owed = false;', '        (void)0;'),
    ('fs-without-activation', 'fullscreen', 'fullscreen', '    } else if (f->provider.consume_activation == NULL || !f->provider.consume_activation(f->provider.opaque))', '    } else if (false)'),
    ('fs-second-request-while-entering', 'fullscreen', 'fullscreen', '    else if (f->pending != NULL || f->leaving)', '    else if (f->leaving)'),
    ('fs-same-element-asks-again', 'fullscreen', 'fullscreen', '    else if (f->element == node) {\n        queue(', '    else if (false) {\n        queue('),
    ('fs-host-refusal-enters', 'fullscreen', 'fullscreen', '    if (!entered || !may_be_fullscreen(dom, node)) {', '    if (!may_be_fullscreen(dom, node)) {'),
    ('fs-disconnected-allowed', 'fullscreen', 'fullscreen', '    if (node->kind != OS64_HTML_ELEMENT || !d_in_window_tree(dom, node))\n        return false;', '    (void)dom;\n    if (node->kind != OS64_HTML_ELEMENT)\n        return false;'),
    ('fs-another-document-allowed', 'fullscreen', 'fullscreen', '    if (node->kind != OS64_HTML_ELEMENT || !d_in_window_tree(dom, node))\n        return false;', '    (void)dom;\n    if (node->kind != OS64_HTML_ELEMENT || !d_connected(node))\n        return false;'),
    ('fs-moved-counts-as-in', 'fullscreen', 'fullscreen', '!f->leaving && !d_in_window_tree(dom, f->element) &&', '!f->leaving && !d_connected(f->element) &&'),
    ('fs-state-bit-never-set', 'fullscreen', 'fullscreen', '    set_bit(dom, node, true);', '    (void)0;'),
    ('fs-replaced-keeps-old', 'fullscreen', 'fullscreen', '        unfullscreen(dom);      /* replaced', '        (void)0;      /* replaced'),
    ('fs-removal-unseen', 'fullscreen', 'fullscreen', '        f->check = true;', '        (void)0;'),
    ('fs-host-exit-keeps-element', 'fullscreen', 'fullscreen', '    if (f->element != NULL) {\n        unfullscreen(dom);\n    } else', '    if (false) {\n        unfullscreen(dom);\n    } else'),
    # (Not run: exit_fullscreen's `f->element == NULL` alone, since the
    # document test after it refuses a NULL element the same way.)
    ('fs-exit-never-asks-host', 'fullscreen', 'fullscreen', '    f->leave = held;\n    if (f->provider.leave != NULL)\n        f->provider.leave(f->provider.opaque);', '    f->leave = held;'),
    ('fs-refusal-resolves', 'fullscreen', 'fullscreen', 't->refusal != NULL ? t->promise.reject : t->promise.resolve', 't->promise.resolve'),
    ('fs-change-does-not-bubble', 'fullscreen', 'fullscreen', 'd_event_new(dom, ctx, type, true, false, true)', 'd_event_new(dom, ctx, type, false, false, true)'),
    ('fs-no-webkit-twin', 'fullscreen', 'fullscreen', 'if (d_delivery_active(dom) && os64_dom_listens(dom, prefixed))', 'if (false && os64_dom_listens(dom, prefixed))'),
    ('fs-error-named-change', 'fullscreen', 'fullscreen', 'const char *type = t->error ? "fullscreenerror" : "fullscreenchange";', 'const char *type = "fullscreenchange";'),
    ('fs-no-provider-allowed', 'fullscreen', 'fullscreen', '    if (f == NULL || !f->has_provider)\n        refusal', '    if (f == NULL)\n        refusal'),
    ('fs-drain-keeps-the-bit', 'fullscreen', 'fullscreen', '    if (f->element != NULL)\n        set_bit(dom, f->element, false);', '    (void)0;'),
    ('fs-record-never-freed', 'fullscreen', 'fullscreen', '    d_free(dom, dom->fullscreen);\n    dom->fullscreen = NULL;', '    dom->fullscreen = NULL;'),
]

# Runtime output is bounded independently of compiler object sizes. Never read
# a mutant's unbounded log into memory; a timeout/signal alone is not a catch.
def child_limits():
    resource.setrlimit(resource.RLIMIT_FSIZE, (8*1024*1024, 8*1024*1024))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

if args.only:
    mutants=[m for m in mutants if args.only in m[0]]
else:
    chosen='realms' if args.realms else 'reclaim' if args.reclaim else 'events' if args.events else 'turn' if args.turn else 'fullscreen' if args.fullscreen else 'base'
    mutants=[m for m in mutants if m[1]==chosen]

def mode_of(m): return args.probe or PROBES[m[1]]

# The objects: given, or built once in this TMPDIR and found there again.
if args.objects is None:
    tmp=pathlib.Path(os.environ.get('TMPDIR') or tempfile.gettempdir())
    note=tmp/'os64-dom-mutants-objects'
    kept=pathlib.Path(note.read_text().strip()) if note.exists() else None
    if kept is None or not (kept/'sanitized-core-probe').exists():
        with (args.logs/'objects.log').open('w') as log:
            built=subprocess.run(['bash','-c',SUITE],cwd=root,env=dict(os.environ,DOM_HOST_KEEP='1'),
                                 stdout=log,stderr=subprocess.STDOUT,timeout=SUITE_TIMEOUT)
        said=[l for l in (args.logs/'objects.log').read_text(errors='replace').splitlines()
              if l.startswith('DOM host artifacts: ')]
        if built.returncode or not said: raise SystemExit(f'the DOM host suite did not build its objects; see {args.logs}/objects.log')
        kept=pathlib.Path(said[-1].split(': ',1)[1])
        note.write_text(str(kept))
    args.objects=kept

# The probes the chosen mutants run, each run once unmutated first (the sets
# whose probe the host suite's own run does not cover).
for mode in sorted({mode_of(m) for m in mutants if m[1] in ('realms','turn','fullscreen') or args.probe}):
    with (args.logs/f'baseline{mode}.log').open('w') as log:
        result=subprocess.run(['timeout',PROBE_SECONDS,str(args.objects/'sanitized-core-probe'),mode],stdout=log,stderr=log,preexec_fn=child_limits)
    if result.returncode: raise SystemExit(f'host baseline {mode} failed; no mutants run')

flags=['-O2','-g','-std=gnu11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-tree-loop-distribute-patterns','-fno-stack-protector','-DOS64_JS_TARGET','-fsanitize=address,undefined','-fno-sanitize-recover=all']
for directory in ['libway/include','libsvg/include','libcanvas/include','libdraw2d/include','libpng/include','libxml/include','libmath/include','libjs/port','libjs/include','libdom/include','libdom','libgarb/include','libhtml/include','libpage/include','libpage/upstream/ryu','libos64/include']:
    flags+=['-I'+str(root/'userland'/directory)]
flags+=['-I'+str(root/'abi/include'),'-isystem',str(root/'userland/obj/js/upstream')]
flags+=['-Dd_alloc=dom_host_private_alloc','-Dos64_malloc=dom_host_malloc','-Dos64_calloc=dom_host_calloc','-Dos64_realloc=dom_host_realloc','-Dos64_free=dom_host_free','-Dos64_malloc_size=dom_host_malloc_size']
raw=subprocess.check_output(['make','-s','-C',str(root/'userland'),'math-host-flags'],text=True)
maths=[]
for line in raw.splitlines():
    if line.startswith('MATH_OBJS='): maths=shlex.split(line.split('=',1)[1])[0].split()
objects=sorted(args.objects.glob('*.o'))
objects=[p for p in objects if p.name!='userland_libos64_str.c.o']
objects.append(args.objects/'userland_libos64_str.c.o-local')
if not (args.objects/'engine-quickjs.o').exists(): raise SystemExit('fully sanitized baseline objects required')
# Each verdict is said on a line of its own as the runner reads it
# (`name: caught`, `name: MISSED`, `name: did not build`).
caught=compiled=0
with tempfile.TemporaryDirectory(prefix='os64-dom-mutants.') as directory:
    directory=pathlib.Path(directory)
    for name,group,unit,old,new in mutants:
        run_mode=mode_of((name,group))
        page_unit=unit.removeprefix('page-') if unit.startswith('page-') else None
        source=(root/'userland'/('libpage' if page_unit else 'libdom')/f'{page_unit or unit}.c').read_text()
        if source.count(old)!=1:
            print(f'ANCHOR ERROR {name}: {source.count(old)} matches\n{name}: did not build',flush=True);continue
        path=directory/f'{name}.c';path.write_text(source.replace(old,new))
        obj=directory/f'{name}.o';binary=directory/name
        with (args.logs/f'{name}.log').open('w') as log:
            unit_flags=['-I'+str(root/'userland/libpage'),*flags[:-6]] if page_unit else flags
            result=subprocess.run(['cc',*unit_flags,'-c',str(path),'-o',str(obj)],stdout=log,stderr=log)
            if result.returncode: print(f'COMPILE ERROR {name}\n{name}: did not build',flush=True);continue
            replaced=f'userland_libpage_{page_unit}.c.o' if page_unit else f'dom-{unit}.o'
            selected=[p for p in objects if p.name!=replaced]
            result=subprocess.run(['cc','-fsanitize=address,undefined','-pthread','-Wl,-z,noexecstack','-Wl,--wrap=d_location_exotic_install','-Wl,--wrap=d_window_named_prototype',*maths,*map(str,selected),str(obj),'-o',str(binary)],cwd=root,stdout=log,stderr=log)
            if result.returncode: print(f'LINK ERROR {name}\n{name}: did not build',flush=True);continue
            compiled+=1
            result=subprocess.run(['timeout',PROBE_SECONDS,str(binary),run_mode],stdout=log,stderr=log,preexec_fn=child_limits)
            if result.returncode==124:
                print(f'TIMEOUT {name} (not counted)\n{name}: MISSED',flush=True);continue
            log.flush()
            with (args.logs/f'{name}.log').open('rb') as evidence:
                excerpt=evidence.read(128*1024).decode('utf-8', errors='replace')
            recognized=result.returncode and any(mark in excerpt for mark in
                ['FAIL:', 'ERROR: AddressSanitizer', 'runtime error:'])
            if recognized:
                caught+=1;print(f'CAUGHT {name}\n{name}: caught',flush=True)
            else: print(f'NOT CAUGHT {name} (exit {result.returncode})\n{name}: MISSED',flush=True)
print(f'DOM mutants: {caught} caught / {compiled} compiling / {len(mutants)} proposed; logs {args.logs}')
raise SystemExit(0 if caught==compiled==len(mutants) else 1)
