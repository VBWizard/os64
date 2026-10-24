#include "internal.h"
#include "os64/date.h"
#include "os64/fmt.h"
#include "way/origin.h"

/* THE DOCUMENT'S STATE (HTML § 3.1.3 and the specifications each member
 * names): P1-Da's members of Document (WINDOW_DOCUMENT.md § P1-Da), a row
 * of the Modern Web table, and document.body's setter, every page's. Each
 * member reads state the binding or its host already keeps, and answers
 * for the document it is asked of: the window's document, the one yonder
 * shows, or a document of its own (implementation.c), which HTML gives no
 * browsing context. For the window's document:
 *
 *   - readyState is its readiness, moved by the host's own dispatches and
 *     nothing else (os64_dom_readiness_after): "loading" until
 *     DOMContentLoaded at the document makes it "interactive", and load at
 *     window "complete", readystatechange fired at the document just before
 *     each (d_readiness_moved). The host says where it stood when the
 *     binding was made (options.readiness);
 *   - visibilityState is "visible" and hidden false: the window shows the
 *     page, and nothing here learns of a desktop minimizing it, so
 *     visibilitychange never fires (DEBTS.md);
 *   - prerendering is false and wasDiscarded false: yonder neither
 *     prerenders a page nor discards one to bring back later, so
 *     prerenderingchange never fires;
 *   - referrer is the Referer the document's own request carried, as the
 *     host gives it (options.referrer), "" for none;
 *   - activeElement is the element the host says has focus in it
 *     (options.focus), else its body, else its document element, else
 *     null; hasFocus() is whether the host says the document has the
 *     system's focus;
 *   - currentScript is the script element the host says is running
 *     (os64_dom_set_current_script), null between scripts;
 *   - URL and documentURI are its address, as location reads it; domain is
 *     its origin's host ("" for an opaque origin), and setting it runs
 *     HTML's checks and then changes nothing, since every window's agent
 *     cluster is origin-keyed (browsing.c's originAgentCluster). Without
 *     the public-suffix list (DEBTS), the check refuses a one-label
 *     domain, every one of which is a suffix the list names, and lets a
 *     longer public suffix (co.uk) through, which only changes whether a
 *     SecurityError is thrown;
 *   - compatMode is "BackCompat" in quirks mode, else "CSS1Compat";
 *     characterSet, charset and inputEncoding the encoding the parser
 *     decoded it with, by its Encoding Standard name; contentType
 *     "text/html";
 *   - lastModified is the response's Last-Modified (options.last_modified),
 *     else the time it is asked, in the local time zone, as
 *     MM/DD/YYYY hh:mm:ss;
 *   - title, dir and rootElement read the tree as it stands, as HTML
 *     defines them; title's setter makes a title element when there is
 *     none, and clear() does nothing, as HTML defines it.
 *
 * A document of its own has no browsing context: readyState "complete",
 * visibilityState "hidden" and hidden true, referrer "", currentScript
 * null, hasFocus() false, URL and documentURI "about:blank", its maker's
 * origin's domain, which it refuses to set (SecurityError), "CSS1Compat"
 * (no-quirks), "UTF-8", the content type its making gave it, and the time
 * it is asked for lastModified. Its activeElement is its body, document
 * element or null, since nothing in it can have focus.
 *
 * The handlers onreadystatechange, onvisibilitychange and
 * onprerenderingchange are the document's IDL properties alone, with no
 * content attribute (event.c). */

enum {
    P_READY_STATE, P_VISIBILITY_STATE, P_HIDDEN, P_REFERRER, P_ACTIVE_ELEMENT, P_HAS_FOCUS,
    P_CURRENT_SCRIPT, P_TITLE, P_PRERENDERING, P_DOMAIN, P_URL, P_WAS_DISCARDED, P_COMPAT_MODE,
    P_DIR, P_CHARACTER_SET, P_CONTENT_TYPE, P_LAST_MODIFIED, P_ROOT_ELEMENT, P_CLEAR
};

static bool html_element(const os64_html_node_t *node, os64_html_tag_t tag)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML && node->tag == tag;
}

static bool svg_element(const os64_html_node_t *node, const char *name)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_SVG &&
           node->name != NULL && os64_strcmp(node->name, name) == 0;
}

static bool ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* HTML § 3.1.3, the title element: for a document whose element is an SVG
 * `svg`, its first child that is an SVG `title`; else the first HTML
 * `title` in tree order (template contents are no element's children, so
 * the walk does not enter them). NULL for none. */
