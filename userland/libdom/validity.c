#include "internal.h"
#include "os64/fmt.h"

/* CONSTRAINT VALIDATION (HTML § 4.10.20; 0f's P4, INTERFACES_FORMS.md § P4).
 *
 * libpage judges a control (os64_page_node_validity): the same judge a
 * submission's validation asks, from the node and the live state, so a
 * control no page holds is judged too. What the binding adds is the
 * engine's half and the events:
 *   - the pattern, which HTML makes a regular expression of the engine's,
 *     matched here with the RegExp taken before any page ran;
 *   - ValidityState, one per element, whose members read the judge each
 *     time they are read, as browsers' flags follow the value;
 *   - willValidate, validity, validationMessage, checkValidity,
 *     reportValidity and setCustomValidity on input, select, textarea,
 *     button, fieldset and output; a form's checkValidity and
 *     reportValidity;
 *   - `invalid`, fired at each invalid control those methods find, and at
 *     each a submission's validation finds before submit fires (window.c's
 *     submit, and os64_dom_form_validate for a person's).
 * A report is the host's (ruling 5): the control focused and its message
 * on the status line, through OS64_DOM_ACTIVATE_REPORT.
 *
 * One row of the Modern Web table, naming ValidityState and oninvalid on
 * window. */

static JSClassID validity_slot;

/* ValidityState's members, in the order of libpage's bits; V_VALID last. */
static const char *const kFlags[] = {
    "valueMissing", "typeMismatch", "patternMismatch", "tooLong", "tooShort", "rangeUnderflow",
    "rangeOverflow", "stepMismatch", "badInput", "customError", "valid",
};
enum { V_VALID = 10 };
_Static_assert(OS64_PAGE_VALIDITY_CUSTOM == 1u << (V_VALID - 1), "ValidityState's members follow libpage's bits");

enum { M_WILL_VALIDATE, M_VALIDITY, M_MESSAGE, M_CHECK, M_REPORT, M_SET_CUSTOM, M_FORM_CHECK, M_FORM_REPORT };

static void validity_finalizer(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

static bool html(const os64_html_node_t *node, os64_html_tag_t tag)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML && node->tag == tag;
}

/* HTML's submittable elements, which a form's validation walks. */
static bool submittable(const os64_html_node_t *node)
{
    return html(node, OS64_HTML_TAG_INPUT) || html(node, OS64_HTML_TAG_BUTTON) || html(node, OS64_HTML_TAG_SELECT) ||
        html(node, OS64_HTML_TAG_TEXTAREA);
}

typedef struct {
    os64_dom_t *dom;
    JSContext *ctx;
    /* An exception the matching leaves pending, and the judge's answer is
     * not used: one it could not swallow (the task's deadline), or the
     * binding's own QuotaExceededError, which is the page's to catch. */
    bool failed;
} Matching;

/* A RegExp of `source` (`len` bytes) with the `v` flag, by the intrinsic
 * constructor; JS_EXCEPTION when it does not compile. */
