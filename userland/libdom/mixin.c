#include "internal.h"
#include "os64/fmt.h"

/* THE DOM PAGES ASSUME (LIBDOM.md § The classic methods): ParentNode's
 * append/prepend/replaceChildren, ChildNode's before/after/replaceWith/
 * remove, contains and isConnected, matches and closest, the class and name
 * queries, insertAdjacent*, the attribute conveniences, the reflected
 * attributes (hidden, lang, dir, tabIndex, a script's src, a link's and an
 * anchor's href), classList and dataset. Each is the standard's steps over
 * the verbs node.c already uses, so a tree change here is the change
 * appendChild would make, scripts and controls included. */

static bool is_element(const os64_html_node_t *node)
{
    return node->kind == OS64_HTML_ELEMENT;
}

static bool is_html(const os64_html_node_t *node, os64_html_tag_t tag)
{
    return node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML && node->tag == tag;
}

/* THE ARGUMENTS ARE CONVERTED BEFORE THE TREE IS READ (Web IDL converts
 * an operation's arguments, then runs its steps). A string's conversion
 * runs script, and that script may move or reclaim any node a position was
 * read from; so every method here converts first, and only then reads a
 * parent or a sibling, and nothing between that read and the change runs
 * script. One converted argument: a node of this document, or text. */
typedef struct { const os64_html_node_t *node; DString text; } Arg;

static void args_free(os64_dom_t *dom, Arg *args, int count)
{
    for (int i = 0; i < count; i++) d_string_free(dom, &args[i].text);
    d_free(dom, args);
}

/* Node-or-string arguments, converted in order. False with the exception
 * thrown (a node from another document is appendChild's TypeError);
 * `*args` is NULL for none. */
static bool args_of(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, Arg **args)
{
    *args = NULL;
    if (argc == 0) return true;
    *args = d_alloc(dom, (size_t)argc * sizeof(**args));
    if (*args == NULL) { d_error(ctx, "QuotaExceededError", "DOM argument quota exceeded"); return false; }
    for (int i = 0; i < argc; i++) {
        if (JS_GetOpaque(argv[i], dom->node_class) != NULL) {
            (*args)[i].node = d_node(dom, ctx, argv[i]);
            if ((*args)[i].node == NULL) { args_free(dom, *args, i); *args = NULL; return false; }
        } else if (!d_string(dom, ctx, argv[i], &(*args)[i].text)) {
            args_free(dom, *args, i);
            *args = NULL;
            return false;
        }
    }
    return true;
}

/* DOM § "convert nodes into a node", over converted arguments: a node is
 * itself, text is a new Text node, and more than one are gathered, in
 * order, into a new fragment. The answer is HELD: the caller releases it.
 * NULL with the exception thrown. Runs no script. */
static os64_html_node_t *into_node(os64_dom_t *dom, JSContext *ctx, const Arg *args, int count)
{
    int64_t status = OS64_HTML_OK;
    os64_html_node_t *fragment = NULL;
    if (count != 1) {
        fragment = os64_html_create_fragment(dom->document, &status);
        if (fragment == NULL) { d_html_error(ctx, status); return NULL; }
        os64_html_hold(dom->document, fragment);
    }
    for (int i = 0; i < count; i++) {
        os64_html_node_t *node = (os64_html_node_t *)args[i].node;
        if (node == NULL) {
            node = os64_html_create_text(dom->document, args[i].text.data, args[i].text.length, &status);
            if (node == NULL) { d_html_error(ctx, status); break; }
        }
        if (fragment == NULL) {
            os64_html_hold(dom->document, node);
            return node;
        }
        status = d_insert(dom, fragment, node, NULL);
        if (status < 0) { d_html_error(ctx, status); break; }
        if (i + 1 == count) return fragment;
    }
    if (fragment != NULL && count == 0) return fragment;
    if (fragment != NULL) os64_html_release(dom->document, fragment);
    return NULL;
}

/* Whether `node` is one of the arguments (before/after/replaceWith skip
 * past them to find where the new nodes go). */
static bool among(const os64_html_node_t *node, const Arg *args, int count)
{
    for (int i = 0; i < count; i++)
        if (args[i].node == node) return true;
    return false;
}

