#include "internal.h"

void d_fold(char *text)
{
    for (; *text != '\0'; text++)
        if (*text >= 'A' && *text <= 'Z') *text += 'a' - 'A';
}

/* QuickJS emits unmatched UTF-16 surrogates as three-byte UTF-8 sequences.
 * Replace these and embedded NUL before crossing native text contracts. */
bool d_string(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DString *out)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return false;
    size_t length;
    const char *source = JS_ToCStringLen(ctx, &length, value);
    if (source == NULL) return false;
    size_t extra = 0;
    for (size_t i = 0; i < length; i++) if (source[i] == '\0') {
        if (extra > SIZE_MAX - 2) { JS_FreeCString(ctx, source); goto quota; }
        extra += 2;
    }
    if (length == SIZE_MAX || extra > SIZE_MAX - length - 1) {
        JS_FreeCString(ctx, source);
        goto quota;
    }
    char *copy = d_alloc_kind(dom, length + extra + 1, OS64_DOM_MEM_TEXT);
    if (copy == NULL) { JS_FreeCString(ctx, source); goto quota; }
    size_t n = 0;
    for (size_t i = 0; i < length;) {
        unsigned char c = (unsigned char)source[i];
        if (c == 0) {
            copy[n++] = (char)0xef; copy[n++] = (char)0xbf; copy[n++] = (char)0xbd;
            i++;
        } else if (c == 0xed && i + 2 < length &&
                   (unsigned char)source[i + 1] >= 0xa0 &&
                   (unsigned char)source[i + 1] <= 0xbf) {
            copy[n++] = (char)0xef; copy[n++] = (char)0xbf; copy[n++] = (char)0xbd;
            i += 3;
        } else copy[n++] = source[i++];
    }
    copy[n] = '\0';
    JS_FreeCString(ctx, source);
    out->data = copy;
    out->length = n;
    return true;
quota:
    d_error(ctx, "QuotaExceededError", "DOM string quota exceeded");
    return false;
}

void d_string_free(os64_dom_t *dom, DString *string)
{
    d_free(dom, string->data);
    string->data = NULL;
    string->length = 0;
}

