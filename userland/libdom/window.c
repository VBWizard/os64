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
    /* The last ask of a task wins, as the last location= does in a browser;
     * but with the Modern Web switch on, history.go()s add up, as HTML
     * queues each traversal after the one before it: go(-2) then go(-1) is
     * three back. */
    if (kind == OS64_DOM_NAVIGATE_HISTORY && dom->navigation.kind == OS64_DOM_NAVIGATE_HISTORY &&
        dom->options.modern) {
        int64_t sum = (int64_t)dom->navigation.delta + delta;
        dom->navigation.delta = sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : (int32_t)sum;
        return;
    }
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

/* One rule for every address on a page: libpage resolves a script's exactly
 * as it resolves a link's, so the two cannot drift apart. */
bool os64_dom_resolve(const os64_html_document_t *document, const char *document_url,
                      const char *reference, char *out, size_t cap)
{
    return os64_page_resolve_in(document, document_url, reference, out, cap);
}

/* A reference resolved against the page, as a link's href is. An absolute
 * reference needs no base, so about:blank still reaches one. */
static bool resolve(os64_dom_t *dom, const char *reference, char *out, size_t cap)
{
    return os64_dom_resolve(dom->document, page_url(dom), reference, out, cap);
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
#define WRITE_UNOPENED "document.write into a document with no window would open it, which this browser does not do"

/* A document of its own (implementation.c) as `this`: it has no window,
 * so no parser of the page's and no location. */
static bool windowless(os64_dom_t *dom, JSValueConst self)
{
    DValue *entry = JS_GetOpaque(self, dom->node_class);
    return entry != NULL && entry->node != NULL && entry->node->kind == OS64_HTML_DOCUMENT &&
           !d_window_document(dom, entry->node);
}

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
    if (part == L_ORIGIN) {
        /* d_origin's, the one serialization: storage is kept by it. */
        char origin[WAY_ORIGIN_TEXT_MAX];
        return JS_NewString(ctx, d_origin(url, origin, sizeof(origin)) ? origin : "null");
    }
    os64_url_t parsed;
    if (os64_url_parse(url, &parsed) != OS64_URL_OK) {
        char scheme[OS64_URL_SCHEME_MAX + 1];
        if (part == L_PROTOCOL && os64_url_scheme_of(url, scheme, sizeof(scheme) - 1)) {
            os64_strcopy(scheme + os64_strlen(scheme), 2, ":");
            return JS_NewString(ctx, scheme);
        }
        return JS_NewString(ctx, "");
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
    case L_HOST:
        if (parsed.port != 0)
            os64_snprintf(text, sizeof(text), "%s:%u", parsed.host, (unsigned)parsed.port);
        else os64_snprintf(text, sizeof(text), "%s", parsed.host);
        break;
    case L_PATHNAME: return JS_NewStringLen(ctx, parsed.path, (size_t)(query - parsed.path));
    case L_SEARCH: return JS_NewString(ctx, query[0] == '?' && query[1] != '\0' ? query : "");
    default: return JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, text);
}

static size_t before_fragment(const char *url)
{
    size_t n = 0;
    while (url[n] != '\0' && url[n] != '#') n++;
    return n;
}

static int fragment_navigation(os64_dom_t *dom, JSContext *ctx, const char *url, bool replace);

/* `name` as the URL parser's fragment state writes it: the fragment
 * percent-encode set (C0 controls, space, `"`, `<`, `>`, backtick, and
 * every byte past ASCII) as %XX. False when it does not fit `cap`. */
