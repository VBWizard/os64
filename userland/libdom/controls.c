#include "internal.h"
#include "os64/fmt.h"

/* THE CONTROLS' REFLECTIONS AND STATE (0f's P3a; INTERFACES_FORMS.md § P3).
 *
 * Most of what input, textarea, select and button add is a content
 * attribute reflected as an IDL attribute, by one of HTML's few rules
 * (§ 2.6.1): a string; a URL, read resolved; a boolean, present or not; an
 * enumerated one, read as its canonical keyword or a default; and the
 * integers, each with its own default and its own answer to a value out of
 * range. kReflections is those rows, one per member and interface, and one
 * accessor reads them all.
 *
 * Beside them, the members that are not a plain attribute: textarea's
 * defaultValue (its child text) and textLength; input's indeterminate (a
 * flag with no attribute, libpage's, which yonder's checkbox draws), its
 * autocomplete and its width and height; and each control's form (its form
 * owner) and labels.
 *
 * One row of the Modern Web table, naming nothing of window's. */

enum { K_STRING, K_URL, K_FORM_ACTION, K_BOOLEAN, K_ENUM, K_LIMITED_LONG, K_UNSIGNED, K_LIMITED_UNSIGNED,
       K_UNSIGNED_FALLBACK };

#define IN (1u << D_PROTO_INPUT)
#define TA (1u << D_PROTO_TEXTAREA)
#define SE (1u << D_PROTO_SELECT)
#define BU (1u << D_PROTO_BUTTON)

static const char *const kEnctypes[] = {"application/x-www-form-urlencoded", "multipart/form-data", "text/plain",
                                        NULL};
/* formmethod's keywords are a form's method's, on an input as on a button. */
static const char *const kMethods[] = {"get", "post", "dialog", NULL};

typedef struct {
    const char *property, *attribute;
    uint8_t kind;
    unsigned on;
    /* An enumerated attribute's keywords, and what it reads as missing and
     * as a value that is none of them. */
    const char *const *keywords;
    const char *missing, *invalid;
    /* An integer's default. */
    uint32_t number;
} DReflection;

static const DReflection kReflections[] = {
    {"accept", "accept", K_STRING, IN, NULL, NULL, NULL, 0},
    {"alt", "alt", K_STRING, IN, NULL, NULL, NULL, 0},
    {"defaultChecked", "checked", K_BOOLEAN, IN, NULL, NULL, NULL, 0},
    {"defaultValue", "value", K_STRING, IN, NULL, NULL, NULL, 0},
    {"dirName", "dirname", K_STRING, IN | TA, NULL, NULL, NULL, 0},
    {"disabled", "disabled", K_BOOLEAN, IN | TA | SE | BU, NULL, NULL, NULL, 0},
    {"formAction", "formaction", K_FORM_ACTION, IN | BU, NULL, NULL, NULL, 0},
    {"formEnctype", "formenctype", K_ENUM, IN | BU, kEnctypes, "", "application/x-www-form-urlencoded", 0},
    {"formMethod", "formmethod", K_ENUM, IN | BU, kMethods, "", "get", 0},
    {"formNoValidate", "formnovalidate", K_BOOLEAN, IN | BU, NULL, NULL, NULL, 0},
    {"formTarget", "formtarget", K_STRING, IN | BU, NULL, NULL, NULL, 0},
    {"max", "max", K_STRING, IN, NULL, NULL, NULL, 0},
    {"maxLength", "maxlength", K_LIMITED_LONG, IN | TA, NULL, NULL, NULL, 0},
    {"min", "min", K_STRING, IN, NULL, NULL, NULL, 0},
    {"minLength", "minlength", K_LIMITED_LONG, IN | TA, NULL, NULL, NULL, 0},
    {"multiple", "multiple", K_BOOLEAN, IN, NULL, NULL, NULL, 0},
    {"pattern", "pattern", K_STRING, IN, NULL, NULL, NULL, 0},
    {"placeholder", "placeholder", K_STRING, IN | TA, NULL, NULL, NULL, 0},
    {"readOnly", "readonly", K_BOOLEAN, IN | TA, NULL, NULL, NULL, 0},
    {"required", "required", K_BOOLEAN, IN | TA | SE, NULL, NULL, NULL, 0},
    {"size", "size", K_LIMITED_UNSIGNED, IN, NULL, NULL, NULL, 20},
    {"src", "src", K_URL, IN, NULL, NULL, NULL, 0},
    {"step", "step", K_STRING, IN, NULL, NULL, NULL, 0},
    {"cols", "cols", K_UNSIGNED_FALLBACK, TA, NULL, NULL, NULL, 20},
    {"rows", "rows", K_UNSIGNED_FALLBACK, TA, NULL, NULL, NULL, 2},
    {"wrap", "wrap", K_STRING, TA, NULL, NULL, NULL, 0},
    /* Obsolete, and still read. */
    {"align", "align", K_STRING, IN, NULL, NULL, NULL, 0},
    {"useMap", "usemap", K_STRING, IN, NULL, NULL, NULL, 0},
};