static bool name_space(unsigned char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static bool name_alpha(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool d_attribute_name_valid(const char *text)
{
    const unsigned char *at = (const unsigned char *)text;
    if (*at == 0) return false;
    for (; *at != 0; at++)
        if (name_space(*at) || *at == '/' || *at == '=' || *at == '>') return false;
    return true;
}

bool d_element_name_valid(const char *text)
{
    const unsigned char *at = (const unsigned char *)text;
    if (*at == 0) return false;
    if (name_alpha(*at)) {
        for (; *at != 0; at++)
            if (name_space(*at) || *at == '/' || *at == '>') return false;
        return true;
    }
    if (*at != ':' && *at != '_' && *at < 0x80) return false;
    for (at++; *at != 0; at++)
        if (!name_alpha(*at) && !(*at >= '0' && *at <= '9') && *at != '-' &&
            *at != '.' && *at != ':' && *at != '_' && *at < 0x80) return false;
    return true;
}

static const os64_html_node_t *text_next(const os64_html_node_t *root,
                                         const os64_html_node_t *node)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

JSValue d_content_get(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                      int property)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    if (property == D_INNER_HTML || property == D_OUTER_HTML) {
        /* A ShadowRoot's innerHTML is its children's, as an element's is. */
        bool root = property == D_INNER_HTML && os64_html_shadow_host(node) != NULL;
        if (node->kind != OS64_HTML_ELEMENT && !root) return JS_ThrowTypeError(ctx, "HTML requires an element");
        bool scripting = d_scripting(dom, node);
        size_t length = os64_html_serialize(node, property == D_INNER_HTML, scripting, NULL, 0);
        char *buffer = length == SIZE_MAX ? NULL : d_alloc_kind(dom, length + 1, OS64_DOM_MEM_TEXT);
        if (buffer == NULL) return d_error(ctx, "QuotaExceededError", "DOM serialization quota exceeded");
        os64_html_serialize(node, property == D_INNER_HTML, scripting, buffer, length + 1);
        JSValue result = JS_NewStringLen(ctx, buffer, length);
        d_free(dom, buffer);
        return result;
    }
    if (node->kind == OS64_HTML_TEXT || node->kind == OS64_HTML_COMMENT || node->kind == OS64_HTML_PI)
        return JS_NewStringLen(ctx, node->text != NULL ? node->text : "", node->text_len);
    if (property == D_DATA) return JS_ThrowTypeError(ctx, "data requires a character-data node");
    if (property == D_NODE_VALUE || node->kind == OS64_HTML_DOCUMENT || node->kind == OS64_HTML_DOCTYPE)
        return JS_NULL;
    size_t length = 0;
    for (const os64_html_node_t *at = node->first_child; at != NULL; at = text_next(node, at))
        if (at->kind == OS64_HTML_TEXT) {
            if (at->text_len > SIZE_MAX - length) return d_error(ctx, "QuotaExceededError", "DOM text quota exceeded");
            length += at->text_len;
        }
    char *buffer = length == SIZE_MAX ? NULL : d_alloc_kind(dom, length + 1, OS64_DOM_MEM_TEXT);
    if (buffer == NULL) return d_error(ctx, "QuotaExceededError", "DOM text quota exceeded");
    size_t offset = 0;
    for (const os64_html_node_t *at = node->first_child; at != NULL; at = text_next(node, at))
        if (at->kind == OS64_HTML_TEXT) {
            os64_memcpy(buffer + offset, at->text, at->text_len);
            offset += at->text_len;
        }
    buffer[length] = '\0';
    JSValue result = JS_NewStringLen(ctx, buffer, length);
    d_free(dom, buffer);
    return result;
}

/* libhtml retains current-document metadata when children become detached. */
int64_t d_replace_content(os64_dom_t *dom, os64_html_node_t *parent,
                           os64_html_node_t *replacement)
{
    dom = d_owner(dom, parent);
    DAdopting scope;
    d_adopting_begin(dom, &scope, replacement, parent);
    int64_t status = os64_page_node_replace_children(dom->state, parent, replacement);
    d_adopting_end(dom, &scope);
    return status;
}

JSValue d_content_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                      int property, JSValueConst value)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    if (property == D_DATA && node->kind != OS64_HTML_TEXT && node->kind != OS64_HTML_COMMENT &&
        node->kind != OS64_HTML_PI)
        return JS_ThrowTypeError(ctx, "data requires a character-data node");
    /* A ShadowRoot's innerHTML parses with its host as the context. */
    const os64_html_node_t *context = property == D_INNER_HTML && os64_html_shadow_host(node) != NULL
        ? os64_html_shadow_host(node) : node;
    if (property == D_INNER_HTML && context->kind != OS64_HTML_ELEMENT)
        return JS_ThrowTypeError(ctx, "innerHTML requires an element");
    DString string = {0};
    JSValue empty = JS_UNDEFINED;
    if (JS_IsNull(value) ||
        (JS_IsUndefined(value) && (property == D_NODE_VALUE || property == D_TEXT_CONTENT))) {
        empty = JS_NewString(ctx, "");
        if (JS_IsException(empty)) return empty;
        value = empty;
    }
    bool converted = d_string(dom, ctx, value, &string);
    JS_FreeValue(ctx, empty);
    if (!converted) return JS_EXCEPTION;
    dom = d_owner(dom, node);
    int64_t status = OS64_HTML_OK;
    if (property == D_NODE_VALUE && node->kind != OS64_HTML_TEXT && node->kind != OS64_HTML_COMMENT &&
        node->kind != OS64_HTML_PI) {
        d_string_free(dom, &string);
        return JS_UNDEFINED;
    }
    if (property == D_INNER_HTML) {
        const os64_html_node_t *parse_context = d_fragment_context(dom, context, &status);
        os64_html_node_t *fragment = parse_context == NULL ? NULL :
            os64_html_parse_fragment(dom->document, parse_context, string.data,
                                               string.length, d_scripting(dom, context), &status);
        /* A script innerHTML inserts never runs: it is born started. */
        if (fragment != NULL && d_script_mark_tree(dom, fragment) < 0) status = OS64_HTML_NO_MEMORY;
        else if (fragment != NULL) status = d_replace_content(dom, node->template_contents != NULL ?
                                          node->template_contents : (os64_html_node_t *)node, fragment);
    } else if (node->kind == OS64_HTML_TEXT || node->kind == OS64_HTML_COMMENT || node->kind == OS64_HTML_PI)
        status = os64_page_node_set_text(d_owner(dom, node)->state, (os64_html_node_t *)node, string.data, string.length);
    else if (node->kind == OS64_HTML_ELEMENT || node->kind == OS64_HTML_FRAGMENT) {
        os64_html_node_t *replacement = NULL;
        if (string.length != 0)
            replacement = os64_html_create_text(dom->document, string.data, string.length, &status);
        if (status == OS64_HTML_OK) status = d_replace_content(dom, (os64_html_node_t *)node, replacement);
    }
    d_string_free(dom, &string);
    return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
}

/* ── innerText and outerText (HTML § 3.2.7) ─────────────────────────────
 * The getter is the element's text as rendered: inner_text.c's walk over
 * the host's four facts per element (os64_dom_set_text_style). With no
 * host to ask, an element outside a window's document, or one the walk
 * finds is not being rendered, the answer is its textContent, which is the
 * standard's for an element not rendered and for a browser without CSS.
 * The setters turn text into Text nodes and a `br` for each line break. */

static void *rendered_grow(void *ctx, void *old, size_t old_size, size_t size)
{
    os64_dom_t *dom = ctx;
    void *block = d_alloc(dom, size);
    if (block == NULL) return NULL;
    if (old != NULL) {
        os64_memcpy(block, old, old_size);
        d_free(dom, old);
    }
    return block;
}

static void rendered_release(void *ctx, void *block)
{
    if (block != NULL) d_free(ctx, block);
}

