#include "internal.h"
#include "os64/fmt.h"
#include "os64/url.h"

/* What a script may ask of the page around it: location and history (asks
 * the host performs after the task), the old web's window.status, element and
 * form actions, and HTML's script-element flags. LIBDOM.md § Asks. */

static const char *page_url(const os64_dom_t *dom)
{
    return dom->url != NULL ? dom->url : "about:blank";
}

void d_navigate(os64_dom_t *dom, os64_dom_navigation_kind_t kind, const os64_html_node_t *node,
                const os64_html_node_t *submitter, const char *url, int32_t delta)
{
    /* The last ask of a task wins, as the last location= does in a browser. */
    if (node != NULL) os64_html_hold(dom->document, node);
    if (submitter != NULL) os64_html_hold(dom->document, submitter);
    if (dom->navigation.node != NULL) os64_html_release(dom->document, dom->navigation.node);
    if (dom->navigation.submitter != NULL) os64_html_release(dom->document, dom->navigation.submitter);
    dom->navigation.kind = kind;
    dom->navigation.node = node;
    dom->navigation.submitter = submitter;
    dom->navigation.delta = delta;
    os64_strcopy(dom->navigation.url, sizeof(dom->navigation.url), url != NULL ? url : "");
}

bool os64_dom_take_navigation(os64_dom_t *dom, os64_dom_navigation_t *ask)
{
    if (ask != NULL) os64_memset(ask, 0, sizeof(*ask));
    if (dom == NULL || ask == NULL || dom->navigation.kind == OS64_DOM_NAVIGATE_NONE) return false;
    ask->kind = dom->navigation.kind;
    ask->node = dom->navigation.node;
    ask->submitter = dom->navigation.submitter;
    ask->delta = dom->navigation.delta;
    os64_strcopy(ask->url, sizeof(ask->url), dom->navigation.url);
    os64_memset(&dom->navigation, 0, sizeof(dom->navigation));
    return true;
}

bool os64_dom_take_report(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    if (outcome != NULL) os64_memset(outcome, 0, sizeof(*outcome));
    if (dom == NULL || outcome == NULL || dom->report.status == OS64_JS_OK) return false;
    *outcome = dom->report;
    os64_memset(&dom->report, 0, sizeof(dom->report));
    return true;
}

/* A reference resolved against the page, as a link's href is. An absolute
 * reference needs no base, so about:blank still reaches one. */
static bool resolve(os64_dom_t *dom, const char *reference, char *out, size_t cap)
{
    char scheme[OS64_URL_SCHEME_MAX];
    if (os64_url_scheme_of(reference, scheme, sizeof(scheme)))
        return os64_strcopy(out, cap, reference) < cap;
    os64_url_t base;
    if (os64_url_parse(page_url(dom), &base) != OS64_URL_OK) return false;
    return os64_url_absolute(&base, reference, out, cap);
}

enum {
    L_HREF, L_PROTOCOL, L_HOST, L_HOSTNAME, L_PORT, L_PATHNAME, L_SEARCH, L_HASH, L_ORIGIN,
    L_ASSIGN, L_REPLACE, L_RELOAD, L_TO_STRING,
    H_BACK, H_FORWARD, H_GO, H_LENGTH, W_STATUS, W_WRITE, W_WRITELN, W_OPEN, W_CLOSE
};

/* document.write is the host's parser taking the text (LIBDOM.md
 * § document.write): the binding joins the arguments, the host's `write`
 * answers libhtml's status, and that answer is all the binding knows of
 * parsing. A refusal of the parse is the page's and not the script's; only
 * "there is no insertion point" throws. */
#define WRITE_ENDED "document.write after the parse has ended would replace the document, which this browser does not do"

static int64_t host_write(os64_dom_t *dom, const char *utf8, size_t length)
{
    return dom->options.write != NULL ? dom->options.write(dom->options.host_opaque, utf8, length)
                                      : OS64_HTML_BAD_ARGUMENT;
}