/* The members that are no plain reflection, numbered after the rows. */
enum { M_TEXT_DEFAULT = 256, M_TEXT_LENGTH, M_INDETERMINATE, M_AUTOCOMPLETE, M_WIDTH, M_HEIGHT, M_FORM, M_LABELS };

static bool html(const os64_html_node_t *node, os64_html_tag_t tag)
{
    return node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML && node->tag == tag;
}

/* The interfaces of `kReflections`' masks a node is one of: a member
 * called on anything else is a TypeError, as WebIDL's brand check is. */
static unsigned control_kind(const os64_html_node_t *node)
{
    return html(node, OS64_HTML_TAG_INPUT) ? IN : html(node, OS64_HTML_TAG_TEXTAREA) ? TA :
           html(node, OS64_HTML_TAG_SELECT) ? SE : html(node, OS64_HTML_TAG_BUTTON) ? BU : 0;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c; }

static bool same_folded(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

/* HTML § 2.3.4.2, the rules for parsing non-negative integers: ASCII
 * whitespace, a sign, digits. A value past INT32_MAX is answered as
 * INT32_MAX + 1, which every reader takes for out of range. -1 when there
 * is no integer or it is negative ("-0" is 0). */
static int64_t non_negative(const char *text)
{
    while (is_space(*text)) text++;
    bool negative = *text == '-';
    if (*text == '-' || *text == '+') text++;
    if (*text < '0' || *text > '9') return -1;
    int64_t value = 0;
    while (*text >= '0' && *text <= '9') {
        if (value <= INT32_MAX) value = value * 10 + (*text - '0');
        text++;
    }
    if (value > INT32_MAX) value = (int64_t)INT32_MAX + 1;
    return negative && value != 0 ? -1 : value;
}

static JSValue set_number(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, const char *name,
                          uint32_t value)
{
    char digits[16];
    os64_snprintf(digits, sizeof(digits), "%u", value);
    int64_t status = d_set_attr(dom, node, name, digits, os64_strlen(digits), false);
    return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
}

/* A URL attribute read resolved against the document's base, and as
 * written when it does not resolve. */
static JSValue url_value(os64_dom_t *dom, JSContext *ctx, const char *text)
{
    char *absolute = d_alloc(dom, OS64_DOM_URL_MAX);
    if (absolute == NULL) return d_error(ctx, "QuotaExceededError", "DOM address quota exceeded");
    JSValue result = os64_dom_resolve(dom->document, dom->url, text, absolute, OS64_DOM_URL_MAX)
        ? JS_NewString(ctx, absolute) : JS_NewString(ctx, text);
    d_free(dom, absolute);
    return result;
}

static JSValue reflection_get(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                              const DReflection *row)
{
    const os64_html_attr_t *attr = os64_html_attr(node, row->attribute);
    switch (row->kind) {
    case K_BOOLEAN: return JS_NewBool(ctx, attr != NULL);
    case K_URL: return attr != NULL ? url_value(dom, ctx, attr->value) : JS_NewString(ctx, "");
    case K_FORM_ACTION:
        /* formAction reads the document's URL when it is missing or empty. */
        if (attr == NULL || attr->value[0] == '\0')
            return JS_NewString(ctx, d_window_document(dom, node) && dom->url != NULL ? dom->url : "about:blank");
        return url_value(dom, ctx, attr->value);
    case K_ENUM: {
        if (attr == NULL) return JS_NewString(ctx, row->missing);
        size_t length = os64_strlen(attr->value);
        for (const char *const *word = row->keywords; *word != NULL; word++)
            if (os64_strlen(*word) == length && same_folded(*word, attr->value, length))
                return JS_NewString(ctx, *word);
        return JS_NewString(ctx, row->invalid);
    }
    case K_LIMITED_LONG: {
        int64_t value = attr != NULL ? non_negative(attr->value) : -1;
        return JS_NewInt32(ctx, value >= 0 && value <= INT32_MAX ? (int32_t)value : -1);
    }
    case K_UNSIGNED: case K_LIMITED_UNSIGNED: case K_UNSIGNED_FALLBACK: {
        int64_t value = attr != NULL ? non_negative(attr->value) : -1;
        int64_t least = row->kind == K_UNSIGNED ? 0 : 1;
        return JS_NewUint32(ctx, value >= least && value <= INT32_MAX ? (uint32_t)value : row->number);
    }
    default: return JS_NewString(ctx, attr != NULL ? attr->value : "");
    }
}

/* Each setter converts its value before it reads anything of the tree. */
static JSValue reflection_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                              const DReflection *row, JSValueConst value)
{
    if (row->kind == K_BOOLEAN) {
        int on = JS_ToBool(ctx, value);
        if (on < 0) return JS_EXCEPTION;
        int64_t status = d_set_attr(dom, node, row->attribute, "", 0, !on);
        return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
    }
    if (row->kind == K_LIMITED_LONG) {
        int32_t number;
        if (JS_ToInt32(ctx, &number, value) < 0) return JS_EXCEPTION;
        if (number < 0) return d_error(ctx, "IndexSizeError", "The value must not be negative");
        return set_number(dom, ctx, node, row->attribute, (uint32_t)number);
    }
    if (row->kind == K_UNSIGNED || row->kind == K_LIMITED_UNSIGNED || row->kind == K_UNSIGNED_FALLBACK) {
        uint32_t number;
        if (JS_ToUint32(ctx, &number, value) < 0) return JS_EXCEPTION;
        if (number == 0 && row->kind == K_LIMITED_UNSIGNED)
            return d_error(ctx, "IndexSizeError", "The value must not be zero");
        uint32_t least = row->kind == K_UNSIGNED ? 0 : 1;
        return set_number(dom, ctx, node, row->attribute,
                          number >= least && number <= INT32_MAX ? number : row->number);
    }
    DString text = {0};
    if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
    int64_t status = d_set_attr(dom, node, row->attribute, text.data != NULL ? text.data : "", text.length,
                                false);
    d_string_free(dom, &text);
    return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
}