static bool fragment_encoded(const char *name, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *s = (const unsigned char *)name; *s != '\0'; s++) {
        bool encode = *s <= 0x20 || *s >= 0x7F || *s == '"' || *s == '<' || *s == '>' || *s == '`';
        if (n + (encode ? 3 : 1) >= cap) return false;
        if (!encode) { out[n++] = (char)*s; continue; }
        out[n++] = '%';
        out[n++] = hex[*s >> 4];
        out[n++] = hex[*s & 15];
    }
    out[n] = '\0';
    return true;
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
         * replaced, which the host reaches without a request (HTML § The
         * Location interface, the hash setter): one leading `#` taken off,
         * the rest parsed in the fragment state, and nothing at all when
         * that is the fragment the address already has (none counting as
         * the empty one). With the Modern Web switch off the name is
         * written as given and always asked for, as before the arc. */
        const char *base = page_url(dom);
        size_t keep = before_fragment(base);
        const char *name = text.data[0] == '#' ? text.data + 1 : text.data;
        char encoded[OS64_DOM_URL_MAX];
        if (dom->options.modern) {
            resolved = fragment_encoded(name, encoded, sizeof(encoded));
            const char *has = base[keep] == '#' ? base + keep + 1 : "";
            if (resolved && os64_strcmp(encoded, has) == 0) {
                d_string_free(dom, &text);
                return JS_UNDEFINED;
            }
        } else {
            resolved = os64_strlen(name) < sizeof(encoded);
            if (resolved) os64_strcopy(encoded, sizeof(encoded), name);
        }
        resolved = resolved && keep + 1 + os64_strlen(encoded) < sizeof(url);
        if (resolved) os64_snprintf(url, sizeof(url), "%.*s#%s", (int)keep, base, encoded);
    } else resolved = resolve(dom, text.data, url, sizeof(url));
    d_string_free(dom, &text);
    if (!resolved) return d_error(ctx, "SyntaxError", "The address cannot be resolved against this page");
    /* An address that differs from the document's only in a fragment it has
     * is HTML's navigation to a fragment, which happens now, inside the
     * script, when the host takes it (fragment_navigation): the same
     * address again replaces the entry rather than adding one. */
    const char *here = page_url(dom);
    size_t keep = before_fragment(url);
    if (url[keep] == '#' && keep == before_fragment(here) && os64_memcmp(url, here, keep) == 0) {
        bool same = os64_strcmp(url, here) == 0;
        int made = fragment_navigation(dom, ctx, url, kind == OS64_DOM_NAVIGATE_REPLACE || same);
        if (made != 0) return made < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
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
    case H_LENGTH: {
        /* The session's entries are the host's to count once the arc's
         * history is on; a window with no host is one entry. */
        uint32_t entries = dom->options.modern && dom->history_host.length != NULL
            ? dom->history_host.length(dom->history_host.opaque) : 1;
        return JS_NewUint32(ctx, entries > 0 ? entries : 1);
    }
    case W_STATUS:
        /* The status line is the browser's; the old web assigns window.status
         * on every mouseover and expects no complaint. */
        return set ? JS_UNDEFINED : JS_NewString(ctx, "");
    case W_WRITE: case W_WRITELN:
        if (windowless(dom, self)) return d_error(ctx, "InvalidStateError", WRITE_UNOPENED);
        return document_write(dom, ctx, argc, argv, magic == W_WRITELN);
    case W_OPEN:
        /* open() over the parser that is running answers the document and
         * changes nothing; any other open() would make a new document. An
         * empty write asks the host whether there is an insertion point. */
        if (windowless(dom, self)) return d_error(ctx, "InvalidStateError", WRITE_UNOPENED);
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
 * window and on document alike. A document with no window has none: it
 * reads null, and assigning to it is assigning to null's href. */
static JSValue location_property(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                 int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (windowless(dom, self))
        return magic & D_SET ? JS_ThrowTypeError(ctx, "This document has no location") : JS_NULL;
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

static const os64_html_node_t *tree_root(const os64_html_node_t *node)
{
    while (node->parent != NULL) node = node->parent;
    return node;
}

bool d_connected(const os64_html_node_t *node)
{
    return tree_root(node)->kind == OS64_HTML_DOCUMENT;
}

bool d_in_window_tree(const os64_dom_t *dom, const os64_html_node_t *node)
{
    return tree_root(node) == dom->document->document;
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
 * names in the control's own document's tree (any document's, as libpage's
 * node_owner reads it), else the nearest form ancestor. libhtml clears a
 * record the moment it stops being true, so a record found is believed. */
const os64_html_node_t *d_form_owner(const os64_html_node_t *node)
{
    if (!form_associated(node)) return NULL;
    if (node->form_owner != NULL) return node->form_owner;
    const os64_html_attr_t *named = os64_html_attr(node, "form");
    if (named != NULL) {
        if (!d_connected(node)) return NULL;
        const os64_html_node_t *root = tree_root(node);
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

/* What the host performs for an element: focus and blur, which move the
 * window's focus, and a form's reset, which the host carries out on the
 * page's model and widgets. Only for an element of the window's document:
 * a document with no window has nothing on screen to focus, and no model,
 * so a form's reset there fires its event and resets no control (DEBTS.md
 * § Classic browser scripting). */
static void notify(os64_dom_t *dom, const os64_html_node_t *node, os64_dom_activation_t what)
{
    if (dom->options.activate != NULL && d_window_document(dom, node))
        dom->options.activate(dom->options.host_opaque, node, what);
}

enum { A_CLICK, A_FOCUS, A_BLUR, A_SUBMIT, A_REQUEST_SUBMIT, A_RESET };

/* A form's submission: HTML fires submit at the form unless the script
 * called submit() itself, and a submit nobody cancelled becomes the ask. A
 * form in a document with no window cannot navigate, and HTML submits
 * nothing from it, not even the event. */
static int submit(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *form,
                  const os64_html_node_t *submitter, bool event)
{
    if (!d_window_document(dom, form)) return 1;
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
static int click_activate(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
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
        /* A link in a document with no window cannot navigate. */
        if (d_window_document(dom, node)) d_navigate(dom, OS64_DOM_NAVIGATE_FOLLOW, node, NULL, NULL, 0);
        return 1;
    }
    bool button = html_element(node, OS64_HTML_TAG_BUTTON);
    bool input = html_element(node, OS64_HTML_TAG_INPUT);
    const os64_html_node_t *form = d_form_owner(node);
    if (form == NULL) return 1;
    if ((button && (type_is(node, "submit", true) || !(type_is(node, "reset", false) || type_is(node, "button", false)))) ||
        (input && (type_is(node, "submit", false) || type_is(node, "image", false))))
        return submit(dom, ctx, form, node, true);
    if ((button || input) && type_is(node, "reset", false)) return reset(dom, ctx, form);
    return 1;
}

struct DClicking {
    const os64_html_node_t *node;
    const struct DClicking *next;
};

/* HTML's click-in-progress flag: an onclick that clicks its own element
 * returns at once instead of recursing until the engine's stack gives out.
 * A different element may still be clicked from inside the listener. The
 * flag is a link in a chain on the C stack, so every exit clears it. */
static int click(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    if (disabled(node)) return 1;
    for (const struct DClicking *at = dom->clicking; at != NULL; at = at->next)
        if (at->node == node) return 1;
    struct DClicking self = {node, dom->clicking};
    dom->clicking = &self;
    int result = click_activate(dom, ctx, node);
    dom->clicking = self.next;
    return result;
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
    if (d_method(dom, ctx, form_prototype, "reset", element_method, 0, A_RESET) < 0) return -1;
    return d_classic_install(dom, ctx, global);
}

/* HISTORY (HTML § The History interface; MODERN_WEB.md § Slice 1b): the
 * Modern Web table's row. A state update (pushState, replaceState) changes
 * the address and the state and fires nothing; a fragment navigation and
 * a traversal to an entry of this document are the host's, which fires
 * popstate, then hashchange when the fragment moved, through
 * os64_dom_history_event. */

void os64_dom_set_history(os64_dom_t *dom, const os64_dom_history_t *history)
{
    if (dom == NULL || dom->closed) return;
    if (history != NULL) dom->history_host = *history;
    else os64_memset(&dom->history_host, 0, sizeof(dom->history_host));
}

bool os64_dom_history_manual(const os64_dom_t *dom)
{
    return dom != NULL && dom->scroll_manual;
}

/* A state's JSON, as the binding writes it: what JSON can carry of a
 * structured clone, and nothing else (dom.h § HISTORY). */
typedef struct {
    os64_dom_t *dom;
    char *text;
    size_t used, cap;
    JSValueConst path[64];      // the objects the walk is inside, for a cycle
} DJson;

static int json_put(DJson *j, JSContext *ctx, const char *s, size_t n)
{
    if (n > OS64_DOM_STATE_MAX - j->used) {
        d_error(ctx, "QuotaExceededError", "A history state may be at most 64 KiB of JSON");
        return -1;
    }
    if (j->used + n + 1 > j->cap) {
        size_t cap = j->cap > 0 ? j->cap : 256;
        while (cap < j->used + n + 1) cap *= 2;
        char *grown = d_alloc(j->dom, cap);
        if (grown == NULL) {
            d_error(ctx, "QuotaExceededError", "No room for a history state");
            return -1;
        }
        if (j->used > 0) os64_memcpy(grown, j->text, j->used);
        d_free(j->dom, j->text);
        j->text = grown;
        j->cap = cap;
    }
    os64_memcpy(j->text + j->used, s, n);
    j->used += n;
    j->text[j->used] = '\0';
    return 0;
}

static int json_puts(DJson *j, JSContext *ctx, const char *s)
{
    return json_put(j, ctx, s, os64_strlen(s));
}

/* A value the engine writes for us: a number's or a string's JSON, which
 * the engine's own JSON.stringify writes for a primitive without running
 * any of the page's code. */
static int json_engine(DJson *j, JSContext *ctx, JSValue text)
{
    if (JS_IsException(text)) return -1;
    size_t length;
    const char *s = JS_ToCStringLen(ctx, &length, text);
    JS_FreeValue(ctx, text);
    if (s == NULL) return -1;
    int result = json_put(j, ctx, s, length);
    JS_FreeCString(ctx, s);
    return result;
}

#define CLONE_REFUSED(what) d_error(ctx, "DataCloneError", what " cannot be kept in a history state")

/* One value; 1 for an `undefined` an object leaves out, as JSON does. */
static int json_value(DJson *j, JSContext *ctx, JSValueConst v, int depth, bool in_array)
{
    if (JS_IsUndefined(v)) return in_array ? json_puts(j, ctx, "null") : 1;
    if (JS_IsNull(v)) return json_puts(j, ctx, "null");
    if (JS_IsBool(v)) return json_puts(j, ctx, JS_ToBool(ctx, v) ? "true" : "false");
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0) return -1;
        union { double d; uint64_t bits; } sign = {d};
        if (!(d == d) || d > 1.7976931348623157e308 || d < -1.7976931348623157e308 ||
            (d == 0 && (sign.bits >> 63) != 0)) {
            CLONE_REFUSED("NaN, an infinity or -0");
            return -1;
        }
        return json_engine(j, ctx, JS_ToString(ctx, v));
    }
    if (JS_IsString(v)) return json_engine(j, ctx, JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED));
    if (!JS_IsObject(v)) {
        CLONE_REFUSED("A symbol or a BigInt");
        return -1;
    }
    JSClassID id = JS_GetClassID(v);
    bool array = id == j->dom->array_class;
    if (!array && id != j->dom->plain_class) {
        CLONE_REFUSED("Only plain objects and arrays; this object");
        return -1;
    }
    if (depth >= (int)(sizeof(j->path) / sizeof(j->path[0]))) {
        CLONE_REFUSED("An object nested this deep");
        return -1;
    }
    for (int i = 0; i < depth; i++)
        if (JS_VALUE_GET_PTR(j->path[i]) == JS_VALUE_GET_PTR(v)) {
            CLONE_REFUSED("An object that holds itself");
            return -1;
        }
    j->path[depth] = v;
    JSPropertyEnum *props = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &count, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return -1;
    int result = json_puts(j, ctx, array ? "[" : "{");
    if (array && result == 0) {
        /* A clone of an array keeps its holes and its named properties,
         * which JSON cannot carry (it writes a hole as null and leaves a
         * name out): only an array that is its own properties 0 to
         * length - 1 and nothing else is kept. */
        uint32_t length = 0;
        JSValue n = JS_GetPropertyStr(ctx, v, "length");
        result = JS_IsException(n) || JS_ToUint32(ctx, &length, n) < 0 ? -1 : 0;
        JS_FreeValue(ctx, n);
        for (uint32_t i = 0; result == 0 && i < count; i++) {
            JSAtom index = JS_NewAtomUInt32(ctx, i);
            bool listed = index == props[i].atom;
            JS_FreeAtom(ctx, index);
            if (!listed) result = -2;
        }
        if (result == 0 && count != length) result = -2;
        if (result == -2) {
            CLONE_REFUSED("An array with holes or named properties");
            result = -1;
        }
        for (uint32_t i = 0; result == 0 && i < count; i++) {
            if (i > 0) result = json_puts(j, ctx, ",");
            JSValue item = result == 0 ? JS_GetPropertyUint32(ctx, v, i) : JS_UNDEFINED;
            if (JS_IsException(item)) result = -1;
            if (result == 0) result = json_value(j, ctx, item, depth + 1, true);
            JS_FreeValue(ctx, item);
        }
    } else {
        bool first = true;
        for (uint32_t i = 0; result == 0 && i < count; i++) {
            JSValue item = JS_GetProperty(ctx, v, props[i].atom);
            if (JS_IsException(item)) { result = -1; break; }
            if (JS_IsUndefined(item)) continue;
            JSValue key = JS_AtomToString(ctx, props[i].atom);
            result = JS_IsException(key) ? -1 : 0;
            if (result == 0 && !first) result = json_puts(j, ctx, ",");
            if (result == 0) result = json_engine(j, ctx, JS_JSONStringify(ctx, key, JS_UNDEFINED, JS_UNDEFINED));
            JS_FreeValue(ctx, key);
            if (result == 0) result = json_puts(j, ctx, ":");
            if (result == 0) result = json_value(j, ctx, item, depth + 1, false);
            JS_FreeValue(ctx, item);
            first = false;
        }
    }
    JS_FreePropertyEnum(ctx, props, count);
    if (result == 0) result = json_puts(j, ctx, array ? "]" : "}");
    return result < 0 ? -1 : 0;
}

/* HTML's "can have its URL rewritten": the same scheme, host and port
 * (the user and password, which os64's parser refuses, by the text), and
 * outside http and https the same path too; only the query and the
 * fragment may move then. */
static uint16_t port_of(const os64_url_t *u)
{
    if (u->port != 0) return u->port;
    return os64_strcmp(u->scheme, "https") == 0 ? 443 : os64_strcmp(u->scheme, "http") == 0 ? 80 : 0;
}

static size_t before_query(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0' && s[n] != '?' && s[n] != '#') n++;
    return n;
}

static bool rewritable(const char *document, const char *target)
{
    os64_url_t a, b;
    bool pa = os64_url_parse(document, &a) == OS64_URL_OK;
    bool pb = os64_url_parse(target, &b) == OS64_URL_OK;
    if (pa != pb) return false;
    if (pa) {
        if (os64_strcmp(a.scheme, b.scheme) != 0 || os64_strcmp(a.host, b.host) != 0 ||
            port_of(&a) != port_of(&b))
            return false;
        if (os64_strcmp(b.scheme, "http") == 0 || os64_strcmp(b.scheme, "https") == 0) return true;
        size_t n = before_query(a.path);
        return n == before_query(b.path) && os64_memcmp(a.path, b.path, n) == 0;
    }
    size_t n = before_query(document);
    return n == before_query(target) && os64_memcmp(document, target, n) == 0;
}

static char *copy_text(os64_dom_t *dom, const char *text, size_t length)
{
    char *copy = d_alloc(dom, length + 1);
    if (copy != NULL) {
        if (length > 0) os64_memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

/* history.state's value: the current entry's JSON read once (a fresh
 * value, as StructuredDeserialize makes) and the same object every read
 * after, until the entry or its state changes. JS_EXCEPTION when it could
 * not be read; a new reference otherwise. */
static JSValue state_value(os64_dom_t *dom, JSContext *ctx)
{
    if (!dom->entry_state_read) {
        JSValue value = dom->entry_state != NULL
            ? JS_ParseJSON(ctx, dom->entry_state, dom->entry_state_length, "<history state>") : JS_NULL;
        if (JS_IsException(value)) return value;
        if (dom->entry_state_value == NULL) {
            if ((dom->entry_state_value = d_retain(dom, ctx, value)) == NULL) return JS_EXCEPTION;
        } else {
            JS_FreeValue(ctx, dom->entry_state_value->value);
            dom->entry_state_value->value = value;
        }
        dom->entry_state_read = true;
    }
    return JS_DupValue(ctx, dom->entry_state_value->value);
}

static JSValue update_state(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, bool push)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "%s needs a state and a title", push ? "pushState" : "replaceState");
    /* WebIDL converts every argument before a step runs: the unused title
     * to a string, the address to one unless it is null or undefined. */
    DString unused = {0}, url = {0};
    if (!d_string(dom, ctx, argv[1], &unused)) return JS_EXCEPTION;
    d_string_free(dom, &unused);
    bool has_url = argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]);
    if (has_url && !d_string(dom, ctx, argv[2], &url)) return JS_EXCEPTION;
    /* An empty address is none (HTML, after whatwg/html#9343): the entry
     * keeps the document's, its fragment included. */
    if (has_url && url.length == 0) {
        d_string_free(dom, &url);
        has_url = false;
    }
    DJson json = {.dom = dom};
    int written = JS_IsUndefined(argv[0]) ? json_puts(&json, ctx, "null")
                                          : json_value(&json, ctx, argv[0], 0, false);
    char target[OS64_DOM_URL_MAX];
    JSValue result = JS_UNDEFINED;
    if (written < 0) {
        result = JS_EXCEPTION;
    } else if (has_url && (!resolve(dom, url.data, target, sizeof(target)) || !rewritable(page_url(dom), target))) {
        result = d_error(ctx, "SecurityError", "A history entry's address must be this document's own origin");
    } else {
        if (!has_url) os64_strcopy(target, sizeof(target), page_url(dom));
        bool null_state = os64_strcmp(json.text, "null") == 0;
        char *address = copy_text(dom, target, os64_strlen(target));
        JSValue value = address != NULL ? JS_ParseJSON(ctx, json.text, json.used, "<history state>")
                                        : JS_EXCEPTION;
        if (address == NULL) d_error(ctx, "QuotaExceededError", "No room for a history entry's address");
        if (!JS_IsException(value) && dom->entry_state_value == NULL) {
            dom->entry_state_value = d_retain(dom, ctx, JS_NULL);
            if (dom->entry_state_value == NULL) {
                JS_FreeValue(ctx, value);
                value = JS_EXCEPTION;
            }
        }
        if (!JS_IsException(value) && dom->history_host.update != NULL &&
            !dom->history_host.update(dom->history_host.opaque, target, null_state ? NULL : json.text,
                                      null_state ? 0 : json.used, push)) {
            JS_FreeValue(ctx, value);
            value = d_error(ctx, "QuotaExceededError", "The browser could not keep another history entry");
        }
        if (JS_IsException(value)) {
            d_free(dom, address);
            result = JS_EXCEPTION;
        } else {
            d_free(dom, dom->url);
            dom->url = address;
            d_free(dom, dom->entry_state);
            dom->entry_state = null_state ? NULL : json.text;
            dom->entry_state_length = null_state ? 0 : json.used;
            if (!null_state) json.text = NULL;
            JS_FreeValue(ctx, dom->entry_state_value->value);
            dom->entry_state_value->value = value;
            dom->entry_state_read = true;
            dom->entry_name++;
        }
    }
    d_free(dom, json.text);
    d_string_free(dom, &url);
    return result;
}