static JSValue rendered_text(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    if (dom->text_style != NULL && d_in_window_tree(dom, node)) {
        d_inner_text_memory_t memory = {rendered_grow, rendered_release, dom};
        char *text = NULL;
        size_t length = 0;
        d_inner_text_status_t status = d_inner_text(node, dom->text_style, dom->text_style_opaque, &memory, &text,
                                                    &length);
        if (status == D_INNER_TEXT_NO_MEMORY) return d_error(ctx, "QuotaExceededError", "DOM text quota exceeded");
        if (status == D_INNER_TEXT_OK) {
            JSValue result = JS_NewStringLen(ctx, text, length);
            d_free(dom, text);
            return result;
        }
    }
    return d_content_get(dom, ctx, node, D_TEXT_CONTENT);
}

/* The standard's "rendered text fragment": a Text node for each run
 * between line breaks that is not empty, and a `br` for each break (CR LF
 * one). NULL in `*out` for no nodes at all. */
static int64_t rendered_fragment(os64_dom_t *dom, const char *text, size_t length, os64_html_node_t **out)
{
    *out = NULL;
    int64_t status = OS64_HTML_OK;
    os64_html_node_t *fragment = NULL;
    for (size_t at = 0; at < length && status >= 0;) {
        if (fragment == NULL) {
            fragment = os64_html_create_fragment(dom->document, &status);
            if (fragment == NULL) break;
        }
        size_t end = at;
        while (end < length && text[end] != '\n' && text[end] != '\r') end++;
        os64_html_node_t *piece = end > at
            ? os64_html_create_text(dom->document, text + at, end - at, &status)
            : os64_html_create_element(dom->document, OS64_HTML_NS_HTML, "br", &status);
        if (piece == NULL) break;
        status = os64_html_insert(dom->document, fragment, piece, NULL);
        if (end == at) at += text[at] == '\r' && at + 1 < length && text[at + 1] == '\n' ? 2 : 1;
        else at = end;
    }
    if (status < 0) return status;
    *out = fragment;
    return OS64_HTML_OK;
}

/* DOM's "merge with the next text node": `node`'s next sibling joins it
 * when both are Text, by replace data (live ranges move as they would for
 * appendData), and is removed. */
static int merge_next_text(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    if (node == NULL || node->kind != OS64_HTML_TEXT) return 0;
    const os64_html_node_t *next = node->next;
    if (next == NULL || next->kind != OS64_HTML_TEXT) return 0;
    int64_t status = os64_page_node_replace_data(dom->state, (os64_html_node_t *)node, os64_html_node_length(node), 0,
                                                 next->text, next->text_len);
    if (status < 0) { d_html_error(ctx, status); return -1; }
    return d_tree_change(dom, ctx, D_REMOVE, NULL, next, NULL);
}

static JSValue rendered_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, JSValueConst value,
                            bool outer)
{
    /* [LegacyNullToEmptyString]: null is "", anything else a string. */
    JSValue text = JS_IsNull(value) ? JS_NewString(ctx, "") : JS_ToString(ctx, value);
    if (JS_IsException(text)) return text;
    DString string = {0};
    bool converted = d_string(dom, ctx, text, &string);
    JS_FreeValue(ctx, text);
    if (!converted) return JS_EXCEPTION;
    const os64_html_node_t *parent = node->parent;
    if (outer && parent == NULL) {
        d_string_free(dom, &string);
        return d_error(ctx, "NoModificationAllowedError", "outerText needs the element to have a parent");
    }
    os64_html_node_t *fragment = NULL;
    int64_t status = rendered_fragment(dom, string.data, string.length, &fragment);
    d_string_free(dom, &string);
    /* outerText leaves something in the element's place: an empty Text
     * node when the value made none. */
    if (status >= 0 && outer && fragment == NULL) {
        fragment = os64_html_create_text(dom->document, "", 0, &status);
    }
    if (status < 0) return d_html_error(ctx, status);
    if (!outer) return d_tree_change(dom, ctx, D_REPLACE_CHILDREN, node, fragment, NULL) < 0 ? JS_EXCEPTION
                                                                                             : JS_UNDEFINED;
    /* The neighbours the merges join, held across the replacement. */
    const os64_html_node_t *previous = node->prev, *next = node->next;
    if (previous != NULL) os64_html_hold(dom->document, previous);
    if (next != NULL) os64_html_hold(dom->document, next);
    int result = d_tree_change(dom, ctx, D_REPLACE, parent, fragment, node);
    if (result == 0 && next != NULL && next->parent == parent) result = merge_next_text(dom, ctx, next->prev);
    if (result == 0 && previous != NULL && previous->parent == parent) result = merge_next_text(dom, ctx, previous);
    if (previous != NULL) os64_html_release(dom->document, previous);
    if (next != NULL) os64_html_release(dom->document, next);
    return result < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

JSValue d_rendered_text_member(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, bool outer, bool set,
                               JSValueConst value)
{
    dom = d_owner(dom, node);
    return set ? rendered_set(dom, ctx, node, value, outer) : rendered_text(dom, ctx, node);
}