/* ── autocomplete: the IDL-exposed autofill value ─────────────────────── */

static const char *const kFieldNames[] = {
    "name", "honorific-prefix", "given-name", "additional-name", "family-name", "honorific-suffix",
    "nickname", "username", "new-password", "current-password", "one-time-code", "organization-title",
    "organization", "street-address", "address-line1", "address-line2", "address-line3", "address-level4",
    "address-level3", "address-level2", "address-level1", "country", "country-name", "postal-code",
    "cc-name", "cc-given-name", "cc-additional-name", "cc-family-name", "cc-number", "cc-exp",
    "cc-exp-month", "cc-exp-year", "cc-csc", "cc-type", "transaction-currency", "transaction-amount",
    "language", "bday", "bday-day", "bday-month", "bday-year", "sex", "url", "photo", NULL,
};
static const char *const kContactNames[] = {
    "tel", "tel-country-code", "tel-national", "tel-area-code", "tel-local", "tel-local-prefix",
    "tel-local-suffix", "tel-extension", "email", "impp", NULL,
};
static const char *const kContactKinds[] = {"home", "work", "mobile", "fax", "pager", NULL};
static const char *const kModes[] = {"shipping", "billing", NULL};

typedef struct { const char *at; size_t length; } Token;

static bool token_in(Token token, const char *const *words)
{
    for (; *words != NULL; words++)
        if (os64_strlen(*words) == token.length && same_folded(*words, token.at, token.length)) return true;
    return false;
}

