#include "internal.h"
#include "os64/fmt.h"

/* THE SELECT AND ITS OPTIONS (0f's P1a, INTERFACES_FORMS.md; HTML § 4.10.7
 * the select element, § 4.10.9 optgroup, § 4.10.10 option, § 2.6.2.2
 * HTMLOptionsCollection). React's option updater, which reads each option's
 * `value` and sets its `selected`, is the first page that needed them. An
 * option's selectedness is libpage's control state
 * (os64_page_node_option_selected) and a select's list of options is
 * libpage's walk (os64_page_option_next); the rest is read off the tree.
 * One row of the Modern Web table: it names the `Option` factory on window
 * and puts everything else on the prototypes. */

enum {
    O_VALUE, O_TEXT, O_LABEL, O_SELECTED, O_DEFAULT_SELECTED, O_DISABLED, O_INDEX, O_FORM,
    G_DISABLED, G_LABEL,
    S_OPTIONS, S_LENGTH, S_SELECTED_OPTIONS, S_MULTIPLE, S_SIZE, S_ITEM, S_NAMED_ITEM, S_ADD, S_REMOVE,
    C_LENGTH, C_SELECTED_INDEX, C_ADD, C_REMOVE
};

/* The most options a `length` assignment appends at once, as browsers
 * bound it (HTML lets a user agent set the limit). */
#define OPTIONS_APPEND_MAX 100000u

static bool is_html_tag(const os64_html_node_t *n, os64_html_tag_t tag)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML && n->tag == tag;
}

static bool ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* The next node of `root`'s descendants after `at`, a script's subtree
 * (HTML's or SVG's) skipped. */
static const os64_html_node_t *text_step(const os64_html_node_t *root, const os64_html_node_t *at)
{
    bool script = at->kind == OS64_HTML_ELEMENT && (at->ns == OS64_HTML_NS_HTML || at->ns == OS64_HTML_NS_SVG) &&
                  at->name != NULL && os64_strcmp(at->name, "script") == 0;
    if (!script && at->first_child != NULL) return at->first_child;
    while (at != root && at->next == NULL) at = at->parent;
    return at != root ? at->next : NULL;
}

/* An option's text (HTML § 4.10.10, its `text`): its descendant text, a
 * script's left out, with ASCII whitespace stripped and collapsed. A new
 * string, or JS_EXCEPTION. */
static JSValue option_text(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *option)
{
    size_t total = 0;
    for (const os64_html_node_t *n = option->first_child; n != NULL; n = text_step(option, n))
        if (n->kind == OS64_HTML_TEXT) total += n->text_len;
    char *out = d_alloc(dom, total + 1);
    if (out == NULL) return d_error(ctx, "QuotaExceededError", "DOM text quota exceeded");
    size_t used = 0;
    bool space = false;
    for (const os64_html_node_t *n = option->first_child; n != NULL; n = text_step(option, n)) {
        if (n->kind != OS64_HTML_TEXT) continue;
        for (size_t i = 0; i < n->text_len; i++) {
            if (ascii_space(n->text[i])) { space = used != 0; continue; }
            if (space) out[used++] = ' ';
            space = false;
            out[used++] = n->text[i];
        }
    }
    JSValue text = JS_NewStringLen(ctx, out, used);
    d_free(dom, out);
    return text;
}

/* Where `option` is in `select`'s list of options, -1 when not there. */
static int32_t option_index(const os64_html_node_t *select, const os64_html_node_t *option)
{
    int32_t i = 0;
    for (const os64_html_node_t *at = os64_page_option_next(select, NULL); at != NULL;
         at = os64_page_option_next(select, at), i++)
        if (at == option) return i;
    return -1;
}

static const os64_html_node_t *option_at(const os64_html_node_t *select, uint32_t index)
{
    const os64_html_node_t *at = os64_page_option_next(select, NULL);
    for (uint32_t i = 0; at != NULL && i < index; i++) at = os64_page_option_next(select, at);
    return at;
}