static int change(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *self, int magic,
                  const Arg *args, int count)
{
    const os64_html_node_t *parent = magic == D_APPEND_NODES || magic == D_PREPEND ||
        magic == D_REPLACE_CHILDREN ? self : self->parent;
    if (parent == NULL) return 0;
    if (magic == D_REPLACE_CHILDREN) {
        /* A document keeps its <html> element (libhtml's rule; LIBDOM.md
         * § The classic methods), so replacing its children is refused
         * before anything moves. */
        if (parent->kind == OS64_HTML_DOCUMENT) { d_html_error(ctx, OS64_HTML_ROOT_REQUIRED); return -1; }
        /* The replacement must be nowhere yet, so a lone node still in a
         * tree is moved into a fragment of its own first; whether it may
         * go under `parent` at all is asked BEFORE that move, or a refusal
         * would leave it taken from where it was. */
        if (count == 1 && args[0].node != NULL && args[0].node->parent != NULL) {
            int64_t verdict = os64_html_may_insert(dom->document, parent, args[0].node);
            if (verdict < 0) { d_html_error(ctx, verdict); return -1; }
        }
    }
    /* Where the nodes go is found before they are gathered: gathering may
     * move the very siblings it would otherwise be measured from. */
    const os64_html_node_t *previous = self->prev, *next = self->next;
    while (magic == D_BEFORE && previous != NULL && among(previous, args, count)) previous = previous->prev;
    while ((magic == D_AFTER || magic == D_REPLACE_WITH) && next != NULL && among(next, args, count))
        next = next->next;
    os64_html_node_t *node = into_node(dom, ctx, args, count);
    if (node == NULL) return -1;
    if (magic == D_REPLACE_CHILDREN && node->parent != NULL) {
        int64_t status = OS64_HTML_OK;
        os64_html_node_t *fragment = os64_html_create_fragment(dom->document, &status);
        if (fragment != NULL) {
            os64_html_hold(dom->document, fragment);
            status = d_insert(dom, fragment, node, NULL);
        }
        os64_html_release(dom->document, node);
        if (fragment == NULL || status < 0) {
            if (fragment != NULL) os64_html_release(dom->document, fragment);
            d_html_error(ctx, status);
            return -1;
        }
        node = fragment;
    }
    int placed;
    switch (magic) {
    case D_APPEND_NODES: placed = d_tree_change(dom, ctx, D_APPEND, parent, node, NULL); break;
    case D_PREPEND: placed = d_tree_change(dom, ctx, D_INSERT, parent, node, parent->first_child); break;
    case D_REPLACE_CHILDREN: placed = d_tree_change(dom, ctx, D_REPLACE_CHILDREN, parent, node, NULL); break;
    case D_BEFORE:
        placed = d_tree_change(dom, ctx, D_INSERT, parent, node, previous != NULL ? previous->next : parent->first_child);
        break;
    case D_AFTER: placed = d_tree_change(dom, ctx, D_INSERT, parent, node, next); break;
    default:    /* D_REPLACE_WITH: in its place if it is still there, else where it was */
        placed = self->parent == parent ? d_tree_change(dom, ctx, D_REPLACE, parent, node, self)
                                        : d_tree_change(dom, ctx, D_INSERT, parent, node, next);
        break;
    }
    os64_html_release(dom->document, node);
    return placed;
}

static JSValue tree_change(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *self,
                           int magic, int argc, JSValueConst *argv)
{
    if (magic == D_REMOVE_SELF)
        return self->parent == NULL || d_tree_change(dom, ctx, D_REMOVE, self->parent, self, NULL) == 0
            ? JS_UNDEFINED : JS_EXCEPTION;
    Arg *args;
    if (!args_of(dom, ctx, argc, argv, &args)) return JS_EXCEPTION;
    int placed = change(dom, ctx, self, magic, args, argc);
    args_free(dom, args, argc);
    return placed == 0 ? JS_UNDEFINED : JS_EXCEPTION;
}

/* insertAdjacent*'s four places, named without regard to ASCII case: the
 * parent and the node to go before. -1 with a SyntaxError thrown for any
 * other word; 0 when the place has no parent (before or after a node
 * outside a tree). */
static int adjacent(JSContext *ctx, const os64_html_node_t *self, char *word,
                    const os64_html_node_t **parent, const os64_html_node_t **before)
{
    d_fold(word);
    if (os64_streq(word, "beforebegin")) { *parent = self->parent; *before = self; }
    else if (os64_streq(word, "afterbegin")) { *parent = self; *before = self->first_child; }
    else if (os64_streq(word, "beforeend")) { *parent = self; *before = NULL; }
    else if (os64_streq(word, "afterend")) { *parent = self->parent; *before = self->next; }
    else {
        d_error(ctx, "SyntaxError", "The position is not beforebegin, afterbegin, beforeend or afterend");
        return -1;
    }
    return *parent != NULL ? 1 : 0;
}

/* After the arguments are converted (the position, then the HTML or the
 * text, both strings; or the element). */
static JSValue adjacent_steps(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *self, int magic,
                              DString *word, const os64_html_node_t *element, const DString *text)
{
    const os64_html_node_t *parent = NULL, *before = NULL;
    int place = adjacent(ctx, self, word->data, &parent, &before);
    if (place < 0) return JS_EXCEPTION;
    if (magic == D_ADJACENT_HTML) {
        /* The fragment is parsed as if it were the context element's
         * contents: the parent's for a place beside this node. A context
         * that is no element, or is <html>, parses as <body> would. */
        if (place == 0 || parent->kind == OS64_HTML_DOCUMENT)
            return d_error(ctx, "NoModificationAllowedError", "The element has no parent element to insert beside");
        int64_t status = OS64_HTML_OK;
        const os64_html_node_t *context = parent;
        if (!is_element(context) || is_html(context, OS64_HTML_TAG_HTML))
            context = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, "body", &status);
        os64_html_node_t *fragment = context == NULL ? NULL :
            os64_html_parse_fragment(dom->document, context, text->data, text->length, dom->options.scripting, &status);
        if (fragment == NULL) return d_html_error(ctx, status);
        os64_html_hold(dom->document, fragment);
        /* A script insertAdjacentHTML inserts never runs: it is born started. */
        int placed = d_script_mark_tree(dom, fragment) < 0
            ? (d_error(ctx, "QuotaExceededError", "DOM script mark quota exceeded"), -1)
            : d_tree_change(dom, ctx, D_INSERT, parent, fragment, before);
        os64_html_release(dom->document, fragment);
        return placed == 0 ? JS_UNDEFINED : JS_EXCEPTION;
    }
    if (place == 0) return JS_NULL;
    if (magic == D_ADJACENT_ELEMENT) {
        JSValue result = d_wrap(dom, ctx, element);
        if (JS_IsException(result)) return result;
        if (d_tree_change(dom, ctx, D_INSERT, parent, element, before) < 0) { JS_FreeValue(ctx, result); return JS_EXCEPTION; }
        return result;
    }
    /* insertAdjacentText's data is a string, whatever it was: always a new
     * Text node, never a node it was handed. */
    int64_t status = OS64_HTML_OK;
    os64_html_node_t *node = os64_html_create_text(dom->document, text->data, text->length, &status);
    if (node == NULL) return d_html_error(ctx, status);
    os64_html_hold(dom->document, node);
    int placed = d_tree_change(dom, ctx, D_INSERT, parent, node, before);
    os64_html_release(dom->document, node);
    return placed == 0 ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue insert_adjacent(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *self,
                               int magic, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "insertAdjacent takes a position and what to insert");
    DString word = {0}, text = {0};
    const os64_html_node_t *element = NULL;
    if (!d_string(dom, ctx, argv[0], &word)) return JS_EXCEPTION;
    JSValue result = JS_EXCEPTION;
    if (magic == D_ADJACENT_ELEMENT) {
        element = d_node(dom, ctx, argv[1]);
        if (element != NULL && !is_element(element)) {
            JS_ThrowTypeError(ctx, "insertAdjacentElement takes an element");
            element = NULL;
        }
        if (element != NULL) result = adjacent_steps(dom, ctx, self, magic, &word, element, NULL);
    } else if (d_string(dom, ctx, argv[1], &text)) {
        result = adjacent_steps(dom, ctx, self, magic, &word, NULL, &text);
    }
    d_string_free(dom, &text);
    d_string_free(dom, &word);
    return result;
}

