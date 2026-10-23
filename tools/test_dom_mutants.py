#!/usr/bin/env python3
"""Compile contract mutants against retained fully sanitized host objects.

Run test_dom_host.sh with DOM_HOST_KEEP=1 first, then pass its artifact path.
Source files are copied into temporary storage; the worktree is not mutated.
A mutant counts only when it compiles and the host assertions/sanitizers fail.
"""
import argparse
import pathlib
import shlex
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('objects',type=pathlib.Path)
parser.add_argument('--logs',type=pathlib.Path,default=pathlib.Path('/tmp/dom-d5a-mutants'))
parser.add_argument('--reclaim',action='store_true',help='run D6 binding ownership mutants')
parser.add_argument('--events',action='store_true',help='run D7a event, timer and ask mutants')
parser.add_argument('--only',default='',help='run only the mutants whose names hold this text')
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True,exist_ok=True)
# Each mutation breaks a separately specified visible rule; it does not supply
# replacement expectations derived from the library's implementation.
mutants=[
 ('clone-state-routing','node','os64_page_node_clone(dom->state, node, deep != 0, &status)','os64_html_clone(dom->document, node, deep != 0, &status)'),
 ('character-data-state-routing','content','os64_page_node_set_text(dom->state, (os64_html_node_t *)node, string.data, string.length)','os64_html_set_text(dom->document, (os64_html_node_t *)node, string.data, string.length)'),
 ('clone-checked-dirty','page-state','fresh->on_dirty = old != NULL && old->on_dirty;','fresh->on_dirty = false;'),
 ('clone-clean-textarea','page-state','fresh->text_clean = textarea && !dirty;','fresh->text_clean = false;'),
 ('clone-template-descendants','page-state','if (source->template_contents != NULL && target->template_contents != NULL &&','if (false && source->template_contents != NULL && target->template_contents != NULL &&'),
 ('comment-parent-notification','page-state','tree_plan_textarea(&reserve, node->parent);\n    uint64_t version = os64_html_version(state->doc);','tree_plan_textarea(&reserve, node->kind == OS64_HTML_COMMENT ? NULL : node->parent);\n    uint64_t version = os64_html_version(state->doc);'),
 ('character-data-null','content','if (JS_IsNull(value) ||','if ((JS_IsNull(value) && property != D_DATA) ||'),
 ('generic-element-control-property','node','return 1u << D_PROTO_INPUT;','return (1u << D_PROTO_INPUT) | (1u << D_PROTO_ELEMENT);'),
 ('text-element-property','node','return D_PROTO_CHARACTER_DATA;','return D_PROTO_ELEMENT;'),

 ('wrapper-identity','node','if (entry != NULL) return JS_DupValue(ctx, entry->value);','if (entry != NULL && false) return JS_DupValue(ctx, entry->value);'),
 ('closed-callback','core','if (dom == NULL || dom->closed) {','if (dom == NULL) {'),
 ('binding-quota','core','if (total > dom->options.max_bytes - dom->bytes) return NULL;','if (false && total > dom->options.max_bytes - dom->bytes) return NULL;'),
 ('html-quota-name','core','else if (status <= OS64_HTML_TOO_LARGE && status >= OS64_HTML_WORK_EXHAUSTED)','else if (false && status <= OS64_HTML_TOO_LARGE && status >= OS64_HTML_WORK_EXHAUSTED)'),
 ('state-quota-name','core','status == -OS64_PAGE_REASON_NO_MEMORY ? "QuotaExceededError" : "TypeError"','status == -OS64_PAGE_REASON_NO_MEMORY ? "TypeError" : "TypeError"'),
 ('same-child-collection','collection','if (query != NULL) return JS_DupValue(ctx, query->entry->value);','if (query != NULL && false) return JS_DupValue(ctx, query->entry->value);'),
 ('live-query-version','collection','if (query->version == version) return true;','if (query->version != 0) return true;'),
 ('elements-only','collection','if (query->elements && node->kind != OS64_HTML_ELEMENT) return false;','if (false && query->elements && node->kind != OS64_HTML_ELEMENT) return false;'),
 ('html-query-case','collection','node->ns == OS64_HTML_NS_HTML ? query->folded : query->name','node->ns == OS64_HTML_NS_HTML ? query->name : query->name'),
 ('numeric-enumeration','collection','desc->flags = JS_PROP_CONFIGURABLE | (numeric ? JS_PROP_ENUMERABLE : 0);','desc->flags = JS_PROP_CONFIGURABLE;'),
 ('nul-conversion','content','if (c == 0) {','if (c == 0 && false) {'),
 ('surrogate-conversion','content','} else if (c == 0xed && i + 2 < length &&','} else if (c == 0xee && i + 2 < length &&'),
 ('attribute-state-routing','attr','os64_page_node_set_attr(dom->state, node, name, value, length, remove)','(remove ? os64_html_remove_attr(dom->document, (os64_html_node_t *)node, name) : os64_html_set_attr(dom->document, (os64_html_node_t *)node, name, value, length))'),
 ('structural-state-routing','node','return os64_page_node_insert(dom->state, parent, node, before);','return os64_html_insert(dom->document, parent, node, before);'),
 ('registry-drain','core','dom->retained = 0;','dom->retained += 0;'),
 ('fragment-scripting-mode','content','string.length, d_scripting(dom, node), &status);','string.length, true, &status);'),
 ('numeric-readonly-hook','collection','return JS_DefineProperty(ctx, query->dom->index_guard->value, atom,','return JS_DefineProperty(ctx, object, atom,'),
 ('undefined-nullable-content','content','(JS_IsUndefined(value) && (property == D_NODE_VALUE || property == D_TEXT_CONTENT))','(false && JS_IsUndefined(value) && (property == D_NODE_VALUE || property == D_TEXT_CONTENT))'),
 ('attribute-name-equals','content'," || *at == '='",''),
 ('error-prototype-setter','core','JS_DefinePropertyValueStr(ctx, error, "name", text, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)','JS_SetPropertyStr(ctx, error, "name", text)'),
 ('selector-scope','collection','garb_selector_matches_in(list, i, element, scope)) return true;','garb_selector_matches_in(list, i, element, ((void)scope, NULL))) return true;'),
 ('selector-pseudo-elements','collection','if (garb_selector_pseudo(list, i) == GARB_PSEUDO_NONE &&','if (true &&'),
 ('static-list-length','collection','JS_NewUint32(ctx, found), 0) < 0) {','JS_NewUint32(ctx, 0), 0) < 0) {'),
 ('class-query-every-class','collection','if (attr == NULL || !d_has_token(attr->value, at, (size_t)(end - at), fold)) return false;','if (attr == NULL || ((void)fold, false)) return false;'),
 ('closest-walks-up','collection','for (const os64_html_node_t *at = element; at != NULL && at->kind == OS64_HTML_ELEMENT; at = at->parent)','for (const os64_html_node_t *at = element; at != NULL && at->kind == OS64_HTML_ELEMENT; at = NULL)'),
 ('before-skips-its-nodes','mixin','while (magic == D_BEFORE && previous != NULL && among(previous, args, count)) previous = previous->prev;','(void)0;'),
 ('replacement-judged-before-it-moves','mixin','if (verdict < 0) { d_html_error(ctx, verdict); return -1; }','(void)verdict;'),
 ('adjacent-text-is-text','mixin','os64_html_node_t *node = os64_html_create_text(dom->document, text->data, text->length, &status);\n    if (node == NULL) return d_html_error(ctx, status);','os64_html_node_t *node = os64_html_create_text(dom->document, text->data, 0, &status);\n    if (node == NULL) return d_html_error(ctx, status);'),
 ('replace-with-in-place','mixin','placed = self->parent == parent ? d_tree_change','placed = false ? d_tree_change'),
 ('adjacent-html-born-started','mixin','int placed = d_script_mark_tree(dom, fragment) < 0','int placed = false'),
 ('contains-walks-up','mixin','while (other != NULL && other != self) other = other->parent;','if (other != NULL && other != self) other = other->parent;'),
 ('toggle-attribute-force','mixin','} else if (present && force != 1) {','} else if (present) {'),
 ('tokens-once','mixin','if (!seen) (*out)[(*count)++]','if (true) (*out)[(*count)++]'),
 ('tokens-no-empty-attribute','mixin','result = changed && !(attr == NULL && n == 0) ?','result = changed ?'),
 ('dataset-camel-case','mixin',"out[used++] = (char)(at[1] - ('a' - 'A')); at++;","out[used++] = *at;"),
 ('tabindex-focusable','mixin','return JS_NewInt32(ctx, focusable(self) ? 0 : -1);','return JS_NewInt32(ctx, ((void)focusable, -1));'),
 # Slice 0c: documents of their own (LIBDOM.md § Documents of their own).
 ('impl-same-object','implementation','if (entry->implementation != NULL) return JS_DupValue(ctx, entry->implementation->value);','if (false) return JS_DupValue(ctx, entry->implementation->value);'),
 ('impl-title-only-when-given','implementation','bool titled = argc > 0 && !JS_IsUndefined(argv[0]);','bool titled = argc > 0;'),
 ('impl-doctype-name-rule','implementation',"if (c == '\\0' || c == '>' || c == ' '","if (c == '\\0' || c == ' '"),
 ('impl-owner-record-read','implementation','return entry != NULL && entry->document != NULL ? entry->document : window_document(dom);','return ((void)entry, window_document(dom));'),
 ('impl-leaving-records','implementation','    if (entry == NULL) return;\n    const os64_html_node_t *document = d_node_document(dom, node);','    if (entry != NULL || entry == NULL) return;\n    const os64_html_node_t *document = d_node_document(dom, node);'),
 ('impl-made-in-records','implementation','if (entry != NULL) entry->document = document != window_document(dom) ? document : NULL;','(void)entry; (void)document;'),
 ('impl-scripting-per-document','implementation','return dom->options.scripting && d_window_document(dom, node);','return ((void)node, dom->options.scripting);'),
 ('impl-xml-keeps-case','node','if (type == D_DOCUMENT_HTML) d_fold(string.data);','d_fold(string.data);'),
 ('impl-control-type-default','node','return JS_NewString(ctx, keywords == kInput ? "text" : "submit");','return JS_NewString(ctx, "submit");'),
 ('impl-reflected-window-only','event','return d_window_document(dom, node) ? 1 : -1;','return ((void)dom, 1);'),
 ('impl-windowless-handler-compile','event','!d_window_document(dom, entry->node))\n        return JS_NULL;','false)\n        return JS_NULL;'),
 ('impl-windowless-script-marked','window','} else if (d_connected(script)) {\n            (void)d_script_mark(dom, script);','} else if (false) {\n            (void)d_script_mark(dom, script);'),
 ('impl-windowless-link','window','if (d_window_document(dom, node)) d_navigate(dom, OS64_DOM_NAVIGATE_FOLLOW','if (true) d_navigate(dom, OS64_DOM_NAVIGATE_FOLLOW'),
 ('impl-windowless-submit','window','if (!d_window_document(dom, form)) return 1;','(void)0;'),
 ('impl-windowless-notify','window','if (dom->options.activate != NULL && d_window_document(dom, node))','if (dom->options.activate != NULL)'),
 ('impl-windowless-write','window','        if (windowless(dom, self)) return d_error(ctx, "InvalidStateError", WRITE_UNOPENED);\n        return document_write','        return document_write'),
 ('impl-windowless-location','window','    if (windowless(dom, self))\n        return magic & D_SET','    if (((void)self, false))\n        return magic & D_SET'),
 ('impl-windowless-cookie','classic','if (!d_window_document(dom, node)) return set ? JS_UNDEFINED : JS_NewString(ctx, "");','(void)0;'),
 ('impl-connected-any-document','window','return tree_root(node)->kind == OS64_HTML_DOCUMENT;','return ((void)node, false);'),
 ('impl-form-lookup-own-tree','collection','if (root->kind != OS64_HTML_DOCUMENT) root = query->owner;','root = query->owner;'),
 ('impl-checked-every-input','page-state','if (on == NULL || !node_kind(state, node, &element, &input) || element != OS64_PAGE_EL_INPUT)','if (on == NULL || !node_kind(state, node, &element, &input) || element != OS64_PAGE_EL_INPUT ||\n        (input != OS64_PAGE_INPUT_CHECKBOX && input != OS64_PAGE_INPUT_RADIO))'),
 # Slice 0d: Element.attributes, NamedNodeMap and Attr (LIBDOM.md § Attributes as nodes).
 ('attr-same-object','attr','DAttr *rec = record_of(dom, element, a->name);\n    if (rec == NULL) {','DAttr *rec = NULL;\n    if (rec == NULL) {'),
 ('attr-value-live','attr','if (element != NULL) return JS_NewString(ctx, os64_html_attr(element, rec->name)->value);','if (element != NULL) return JS_NewString(ctx, "");'),
 ('attr-written-through','attr','    if (element != NULL) {\n        int64_t status = d_set_attr(','    if (false) {\n        int64_t status = d_set_attr('),
 ('attr-detached-own-value','attr','    rec->value = text->data;\n    rec->length = text->length;\n    text->data = NULL;','    (void)0;'),
 ('attr-detached-at-door','attr','if (rec != NULL) detach(dom, rec, kept, kept_length);','d_free(dom, kept);'),
 ('attr-last-value-kept','attr','if (rec != NULL) detach(dom, rec, kept, kept_length);','if (rec != NULL) { d_free(dom, kept); detach(dom, rec, NULL, 0); }'),
 ('attr-removal-refused-whole','attr','if (a != NULL && kept == NULL) return OS64_HTML_NO_MEMORY;','(void)0;'),
 ('attr-detached-document','attr','rec->document = document != window_document(dom) ? document : NULL;\n    rec->entry->target = NULL;','rec->document = NULL;\n    rec->entry->target = NULL;'),
 ('attr-in-use','attr','if (at != NULL && at != element)\n        return d_error(ctx, "InUseAttributeError"','if (false)\n        return d_error(ctx, "InUseAttributeError"'),
 ('attr-own-element-answers-itself','attr','if (at == element) return JS_DupValue(ctx, value);','if (at == element) return JS_NULL;'),
 ('attr-replaced-detached','attr','if (gone != NULL) detach(dom, gone, kept, kept_length);','d_free(dom, kept);'),
 ('attr-namespace-kept-or-refused','attr','if (old != os64_html_attr(element, rec->name) || (old == NULL && rec->ns != NULL))','if (false)'),
 ('attr-handler-slot','attr','if (d_handler_attribute_set(dom, ctx, element, rec->name) < 0) {','if (false) {'),
 ('attr-remove-node-own-only','attr','if (rec->entry->target != element) return d_error(ctx, "NotFoundError"','if (rec->entry->target == NULL) return d_error(ctx, "NotFoundError"'),
 ('attr-create-folds','attr','else if (d_document_type(dom, document) == D_DOCUMENT_HTML) d_fold(name.data);','else (void)0;'),
 ('attr-prefix-only-in-namespace','attr','const char *colon = ns != NULL ? os64_strchr(name, \':\') : NULL;','const char *colon = ((void)ns, os64_strchr(name, \':\'));'),
 ('attr-names-unenumerable','attr','.flags = JS_PROP_CONFIGURABLE | (numeric ? JS_PROP_ENUMERABLE : 0),','.flags = JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE,'),
 ('attr-uppercase-unlisted','attr','for (size_t i = 0; folds(element) && i < length; i++)','for (size_t i = 0; false && i < length; i++)'),
 ('attr-cursor-checked','attr','    return dom->attr_cursor.element == element && dom->attr_cursor.head == element->attrs &&\n           dom->attr_cursor.version == os64_html_version(dom->document);','    return dom->attr_cursor.element == element;'),
 ('attr-names-behind-prototype','attr','    int shown = visible(ctx, object, atom);\n    *found =','    int shown = 1;\n    *found ='),
 ('attr-looked-up-again','attr','    int shown = visible(ctx, object, atom);\n    *found = shown > 0 ? listed(dom, element, text, length) : NULL;','    *found = listed(dom, element, text, length);\n    int shown = visible(ctx, object, atom);\n    if (shown <= 0) *found = NULL;'),
 ('attr-index-read-only','attr','return JS_DefineProperty(ctx, refused ? entry->dom->index_guard->value : object, atom,','return JS_DefineProperty(ctx, object, atom,'),
 ('attr-shadow-whole','attr','        {"parentNode", A_NO_NODE, false}, {"parentElement", A_NO_NODE, false},','        {"parentElement", A_NO_NODE, false},'),
 ('attr-an-event-target','attr','if (d_event_target(dom, ctx, attr, dom->attr_class, NULL) < 0) return -1;','(void)0;'),
 ('attr-backstop-told','attr','if (dom->options.broken != NULL) dom->options.broken(dom->options.host_opaque, kBroken);','(void)kBroken;'),
 ('attr-node-names-attr','node','    if (node == NULL && d_is_attr(dom, value)) {','    if (false) {'),
 ('attr-tree-arguments','node','if (d_is_attr(dom, argv[0]) || (needed == 2 && d_is_attr(dom, argv[1]))) {','if (false) {'),
 ('attr-door-remove-attribute','node','int64_t status = d_set_attr(dom, node, name.data, value.data, value.length, magic == D_REMOVE_ATTR);','int64_t status = os64_page_node_set_attr(dom->state, node, name.data, value.data, value.length, magic == D_REMOVE_ATTR);'),
 ('attr-convert-refused','mixin','        if (args[i].attr) {\n            d_error(ctx, "HierarchyRequestError"','        if (false) {\n            d_error(ctx, "HierarchyRequestError"'),
 ('attr-contained-by-none','mixin','if (argc < 1 || JS_IsNull(argv[0]) || d_is_attr(dom, argv[0])) return JS_FALSE;','if (argc < 1 || JS_IsNull(argv[0])) return JS_FALSE;'),
]
if args.reclaim:
    mutants=[
      ('wrapper-hold','node','os64_html_hold(dom->document, node);','(void)node;'),
      ('wrapper-release','core','os64_html_release(dom->document, entry->node);','(void)entry->node;'),
      ('query-root-hold','collection','os64_html_hold(dom->document, root);','(void)root;'),
      ('query-root-release','collection','os64_html_release(dom->document, query->root);','(void)query->root;'),
      ('query-answer-hold','collection','os64_html_hold(query->dom->document, at);','(void)at;'),
      ('query-answer-release','collection','os64_html_release(query->dom->document, old[i]);','(void)old[i];'),
      ('empty-content-consumption','page-state','return node != NULL ? os64_html_insert(state->doc, (os64_html_node_t *)parent,\n            (os64_html_node_t *)node, NULL) : OS64_HTML_OK;','return OS64_HTML_OK;'),
    ]