static JSValue document_write(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, bool line)
{
    DString *parts = argc > 0 ? d_alloc(dom, sizeof(*parts) * (size_t)argc) : NULL;
    if (argc > 0 && parts == NULL) return d_error(ctx, "QuotaExceededError", "document.write quota exceeded");
    size_t length = line ? 1 : 0;
    int done = 0;
    bool ok = true;
    for (; done < argc && ok; done++) {
        ok = d_string(dom, ctx, argv[done], &parts[done]);
        if (ok && parts[done].length > SIZE_MAX - 1 - length) {
            d_string_free(dom, &parts[done]);
            d_error(ctx, "QuotaExceededError", "document.write quota exceeded");
            ok = false;
        }
        if (ok) length += parts[done].length;
    }
    if (!ok) done--;
    char *text = ok ? d_alloc(dom, length + 1) : NULL;
    if (ok && text == NULL) {
        d_error(ctx, "QuotaExceededError", "document.write quota exceeded");
        ok = false;
    }
    if (ok) {
        size_t at = 0;
        for (int i = 0; i < argc; i++) {
            os64_memcpy(text + at, parts[i].data, parts[i].length);
            at += parts[i].length;
        }
        if (line) text[at++] = '\n';
        text[at] = '\0';
    }
    for (int i = 0; i < done; i++) d_string_free(dom, &parts[i]);
    d_free(dom, parts);
    if (!ok) return JS_EXCEPTION;
    int64_t status = host_write(dom, text, length);
    d_free(dom, text);
    if (status == OS64_HTML_BAD_ARGUMENT) return d_error(ctx, "InvalidStateError", WRITE_ENDED);
    return JS_UNDEFINED;
}

/* The page's address taken apart for location's readers. The parser drops
 * a fragment and keeps the query in the path; location splits them. */
static JSValue location_part(os64_dom_t *dom, JSContext *ctx, int part)
{
    const char *url = page_url(dom);
    const char *hash = url;
    while (*hash != '\0' && *hash != '#') hash++;
    if (part == L_HREF) return JS_NewString(ctx, url);
    if (part == L_HASH) return JS_NewString(ctx, hash[0] == '#' && hash[1] != '\0' ? hash : "");
    os64_url_t parsed;
    if (os64_url_parse(url, &parsed) != OS64_URL_OK) {
        char scheme[OS64_URL_SCHEME_MAX + 1];
        if (part == L_PROTOCOL && os64_url_scheme_of(url, scheme, sizeof(scheme) - 1)) {
            os64_strcopy(scheme + os64_strlen(scheme), 2, ":");
            return JS_NewString(ctx, scheme);
        }
        return JS_NewString(ctx, part == L_ORIGIN ? "null" : "");
    }
    const char *query = parsed.path;
    while (*query != '\0' && *query != '?') query++;
    char text[OS64_URL_HOST_MAX + OS64_URL_SCHEME_MAX + 16];
    switch (part) {
    case L_PROTOCOL: os64_snprintf(text, sizeof(text), "%s:", parsed.scheme); break;
    case L_HOSTNAME: return JS_NewString(ctx, parsed.host);
    case L_PORT:
        if (parsed.port == 0) return JS_NewString(ctx, "");
        os64_snprintf(text, sizeof(text), "%u", (unsigned)parsed.port);
        break;
    case L_HOST: case L_ORIGIN: {
        size_t at = 0;
        if (part == L_ORIGIN) {
            os64_snprintf(text, sizeof(text), "%s://", parsed.scheme);
            at = os64_strlen(text);
        }
        if (parsed.port != 0)
            os64_snprintf(text + at, sizeof(text) - at, "%s:%u", parsed.host, (unsigned)parsed.port);
        else os64_snprintf(text + at, sizeof(text) - at, "%s", parsed.host);
        break;
    }
    case L_PATHNAME: return JS_NewStringLen(ctx, parsed.path, (size_t)(query - parsed.path));
    case L_SEARCH: return JS_NewString(ctx, query[0] == '?' && query[1] != '\0' ? query : "");
    default: return JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, text);
}

static JSValue ask_url(os64_dom_t *dom, JSContext *ctx, os64_dom_navigation_kind_t kind, JSValueConst value,
                       bool fragment)
{
    DString text = {0};
    if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
    char url[OS64_DOM_URL_MAX];
    bool resolved;
    if (fragment) {
        /* hash = names a place in this page: the address with its fragment
         * replaced, which the host reaches without a request. */
        const char *base = page_url(dom);
        size_t keep = 0;
        while (base[keep] != '\0' && base[keep] != '#') keep++;
        const char *name = text.data[0] == '#' ? text.data + 1 : text.data;
        resolved = keep + 1 + os64_strlen(name) < sizeof(url);
        if (resolved) os64_snprintf(url, sizeof(url), "%.*s#%s", (int)keep, base, name);
    } else resolved = resolve(dom, text.data, url, sizeof(url));
    d_string_free(dom, &text);
    if (!resolved) return d_error(ctx, "SyntaxError", "The address cannot be resolved against this page");
    d_navigate(dom, kind, NULL, NULL, url, 0);
    return JS_UNDEFINED;
}