/* An attribute name as a script names it: HTML folds its elements' names. */
static bool attribute_name(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *self,
                           JSValueConst value, DString *name)
{
    if (!d_string(dom, ctx, value, name)) return false;
    if (self->ns == OS64_HTML_NS_HTML) d_fold(name->data);
    return true;
}

static JSValue set_attribute(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                             const char *name, const char *value, size_t length, bool remove)
{
    int64_t status = os64_page_node_set_attr(dom->state, node, name, value, length, remove);
    return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
}

static JSValue element_method(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *self,
                              int magic, int argc, JSValueConst *argv)
{
    if (magic == D_MATCHES || magic == D_CLOSEST) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing selector");
        return d_selector_test(dom, ctx, self, argv[0], magic == D_CLOSEST);
    }
    if (magic >= D_ADJACENT_HTML && magic <= D_ADJACENT_TEXT)
        return insert_adjacent(dom, ctx, self, magic, argc, argv);
    if (magic == D_HAS_ATTRS) return JS_NewBool(ctx, self->attrs != NULL);
    if (magic == D_ATTRIBUTE_NAMES) {
        JSValue names = JS_NewArray(ctx);
        uint32_t i = 0;
        for (const os64_html_attr_t *attr = self->attrs; attr != NULL && !JS_IsException(names); attr = attr->next)
            if (JS_DefinePropertyValueUint32(ctx, names, i++, JS_NewString(ctx, attr->name), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, names);
                names = JS_EXCEPTION;
            }
        return names;
    }
    /* D_TOGGLE_ATTR: present unless `force` says otherwise; answers whether
     * it is present afterwards. */
    if (argc < 1) return JS_ThrowTypeError(ctx, "Missing attribute name");
    DString name = {0};
    if (!attribute_name(dom, ctx, self, argv[0], &name)) return JS_EXCEPTION;
    if (!d_attribute_name_valid(name.data)) {
        d_string_free(dom, &name);
        return d_error(ctx, "InvalidCharacterError", "Invalid attribute name");
    }
    int force = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToBool(ctx, argv[1]) : -1;
    bool present = os64_html_attr(self, name.data) != NULL;
    JSValue result = JS_UNDEFINED;
    if (!present && force != 0) {
        result = d_handler_attribute_set(dom, ctx, self, name.data) < 0 ? JS_EXCEPTION
            : set_attribute(dom, ctx, self, name.data, "", 0, false);
        present = true;
    } else if (present && force != 1) {
        result = set_attribute(dom, ctx, self, name.data, NULL, 0, true);
        present = false;
    }
    d_string_free(dom, &name);
    return JS_IsException(result) ? result : JS_NewBool(ctx, present);
}

static JSValue mixin_method(JSContext *ctx, JSValueConst this_value, int argc,
                            JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *self = d_node(dom, ctx, this_value);
    if (self == NULL) return JS_EXCEPTION;
    switch (magic) {
    case D_APPEND_NODES: case D_PREPEND: case D_REPLACE_CHILDREN:
    case D_BEFORE: case D_AFTER: case D_REPLACE_WITH: case D_REMOVE_SELF:
        return tree_change(dom, ctx, self, magic, argc, argv);
    case D_CONTAINS: {
        if (argc < 1 || JS_IsNull(argv[0])) return JS_FALSE;
        const os64_html_node_t *other = d_node(dom, ctx, argv[0]);
        if (other == NULL) return JS_EXCEPTION;
        while (other != NULL && other != self) other = other->parent;
        return JS_NewBool(ctx, other == self);
    }
    case D_BY_CLASS: case D_BY_NAME:
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing query argument");
        return d_attribute_collection(dom, ctx, self, argv[0], magic == D_BY_NAME);
    default:
        if (!is_element(self)) return JS_ThrowTypeError(ctx, "Element method requires an element");
        return element_method(dom, ctx, self, magic, argc, argv);
    }
}

/* ── Reflected attributes ─────────────────────────────────────────────── */

enum { R_HIDDEN, R_LANG, R_DIR, R_TAB_INDEX, R_URL, R_CLASS_LIST, R_DATASET, R_CONNECTED };

/* An element that can take focus without a tabindex: its tabIndex reads 0
 * where any other reads -1 (HTML § The tabindex attribute). */
static bool focusable(const os64_html_node_t *node)
{
    if (node->ns != OS64_HTML_NS_HTML) return false;
    switch (node->tag) {
    case OS64_HTML_TAG_A: case OS64_HTML_TAG_AREA: return os64_html_attr(node, "href") != NULL;
    case OS64_HTML_TAG_BUTTON: case OS64_HTML_TAG_INPUT: case OS64_HTML_TAG_SELECT:
    case OS64_HTML_TAG_TEXTAREA: case OS64_HTML_TAG_IFRAME: case OS64_HTML_TAG_SUMMARY:
        return true;
    default: return false;
    }
}

