#include "internal.h"

/* TextEncoder and TextDecoder (the Encoding Standard, § 7). The encoder is
 * UTF-8 alone, as the standard has it. The decoder knows the encodings
 * libhtml's label table names, UTF-8, windows-1252 (which `latin1`,
 * `iso-8859-1` and `ascii` all are) and UTF-16 of either order, each
 * label as that table reads it (os64_html_encoding_for_label), so a page
 * and its scripts agree on what a label means; any other label is a
 * RangeError, which is what a page that asks for an encoding this browser
 * does not have can test for. A decoder keeps its partial sequence between
 * `stream` calls in engine memory, traced by nothing because it holds no
 * value. */

static JSClassID encoder_slot, decoder_slot;
static int encoder_mark;

/* The engine's classes for the buffer sources (WebIDL's BufferSource),
 * read off instances the engine makes, the same in every runtime since
 * the engine numbers its own classes. ArrayBuffer's and the typed
 * arrays' are read at installation, which runs no function; DataView's
 * needs its constructor run, so it is read at the first decode that meets
 * an object of neither kind, through the constructor the binding took
 * before any page ran. `typed_classes[0]` is Uint8Array's. */
static JSClassID array_buffer_class, data_view_class, typed_classes[12];
static bool buffer_classes_known, data_view_known;

bool d_string_text(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DString *out, bool scalar)
{
    out->data = NULL;
    out->length = 0;
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return false;
    size_t length;
    const char *source = JS_ToCStringLen(ctx, &length, value);
    if (source == NULL) return false;
    char *copy = length < SIZE_MAX ? d_alloc(dom, length + 1) : NULL;
    if (copy == NULL) {
        JS_FreeCString(ctx, source);
        d_error(ctx, "QuotaExceededError", "DOM string quota exceeded");
        return false;
    }
    os64_memcpy(copy, source, length);
    copy[length] = '\0';
    JS_FreeCString(ctx, source);
    /* A lone surrogate is ED A0..BF xx, three bytes, as is U+FFFD (EF BF
     * BD), so it is replaced where it stands. */
    for (size_t i = 0; scalar && i + 2 < length; i++)
        if ((unsigned char)copy[i] == 0xed && (unsigned char)copy[i + 1] >= 0xa0) {
            copy[i] = (char)0xef;
            copy[i + 1] = (char)0xbf;
            copy[i + 2] = (char)0xbd;
            i += 2;
        }
    out->data = copy;
    out->length = length;
    return true;
}

/* One code point of valid UTF-8 (or WTF-8) at `text`, and its length. */
static uint32_t next_point(const unsigned char *text, size_t left, size_t *took)
{
    unsigned char c = text[0];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (n > left) n = left;
    uint32_t point = n == 1 ? c : n == 2 ? c & 0x1fu : n == 3 ? c & 0x0fu : c & 0x07u;
    for (size_t i = 1; i < n; i++) point = point << 6 | (text[i] & 0x3fu);
    *took = n;
    return point;
}