static const os64_html_node_t *title_element(const os64_html_node_t *document, bool *svg)
{
    const os64_html_node_t *root = d_document_element(document);
    *svg = svg_element(root, "svg");
    if (*svg) {
        for (const os64_html_node_t *at = root->first_child; at != NULL; at = at->next)
            if (svg_element(at, "title")) return at;
        return NULL;
    }
    for (const os64_html_node_t *at = document->first_child; at != NULL;) {
        if (html_element(at, OS64_HTML_TAG_TITLE)) return at;
        if (at->first_child != NULL) { at = at->first_child; continue; }
        while (at != NULL && at != document && at->next == NULL) at = at->parent;
        at = at != NULL && at != document ? at->next : NULL;
    }
    return NULL;
}

/* The title element's child text content with ASCII whitespace stripped
 * and collapsed (HTML § document.title). Linear in the text. */
static JSValue title_text(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *title)
{
    size_t length = 0;
    for (const os64_html_node_t *at = title != NULL ? title->first_child : NULL; at != NULL; at = at->next)
        if (at->kind == OS64_HTML_TEXT) length += at->text_len;
    if (length == 0) return JS_NewString(ctx, "");
    char *text = d_alloc(dom, length);
    if (text == NULL) return d_error(ctx, "QuotaExceededError", "DOM title quota exceeded");
    size_t used = 0;
    bool space = false;
    for (const os64_html_node_t *at = title->first_child; at != NULL; at = at->next) {
        if (at->kind != OS64_HTML_TEXT) continue;
        for (size_t i = 0; i < at->text_len; i++) {
            if (ascii_space(at->text[i])) {
                space = used > 0;
                continue;
            }
            if (space) text[used++] = ' ';
            space = false;
            text[used++] = at->text[i];
        }
    }
    JSValue result = JS_NewStringLen(ctx, text, used);
    d_free(dom, text);
    return result;
}

/* document.title's setter (HTML § document.title): the value converted
 * first, then the title element's text replaced by it, whole, as
 * textContent replaces (no Text node for ""). An SVG document gets an SVG
 * title as its element's first child when it has none; an HTML document
 * a title appended to its head, unless it has no head either, when
 * nothing happens; any other document nothing. */
static JSValue title_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document, JSValueConst value)
{
    JSValue text = JS_ToString(ctx, value);
    if (JS_IsException(text)) return text;
    bool svg;
    const os64_html_node_t *element = title_element(document, &svg);
    const os64_html_node_t *root = d_document_element(document);
    JSValue result = JS_UNDEFINED;
    if (element == NULL && (svg || (root != NULL && root->ns == OS64_HTML_NS_HTML))) {
        const os64_html_node_t *parent = svg ? root : d_document_part(document, false);
        if (parent != NULL) {
            int64_t status;
            os64_html_node_t *made = os64_html_create_element(dom->document, svg ? OS64_HTML_NS_SVG : OS64_HTML_NS_HTML,
                                                              "title", &status);
            if (made == NULL) result = d_html_error(ctx, status);
            else if (d_tree_change(dom, ctx, svg ? D_INSERT : D_APPEND, parent, made,
                                   svg ? parent->first_child : NULL) < 0)
                result = JS_EXCEPTION;
            else element = made;
        }
    }
    if (element != NULL && JS_IsUndefined(result)) result = d_content_set(dom, ctx, element, D_TEXT_CONTENT, text);
    JS_FreeValue(ctx, text);
    return result;
}

/* HTML § Document.activeElement's candidate as DOM's retargeting leaves it
 * for a document with no shadow trees: the focused element when it is in
 * this document, else the document, whose answer is its body, else its
 * element, else null. Only the window's document has anything focused. */
static JSValue active_element(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document, bool window)
{
    const os64_html_node_t *focused = NULL;
    if (window && dom->options.focus != NULL) (void)dom->options.focus(dom->options.host_opaque, &focused);
    if (focused != NULL && os64_html_owns_node(dom->document, focused) && focused->kind == OS64_HTML_ELEMENT &&
        d_in_window_tree(dom, focused))
        return d_wrap(dom, ctx, focused);
    const os64_html_node_t *body = d_document_part(document, true);
    return d_wrap(dom, ctx, body != NULL ? body : d_document_element(document));
}

static bool ascii_digits_and_dots(const char *text)
{
    for (; *text != '\0'; text++)
        if ((*text < '0' || *text > '9') && *text != '.') return false;
    return true;
}