static JSValue window_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                             int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    switch (magic) {
    case L_HREF:
        return set ? ask_url(dom, ctx, OS64_DOM_NAVIGATE_URL, value, false) : location_part(dom, ctx, magic);
    case L_HASH:
        return set ? ask_url(dom, ctx, OS64_DOM_NAVIGATE_URL, value, true) : location_part(dom, ctx, magic);
    case L_PROTOCOL: case L_HOST: case L_HOSTNAME: case L_PORT: case L_PATHNAME:
    case L_SEARCH: case L_ORIGIN:
        return set ? JS_UNDEFINED : location_part(dom, ctx, magic);
    case L_ASSIGN: return ask_url(dom, ctx, OS64_DOM_NAVIGATE_URL, value, false);
    case L_REPLACE: return ask_url(dom, ctx, OS64_DOM_NAVIGATE_REPLACE, value, false);
    case L_RELOAD: d_navigate(dom, OS64_DOM_NAVIGATE_RELOAD, NULL, NULL, page_url(dom), 0); return JS_UNDEFINED;
    case L_TO_STRING: return location_part(dom, ctx, L_HREF);
    case H_BACK: case H_FORWARD: case H_GO: {
        int32_t delta = magic == H_BACK ? -1 : 1;
        if (magic == H_GO) {
            delta = 0;
            if (argc > 0 && JS_ToInt32(ctx, &delta, value) < 0) return JS_EXCEPTION;
        }
        /* go(0) is a reload, as HTML's traversal makes it. */
        if (delta == 0) d_navigate(dom, OS64_DOM_NAVIGATE_RELOAD, NULL, NULL, page_url(dom), 0);
        else d_navigate(dom, OS64_DOM_NAVIGATE_HISTORY, NULL, NULL, NULL, delta);
        return JS_UNDEFINED;
    }
    case H_LENGTH: return JS_NewInt32(ctx, 1);
    case W_STATUS:
        /* The status line is the browser's; the old web assigns window.status
         * on every mouseover and expects no complaint. */
        return set ? JS_UNDEFINED : JS_NewString(ctx, "");
    case W_WRITE: case W_WRITELN:
        return document_write(dom, ctx, argc, argv, magic == W_WRITELN);
    case W_OPEN:
        /* open() over the parser that is running answers the document and
         * changes nothing; any other open() would make a new document. An
         * empty write asks the host whether there is an insertion point. */
        if (host_write(dom, "", 0) == OS64_HTML_BAD_ARGUMENT)
            return d_error(ctx, "InvalidStateError", WRITE_ENDED);
        return JS_DupValue(ctx, self);
    case W_CLOSE:
        /* close() acts only on a parser open() created, and none is. */
        return JS_UNDEFINED;
    default: return JS_ThrowTypeError(ctx, "Unknown window member");
    }
}

/* location itself: reading answers the object, assigning navigates, on
 * window and on document alike. */
static JSValue location_property(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                 int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic & D_SET) return ask_url(dom, ctx, OS64_DOM_NAVIGATE_URL, argc > 0 ? argv[0] : JS_UNDEFINED, false);
    return JS_DupValue(ctx, dom->location->value);
}

static bool html_element(const os64_html_node_t *node, os64_html_tag_t tag)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML && node->tag == tag;
}