static size_t put_point(char *out, uint32_t point)
{
    if (point < 0x80) { out[0] = (char)point; return 1; }
    if (point < 0x800) {
        out[0] = (char)(0xc0 | point >> 6);
        out[1] = (char)(0x80 | (point & 0x3f));
        return 2;
    }
    if (point < 0x10000) {
        out[0] = (char)(0xe0 | point >> 12);
        out[1] = (char)(0x80 | ((point >> 6) & 0x3f));
        out[2] = (char)(0x80 | (point & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | point >> 18);
    out[1] = (char)(0x80 | ((point >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((point >> 6) & 0x3f));
    out[3] = (char)(0x80 | (point & 0x3f));
    return 4;
}

/* ── Buffer sources ───────────────────────────────────────────────────── */

/* A typed array of `kind` over the whole of `buffer`: the constructor's
 * (buffer, byteOffset, length), the last two undefined. The engine reads
 * all three whatever the count it is given. */
static JSValue typed_over(JSContext *ctx, JSValueConst buffer, JSTypedArrayEnum kind)
{
    JSValueConst args[3] = {buffer, JS_UNDEFINED, JS_UNDEFINED};
    return JS_NewTypedArray(ctx, 3, args, kind);
}

static int buffer_classes(JSContext *ctx)
{
    if (buffer_classes_known) return 0;
    JSValue buffer = JS_NewArrayBufferCopy(ctx, NULL, 0);
    if (JS_IsException(buffer)) return -1;
    JSClassID typed[12];
    for (int kind = 0; kind < 12; kind++) {
        JSValue array = typed_over(ctx, buffer, (JSTypedArrayEnum)kind);
        if (JS_IsException(array)) {
            JS_FreeValue(ctx, buffer);
            return -1;
        }
        typed[kind] = JS_GetClassID(array);
        JS_FreeValue(ctx, array);
    }
    array_buffer_class = JS_GetClassID(buffer);
    JS_FreeValue(ctx, buffer);
    /* Uint8Array first: encodeInto takes nothing else. */
    typed_classes[0] = typed[JS_TYPED_ARRAY_UINT8];
    for (int kind = 0, at = 1; kind < 12; kind++)
        if (kind != JS_TYPED_ARRAY_UINT8) typed_classes[at++] = typed[kind];
    buffer_classes_known = true;
    return 0;
}

static bool is_typed(JSClassID id)
{
    for (int i = 0; i < 12; i++)
        if (typed_classes[i] == id) return true;
    return false;
}

/* WebIDL's BufferSource: an ArrayBuffer, a typed array or a DataView.
 * 1 for one, 0 for anything else, -1 with an exception pending (DataView's
 * class could not be learned). Nothing of the value is read. */
static int buffer_kind(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    if (!JS_IsObject(value)) return 0;
    JSClassID id = JS_GetClassID(value);
    if (id == array_buffer_class || is_typed(id)) return 1;
    if (!data_view_known) {
        JSValue buffer = JS_NewArrayBufferCopy(ctx, NULL, 0);
        JSValue view = JS_IsException(buffer) ? JS_EXCEPTION
                     : JS_CallConstructor(ctx, dom->data_view[0]->value, 1, (JSValueConst *)&buffer);
        JS_FreeValue(ctx, buffer);
        if (JS_IsException(view)) return -1;
        data_view_class = JS_GetClassID(view);
        data_view_known = true;
        JS_FreeValue(ctx, view);
    }
    return id == data_view_class;
}

/* The bytes a buffer source holds now, borrowed: a detached buffer, or a
 * view past the end of its buffer, holds none (WebIDL's "get a copy of the
 * bytes held by the buffer source"), which is not an error. A DataView's
 * buffer, offset and length are read through the getters its prototype had
 * before any page ran (dom->data_view). -1 with an exception pending. */
static int buffer_bytes(os64_dom_t *dom, JSContext *ctx, JSValueConst value, const uint8_t **bytes,
                        size_t *length)
{
    *bytes = NULL;
    *length = 0;
    JSClassID id = JS_GetClassID(value);
    if (id == array_buffer_class) {
        uint8_t *data = JS_GetArrayBuffer(ctx, length, value);
        if (data == NULL) { JS_FreeValue(ctx, JS_GetException(ctx)); *length = 0; return 0; }
        *bytes = data;
        return 0;
    }
    size_t offset = 0, span = 0;
    JSValue buffer;
    if (is_typed(id)) {
        buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &span, NULL);
        if (JS_IsException(buffer)) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
    } else {
        JSValue got[3];
        for (int i = 0; i < 3; i++) {
            got[i] = JS_Call(ctx, dom->data_view[1 + i]->value, value, 0, NULL);
            if (JS_IsException(got[i])) {
                /* A view whose buffer was detached throws from its getters:
                 * it holds nothing. */
                for (int k = 0; k < i; k++) JS_FreeValue(ctx, got[k]);
                JS_FreeValue(ctx, JS_GetException(ctx));
                return 0;
            }
        }
        double numbers[2] = {0, 0};
        bool read = JS_ToFloat64(ctx, &numbers[0], got[1]) == 0 && JS_ToFloat64(ctx, &numbers[1], got[2]) == 0;
        JS_FreeValue(ctx, got[1]);
        JS_FreeValue(ctx, got[2]);
        if (!read) { JS_FreeValue(ctx, got[0]); return -1; }
        buffer = got[0];
        offset = (size_t)numbers[0];
        span = (size_t)numbers[1];
    }
    size_t whole;
    uint8_t *data = JS_GetArrayBuffer(ctx, &whole, buffer);
    JS_FreeValue(ctx, buffer);
    if (data == NULL) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
    if (offset > whole || span > whole - offset) return 0;
    *bytes = data + offset;
    *length = span;
    return 0;
}

/* ── TextEncoder ──────────────────────────────────────────────────────── */

static JSValue encoder_new(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (d_binding(ctx) == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return JS_EXCEPTION;
    JSClassID id = os64_js_class_id(&encoder_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (!JS_IsException(object)) JS_SetOpaque(object, &encoder_mark);
    return object;
}

static JSValue result_pair(JSContext *ctx, uint64_t read, uint64_t written)
{
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object)) return object;
    if (JS_DefinePropertyValueStr(ctx, object, "read", JS_NewInt64(ctx, (int64_t)read), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, object, "written", JS_NewInt64(ctx, (int64_t)written), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

enum { E_ENCODING, E_ENCODE, E_ENCODE_INTO };

static JSValue encoder_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (JS_GetOpaque(self, os64_js_class_id(&encoder_slot)) == NULL)
        return JS_ThrowTypeError(ctx, "Expected a TextEncoder");
    if (magic == E_ENCODING) return JS_NewString(ctx, "utf-8");
    if (magic == E_ENCODE_INTO && argc < 2)
        return JS_ThrowTypeError(ctx, "encodeInto needs a source and a destination");
    /* encode's input defaults to ""; encodeInto's source is required, so
     * undefined there is the string "undefined". */
    DString text = {0};
    if ((magic == E_ENCODE_INTO || (argc > 0 && !JS_IsUndefined(argv[0]))) &&
        !d_string_text(dom, ctx, argv[0], &text, true))
        return JS_EXCEPTION;
    if (magic == E_ENCODE) {
        JSValue buffer = JS_NewArrayBufferCopy(ctx, (const uint8_t *)text.data, text.length);
        d_string_free(dom, &text);
        if (JS_IsException(buffer)) return buffer;
        JSValue array = typed_over(ctx, buffer, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, buffer);
        return array;
    }
    /* encodeInto(source, destination): as much of the source as fits,
     * never part of a character; `read` counts UTF-16 code units, so a
     * character past the BMP is two of them. */
    if (!JS_IsObject(argv[1]) || JS_GetClassID(argv[1]) != typed_classes[0]) {
        d_string_free(dom, &text);
        return JS_ThrowTypeError(ctx, "encodeInto writes into a Uint8Array");
    }
    const uint8_t *bytes;
    size_t room;
    if (buffer_bytes(dom, ctx, argv[1], &bytes, &room) < 0) {
        d_string_free(dom, &text);
        return JS_EXCEPTION;
    }
    uint8_t *into = (uint8_t *)bytes;
    uint64_t read = 0, written = 0;
    for (size_t at = 0; at < text.length;) {
        size_t took;
        uint32_t point = next_point((const unsigned char *)text.data + at, text.length - at, &took);
        if (took > room - written) break;
        os64_memcpy(into + written, text.data + at, took);
        written += took;
        read += point >= 0x10000 ? 2 : 1;
        at += took;
    }
    d_string_free(dom, &text);
    return result_pair(ctx, read, written);
}

/* ── TextDecoder ──────────────────────────────────────────────────────── */

typedef enum { DEC_UTF8, DEC_1252, DEC_UTF16LE, DEC_UTF16BE } DecoderEncoding;

typedef struct {
    DecoderEncoding encoding;
    bool fatal, ignore_bom, bom_seen, do_not_flush;
    /* UTF-8's state: the code point so far, the bytes it still needs, and
     * the range its next byte must be in. */
    uint32_t point;
    uint8_t needed, seen, lower, upper;
    /* UTF-16's: a byte waiting for its pair, and a lead surrogate waiting
     * for its trail. */
    int16_t lead_byte;
    uint16_t lead_surrogate;
} DDecoder;

static const char *const kEncodingNames[] = {"utf-8", "windows-1252", "utf-16le", "utf-16be"};

static void decoder_reset(DDecoder *d)
{
    d->point = 0;
    d->needed = d->seen = 0;
    d->lower = 0x80;
    d->upper = 0xbf;
    d->lead_byte = -1;
    d->lead_surrogate = 0;
    d->bom_seen = false;
}

static void decoder_finalizer(JSRuntime *rt, JSValue value)
{
    DDecoder *d = JS_GetOpaque(value, os64_js_class_id(&decoder_slot));
    if (d != NULL) js_free_rt(rt, d);
}

/* Where decoded code points go: counted when `out` is NULL, written when
 * it is not. The first one is the BOM rule's to judge (the standard's
 * "serialize I/O queue"): U+FEFF there is dropped unless ignoreBOM. */
typedef struct {
    DDecoder *d;
    char *out;
    size_t used;
    bool error;
} DSink;

static void emit(DSink *sink, uint32_t point)
{
    DDecoder *d = sink->d;
    if (d->encoding != DEC_1252 && !d->ignore_bom && !d->bom_seen) {
        d->bom_seen = true;
        if (point == 0xfeff) return;
    }
    if (sink->out != NULL) sink->used += put_point(sink->out + sink->used, point);
    else sink->used += point < 0x80 ? 1 : point < 0x800 ? 2 : point < 0x10000 ? 3 : 4;
}

/* A decoder error: U+FFFD, or, for a fatal decoder, the end of the decode
 * (which throws). False when decoding stops. */
static bool error(DSink *sink)
{
    if (sink->d->fatal) { sink->error = true; return false; }
    emit(sink, 0xfffd);
    return true;
}

/* The Encoding Standard's UTF-8 decoder over `bytes`, then its end of
 * queue when `flush`. A byte a sequence cannot take is an error and is
 * read again as the start of what follows. */
static void decode_utf8(DSink *sink, const uint8_t *bytes, size_t length, bool flush)
{
    DDecoder *d = sink->d;
    for (size_t at = 0; at < length;) {
        uint8_t b = bytes[at];
        if (d->needed == 0) {
            at++;
            if (b < 0x80) emit(sink, b);
            else if (b >= 0xc2 && b <= 0xdf) { d->needed = 1; d->point = b & 0x1fu; }
            else if (b >= 0xe0 && b <= 0xef) {
                if (b == 0xe0) d->lower = 0xa0;
                if (b == 0xed) d->upper = 0x9f;
                d->needed = 2;
                d->point = b & 0x0fu;
            } else if (b >= 0xf0 && b <= 0xf4) {
                if (b == 0xf0) d->lower = 0x90;
                if (b == 0xf4) d->upper = 0x8f;
                d->needed = 3;
                d->point = b & 0x07u;
            } else if (!error(sink)) return;
            continue;
        }
        if (b < d->lower || b > d->upper) {
            d->point = 0;
            d->needed = d->seen = 0;
            d->lower = 0x80;
            d->upper = 0xbf;
            if (!error(sink)) return;
            continue;                   /* the byte is read again */
        }
        at++;
        d->lower = 0x80;
        d->upper = 0xbf;
        d->point = d->point << 6 | (b & 0x3fu);
        if (++d->seen != d->needed) continue;
        uint32_t point = d->point;
        d->point = 0;
        d->needed = d->seen = 0;
        emit(sink, point);
    }
    if (flush && d->needed != 0) {
        d->point = 0;
        d->needed = d->seen = 0;
        (void)error(sink);
    }
}

/* The UTF-16 decoder, for either order. A lead surrogate not followed by a
 * trail is an error, and the code unit that followed it is read again. */
static void decode_utf16(DSink *sink, const uint8_t *bytes, size_t length, bool flush)
{
    DDecoder *d = sink->d;
    bool big = d->encoding == DEC_UTF16BE;
    for (size_t at = 0; at < length; at++) {
        if (d->lead_byte < 0) { d->lead_byte = bytes[at]; continue; }
        uint16_t unit = big ? (uint16_t)(d->lead_byte << 8 | bytes[at]) : (uint16_t)(bytes[at] << 8 | d->lead_byte);
        d->lead_byte = -1;
        for (;;) {
            if (d->lead_surrogate != 0) {
                uint16_t lead = d->lead_surrogate;
                d->lead_surrogate = 0;
                if (unit >= 0xdc00 && unit <= 0xdfff) {
                    emit(sink, 0x10000 + ((uint32_t)(lead - 0xd800) << 10) + (uint32_t)(unit - 0xdc00));
                    break;
                }
                if (!error(sink)) return;
                continue;               /* the unit is read again */
            }
            if (unit >= 0xd800 && unit <= 0xdbff) d->lead_surrogate = unit;
            else if (unit >= 0xdc00 && unit <= 0xdfff) { if (!error(sink)) return; }
            else emit(sink, unit);
            break;
        }
    }
    if (flush && (d->lead_byte >= 0 || d->lead_surrogate != 0)) {
        d->lead_byte = -1;
        d->lead_surrogate = 0;
        (void)error(sink);
    }
}

static void decode_run(DSink *sink, const uint8_t *bytes, size_t length, bool flush)
{
    switch (sink->d->encoding) {
    case DEC_UTF8: decode_utf8(sink, bytes, length, flush); break;
    case DEC_1252:
        for (size_t at = 0; at < length; at++) emit(sink, os64_html_decode_windows_1252(bytes[at]));
        break;
    default: decode_utf16(sink, bytes, length, flush); break;
    }
}

/* A dictionary member's truth (WebIDL boolean), false when the dictionary
 * is undefined or null; a dictionary that is not an object is a
 * TypeError. -1 with an exception pending. */
static int option(JSContext *ctx, int argc, JSValueConst *argv, int at, const char *name)
{
    int dictionary = d_dictionary(ctx, argc, argv, at);
    if (dictionary <= 0) return dictionary;
    JSValue value = JS_GetPropertyStr(ctx, argv[at], name);
    if (JS_IsException(value)) return -1;
    int truth = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
    return truth;
}

static JSValue decoder_new(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    DecoderEncoding encoding = DEC_UTF8;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        DString label = {0};
        if (!d_string_text(dom, ctx, argv[0], &label, false)) return JS_EXCEPTION;
        const char *name = os64_html_encoding_for_label(label.data, label.length);
        d_string_free(dom, &label);
        if (name == NULL) return JS_ThrowRangeError(ctx, "That encoding is not one this browser decodes");
        for (int i = 0; i < 4; i++)
            if (os64_streq(name, kEncodingNames[i])) encoding = (DecoderEncoding)i;
    }
    int fatal = option(ctx, argc, argv, 1, "fatal");
    int ignore = fatal < 0 ? -1 : option(ctx, argc, argv, 1, "ignoreBOM");
    if (ignore < 0) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return JS_EXCEPTION;
    JSClassID id = os64_js_class_id(&decoder_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DDecoder *d = js_mallocz(ctx, sizeof(*d));
    if (d == NULL) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
    d->encoding = encoding;
    d->fatal = fatal != 0;
    d->ignore_bom = ignore != 0;
    decoder_reset(d);
    JS_SetOpaque(object, d);
    return object;
}

enum { T_ENCODING, T_FATAL, T_IGNORE_BOM, T_DECODE };

static JSValue decoder_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DDecoder *d = JS_GetOpaque(self, os64_js_class_id(&decoder_slot));
    if (d == NULL) return JS_ThrowTypeError(ctx, "Expected a TextDecoder");
    if (magic == T_ENCODING) return JS_NewString(ctx, kEncodingNames[d->encoding]);
    if (magic == T_FATAL) return JS_NewBool(ctx, d->fatal);
    if (magic == T_IGNORE_BOM) return JS_NewBool(ctx, d->ignore_bom);
    /* decode(input, {stream}): the input is checked to be a buffer source,
     * the options read, and only then are the bytes taken, since reading
     * the options may run a getter that detaches the buffer. */
    bool given = argc > 0 && !JS_IsUndefined(argv[0]);
    int kind = given ? buffer_kind(dom, ctx, argv[0]) : 1;
    if (kind < 0) return JS_EXCEPTION;
    if (kind == 0) return JS_ThrowTypeError(ctx, "decode takes an ArrayBuffer or a view of one");
    int stream = option(ctx, argc, argv, 1, "stream");
    if (stream < 0) return JS_EXCEPTION;
    const uint8_t *bytes = NULL;
    size_t length = 0;
    if (given && buffer_bytes(dom, ctx, argv[0], &bytes, &length) < 0) return JS_EXCEPTION;
    if (!d->do_not_flush) decoder_reset(d);
    d->do_not_flush = stream != 0;
    /* Counted first on a copy of the state, then written exactly: the
     * text is staged in the binding's budget, and a decode's worst case
     * (three bytes for every byte) would refuse a buffer a third the size
     * of the one it can decode. */
    DDecoder counting = *d;
    DSink sink = {&counting, NULL, 0, false};
    decode_run(&sink, bytes, length, !stream);
    if (sink.error) {
        /* The decoder is where the error left it, and `stream` stands, as
         * the standard's decode leaves them: the next call resets it only
         * if this one did not ask to stream. */
        *d = counting;
        return JS_ThrowTypeError(ctx, "The bytes are not valid %s", kEncodingNames[d->encoding]);
    }
    char *out = d_alloc(dom, sink.used + 1);
    if (out == NULL) return d_error(ctx, "QuotaExceededError", "TextDecoder quota exceeded");
    DSink writing = {d, out, 0, false};
    decode_run(&writing, bytes, length, !stream);
    JSValue text = JS_NewStringLen(ctx, out, writing.used);
    d_free(dom, out);
    return text;
}

static int install_class(os64_dom_t *dom, JSContext *ctx, JSValueConst global, JSClassID *slot,
                         const JSClassDef *definition, const char *name, int length,
                         JSCFunction *constructor, JSValue *made)
{
    JSClassID id = os64_js_class_id(slot);
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (JS_IsException(prototype)) return -1;
    DValue *held = d_retain(dom, ctx, prototype);
    if (held == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    JSValue f = d_interface_new(ctx, name, length, constructor, prototype, JS_UNDEFINED);
    if (JS_IsException(f)) return -1;
    if (JS_DefinePropertyValueStr(ctx, global, name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    *made = prototype;
    return 0;
}

int d_encoding_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (buffer_classes(ctx) < 0) return -1;
    /* DataView's constructor and own getters, before any page can replace
     * them. */
    JSValue view = JS_GetPropertyStr(ctx, global, "DataView");
    dom->data_view[0] = d_retain(dom, ctx, view);
    if (dom->data_view[0] == NULL) return -1;
    JSValue view_prototype = JS_GetPropertyStr(ctx, view, "prototype");
    if (JS_IsException(view_prototype)) return -1;
    static const char *const kViewNames[3] = {"buffer", "byteOffset", "byteLength"};
    for (int i = 0; i < 3; i++) {
        JSAtom atom = JS_NewAtom(ctx, kViewNames[i]);
        JSPropertyDescriptor descriptor;
        int found = atom == JS_ATOM_NULL ? -1 : JS_GetOwnProperty(ctx, &descriptor, view_prototype, atom);
        if (atom != JS_ATOM_NULL) JS_FreeAtom(ctx, atom);
        if (found <= 0) {
            JS_FreeValue(ctx, view_prototype);
            if (found == 0) JS_ThrowTypeError(ctx, "DataView has no %s", kViewNames[i]);
            return -1;
        }
        JS_FreeValue(ctx, descriptor.value);
        JS_FreeValue(ctx, descriptor.setter);
        dom->data_view[1 + i] = d_retain(dom, ctx, descriptor.getter);
        if (dom->data_view[1 + i] == NULL) {
            JS_FreeValue(ctx, view_prototype);
            return -1;
        }
    }
    JS_FreeValue(ctx, view_prototype);
    static const JSClassDef encoder_class = {.class_name = "TextEncoder"};
    static const JSClassDef decoder_class = {.class_name = "TextDecoder", .finalizer = decoder_finalizer};
    JSValue encoder, decoder;
    if (install_class(dom, ctx, global, &encoder_slot, &encoder_class, "TextEncoder", 0, encoder_new, &encoder) < 0 ||
        d_accessor(dom, ctx, encoder, "encoding", encoder_member, E_ENCODING, false) < 0 ||
        d_method(dom, ctx, encoder, "encode", encoder_member, 0, E_ENCODE) < 0 ||
        d_method(dom, ctx, encoder, "encodeInto", encoder_member, 2, E_ENCODE_INTO) < 0)
        return -1;
    if (install_class(dom, ctx, global, &decoder_slot, &decoder_class, "TextDecoder", 0, decoder_new, &decoder) < 0 ||
        d_accessor(dom, ctx, decoder, "encoding", decoder_member, T_ENCODING, false) < 0 ||
        d_accessor(dom, ctx, decoder, "fatal", decoder_member, T_FATAL, false) < 0 ||
        d_accessor(dom, ctx, decoder, "ignoreBOM", decoder_member, T_IGNORE_BOM, false) < 0 ||
        d_method(dom, ctx, decoder, "decode", decoder_member, 0, T_DECODE) < 0)
        return -1;
    return 0;
}

/* ── For the other rows: a buffer source's bytes, a Uint8Array made ─── */

int d_buffer_source(os64_dom_t *dom, JSContext *ctx, JSValueConst value, const uint8_t **bytes, size_t *length)
{
    if (buffer_classes(ctx) < 0) return -1;
    int kind = buffer_kind(dom, ctx, value);
    if (kind <= 0) return kind;
    return buffer_bytes(dom, ctx, value, bytes, length) < 0 ? -1 : 1;
}

JSValue d_uint8_array(JSContext *ctx, const uint8_t *bytes, size_t length)
{
    JSValue buffer = JS_NewArrayBufferCopy(ctx, bytes, length);
    if (JS_IsException(buffer)) return buffer;
    JSValue array = typed_over(ctx, buffer, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, buffer);
    return array;
}

JSValue d_text_decode(JSContext *ctx, const char *label, const uint8_t *bytes, size_t length)
{
    DDecoder d = {0};
    d.encoding = DEC_UTF8;
    const char *name = label != NULL ? os64_html_encoding_for_label(label, os64_strlen(label)) : NULL;
    for (int i = 0; name != NULL && i < 4; i++)
        if (os64_streq(name, kEncodingNames[i])) d.encoding = (DecoderEncoding)i;
    /* "decode" sniffs a BOM, which outranks the label; "UTF-8 decode" (no
     * label) takes only UTF-8's off, which the sink does. */
    if (label != NULL && length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) d.encoding = DEC_UTF8;
    else if (label != NULL && length >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) d.encoding = DEC_UTF16BE;
    else if (label != NULL && length >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) d.encoding = DEC_UTF16LE;
    decoder_reset(&d);
    /* Counted, then written, as decode does; in the runtime's memory,
     * since what is decoded here is a response body. */
    DDecoder counting = d;
    DSink sink = {&counting, NULL, 0, false};
    decode_run(&sink, bytes, length, true);
    char *out = js_malloc(ctx, sink.used + 1);
    if (out == NULL) return JS_EXCEPTION;
    DSink writing = {&d, out, 0, false};
    decode_run(&writing, bytes, length, true);
    JSValue text = JS_NewStringLen(ctx, out, writing.used);
    js_free(ctx, out);
    return text;
}

JSValue d_utf8_decode(JSContext *ctx, const uint8_t *bytes, size_t length)
{
    return d_text_decode(ctx, NULL, bytes, length);
}