static JSValue history_event(os64_dom_t *dom, JSContext *ctx, void *arg);

/* A fragment navigation a script made (HTML § Navigate to a fragment): the
 * host takes the entry, the document's address and its null state follow,
 * and then — inside the script that asked, before it goes on — `popstate`,
 * because the navigation's "update document for history step application"
 * fires it there and then. The hashchange it owes is the host's to queue
 * (os64_dom_history_t.fragment). 1 when it was made, its popstate
 * dispatched or, when the binding had no room for it, reported, since the
 * navigation the script asked for stands; 0, with nothing changed and
 * nothing fired, when the host could not take it; -1 when it was made and
 * the runtime failed under a listener, with the exception pending, so the
 * script ends as it would have in dispatchEvent. */
static int fragment_navigation(os64_dom_t *dom, JSContext *ctx, const char *url, bool replace)
{
    if (!dom->options.modern || dom->history_host.fragment == NULL) return 0;
    char *address = copy_text(dom, url, os64_strlen(url));
    if (address == NULL) return 0;
    if (!dom->history_host.fragment(dom->history_host.opaque, url, replace)) {
        d_free(dom, address);
        return 0;
    }
    d_free(dom, dom->url);
    dom->url = address;
    d_free(dom, dom->entry_state);
    dom->entry_state = NULL;
    dom->entry_state_length = 0;
    dom->entry_state_read = false;
    dom->entry_name++;
    if (dom->popstate_prototype == NULL || !os64_dom_listens(dom, "popstate")) return 1;
    os64_dom_history_event_t popstate = {.entry = dom->entry_name};
    JSValue event = history_event(dom, ctx, &popstate);
    int dispatched = JS_IsException(event) ? -1 : d_dispatch(dom, ctx, event, NULL);
    JS_FreeValue(ctx, event);
    if (dispatched >= 0) return 1;
    os64_js_outcome_t probe;
    if (os64_js_context(dom->runtime, OS64_JS_ABI_ID, &probe) == NULL) return -1;
    JSValue error = JS_GetException(ctx);
    d_report(dom, ctx, error);
    JS_FreeValue(ctx, error);
    return 1;
}