static bool type_is(const os64_html_node_t *node, const char *wanted, bool missing)
{
    const os64_html_attr_t *type = os64_html_attr(node, "type");
    if (type == NULL) return missing;
    const char *a = type->value, *b = wanted;
    while (*a != '\0' && *b != '\0' && (*a | 0x20) == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

bool d_connected(const os64_dom_t *dom, const os64_html_node_t *node)
{
    while (node->parent != NULL) node = node->parent;
    return node == dom->document->document;
}

static bool form_associated(const os64_html_node_t *node)
{
    if (node == NULL || node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML) return false;
    switch (node->tag) {
    case OS64_HTML_TAG_INPUT: case OS64_HTML_TAG_BUTTON: case OS64_HTML_TAG_SELECT:
    case OS64_HTML_TAG_TEXTAREA: case OS64_HTML_TAG_FIELDSET: case OS64_HTML_TAG_OUTPUT:
    case OS64_HTML_TAG_OBJECT:
        return true;
    default:
        return false;
    }
}

/* HTML's form owner: the parser's record, else the form a `form` attribute
 * names, else the nearest form ancestor. libhtml clears a record the moment
 * it stops being true, so a record found is believed. */
const os64_html_node_t *d_form_owner(const os64_dom_t *dom, const os64_html_node_t *node)
{
    if (!form_associated(node)) return NULL;
    if (node->form_owner != NULL) return node->form_owner;
    const os64_html_attr_t *named = os64_html_attr(node, "form");
    if (named != NULL) {
        if (!d_connected(dom, node)) return NULL;
        const os64_html_node_t *root = dom->document->document;
        for (const os64_html_node_t *at = root; at != NULL;) {
            const os64_html_attr_t *id = at->kind == OS64_HTML_ELEMENT ? os64_html_attr(at, "id") : NULL;
            if (id != NULL && os64_strcmp(id->value, named->value) == 0)
                return html_element(at, OS64_HTML_TAG_FORM) ? at : NULL;
            if (at->first_child != NULL) { at = at->first_child; continue; }
            while (at != root && at->next == NULL) at = at->parent;
            at = at != root ? at->next : NULL;
        }
        return NULL;
    }
    for (const os64_html_node_t *at = node->parent; at != NULL; at = at->parent)
        if (html_element(at, OS64_HTML_TAG_FORM)) return at;
    return NULL;
}

static bool disabled(const os64_html_node_t *node)
{
    return form_associated(node) && node->tag != OS64_HTML_TAG_OBJECT && node->tag != OS64_HTML_TAG_OUTPUT &&
        os64_html_attr(node, "disabled") != NULL;
}

/* Fire a plain event a script's action raises, inside the script's task. */
static int fire(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, const char *type,
                bool bubbles, bool cancelable)
{
    JSValue event = d_event_new(dom, ctx, type, bubbles, cancelable, false);
    if (JS_IsException(event)) return -1;
    int result = d_dispatch(dom, ctx, event, node);
    JS_FreeValue(ctx, event);
    return result;
}

static void notify(os64_dom_t *dom, const os64_html_node_t *node, os64_dom_activation_t what)
{
    if (dom->options.activate != NULL) dom->options.activate(dom->options.host_opaque, node, what);
}

enum { A_CLICK, A_FOCUS, A_BLUR, A_SUBMIT, A_REQUEST_SUBMIT, A_RESET };

/* A form's submission: HTML fires submit at the form unless the script
 * called submit() itself, and a submit nobody cancelled becomes the ask. */
static int submit(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *form,
                  const os64_html_node_t *submitter, bool event)
{
    int result = event ? fire(dom, ctx, form, "submit", true, true) : 1;
    if (result > 0) d_navigate(dom, OS64_DOM_NAVIGATE_SUBMIT, form, submitter, NULL, 0);
    return result;
}

static int reset(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *form)
{
    int result = fire(dom, ctx, form, "reset", true, true);
    if (result > 0) notify(dom, form, OS64_DOM_ACTIVATE_RESET);
    return result;
}

/* element.click() is a synthetic click: the event, then the element's
 * activation behaviour unless a listener cancelled it. A checkbox or radio
 * changes before its click is dispatched and is put back when the click is
 * cancelled (HTML's legacy-pre-activation behaviour). */
static int click(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    if (disabled(node)) return 1;
    bool checkable = html_element(node, OS64_HTML_TAG_INPUT) &&
        (type_is(node, "checkbox", false) || type_is(node, "radio", false));
    bool was = false;
    if (checkable) {
        int64_t status = os64_page_node_checked(dom->state, node, &was);
        bool radio = type_is(node, "radio", false);
        if (status >= 0) status = os64_page_node_set_checked(dom->state, node, radio ? true : !was);
        if (status < 0) { d_page_error(ctx, status); return -1; }
    }
    int result = fire(dom, ctx, node, "click", true, true);
    if (result < 0) return result;
    if (checkable) {
        bool now = false;
        if (result == 0) {
            int64_t status = os64_page_node_set_checked(dom->state, node, was);
            if (status < 0) { d_page_error(ctx, status); return -1; }
        } else if (os64_page_node_checked(dom->state, node, &now) >= 0 && now != was) {
            result = fire(dom, ctx, node, "input", true, false);
            if (result >= 0) result = fire(dom, ctx, node, "change", true, false);
        }
        return result;
    }
    if (result == 0) return 0;
    if ((html_element(node, OS64_HTML_TAG_A) || html_element(node, OS64_HTML_TAG_AREA)) &&
        os64_html_attr(node, "href") != NULL) {
        d_navigate(dom, OS64_DOM_NAVIGATE_FOLLOW, node, NULL, NULL, 0);
        return 1;
    }
    bool button = html_element(node, OS64_HTML_TAG_BUTTON);
    bool input = html_element(node, OS64_HTML_TAG_INPUT);
    const os64_html_node_t *form = d_form_owner(dom, node);
    if (form == NULL) return 1;
    if ((button && (type_is(node, "submit", true) || !(type_is(node, "reset", false) || type_is(node, "button", false)))) ||
        (input && (type_is(node, "submit", false) || type_is(node, "image", false))))
        return submit(dom, ctx, form, node, true);
    if ((button || input) && type_is(node, "reset", false)) return reset(dom, ctx, form);
    return 1;
}

JSValue d_element_action(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, int action)
{
    int result = 1;
    switch (action) {
    case A_CLICK: result = click(dom, ctx, node); break;
    case A_FOCUS: notify(dom, node, OS64_DOM_ACTIVATE_FOCUS); break;
    case A_BLUR: notify(dom, node, OS64_DOM_ACTIVATE_BLUR); break;
    case A_SUBMIT: result = submit(dom, ctx, node, NULL, false); break;
    case A_REQUEST_SUBMIT: result = submit(dom, ctx, node, NULL, true); break;
    case A_RESET: result = reset(dom, ctx, node); break;
    default: return JS_ThrowTypeError(ctx, "Unknown element action");
    }
    return result < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue element_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                              int magic, JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "Expected an element");
    return d_element_action(dom, ctx, node, magic);
}

typedef struct { const char *name; int magic, argc; bool accessor, writable; } Member;

static JSValue object_with(os64_dom_t *dom, JSContext *ctx, DValue **slot, const Member *members,
                           size_t count)
{
    JSValue object = JS_NewObject(ctx);
    *slot = d_retain(dom, ctx, object);
    if (*slot == NULL) return JS_EXCEPTION;
    for (size_t i = 0; i < count; i++)
        if ((members[i].accessor ?
             d_accessor(dom, ctx, (*slot)->value, members[i].name, window_member, members[i].magic, members[i].writable) :
             d_method(dom, ctx, (*slot)->value, members[i].name, window_member, members[i].argc, members[i].magic)) < 0)
            return JS_EXCEPTION;
    return JS_UNDEFINED;
}

int d_window_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const Member location[] = {
        {"href", L_HREF, 0, true, true}, {"protocol", L_PROTOCOL, 0, true, true},
        {"host", L_HOST, 0, true, true}, {"hostname", L_HOSTNAME, 0, true, true},
        {"port", L_PORT, 0, true, true}, {"pathname", L_PATHNAME, 0, true, true},
        {"search", L_SEARCH, 0, true, true}, {"hash", L_HASH, 0, true, true},
        {"origin", L_ORIGIN, 0, true, false}, {"assign", L_ASSIGN, 1, false, false},
        {"replace", L_REPLACE, 1, false, false}, {"reload", L_RELOAD, 0, false, false},
        {"toString", L_TO_STRING, 0, false, false}
    };
    static const Member history[] = {
        {"back", H_BACK, 0, false, false}, {"forward", H_FORWARD, 0, false, false},
        {"go", H_GO, 1, false, false}, {"length", H_LENGTH, 0, true, false}
    };
    if (JS_IsException(object_with(dom, ctx, &dom->location, location,
                                   sizeof(location) / sizeof(location[0]))) ||
        JS_IsException(object_with(dom, ctx, &dom->history, history,
                                   sizeof(history) / sizeof(history[0]))))
        return -1;
    JSValueConst document_prototype = dom->prototypes[D_PROTO_DOCUMENT]->value;
    JSValueConst element_prototype = dom->prototypes[D_PROTO_ELEMENT]->value;
    JSValueConst form_prototype = dom->prototypes[D_PROTO_FORM]->value;
    if (d_accessor(dom, ctx, global, "location", location_property, 0, true) < 0 ||
        d_accessor(dom, ctx, document_prototype, "location", location_property, 0, true) < 0 ||
        JS_SetPropertyStr(ctx, global, "history", JS_DupValue(ctx, dom->history->value)) < 0 ||
        d_accessor(dom, ctx, global, "status", window_member, W_STATUS, true) < 0 ||
        d_accessor(dom, ctx, global, "defaultStatus", window_member, W_STATUS, true) < 0 ||
        d_method(dom, ctx, document_prototype, "write", window_member, 1, W_WRITE) < 0 ||
        d_method(dom, ctx, document_prototype, "writeln", window_member, 1, W_WRITELN) < 0 ||
        d_method(dom, ctx, document_prototype, "open", window_member, 0, W_OPEN) < 0 ||
        d_method(dom, ctx, document_prototype, "close", window_member, 0, W_CLOSE) < 0 ||
        d_method(dom, ctx, element_prototype, "click", element_method, 0, A_CLICK) < 0 ||
        d_method(dom, ctx, element_prototype, "focus", element_method, 0, A_FOCUS) < 0 ||
        d_method(dom, ctx, element_prototype, "blur", element_method, 0, A_BLUR) < 0 ||
        d_method(dom, ctx, form_prototype, "submit", element_method, 0, A_SUBMIT) < 0 ||
        d_method(dom, ctx, form_prototype, "requestSubmit", element_method, 0, A_REQUEST_SUBMIT) < 0)
        return -1;
    return d_method(dom, ctx, form_prototype, "reset", element_method, 0, A_RESET);
}

