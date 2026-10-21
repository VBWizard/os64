#include "internal.h"

/* btoa and atob (HTML § 8.3, "Base64 utility methods"). A string here is
 * one byte per code unit, the convention these two predate every other
 * binary type with: btoa takes a string whose every code unit is below 256
 * and answers its base64, and atob answers a string whose code units are
 * the bytes the base64 names. Anything else is an InvalidCharacterError,
 * before any work, so a refused call has made nothing. */

static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* A base64 digit's value, or -1 for a byte that is none. */
static int digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* The infra standard's ASCII whitespace, which forgiving-base64 skips. */
static bool ascii_space(unsigned char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

/* The code units of `text` (WTF-8) as bytes, in place, when every one is
 * below 256; false when one is not. A code unit at or past 0x80 is two
 * bytes of UTF-8 (C2 or C3 and a continuation); anything longer is a code
 * unit past 0xFF, a surrogate included. */
static bool narrow(DString *text)
{
    size_t out = 0;
    for (size_t at = 0; at < text->length; out++) {
        unsigned char c = (unsigned char)text->data[at];
        if (c < 0x80) { text->data[out] = (char)c; at++; continue; }
        if ((c != 0xc2 && c != 0xc3) || at + 1 >= text->length) return false;
        text->data[out] = (char)(((c & 0x03) << 6) | ((unsigned char)text->data[at + 1] & 0x3f));
        at += 2;
    }
    text->length = out;
    return true;
}

static JSValue encode(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    DString text = {0};
    if (!d_string_text(dom, ctx, value, &text, false)) return JS_EXCEPTION;
    if (!narrow(&text)) {
        d_string_free(dom, &text);
        return d_error(ctx, "InvalidCharacterError", "btoa takes a string whose every character is below U+0100");
    }
    size_t groups = text.length / 3 + (text.length % 3 != 0);
    char *out = groups <= (SIZE_MAX - 1) / 4 ? d_alloc(dom, groups * 4 + 1) : NULL;
    if (out == NULL) {
        d_string_free(dom, &text);
        return d_error(ctx, "QuotaExceededError", "btoa quota exceeded");
    }
    const unsigned char *in = (const unsigned char *)text.data;
    size_t used = 0;
    for (size_t at = 0; at < text.length; at += 3) {
        size_t left = text.length - at;
        uint32_t bits = (uint32_t)in[at] << 16 | (left > 1 ? (uint32_t)in[at + 1] << 8 : 0) |
                        (left > 2 ? in[at + 2] : 0);
        out[used++] = kAlphabet[(bits >> 18) & 63];
        out[used++] = kAlphabet[(bits >> 12) & 63];
        out[used++] = left > 1 ? kAlphabet[(bits >> 6) & 63] : '=';
        out[used++] = left > 2 ? kAlphabet[bits & 63] : '=';
    }
    d_string_free(dom, &text);
    JSValue result = JS_NewStringLen(ctx, out, used);
    d_free(dom, out);
    return result;
}

/* Forgiving-base64 decode (infra § 4.8): ASCII whitespace is skipped
 * anywhere, a length that is a multiple of four may end in one or two
 * `=`, a length that leaves one digit over names no byte, and any other
 * byte is refused. Each byte comes back as one code unit, so one at or
 * past 0x80 is two bytes of the UTF-8 the engine reads. */
static JSValue decode(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    DString text = {0};
    if (!d_string_text(dom, ctx, value, &text, false)) return JS_EXCEPTION;
    size_t kept = 0;
    for (size_t at = 0; at < text.length; at++)
        if (!ascii_space((unsigned char)text.data[at])) text.data[kept++] = text.data[at];
    if (kept % 4 == 0 && kept > 0 && text.data[kept - 1] == '=') {
        kept--;
        if (text.data[kept - 1] == '=') kept--;
    }
    bool valid = kept % 4 != 1;
    for (size_t at = 0; valid && at < kept; at++) valid = digit((unsigned char)text.data[at]) >= 0;
    if (!valid) {
        d_string_free(dom, &text);
        return d_error(ctx, "InvalidCharacterError", "atob takes base64 text");
    }
    /* Three bytes per four digits, each byte at most two bytes of UTF-8. */
    size_t bytes = kept / 4 * 3 + (kept % 4 != 0 ? kept % 4 - 1 : 0);
    char *out = bytes <= SIZE_MAX / 2 - 1 ? d_alloc(dom, bytes * 2 + 1) : NULL;
    if (out == NULL) {
        d_string_free(dom, &text);
        return d_error(ctx, "QuotaExceededError", "atob quota exceeded");
    }
    size_t used = 0;
    uint32_t bits = 0;
    int held = 0;
    for (size_t at = 0; at < kept; at++) {
        bits = bits << 6 | (uint32_t)digit((unsigned char)text.data[at]);
        held += 6;
        if (held < 8) continue;
        held -= 8;
        unsigned char byte = (unsigned char)(bits >> held);
        bits &= (1u << held) - 1;
        if (byte < 0x80) out[used++] = (char)byte;
        else {
            out[used++] = (char)(0xc0 | byte >> 6);
            out[used++] = (char)(0x80 | (byte & 0x3f));
        }
    }
    d_string_free(dom, &text);
    JSValue result = JS_NewStringLen(ctx, out, used);
    d_free(dom, out);
    return result;
}

static JSValue base64(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                      JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "%s needs one argument", magic == 0 ? "btoa" : "atob");
    return magic == 0 ? encode(dom, ctx, argv[0]) : decode(dom, ctx, argv[0]);
}

int d_base64_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    return d_method(dom, ctx, global, "btoa", base64, 1, 0) < 0 ||
           d_method(dom, ctx, global, "atob", base64, 1, 1) < 0 ? -1 : 0;
}