bool os64_dom_history_moved(os64_dom_t *dom, const char *url, const char *state, size_t length)
{
    if (dom == NULL || dom->closed || url == NULL || (state == NULL && length != 0)) return false;
    char *address = copy_text(dom, url, os64_strlen(url));
    char *kept = state != NULL ? copy_text(dom, state, length) : NULL;
    if (address == NULL || (state != NULL && kept == NULL)) {
        d_free(dom, address);
        d_free(dom, kept);
        return false;
    }
    d_free(dom, dom->url);
    dom->url = address;
    d_free(dom, dom->entry_state);
    dom->entry_state = kept;
    dom->entry_state_length = kept != NULL ? length : 0;
    dom->entry_state_read = false;
    dom->entry_name++;
    return true;
}

uint64_t os64_dom_history_entry(const os64_dom_t *dom)
{
    return dom != NULL ? dom->entry_name : 0;
}

/* A popstate's state: history.state itself while the entry it was owed
 * for is still the current one (the same object, as HTML's event and
 * history.state share one deserialization), else that entry's state read
 * from the JSON it carried. */
static JSValue owed_state(os64_dom_t *dom, JSContext *ctx, const os64_dom_history_event_t *what)
{
    if (what->entry == dom->entry_name) return state_value(dom, ctx);
    if (what->state == NULL) return JS_NULL;
    return JS_ParseJSON(ctx, what->state, what->length, "<history state>");
}