/* HTML § Rules for parsing integers: leading whitespace, a sign, digits. */
static bool parse_integer(const char *text, int32_t *out)
{
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\f' || *text == '\r') text++;
    bool negative = *text == '-';
    if (*text == '-' || *text == '+') text++;
    if (*text < '0' || *text > '9') return false;
    int64_t value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text++ - '0');
        if (value > (int64_t)INT32_MAX + 1) return false;
    }
    if (negative) value = -value;
    if (value > INT32_MAX) return false;
    *out = (int32_t)value;
    return true;
}

JSValue d_class_list(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);
JSValue d_dataset(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node);

static JSValue reflect(JSContext *ctx, JSValueConst this_value, int argc,
                       JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *self = d_node(dom, ctx, this_value);
    if (self == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (magic == R_CONNECTED) return JS_NewBool(ctx, d_connected(dom, self));
    if (!is_element(self)) return JS_ThrowTypeError(ctx, "Reflected attribute requires an element");
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    const char *name = magic == R_HIDDEN ? "hidden" : magic == R_LANG ? "lang" : magic == R_DIR ? "dir" :
                       magic == R_TAB_INDEX ? "tabindex" : magic == R_CLASS_LIST ? "class" :
                       is_html(self, OS64_HTML_TAG_SCRIPT) ? "src" : "href";
    if (magic == R_CLASS_LIST) {
        if (!set) return d_class_list(dom, ctx, self);
        /* [PutForwards=value]: assigning the list sets the attribute. */
        DString text = {0};
        if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
        JSValue result = set_attribute(dom, ctx, self, "class", text.data, text.length, false);
        d_string_free(dom, &text);
        return result;
    }
    if (magic == R_DATASET) return d_dataset(dom, ctx, self);
    const os64_html_attr_t *attr = os64_html_attr(self, name);
    if (!set) {
        switch (magic) {
        case R_HIDDEN: return JS_NewBool(ctx, attr != NULL);
        case R_DIR: {
            /* An enumerated attribute: one of its keywords, lowercased, or "". */
            static const char *const keywords[] = {"ltr", "rtl", "auto"};
            for (unsigned i = 0; attr != NULL && i < 3; i++) {
                const char *k = keywords[i], *v = attr->value;
                size_t n = 0;
                while (k[n] != '\0' && (v[n] == k[n] || v[n] == k[n] - ('a' - 'A'))) n++;
                if (k[n] == '\0' && v[n] == '\0') return JS_NewString(ctx, k);
            }
            return JS_NewString(ctx, "");
        }
        case R_TAB_INDEX: {
            int32_t index;
            if (attr != NULL && parse_integer(attr->value, &index)) return JS_NewInt32(ctx, index);
            return JS_NewInt32(ctx, focusable(self) ? 0 : -1);
        }
        case R_URL: {
            /* A URL attribute reads resolved against the document's base,
             * and as written when it does not resolve. */
            if (attr == NULL) return JS_NewString(ctx, "");
            char *absolute = d_alloc(dom, OS64_DOM_URL_MAX);
            if (absolute == NULL) return d_error(ctx, "QuotaExceededError", "DOM address quota exceeded");
            JSValue result = os64_dom_resolve(dom->document, dom->url, attr->value, absolute, OS64_DOM_URL_MAX)
                ? JS_NewString(ctx, absolute) : JS_NewString(ctx, attr->value);
            d_free(dom, absolute);
            return result;
        }
        default: return JS_NewString(ctx, attr != NULL ? attr->value : "");
        }
    }
    if (magic == R_HIDDEN) {
        int on = JS_ToBool(ctx, value);
        if (on < 0) return JS_EXCEPTION;
        return set_attribute(dom, ctx, self, name, "", 0, !on);
    }
    if (magic == R_TAB_INDEX) {
        int32_t index;
        if (JS_ToInt32(ctx, &index, value) < 0) return JS_EXCEPTION;
        char digits[16];
        os64_snprintf(digits, sizeof(digits), "%d", index);
        return set_attribute(dom, ctx, self, name, digits, os64_strlen(digits), false);
    }
    DString text = {0};
    if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
    JSValue result = set_attribute(dom, ctx, self, name, text.data, text.length, false);
    d_string_free(dom, &text);
    return result;
}

/* ── classList: a DOMTokenList over the class attribute ─────────────── */

typedef struct { const char *at; size_t length; } Token;

static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

/* The attribute's tokens, in order and each once. *count tokens; false on
 * no memory (QuotaExceededError thrown). */
static bool tokens_of(os64_dom_t *dom, JSContext *ctx, const char *text, Token **out, size_t *count)
{
    size_t most = 0;
    for (const char *at = text; *at != '\0'; at++)
        if (!space(*at) && (at == text || space(at[-1]))) most++;
    *out = NULL;
    *count = 0;
    if (most == 0) return true;
    *out = d_alloc(dom, most * sizeof(**out));
    if (*out == NULL) { d_error(ctx, "QuotaExceededError", "DOM token quota exceeded"); return false; }
    for (const char *at = text; *at != '\0';) {
        while (space(*at)) at++;
        const char *end = at;
        while (*end != '\0' && !space(*end)) end++;
        if (end == at) break;
        bool seen = false;
        for (size_t i = 0; i < *count && !seen; i++)
            seen = (*out)[i].length == (size_t)(end - at) && os64_memcmp((*out)[i].at, at, (*out)[i].length) == 0;
        if (!seen) (*out)[(*count)++] = (Token){at, (size_t)(end - at)};
        at = end;
    }
    return true;
}

static size_t find_token(const Token *tokens, size_t count, const char *token, size_t length)
{
    for (size_t i = 0; i < count; i++)
        if (tokens[i].length == length && os64_memcmp(tokens[i].at, token, length) == 0) return i;
    return count;
}

/* The ordered set written back, one space between tokens. */
static JSValue write_tokens(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                            const Token *tokens, size_t count)
{
    size_t length = 0;
    for (size_t i = 0; i < count; i++) length += tokens[i].length + 1;
    char *text = d_alloc(dom, length + 1);
    if (text == NULL) return d_error(ctx, "QuotaExceededError", "DOM token quota exceeded");
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (i != 0) text[used++] = ' ';
        os64_memcpy(text + used, tokens[i].at, tokens[i].length);
        used += tokens[i].length;
    }
    text[used] = '\0';
    JSValue result = set_attribute(dom, ctx, node, "class", text, used, false);
    d_free(dom, text);
    return result;
}

