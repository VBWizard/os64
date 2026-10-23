#include "internal.h"
#include "os64/date.h"
#include "os64/fmt.h"

/* Blob and File (the File API, § 3 and § 4) and FormData (XHR § 4): the
 * bytes a request body carries and the forms a page builds. Their bytes are
 * ENGINE memory (js_malloc), counted against the runtime's memory limit as
 * anything else a script makes is, and freed with the object; nothing here
 * is a container a page can reach: a FormData's entries are a C array, read
 * by index and looked up again after anything that can run page code. */

static JSClassID blob_slot, form_slot, form_iterator_slot;

typedef struct {
    uint8_t *bytes;
    size_t length;
    char *type;                 /* "" for none; lowercase printable ASCII */
    bool file;
    char *name;                 /* File: UTF-8 */
    int64_t last_modified;      /* File: ms since 1970 */
} DBlob;

typedef struct {
    char *name, *value;         /* UTF-8, js_malloc'd; value NULL for a file */
    size_t name_length, value_length;
    JSValue file;               /* a File, or undefined */
} DEntry;

typedef struct {
    DEntry *entries;
    size_t count, cap;
} DForm;

typedef struct {
    JSValue form;
    size_t index;
    int kind;                   /* F_ENTRIES, F_KEYS, F_VALUES */
} DFormIterator;

enum { F_ENTRIES, F_KEYS, F_VALUES };

/* ── Sequences, strings and bytes ────────────────────────────────────── */

void d_sequence_free(JSContext *ctx, JSValue *items, size_t count)
{
    for (size_t i = 0; i < count; i++) JS_FreeValue(ctx, items[i]);
    js_free(ctx, items);
}

/* Closes an iterator a conversion left early, keeping the exception that
 * made it leave (ECMAScript's IteratorClose on an abrupt completion). */
static void iterator_close(JSContext *ctx, JSValueConst iterator)
{
    JSValue pending = JS_GetException(ctx);
    JSValue method = JS_GetPropertyStr(ctx, iterator, "return");
    if (!JS_IsException(method) && JS_IsFunction(ctx, method)) {
        JSValue done = JS_Call(ctx, method, iterator, 0, NULL);
        JS_FreeValue(ctx, done);
    }
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_Throw(ctx, pending);
}

int d_sequence(os64_dom_t *dom, JSContext *ctx, JSValueConst value, size_t max, JSValue **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "Expected a sequence");
        return -1;
    }
    JSAtom key = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
    if (key == JS_ATOM_NULL) return -1;
    JSValue method = JS_GetProperty(ctx, value, key);
    JS_FreeAtom(ctx, key);
    if (JS_IsException(method)) return -1;
    if (!JS_IsFunction(ctx, method)) {
        JS_FreeValue(ctx, method);
        JS_ThrowTypeError(ctx, "Expected an iterable");
        return -1;
    }
    JSValue iterator = JS_Call(ctx, method, value, 0, NULL);
    JS_FreeValue(ctx, method);
    if (JS_IsException(iterator)) return -1;
    if (!JS_IsObject(iterator)) {
        JS_FreeValue(ctx, iterator);
        JS_ThrowTypeError(ctx, "The iterator is not an object");
        return -1;
    }
    JSValue next = JS_GetPropertyStr(ctx, iterator, "next");
    if (JS_IsException(next)) {
        JS_FreeValue(ctx, iterator);
        return -1;
    }
    JSValue *items = NULL;
    size_t n = 0, cap = 0;
    int result = 0;
    for (;;) {
        JSValue step = JS_Call(ctx, next, iterator, 0, NULL);
        if (JS_IsException(step)) { result = -1; break; }
        if (!JS_IsObject(step)) {
            JS_FreeValue(ctx, step);
            JS_ThrowTypeError(ctx, "The iterator's result is not an object");
            result = -1;
            break;
        }
        JSValue done = JS_GetPropertyStr(ctx, step, "done");
        int finished = JS_IsException(done) ? -1 : JS_ToBool(ctx, done);
        JS_FreeValue(ctx, done);
        if (finished != 0) {
            JS_FreeValue(ctx, step);
            if (finished < 0) result = -1;
            break;
        }
        JSValue item = JS_GetPropertyStr(ctx, step, "value");
        JS_FreeValue(ctx, step);
        if (JS_IsException(item)) { result = -1; break; }
        if (n == max) {
            JS_FreeValue(ctx, item);
            JS_ThrowTypeError(ctx, "The sequence is longer than this browser takes");
            iterator_close(ctx, iterator);
            result = -1;
            break;
        }
        if (n == cap) {
            size_t grown_cap = cap ? cap * 2 : 8;
            JSValue *grown = js_realloc(ctx, items, grown_cap * sizeof(*grown));
            if (grown == NULL) {
                JS_FreeValue(ctx, item);
                result = -1;
                break;
            }
            items = grown;
            cap = grown_cap;
        }
        items[n++] = item;
    }
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, iterator);
    if (result < 0) {
        d_sequence_free(ctx, items, n);
        return -1;
    }
    *out = items;
    *count = n;
    return 0;
}

void d_bytes_free(JSContext *ctx, DBytes *b)
{
    js_free(ctx, b->bytes);
    js_free(ctx, b->type);
    b->bytes = NULL;
    b->type = NULL;
    b->length = 0;
}

/* A string's text as UTF-8 in ENGINE memory (a body may be megabytes, past
 * the binding's own budget), each lone surrogate U+FFFD when `scalar`
 * (USVString). False with an exception pending. */
static bool engine_text(JSContext *ctx, JSValueConst value, bool scalar, char **out, size_t *length)
{
    size_t n;
    const char *source = JS_ToCStringLen(ctx, &n, value);
    if (source == NULL) return false;
    char *copy = js_malloc(ctx, n + 1);
    if (copy == NULL) {
        JS_FreeCString(ctx, source);
        return false;
    }
    os64_memcpy(copy, source, n);
    copy[n] = '\0';
    JS_FreeCString(ctx, source);
    for (size_t i = 0; scalar && i + 2 < n; i++)
        if ((unsigned char)copy[i] == 0xed && (unsigned char)copy[i + 1] >= 0xa0) {
            copy[i] = (char)0xef;
            copy[i + 1] = (char)0xbf;
            copy[i + 2] = (char)0xbd;
            i += 2;
        }
    *out = copy;
    *length = n;
    return true;
}

static char *engine_copy(JSContext *ctx, const char *s, size_t n)
{
    char *copy = js_malloc(ctx, n + 1);
    if (copy == NULL) return NULL;
    os64_memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

/* A growing byte buffer in engine memory. */
typedef struct {
    uint8_t *bytes;
    size_t length, cap;
} Buf;

static bool buf_put(JSContext *ctx, Buf *b, const void *bytes, size_t n)
{
    if (n > SIZE_MAX - b->length - 1) {
        JS_ThrowRangeError(ctx, "The data is too long");
        return false;
    }
    if (b->length + n > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->length + n) cap = cap > SIZE_MAX / 2 ? b->length + n : cap * 2;
        uint8_t *grown = js_realloc(ctx, b->bytes, cap);
        if (grown == NULL) return false;
        b->bytes = grown;
        b->cap = cap;
    }
    if (n != 0) os64_memcpy(b->bytes + b->length, bytes, n);
    b->length += n;
    return true;
}

static bool buf_text(JSContext *ctx, Buf *b, const char *text)
{
    return buf_put(ctx, b, text, os64_strlen(text));
}

/* ── Blob ────────────────────────────────────────────────────────────── */