/* SCRIPT ELEMENTS. */

static bool ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static bool javascript_type(const char *value, size_t length, bool from_language)
{
    /* MIME Sniffing's JavaScript essence strings. language supplies a
     * virtual text/ prefix, so matching it needs no temporary allocation. */
    static const char *const essences[] = {
        "application/ecmascript", "application/javascript",
        "application/x-ecmascript", "application/x-javascript",
        "text/ecmascript", "text/javascript", "text/javascript1.0",
        "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
        "text/javascript1.4", "text/javascript1.5", "text/jscript",
        "text/livescript", "text/x-ecmascript", "text/x-javascript"
    };
    for (size_t i = 0; i < sizeof(essences) / sizeof(essences[0]); i++) {
        const char *essence = essences[i];
        if (from_language) {
            if (os64_memcmp(essence, "text/", 5) != 0) continue;
            essence += 5;
        }
        if (os64_strlen(essence) != length) continue;
        size_t j = 0;
        for (; j < length; j++) {
            unsigned char c = (unsigned char)value[j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (unsigned char)essence[j]) break;
        }
        if (j == length) return true;
    }
    return false;
}

os64_dom_script_kind_t os64_dom_script_kind(const os64_html_node_t *node)
{
    if (!html_element(node, OS64_HTML_TAG_SCRIPT)) return OS64_DOM_SCRIPT_NONE;
    const os64_html_attr_t *type = os64_html_attr(node, "type");
    const os64_html_attr_t *language = type == NULL ? os64_html_attr(node, "language") : NULL;
    const char *value = type != NULL ? type->value : language != NULL ? language->value : NULL;
    if (value == NULL || value[0] == '\0') return OS64_DOM_SCRIPT_CLASSIC;
    size_t length = os64_strlen(value);
    /* Only a present type is trimmed. Empty attributes default before
     * trimming, so whitespace-only type and padded language do not default. */
    if (type != NULL) {
        while (length != 0 && ascii_space((unsigned char)*value)) { value++; length--; }
        while (length != 0 && ascii_space((unsigned char)value[length - 1])) length--;
        if (length == 6) {
            const char *module = "module";
            size_t i = 0;
            while (i < 6 && (value[i] | 0x20) == module[i]) i++;
            if (i == 6) return OS64_DOM_SCRIPT_MODULE;
        }
    }
    return javascript_type(value, length, type == NULL) ? OS64_DOM_SCRIPT_CLASSIC : OS64_DOM_SCRIPT_NONE;
}