/* A token a script hands in: empty is a SyntaxError, whitespace in it an
 * InvalidCharacterError (DOM § DOMTokenList). */
static bool token_argument(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DString *out)
{
    if (!d_string(dom, ctx, value, out)) return false;
    const char *problem = out->length == 0 ? "SyntaxError" : NULL;
    for (size_t i = 0; problem == NULL && i < out->length; i++)
        if (space(out->data[i])) problem = "InvalidCharacterError";
    if (problem == NULL) return true;
    d_string_free(dom, out);
    d_error(ctx, problem, problem[0] == 'S' ? "The token is empty" : "The token holds whitespace");
    return false;
}

enum { T_LENGTH, T_VALUE, T_ITEM, T_CONTAINS, T_ADD, T_REMOVE, T_TOGGLE, T_REPLACE, T_STRING };

static JSValue token_method(JSContext *ctx, JSValueConst this_value, int argc,
                            JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DValue *entry = JS_GetOpaque(this_value, dom->token_class);
    if (entry == NULL) return JS_ThrowTypeError(ctx, "Expected a DOMTokenList");
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    const os64_html_node_t *node = entry->target;
    const os64_html_attr_t *attr = os64_html_attr(node, "class");
    if (magic == T_VALUE || magic == T_STRING) {
        if (!set) return JS_NewString(ctx, attr != NULL ? attr->value : "");
        DString text = {0};
        if (!d_string(dom, ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &text)) return JS_EXCEPTION;
        JSValue result = set_attribute(dom, ctx, node, "class", text.data, text.length, false);
        d_string_free(dom, &text);
        return result;
    }
    /* Arguments are converted, and judged, before the list is read: a
     * conversion may run script that changes the attribute. */
    int wanted = magic == T_ADD || magic == T_REMOVE ? argc : magic == T_REPLACE ? 2 :
                 magic == T_ITEM || magic == T_LENGTH ? 0 : 1;
    if (magic != T_LENGTH && magic != T_ITEM && argc < wanted)
        return JS_ThrowTypeError(ctx, "Missing token");
    uint32_t index = 0;
    if (magic == T_ITEM && (argc < 1 || JS_ToUint32(ctx, &index, argv[0]) < 0))
        return argc < 1 ? JS_ThrowTypeError(ctx, "Missing index") : JS_EXCEPTION;
    DString *given = wanted > 0 ? d_alloc(dom, (size_t)wanted * sizeof(*given)) : NULL;
    if (wanted > 0 && given == NULL) return d_error(ctx, "QuotaExceededError", "DOM token quota exceeded");
    int converted = 0;
    bool ok = true;
    for (; ok && converted < wanted; converted++) {
        ok = magic == T_CONTAINS ? d_string(dom, ctx, argv[converted], &given[converted])
                                 : token_argument(dom, ctx, argv[converted], &given[converted]);
        if (!ok) break;
    }
    int force = magic == T_TOGGLE && argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToBool(ctx, argv[1]) : -1;
    attr = os64_html_attr(node, "class");
    Token *tokens = NULL;
    size_t count = 0;
    JSValue result = JS_EXCEPTION;
    if (ok && tokens_of(dom, ctx, attr != NULL ? attr->value : "", &tokens, &count)) {
        switch (magic) {
        case T_LENGTH: result = JS_NewUint32(ctx, (uint32_t)count); break;
        case T_ITEM:
            result = index < count ? JS_NewStringLen(ctx, tokens[index].at, tokens[index].length) : JS_NULL;
            break;
        case T_CONTAINS:
            result = JS_NewBool(ctx, find_token(tokens, count, given[0].data, given[0].length) < count);
            break;
        case T_ADD: case T_REMOVE: case T_TOGGLE: case T_REPLACE: {
            /* Room for every token added; removing writes the list again
             * even when nothing was in it, as the standard's update does
             * whenever the attribute exists. */
            Token *next = d_alloc(dom, (count + (size_t)wanted + 1) * sizeof(*next));
            if (next == NULL) { d_error(ctx, "QuotaExceededError", "DOM token quota exceeded"); break; }
            size_t n = 0;
            for (size_t i = 0; i < count; i++) next[n++] = tokens[i];
            bool present = false, changed = true;
            if (magic == T_ADD) {
                for (int a = 0; a < wanted; a++)
                    if (find_token(next, n, given[a].data, given[a].length) == n)
                        next[n++] = (Token){given[a].data, given[a].length};
            } else if (magic == T_REMOVE) {
                for (int a = 0; a < wanted; a++) {
                    size_t at = find_token(next, n, given[a].data, given[a].length);
                    if (at < n) { os64_memmove(next + at, next + at + 1, (n - at - 1) * sizeof(*next)); n--; }
                }
                changed = attr != NULL;
            } else if (magic == T_TOGGLE) {
                size_t at = find_token(next, n, given[0].data, given[0].length);
                present = at < n;
                if (present && force != 1) { os64_memmove(next + at, next + at + 1, (n - at - 1) * sizeof(*next)); n--; present = false; }
                else if (!present && force != 0) { next[n++] = (Token){given[0].data, given[0].length}; present = true; }
                else changed = false;
            } else {
                /* replace(old, new): the first of them keeps its place. */
                size_t at = find_token(next, n, given[0].data, given[0].length);
                present = at < n;
                changed = present;
                if (present) {
                    size_t other = find_token(next, n, given[1].data, given[1].length);
                    if (other < n && other != at) {
                        size_t keep = at < other ? at : other, drop = at < other ? other : at;
                        next[keep] = (Token){given[1].data, given[1].length};
                        os64_memmove(next + drop, next + drop + 1, (n - drop - 1) * sizeof(*next));
                        n--;
                    } else next[at] = (Token){given[1].data, given[1].length};
                }
            }
            /* An element with no class attribute gets none for an empty set. */
            result = changed && !(attr == NULL && n == 0) ? write_tokens(dom, ctx, node, next, n) : JS_UNDEFINED;
            if (!JS_IsException(result) && (magic == T_TOGGLE || magic == T_REPLACE))
                result = JS_NewBool(ctx, present);
            d_free(dom, next);
            break;
        }
        default: result = JS_ThrowTypeError(ctx, "Unknown DOMTokenList member"); break;
        }
    }
    d_free(dom, tokens);
    for (int i = 0; i < converted; i++) d_string_free(dom, &given[i]);
    d_free(dom, given);
    return result;
}