static uint32_t option_count(const os64_html_node_t *select)
{
    uint32_t count = 0;
    for (const os64_html_node_t *at = os64_page_option_next(select, NULL); at != NULL;
         at = os64_page_option_next(select, at))
        count++;
    return count;
}

static JSValue attr_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, const char *name,
                        JSValueConst value)
{
    DString text = {0};
    if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
    int64_t status = d_set_attr(dom, node, name, text.data != NULL ? text.data : "", text.length, false);
    d_string_free(dom, &text);
    return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
}

static JSValue flag_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, const char *name,
                        JSValueConst value)
{
    int on = JS_ToBool(ctx, value);
    if (on < 0) return JS_EXCEPTION;
    int64_t status = d_set_attr(dom, node, name, "", 0, !on);
    return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
}

static JSValue page_error(JSContext *ctx, int64_t status)
{
    return status == -OS64_PAGE_REASON_NO_MEMORY ? d_error(ctx, "QuotaExceededError", "DOM state quota exceeded")
                                                 : JS_ThrowTypeError(ctx, "The node is not an option of this page");
}

/* ── HTMLOptionElement and HTMLOptGroupElement ────────────────────────── */

static JSValue option_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv,
                             int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    const os64_html_node_t *self = d_node(dom, ctx, this_value);
    if (self == NULL) return JS_EXCEPTION;
    bool group = magic == G_DISABLED || magic == G_LABEL;
    if (!is_html_tag(self, group ? OS64_HTML_TAG_OPTGROUP : OS64_HTML_TAG_OPTION))
        return JS_ThrowTypeError(ctx, group ? "Expected an optgroup" : "Expected an option");
    const os64_html_attr_t *attr;
    switch (magic) {
    case O_VALUE:
        if (set) return attr_set(dom, ctx, self, "value", value);
        attr = os64_html_attr(self, "value");
        return attr != NULL ? JS_NewString(ctx, attr->value) : option_text(dom, ctx, self);
    case O_TEXT:
        if (set) return d_content_set(dom, ctx, self, D_TEXT_CONTENT, value);
        return option_text(dom, ctx, self);
    case O_LABEL: case G_LABEL:
        if (set) return attr_set(dom, ctx, self, "label", value);
        attr = os64_html_attr(self, "label");
        if (attr != NULL || magic == G_LABEL) return JS_NewString(ctx, attr != NULL ? attr->value : "");
        return option_text(dom, ctx, self);
    case O_SELECTED: {
        if (set) {
            int on = JS_ToBool(ctx, value);
            if (on < 0) return JS_EXCEPTION;
            int64_t status = os64_page_node_set_option_selected(dom->state, self, on != 0);
            return status < 0 ? page_error(ctx, status) : JS_UNDEFINED;
        }
        bool on = false;
        int64_t status = os64_page_node_option_selected(dom->state, self, &on);
        return status < 0 ? page_error(ctx, status) : JS_NewBool(ctx, on);
    }
    case O_DEFAULT_SELECTED:
        if (set) return flag_set(dom, ctx, self, "selected", value);
        return JS_NewBool(ctx, os64_html_attr(self, "selected") != NULL);
    case O_DISABLED: case G_DISABLED:
        if (set) return flag_set(dom, ctx, self, "disabled", value);
        return JS_NewBool(ctx, os64_html_attr(self, "disabled") != NULL);
    case O_INDEX: {
        const os64_html_node_t *select = os64_page_option_select(self);
        return JS_NewInt32(ctx, select != NULL ? option_index(select, self) : 0);
    }
    case O_FORM: {
        const os64_html_node_t *select = os64_page_option_select(self);
        const os64_html_node_t *form = select != NULL ? d_form_owner(select) : NULL;
        return form != NULL ? d_wrap(dom, ctx, form) : JS_NULL;
    }
    default: return JS_ThrowTypeError(ctx, "Unknown option member");
    }
}

/* ── The select's members and its options collection's ────────────────── */

/* The select's options ([SameObject]), a live collection over its list of
 * options with HTMLOptionsCollection's prototype, or its selectedOptions,
 * a live collection with HTMLCollection's. */