static size_t script_bucket(const os64_html_node_t *node)
{
    uintptr_t key = (uintptr_t)node;
    return ((key >> 4) ^ (key >> 12)) % (sizeof(((os64_dom_t *)0)->scripts) / sizeof(DScript *));
}

static bool script_marked(const os64_dom_t *dom, const os64_html_node_t *node)
{
    for (const DScript *mark = dom->scripts[script_bucket(node)]; mark != NULL; mark = mark->next)
        if (mark->node == node) return true;
    return false;
}

/* ALREADY STARTED, kept as a held node so the identity outlives a removal. */
int d_script_mark(os64_dom_t *dom, const os64_html_node_t *node)
{
    if (script_marked(dom, node)) return 0;
    DScript *mark = d_alloc(dom, sizeof(*mark));
    if (mark == NULL) return -1;
    size_t slot = script_bucket(node);
    mark->node = node;
    mark->next = dom->scripts[slot];
    dom->scripts[slot] = mark;
    os64_html_hold(dom->document, node);
    return 1;
}

int os64_dom_script_start(os64_dom_t *dom, const os64_html_node_t *node)
{
    if (dom == NULL || node == NULL || !os64_html_owns_node(dom->document, node)) return -1;
    return d_script_mark(dom, node);
}

static const os64_html_node_t *subtree_next(const os64_html_node_t *root, const os64_html_node_t *at)
{
    if (at->first_child != NULL) return at->first_child;
    while (at != root && at->next == NULL) at = at->parent;
    return at != root ? at->next : NULL;
}