/* document.domain's setter (HTML § Relaxing the same-origin restriction),
 * after WebIDL converts the value: a document with no browsing context, or
 * an opaque origin, is a SecurityError; so is a value that is neither the
 * origin's host nor a suffix of it at a dot (an IP address has none) that
 * could be a registrable domain, which without the public-suffix list
 * means one of more than one label. Then the document's agent cluster is
 * origin-keyed, and setting it changes nothing. */
static JSValue domain_set(os64_dom_t *dom, JSContext *ctx, bool window, JSValueConst value)
{
    DString given = {0};
    if (!d_string(dom, ctx, value, &given)) return JS_EXCEPTION;
    for (size_t i = 0; i < given.length; i++)
        if (given.data[i] >= 'A' && given.data[i] <= 'Z') given.data[i] += 'a' - 'A';
    way_origin_t origin;
    way_origin_of(dom->url != NULL ? dom->url : "about:blank", 0, &origin);
    bool allowed = false;
    if (window && !origin.opaque) {
        size_t host = os64_strlen(origin.host);
        allowed = os64_strcmp(given.data, origin.host) == 0;
        if (!allowed && given.length > 0 && given.length < host && origin.host[0] != '[' &&
            !ascii_digits_and_dots(origin.host) && given.data[0] != '.' && os64_strchr(given.data, '.') != NULL)
            allowed = origin.host[host - given.length - 1] == '.' &&
                      os64_strcmp(origin.host + host - given.length, given.data) == 0;
    }
    d_string_free(dom, &given);
    if (!allowed) return d_error(ctx, "SecurityError", "That is not a domain this document may set");
    return JS_UNDEFINED;
}

/* The Encoding Standard's name for libhtml's: it names an encoding by its
 * lowercased label, and a document whose encoding was never chosen (a
 * refusal) is read as UTF-8, the default. */
static const char *encoding_name(const char *charset)
{
    if (charset == NULL || os64_strcmp(charset, "utf-8") == 0) return "UTF-8";
    if (os64_strcmp(charset, "utf-16le") == 0) return "UTF-16LE";
    if (os64_strcmp(charset, "utf-16be") == 0) return "UTF-16BE";
    return charset;
}

/* HTML § document.lastModified: MM/DD/YYYY hh:mm:ss in the local time
 * zone, of the response's Last-Modified when the host gave one, else of
 * the moment it is asked. */
static JSValue last_modified(JSContext *ctx, int64_t when)
{
    os64_date_t date;
    if (when == OS64_DOM_NO_DATE) os64_date_now(&date, NULL);
    else os64_localtime(when, &date);
    char text[48];
    os64_snprintf(text, sizeof(text), "%02d/%02d/%04d %02d:%02d:%02d", (int)date.month, (int)date.day,
                  (int)date.year, (int)date.hour, (int)date.minute, (int)date.second);
    return JS_NewString(ctx, text);
}