/* HTML § 4.10.18.7.1, the autocomplete attribute's grammar read from its
 * last token back: [section-*] [shipping|billing] [home|work|...] field
 * [webauthn], or a lone "on" or "off" (not on a hidden input, which wears
 * the autofill anchor mantle). The tokens it took, lowercased and one
 * space apart; "" for a value the grammar refuses or a missing one. */
static JSValue autocomplete_value(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    const os64_html_attr_t *attr = os64_html_attr(node, "autocomplete");
    if (attr == NULL) return JS_NewString(ctx, "");
    Token tokens[6];
    size_t count = 0;
    for (const char *at = attr->value; *at != '\0';) {
        while (is_space(*at)) at++;
        const char *end = at;
        while (*end != '\0' && !is_space(*end)) end++;
        if (end == at) break;
        if (count == sizeof(tokens) / sizeof(tokens[0])) return JS_NewString(ctx, "");
        tokens[count++] = (Token){at, (size_t)(end - at)};
        at = end;
    }
    if (count == 0) return JS_NewString(ctx, "");
    bool hidden = html(node, OS64_HTML_TAG_INPUT) && os64_html_attr(node, "type") != NULL &&
        os64_strlen(os64_html_attr(node, "type")->value) == 6 &&
        same_folded(os64_html_attr(node, "type")->value, "hidden", 6);
    if (count == 1 && !hidden && (token_in(tokens[0], (const char *const[]){"on", "off", NULL})))
        return JS_NewString(ctx, lower(tokens[0].at[1]) == 'n' ? "on" : "off");
    size_t last = count;
    if (token_in(tokens[last - 1], (const char *const[]){"webauthn", NULL})) last--;
    if (last == 0) return JS_NewString(ctx, "");
    Token field = tokens[last - 1];
    bool contact = token_in(field, kContactNames);
    if (!contact && !token_in(field, kFieldNames)) return JS_NewString(ctx, "");
    size_t first = last - 1;
    if (contact && first > 0 && token_in(tokens[first - 1], kContactKinds)) first--;
    if (first > 0 && token_in(tokens[first - 1], kModes)) first--;
    if (first > 0 && tokens[first - 1].length > 8 && same_folded(tokens[first - 1].at, "section-", 8)) first--;
    if (first != 0) return JS_NewString(ctx, "");
    size_t size = 0;
    for (size_t i = 0; i < count; i++) size += tokens[i].length + 1;
    char *text = d_alloc(dom, size);
    if (text == NULL) return d_error(ctx, "QuotaExceededError", "DOM string quota exceeded");
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        if (i != 0) text[at++] = ' ';
        for (size_t k = 0; k < tokens[i].length; k++) text[at++] = lower(tokens[i].at[k]);
    }
    JSValue result = JS_NewStringLen(ctx, text, at);
    d_free(dom, text);
    return result;
}

/* ── The members that are not plain reflections ──────────────────────── */

