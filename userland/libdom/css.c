#include "internal.h"
#include "garb/cascade.h"

/* THE CSS NAMESPACE (CSSOM § 6.1, `namespace CSS`, and CSS Conditional 3
 * § 6.1's partial namespace): escape() and supports(). supports() is
 * libgarb's own @supports judgment (garb_supports), so a page's feature
 * test and a sheet's @supports rule answer alike, except that supports()
 * reads as a no-quirks document does whatever the page's mode, as other
 * browsers' does (a quirks page's sheet takes a unitless length; its
 * supports() does not).
 *
 * escape() is "serialize an identifier" over the string's UTF-8 form, as
 * QuickJS hands it: a code point at or above U+0080 is passed through
 * byte for byte, which is how a lone surrogate survives the trip (QuickJS
 * writes one as its three-byte form and reads that form back as the same
 * code unit), and every rule of the algorithm is about ASCII. A byte's
 * index stands for its code point's where a rule asks for a position: the
 * second-character rule follows a `-`, one byte, so byte 1 is code point
 * 1, and a lone `-` is the one-byte string. */

/* "Escape a character as code point": a backslash, the code point in
 * lowercase hexadecimal, and a space. */
static void escape_code_point(char *out, size_t *n, unsigned value)
{
    static const char kHex[] = "0123456789abcdef";
    char digits[2];
    size_t count = 0;
    do {
        digits[count++] = kHex[value & 15];
        value >>= 4;
    } while (value != 0);
    out[(*n)++] = '\\';
    while (count != 0) out[(*n)++] = digits[--count];
    out[(*n)++] = ' ';
}

static bool ident_ascii(unsigned char c)
{
    return c == '-' || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

enum { M_ESCAPE, M_SUPPORTS };

/* "Serialize an identifier" (CSSOM § 2.1). */
static JSValue css_escape(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "CSS.escape requires 1 argument");
    size_t length;
    const char *ident = JS_ToCStringLen(ctx, &length, argv[0]);
    if (ident == NULL) return JS_EXCEPTION;
    /* Each byte becomes at most "\7f " (four bytes) or U+FFFD's three. */
    if (length > (SIZE_MAX - 1) / 4) {
        JS_FreeCString(ctx, ident);
        return JS_ThrowRangeError(ctx, "The identifier is too long to escape");
    }
    char *out = js_malloc(ctx, length * 4 + 1);
    if (out == NULL) {
        JS_FreeCString(ctx, ident);
        return JS_EXCEPTION;
    }
    size_t n = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)ident[i];
        if (c == 0) {
            out[n++] = (char)0xef;
            out[n++] = (char)0xbf;
            out[n++] = (char)0xbd;
        } else if (c <= 0x1f || c == 0x7f || (i == 0 && c >= '0' && c <= '9') ||
                   (i == 1 && c >= '0' && c <= '9' && ident[0] == '-')) {
            escape_code_point(out, &n, c);
        } else if (i == 0 && c == '-' && length == 1) {
            out[n++] = '\\';
            out[n++] = '-';
        } else if (c >= 0x80 || ident_ascii(c)) {
            out[n++] = (char)c;
        } else {
            out[n++] = '\\';
            out[n++] = (char)c;
        }
    }
    JS_FreeCString(ctx, ident);
    JSValue result = JS_NewStringLen(ctx, out, n);
    js_free(ctx, out);
    return result;
}

/* supports(property, value) with two arguments or more, WebIDL's overload
 * resolution by count; supports(conditionText) with one. */
static JSValue css_supports(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "CSS.supports requires 1 argument");
    size_t first_length = 0, second_length = 0;
    const char *first = JS_ToCStringLen(ctx, &first_length, argv[0]);
    if (first == NULL) return JS_EXCEPTION;
    const char *second = NULL;
    if (argc >= 2) {
        second = JS_ToCStringLen(ctx, &second_length, argv[1]);
        if (second == NULL) {
            JS_FreeCString(ctx, first);
            return JS_EXCEPTION;
        }
    }
    garb_reading_t how = {.quirks = false, .modern_off = !dom->options.modern};
    int32_t r = second != NULL ? garb_supports_property(first, first_length, second, second_length, how)
                               : garb_supports(first, first_length, how);
    JS_FreeCString(ctx, first);
    if (second != NULL) JS_FreeCString(ctx, second);
    /* Only memory refuses an answer (garb_supports). */
    if (r < 0) return d_error(ctx, "QuotaExceededError", "CSS.supports had no memory to read its condition");
    return JS_NewBool(ctx, r != 0);
}

static JSValue css_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    return magic == M_SUPPORTS ? css_supports(dom, ctx, argc, argv) : css_escape(ctx, argc, argv);
}

/* One operation of the namespace: writable, enumerable and configurable,
 * named, its length WebIDL's (its shortest overload's arguments). */
static bool operation(os64_dom_t *dom, JSContext *ctx, JSValueConst css, const char *name, int magic)
{
    JSValue function = JS_NewCFunctionData(ctx, css_member, 1, magic, 1, &dom->anchor->value);
    if (JS_IsException(function)) return false;
    JSValue text = JS_NewString(ctx, name);
    bool made = !JS_IsException(text) &&
                JS_DefinePropertyValueStr(ctx, function, "name", text, JS_PROP_CONFIGURABLE) >= 0 &&
                JS_DefinePropertyValueStr(ctx, css, name, JS_DupValue(ctx, function), JS_PROP_C_W_E) >= 0;
    JS_FreeValue(ctx, function);
    return made;
}

/* The CSS row (modern.c): the namespace object on the global, as WebIDL
 * defines one: an ordinary object over Object.prototype, its operations
 * writable, enumerable and configurable, its @@toStringTag "CSS", and the
 * global property writable and configurable, not enumerable. */
int d_css_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue css = JS_NewObject(ctx);
    if (JS_IsException(css)) return -1;
    bool made = operation(dom, ctx, css, "escape", M_ESCAPE) && operation(dom, ctx, css, "supports", M_SUPPORTS) &&
                d_string_tag(ctx, css, "CSS") >= 0 &&
                JS_DefinePropertyValueStr(ctx, global, "CSS", JS_DupValue(ctx, css),
                                          JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) >= 0;
    JS_FreeValue(ctx, css);
    return made ? 0 : -1;
}