static JSValue select_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *select, bool selected)
{
    DValue *entry = d_find(dom, select);
    if (entry == NULL) return JS_ThrowTypeError(ctx, "Missing node wrapper");
    DQuery **slot = selected ? &entry->selected_options : &entry->options;
    if (*slot != NULL) return JS_DupValue(ctx, (*slot)->entry->value);
    JSValue value = d_special_collection(dom, ctx, select, selected ? D_QUERY_SELECTED_OPTIONS : D_QUERY_OPTIONS,
                                         NULL);
    if (JS_IsException(value)) return value;
    if (!selected && JS_SetPrototype(ctx, value, dom->options_prototype->value) < 0) {
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    *slot = JS_GetOpaque(value, dom->collection_class);
    return value;
}

/* add(element, before) (HTML § 2.6.2.2): an option or optgroup put into the
 * select before `before`, an element of it or the option at an index (none:
 * last). Both arguments are converted before the tree is read. */
static JSValue options_add(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *select, int argc,
                           JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "Missing option");
    const os64_html_node_t *element = d_node(dom, ctx, argv[0]);
    if (element == NULL) return JS_EXCEPTION;
    if (!is_html_tag(element, OS64_HTML_TAG_OPTION) && !is_html_tag(element, OS64_HTML_TAG_OPTGROUP))
        return JS_ThrowTypeError(ctx, "Expected an option or an optgroup");
    const os64_html_node_t *before = NULL;
    bool indexed = false;
    int32_t index = 0;
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        /* (HTMLElement or long): an HTML element is one, anything else is
         * converted to a number, a node of another kind included. */
        if (JS_IsObject(argv[1]) && JS_GetOpaque(argv[1], dom->node_class) != NULL) {
            before = d_node(dom, ctx, argv[1]);
            if (before == NULL) return JS_EXCEPTION;
            if (before->kind != OS64_HTML_ELEMENT || before->ns != OS64_HTML_NS_HTML) before = NULL;
        }
        if (before == NULL) {
            if (JS_ToInt32(ctx, &index, argv[1]) < 0) return JS_EXCEPTION;
            indexed = true;
        }
    }
    for (const os64_html_node_t *at = select; at != NULL; at = at->parent)
        if (at == element) return d_error(ctx, "HierarchyRequestError", "The option holds the select");
    if (before != NULL) {
        const os64_html_node_t *at = before;
        while (at != NULL && at != select) at = at->parent;
        if (at == NULL || before == select) return d_error(ctx, "NotFoundError", "The node is not in the select");
        if (before == element) return JS_UNDEFINED;
    }
    if (indexed) before = index >= 0 ? option_at(select, (uint32_t)index) : NULL;
    const os64_html_node_t *parent = before != NULL ? before->parent : select;
    return d_tree_change(dom, ctx, D_INSERT, parent, element, before) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* remove(index): the option at `index`, if there is one, taken out. */