/* A textarea's child text content: its Text children's data, in order. */
static JSValue child_text(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    size_t size = 0;
    for (const os64_html_node_t *child = node->first_child; child != NULL; child = child->next)
        if (child->kind == OS64_HTML_TEXT) size += child->text_len;
    char *text = d_alloc(dom, size + 1);
    if (text == NULL) return d_error(ctx, "QuotaExceededError", "DOM string quota exceeded");
    size_t at = 0;
    for (const os64_html_node_t *child = node->first_child; child != NULL; child = child->next)
        if (child->kind == OS64_HTML_TEXT) {
            os64_memcpy(text + at, child->text, child->text_len);
            at += child->text_len;
        }
    JSValue result = JS_NewStringLen(ctx, text, at);
    d_free(dom, text);
    return result;
}

static bool image_input(const os64_html_node_t *node)
{
    const os64_html_attr_t *type = os64_html_attr(node, "type");
    return type != NULL && os64_strlen(type->value) == 5 && same_folded(type->value, "image", 5);
}

/* Whether a labels list exists for the node: a labelable element's, and
 * an input's only while its type is not hidden (HTML § 4.10.4). */
static bool labelable(const os64_html_node_t *node)
{
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML) return false;
    switch (node->tag) {
    case OS64_HTML_TAG_INPUT: {
        const os64_html_attr_t *type = os64_html_attr(node, "type");
        return type == NULL || os64_strlen(type->value) != 6 || !same_folded(type->value, "hidden", 6);
    }
    case OS64_HTML_TAG_BUTTON: case OS64_HTML_TAG_METER: case OS64_HTML_TAG_OUTPUT: case OS64_HTML_TAG_PROGRESS:
    case OS64_HTML_TAG_SELECT: case OS64_HTML_TAG_TEXTAREA:
        return true;
    default: return false;
    }
}

const os64_html_node_t *d_label_control(const os64_html_node_t *label)
{
    if (!html(label, OS64_HTML_TAG_LABEL)) return NULL;
    const os64_html_attr_t *named = os64_html_attr(label, "for");
    /* No ID is empty, so an empty `for` names no control. */
    if (named != NULL && named->value[0] == '\0') return NULL;
    const os64_html_node_t *root = label;
    if (named != NULL) {
        while (root->parent != NULL) root = root->parent;
    }
    for (const os64_html_node_t *at = named != NULL ? root : label->first_child; at != NULL;) {
        if (named != NULL) {
            const os64_html_attr_t *id = at->kind == OS64_HTML_ELEMENT ? os64_html_attr(at, "id") : NULL;
            if (id != NULL && os64_streq(id->value, named->value)) return labelable(at) ? at : NULL;
        } else if (labelable(at)) return at;
        if (at->first_child != NULL) { at = at->first_child; continue; }
        while (at != root && at->next == NULL) at = at->parent;
        at = at != root ? at->next : NULL;
    }
    return NULL;
}