static JSValue compile(Matching *m, const char *source, size_t len)
{
    JSContext *ctx = m->ctx;
    JSValue args[2] = {JS_NewStringLen(ctx, source, len), JS_NewString(ctx, "v")};
    JSValue re = JS_IsException(args[0]) || JS_IsException(args[1]) ? JS_EXCEPTION
        : JS_CallConstructor(ctx, m->dom->regexp->value, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return re;
}

/* HTML's pattern (§ 4.10.5.3.6): it must compile alone with the `v` flag,
 * so one that closes the group the anchoring opens is refused; then
 * `^(?:pattern)$` is matched against the value. A pattern that does not
 * compile is no constraint. */
static int match(void *opaque, const char *pattern, const char *value, size_t len)
{
    Matching *m = opaque;
    if (m->failed) return -1;
    JSContext *ctx = m->ctx;
    size_t plen = os64_strlen(pattern);
    JSValue alone = compile(m, pattern, plen);
    JSValue re = JS_EXCEPTION;
    if (!JS_IsException(alone)) {
        JS_FreeValue(ctx, alone);
        char *source = d_alloc(m->dom, plen + 8);
        if (source == NULL) {
            m->failed = true;
            d_error(ctx, "QuotaExceededError", "DOM validity quota exceeded");
            return -1;
        }
        os64_snprintf(source, plen + 8, "^(?:%s)$", pattern);
        re = compile(m, source, plen + 6);
        d_free(m->dom, source);
    }
    int result = -1;
    if (!JS_IsException(re)) {
        JSValue text = JS_NewStringLen(ctx, value, len);
        JSValue found = JS_IsException(text) ? JS_EXCEPTION
            : JS_Call(ctx, m->dom->regexp_exec->value, re, 1, (JSValueConst *)&text);
        JS_FreeValue(ctx, text);
        if (!JS_IsException(found)) result = JS_IsNull(found) ? 0 : 1;
        JS_FreeValue(ctx, found);
        JS_FreeValue(ctx, re);
    }
    if (result < 0 && JS_HasException(ctx)) {
        if (d_exception_catchable(ctx)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            m->failed = true;
        }
    }
    return result;
}

/* The node's flags, with its pattern matched. -1 with an exception
 * pending. */
static int flags_of(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, uint32_t *flags)
{
    Matching m = {dom, ctx, false};
    int64_t status = os64_page_node_validity(dom->state, node, match, &m, flags);
    if (m.failed) return -1;
    if (status < 0) { d_page_error(ctx, status); return -1; }
    return 0;
}

/* Whether the node is a candidate for constraint validation that does not
 * satisfy its constraints. -1 with an exception pending. */
static int invalid(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, uint32_t *flags)
{
    bool will = false;
    int64_t status = os64_page_node_will_validate(dom->state, node, &will);
    if (status < 0) { d_page_error(ctx, status); return -1; }
    *flags = 0;
    if (!will) return 0;
    if (flags_of(dom, ctx, node, flags) < 0) return -1;
    return *flags != 0;
}

/* `invalid` at the node, cancelable and not bubbling: 1 when nobody
 * cancelled it, 0 when somebody did, -1 with an exception pending. */
static int fire_invalid(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    JSValue event = d_event_new(dom, ctx, "invalid", false, true, true);
    if (JS_IsException(event)) return -1;
    int result = d_dispatch(dom, ctx, event, node);
    JS_FreeValue(ctx, event);
    return result;
}

/* Report the node's problem to the person: the host focuses it and says
 * its message, kept here for the host to read after the task. Only for a
 * node of the window's document: no other has a screen to report on. */
static int report(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    uint32_t flags = 0;
    if (flags_of(dom, ctx, node, &flags) < 0) return -1;
    if (dom->options.activate == NULL || !d_window_document(dom, node)) return 0;
    int64_t status = os64_page_node_validation_message(dom->state, node, flags, dom->reported_message,
                                                       sizeof(dom->reported_message));
    if (status < 0) { d_page_error(ctx, status); return -1; }
    dom->reported = node;
    dom->options.activate(dom->options.host_opaque, node, OS64_DOM_ACTIVATE_REPORT);
    return 0;
}

int d_form_validate(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *form, bool report_first)
{
    if (!dom->options.modern) return 1;
    /* The invalid controls first, then their events: a listener that
     * changes the tree changes neither the list nor its order. Each is
     * held until its event has been fired. */
    const os64_html_node_t *root = form;
    while (root->parent != NULL) root = root->parent;
    size_t count = 0, cap = 0;
    const os64_html_node_t **list = NULL;
    int result = 1;
    for (const os64_html_node_t *at = root; at != NULL && result >= 0;) {
        if (submittable(at) && d_form_owner(at) == form) {
            uint32_t flags = 0;
            int bad = invalid(dom, ctx, at, &flags);
            if (bad < 0) result = -1;
            else if (bad > 0) {
                if (count == cap) {
                    size_t grown = cap != 0 ? cap * 2 : 8;
                    const os64_html_node_t **bigger = d_alloc(dom, grown * sizeof(*bigger));
                    if (bigger == NULL) {
                        result = -1;
                        d_error(ctx, "QuotaExceededError", "DOM validity quota exceeded");
                        break;
                    }
                    if (count != 0) os64_memcpy(bigger, list, count * sizeof(*list));
                    d_free(dom, list);
                    list = bigger;
                    cap = grown;
                }
                os64_html_hold(dom->document, at);
                list[count++] = at;
            }
        }
        if (at->first_child != NULL) { at = at->first_child; continue; }
        while (at != root && at->next == NULL) at = at->parent;
        at = at != root ? at->next : NULL;
    }
    const os64_html_node_t *unhandled = NULL;
    for (size_t i = 0; i < count; i++) {
        if (result >= 0) {
            int heard = fire_invalid(dom, ctx, list[i]);
            if (heard < 0) result = -1;
            else {
                result = 0;
                if (heard > 0 && unhandled == NULL) unhandled = list[i];
            }
        }
    }
    if (result == 0 && report_first && unhandled != NULL && report(dom, ctx, unhandled) < 0) result = -1;
    for (size_t i = 0; i < count; i++) os64_html_release(dom->document, list[i]);
    d_free(dom, list);
    return result;
}

static JSValue validity_object(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    DValue *owner = d_find(dom, node);
    if (owner == NULL) return JS_ThrowTypeError(ctx, "Missing node wrapper");
    if (owner->validity != NULL) return JS_DupValue(ctx, owner->validity->value);
    JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&validity_slot));
    DValue *entry = d_retain(dom, ctx, object);
    if (entry == NULL) return JS_EXCEPTION;
    entry->dom = dom;
    entry->target = node;
    os64_html_hold(dom->document, node);
    JS_SetOpaque(object, entry);
    owner->validity = entry;
    return JS_DupValue(ctx, object);
}