static void blob_finalizer(JSRuntime *rt, JSValue value)
{
    DBlob *b = JS_GetOpaque(value, os64_js_class_id(&blob_slot));
    if (b == NULL) return;
    js_free_rt(rt, b->bytes);
    js_free_rt(rt, b->type);
    js_free_rt(rt, b->name);
    js_free_rt(rt, b);
}

static DBlob *blob_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&blob_slot));
}

bool d_blob_get(JSValueConst value, const uint8_t **bytes, size_t *length, const char **type)
{
    DBlob *b = blob_state(value);
    if (b == NULL) return false;
    *bytes = b->bytes;
    *length = b->length;
    *type = b->type;
    return true;
}

/* The File API's type rule: a type with a byte outside U+0020..U+007E is
 * none, and the rest is lowercased. Engine memory. */
static char *type_normalized(JSContext *ctx, const char *type, size_t length)
{
    bool printable = true;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)type[i] < 0x20 || (unsigned char)type[i] > 0x7e) printable = false;
    char *out = js_malloc(ctx, (printable ? length : 0) + 1);
    if (out == NULL) return NULL;
    size_t n = 0;
    for (size_t i = 0; printable && i < length; i++)
        out[n++] = type[i] >= 'A' && type[i] <= 'Z' ? (char)(type[i] - 'A' + 'a') : type[i];
    out[n] = '\0';
    return out;
}

/* A Blob (or a File, with `file`) of the bytes (taken) and type (taken),
 * with `prototype` (the class's when not an object). */
static JSValue blob_make(JSContext *ctx, JSValueConst prototype, uint8_t *bytes, size_t length, char *type,
                         bool file, char *name, int64_t last_modified)
{
    JSClassID id = os64_js_class_id(&blob_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    DBlob *b = JS_IsException(object) ? NULL : js_mallocz(ctx, sizeof(*b));
    if (b == NULL) {
        JS_FreeValue(ctx, object);
        js_free(ctx, bytes);
        js_free(ctx, type);
        js_free(ctx, name);
        return JS_EXCEPTION;
    }
    b->bytes = bytes;
    b->length = length;
    b->type = type;
    b->file = file;
    b->name = name;
    b->last_modified = last_modified;
    JS_SetOpaque(object, b);
    return object;
}

JSValue d_blob_new(os64_dom_t *dom, JSContext *ctx, DBytes *bytes)
{
    (void)dom;
    char *type = bytes->type != NULL ? bytes->type : engine_copy(ctx, "", 0);
    if (type == NULL) {
        d_bytes_free(ctx, bytes);
        return JS_EXCEPTION;
    }
    JSValue blob = blob_make(ctx, JS_UNDEFINED, bytes->bytes, bytes->length, type, false, NULL, 0);
    bytes->bytes = NULL;
    bytes->type = NULL;
    bytes->length = 0;
    return blob;
}

/* sequence<BlobPart>, converted ONCE, as WebIDL converts the argument
 * before the options are read: the items, and for each one that is
 * neither a BufferSource nor a Blob its USVString (engine memory; NULL for
 * the others, whose bytes are read when the parts are processed). */
typedef struct {
    JSValue *items;
    char **texts;
    size_t *lengths;
    size_t count;
} Parts;

static void parts_free(JSContext *ctx, Parts *p)
{
    for (size_t i = 0; p->texts != NULL && i < p->count; i++) js_free(ctx, p->texts[i]);
    js_free(ctx, p->texts);
    js_free(ctx, p->lengths);
    d_sequence_free(ctx, p->items, p->count);
    *p = (Parts){0};
}

static int parts_convert(os64_dom_t *dom, JSContext *ctx, JSValueConst parts, Parts *out)
{
    *out = (Parts){0};
    if (JS_IsUndefined(parts)) return 0;
    if (d_sequence(dom, ctx, parts, D_SEQUENCE_MAX, &out->items, &out->count) < 0) return -1;
    if (out->count == 0) return 0;
    out->texts = js_mallocz(ctx, out->count * sizeof(*out->texts));
    out->lengths = js_mallocz(ctx, out->count * sizeof(*out->lengths));
    if (out->texts == NULL || out->lengths == NULL) {
        parts_free(ctx, out);
        return -1;
    }
    for (size_t i = 0; i < out->count; i++) {
        const uint8_t *bytes;
        size_t length;
        const char *type;
        int kind = d_buffer_source(dom, ctx, out->items[i], &bytes, &length);
        if (kind < 0 ||
            (kind == 0 && !d_blob_get(out->items[i], &bytes, &length, &type) &&
             !engine_text(ctx, out->items[i], true, &out->texts[i], &out->lengths[i]))) {
            parts_free(ctx, out);
            return -1;
        }
    }
    return 0;
}

/* The File API's "process blob parts", over the parts converted above:
 * each part's bytes, a string's as UTF-8 with its line endings made "\n"
 * when `native`. */
static int parts_process(os64_dom_t *dom, JSContext *ctx, const Parts *p, bool native, Buf *out)
{
    for (size_t i = 0; i < p->count; i++) {
        const char *text = p->texts[i];
        size_t n = p->lengths[i];
        if (text == NULL) {
            const uint8_t *bytes;
            size_t length;
            const char *type;
            int kind = d_buffer_source(dom, ctx, p->items[i], &bytes, &length);
            if (kind < 0) return -1;
            if ((kind > 0 || d_blob_get(p->items[i], &bytes, &length, &type)) && !buf_put(ctx, out, bytes, length))
                return -1;
            continue;
        }
        for (size_t k = 0; k < n; k++) {
            bool put;
            if (native && text[k] == '\r') {
                if (k + 1 < n && text[k + 1] == '\n') k++;
                put = buf_put(ctx, out, "\n", 1);
            } else put = buf_put(ctx, out, text + k, 1);
            if (!put) return -1;
        }
    }
    return 0;
}

/* BlobPropertyBag (endings, type) and FilePropertyBag's lastModified, read
 * in WebIDL's order. `*type` is engine memory. */
static int blob_options(os64_dom_t *dom, JSContext *ctx, JSValueConst options, bool file, bool *native, char **type,
                        int64_t *last_modified)
{
    *native = false;
    *type = NULL;
    if (!JS_IsUndefined(options) && !JS_IsNull(options) && !JS_IsObject(options)) {
        JS_ThrowTypeError(ctx, "The options are not a dictionary");
        return -1;
    }
    if (JS_IsObject(options)) {
        JSValue endings = JS_GetPropertyStr(ctx, options, "endings");
        if (JS_IsException(endings)) return -1;
        if (!JS_IsUndefined(endings)) {
            const char *text = JS_ToCString(ctx, endings);
            JS_FreeValue(ctx, endings);
            if (text == NULL) return -1;
            bool known = os64_streq(text, "transparent") || os64_streq(text, "native");
            *native = os64_streq(text, "native");
            JS_FreeCString(ctx, text);
            if (!known) {
                JS_ThrowTypeError(ctx, "endings must be \"transparent\" or \"native\"");
                return -1;
            }
        }
        JSValue value = JS_GetPropertyStr(ctx, options, "type");
        if (JS_IsException(value)) return -1;
        if (!JS_IsUndefined(value)) {
            char *text;
            size_t n;
            bool got = engine_text(ctx, value, false, &text, &n);
            JS_FreeValue(ctx, value);
            if (!got) return -1;
            *type = type_normalized(ctx, text, n);
            js_free(ctx, text);
            if (*type == NULL) return -1;
        }
        if (file) {
            JSValue modified = JS_GetPropertyStr(ctx, options, "lastModified");
            if (JS_IsException(modified)) return -1;
            if (!JS_IsUndefined(modified)) {
                int64_t ms;
                int got = JS_ToInt64(ctx, &ms, modified);
                JS_FreeValue(ctx, modified);
                if (got < 0) return -1;
                *last_modified = ms;
            }
        }
    }
    if (*type == NULL) *type = engine_copy(ctx, "", 0);
    if (*type == NULL) return -1;
    (void)dom;
    return 0;
}

static int64_t now_epoch_ms(void)
{
    os64_time_t wall;
    return os64_time(&wall) == 0 && wall.ticks_per_second != 0
        ? wall.epoch * 1000 + (int64_t)((uint64_t)wall.ticks_into_second * 1000 / wall.ticks_per_second) : 0;
}

/* new Blob(blobParts, options) and new File(fileBits, fileName, options). */
static JSValue blob_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, bool file)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    if (file && argc < 2) return JS_ThrowTypeError(ctx, "File needs its bits and a name");
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    Buf bytes = {0};
    char *name = NULL, *type = NULL;
    size_t name_length = 0;
    bool native = false;
    int64_t last_modified = now_epoch_ms();
    JSValueConst options = argc > (file ? 2 : 1) ? argv[file ? 2 : 1] : JS_UNDEFINED;
    /* WebIDL's argument order: the parts converted, the name, the options;
     * then the parts processed, once, with the endings the options chose
     * (a generator among the parts is iterated once). */
    Parts parts;
    if (parts_convert(dom, ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &parts) < 0 ||
        (file && !engine_text(ctx, argv[1], true, &name, &name_length)) ||
        blob_options(dom, ctx, options, file, &native, &type, &last_modified) < 0 ||
        parts_process(dom, ctx, &parts, native, &bytes) < 0) {
        parts_free(ctx, &parts);
        js_free(ctx, bytes.bytes);
        js_free(ctx, name);
        js_free(ctx, type);
        JS_FreeValue(ctx, prototype);
        return JS_EXCEPTION;
    }
    parts_free(ctx, &parts);
    JSValue made = blob_make(ctx, prototype, bytes.bytes, bytes.length, type, file, name, last_modified);
    JS_FreeValue(ctx, prototype);
    return made;
}

