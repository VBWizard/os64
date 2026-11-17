#include "internal.h"
#include "os64/fmt.h"
#include "way/origin.h"

/* FileReader (the File API's § 8; BLOB_URLS.md decision 10): a Blob's
 * bytes read as an ArrayBuffer, a binary string, text or a data: URL.
 * A Blob here holds its bytes whole, so a read never waits: starting one
 * puts the reader in LOADING and queues ONE task on the posted-message
 * queue (d_message_task, its source "file reading"), which fires loadstart
 * and one progress, sets DONE and the result, fires load (or error), and
 * then loadend only if the reader is not LOADING again: a load handler
 * that starts the next read, the chunked-reading loop, suppresses the
 * first read's loadend, as the standard says.
 *
 * `generation` names the read under way: the queued task carries it, and
 * abort() and each new read move it, so a task that finds another
 * generation does nothing. abort() during LOADING fires abort then
 * loadend at once, inside the call; in EMPTY or DONE it only clears the
 * result. A reader is a collectable plain event target (event.c); the
 * queued task holds it until it runs, as the standard keeps an object with
 * pending activity, and the Blob it reads is held until the read ends. */

static JSClassID reader_slot;

enum { R_EMPTY, R_LOADING, R_DONE };
static const char *const kStates[] = {"EMPTY", "LOADING", "DONE"};
enum { K_ARRAY_BUFFER, K_BINARY_STRING, K_TEXT, K_DATA_URL };

/* The engine's largest string, in characters, and largest ArrayBuffer: a
 * result past either is a NotReadableError, not an allocation the engine
 * refuses mid-task. */
#define READER_STRING_MAX ((size_t)(1u << 30) - 1)
#define READER_BUFFER_MAX ((size_t)INT32_MAX)

typedef struct {
    DValue target;              /* first: the collectable plain target's record */
    int state, kind;
    uint32_t generation;
    JSValue blob;               /* the Blob being read; JS_UNDEFINED between reads */
    JSValue result, error;      /* JS_NULL until a read gives one */
    char *label;                /* readAsText's encoding label, engine memory; NULL for none */
} DReader;

static DReader *reader_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&reader_slot));
}

static void reader_finalizer(JSRuntime *rt, JSValue value)
{
    DReader *r = reader_state(value);
    if (r == NULL) return;
    d_event_target_finalize(rt, &r->target);
    JS_FreeValueRT(rt, r->blob);
    JS_FreeValueRT(rt, r->result);
    JS_FreeValueRT(rt, r->error);
    js_free_rt(rt, r->label);
    js_free_rt(rt, r);
}

static void reader_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DReader *r = reader_state(value);
    if (r == NULL) return;
    d_event_target_mark(rt, &r->target, mark);
    JS_MarkValue(rt, r->blob, mark);
    JS_MarkValue(rt, r->result, mark);
    JS_MarkValue(rt, r->error, mark);
}

/* `value` (taken) in `*slot`, the old one freed. */
static void slot_set(JSContext *ctx, JSValue *slot, JSValue value)
{
    JS_FreeValue(ctx, *slot);
    *slot = value;
}

/* The read's Blob let go, and its label. */
static void read_end(JSContext *ctx, DReader *r)
{
    slot_set(ctx, &r->blob, JS_UNDEFINED);
    js_free(ctx, r->label);
    r->label = NULL;
}

/* A ProgressEvent `type` at the reader: from the read's task (`outcome`),
 * each listener the task's call; from a page's own call (abort()), a
 * nested one. -1 when the runtime is finished with. */
static int fire(os64_dom_t *dom, JSContext *ctx, DReader *r, const char *type, double loaded, double total,
                os64_js_outcome_t *outcome)
{
    JSValue event = d_progress_event_new(dom, ctx, type, true, loaded, total, true);
    if (JS_IsException(event)) return -1;
    int result = outcome != NULL ? d_dispatch_plain_task(dom, ctx, event, &r->target, outcome)
                                 : d_dispatch_plain(dom, ctx, event, &r->target);
    JS_FreeValue(ctx, event);
    return result < 0 ? -1 : 0;
}