static JSValue validity_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DValue *entry = JS_GetOpaque(this_value, os64_js_class_id(&validity_slot));
    if (entry == NULL || entry->target == NULL) return JS_ThrowTypeError(ctx, "Expected a ValidityState");
    uint32_t flags = 0;
    if (flags_of(dom, ctx, entry->target, &flags) < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, magic == V_VALID ? flags == 0 : (flags & (1u << magic)) != 0);
}

static JSValue control_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, this_value);
    if (node == NULL) return JS_EXCEPTION;
    if (magic == M_FORM_CHECK || magic == M_FORM_REPORT) {
        if (!html(node, OS64_HTML_TAG_FORM)) return JS_ThrowTypeError(ctx, "Illegal invocation");
        int valid = d_form_validate(dom, ctx, node, magic == M_FORM_REPORT);
        return valid < 0 ? JS_EXCEPTION : JS_NewBool(ctx, valid > 0);
    }
    bool kind = false;
    int64_t status = os64_page_node_will_validate(dom->state, node, &kind);
    if (status < 0) return JS_ThrowTypeError(ctx, "Illegal invocation");
    uint32_t flags = 0;
    switch (magic) {
    case M_WILL_VALIDATE:
        return JS_NewBool(ctx, kind);
    case M_VALIDITY:
        return validity_object(dom, ctx, node);
    case M_MESSAGE: {
        int bad = invalid(dom, ctx, node, &flags);
        if (bad <= 0) return bad < 0 ? JS_EXCEPTION : JS_NewString(ctx, "");
        if (flags & OS64_PAGE_VALIDITY_CUSTOM) {
            /* The custom message whole, as it was set, however long. */
            const char *custom = "";
            size_t length = 0;
            status = os64_page_node_custom_validity(dom->state, node, &custom, &length);
            return status < 0 ? d_page_error(ctx, status) : JS_NewStringLen(ctx, custom, length);
        }
        char message[256];
        status = os64_page_node_validation_message(dom->state, node, flags, message, sizeof(message));
        return status < 0 ? d_page_error(ctx, status) : JS_NewString(ctx, message);
    }
    case M_CHECK: case M_REPORT: {
        int bad = invalid(dom, ctx, node, &flags);
        if (bad <= 0) return bad < 0 ? JS_EXCEPTION : JS_TRUE;
        int heard = fire_invalid(dom, ctx, node);
        if (heard < 0) return JS_EXCEPTION;
        if (magic == M_REPORT && heard > 0 && report(dom, ctx, node) < 0) return JS_EXCEPTION;
        return JS_FALSE;
    }
    case M_SET_CUSTOM: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing validity message");
        DString text = {0};
        if (!d_string(dom, ctx, argv[0], &text)) return JS_EXCEPTION;
        status = os64_page_node_set_custom_validity(dom->state, node, text.data, text.length);
        d_string_free(dom, &text);
        return status < 0 ? d_page_error(ctx, status) : JS_UNDEFINED;
    }
    default:
        return JS_ThrowTypeError(ctx, "Unknown validity member");
    }
}

/* Take `name` of `from` and keep it for the binding's life (the registry
 * owns the reference d_retain is given). */
static DValue *take(os64_dom_t *dom, JSContext *ctx, JSValueConst from, const char *name)
{
    JSValue value = JS_GetPropertyStr(ctx, from, name);
    if (JS_IsException(value)) return NULL;
    if (!JS_IsFunction(ctx, value)) {
        JS_FreeValue(ctx, value);
        JS_ThrowTypeError(ctx, "Missing intrinsic %s", name);
        return NULL;
    }
    return d_retain(dom, ctx, value);
}