static JSValue blob_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return blob_construct(ctx, new_target, argc, argv, false);
}

static JSValue file_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return blob_construct(ctx, new_target, argc, argv, true);
}

/* A promise already settled with `value` (taken), or rejected with the
 * pending exception when `value` is JS_EXCEPTION. */
static JSValue settled(JSContext *ctx, JSValue value)
{
    bool rejected = JS_IsException(value);
    JSValue argument = rejected ? JS_GetException(ctx) : value;
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, argument);
        return promise;
    }
    JSValue done = JS_Call(ctx, resolving[rejected ? 1 : 0], JS_UNDEFINED, 1, (JSValueConst *)&argument);
    JS_FreeValue(ctx, argument);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    if (JS_IsException(done)) {
        JS_FreeValue(ctx, promise);
        return done;
    }
    JS_FreeValue(ctx, done);
    return promise;
}

JSValue d_promise_settled(JSContext *ctx, JSValue value)
{
    return settled(ctx, value);
}


enum { B_SIZE, B_TYPE, B_SLICE, B_TEXT, B_ARRAY_BUFFER, B_BYTES, B_STREAM, B_NAME, B_LAST_MODIFIED, B_RELATIVE };

/* A position for slice: from the end when negative, clamped to the size. */
static size_t relative(double value, size_t size)
{
    if (value != value) return 0;
    if (value < 0) return -value >= (double)size ? 0 : size - (size_t)(-value);
    return value >= (double)size ? size : (size_t)value;
}

static JSValue blob_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DBlob *b = blob_state(self);
    if (b == NULL || ((magic == B_NAME || magic == B_LAST_MODIFIED || magic == B_RELATIVE) && !b->file)) {
        JSValue error = JS_ThrowTypeError(ctx, "Expected a %s", magic >= B_NAME ? "File" : "Blob");
        return magic == B_TEXT || magic == B_ARRAY_BUFFER || magic == B_BYTES ? settled(ctx, error) : error;
    }
    switch (magic) {
    case B_SIZE: return JS_NewFloat64(ctx, (double)b->length);
    case B_TYPE: return JS_NewString(ctx, b->type);
    case B_NAME: return JS_NewString(ctx, b->name != NULL ? b->name : "");
    case B_LAST_MODIFIED: return JS_NewInt64(ctx, b->last_modified);
    case B_RELATIVE: return JS_NewString(ctx, "");
    case B_TEXT: return settled(ctx, d_utf8_decode(ctx, b->bytes, b->length));
    case B_ARRAY_BUFFER: return settled(ctx, JS_NewArrayBufferCopy(ctx, b->bytes, b->length));
    case B_BYTES: return settled(ctx, d_uint8_array(ctx, b->bytes, b->length));
    case B_STREAM: return d_stream_new(dom, ctx, b->bytes, b->length);
    case B_SLICE: {
        double start = 0, end = (double)b->length;
        if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToFloat64(ctx, &start, argv[0]) < 0) return JS_EXCEPTION;
        if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &end, argv[1]) < 0) return JS_EXCEPTION;
        char *type = NULL;
        if (argc > 2 && !JS_IsUndefined(argv[2])) {
            char *text;
            size_t n;
            if (!engine_text(ctx, argv[2], false, &text, &n)) return JS_EXCEPTION;
            type = type_normalized(ctx, text, n);
            js_free(ctx, text);
        } else type = engine_copy(ctx, "", 0);
        if (type == NULL) return JS_EXCEPTION;
        /* The conversions above ran page code: the blob is read again. */
        b = blob_state(self);
        size_t from = relative(start, b->length), to = relative(end, b->length);
        size_t span = to > from ? to - from : 0;
        uint8_t *bytes = js_malloc(ctx, span + 1);
        if (bytes == NULL) {
            js_free(ctx, type);
            return JS_EXCEPTION;
        }
        if (span != 0) os64_memcpy(bytes, b->bytes + from, span);
        JSValue prototype = JS_GetClassProto(ctx, os64_js_class_id(&blob_slot));
        JSValue made = blob_make(ctx, prototype, bytes, span, type, false, NULL, 0);
        JS_FreeValue(ctx, prototype);
        return made;
    }
    default: return JS_UNDEFINED;
    }
}

/* ── FormData ────────────────────────────────────────────────────────── */

static void form_finalizer(JSRuntime *rt, JSValue value)
{
    DForm *f = JS_GetOpaque(value, os64_js_class_id(&form_slot));
    if (f == NULL) return;
    for (size_t i = 0; i < f->count; i++) {
        js_free_rt(rt, f->entries[i].name);
        js_free_rt(rt, f->entries[i].value);
        JS_FreeValueRT(rt, f->entries[i].file);
    }
    js_free_rt(rt, f->entries);
    js_free_rt(rt, f);
}

static void form_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DForm *f = JS_GetOpaque(value, os64_js_class_id(&form_slot));
    for (size_t i = 0; f != NULL && i < f->count; i++) JS_MarkValue(rt, f->entries[i].file, mark);
}

static void form_iterator_finalizer(JSRuntime *rt, JSValue value)
{
    DFormIterator *it = JS_GetOpaque(value, os64_js_class_id(&form_iterator_slot));
    if (it == NULL) return;
    JS_FreeValueRT(rt, it->form);
    js_free_rt(rt, it);
}