/* `length` bytes as a string of as many code units, each a byte's value
 * (the File API's binary string): Latin-1 written as the engine's UTF-8. */
static JSValue binary_string(JSContext *ctx, const uint8_t *bytes, size_t length)
{
    char *out = js_malloc(ctx, length * 2 + 1);
    if (out == NULL) return JS_EXCEPTION;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] < 0x80) {
            out[used++] = (char)bytes[i];
        } else {
            out[used++] = (char)(0xc0 | (bytes[i] >> 6));
            out[used++] = (char)(0x80 | (bytes[i] & 0x3f));
        }
    }
    JSValue text = JS_NewStringLen(ctx, out, used);
    js_free(ctx, out);
    return text;
}

/* `data:<type>;base64,<bytes>`, application/octet-stream for a Blob with
 * no type. */
static JSValue data_url(JSContext *ctx, const uint8_t *bytes, size_t length, const char *type)
{
    const char *essence = type != NULL && type[0] != '\0' ? type : "application/octet-stream";
    size_t head = 5 + os64_strlen(essence) + 8, body = (length + 2) / 3 * 4;
    char *out = js_malloc(ctx, head + body + 1);
    if (out == NULL) return JS_EXCEPTION;
    os64_snprintf(out, head + 1, "data:%s;base64,", essence);
    size_t used = head + d_base64_write(bytes, length, out + head);
    JSValue url = JS_NewStringLen(ctx, out, used);
    js_free(ctx, out);
    return url;
}

/* The File API's "package data": the encoding readAsText decodes with is
 * the label given when it names one this browser decodes, else the Blob's
 * charset parameter when that does, else UTF-8; the decode sniffs a BOM,
 * which outranks all three (d_text_decode). JS_NULL for a result past the
 * engine's limits, JS_EXCEPTION for an engine refusal. */
static JSValue package(JSContext *ctx, const DReader *r, const uint8_t *bytes, size_t length, const char *type)
{
    size_t data_url_length = (length + 2) / 3 * 4;
    switch (r->kind) {
    case K_ARRAY_BUFFER:
        return length > READER_BUFFER_MAX ? JS_NULL : JS_NewArrayBufferCopy(ctx, bytes, length);
    case K_BINARY_STRING:
        return length > READER_STRING_MAX ? JS_NULL : binary_string(ctx, bytes, length);
    case K_TEXT: {
        if (length > READER_STRING_MAX) return JS_NULL;
        char charset[WAY_MIME_CHARSET_MAX], essence[WAY_MIME_ESSENCE_MAX];
        const char *label = "utf-8";
        if (r->label != NULL && d_text_decodes(r->label))
            label = r->label;
        else if (type != NULL &&
                 way_mime_parse(type, os64_strlen(type), essence, sizeof(essence), charset, sizeof(charset)) &&
                 d_text_decodes(charset))
            label = charset;
        return d_text_decode(ctx, label, bytes, length);
    }
    default:
        return length / 3 > READER_STRING_MAX / 4 || data_url_length > READER_STRING_MAX - 128
            ? JS_NULL : data_url(ctx, bytes, length, type);
    }
}

/* The read's task: the read named by `token` run to its end, unless an
 * abort() or another read moved the reader on since it was queued. */
