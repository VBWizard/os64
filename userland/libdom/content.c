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
    char *copy = d_alloc(dom, length + extra + 1);
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
        if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "HTML requires an element");
        size_t length = os64_html_serialize(node, property == D_INNER_HTML,
                                            dom->options.scripting, NULL, 0);
        char *buffer = length == SIZE_MAX ? NULL : d_alloc(dom, length + 1);
        if (buffer == NULL) return d_error(ctx, "QuotaExceededError", "DOM serialization quota exceeded");
        os64_html_serialize(node, property == D_INNER_HTML, dom->options.scripting, buffer, length + 1);
        JSValue result = JS_NewStringLen(ctx, buffer, length);
        d_free(dom, buffer);
        return result;
    }
    if (node->kind == OS64_HTML_TEXT || node->kind == OS64_HTML_COMMENT)
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
    char *buffer = length == SIZE_MAX ? NULL : d_alloc(dom, length + 1);
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

int64_t d_replace_content(os64_dom_t *dom, os64_html_node_t *parent,
                           os64_html_node_t *replacement)
{
    return os64_page_node_replace_children(dom->state, parent, replacement);
}

JSValue d_content_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                      int property, JSValueConst value)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    if (property == D_DATA && node->kind != OS64_HTML_TEXT && node->kind != OS64_HTML_COMMENT)
        return JS_ThrowTypeError(ctx, "data requires a character-data node");
    if (property == D_INNER_HTML && node->kind != OS64_HTML_ELEMENT)
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
    int64_t status = OS64_HTML_OK;
    if (property == D_NODE_VALUE && node->kind != OS64_HTML_TEXT && node->kind != OS64_HTML_COMMENT) {
        d_string_free(dom, &string);
        return JS_UNDEFINED;
    }
    if (property == D_INNER_HTML) {
        os64_html_node_t *fragment = os64_html_parse_fragment(dom->document, node, string.data,
                                               string.length, dom->options.scripting, &status);
        /* A script innerHTML inserts never runs: it is born started. */
        if (fragment != NULL && d_script_mark_tree(dom, fragment) < 0) status = OS64_HTML_NO_MEMORY;
        else if (fragment != NULL) status = d_replace_content(dom, node->template_contents != NULL ?
                                          node->template_contents : (os64_html_node_t *)node, fragment);
    } else if (node->kind == OS64_HTML_TEXT || node->kind == OS64_HTML_COMMENT)
        status = os64_page_node_set_text(dom->state, (os64_html_node_t *)node, string.data, string.length);
    else if (node->kind == OS64_HTML_ELEMENT || node->kind == OS64_HTML_FRAGMENT) {
        os64_html_node_t *replacement = NULL;
        if (string.length != 0)
            replacement = os64_html_create_text(dom->document, string.data, string.length, &status);
        if (status == OS64_HTML_OK) status = d_replace_content(dom, (os64_html_node_t *)node, replacement);
    }
    d_string_free(dom, &string);
    return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
}