static void form_iterator_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DFormIterator *it = JS_GetOpaque(value, os64_js_class_id(&form_iterator_slot));
    if (it != NULL) JS_MarkValue(rt, it->form, mark);
}

static DForm *form_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&form_slot));
}

bool d_form_data_is(JSValueConst value)
{
    return form_state(value) != NULL;
}

static void entry_free(JSContext *ctx, DEntry *e)
{
    js_free(ctx, e->name);
    js_free(ctx, e->value);
    JS_FreeValue(ctx, e->file);
}

/* An entry appended (its strings and file taken). -1 on no memory, with
 * the entry freed. */
static int form_add(JSContext *ctx, DForm *f, DEntry *e)
{
    if (f->count == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 8;
        DEntry *grown = js_realloc(ctx, f->entries, cap * sizeof(*grown));
        if (grown == NULL) {
            entry_free(ctx, e);
            return -1;
        }
        f->entries = grown;
        f->cap = cap;
    }
    f->entries[f->count++] = *e;
    return 0;
}

static JSValue form_make(JSContext *ctx, JSValueConst prototype)
{
    JSClassID id = os64_js_class_id(&form_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DForm *f = js_mallocz(ctx, sizeof(*f));
    if (f == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(object, f);
    return object;
}

/* XHR § 4 "create an entry": a string value as it is; a Blob as a File,
 * named `filename`, else its own name, else "blob". The value converted,
 * page code and all, before anything is changed. */
static int entry_make(os64_dom_t *dom, JSContext *ctx, JSValueConst name, JSValueConst value, int argc_filename,
                      JSValueConst filename, DEntry *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->file = JS_UNDEFINED;
    if (!engine_text(ctx, name, true, &out->name, &out->name_length)) return -1;
    DBlob *blob = blob_state(value);
    if (blob == NULL) {
        if (!engine_text(ctx, value, true, &out->value, &out->value_length)) {
            entry_free(ctx, out);
            return -1;
        }
        return 0;
    }
    if (blob->file && argc_filename == 0) {
        out->file = JS_DupValue(ctx, value);
        return 0;
    }
    char *file_name;
    size_t file_name_length;
    if (argc_filename > 0) {
        if (!engine_text(ctx, filename, true, &file_name, &file_name_length)) {
            entry_free(ctx, out);
            return -1;
        }
    } else {
        file_name = engine_copy(ctx, "blob", 4);
        if (file_name == NULL) {
            entry_free(ctx, out);
            return -1;
        }
    }
    /* The conversion ran page code: the blob is read again. */
    blob = blob_state(value);
    uint8_t *bytes = js_malloc(ctx, blob->length + 1);
    char *type = bytes != NULL ? engine_copy(ctx, blob->type, os64_strlen(blob->type)) : NULL;
    if (type == NULL) {
        js_free(ctx, bytes);
        js_free(ctx, file_name);
        entry_free(ctx, out);
        return -1;
    }
    if (blob->length != 0) os64_memcpy(bytes, blob->bytes, blob->length);
    int64_t modified = blob->file ? blob->last_modified : now_epoch_ms();
    JSValue prototype = dom->file_prototype != NULL ? JS_DupValue(ctx, dom->file_prototype->value) : JS_UNDEFINED;
    out->file = blob_make(ctx, prototype, bytes, blob->length, type, true, file_name, modified);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(out->file)) {
        out->file = JS_UNDEFINED;
        entry_free(ctx, out);
        return -1;
    }
    return 0;
}

static JSValue entry_value(JSContext *ctx, const DEntry *e)
{
    return JS_IsUndefined(e->file) ? JS_NewStringLen(ctx, e->value, e->value_length) : JS_DupValue(ctx, e->file);
}

static bool name_is(const DEntry *e, const char *name, size_t length)
{
    return e->name_length == length && os64_memcmp(e->name, name, length) == 0;
}

static int form_entries_add(JSContext *ctx, JSValueConst form, const char *name, size_t name_length, const char *value,
                       size_t value_length)
{
    DForm *f = form_state(form);
    if (f == NULL) return 0;
    DEntry e = {engine_copy(ctx, name, name_length), engine_copy(ctx, value, value_length), name_length, value_length,
                JS_UNDEFINED};
    if (e.name == NULL || e.value == NULL) {
        entry_free(ctx, &e);
        return -1;
    }
    return form_add(ctx, f, &e);
}

/* The entries the form element `node` would submit, as HTML's "constructing
 * the entry list" makes them (libpage's own list, which a submission sends).
 * -1 with an exception pending. */
static int form_from_element(os64_dom_t *dom, JSContext *ctx, JSValueConst form, const os64_html_node_t *node);

static void form_entry_heard(void *opaque, const char *name, size_t name_length, const char *value,
                             size_t value_length, bool is_file)
{
    struct { JSContext *ctx; JSValueConst form; int result; } *at = opaque;
    if (at->result < 0) return;
    /* A file control with no file chosen (nothing can choose one yet) is an
     * empty File named "" of type application/octet-stream. */
    if (is_file) {
        DForm *f = form_state(at->form);
        DEntry e = {engine_copy(at->ctx, name, name_length), NULL, name_length, 0, JS_UNDEFINED};
        char *type = engine_copy(at->ctx, "application/octet-stream", 24);
        char *empty = engine_copy(at->ctx, "", 0);
        uint8_t *bytes = js_malloc(at->ctx, 1);
        if (e.name == NULL || type == NULL || empty == NULL || bytes == NULL) {
            js_free(at->ctx, e.name);
            js_free(at->ctx, type);
            js_free(at->ctx, empty);
            js_free(at->ctx, bytes);
            at->result = -1;
            return;
        }
        os64_dom_t *dom = d_binding(at->ctx);
        JSValue prototype = dom != NULL && dom->file_prototype != NULL ? JS_DupValue(at->ctx, dom->file_prototype->value)
                                                                       : JS_UNDEFINED;
        e.file = blob_make(at->ctx, prototype, bytes, 0, type, true, empty, now_epoch_ms());
        JS_FreeValue(at->ctx, prototype);
        if (JS_IsException(e.file) || form_add(at->ctx, f, &e) < 0) at->result = -1;
        return;
    }
    if (form_entries_add(at->ctx, at->form, name, name_length, value, value_length) < 0) at->result = -1;
}

static int form_from_element(os64_dom_t *dom, JSContext *ctx, JSValueConst form, const os64_html_node_t *node)
{
    struct { JSContext *ctx; JSValueConst form; int result; } at = {ctx, form, 0};
    int64_t status = os64_page_form_entries(dom->document, dom->url != NULL ? dom->url : "about:blank", dom->state,
                                            node, form_entry_heard, &at);
    if (status < 0) {
        if (at.result >= 0) d_error(ctx, "QuotaExceededError", "The form's entries could not be built");
        return -1;
    }
    return at.result;
}

static JSValue form_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *element = NULL;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        element = d_node(dom, ctx, argv[0]);
        if (element == NULL || element->kind != OS64_HTML_ELEMENT || element->ns != OS64_HTML_NS_HTML ||
            element->tag != OS64_HTML_TAG_FORM) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return JS_ThrowTypeError(ctx, "FormData's argument must be a form element");
        }
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        /* A submitter is a submit button of this form; the entries a
         * submission sends with one name it, which libpage does for a
         * person's press but not, yet, for a script's: refused, honestly. */
        return d_dom_exception(dom, ctx, "NotSupportedError", "FormData with a submitter is not supported");
    }
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = form_make(ctx, prototype);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object) || element == NULL) return object;
    if (form_from_element(dom, ctx, object, element) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

enum { M_APPEND, M_DELETE, M_GET, M_GET_ALL, M_HAS, M_SET, M_ENTRIES, M_KEYS, M_VALUES, M_FOR_EACH, M_NEXT };

static JSValue iterator_new(JSContext *ctx, JSValueConst form, int kind)
{
    JSClassID id = os64_js_class_id(&form_iterator_slot);
    JSValue object = JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DFormIterator *it = js_mallocz(ctx, sizeof(*it));
    if (it == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    it->form = JS_DupValue(ctx, form);
    it->kind = kind;
    JS_SetOpaque(object, it);
    return object;
}

/* [name, value], an Array that DEFINES its two elements (WebIDL's
 * CreateArrayFromList), so a setter on Array.prototype hears nothing. */
static JSValue pair(JSContext *ctx, JSValue a, JSValue b)
{
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array) || JS_IsException(a) || JS_IsException(b) ||
        JS_DefinePropertyValueUint32(ctx, array, 0, a, JS_PROP_C_W_E) < 0) {
        if (!JS_IsException(array) && (JS_IsException(a) || JS_IsException(b))) JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueUint32(ctx, array, 1, b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    return array;
}

static JSValue iterator_result(JSContext *ctx, JSValue value, bool done)
{
    JSValue result = JS_NewObject(ctx);
    if (JS_IsException(result) || JS_IsException(value)) {
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, result, "value", value, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, result, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

static JSValue form_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == M_NEXT) {
        DFormIterator *it = JS_GetOpaque(self, os64_js_class_id(&form_iterator_slot));
        if (it == NULL) return JS_ThrowTypeError(ctx, "Expected a FormData iterator");
        /* The entries looked up by index each time: an entry removed or
         * added since the last step is seen as the list now stands. */
        DForm *f = form_state(it->form);
        if (f == NULL || it->index >= f->count) return iterator_result(ctx, JS_UNDEFINED, true);
        const DEntry *e = &f->entries[it->index++];
        JSValue name = JS_NewStringLen(ctx, e->name, e->name_length);
        if (it->kind == F_KEYS) return iterator_result(ctx, name, false);
        if (it->kind == F_VALUES) {
            JS_FreeValue(ctx, name);
            return iterator_result(ctx, entry_value(ctx, e), false);
        }
        return iterator_result(ctx, pair(ctx, name, entry_value(ctx, e)), false);
    }
    DForm *f = form_state(self);
    if (f == NULL) return JS_ThrowTypeError(ctx, "Expected a FormData");
    switch (magic) {
    case M_APPEND: case M_SET: {
        if (argc < 2) return JS_ThrowTypeError(ctx, "%s needs a name and a value", magic == M_SET ? "set" : "append");
        bool blob = blob_state(argv[1]) != NULL;
        if (argc > 2 && !blob) return JS_ThrowTypeError(ctx, "A file name goes only with a Blob");
        DEntry e;
        if (entry_make(dom, ctx, argv[0], argv[1], argc > 2 ? 1 : 0, argc > 2 ? argv[2] : JS_UNDEFINED, &e) < 0)
            return JS_EXCEPTION;
        f = form_state(self);       /* conversions ran page code */
        if (magic == M_SET) {
            /* The first of the name replaced, the rest removed, in place. */
            size_t kept = 0;
            bool replaced = false;
            for (size_t i = 0; i < f->count; i++) {
                if (name_is(&f->entries[i], e.name, e.name_length)) {
                    if (!replaced) {
                        entry_free(ctx, &f->entries[i]);
                        f->entries[kept++] = e;
                        replaced = true;
                    } else entry_free(ctx, &f->entries[i]);
                    continue;
                }
                f->entries[kept++] = f->entries[i];
            }
            f->count = kept;
            if (replaced) return JS_UNDEFINED;
        }
        return form_add(ctx, f, &e) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    case M_DELETE: case M_GET: case M_GET_ALL: case M_HAS: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "A name is needed");
        char *name;
        size_t length;
        if (!engine_text(ctx, argv[0], true, &name, &length)) return JS_EXCEPTION;
        f = form_state(self);
        JSValue result = JS_UNDEFINED;
        if (magic == M_DELETE) {
            size_t kept = 0;
            for (size_t i = 0; i < f->count; i++) {
                if (name_is(&f->entries[i], name, length)) entry_free(ctx, &f->entries[i]);
                else f->entries[kept++] = f->entries[i];
            }
            f->count = kept;
        } else if (magic == M_HAS) {
            bool found = false;
            for (size_t i = 0; i < f->count && !found; i++) found = name_is(&f->entries[i], name, length);
            result = JS_NewBool(ctx, found);
        } else if (magic == M_GET) {
            result = JS_NULL;
            for (size_t i = 0; i < f->count; i++)
                if (name_is(&f->entries[i], name, length)) {
                    result = entry_value(ctx, &f->entries[i]);
                    break;
                }
        } else {
            result = JS_NewArray(ctx);
            uint32_t at = 0;
            for (size_t i = 0; !JS_IsException(result) && i < f->count; i++)
                if (name_is(&f->entries[i], name, length) &&
                    JS_DefinePropertyValueUint32(ctx, result, at++, entry_value(ctx, &f->entries[i]), JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, result);
                    result = JS_EXCEPTION;
                }
        }
        js_free(ctx, name);
        return result;
    }
    case M_ENTRIES: return iterator_new(ctx, self, F_ENTRIES);
    case M_KEYS: return iterator_new(ctx, self, F_KEYS);
    case M_VALUES: return iterator_new(ctx, self, F_VALUES);
    case M_FOR_EACH: {
        if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "forEach needs a function");
        /* Index by index, the list read again after each call. */
        for (size_t i = 0;; i++) {
            f = form_state(self);
            if (f == NULL || i >= f->count) break;
            JSValue args[3] = {entry_value(ctx, &f->entries[i]),
                               JS_NewStringLen(ctx, f->entries[i].name, f->entries[i].name_length), JS_DupValue(ctx, self)};
            JSValue done = JS_IsException(args[0]) || JS_IsException(args[1]) ? JS_EXCEPTION
                         : JS_Call(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, 3, args);
            for (int k = 0; k < 3; k++) JS_FreeValue(ctx, args[k]);
            if (JS_IsException(done)) return done;
            JS_FreeValue(ctx, done);
        }
        return JS_UNDEFINED;
    }
    default: return JS_UNDEFINED;
    }
}