static void read_task(os64_dom_t *dom, JSValueConst self, uint32_t token, os64_js_outcome_t *outcome)
{
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#file-reading", (int)(sizeof(name) - 20),
                  dom->url != NULL ? dom->url : "about:blank");
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, name, outcome) != OS64_JS_OK) return;
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
    if (ctx == NULL) d_outcome_fold(outcome, &access);
    DReader *r = ctx != NULL ? reader_state(self) : NULL;
    const uint8_t *bytes = NULL;
    size_t length = 0;
    const char *type = NULL;
    bool live = r != NULL && r->generation == token && r->state == R_LOADING &&
                d_blob_get(r->blob, &bytes, &length, &type);
    double total = (double)length;
    if (live && fire(dom, ctx, r, "loadstart", 0, total, outcome) < 0) live = false;
    live = live && d_delivery_active(dom) && reader_state(self)->generation == token;
    if (live && fire(dom, ctx, r, "progress", total, total, outcome) < 0) live = false;
    live = live && d_delivery_active(dom) && reader_state(self)->generation == token;
    if (live) {
        /* The Blob is still held, so its bytes are where they were. */
        d_blob_get(r->blob, &bytes, &length, &type);
        JSValue result = package(ctx, r, bytes, length, type);
        bool refused = JS_IsException(result);
        if (refused && d_exception_catchable(ctx)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            result = JS_NULL;
            refused = false;
        }
        if (!refused) {
            r->state = R_DONE;
            read_end(ctx, r);
            const char *event = "load";
            if (JS_IsNull(result)) {
                slot_set(ctx, &r->error, d_dom_exception_new(dom, ctx, "NotReadableError",
                                                             "The Blob is too large to read as that"));
                event = "error";
            } else {
                slot_set(ctx, &r->result, result);
            }
            if (fire(dom, ctx, r, event, total, total, outcome) == 0 && d_delivery_active(dom) &&
                reader_state(self)->state != R_LOADING)
                fire(dom, ctx, r, "loadend", total, total, outcome);
        }
    }
    d_task_finish(dom, outcome);
    d_task_close(dom, name, outcome);
}

/* readAsArrayBuffer, readAsBinaryString, readAsText and readAsDataURL:
 * the File API's "read operation" begun. */
