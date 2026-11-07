#include "internal.h"

/* CDATASection AND ProcessingInstruction (DOM § 4.12, § 4.13), and the
 * Document members that make them, createCDATASection and
 * createProcessingInstruction.
 *
 * libhtml keeps a CDATA section as a TEXT node with `cdata` set and a
 * processing instruction as its PI kind, so neither has a prototype kind of
 * its own here (the binding's kinds are one uint64_t's bits, all taken):
 * their prototypes refine a wrapper's as ShadowRoot's do (d_node_prototype),
 * CDATASection's over Text's and ProcessingInstruction's over
 * CharacterData's, so every member and receiver check those kinds have
 * applies to them unchanged. The prototypes, and a PI's `target`, are the
 * binding's structure and exist whatever the Modern Web's switch says (a
 * processing instruction the XML builder made has CharacterData's members
 * with it off too); the switch takes away only what its row adds, the two
 * names on window and Document's creation members. Unlike ShadowRoot's,
 * whose prototypes come and go with the shadow trees' switch, these levels
 * are part of the tree every document can hold. */

static JSValue data_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                           JSValue *data);

enum { C_CDATA, C_PI, C_TARGET };

int d_data_prototypes(os64_dom_t *dom, JSContext *ctx)
{
    dom->cdata_proto = d_retain(dom, ctx, JS_NewObjectProto(ctx, dom->prototypes[D_PROTO_TEXT]->value));
    if (dom->cdata_proto == NULL) return -1;
    dom->pi_proto = d_retain(dom, ctx, JS_NewObjectProto(ctx, dom->prototypes[D_PROTO_CHARACTER_DATA]->value));
    if (dom->pi_proto == NULL) return -1;
    return d_accessor(dom, ctx, dom->pi_proto->value, "target", data_member, C_TARGET, false);
}

/* XML's Name production, as a processing instruction's target must match
 * it: a letter, '_' or ':' first, then those, digits, '-' and '.'; a byte
 * past ASCII is taken as one of the Unicode name characters XML admits. */
static bool xml_name(const char *text)
{
    const unsigned char *at = (const unsigned char *)text;
    if (*at == 0) return false;
    for (bool first = true; *at != 0; at++, first = false) {
        unsigned char c = *at;
        bool start = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80;
        if (!start && (first || !((c >= '0' && c <= '9') || c == '-' || c == '.'))) return false;
    }
    return true;
}

static bool contains(const char *data, size_t length, const char *needle)
{
    size_t n = os64_strlen(needle);
    for (size_t i = 0; i + n <= length; i++)
        if (os64_memcmp(data + i, needle, n) == 0) return true;
    return false;
}

static JSValue data_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                           JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    if (magic == C_TARGET) {
        if (node->kind != OS64_HTML_PI) return JS_ThrowTypeError(ctx, "target requires a processing instruction");
        return JS_NewString(ctx, node->name != NULL ? node->name : "");
    }
    if (node->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Creation requires a document");
    int needed = magic == C_PI ? 2 : 1;
    if (argc < needed) return JS_ThrowTypeError(ctx, "Missing argument");
    /* The arguments converted first, in order, as WebIDL converts them. */
    DString first = {0}, second = {0};
    if (!d_string(dom, ctx, argv[0], &first)) return JS_EXCEPTION;
    if (magic == C_PI && !d_string(dom, ctx, argv[1], &second)) {
        d_string_free(dom, &first);
        return JS_EXCEPTION;
    }
    JSValue result = JS_UNDEFINED;
    os64_html_node_t *made = NULL;
    int64_t status = OS64_HTML_OK;
    if (magic == C_CDATA) {
        /* createCDATASection: an HTML document has none; "]]>" would end
         * one early. */
        if (d_document_type(dom, node) == D_DOCUMENT_HTML)
            result = d_error(ctx, "NotSupportedError", "An HTML document has no CDATA sections");
        else if (contains(first.data, first.length, "]]>"))
            result = d_error(ctx, "InvalidCharacterError", "A CDATA section's data cannot hold \"]]>\"");
        else
            made = os64_html_create_cdata(dom->document, first.data, first.length, &status);
    } else {
        /* createProcessingInstruction: a target matching XML's Name, data
         * without "?>", in any document. */
        if (!xml_name(first.data))
            result = d_error(ctx, "InvalidCharacterError", "A processing instruction's target is an XML name");
        else if (contains(second.data, second.length, "?>"))
            result = d_error(ctx, "InvalidCharacterError", "A processing instruction's data cannot hold \"?>\"");
        else
            made = os64_html_create_pi(dom->document, first.data, second.data, second.length, &status);
    }
    d_string_free(dom, &first);
    d_string_free(dom, &second);
    if (!JS_IsUndefined(result)) return result;
    if (made == NULL) return d_html_error(ctx, status);
    /* Wrapped first, so d_made_in has a wrapper to record the document on. */
    JSValue value = d_wrap(dom, ctx, made);
    if (!JS_IsException(value)) d_made_in(dom, made, node);
    return value;
}

/* The row (modern.c): the two interface objects, Illegal constructors whose
 * parents are Text and CharacterData, and Document's two creation members.
 * After the interfaces row, whose objects it inherits. */
int d_data_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const struct { const char *name, *parent; int which; } kInterfaces[] = {
        {"CDATASection", "Text", 0}, {"ProcessingInstruction", "CharacterData", 1},
    };
    for (size_t i = 0; i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); i++) {
        JSValue parent = JS_GetPropertyStr(ctx, global, kInterfaces[i].parent);
        if (JS_IsException(parent)) return -1;
        JSValue f = d_interface_new(ctx, kInterfaces[i].name, 0, NULL,
                                    kInterfaces[i].which ? dom->pi_proto->value : dom->cdata_proto->value, parent);
        JS_FreeValue(ctx, parent);
        if (JS_IsException(f) ||
            JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            return -1;
    }
    JSValueConst document = dom->prototypes[D_PROTO_DOCUMENT]->value;
    if (d_method_named(dom, ctx, document, "createCDATASection", data_member, 1, C_CDATA) < 0 ||
        d_method_named(dom, ctx, document, "createProcessingInstruction", data_member, 2, C_PI) < 0)
        return -1;
    return 0;
}