/* ── multipart/form-data, both ways ──────────────────────────────────── */

/* HTML's "convert to a list of name-value pairs": every CR or LF alone made
 * a CR LF pair. */
static bool newlines_normalized(JSContext *ctx, Buf *out, const char *text, size_t n,
                                bool (*put)(JSContext *, Buf *, const char *, size_t))
{
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\r' || text[i] == '\n') {
            if (text[i] == '\r' && i + 1 < n && text[i + 1] == '\n') i++;
            if (!put(ctx, out, "\r\n", 2)) return false;
        } else if (!put(ctx, out, text + i, 1)) return false;
    }
    return true;
}

static bool put_raw(JSContext *ctx, Buf *out, const char *text, size_t n)
{
    return buf_put(ctx, out, text, n);
}

/* A name or file name in a part's header: `"`, CR and LF escaped, and
 * nothing else (HTML's multipart/form-data encoding algorithm). */
static bool put_escaped(JSContext *ctx, Buf *out, const char *text, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char *escape = text[i] == '"' ? "%22" : text[i] == '\n' ? "%0A" : text[i] == '\r' ? "%0D" : NULL;
        if (escape != NULL ? !buf_text(ctx, out, escape) : !buf_put(ctx, out, text + i, 1)) return false;
    }
    return true;
}