if args.events:
    mutants=[
      ('capture-top-down','event','for (size_t i = count; result == 0 && i > 1; i--)\n        result = invoke_target(d, path[i - 1], PHASE_CAPTURING, true);','for (size_t i = 2; result == 0 && i <= count; i++)\n        result = invoke_target(d, path[i - 1], PHASE_CAPTURING, true);'),
      ('target-capture-first','event','if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, true);\n    if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, false);','if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, false);\n    if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, true);'),
      ('bubbles-flag','event','result == 0 && event->bubbles && i < count','result == 0 && i < count'),
      ('window-in-path','event','bool to_window = node == NULL || (top == document_node(dom) && kind != D_EVENT_LOAD);','bool to_window = node == NULL || (top == NULL && kind != D_EVENT_LOAD);'),
      ('once-removed','event','if (listener->once) remove_listener(dom, entry, listener);','(void)0;'),
      ('list-cut-at-reach','event','bool final = listener == last;','bool final = false;'),
            ('stop-immediate','event','if (event->stop_now) break;','(void)0;'),
      ('stop-propagation','event','if (entry == NULL || event->stop) return 0;','if (entry == NULL) return 0;'),
      ('handler-false-cancels','event','if (state != NULL) cancel(state);','(void)state;'),
      ('listener-false-ignored','event','if (listener->handler != D_LISTENER && JS_VALUE_GET_TAG(result) == JS_TAG_BOOL &&','if (JS_VALUE_GET_TAG(result) == JS_TAG_BOOL &&'),
      ('uncancelable','event','    if (event->cancelable) event->canceled = true;','    event->canceled = true;'),
      ('return-value-false','event','if (magic == E_RETURN_VALUE && !flag) cancel(event);','if (magic == E_RETURN_VALUE && flag) cancel(event);'),
      ('checkpoint-per-listener','event','os64_js_checkpoint(dom->runtime, OS64_JS_ABI_ID, &point);','os64_memset(&point, 0, sizeof(point));'),
      ('throwing-listener-next','event','if (!sticky(call.status)) {','if (call.status == OS64_JS_OK) {'),
      ('nested-error-reported','event','if (runtime_failed(dom)) {\n                        JS_Throw(d->ctx, error);','if (true) {\n                        JS_Throw(d->ctx, error);'),
      ('listener-dedupe','event','os64_strcmp(listener->type, type.data) == 0) found = listener;','os64_strcmp(listener->type, type.data) == 0 && magic == M_REMOVE) found = listener;'),
      ('document-scope','event','d_wrap(dom, ctx, document_node(dom)),\n        form != NULL','JS_NewObject(ctx),\n        form != NULL'),
      ('form-scope','event','form != NULL ? d_wrap(dom, ctx, form) : JS_NewObject(ctx),','form != NULL && false ? d_wrap(dom, ctx, form) : JS_NewObject(ctx),'),
      ('attribute-change-seen','event','    if (text == NULL ? slot->seen == NULL : slot->seen != NULL && os64_strcmp(text, slot->seen) == 0)\n        return 0;','    if (slot->handler != D_SLOT_EMPTY || text == NULL) return 0;'),
      ('failed-handler-absent','event','if (slot->handler == D_SLOT_UNCOMPILED && slot_compile(dom, ctx, entry, slot) < 0)','if ((slot->handler == D_SLOT_UNCOMPILED || slot->handler == D_SLOT_ERROR) && slot_compile(dom, ctx, entry, slot) < 0)'),
      ('body-reflects-window','event','return kind == D_EVENT_LOAD || kind == D_EVENT_FOCUS || kind == D_EVENT_BLUR;','return kind < 0;'),
      ('load-reads-document','event','node == NULL && d_event_kind(ev->type) == D_EVENT_LOAD};','false};'),
      ('parser-attribute-first','event','return slot_for(dom, ctx, entry, kind, entry != dom->window) != NULL ? 0 : -1;','return slot_for(dom, ctx, entry, kind, false) != NULL ? 0 : -1;'),
      ('window-attribute-before-a-listener','event','        for (int kind = 0; entry == dom->window && kind < D_EVENT_COUNT; kind++)','        for (int kind = 0; entry == dom->window && kind < 0; kind++)'),
      ('window-late-attribute-last','event','return slot_for(dom, ctx, entry, kind, entry != dom->window) != NULL ? 0 : -1;','return slot_for(dom, ctx, entry, kind, true) != NULL ? 0 : -1;'),
      ('ancestor-attribute-wrapped','event','if (entry == NULL && element_has_handler(at, kind)) {','if (entry == NULL && element_has_handler(at, kind) && false) {'),
      ('silent-without-listeners','event','    if (!os64_dom_listens(dom, ev->type)) return OS64_JS_OK;\n','\n'),
      ('attribute-count-version','event','if (dom->attribute_scanned && dom->attribute_version == version) return dom->attribute_mask;','if (dom->attribute_scanned) return dom->attribute_mask;'),
      ('report-folded','event','    if (outcome->status == OS64_JS_OK && dom->report.status != OS64_JS_OK) {\n        *outcome = dom->report;','    if (false) {\n        *outcome = dom->report;'),
      ('listener-drain','event','            if (!JS_IsUndefined(listener->callback)) JS_FreeValueRT(dom->engine, listener->callback);\n            listener->callback = JS_UNDEFINED;','            listener->callback = JS_UNDEFINED;'),
      ('timer-order','timer','((*link)->due == timer->due && (*link)->id < timer->id)','((*link)->due == timer->due && (*link)->id > timer->id)'),
      ('timer-negative-zero','timer','    if (delay < 0) delay = 0;\n','\n'),
      ('timer-cap','timer','if (dom->timer_count >= OS64_DOM_TIMERS_MAX','if (dom->timer_count > OS64_DOM_TIMERS_MAX'),
      ('timer-arguments','timer','timer->argc = extra;','timer->argc = 0;'),
      ('timer-clear','timer','if (timer != NULL) timer_free(dom, timer);','if (timer != NULL && false) timer_free(dom, timer);'),
      ('timer-drain','timer','    for (DTimer *timer = dom->timers; timer != NULL; timer = timer->next)\n        release_values(dom, timer);','    (void)0;'),
      ('navigation-last-wins','window','    /* The last ask of a task wins, as the last location= does in a browser. */','    if (dom->navigation.kind != OS64_DOM_NAVIGATE_NONE && kind != OS64_DOM_NAVIGATE_NONE) return;'),
      ('navigation-resolved','window','    return os64_url_absolute(&base, reference, out, cap);','    return os64_strcopy(out, cap, reference) < cap;'),
      ('hash-fragment','window','return set ? ask_url(dom, ctx, OS64_DOM_NAVIGATE_URL, value, true) : location_part(dom, ctx, magic);','return set ? ask_url(dom, ctx, OS64_DOM_NAVIGATE_URL, value, false) : location_part(dom, ctx, magic);'),
      ('checkbox-put-back','window','        if (result == 0) {\n            int64_t status = os64_page_node_set_checked(dom->state, node, was);','        if (false) {\n            int64_t status = os64_page_node_set_checked(dom->state, node, was);'),
      ('submit-event','window','return submit(dom, ctx, form, node, true);','return submit(dom, ctx, form, node, false);'),
      ('write-no-insertion-point-throws','window','    if (status == OS64_HTML_BAD_ARGUMENT) return d_error(ctx, "InvalidStateError", WRITE_ENDED);\n    return JS_UNDEFINED;','    if (false && status == OS64_HTML_BAD_ARGUMENT) return d_error(ctx, "InvalidStateError", WRITE_ENDED);\n    return JS_UNDEFINED;'),
      ('write-refusal-is-the-pages','window','    if (status == OS64_HTML_BAD_ARGUMENT) return d_error(ctx, "InvalidStateError", WRITE_ENDED);\n    return JS_UNDEFINED;','    if (status < 0) return d_error(ctx, "InvalidStateError", WRITE_ENDED);\n    return JS_UNDEFINED;'),
      ('write-joins-every-argument','window','    for (; done < argc && ok; done++) {','    for (; done < argc && done < 1 && ok; done++) {'),
      ('writeln-newline','window',"        if (line) text[at++] = '\\n';","        (void)line;"),
      ('write-budget-before-host','window','    if (ok && text == NULL) {\n        d_error(ctx, "QuotaExceededError", "document.write quota exceeded");\n        ok = false;\n    }','    if (ok && text == NULL) return JS_UNDEFINED;'),
      ('open-asks-the-host','window','        if (host_write(dom, "", 0) == OS64_HTML_BAD_ARGUMENT)\n            return d_error(ctx, "InvalidStateError", WRITE_ENDED);','        (void)host_write;'),
      ('no-host-no-insertion-point','window',': OS64_HTML_BAD_ARGUMENT;\n}',': OS64_HTML_OK;\n}'),
      ('module-not-classic','window','if (i == 6) return OS64_DOM_SCRIPT_MODULE;','if (i == 6) return OS64_DOM_SCRIPT_CLASSIC;'),
      ('connected-once','window','if (d_script_mark(dom, script) == 1) dom->options.script_connected','if (d_script_mark(dom, script) >= 0) dom->options.script_connected'),
      ('innerhtml-born-started','content','if (fragment != NULL && d_script_mark_tree(dom, fragment) < 0) status = OS64_HTML_NO_MEMORY;\n        else if','if (false) status = OS64_HTML_NO_MEMORY;\n        else if'),
      ('clone-carries-started','node','if (copy != NULL && d_script_copy_marks(dom, node, copy) >= 0)','if (copy != NULL)'),
      ('script-attribute-slot','node','} else if (d_handler_attribute_set(dom, ctx, node, name.data) < 0) {','} else if (false) {'),
      ('script-mixins-connect','node',"    if (status >= 0) d_script_connected(dom, scripts, found);","    (void)0;"),
    ]