static JSValue history_event(os64_dom_t *dom, JSContext *ctx, void *arg)
{
    const os64_dom_history_event_t *what = arg;
    JSValueConst prototype = what->hashchange ? dom->hashchange_prototype->value : dom->popstate_prototype->value;
    JSValue event = d_event_new_proto(dom, ctx, prototype, what->hashchange ? "hashchange" : "popstate",
                                      false, false, true);
    if (JS_IsException(event)) return event;
    int defined;
    if (what->hashchange) {
        JSValue old_url = JS_NewString(ctx, what->old_url);
        defined = JS_IsException(old_url) ? -1
            : JS_DefinePropertyValueStr(ctx, event, "oldURL", old_url, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JSValue new_url = defined < 0 ? JS_EXCEPTION : JS_NewString(ctx, what->new_url);
        if (defined >= 0)
            defined = JS_IsException(new_url) ? -1
                : JS_DefinePropertyValueStr(ctx, event, "newURL", new_url, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
    } else {
        JSValue state = owed_state(dom, ctx, what);
        defined = JS_IsException(state) ? -1
            : JS_DefinePropertyValueStr(ctx, event, "state", state, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        if (defined >= 0)
            defined = JS_DefinePropertyValueStr(ctx, event, "hasUAVisualTransition", JS_FALSE,
                                                JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
    }
    if (defined < 0) {
        JS_FreeValue(ctx, event);
        return JS_EXCEPTION;
    }
    return event;
}

os64_js_status_t os64_dom_history_event(os64_dom_t *dom, const os64_dom_history_event_t *event,
                                        os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (dom == NULL || event == NULL ||
        (event->hashchange ? event->old_url == NULL || event->new_url == NULL
                           : event->state == NULL && event->length != 0)) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return outcome->status;
    }
    if (dom->closed) {
        outcome->status = OS64_JS_FAILED_RUNTIME;
        return outcome->status;
    }
    const char *type = event->hashchange ? "hashchange" : "popstate";
    if (!dom->options.modern || dom->popstate_prototype == NULL || !os64_dom_listens(dom, type))
        return OS64_JS_OK;
    return d_host_dispatch(dom, NULL, NULL, type, history_event, (void *)event, NULL, outcome);
}

enum { HS_PUSH, HS_REPLACE, HS_STATE, HS_RESTORATION };

static JSValue history_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                              int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    switch (magic) {
    case HS_PUSH: case HS_REPLACE: return update_state(dom, ctx, argc, argv, magic == HS_PUSH);
    case HS_STATE: return state_value(dom, ctx);
    case HS_RESTORATION: {
        if (!set) return JS_NewString(ctx, dom->scroll_manual ? "manual" : "auto");
        /* An enum attribute ignores a word it does not have (WebIDL). */
        const char *word = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (word == NULL) return JS_EXCEPTION;
        if (os64_strcmp(word, "manual") == 0) dom->scroll_manual = true;
        else if (os64_strcmp(word, "auto") == 0) dom->scroll_manual = false;
        JS_FreeCString(ctx, word);
        return JS_UNDEFINED;
    }
    default: return JS_ThrowTypeError(ctx, "Unknown History member");
    }
}

/* `new PopStateEvent(type, {state, hasUAVisualTransition, ...})` and
 * `new HashChangeEvent(type, {oldURL, newURL, ...})`: an Event, and the
 * members of its own dictionary in WebIDL's order. */
static JSValue history_event_construct(JSContext *ctx, JSValueConst new_target, int argc,
                                       JSValueConst *argv, bool hashchange)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValueConst own = hashchange ? dom->hashchange_prototype->value : dom->popstate_prototype->value;
    JSValue event = d_event_construct(dom, ctx, argc, argv, JS_IsObject(prototype) ? prototype : own);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(event)) return event;
    static const char *const kHash[] = {"newURL", "oldURL"};
    static const char *const kPop[] = {"hasUAVisualTransition", "state"};
    const char *const *names = hashchange ? kHash : kPop;
    for (int i = 0; i < 2; i++) {
        JSValue value = argc > 1 && JS_IsObject(argv[1]) ? JS_GetPropertyStr(ctx, argv[1], names[i]) : JS_UNDEFINED;
        if (!JS_IsException(value)) {
            if (hashchange) {
                JSValue text = JS_IsUndefined(value) ? JS_NewString(ctx, "") : JS_ToString(ctx, value);
                JS_FreeValue(ctx, value);
                value = text;
            } else if (i == 0) {
                int flag = JS_ToBool(ctx, value);
                JS_FreeValue(ctx, value);
                value = flag < 0 ? JS_EXCEPTION : JS_NewBool(ctx, flag != 0);
            } else if (JS_IsUndefined(value)) {
                value = JS_NULL;
            }
        }
        if (JS_IsException(value) ||
            JS_DefinePropertyValueStr(ctx, event, names[i], value, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0) {
            JS_FreeValue(ctx, event);
            return JS_EXCEPTION;
        }
    }
    return event;
}

static JSValue popstate_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return history_event_construct(ctx, new_target, argc, argv, false);
}