static bool contains(const uint8_t *hay, size_t n, const char *needle, size_t m)
{
    for (size_t i = 0; m <= n && i + m <= n; i++)
        if (hay[i] == (uint8_t)needle[0] && os64_memcmp(hay + i, needle, m) == 0) return true;
    return false;
}

int d_form_data_encode(os64_dom_t *dom, JSContext *ctx, JSValueConst form, DBytes *out)
{
    (void)dom;
    os64_memset(out, 0, sizeof(*out));
    DForm *f = form_state(form);
    if (f == NULL) {
        JS_ThrowTypeError(ctx, "Expected a FormData");
        return -1;
    }
    /* The boundary is the content's hash, lengthened until no entry holds
     * it, as libpage's form encoder chooses its own. */
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < f->count; i++) {
        const DEntry *e = &f->entries[i];
        for (size_t k = 0; k < e->name_length; k++) hash = (hash ^ (uint8_t)e->name[k]) * 0x100000001b3ull;
        for (size_t k = 0; k < e->value_length; k++) hash = (hash ^ (uint8_t)e->value[k]) * 0x100000001b3ull;
    }
    char boundary[80];
    size_t blen = (size_t)os64_snprintf(boundary, sizeof(boundary), "----os64FormBoundary%016lx", (unsigned long)hash);
    for (bool clashes = true; clashes;) {
        clashes = false;
        for (size_t i = 0; i < f->count && !clashes; i++) {
            const DEntry *e = &f->entries[i];
            const uint8_t *bytes;
            size_t length;
            const char *type;
            clashes = contains((const uint8_t *)e->name, e->name_length, boundary, blen) ||
                      (e->value != NULL && contains((const uint8_t *)e->value, e->value_length, boundary, blen)) ||
                      (d_blob_get(e->file, &bytes, &length, &type) && contains(bytes, length, boundary, blen));
        }
        if (clashes) {
            if (blen + 1 >= 70) {
                JS_ThrowTypeError(ctx, "No boundary could be found for this form's data");
                return -1;
            }
            boundary[blen++] = '-';
            boundary[blen] = '\0';
        }
    }
    Buf b = {0};
    bool ok = true;
    for (size_t i = 0; ok && i < f->count; i++) {
        const DEntry *e = &f->entries[i];
        ok = buf_text(ctx, &b, "--") && buf_put(ctx, &b, boundary, blen) &&
             buf_text(ctx, &b, "\r\nContent-Disposition: form-data; name=\"");
        /* The name: its newlines normalized, then escaped. */
        Buf name = {0};
        ok = ok && newlines_normalized(ctx, &name, e->name, e->name_length, put_raw) &&
             put_escaped(ctx, &b, (const char *)name.bytes, name.length) && buf_text(ctx, &b, "\"");
        js_free(ctx, name.bytes);
        DBlob *file = blob_state(e->file);
        if (ok && file != NULL) {
            ok = buf_text(ctx, &b, "; filename=\"") &&
                 put_escaped(ctx, &b, file->name != NULL ? file->name : "", file->name != NULL ? os64_strlen(file->name) : 0) &&
                 buf_text(ctx, &b, "\"\r\nContent-Type: ") &&
                 buf_text(ctx, &b, file->type[0] != '\0' ? file->type : "application/octet-stream") &&
                 buf_text(ctx, &b, "\r\n\r\n") && buf_put(ctx, &b, file->bytes, file->length) &&
                 buf_text(ctx, &b, "\r\n");
        } else if (ok) {
            ok = buf_text(ctx, &b, "\r\n\r\n") &&
                 newlines_normalized(ctx, &b, e->value, e->value_length, put_raw) && buf_text(ctx, &b, "\r\n");
        }
    }
    /* The close delimiter ends every multipart body, an empty entry list's
     * included: HTML's encoding is RFC 7578's, whose body (RFC 2046)
     * always ends with it, and Chrome and Firefox send it for an empty
     * FormData too (a body of the close delimiter alone). */
    ok = ok && buf_text(ctx, &b, "--") && buf_put(ctx, &b, boundary, blen) && buf_text(ctx, &b, "--\r\n");
    char type[128];
    os64_snprintf(type, sizeof(type), "multipart/form-data; boundary=%s", boundary);
    out->type = ok ? engine_copy(ctx, type, os64_strlen(type)) : NULL;
    if (out->type == NULL) {
        js_free(ctx, b.bytes);
        return -1;
    }
    out->bytes = b.bytes;
    out->length = b.length;
    return 0;
}

/* A MIME type's (or a Content-Disposition's) parameter `name`, matched
 * without regard to case: 1 when it is PRESENT, its value, quoted or not,
 * in `out` ("" for an empty one, which is still present); 0 when absent;
 * -1 when present and longer than `cap` holds, so that no caller reads a
 * value cut short as if it were whole. */
static int mime_parameter(const char *type, const char *name, char *out, size_t cap)
{
    size_t nlen = os64_strlen(name);
    const char *p = os64_strchr(type, ';');
    while (p != NULL) {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        const char *key = p;
        while (*p != '\0' && *p != '=' && *p != ';') p++;
        bool match = (size_t)(p - key) == nlen;
        for (size_t i = 0; match && i < nlen; i++)
            if ((key[i] | 0x20) != name[i]) match = false;
        if (*p == '=' && match) {
            p++;
            size_t n = 0;
            bool whole;
            if (*p == '"') {
                p++;
                while (*p != '\0' && *p != '"' && n + 1 < cap) {
                    if (*p == '\\' && p[1] != '\0') p++;
                    out[n++] = *p++;
                }
                whole = *p == '\0' || *p == '"';
            } else {
                while (*p != '\0' && *p != ';' && *p != ' ' && *p != '\t' && n + 1 < cap) out[n++] = *p++;
                whole = *p == '\0' || *p == ';' || *p == ' ' || *p == '\t';
            }
            out[n] = '\0';
            return whole ? 1 : -1;
        }
        /* Another parameter: its whole value is passed over, a quoted one
         * to its closing quote with its escapes, before the next ';' is
         * looked for, so `name="a; filename=b"` names no filename. */
        if (*p == '=' && p[1] == '"') {
            p += 2;
            while (*p != '\0' && *p != '"') p += p[0] == '\\' && p[1] != '\0' ? 2 : 1;
        }
        p = os64_strchr(p, ';');
    }
    if (cap > 0) out[0] = '\0';
    return 0;
}