static JSValue options_remove(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *select, JSValueConst value)
{
    int32_t index;
    if (JS_ToInt32(ctx, &index, value) < 0) return JS_EXCEPTION;
    const os64_html_node_t *option = index >= 0 ? option_at(select, (uint32_t)index) : NULL;
    if (option == NULL || option->parent == NULL) return JS_UNDEFINED;
    return d_tree_change(dom, ctx, D_REMOVE, option->parent, option, NULL) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* The collection's `length` set: options taken off the end, or empty ones
 * appended to the select (OPTIONS_APPEND_MAX at most at once). */
static JSValue options_length_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *select, JSValueConst value)
{
    uint32_t wanted;
    if (JS_ToUint32(ctx, &wanted, value) < 0) return JS_EXCEPTION;
    uint32_t count = option_count(select);
    while (count > wanted) {
        const os64_html_node_t *last = option_at(select, count - 1);
        if (last->parent != NULL && d_tree_change(dom, ctx, D_REMOVE, last->parent, last, NULL) < 0)
            return JS_EXCEPTION;
        count--;
    }
    if (wanted - count > OPTIONS_APPEND_MAX) return JS_UNDEFINED;
    for (; count < wanted; count++) {
        int64_t status;
        os64_html_node_t *option = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, "option", &status);
        if (option == NULL) return d_html_error(ctx, status);
        d_made_in(dom, option, d_node_document(dom, select));
        if (d_tree_change(dom, ctx, D_APPEND, select, option, NULL) < 0) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

int d_options_set_index(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *select, uint32_t index,
                        JSValueConst value)
{
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        const os64_html_node_t *option = option_at(select, index);
        if (option == NULL || option->parent == NULL) return 0;
        return d_tree_change(dom, ctx, D_REMOVE, option->parent, option, NULL);
    }
    const os64_html_node_t *option = JS_IsObject(value) && JS_GetOpaque(value, dom->node_class) != NULL
        ? d_node(dom, ctx, value) : NULL;
    if (option == NULL || !is_html_tag(option, OS64_HTML_TAG_OPTION)) {
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "Expected an option or null");
        return -1;
    }
    uint32_t count = option_count(select);
    if (index >= count) {
        if (index - count > OPTIONS_APPEND_MAX) return 0;
        for (; count < index; count++) {
            int64_t status;
            os64_html_node_t *empty = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, "option", &status);
            if (empty == NULL) { d_html_error(ctx, status); return -1; }
            d_made_in(dom, empty, d_node_document(dom, select));
            if (d_tree_change(dom, ctx, D_APPEND, select, empty, NULL) < 0) return -1;
        }
        return d_tree_change(dom, ctx, D_APPEND, select, option, NULL);
    }
    const os64_html_node_t *old = option_at(select, index);
    if (old == option) return 0;
    return d_tree_change(dom, ctx, D_REPLACE, old->parent, option, old);
}

static JSValue select_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv,
                             int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    const os64_html_node_t *select;
    if (magic >= C_LENGTH) {
        /* The options collection's own: its select is the query's root. */
        DQuery *query = JS_GetOpaque(this_value, dom->collection_class);
        if (query == NULL || query->kind != D_QUERY_OPTIONS)
            return JS_ThrowTypeError(ctx, "Expected an HTMLOptionsCollection");
        select = query->root;
    } else {
        select = d_node(dom, ctx, this_value);
        if (select == NULL) return JS_EXCEPTION;
        if (!is_html_tag(select, OS64_HTML_TAG_SELECT)) return JS_ThrowTypeError(ctx, "Expected a select");
    }
    switch (magic) {
    case S_OPTIONS: return select_collection(dom, ctx, select, false);
    case S_SELECTED_OPTIONS: return select_collection(dom, ctx, select, true);
    case S_LENGTH: case C_LENGTH:
        if (set) return options_length_set(dom, ctx, select, value);
        return JS_NewUint32(ctx, option_count(select));
    case S_MULTIPLE:
        if (set) return flag_set(dom, ctx, select, "multiple", value);
        return JS_NewBool(ctx, os64_html_attr(select, "multiple") != NULL);
    case S_SIZE: {
        if (set) {
            uint32_t size;
            if (JS_ToUint32(ctx, &size, value) < 0) return JS_EXCEPTION;
            char digits[16];
            os64_snprintf(digits, sizeof(digits), "%u", size <= 2147483647u ? size : 0u);
            int64_t status = d_set_attr(dom, select, "size", digits, os64_strlen(digits), false);
            return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
        }
        /* An unsigned reflection: the attribute's non-negative integer, 0
         * when it is absent or not one. */
        const os64_html_attr_t *attr = os64_html_attr(select, "size");
        const char *at = attr != NULL ? attr->value : "";
        while (ascii_space(*at)) at++;
        if (*at == '+') at++;
        uint64_t size = 0;
        bool digits = *at >= '0' && *at <= '9';
        while (*at >= '0' && *at <= '9' && size <= 2147483647u) size = size * 10 + (uint64_t)(*at++ - '0');
        return JS_NewUint32(ctx, digits && size <= 2147483647u ? (uint32_t)size : 0);
    }
    case S_ITEM: {
        uint32_t index;
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing index");
        if (JS_ToUint32(ctx, &index, value) < 0) return JS_EXCEPTION;
        const os64_html_node_t *option = option_at(select, index);
        return option != NULL ? d_wrap(dom, ctx, option) : JS_NULL;
    }
    case S_NAMED_ITEM: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing name");
        DString name = {0};
        if (!d_string(dom, ctx, value, &name)) return JS_EXCEPTION;
        JSValue result = JS_NULL;
        for (const os64_html_node_t *at = os64_page_option_next(select, NULL); at != NULL && name.length != 0;
             at = os64_page_option_next(select, at))
            if (d_named_match(at, name.data)) { result = d_wrap(dom, ctx, at); break; }
        d_string_free(dom, &name);
        return result;
    }
    case S_ADD: case C_ADD: return options_add(dom, ctx, select, argc, argv);
    case S_REMOVE:
        /* With no argument it is ChildNode's remove(), which select's own
         * remove(index) shadows. */
        if (argc == 0) {
            if (select->parent == NULL) return JS_UNDEFINED;
            return d_tree_change(dom, ctx, D_REMOVE, select->parent, select, NULL) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
        }
        return options_remove(dom, ctx, select, value);
    case C_REMOVE:
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing index");
        return options_remove(dom, ctx, select, value);
    case C_SELECTED_INDEX: {
        if (set) {
            int32_t index;
            if (JS_ToInt32(ctx, &index, value) < 0) return JS_EXCEPTION;
            int64_t status = os64_page_node_set_selected_index(dom->state, select, index);
            return status < 0 ? page_error(ctx, status) : JS_UNDEFINED;
        }
        int32_t index = -1;
        int64_t status = os64_page_node_selected_index(dom->state, select, &index);
        return status < 0 ? page_error(ctx, status) : JS_NewInt32(ctx, index);
    }
    default: return JS_ThrowTypeError(ctx, "Unknown select member");
    }
}