static DValue *token_receiver(JSContext *ctx, JSValueConst object)
{
    JSClassID id;
    DValue *entry = JS_GetAnyOpaque(object, &id);
    if (entry == NULL) { d_error(ctx, "InvalidStateError", "DOM token list is closed"); return NULL; }
    return d_context(entry->dom, ctx, OS64_JS_ABI_ID) ? entry : NULL;
}

/* The list's indices, read-only and live, as a collection's are. */
static int token_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom atom)
{
    DValue *entry = token_receiver(ctx, object);
    if (entry == NULL) return -1;
    uint32_t index;
    int numeric = d_index_key(ctx, atom, &index);
    if (numeric <= 0) return numeric;
    const os64_html_attr_t *attr = os64_html_attr(entry->target, "class");
    Token *tokens;
    size_t count;
    if (!tokens_of(entry->dom, ctx, attr != NULL ? attr->value : "", &tokens, &count)) return -1;
    int found = index < count;
    if (found && desc != NULL) {
        JSValue value = JS_NewStringLen(ctx, tokens[index].at, tokens[index].length);
        if (JS_IsException(value)) found = -1;
        else {
            desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
            desc->value = value;
            desc->getter = JS_UNDEFINED;
            desc->setter = JS_UNDEFINED;
        }
    }
    d_free(entry->dom, tokens);
    return found;
}

static int token_own_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object)
{
    DValue *entry = token_receiver(ctx, object);
    if (entry == NULL) return -1;
    const os64_html_attr_t *attr = os64_html_attr(entry->target, "class");
    Token *tokens;
    size_t count;
    if (!tokens_of(entry->dom, ctx, attr != NULL ? attr->value : "", &tokens, &count)) return -1;
    d_free(entry->dom, tokens);
    return d_index_names(ctx, count, table, length);
}