int d_validity_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSClassID id = os64_js_class_id(&validity_slot);
    static const JSClassDef definition = {.class_name = "ValidityState", .finalizer = validity_finalizer};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    dom->regexp = take(dom, ctx, global, "RegExp");
    if (dom->regexp == NULL) return -1;
    JSValue regexp_prototype = JS_GetPropertyStr(ctx, dom->regexp->value, "prototype");
    if (JS_IsException(regexp_prototype)) return -1;
    dom->regexp_exec = take(dom, ctx, regexp_prototype, "exec");
    JS_FreeValue(ctx, regexp_prototype);
    if (dom->regexp_exec == NULL) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (JS_IsException(prototype)) return -1;
    dom->validity_prototype = d_retain(dom, ctx, prototype);
    if (dom->validity_prototype == NULL) return -1;
    for (int i = 0; i <= V_VALID; i++)
        if (d_accessor(dom, ctx, prototype, kFlags[i], validity_member, i, false) < 0) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    JSValue interface = d_interface_new(ctx, "ValidityState", 0, NULL, prototype, JS_UNDEFINED);
    if (JS_IsException(interface)) return -1;
    if (JS_DefinePropertyValueStr(ctx, global, "ValidityState", interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) <
        0)
        return -1;
    /* The six kinds' members; a fieldset's and an output's are its custom
     * validity alone, as willValidate is false for both. */
    static const int kKinds[] = {D_PROTO_INPUT, D_PROTO_TEXTAREA, D_PROTO_SELECT, D_PROTO_BUTTON, D_PROTO_FIELDSET,
                                 D_PROTO_OUTPUT};
    for (size_t k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); k++) {
        JSValueConst on = dom->prototypes[kKinds[k]]->value;
        if (d_accessor(dom, ctx, on, "willValidate", control_member, M_WILL_VALIDATE, false) < 0 ||
            d_accessor(dom, ctx, on, "validity", control_member, M_VALIDITY, false) < 0 ||
            d_accessor(dom, ctx, on, "validationMessage", control_member, M_MESSAGE, false) < 0 ||
            d_method(dom, ctx, on, "checkValidity", control_member, 0, M_CHECK) < 0 ||
            d_method(dom, ctx, on, "reportValidity", control_member, 0, M_REPORT) < 0 ||
            d_method(dom, ctx, on, "setCustomValidity", control_member, 1, M_SET_CUSTOM) < 0)
            return -1;
    }
    JSValueConst form = dom->prototypes[D_PROTO_FORM]->value;
    if (d_method(dom, ctx, form, "checkValidity", control_member, 0, M_FORM_CHECK) < 0 ||
        d_method(dom, ctx, form, "reportValidity", control_member, 0, M_FORM_REPORT) < 0)
        return -1;
    /* oninvalid is every element's, the document's and window's (HTML's
     * GlobalEventHandlers). */
    JSValueConst holders[] = {dom->prototypes[D_PROTO_ELEMENT]->value, dom->prototypes[D_PROTO_DOCUMENT]->value,
                              global};
    for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++)
        if (d_event_handler(dom, ctx, holders[i], D_EVENT_INVALID) < 0) return -1;
    return 0;
}

os64_js_status_t os64_dom_form_validate(os64_dom_t *dom, const os64_html_node_t *form,
                                        const os64_html_node_t *submitter, bool *valid, bool *ran,
                                        os64_js_outcome_t *outcome)
{
    if (valid != NULL) *valid = true;
    if (ran != NULL) *ran = false;
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (dom == NULL || valid == NULL || ran == NULL || form == NULL || !os64_html_owns_node(dom->document, form) ||
        (submitter != NULL && !os64_html_owns_node(dom->document, submitter))) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return outcome->status;
    }
    if (dom->closed) {
        outcome->status = OS64_JS_FAILED_RUNTIME;
        return outcome->status;
    }
    /* The submitter's no-validate state: its formnovalidate, or the form's
     * novalidate. */
    bool skip = (submitter != NULL && os64_html_attr(submitter, "formnovalidate") != NULL) ||
        os64_html_attr(form, "novalidate") != NULL;
    if (!dom->options.modern || skip) return OS64_JS_OK;
    *ran = true;
    os64_js_outcome_t begin;
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, "invalid event", &begin) != OS64_JS_OK) {
        *outcome = begin;
        return outcome->status;
    }
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    if (ctx == NULL) return d_task_finish(dom, outcome);
    /* The form is being submitted (window.c's submit's flag): a submission
     * an invalid listener asks of it does nothing. */
    struct DClicking self = {form, dom->submitting};
    dom->submitting = &self;
    int result = d_form_validate(dom, ctx, form, true);
    dom->submitting = self.next;
    if (result < 0 && JS_HasException(ctx)) {
        /* A runtime that must stop is the task's failure, which task_end
         * reports; the binding's own refusal is the page's error. */
        JSValue error = JS_GetException(ctx);
        if (d_exception_catchable(ctx)) d_report(dom, ctx, error);
        JS_FreeValue(ctx, error);
    }
    /* A validation the binding could not finish stops nothing: the
     * model's judge still validates the submission (libpage's
     * os64_page_activate). */
    *valid = result != 0;
    return d_task_finish(dom, outcome);
}

bool os64_dom_report_message(const os64_dom_t *dom, const os64_html_node_t *node, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    if (dom == NULL || node == NULL || dom->reported != node) return false;
    os64_strcopy(out, cap, dom->reported_message);
    return true;
}