/* ── The Option factory (HTML § 4.10.10, a legacy factory function) ───── */

/* `new Option(text, value, defaultSelected, selected)`: an option of the
 * window's document with `text` as its child, `value` as its attribute
 * when given, the `selected` attribute when defaultSelected, and
 * `selected` as its selectedness. Its prototype is new.target's, which
 * for Option itself is HTMLOptionElement's. Every argument is converted
 * before the option is made. */
static JSValue option_factory(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return JS_EXCEPTION;
    DString text = {0}, value = {0};
    bool has_value = argc > 1 && !JS_IsUndefined(argv[1]);
    int default_selected = 0, selected = 0;
    bool ok = (argc < 1 || JS_IsUndefined(argv[0]) || d_string(dom, ctx, argv[0], &text)) &&
              (!has_value || d_string(dom, ctx, argv[1], &value));
    if (ok && argc > 2) ok = (default_selected = JS_ToBool(ctx, argv[2])) >= 0;
    if (ok && argc > 3) ok = (selected = JS_ToBool(ctx, argv[3])) >= 0;
    JSValue result = JS_EXCEPTION;
    if (ok) {
        int64_t status = 0;
        os64_html_node_t *option = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, "option", &status);
        os64_html_node_t *child = option != NULL && text.length != 0
            ? os64_html_create_text(dom->document, text.data, text.length, &status) : NULL;
        if (option != NULL && (text.length == 0 || child != NULL)) {
            if (child != NULL) status = d_insert(dom, option, child, NULL);
            if (status >= 0 && has_value)
                status = d_set_attr(dom, option, "value", value.data != NULL ? value.data : "", value.length, false);
            if (status >= 0 && default_selected) status = d_set_attr(dom, option, "selected", "", 0, false);
            if (status >= 0) {
                int64_t page = os64_page_node_set_option_selected(dom->state, option, selected != 0);
                result = page < 0 ? page_error(ctx, page) : d_wrap_constructed(dom, ctx, option, prototype);
            } else
                result = d_html_error(ctx, status);
        } else
            result = d_html_error(ctx, status);
    }
    d_string_free(dom, &text);
    d_string_free(dom, &value);
    JS_FreeValue(ctx, prototype);
    return result;
}