static int token_define(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value,
                        JSValueConst getter, JSValueConst setter, int flags)
{
    DValue *entry = token_receiver(ctx, object);
    if (entry == NULL) return -1;
    uint32_t index;
    int numeric = d_index_key(ctx, atom, &index);
    if (numeric < 0) return -1;
    return JS_DefineProperty(ctx, numeric ? entry->dom->index_guard->value : object, atom,
                             value, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

static int token_delete(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    DValue *entry = token_receiver(ctx, object);
    if (entry == NULL) return -1;
    uint32_t index;
    int numeric = d_index_key(ctx, atom, &index);
    if (numeric <= 0) return numeric < 0 ? -1 : 1;
    int present = token_own_property(ctx, NULL, object, atom);
    return present < 0 ? -1 : !present;
}

JSClassExoticMethods d_token_exotic = {
    .get_own_property = token_own_property, .get_own_property_names = token_own_names,
    .define_own_property = token_define, .delete_property = token_delete,
};

/* One view object per element, made the first time it is asked for. */
static JSValue element_view(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                            JSClassID class_id, bool tokens)
{
    DValue *owner = d_find(dom, node);
    if (owner == NULL) return JS_ThrowTypeError(ctx, "Missing node wrapper");
    DValue **slot = tokens ? &owner->class_list : &owner->dataset;
    if (*slot != NULL) return JS_DupValue(ctx, (*slot)->value);
    JSValue object = JS_NewObjectClass(ctx, class_id);
    DValue *entry = d_retain(dom, ctx, object);
    if (entry == NULL) return JS_EXCEPTION;
    entry->dom = dom;
    entry->target = node;
    os64_html_hold(dom->document, node);
    JS_SetOpaque(object, entry);
    *slot = entry;
    return JS_DupValue(ctx, object);
}

JSValue d_class_list(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    return element_view(dom, ctx, node, dom->token_class, true);
}

/* ── dataset: a DOMStringMap over the data-* attributes ─────────────── */

/* HTML § The dataset IDL attribute: `data-foo-bar` is `fooBar`. A name with
 * a hyphen before a lowercase letter names no attribute; any other is
 * `data-` and the name, each uppercase letter a hyphen and its lowercase.
 * The answer is d_alloc'd; NULL when the name names none (or no memory,
 * then with *refused set). */
static char *data_attribute(os64_dom_t *dom, const char *name, bool *refused)
{
    *refused = false;
    size_t length = 5;
    for (const char *at = name; *at != '\0'; at++) {
        if (at[0] == '-' && at[1] >= 'a' && at[1] <= 'z') return NULL;
        length += *at >= 'A' && *at <= 'Z' ? 2 : 1;
    }
    char *out = d_alloc(dom, length + 1);
    if (out == NULL) { *refused = true; return NULL; }
    os64_memcpy(out, "data-", 5);
    size_t used = 5;
    for (const char *at = name; *at != '\0'; at++) {
        if (*at >= 'A' && *at <= 'Z') { out[used++] = '-'; out[used++] = (char)(*at + ('a' - 'A')); }
        else out[used++] = *at;
    }
    out[used] = '\0';
    return out;
}

/* The map's name for a data-* attribute, or NULL when it has none (it is
 * not a data-* name, or has an uppercase letter). d_alloc'd. */
static char *data_name(os64_dom_t *dom, const char *attribute)
{
    if (os64_memcmp(attribute, "data-", 5) != 0 || os64_strlen(attribute) < 5) return NULL;
    for (const char *at = attribute; *at != '\0'; at++)
        if (*at >= 'A' && *at <= 'Z') return NULL;
    char *out = d_alloc(dom, os64_strlen(attribute) + 1);
    if (out == NULL) return NULL;
    size_t used = 0;
    for (const char *at = attribute + 5; *at != '\0'; at++) {
        if (at[0] == '-' && at[1] >= 'a' && at[1] <= 'z') { out[used++] = (char)(at[1] - ('a' - 'A')); at++; }
        else out[used++] = *at;
    }
    out[used] = '\0';
    return out;
}

static DValue *map_receiver(JSContext *ctx, JSValueConst object)
{
    JSClassID id;
    DValue *entry = JS_GetAnyOpaque(object, &id);
    if (entry == NULL) { d_error(ctx, "InvalidStateError", "DOM string map is closed"); return NULL; }
    return d_context(entry->dom, ctx, OS64_JS_ABI_ID) ? entry : NULL;
}

/* The attribute a string key names: 1 found (*name set, d_alloc'd), 0 a
 * key that names none (a Symbol, or one with `-x` in it), -1 no memory. */
static int map_key(os64_dom_t *dom, JSContext *ctx, JSAtom atom, char **name)
{
    *name = NULL;
    JSValue key = JS_AtomToValue(ctx, atom);
    if (JS_IsException(key)) return -1;
    bool symbol = JS_IsSymbol(key);
    JS_FreeValue(ctx, key);
    if (symbol) return 0;
    const char *text = JS_AtomToCString(ctx, atom);
    if (text == NULL) return -1;
    bool refused;
    *name = data_attribute(dom, text, &refused);
    JS_FreeCString(ctx, text);
    if (refused) { d_error(ctx, "QuotaExceededError", "DOM data name quota exceeded"); return -1; }
    return *name != NULL;
}

static int map_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom atom)
{
    DValue *entry = map_receiver(ctx, object);
    if (entry == NULL) return -1;
    char *name;
    int named = map_key(entry->dom, ctx, atom, &name);
    if (named <= 0) return named;
    const os64_html_attr_t *attr = os64_html_attr(entry->target, name);
    d_free(entry->dom, name);
    if (attr == NULL) return 0;
    if (desc != NULL) {
        JSValue value = JS_NewString(ctx, attr->value);
        if (JS_IsException(value)) return -1;
        desc->flags = JS_PROP_C_W_E;
        desc->value = value;
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

static int map_own_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object)
{
    DValue *entry = map_receiver(ctx, object);
    if (entry == NULL) return -1;
    uint32_t count = 0;
    for (const os64_html_attr_t *attr = entry->target->attrs; attr != NULL; attr = attr->next) count++;
    JSPropertyEnum *entries = count != 0 ? js_malloc(ctx, count * sizeof(*entries)) : NULL;
    if (count != 0 && entries == NULL) return -1;
    uint32_t used = 0;
    for (const os64_html_attr_t *attr = entry->target->attrs; attr != NULL; attr = attr->next) {
        if (attr->ns != NULL) continue;
        char *name = data_name(entry->dom, attr->name);
        if (name == NULL) continue;
        JSAtom atom = JS_NewAtom(ctx, name);
        d_free(entry->dom, name);
        if (atom == JS_ATOM_NULL) {
            while (used != 0) JS_FreeAtom(ctx, entries[--used].atom);
            js_free(ctx, entries);
            return -1;
        }
        entries[used].is_enumerable = true;
        entries[used++].atom = atom;
    }
    *table = entries;
    *length = used;
    return 0;
}

/* Setting a name sets its attribute: a name with `-x` in it is a
 * SyntaxError, one that makes no attribute name an InvalidCharacterError.
 * A Symbol, or a getter, is an ordinary own property. */
static int map_define(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value,
                      JSValueConst getter, JSValueConst setter, int flags)
{
    DValue *entry = map_receiver(ctx, object);
    if (entry == NULL) return -1;
    JSValue key = JS_AtomToValue(ctx, atom);
    if (JS_IsException(key)) return -1;
    bool symbol = JS_IsSymbol(key);
    JS_FreeValue(ctx, key);
    if (symbol || !(flags & JS_PROP_HAS_VALUE))
        return JS_DefineProperty(ctx, object, atom, value, getter, setter, flags | JS_PROP_NO_EXOTIC);
    char *name;
    int named = map_key(entry->dom, ctx, atom, &name);
    if (named < 0) return -1;
    if (named == 0) {
        d_error(ctx, "SyntaxError", "A dataset name may not hold a hyphen before a lowercase letter");
        return -1;
    }
    if (!d_attribute_name_valid(name)) {
        d_free(entry->dom, name);
        d_error(ctx, "InvalidCharacterError", "Invalid attribute name");
        return -1;
    }
    DString text = {0};
    int result = d_string(entry->dom, ctx, value, &text) ? 1 : -1;
    if (result > 0 && JS_IsException(set_attribute(entry->dom, ctx, entry->target, name, text.data, text.length, false)))
        result = -1;
    d_string_free(entry->dom, &text);
    d_free(entry->dom, name);
    return result;
}

static int map_delete(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    DValue *entry = map_receiver(ctx, object);
    if (entry == NULL) return -1;
    char *name;
    int named = map_key(entry->dom, ctx, atom, &name);
    if (named <= 0) return named < 0 ? -1 : 1;
    int result = 1;
    if (os64_html_attr(entry->target, name) != NULL &&
        JS_IsException(set_attribute(entry->dom, ctx, entry->target, name, NULL, 0, true)))
        result = -1;
    d_free(entry->dom, name);
    return result;
}

JSClassExoticMethods d_map_exotic = {
    .get_own_property = map_own_property, .get_own_property_names = map_own_names,
    .define_own_property = map_define, .delete_property = map_delete,
};

JSValue d_dataset(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    return element_view(dom, ctx, node, dom->map_class, false);
}

/* ── Installation ─────────────────────────────────────────────────────── */

#define PARENTS ((1u << D_PROTO_DOCUMENT) | (1u << D_PROTO_ELEMENT) | (1u << D_PROTO_FRAGMENT))
#define CHILDREN ((1u << D_PROTO_ELEMENT) | (1u << D_PROTO_CHARACTER_DATA))
#define ELEMENTS (1u << D_PROTO_ELEMENT)

int d_mixin_install(os64_dom_t *dom, JSContext *ctx)
{
    static const struct { const char *name; int magic, argc; unsigned on; } methods[] = {
        {"append", D_APPEND_NODES, 0, PARENTS}, {"prepend", D_PREPEND, 0, PARENTS},
        {"replaceChildren", D_REPLACE_CHILDREN, 0, PARENTS},
        {"before", D_BEFORE, 0, CHILDREN}, {"after", D_AFTER, 0, CHILDREN},
        {"replaceWith", D_REPLACE_WITH, 0, CHILDREN}, {"remove", D_REMOVE_SELF, 0, CHILDREN},
        {"contains", D_CONTAINS, 1, 1u << D_PROTO_NODE},
        {"getElementsByClassName", D_BY_CLASS, 1, (1u << D_PROTO_DOCUMENT) | ELEMENTS},
        {"getElementsByName", D_BY_NAME, 1, 1u << D_PROTO_DOCUMENT},
        {"matches", D_MATCHES, 1, ELEMENTS}, {"closest", D_CLOSEST, 1, ELEMENTS},
        {"insertAdjacentHTML", D_ADJACENT_HTML, 2, ELEMENTS},
        {"insertAdjacentElement", D_ADJACENT_ELEMENT, 2, ELEMENTS},
        {"insertAdjacentText", D_ADJACENT_TEXT, 2, ELEMENTS},
        {"getAttributeNames", D_ATTRIBUTE_NAMES, 0, ELEMENTS},
        {"toggleAttribute", D_TOGGLE_ATTR, 1, ELEMENTS}, {"hasAttributes", D_HAS_ATTRS, 0, ELEMENTS},
    };
    static const struct { const char *name; int magic; bool writable; unsigned on; } reflections[] = {
        {"isConnected", R_CONNECTED, false, 1u << D_PROTO_NODE},
        {"hidden", R_HIDDEN, true, ELEMENTS}, {"lang", R_LANG, true, ELEMENTS},
        {"dir", R_DIR, true, ELEMENTS}, {"tabIndex", R_TAB_INDEX, true, ELEMENTS},
        {"classList", R_CLASS_LIST, true, ELEMENTS}, {"dataset", R_DATASET, false, ELEMENTS},
        {"src", R_URL, true, 1u << D_PROTO_SCRIPT},
        {"href", R_URL, true, (1u << D_PROTO_ANCHOR) | (1u << D_PROTO_LINK)},
    };
    for (int kind = 0; kind < D_PROTO_COUNT; kind++) {
        JSValueConst prototype = dom->prototypes[kind]->value;
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
            if ((methods[i].on & (1u << kind)) &&
                d_method(dom, ctx, prototype, methods[i].name, mixin_method, methods[i].argc, methods[i].magic) < 0)
                return -1;
        for (size_t i = 0; i < sizeof(reflections) / sizeof(reflections[0]); i++)
            if ((reflections[i].on & (1u << kind)) &&
                d_accessor(dom, ctx, prototype, reflections[i].name, reflect, reflections[i].magic,
                           reflections[i].writable) < 0)
                return -1;
    }
    static const struct { const char *name; int magic, argc; bool accessor, writable; } members[] = {
        {"length", T_LENGTH, 0, true, false}, {"value", T_VALUE, 0, true, true},
        {"item", T_ITEM, 1, false, false}, {"contains", T_CONTAINS, 1, false, false},
        {"add", T_ADD, 0, false, false}, {"remove", T_REMOVE, 0, false, false},
        {"toggle", T_TOGGLE, 1, false, false}, {"replace", T_REPLACE, 2, false, false},
        {"toString", T_STRING, 0, false, false},
    };
    JSValue tokens = JS_NewObject(ctx);
    DValue *token_prototype = d_retain(dom, ctx, tokens);
    if (token_prototype == NULL) return -1;
    JS_SetClassProto(ctx, dom->token_class, JS_DupValue(ctx, tokens));
    for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++)
        if ((members[i].accessor
                ? d_accessor(dom, ctx, tokens, members[i].name, token_method, members[i].magic, members[i].writable)
                : d_method(dom, ctx, tokens, members[i].name, token_method, members[i].argc, members[i].magic)) < 0)
            return -1;
    return d_iterable(ctx, tokens, true);
}