static int hex_digit(uint8_t c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* application/x-www-form-urlencoded's parser (URL § 5.1), into `form`. */
static int urlencoded_parse(JSContext *ctx, JSValueConst form, const uint8_t *bytes, size_t length)
{
    for (size_t at = 0; at <= length;) {
        size_t end = at;
        while (end < length && bytes[end] != '&') end++;
        if (end > at) {
            size_t eq = at;
            while (eq < end && bytes[eq] != '=') eq++;
            char *parts[2] = {NULL, NULL};
            size_t lengths[2] = {0, 0};
            size_t from[2] = {at, eq < end ? eq + 1 : end}, to[2] = {eq, end};
            for (int k = 0; k < 2; k++) {
                parts[k] = js_malloc(ctx, to[k] - from[k] + 1);
                if (parts[k] == NULL) {
                    js_free(ctx, parts[0]);
                    return -1;
                }
                size_t n = 0;
                for (size_t i = from[k]; i < to[k]; i++) {
                    uint8_t c = bytes[i];
                    if (c == '+') c = ' ';
                    else if (c == '%' && i + 2 < to[k] && hex_digit(bytes[i + 1]) >= 0 && hex_digit(bytes[i + 2]) >= 0) {
                        c = (uint8_t)(hex_digit(bytes[i + 1]) * 16 + hex_digit(bytes[i + 2]));
                        i += 2;
                    }
                    parts[k][n++] = (char)c;
                }
                parts[k][n] = '\0';
                lengths[k] = n;
            }
            /* The bytes are "UTF-8 decode without BOM"ed (the urlencoded
             * parser's own words): an invalid sequence is U+FFFD, and a
             * leading U+FEFF is part of the name or value. */
            int result = 0;
            JSValue name = d_utf8_decode_without_bom(ctx, (const uint8_t *)parts[0], lengths[0]);
            JSValue value = JS_IsException(name) ? JS_EXCEPTION
                                                 : d_utf8_decode_without_bom(ctx, (const uint8_t *)parts[1], lengths[1]);
            js_free(ctx, parts[0]);
            js_free(ctx, parts[1]);
            char *n8 = NULL, *v8 = NULL;
            size_t nl = 0, vl = 0;
            if (JS_IsException(value) || !engine_text(ctx, name, true, &n8, &nl) || !engine_text(ctx, value, true, &v8, &vl))
                result = -1;
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, value);
            if (result == 0) result = form_entries_add(ctx, form, n8, nl, v8, vl);
            js_free(ctx, n8);
            js_free(ctx, v8);
            if (result < 0) return -1;
        }
        at = end + 1;
    }
    return 0;
}

/* A multipart/form-data body's parts, into `form`. -1 with a TypeError when
 * it is not one. */
/* What follows "--boundary" at `at` (RFC 2046 § 5.1.1): "--", the close
 * (2); transport padding (spaces and tabs) then CRLF, a part follows, from
 * *next (1); anything else, which makes it no delimiter at all, only bytes
 * that begin like one (0). */
static int delimiter_tail(const uint8_t *bytes, size_t length, size_t at, size_t *next)
{
    if (at + 2 <= length && bytes[at] == '-' && bytes[at + 1] == '-') return 2;
    while (at < length && (bytes[at] == ' ' || bytes[at] == '\t')) at++;
    if (at + 2 > length || bytes[at] != '\r' || bytes[at + 1] != '\n') return 0;
    *next = at + 2;
    return 1;
}

static int multipart_parse(os64_dom_t *dom, JSContext *ctx, JSValueConst form, const uint8_t *bytes, size_t length,
                           const char *boundary)
{
    char delimiter[96];
    size_t dlen = (size_t)os64_snprintf(delimiter, sizeof(delimiter), "--%s", boundary);
    size_t at = 0;
    int tail = 0;
    /* The first delimiter: at the start, or after a preamble and its CRLF
     * (RFC 2046 § 5.1.1: [preamble CRLF] dash-boundary), the first that is
     * a whole delimiter line. */
    if (dlen < sizeof(delimiter))
        for (size_t s = 0; tail == 0 && s + dlen <= length; s++)
            if ((s == 0 || (s >= 2 && bytes[s - 2] == '\r' && bytes[s - 1] == '\n')) &&
                os64_memcmp(bytes + s, delimiter, dlen) == 0)
                tail = delimiter_tail(bytes, length, s + dlen, &at);
    for (;;) {
        if (tail == 2) return 0;   /* the close */
        if (tail == 0) break;
        /* The part's headers, to the blank line. */
        char name[1024] = "", file_name[1024] = "", type[256] = "";
        bool has_name = false, is_file = false;
        for (;;) {
            size_t end = at;
            while (end + 1 < length && !(bytes[end] == '\r' && bytes[end + 1] == '\n')) end++;
            if (end + 1 >= length) goto refused;
            if (end == at) {
                at += 2;
                break;
            }
            /* A part's header line is read whole or the body is refused: one
             * longer than the line holds, or holding a NUL, would be read
             * from a prefix, a name cut short and a filename past the cut
             * gone (a file read as a string). The same holds for each value
             * below that its buffer cannot hold whole. */
            char line[2048];
            size_t n = end - at;
            if (n >= sizeof(line) || os64_memchr(bytes + at, '\0', n) != NULL) goto refused;
            os64_memcpy(line, bytes + at, n);
            line[n] = '\0';
            /* Each header recognised by its whole name, however short its
             * value ("Content-Type: a/b" is one). */
            const char *colon = os64_strchr(line, ':');
            if (colon != NULL) {
                size_t key = (size_t)(colon - line);
                bool disposition = key == 19, content_type = key == 12;
                for (size_t i = 0; disposition && i < key; i++)
                    if ((line[i] | 0x20) != "content-disposition"[i]) disposition = false;
                for (size_t i = 0; content_type && i < key; i++)
                    if ((line[i] | 0x20) != "content-type"[i]) content_type = false;
                const char *value = colon + 1;
                while (*value == ' ' || *value == '\t') value++;
                if (disposition) {
                    /* A part with a name parameter, an empty one included,
                     * is an entry; one with a filename parameter (in any
                     * case, an empty one included) is a file. */
                    int got_name = mime_parameter(value, "name", name, sizeof(name));
                    int got_file = mime_parameter(value, "filename", file_name, sizeof(file_name));
                    if (got_name < 0 || got_file < 0) goto refused;
                    has_name = got_name > 0;
                    is_file = got_file > 0;
                } else if (content_type && os64_strcopy(type, sizeof(type), value) >= sizeof(type))
                    goto refused;
            }
            at = end + 2;
        }
        if (!has_name) goto refused;
        /* The part's body, to the next CRLF delimiter: "--boundary" then
         * the close or padding and CRLF. A body line that only begins with
         * the boundary ("--b-extra" for "b") is the body's. */
        size_t end = at, next = 0;
        tail = 0;
        for (; end + 2 + dlen <= length; end++)
            if (bytes[end] == '\r' && bytes[end + 1] == '\n' && os64_memcmp(bytes + end + 2, delimiter, dlen) == 0 &&
                (tail = delimiter_tail(bytes, length, end + 2 + dlen, &next)) != 0)
                break;
        if (tail == 0) goto refused;
        if (is_file) {
            uint8_t *copy = js_malloc(ctx, end - at + 1);
            char *t = copy != NULL ? type_normalized(ctx, type, os64_strlen(type)) : NULL;
            char *fn = t != NULL ? engine_copy(ctx, file_name, os64_strlen(file_name)) : NULL;
            char *nm = fn != NULL ? engine_copy(ctx, name, os64_strlen(name)) : NULL;
            if (nm == NULL) {
                js_free(ctx, copy);
                js_free(ctx, t);
                js_free(ctx, fn);
                return -1;
            }
            if (end > at) os64_memcpy(copy, bytes + at, end - at);
            JSValue prototype = dom->file_prototype != NULL ? JS_DupValue(ctx, dom->file_prototype->value) : JS_UNDEFINED;
            DEntry e = {nm, NULL, os64_strlen(name), 0,
                        blob_make(ctx, prototype, copy, end - at, t, true, fn, now_epoch_ms())};
            JS_FreeValue(ctx, prototype);
            if (JS_IsException(e.file) || form_add(ctx, form_state(form), &e) < 0) return -1;
        } else {
            JSValue value = d_utf8_decode(ctx, bytes + at, end - at);
            char *v8;
            size_t vl;
            if (JS_IsException(value)) return -1;
            bool got = engine_text(ctx, value, true, &v8, &vl);
            JS_FreeValue(ctx, value);
            if (!got) return -1;
            int added = form_entries_add(ctx, form, name, os64_strlen(name), v8, vl);
            js_free(ctx, v8);
            if (added < 0) return -1;
        }
        at = next;
    }
refused:
    JS_ThrowTypeError(ctx, "The body is not multipart/form-data");
    return -1;
}