flags=['-O2','-g','-std=gnu11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-tree-loop-distribute-patterns','-fno-stack-protector','-DOS64_JS_TARGET','-fsanitize=address,undefined','-fno-sanitize-recover=all']
for directory in ['libmath/include','libjs/port','libjs/include','libdom/include','libdom','libgarb/include','libhtml/include','libpage/include','libpage/upstream/ryu','libos64/include']:
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
if args.only: mutants=[m for m in mutants if args.only in m[0]]
caught=compiled=0
with tempfile.TemporaryDirectory(prefix='os64-dom-mutants.') as directory:
    directory=pathlib.Path(directory)
    for name,unit,old,new in mutants:
        page_unit=unit.removeprefix('page-') if unit.startswith('page-') else None
        source=(root/'userland'/('libpage' if page_unit else 'libdom')/f'{page_unit or unit}.c').read_text()
        if source.count(old)!=1:
            print(f'ANCHOR ERROR {name}: {source.count(old)} matches');continue
        path=directory/f'{name}.c';path.write_text(source.replace(old,new))
        obj=directory/f'{name}.o';binary=directory/name
        with (args.logs/f'{name}.log').open('w') as log:
            unit_flags=['-I'+str(root/'userland/libpage'),*flags[:-6]] if page_unit else flags
            result=subprocess.run(['cc',*unit_flags,'-c',str(path),'-o',str(obj)],stdout=log,stderr=log)
            if result.returncode: print(f'COMPILE ERROR {name}');continue
            replaced=f'userland_libpage_{page_unit}.c.o' if page_unit else f'dom-{unit}.o'
            selected=[p for p in objects if p.name!=replaced]
            result=subprocess.run(['cc','-fsanitize=address,undefined','-pthread','-Wl,-z,noexecstack',*maths,*map(str,selected),str(obj),'-o',str(binary)],cwd=root,stdout=log,stderr=log)
            if result.returncode: print(f'LINK ERROR {name}');continue
            compiled+=1
            try: result=subprocess.run([str(binary),'--reclaim' if args.reclaim else '--events' if args.events else '--mutants'],stdout=log,stderr=log,timeout=45)
            except subprocess.TimeoutExpired:
                print(f'TIMEOUT {name} (not counted)');continue
            if result.returncode:
                caught+=1;print(f'CAUGHT {name}')
            else: print(f'SURVIVED {name}')
print(f'DOM mutants: {caught} caught / {compiled} compiling / {len(mutants)} proposed; logs {args.logs}')
raise SystemExit(0 if caught==compiled==len(mutants) else 1)