static JSValue document_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *document = d_node(dom, ctx, self);
    if (document == NULL) return JS_EXCEPTION;
    if (document->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Expected a document");
    bool window = document == dom->document->document;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    switch (magic) {
    case P_READY_STATE: {
        static const char *const kStates[] = {"loading", "interactive", "complete"};
        return JS_NewString(ctx, kStates[window ? dom->readiness : OS64_DOM_COMPLETE]);
    }
    case P_VISIBILITY_STATE: return JS_NewString(ctx, window ? "visible" : "hidden");
    case P_HIDDEN: return JS_NewBool(ctx, !window);
    case P_REFERRER: return JS_NewString(ctx, window && dom->referrer != NULL ? dom->referrer : "");
    case P_ACTIVE_ELEMENT: return active_element(dom, ctx, document, window);
    case P_HAS_FOCUS: {
        const os64_html_node_t *focused = NULL;
        return JS_NewBool(ctx, window && dom->options.focus != NULL &&
                                   dom->options.focus(dom->options.host_opaque, &focused));
    }
    case P_CURRENT_SCRIPT: return window && dom->current_script != NULL ? d_wrap(dom, ctx, dom->current_script)
                                                                         : JS_NULL;
    case P_TITLE: {
        if (set) return title_set(dom, ctx, document, value);
        bool svg;
        return title_text(dom, ctx, title_element(document, &svg));
    }
    case P_PRERENDERING: case P_WAS_DISCARDED: return JS_FALSE;
    case P_DOMAIN: {
        if (set) return domain_set(dom, ctx, window, value);
        /* A document of its own has its maker's origin (DOM § 4.5), which
         * is the window's. */
        way_origin_t origin;
        way_origin_of(dom->url != NULL ? dom->url : "about:blank", 0, &origin);
        return JS_NewString(ctx, origin.opaque ? "" : origin.host);
    }
    case P_URL: return JS_NewString(ctx, window && dom->url != NULL ? dom->url : "about:blank");
    case P_COMPAT_MODE:
        return JS_NewString(ctx, window && dom->document->quirks == OS64_HTML_QUIRKS ? "BackCompat" : "CSS1Compat");
    case P_DIR: {
        /* The html element's dir, limited to its known values; "" and
         * nothing set when the document element is not an HTML html. A
         * value is converted before the element is looked for, since its
         * conversion is page code that may move it. */
        DString text = {0};
        if (set && !d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
        const os64_html_node_t *root = d_document_element(document);
        if (!html_element(root, OS64_HTML_TAG_HTML)) {
            d_string_free(dom, &text);
            return set ? JS_UNDEFINED : JS_NewString(ctx, "");
        }
        if (!set) return d_dir_value(ctx, root);
        int64_t status = d_set_attr(dom, root, "dir", text.data, text.length, false);
        d_string_free(dom, &text);
        return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
    }
    case P_CHARACTER_SET: return JS_NewString(ctx, window ? encoding_name(dom->document->charset) : "UTF-8");
    case P_CONTENT_TYPE: {
        if (window) return JS_NewString(ctx, "text/html");
        switch (d_document_type(dom, document)) {
        case D_DOCUMENT_XHTML: return JS_NewString(ctx, "application/xhtml+xml");
        case D_DOCUMENT_SVG: return JS_NewString(ctx, "image/svg+xml");
        case D_DOCUMENT_XML: return JS_NewString(ctx, "application/xml");
        default: return JS_NewString(ctx, "text/html");
        }
    }
    case P_LAST_MODIFIED: return last_modified(ctx, window ? dom->options.last_modified : OS64_DOM_NO_DATE);
    case P_ROOT_ELEMENT: {
        const os64_html_node_t *root = d_document_element(document);
        return svg_element(root, "svg") ? d_wrap(dom, ctx, root) : JS_NULL;
    }
    case P_CLEAR: return JS_UNDEFINED;
    default: return JS_ThrowTypeError(ctx, "Unknown Document member");
    }
}

int d_document_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    (void)global;
    static const struct { const char *name; int magic; bool writable; } kAttributes[] = {
        {"readyState", P_READY_STATE, false}, {"visibilityState", P_VISIBILITY_STATE, false},
        {"hidden", P_HIDDEN, false}, {"referrer", P_REFERRER, false},
        {"activeElement", P_ACTIVE_ELEMENT, false}, {"currentScript", P_CURRENT_SCRIPT, false},
        {"title", P_TITLE, true}, {"prerendering", P_PRERENDERING, false}, {"domain", P_DOMAIN, true},
        {"URL", P_URL, false}, {"documentURI", P_URL, false}, {"wasDiscarded", P_WAS_DISCARDED, false},
        {"compatMode", P_COMPAT_MODE, false}, {"dir", P_DIR, true},
        {"characterSet", P_CHARACTER_SET, false}, {"charset", P_CHARACTER_SET, false},
        {"inputEncoding", P_CHARACTER_SET, false}, {"contentType", P_CONTENT_TYPE, false},
        {"lastModified", P_LAST_MODIFIED, false}, {"rootElement", P_ROOT_ELEMENT, false},
    };
    JSValueConst prototype = dom->prototypes[D_PROTO_DOCUMENT]->value;
    for (size_t i = 0; i < sizeof(kAttributes) / sizeof(kAttributes[0]); i++)
        if (d_accessor(dom, ctx, prototype, kAttributes[i].name, document_member, kAttributes[i].magic,
                       kAttributes[i].writable) < 0)
            return -1;
    if (d_method_named(dom, ctx, prototype, "hasFocus", document_member, 0, P_HAS_FOCUS) < 0 ||
        d_method_named(dom, ctx, prototype, "clear", document_member, 0, P_CLEAR) < 0)
        return -1;
    return d_event_handler(dom, ctx, prototype, D_EVENT_READYSTATECHANGE) < 0 ||
           d_event_handler(dom, ctx, prototype, D_EVENT_VISIBILITYCHANGE) < 0 ||
           d_event_handler(dom, ctx, prototype, D_EVENT_PRERENDERINGCHANGE) < 0 ? -1 : 0;
}