int d_options_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const struct { const char *name; int magic; bool writable; } kOption[] = {
        {"value", O_VALUE, true}, {"text", O_TEXT, true}, {"label", O_LABEL, true},
        {"selected", O_SELECTED, true}, {"defaultSelected", O_DEFAULT_SELECTED, true},
        {"disabled", O_DISABLED, true}, {"index", O_INDEX, false}, {"form", O_FORM, false},
    };
    JSValueConst option = dom->prototypes[D_PROTO_OPTION]->value;
    JSValueConst group = dom->prototypes[D_PROTO_OPTGROUP]->value;
    JSValueConst select = dom->prototypes[D_PROTO_SELECT]->value;
    for (size_t i = 0; i < sizeof(kOption) / sizeof(kOption[0]); i++)
        if (d_accessor(dom, ctx, option, kOption[i].name, option_member, kOption[i].magic, kOption[i].writable) < 0)
            return -1;
    if (d_accessor(dom, ctx, group, "disabled", option_member, G_DISABLED, true) < 0 ||
        d_accessor(dom, ctx, group, "label", option_member, G_LABEL, true) < 0)
        return -1;
    static const struct { const char *name; int magic; bool writable; } kSelect[] = {
        {"options", S_OPTIONS, false}, {"length", S_LENGTH, true}, {"selectedOptions", S_SELECTED_OPTIONS, false},
        {"multiple", S_MULTIPLE, true}, {"size", S_SIZE, true},
    };
    static const struct { const char *name; int magic, argc; } kSelectMethods[] = {
        {"item", S_ITEM, 1}, {"namedItem", S_NAMED_ITEM, 1}, {"add", S_ADD, 1}, {"remove", S_REMOVE, 0},
    };
    for (size_t i = 0; i < sizeof(kSelect) / sizeof(kSelect[0]); i++)
        if (d_accessor(dom, ctx, select, kSelect[i].name, select_member, kSelect[i].magic, kSelect[i].writable) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kSelectMethods) / sizeof(kSelectMethods[0]); i++)
        if (d_method(dom, ctx, select, kSelectMethods[i].name, select_member, kSelectMethods[i].argc,
                     kSelectMethods[i].magic) < 0)
            return -1;
    /* HTMLOptionsCollection.prototype, over a collection's; its interface
     * object waits for HTMLCollection's (INTERFACES_FORMS.md P1b), which it
     * would inherit. */
    JSValue collection = JS_GetClassProto(ctx, dom->collection_class);
    JSValue options = JS_NewObjectProto(ctx, collection);
    JS_FreeValue(ctx, collection);
    if (JS_IsException(options)) return -1;
    dom->options_prototype = d_retain(dom, ctx, options);
    if (dom->options_prototype == NULL) return -1;
    if (d_accessor(dom, ctx, options, "length", select_member, C_LENGTH, true) < 0 ||
        d_accessor(dom, ctx, options, "selectedIndex", select_member, C_SELECTED_INDEX, true) < 0 ||
        d_method(dom, ctx, options, "add", select_member, 1, C_ADD) < 0 ||
        d_method(dom, ctx, options, "remove", select_member, 1, C_REMOVE) < 0 ||
        d_string_tag(ctx, options, "HTMLOptionsCollection") < 0)
        return -1;
    /* Option: its `prototype` is HTMLOptionElement's, whose `constructor`
     * stays HTMLOptionElement (WebIDL § 3.7.2, a legacy factory function). */
    JSValue factory = JS_NewCFunction2(ctx, option_factory, "Option", 0, JS_CFUNC_constructor, 0);
    if (JS_IsException(factory)) return -1;
    if (JS_DefinePropertyValueStr(ctx, factory, "prototype", JS_DupValue(ctx, option), 0) < 0) {
        JS_FreeValue(ctx, factory);
        return -1;
    }
    return JS_DefinePropertyValueStr(ctx, global, "Option", factory, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0
        ? -1 : 0;
}