JSValue d_form_data_parse(os64_dom_t *dom, JSContext *ctx, const uint8_t *bytes, size_t length, const char *type)
{
    char essence[WAY_MIME_ESSENCE_MAX];
    if (type == NULL || !way_mime_parse(type, os64_strlen(type), essence, sizeof(essence), NULL, 0))
        return JS_ThrowTypeError(ctx, "The body's type is not a form's");
    bool urlencoded = os64_streq(essence, "application/x-www-form-urlencoded");
    char boundary[80];
    if (!urlencoded && (!os64_streq(essence, "multipart/form-data") || mime_parameter(type, "boundary", boundary, sizeof(boundary)) <= 0 ||
                        boundary[0] == '\0'))
        return JS_ThrowTypeError(ctx, "The body's type is not a form's");
    JSValue prototype = dom->form_prototype != NULL ? JS_DupValue(ctx, dom->form_prototype->value) : JS_UNDEFINED;
    JSValue form = form_make(ctx, prototype);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(form)) return form;
    int parsed = urlencoded ? urlencoded_parse(ctx, form, bytes, length)
                            : multipart_parse(dom, ctx, form, bytes, length, boundary);
    if (parsed < 0) {
        JS_FreeValue(ctx, form);
        return JS_EXCEPTION;
    }
    return form;
}

/* ── Installation ────────────────────────────────────────────────────── */

int d_blob_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef blob_class = {.class_name = "Blob", .finalizer = blob_finalizer};
    static const JSClassDef form_class = {.class_name = "FormData", .finalizer = form_finalizer, .gc_mark = form_mark};
    static const JSClassDef iterator_class = {.class_name = "FormData Iterator", .finalizer = form_iterator_finalizer,
                                              .gc_mark = form_iterator_mark};
    JSClassID blob_id = os64_js_class_id(&blob_slot), form_id = os64_js_class_id(&form_slot),
              iterator_id = os64_js_class_id(&form_iterator_slot);
    if ((!JS_IsRegisteredClass(dom->engine, blob_id) && JS_NewClass(dom->engine, blob_id, &blob_class) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, form_id) && JS_NewClass(dom->engine, form_id, &form_class) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, iterator_id) && JS_NewClass(dom->engine, iterator_id, &iterator_class) < 0))
        return -1;
    DValue *blob = d_retain(dom, ctx, JS_NewObject(ctx));
    if (blob == NULL) return -1;
    dom->file_prototype = d_retain(dom, ctx, JS_NewObjectProto(ctx, blob->value));
    dom->form_prototype = dom->file_prototype != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (dom->form_prototype == NULL) return -1;
    /* The iterator's prototype inherits %IteratorPrototype%, read off an
     * Array's iterator's before any page could change it. */
    JSValue global_iterator = JS_GetPropertyStr(ctx, global, "Iterator");
    JSValue iterator_parent = JS_IsException(global_iterator) ? JS_EXCEPTION
                            : JS_GetPropertyStr(ctx, global_iterator, "prototype");
    JS_FreeValue(ctx, global_iterator);
    if (JS_IsException(iterator_parent)) return -1;
    DValue *iterator = d_retain(dom, ctx, JS_NewObjectProto(ctx, iterator_parent));
    JS_FreeValue(ctx, iterator_parent);
    if (iterator == NULL) return -1;
    JS_SetClassProto(ctx, blob_id, JS_DupValue(ctx, blob->value));
    JS_SetClassProto(ctx, form_id, JS_DupValue(ctx, dom->form_prototype->value));
    JS_SetClassProto(ctx, iterator_id, JS_DupValue(ctx, iterator->value));
    JSValueConst bp = blob->value, fp = dom->file_prototype->value, dp = dom->form_prototype->value;
    static const struct { const char *name; int magic, argc; bool accessor; } kBlob[] = {
        {"size", B_SIZE, 0, true}, {"type", B_TYPE, 0, true}, {"slice", B_SLICE, 0, false},
        {"text", B_TEXT, 0, false}, {"arrayBuffer", B_ARRAY_BUFFER, 0, false}, {"bytes", B_BYTES, 0, false},
        {"stream", B_STREAM, 0, false},
    };
    for (size_t i = 0; i < sizeof(kBlob) / sizeof(kBlob[0]); i++)
        if ((kBlob[i].accessor ? d_accessor(dom, ctx, bp, kBlob[i].name, blob_member, kBlob[i].magic, false)
                               : d_method(dom, ctx, bp, kBlob[i].name, blob_member, kBlob[i].argc, kBlob[i].magic)) < 0)
            return -1;
    if (d_accessor(dom, ctx, fp, "name", blob_member, B_NAME, false) < 0 ||
        d_accessor(dom, ctx, fp, "lastModified", blob_member, B_LAST_MODIFIED, false) < 0 ||
        d_accessor(dom, ctx, fp, "webkitRelativePath", blob_member, B_RELATIVE, false) < 0)
        return -1;
    static const struct { const char *name; int magic, argc; } kForm[] = {
        {"append", M_APPEND, 2}, {"delete", M_DELETE, 1}, {"get", M_GET, 1}, {"getAll", M_GET_ALL, 1},
        {"has", M_HAS, 1}, {"set", M_SET, 2}, {"entries", M_ENTRIES, 0}, {"keys", M_KEYS, 0},
        {"values", M_VALUES, 0}, {"forEach", M_FOR_EACH, 1},
    };
    for (size_t i = 0; i < sizeof(kForm) / sizeof(kForm[0]); i++)
        if (d_method(dom, ctx, dp, kForm[i].name, form_member, kForm[i].argc, kForm[i].magic) < 0) return -1;
    if (d_iterator_next(dom, ctx, iterator->value, form_member, M_NEXT) < 0) return -1;
    /* [Symbol.iterator] is entries (WebIDL § 3.7.9.1), and the iterator says
     * what it is. */
    JSAtom iterator_atom = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
    JSValue entries = iterator_atom == JS_ATOM_NULL ? JS_EXCEPTION : JS_GetPropertyStr(ctx, dp, "entries");
    int linked = JS_IsException(entries) ? -1
        : JS_DefinePropertyValue(ctx, dp, iterator_atom, entries, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    if (iterator_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, iterator_atom);
    if (linked < 0 || d_string_tag(ctx, iterator->value, "FormData Iterator") < 0) return -1;
    JSValue blob_interface = d_interface_new(ctx, "Blob", 0, blob_constructor, bp, JS_UNDEFINED);
    if (JS_IsException(blob_interface)) return -1;
    JSValue file_interface = d_interface_new(ctx, "File", 2, file_constructor, fp, blob_interface);
    if (JS_IsException(file_interface)) {
        JS_FreeValue(ctx, blob_interface);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, global, "Blob", blob_interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, file_interface);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, global, "File", file_interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    JSValue form_interface = d_interface_new(ctx, "FormData", 0, form_constructor, dp, JS_UNDEFINED);
    if (JS_IsException(form_interface)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, "FormData", form_interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}