static JSValue hashchange_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return history_event_construct(ctx, new_target, argc, argv, true);
}

int d_history_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue plain = JS_NewObject(ctx), array = JS_NewArray(ctx);
    if (JS_IsException(plain) || JS_IsException(array)) {
        JS_FreeValue(ctx, plain);
        JS_FreeValue(ctx, array);
        return -1;
    }
    dom->plain_class = JS_GetClassID(plain);
    dom->array_class = JS_GetClassID(array);
    JS_FreeValue(ctx, plain);
    JS_FreeValue(ctx, array);
    JSValueConst history = dom->history->value;
    if (d_method_named(dom, ctx, history, "pushState", history_member, 2, HS_PUSH) < 0 ||
        d_method_named(dom, ctx, history, "replaceState", history_member, 2, HS_REPLACE) < 0 ||
        d_accessor(dom, ctx, history, "state", history_member, HS_STATE, false) < 0 ||
        d_accessor(dom, ctx, history, "scrollRestoration", history_member, HS_RESTORATION, true) < 0 ||
        d_event_handler(dom, ctx, global, D_EVENT_POPSTATE) < 0 ||
        d_event_handler(dom, ctx, global, D_EVENT_HASHCHANGE) < 0)
        return -1;
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    JSValue event = JS_GetPropertyStr(ctx, global, "Event");
    JSValue popstate = JS_NewObjectProto(ctx, event_prototype);
    dom->popstate_prototype = d_retain(dom, ctx, popstate);
    JSValue hashchange = dom->popstate_prototype != NULL ? JS_NewObjectProto(ctx, event_prototype) : JS_EXCEPTION;
    dom->hashchange_prototype = dom->popstate_prototype != NULL ? d_retain(dom, ctx, hashchange) : NULL;
    int result = dom->hashchange_prototype == NULL || JS_IsException(event) ? -1 : 0;
    static const char *const names[] = {"PopStateEvent", "HashChangeEvent"};
    JSCFunction *const constructs[] = {popstate_construct, hashchange_construct};
    for (int i = 0; result == 0 && i < 2; i++) {
        JSValueConst own = i == 0 ? dom->popstate_prototype->value : dom->hashchange_prototype->value;
        JSValue f = d_interface_new(ctx, names[i], 1, constructs[i], own, event);
        if (JS_IsException(f) ||
            JS_DefinePropertyValueStr(ctx, global, names[i], f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            result = -1;
    }
    JS_FreeValue(ctx, event_prototype);
    JS_FreeValue(ctx, event);
    return result;
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
 * not started is marked started and handed to the host, which runs it.
 * Connected to a document with no window, it is marked started all the
 * same and runs nowhere, then or after it is moved into the page (HTML's
 * "prepare the script element": scripting is disabled for it), unless the
 * binding has no memory left to mark it with. */
void d_script_connected(os64_dom_t *dom, const os64_html_node_t *const *scripts, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const os64_html_node_t *script = scripts[i];
        if (os64_dom_script_kind(script) != OS64_DOM_SCRIPT_CLASSIC) continue;
        if (d_in_window_tree(dom, script)) {
            if (d_script_mark(dom, script) == 1) dom->options.script_connected(dom->options.host_opaque, script);
        } else if (d_connected(script)) {
            (void)d_script_mark(dom, script);
        }
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
    d_free(dom, dom->entry_state);
    dom->entry_state = NULL;
}