/* document.body's setter (HTML § 3.1.4, the body element), every page's: a
 * value that is not an HTML element is a TypeError (WebIDL's HTMLElement?),
 * one that is neither a body nor a frameset a HierarchyRequestError (null
 * among them); the current body itself changes nothing; otherwise it
 * replaces the current body where that is, or with none is appended to the
 * document element, and with no document element either it is a
 * HierarchyRequestError. The tree change is appendChild's (d_tree_change):
 * a refusal of the tree's own is thrown as appendChild's would be. */
JSValue d_body_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document, JSValueConst value)
{
    const os64_html_node_t *body = NULL;
    if (!JS_IsNull(value) && !JS_IsUndefined(value)) {
        body = d_node(dom, ctx, value);
        if (body == NULL) return JS_EXCEPTION;
        if (body->kind != OS64_HTML_ELEMENT || body->ns != OS64_HTML_NS_HTML)
            return JS_ThrowTypeError(ctx, "document.body must be an HTML element");
    }
    if (body == NULL || (body->tag != OS64_HTML_TAG_BODY && body->tag != OS64_HTML_TAG_FRAMESET))
        return d_error(ctx, "HierarchyRequestError", "document.body must be a body or a frameset");
    const os64_html_node_t *current = d_document_part(document, true);
    if (current == body) return JS_UNDEFINED;
    int done;
    if (current != NULL) done = d_tree_change(dom, ctx, D_REPLACE, current->parent, body, current);
    else {
        const os64_html_node_t *root = d_document_element(document);
        if (root == NULL) return d_error(ctx, "HierarchyRequestError", "The document has no element to hold a body");
        done = d_tree_change(dom, ctx, D_APPEND, root, body, NULL);
    }
    return done < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* READINESS. The rule is os64_dom_readiness_after's; the move, and the
 * readystatechange before the event that made it, are here. */

os64_dom_readiness_t os64_dom_readiness_after(os64_dom_readiness_t now, const os64_html_document_t *doc,
                                              const os64_html_node_t *node, const char *type)
{
    if (type == NULL) return now;
    if (node == NULL && os64_strcmp(type, "load") == 0) return OS64_DOM_COMPLETE;
    if (now == OS64_DOM_LOADING && doc != NULL && node == doc->document &&
        os64_strcmp(type, "DOMContentLoaded") == 0)
        return OS64_DOM_INTERACTIVE;
    return now;
}

static JSValue readystatechange_event(os64_dom_t *dom, JSContext *ctx, void *arg)
{
    (void)arg;
    return d_event_new(dom, ctx, "readystatechange", false, false, true);
}

bool d_readiness_moved(os64_dom_t *dom, const os64_html_node_t *node, const char *type,
                       os64_js_outcome_t *outcome)
{
    os64_dom_readiness_t next = os64_dom_readiness_after(dom->readiness, dom->document, node, type);
    if (next == dom->readiness) return false;
    dom->readiness = next;
    if (dom->options.modern && os64_dom_listens(dom, "readystatechange"))
        d_host_dispatch(dom, dom->document->document, NULL, "readystatechange", readystatechange_event, NULL,
                        NULL, outcome);
    return true;
}

void os64_dom_set_current_script(os64_dom_t *dom, const os64_html_node_t *script)
{
    if (dom == NULL) return;
    if (script != NULL && (!os64_html_owns_node(dom->document, script) || script->kind != OS64_HTML_ELEMENT))
        return;
    if (script != NULL) os64_html_hold(dom->document, script);
    if (dom->current_script != NULL) os64_html_release(dom->document, dom->current_script);
    dom->current_script = script;
}

int d_document_setup(os64_dom_t *dom)
{
    const char *referrer = dom->options.referrer;
    dom->options.referrer = NULL;
    if (dom->options.readiness > OS64_DOM_COMPLETE) dom->options.readiness = OS64_DOM_LOADING;
    dom->readiness = dom->options.readiness;
    if (referrer == NULL || referrer[0] == '\0') return 0;
    size_t size = os64_strlen(referrer) + 1;
    dom->referrer = d_alloc(dom, size);
    if (dom->referrer == NULL) return -1;
    os64_memcpy(dom->referrer, referrer, size);
    return 0;
}

void d_document_free(os64_dom_t *dom)
{
    d_free(dom, dom->referrer);
    dom->referrer = NULL;
    if (dom->current_script != NULL) os64_html_release(dom->document, dom->current_script);
    dom->current_script = NULL;
}