/* A script parsed for innerHTML never runs: it is born started. */
int d_script_mark_tree(os64_dom_t *dom, const os64_html_node_t *root)
{
    for (const os64_html_node_t *at = root; at != NULL; at = subtree_next(root, at))
        if (html_element(at, OS64_HTML_TAG_SCRIPT) && d_script_mark(dom, at) < 0) return -1;
    return 0;
}

/* HTML's cloning steps copy ALREADY STARTED. clone copies a subtree's shape
 * exactly, so the two are walked in step. */
int d_script_copy_marks(os64_dom_t *dom, const os64_html_node_t *from, const os64_html_node_t *to)
{
    const os64_html_node_t *a = from, *b = to;
    while (a != NULL && b != NULL) {
        if (html_element(a, OS64_HTML_TAG_SCRIPT) && script_marked(dom, a) && d_script_mark(dom, b) < 0)
            return -1;
        const os64_html_node_t *next_a = subtree_next(from, a);
        const os64_html_node_t *next_b = subtree_next(to, b);
        if ((next_a == NULL) != (next_b == NULL)) break;
        a = next_a;
        b = next_b;
    }
    return 0;
}

/* The classic scripts a verb is about to move, in tree order: a fragment's
 * children, or the node's own subtree. Answers how many there are; at most
 * cap are stored. Whether one has started is asked once it is connected. */
size_t d_script_collect(const os64_html_node_t *node,
                        const os64_html_node_t **out, size_t cap)
{
    size_t count = 0;
    const os64_html_node_t *first = node->kind == OS64_HTML_FRAGMENT ? node->first_child : node;
    for (const os64_html_node_t *top = first; top != NULL;
         top = node->kind == OS64_HTML_FRAGMENT ? top->next : NULL)
        for (const os64_html_node_t *at = top; at != NULL; at = subtree_next(top, at))
            if (os64_dom_script_kind(at) == OS64_DOM_SCRIPT_CLASSIC) {
                if (count < cap) out[count] = at;
                count++;
            }
    return count;
}

/* HTML prepares a script element when it becomes connected: one that has
 * not started is marked started and handed to the host, which runs it. */
void d_script_connected(os64_dom_t *dom, const os64_html_node_t *const *scripts, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const os64_html_node_t *script = scripts[i];
        if (!d_connected(dom, script) || os64_dom_script_kind(script) != OS64_DOM_SCRIPT_CLASSIC) continue;
        if (d_script_mark(dom, script) == 1) dom->options.script_connected(dom->options.host_opaque, script);
    }
}

void d_window_free(os64_dom_t *dom)
{
    d_navigate(dom, OS64_DOM_NAVIGATE_NONE, NULL, NULL, NULL, 0);
    for (size_t i = 0; i < sizeof(dom->scripts) / sizeof(dom->scripts[0]); i++)
        while (dom->scripts[i] != NULL) {
            DScript *mark = dom->scripts[i];
            dom->scripts[i] = mark->next;
            os64_html_release(dom->document, mark->node);
            d_free(dom, mark);
        }
    d_free(dom, dom->url);
    dom->url = NULL;
}