static JSValue control_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, this_value);
    if (node == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    unsigned kind = control_kind(node);
    if (magic < M_TEXT_DEFAULT) {
        const DReflection *row = &kReflections[magic];
        if ((row->on & kind) == 0) return JS_ThrowTypeError(ctx, "Illegal invocation");
        return set ? reflection_set(dom, ctx, node, row, value) : reflection_get(dom, ctx, node, row);
    }
    switch (magic) {
    case M_TEXT_DEFAULT: {
        if (kind != TA) return JS_ThrowTypeError(ctx, "Illegal invocation");
        if (!set) return child_text(dom, ctx, node);
        /* String replace all: textContent's setter, with the value a
         * string first (null is "null" here, and "" there). */
        JSValue text = JS_ToString(ctx, value);
        if (JS_IsException(text)) return text;
        JSValue result = d_content_set(dom, ctx, node, D_TEXT_CONTENT, text);
        JS_FreeValue(ctx, text);
        return result;
    }
    case M_TEXT_LENGTH: {
        if (kind != TA) return JS_ThrowTypeError(ctx, "Illegal invocation");
        const char *text;
        size_t length;
        int64_t status = os64_page_node_value(dom->state, node, &text, &length);
        return status < 0 ? d_page_error(ctx, status) : JS_NewUint32(ctx, d_utf16_length(text, length));
    }
    case M_INDETERMINATE: {
        if (kind != IN) return JS_ThrowTypeError(ctx, "Illegal invocation");
        bool on = false;
        int64_t status;
        if (set) {
            int boolean = JS_ToBool(ctx, value);
            if (boolean < 0) return JS_EXCEPTION;
            status = os64_page_node_set_indeterminate(dom->state, node, boolean != 0);
        } else status = os64_page_node_indeterminate(dom->state, node, &on);
        return status < 0 ? d_page_error(ctx, status) : set ? JS_UNDEFINED : JS_NewBool(ctx, on);
    }
    case M_AUTOCOMPLETE: {
        if ((kind & (IN | TA | SE)) == 0) return JS_ThrowTypeError(ctx, "Illegal invocation");
        if (!set) return autocomplete_value(dom, ctx, node);
        static const DReflection row = {"autocomplete", "autocomplete", K_STRING, IN | TA | SE, NULL, NULL, NULL, 0};
        return reflection_set(dom, ctx, node, &row, value);
    }
    case M_WIDTH: case M_HEIGHT: {
        /* An image button's dimensions as its attributes give them, as an
         * img's are read here; 0 for an input of any other type, which has
         * no image. */
        if (kind != IN) return JS_ThrowTypeError(ctx, "Illegal invocation");
        static const DReflection rows[] = {
            {"width", "width", K_UNSIGNED, IN, NULL, NULL, NULL, 0},
            {"height", "height", K_UNSIGNED, IN, NULL, NULL, NULL, 0},
        };
        const DReflection *row = &rows[magic == M_HEIGHT];
        if (set) return reflection_set(dom, ctx, node, row, value);
        return image_input(node) ? reflection_get(dom, ctx, node, row) : JS_NewUint32(ctx, 0);
    }
    case M_FORM:
        if (kind == 0) return JS_ThrowTypeError(ctx, "Illegal invocation");
        return d_wrap(dom, ctx, d_form_owner(node));
    case M_LABELS:
        if (kind == 0) return JS_ThrowTypeError(ctx, "Illegal invocation");
        return labelable(node) ? d_labels(dom, ctx, node) : JS_NULL;
    default: return JS_ThrowTypeError(ctx, "Unknown control member");
    }
}

int d_controls_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    (void)global;
    static const struct { const char *name; int magic; bool writable; unsigned on; } kMembers[] = {
        {"defaultValue", M_TEXT_DEFAULT, true, TA}, {"textLength", M_TEXT_LENGTH, false, TA},
        {"indeterminate", M_INDETERMINATE, true, IN}, {"autocomplete", M_AUTOCOMPLETE, true, IN | TA | SE},
        {"width", M_WIDTH, true, IN}, {"height", M_HEIGHT, true, IN},
        {"form", M_FORM, false, IN | TA | SE | BU}, {"labels", M_LABELS, false, IN | TA | SE | BU},
    };
    static const int kinds[] = {D_PROTO_INPUT, D_PROTO_TEXTAREA, D_PROTO_SELECT, D_PROTO_BUTTON};
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        JSValueConst prototype = dom->prototypes[kinds[k]]->value;
        unsigned mask = 1u << kinds[k];
        for (size_t i = 0; i < sizeof(kReflections) / sizeof(kReflections[0]); i++)
            if ((kReflections[i].on & mask) &&
                d_accessor(dom, ctx, prototype, kReflections[i].property, control_member, (int)i, true) < 0)
                return -1;
        for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++)
            if ((kMembers[i].on & mask) &&
                d_accessor(dom, ctx, prototype, kMembers[i].name, control_member, kMembers[i].magic,
                           kMembers[i].writable) < 0)
                return -1;
    }
    return 0;
}