static JSValue read_start(os64_dom_t *dom, JSContext *ctx, JSValueConst self, DReader *r, int kind, int argc,
                          JSValueConst *argv)
{
    const uint8_t *bytes;
    size_t length;
    const char *type;
    if (argc < 1 || !d_blob_get(argv[0], &bytes, &length, &type))
        return JS_ThrowTypeError(ctx, "Failed to read: parameter 1 is not of type 'Blob'");
    char *label = NULL;
    if (kind == K_TEXT && argc > 1 && !JS_IsUndefined(argv[1])) {
        size_t n;
        const char *text = JS_ToCStringLen(ctx, &n, argv[1]);
        if (text == NULL) return JS_EXCEPTION;
        label = js_malloc(ctx, n + 1);
        if (label != NULL) os64_memcpy(label, text, n + 1);
        JS_FreeCString(ctx, text);
        if (label == NULL) return JS_EXCEPTION;
        r = reader_state(self);
    }
    if (r->state == R_LOADING) {
        js_free(ctx, label);
        return d_dom_exception(dom, ctx, "InvalidStateError", "The reader is already reading a Blob");
    }
    uint32_t generation = r->generation + 1;
    if (d_message_task(dom, ctx, "file reading", read_task, self, generation) < 0) {
        js_free(ctx, label);
        return JS_EXCEPTION;
    }
    r->generation = generation;
    r->state = R_LOADING;
    r->kind = kind;
    slot_set(ctx, &r->result, JS_NULL);
    slot_set(ctx, &r->error, JS_NULL);
    js_free(ctx, r->label);
    r->label = label;
    slot_set(ctx, &r->blob, JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue reader_abort(os64_dom_t *dom, JSContext *ctx, JSValueConst self, DReader *r)
{
    if (r->state != R_LOADING) {
        slot_set(ctx, &r->result, JS_NULL);
        return JS_UNDEFINED;
    }
    r->state = R_DONE;
    r->generation++;
    slot_set(ctx, &r->result, JS_NULL);
    read_end(ctx, r);
    if (fire(dom, ctx, r, "abort", 0, 0, NULL) < 0) return JS_EXCEPTION;
    r = reader_state(self);
    if (r->state != R_LOADING && fire(dom, ctx, r, "loadend", 0, 0, NULL) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

enum { M_READY_STATE, M_RESULT, M_ERROR, M_ARRAY_BUFFER, M_BINARY_STRING, M_TEXT, M_DATA_URL, M_ABORT };

static JSValue reader_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DReader *r = reader_state(self);
    if (r == NULL) return JS_ThrowTypeError(ctx, "Expected a FileReader");
    dom = r->target.dom;
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    switch (magic) {
    case M_READY_STATE: return JS_NewInt32(ctx, r->state);
    case M_RESULT: return JS_DupValue(ctx, r->result);
    case M_ERROR: return JS_DupValue(ctx, r->error);
    case M_ARRAY_BUFFER: return read_start(dom, ctx, self, r, K_ARRAY_BUFFER, argc, argv);
    case M_BINARY_STRING: return read_start(dom, ctx, self, r, K_BINARY_STRING, argc, argv);
    case M_TEXT: return read_start(dom, ctx, self, r, K_TEXT, argc, argv);
    case M_DATA_URL: return read_start(dom, ctx, self, r, K_DATA_URL, argc, argv);
    case M_ABORT: return reader_abort(dom, ctx, self, r);
    default: return JS_UNDEFINED;
    }
}

static JSValue reader_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSClassID id = os64_js_class_id(&reader_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DReader *r = js_mallocz(ctx, sizeof(*r));
    if (r == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &r->target, object);
    r->blob = JS_UNDEFINED;
    r->result = r->error = JS_NULL;
    JS_SetOpaque(object, r);
    return object;
}

int d_file_reader_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef reader_class = {.class_name = "FileReader", .finalizer = reader_finalizer,
                                            .gc_mark = reader_mark};
    JSClassID id = os64_js_class_id(&reader_slot);
    /* The events it fires are ProgressEvents: installed here unless the
     * XHR row has (a second install would make a second prototype). */
    if (dom->progress_prototype == NULL && d_progress_event_install(dom, ctx, global) < 0) return -1;
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &reader_class) < 0) return -1;
    DValue *proto = d_retain(dom, ctx, JS_NewObject(ctx));
    if (proto == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, proto->value));
    if (d_event_target(dom, ctx, proto->value, id, NULL) < 0) return -1;
    static const char *const kHandlers[] = {"loadstart", "progress", "load", "abort", "error", "loadend"};
    for (size_t i = 0; i < sizeof(kHandlers) / sizeof(kHandlers[0]); i++)
        if (d_event_handler_named(dom, ctx, proto->value, kHandlers[i]) < 0) return -1;
    static const struct { const char *name; int magic, argc; bool method; } kMembers[] = {
        {"readyState", M_READY_STATE, 0, false}, {"result", M_RESULT, 0, false}, {"error", M_ERROR, 0, false},
        {"readAsArrayBuffer", M_ARRAY_BUFFER, 1, true}, {"readAsBinaryString", M_BINARY_STRING, 1, true},
        {"readAsText", M_TEXT, 1, true}, {"readAsDataURL", M_DATA_URL, 1, true}, {"abort", M_ABORT, 0, true},
    };
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++)
        if ((kMembers[i].method
                 ? d_method(dom, ctx, proto->value, kMembers[i].name, reader_member, kMembers[i].argc, kMembers[i].magic)
                 : d_accessor(dom, ctx, proto->value, kMembers[i].name, reader_member, kMembers[i].magic, false)) < 0)
            return -1;
    JSValue interface = d_interface_new(ctx, "FileReader", 0, reader_constructor, proto->value, JS_UNDEFINED);
    if (JS_IsException(interface)) return -1;
    /* WebIDL constants: on the interface object and its prototype. */
    for (int i = 0; i < 3; i++)
        if (JS_DefinePropertyValueStr(ctx, interface, kStates[i], JS_NewInt32(ctx, i), JS_PROP_ENUMERABLE) < 0 ||
            JS_DefinePropertyValueStr(ctx, proto->value, kStates[i], JS_NewInt32(ctx, i), JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(ctx, interface);
            return -1;
        }
    return JS_DefinePropertyValueStr(ctx, global, "FileReader", interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0
        ? -1 : 0;
}
