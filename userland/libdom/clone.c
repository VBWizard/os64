#include "internal.h"

/* STRUCTURED CLONE (HTML § 2.7, "Safe passing of structured data"): the
 * StructuredSerializeWithTransfer and StructuredDeserializeWithTransfer
 * pair, and structuredClone, P2b's row (WINDOW_DOCUMENT.md § P2b). The
 * pair is the binding's one copy of the algorithm, which every clone a
 * page asks for goes through, so no two of them can disagree about what a
 * value's copy is.
 *
 * THE RECORD IS NO REALM'S. d_clone_write reads a value in the realm of
 * the context it is given and d_clone_read builds the copy in the realm of
 * the context IT is given, each through the intrinsics its own binding took
 * before any page ran (d_clone_setup), so the frame slice can hand the
 * record from one realm to another (IFRAME.md § The cluster and the window
 * proxy, postMessage) with nothing here reshaped. What the record holds is
 * the runtime's, which every realm of a page shares: primitives (strings
 * and BigInts are the runtime's, not a realm's), property keys as atoms,
 * and bytes, all in engine memory, so a page's clone is charged to the
 * memory a page's own values are.
 *
 * WHAT IS CLONED, as HTML's StructuredSerializeInternal walks it:
 * undefined, null, booleans, numbers, BigInts and strings (a lone surrogate
 * kept); the Boolean, Number, BigInt and String objects; Date; RegExp (its
 * source and flags; lastIndex is 0 again); ArrayBuffer (a resizable one
 * keeps its maximum) and every typed array and DataView, which share their
 * buffer with whatever else shares it; Map and Set, their entries in
 * order; Error and its six native kinds (name, message, and the `cause`
 * and `stack` an own data property holds, HTML's "interesting accompanying
 * data"); arrays (their length, holes and non-index properties) and
 * ordinary objects, each own enumerable string-keyed property read with
 * [[Get]], so a getter runs, in [[OwnPropertyKeys]] order, a key a getter
 * deleted first skipped; and the platform objects HTML makes serializable
 * that this browser has: Blob, File and DOMException. A value met twice
 * is one value in the copy, cycles included.
 *
 * Everything else is a DataCloneError: a Symbol, a function, a Proxy, a
 * SharedArrayBuffer (no page here is cross-origin isolated, so HTML refuses
 * one), a view out of its buffer's bounds, a detached buffer, any other
 * object with internal state (a Promise, a WeakMap, an arguments object),
 * and every other platform object: a node, an Event, the window, and
 * location, history and navigator, which are ordinary objects in this
 * binding but platform objects in HTML. What a getter or a message's
 * toString throws is thrown as it is.
 *
 * TRANSFER: the transfer list's ArrayBuffers (anything else, a duplicate, a
 * SharedArrayBuffer or a detached one is a DataCloneError) are detached
 * once the value is written, their bytes copied into the record first
 * (the engine can detach a buffer but not hand its bytes over; DEBTS.md),
 * and are new buffers in the copy wherever the value named them. HTML
 * checks and detaches them one by one; this checks and copies them all,
 * then detaches them all, which differs only in what a refusal halfway
 * leaves detached (here, nothing).
 *
 * WHAT A PAGE CAN SIZE. Both directions walk iteratively, on stacks of
 * their own in engine memory, so a graph a million objects deep is a
 * million frames, never a C stack's overflow; each object is looked up once
 * in a hash table keyed by its address, which holds it (a getter that
 * frees an object it already wrote cannot let a new one take its address
 * and pass for it); a Map's or Set's entries are copied out by its own
 * forEach as the engine made it, never by an iterator a page can replace.
 * The work is linear in what the value holds plus what its getters do,
 * under the task's deadline.
 *
 * Not kept: a length-tracking view tracks no length in the copy (the
 * engine does not say which views track), and a DOMException's name or
 * message is cut at a NUL (DEBTS.md). */

static JSClassID collector_slot;

/* The engine's own classes, read off instances it makes at setup (the
 * same in every runtime, since the engine numbers its own classes). */
static JSClassID s_object, s_array, s_error, s_date, s_regexp, s_map, s_set, s_boolean, s_number, s_string,
    s_bigint, s_buffer, s_shared;

/* The intrinsics the pair calls, taken before any page ran. */
enum {
    H_OBJECT, H_BOOLEAN_VALUE, H_NUMBER_VALUE, H_STRING_VALUE, H_BIGINT_VALUE, H_DATE_TIME,
    H_REGEXP, H_REGEXP_SOURCE, H_FLAG_FIRST, H_FLAG_LAST = H_FLAG_FIRST + 7,
    H_BUFFER, H_BUFFER_RESIZABLE, H_BUFFER_MAX,
    H_MAP, H_MAP_SET, H_MAP_FOR_EACH, H_SET, H_SET_ADD, H_SET_FOR_EACH,
    H_ERROR_FIRST, H_ERROR_LAST = H_ERROR_FIRST + 6, H_SHARED,
    H_COUNT
};
_Static_assert(H_COUNT == D_CLONE_HELD, "internal.h's clone_held holds every intrinsic the pair calls");

/* RegExp's flags in the order its `flags` getter spells them, each read
 * through its own getter, which reads the compiled expression and runs no
 * page code (`flags` itself reads the properties, which a page can
 * shadow). */
static const char kFlagGetters[8][12] = {"hasIndices", "global", "ignoreCase", "multiline", "dotAll", "unicode",
                                         "unicodeSets", "sticky"};
static const char kFlagLetters[8] = {'d', 'g', 'i', 'm', 's', 'u', 'v', 'y'};
/* The names an Error keeps (HTML's list); any other is "Error". */
static const char *const kErrorNames[7] = {"Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError",
                                           "TypeError", "URIError"};

/* ── The record ───────────────────────────────────────────────────────── */

enum {
    K_PRIMITIVE, K_REFERENCE, K_OBJECT, K_ARRAY, K_WRAPPER, K_DATE, K_REGEXP, K_BUFFER, K_VIEW, K_MAP, K_SET,
    K_ERROR, K_BLOB, K_EXCEPTION
};

/* What a buffer, a blob, an exception or an error carries besides its
 * item. Engine memory, the record's. */
typedef struct {
    uint8_t *bytes;
    size_t length;
    int64_t max;               /* a resizable buffer's maximum, else -1 */
    char *type, *name;         /* a blob's type and a File's name; an exception's name */
    char *message;             /* an exception's message */
    int64_t modified;          /* a File's lastModified */
    bool file;
    JSValue stack;             /* an error's own stack, a string, or undefined */
} DData;

/* One value of the record, in the order the walk wrote them: a container's
 * children follow it. `key` is the property it is the value of, when its
 * parent is an object or an array. */
typedef struct {
    JSValue value;             /* a primitive; a wrapper's primitive; a Date's time; a RegExp's source; an
                                * error's message (undefined for none) */
    JSAtom key;
    uint8_t kind;
    uint8_t sub;               /* a view's kind (d_view_kind); an error's name; a RegExp's flags */
    uint32_t id;               /* the object's number; a reference's target */
    uint64_t count;            /* an object's or array's properties; a map's or set's entries; an error's
                                * cause (1) or none (0); a view's byte offset */
    uint64_t size;             /* an array's length; a view's length, elements or a DataView's bytes */
    DData *data;
} DItem;

struct DClone {
    DItem *items;
    size_t count, cap;
    /* Objects numbered: the transferred buffers first, then each object as
     * the walk first meets it. */
    uint32_t objects;
    DData *transfers;
    size_t transfer_count;
};

static void data_free(JSRuntime *rt, DData *data)
{
    if (data == NULL) return;
    js_free_rt(rt, data->bytes);
    js_free_rt(rt, data->type);
    js_free_rt(rt, data->name);
    js_free_rt(rt, data->message);
    JS_FreeValueRT(rt, data->stack);
    js_free_rt(rt, data);
}

void d_clone_free(JSRuntime *rt, DClone *clone)
{
    if (clone == NULL) return;
    for (size_t i = 0; i < clone->count; i++) {
        DItem *item = &clone->items[i];
        JS_FreeValueRT(rt, item->value);
        if (item->key != JS_ATOM_NULL) JS_FreeAtomRT(rt, item->key);
        data_free(rt, item->data);
    }
    js_free_rt(rt, clone->items);
    for (size_t i = 0; i < clone->transfer_count; i++) js_free_rt(rt, clone->transfers[i].bytes);
    js_free_rt(rt, clone->transfers);
    js_free_rt(rt, clone);
}

/* ── Setup ────────────────────────────────────────────────────────────── */

static JSValue collect(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data);

static JSClassID class_of(JSValue value)
{
    JSClassID id = JS_IsException(value) ? 0 : JS_GetClassID(value);
    return id;
}

/* The engine's classes, read off instances made through the constructors
 * setup took, at a binding's first clone. Not at setup: the calls are
 * engine calls, the first of which asks the runtime whether to stop, and
 * outside a task an allocation the engine refused before it makes that
 * the runtime's failure (libjs CONTRACT.md), where a refused binding
 * creation must leave it usable. And once a binding, not once a process:
 * the calls count toward the engine's interrupt checks, which read the
 * clock, so a binding's work must not depend on whether another binding
 * cloned before it in the process (a replay runs each page in a process of
 * its own and must time it as the window that kept it did). -1 with an
 * exception pending. */
static int learn_classes(os64_dom_t *dom, JSContext *ctx)
{
    if (dom->clone_classes) return 0;
    JSValueConst shared = dom->clone_held[H_SHARED]->value;
    JSValue made[13];
    made[0] = JS_NewObject(ctx);
    made[1] = JS_NewArray(ctx);
    made[2] = JS_NewError(ctx);
    made[3] = JS_NewDate(ctx, 0);
    made[4] = JS_CallConstructor(ctx, dom->clone_held[H_REGEXP]->value, 0, NULL);
    made[5] = JS_CallConstructor(ctx, dom->clone_held[H_MAP]->value, 0, NULL);
    made[6] = JS_CallConstructor(ctx, dom->clone_held[H_SET]->value, 0, NULL);
    JSValue primitives[4] = {JS_TRUE, JS_NewInt32(ctx, 0), JS_NewString(ctx, ""), JS_NewBigInt64(ctx, 0)};
    for (int i = 0; i < 4; i++)
        made[7 + i] = JS_IsException(primitives[i]) ? JS_EXCEPTION
            : JS_Call(ctx, dom->clone_held[H_OBJECT]->value, JS_UNDEFINED, 1, (JSValueConst *)&primitives[i]);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, primitives[i]);
    made[11] = JS_NewArrayBufferCopy(ctx, NULL, 0);
    made[12] = JS_IsFunction(ctx, shared) ? JS_CallConstructor(ctx, shared, 0, NULL) : JS_UNDEFINED;
    bool failed = false;
    for (int i = 0; i < 13; i++) failed |= JS_IsException(made[i]);
    if (!failed) {
        s_object = class_of(made[0]); s_array = class_of(made[1]); s_error = class_of(made[2]);
        s_date = class_of(made[3]); s_regexp = class_of(made[4]); s_map = class_of(made[5]);
        s_set = class_of(made[6]); s_boolean = class_of(made[7]); s_number = class_of(made[8]);
        s_string = class_of(made[9]); s_bigint = class_of(made[10]); s_buffer = class_of(made[11]);
        /* No SharedArrayBuffer: no object can have its class. */
        s_shared = JS_IsObject(made[12]) ? class_of(made[12]) : 0;
        dom->clone_classes = true;
    }
    for (int i = 0; i < 13; i++) JS_FreeValue(ctx, made[i]);
    return failed ? -1 : 0;
}

static int take_value(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const char *name, int slot)
{
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsException(value)) return -1;
    dom->clone_held[slot] = d_retain(dom, ctx, value);
    return dom->clone_held[slot] != NULL ? 0 : -1;
}

/* An interface's constructor from the global, and `name` of its prototype
 * when `member` names one (an accessor's getter with `getter`). */
static int take_from(os64_dom_t *dom, JSContext *ctx, JSValueConst global, const char *interface,
                     int slot, const char *member, int member_slot, bool getter)
{
    JSValue constructor = JS_GetPropertyStr(ctx, global, interface);
    if (JS_IsException(constructor)) return -1;
    int result = 0;
    if (slot >= 0) {
        dom->clone_held[slot] = d_retain(dom, ctx, JS_DupValue(ctx, constructor));
        result = dom->clone_held[slot] != NULL ? 0 : -1;
    }
    if (result == 0 && member != NULL) {
        JSValue prototype = JS_GetPropertyStr(ctx, constructor, "prototype");
        result = JS_IsException(prototype) ? -1
            : getter ? d_take_getter(dom, ctx, prototype, member, &dom->clone_held[member_slot])
                     : take_value(dom, ctx, prototype, member, member_slot);
        JS_FreeValue(ctx, prototype);
    }
    JS_FreeValue(ctx, constructor);
    return result;
}

int d_clone_setup(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (dom->clone_held[H_OBJECT] != NULL) return 0;
    JSClassID id = os64_js_class_id(&collector_slot);
    static const JSClassDef collector_class = {.class_name = "StructuredCloneEntries"};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &collector_class) < 0) return -1;
    if (take_from(dom, ctx, global, "Object", H_OBJECT, NULL, 0, false) < 0 ||
        take_from(dom, ctx, global, "Boolean", -1, "valueOf", H_BOOLEAN_VALUE, false) < 0 ||
        take_from(dom, ctx, global, "Number", -1, "valueOf", H_NUMBER_VALUE, false) < 0 ||
        take_from(dom, ctx, global, "String", -1, "valueOf", H_STRING_VALUE, false) < 0 ||
        take_from(dom, ctx, global, "BigInt", -1, "valueOf", H_BIGINT_VALUE, false) < 0 ||
        take_from(dom, ctx, global, "Date", -1, "getTime", H_DATE_TIME, false) < 0 ||
        take_from(dom, ctx, global, "RegExp", H_REGEXP, "source", H_REGEXP_SOURCE, true) < 0 ||
        take_from(dom, ctx, global, "ArrayBuffer", H_BUFFER, "resizable", H_BUFFER_RESIZABLE, true) < 0 ||
        take_from(dom, ctx, global, "ArrayBuffer", -1, "maxByteLength", H_BUFFER_MAX, true) < 0 ||
        take_from(dom, ctx, global, "Map", H_MAP, "set", H_MAP_SET, false) < 0 ||
        take_from(dom, ctx, global, "Map", -1, "forEach", H_MAP_FOR_EACH, false) < 0 ||
        take_from(dom, ctx, global, "Set", H_SET, "add", H_SET_ADD, false) < 0 ||
        take_from(dom, ctx, global, "Set", -1, "forEach", H_SET_FOR_EACH, false) < 0)
        return -1;
    for (int i = 0; i < 8; i++)
        if (take_from(dom, ctx, global, "RegExp", -1, kFlagGetters[i], H_FLAG_FIRST + i, true) < 0) return -1;
    for (int i = 0; i < 7; i++) {
        JSValue constructor = JS_GetPropertyStr(ctx, global, kErrorNames[i]);
        if (JS_IsException(constructor)) return -1;
        int result = take_value(dom, ctx, constructor, "prototype", H_ERROR_FIRST + i);
        JS_FreeValue(ctx, constructor);
        if (result < 0) return -1;
    }
    /* SharedArrayBuffer's constructor, or undefined where there is none. */
    return take_value(dom, ctx, global, "SharedArrayBuffer", H_SHARED);
}

/* ── Writing ──────────────────────────────────────────────────────────── */

/* An object being walked: its keys (an object's or array's, as the walk
 * began it) or its list (a map's keys and values, a set's values, an
 * error's cause), the next one, and the item whose count it ends with. */
typedef struct {
    JSValue object;            /* the memory table's reference */
    JSPropertyEnum *keys;
    uint32_t key_count;
    JSValue *list;
    size_t list_count;
    size_t at, header;
    uint64_t written;
} DFrame;

typedef struct {
    void *object;              /* NULL: a free slot */
    uint32_t id;
} DSlot;

typedef struct {
    os64_dom_t *dom;
    JSContext *ctx;
    JSRuntime *rt;
    DClone *out;
    /* THE MEMORY (HTML's "memory" map): each object met, by address, with
     * its number; the slots hold the objects, released at the end. */
    DSlot *table;
    size_t table_cap, table_used;
    JSValue *held;
    size_t held_count, held_cap;
    DFrame *frames;
    size_t frame_count, frame_cap;
} DWriter;

static bool grow(JSContext *ctx, void **array, size_t *cap, size_t need, size_t size)
{
    if (need <= *cap) return true;
    size_t next = *cap < 16 ? 16 : *cap;
    while (next < need) {
        if (next > SIZE_MAX / 2 / size) { JS_ThrowOutOfMemory(ctx); return false; }
        next *= 2;
    }
    void *grown = js_realloc(ctx, *array, next * size);
    if (grown == NULL) return false;
    *array = grown;
    *cap = next;
    return true;
}

static uint64_t hash_pointer(const void *object)
{
    uint64_t h = (uint64_t)(uintptr_t)object;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

static DSlot *memory_find(const DWriter *w, const void *object)
{
    if (w->table_cap == 0) return NULL;
    for (size_t at = hash_pointer(object) & (w->table_cap - 1);; at = (at + 1) & (w->table_cap - 1)) {
        if (w->table[at].object == object) return &w->table[at];
        if (w->table[at].object == NULL) return NULL;
    }
}

/* `object` remembered as number `id`, the table holding it. */
static int memory_add(DWriter *w, JSValueConst object, uint32_t id)
{
    if (!grow(w->ctx, (void **)&w->held, &w->held_cap, w->held_count + 1, sizeof(*w->held))) return -1;
    if ((w->table_used + 1) * 2 > w->table_cap) {
        size_t cap = w->table_cap < 32 ? 32 : w->table_cap * 2;
        DSlot *table = js_mallocz(w->ctx, cap * sizeof(*table));
        if (table == NULL) return -1;
        for (size_t i = 0; i < w->table_cap; i++)
            if (w->table[i].object != NULL) {
                size_t at = hash_pointer(w->table[i].object) & (cap - 1);
                while (table[at].object != NULL) at = (at + 1) & (cap - 1);
                table[at] = w->table[i];
            }
        js_free(w->ctx, w->table);
        w->table = table;
        w->table_cap = cap;
    }
    void *key = JS_VALUE_GET_PTR(object);
    size_t at = hash_pointer(key) & (w->table_cap - 1);
    while (w->table[at].object != NULL) at = (at + 1) & (w->table_cap - 1);
    w->table[at] = (DSlot){key, id};
    w->table_used++;
    w->held[w->held_count++] = JS_DupValue(w->ctx, object);
    return 0;
}

/* A new item at the end of the record, its key (held) `key`. NULL on no
 * memory. */
static DItem *item_new(DWriter *w, JSAtom key, uint8_t kind)
{
    DClone *out = w->out;
    if (!grow(w->ctx, (void **)&out->items, &out->cap, out->count + 1, sizeof(*out->items))) return NULL;
    DItem *item = &out->items[out->count++];
    *item = (DItem){.value = JS_UNDEFINED, .key = key != JS_ATOM_NULL ? JS_DupAtom(w->ctx, key) : JS_ATOM_NULL,
                    .kind = kind};
    return item;
}

static DData *data_new(DWriter *w, DItem *item)
{
    item->data = js_mallocz(w->ctx, sizeof(*item->data));
    if (item->data != NULL) {
        item->data->max = -1;
        item->data->stack = JS_UNDEFINED;
    }
    return item->data;
}

/* A number for the object that now has an item, remembered. */
static int number(DWriter *w, JSValueConst object, size_t index)
{
    if (w->out->objects == UINT32_MAX) {
        JS_ThrowRangeError(w->ctx, "Too many objects to clone");
        return -1;
    }
    uint32_t id = w->out->objects++;
    w->out->items[index].id = id;
    return memory_add(w, object, id);
}

static JSValue data_clone_error(DWriter *w, const char *message)
{
    return d_error(w->ctx, "DataCloneError", message);
}

static char *copy_text(JSContext *ctx, const char *text)
{
    return js_strdup(ctx, text != NULL ? text : "");
}

/* A frame for the object whose item is `header`: its keys, or `list`
 * (taken). */
static int frame_push(DWriter *w, JSValueConst object, size_t header, JSValue *list, size_t list_count)
{
    if (!grow(w->ctx, (void **)&w->frames, &w->frame_cap, w->frame_count + 1, sizeof(*w->frames))) {
        for (size_t i = 0; i < list_count; i++) JS_FreeValue(w->ctx, list[i]);
        js_free(w->ctx, list);
        return -1;
    }
    DFrame *frame = &w->frames[w->frame_count];
    *frame = (DFrame){.object = (JSValue)object, .header = header, .list = list, .list_count = list_count};
    if (list == NULL &&
        JS_GetOwnPropertyNames(w->ctx, &frame->keys, &frame->key_count, object,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;
    w->frame_count++;
    return 0;
}

static void frame_pop(DWriter *w)
{
    DFrame *frame = &w->frames[--w->frame_count];
    if (frame->keys != NULL) JS_FreePropertyEnum(w->ctx, frame->keys, frame->key_count);
    for (size_t i = frame->at; i < frame->list_count; i++) JS_FreeValue(w->ctx, frame->list[i]);
    js_free(w->ctx, frame->list);
}

/* Map.prototype.forEach's and Set.prototype.forEach's callback, collecting
 * each entry (a set's value once) into the list its collector holds. */
typedef struct {
    JSValue *list;
    size_t count, cap;
    bool pairs;
} DCollected;

static JSValue collect(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)magic;
    DCollected *c = JS_GetOpaque(data[0], os64_js_class_id(&collector_slot));
    if (c == NULL || argc < 2) return JS_ThrowTypeError(ctx, "Not a clone's collection");
    size_t add = c->pairs ? 2 : 1;
    if (!grow(ctx, (void **)&c->list, &c->cap, c->count + add, sizeof(*c->list))) return JS_EXCEPTION;
    if (c->pairs) c->list[c->count++] = JS_DupValue(ctx, argv[1]);
    c->list[c->count++] = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

/* A map's entries as key, value, key, value; a set's values. */
static int entries(DWriter *w, JSValueConst object, bool map, JSValue **list, size_t *count)
{
    DCollected c = {NULL, 0, 0, map};
    JSValue holder = JS_NewObjectClass(w->ctx, (int)os64_js_class_id(&collector_slot));
    if (JS_IsException(holder)) return -1;
    JS_SetOpaque(holder, &c);
    JSValue callback = JS_NewCFunctionData(w->ctx, collect, 3, 0, 1, (JSValueConst *)&holder);
    JSValue done = JS_IsException(callback) ? JS_EXCEPTION
        : JS_Call(w->ctx, w->dom->clone_held[map ? H_MAP_FOR_EACH : H_SET_FOR_EACH]->value, object, 1,
                  (JSValueConst *)&callback);
    JS_SetOpaque(holder, NULL);
    JS_FreeValue(w->ctx, callback);
    JS_FreeValue(w->ctx, holder);
    if (JS_IsException(done)) {
        for (size_t i = 0; i < c.count; i++) JS_FreeValue(w->ctx, c.list[i]);
        js_free(w->ctx, c.list);
        return -1;
    }
    JS_FreeValue(w->ctx, done);
    *list = c.list;
    *count = c.count;
    return 0;
}

static JSValue call_held(DWriter *w, int slot, JSValueConst self)
{
    return JS_Call(w->ctx, w->dom->clone_held[slot]->value, self, 0, NULL);
}

/* An ArrayBuffer's bytes copied into `data`, or a refusal: detached, or
 * shared (HTML refuses a SharedArrayBuffer outside a cross-origin
 * isolated context, which no page here is). */
static int buffer_copy(DWriter *w, JSValueConst buffer, DData *data)
{
    if (JS_GetClassID(buffer) == s_shared) {
        data_clone_error(w, "A SharedArrayBuffer cannot be cloned here");
        return -1;
    }
    size_t length;
    uint8_t *bytes = JS_GetArrayBuffer(w->ctx, &length, buffer);
    if (bytes == NULL) {
        JS_FreeValue(w->ctx, JS_GetException(w->ctx));
        data_clone_error(w, "A detached ArrayBuffer cannot be cloned");
        return -1;
    }
    JSValue resizable = call_held(w, H_BUFFER_RESIZABLE, buffer);
    if (JS_IsException(resizable)) return -1;
    if (JS_ToBool(w->ctx, resizable)) {
        JSValue max = call_held(w, H_BUFFER_MAX, buffer);
        int64_t most;
        if (JS_IsException(max) || JS_ToInt64(w->ctx, &most, max) < 0) { JS_FreeValue(w->ctx, max); return -1; }
        JS_FreeValue(w->ctx, max);
        data->max = most;
    }
    /* Nothing above ran a page's code, so the bytes are where they were. */
    if (length != 0) {
        data->bytes = js_malloc(w->ctx, length);
        if (data->bytes == NULL) return -1;
        os64_memcpy(data->bytes, bytes, length);
    }
    data->length = length;
    return 0;
}

/* The binding's own objects that are ordinary in the engine and platform
 * objects in HTML, which refuses them. */
static bool binding_object(const os64_dom_t *dom, JSValueConst object)
{
    const DValue *mine[3] = {dom->location, dom->history, dom->navigator};
    for (int i = 0; i < 3; i++)
        if (mine[i] != NULL && JS_VALUE_GET_PTR(mine[i]->value) == JS_VALUE_GET_PTR(object)) return true;
    return false;
}

static int write_value(DWriter *w, JSAtom key, JSValueConst value);

/* An Error (HTML's serialization steps for one): its name, read with
 * [[Get]]; its message, its own data property converted to a string; and
 * its own `cause` and `stack`, which the copy keeps. */
static int write_error(DWriter *w, JSAtom key, JSValueConst value)
{
    JSContext *ctx = w->ctx;
    uint8_t name_index = 0;
    JSValue name = JS_GetPropertyStr(ctx, value, "name");
    if (JS_IsException(name)) return -1;
    if (JS_IsString(name)) {
        const char *text = JS_ToCString(ctx, name);
        if (text == NULL) { JS_FreeValue(ctx, name); return -1; }
        for (uint8_t i = 0; i < 7; i++)
            if (os64_streq(text, kErrorNames[i])) name_index = i;
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, name);
    JSValue own[3] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    bool has[3] = {false, false, false};
    static const char *const kOwn[3] = {"message", "stack", "cause"};
    for (int i = 0; i < 3; i++) {
        JSAtom atom = JS_NewAtom(ctx, kOwn[i]);
        JSPropertyDescriptor descriptor;
        int found = atom == JS_ATOM_NULL ? -1 : JS_GetOwnProperty(ctx, &descriptor, value, atom);
        if (atom != JS_ATOM_NULL) JS_FreeAtom(ctx, atom);
        if (found < 0) {
            for (int j = 0; j < i; j++) JS_FreeValue(ctx, own[j]);
            return -1;
        }
        if (found == 0) continue;
        JS_FreeValue(ctx, descriptor.getter);
        JS_FreeValue(ctx, descriptor.setter);
        if (descriptor.flags & JS_PROP_GETSET) { JS_FreeValue(ctx, descriptor.value); continue; }
        own[i] = descriptor.value;
        has[i] = true;
    }
    if (has[0]) {
        JSValue text = JS_ToString(ctx, own[0]);
        JS_FreeValue(ctx, own[0]);
        own[0] = text;
        if (JS_IsException(text)) {
            JS_FreeValue(ctx, own[1]);
            JS_FreeValue(ctx, own[2]);
            return -1;
        }
    }
    if (!JS_IsString(own[1])) {
        JS_FreeValue(ctx, own[1]);
        own[1] = JS_UNDEFINED;
    }
    DItem *item = item_new(w, key, K_ERROR);
    DData *data = item != NULL ? data_new(w, item) : NULL;
    if (data == NULL) {
        for (int i = 0; i < 3; i++) JS_FreeValue(ctx, own[i]);
        return -1;
    }
    size_t index = w->out->count - 1;
    item->sub = name_index;
    item->value = own[0];
    data->stack = own[1];
    item->count = has[2];
    if (number(w, value, index) < 0) { JS_FreeValue(ctx, own[2]); return -1; }
    if (!has[2]) return 0;
    JSValue *list = js_malloc(ctx, sizeof(*list));
    if (list == NULL) { JS_FreeValue(ctx, own[2]); return -1; }
    list[0] = own[2];
    return frame_push(w, value, index, list, 1);
}

/* A typed array or a DataView: the view's item, then its buffer's (or a
 * reference to it), then the view's number, as HTML serializes the buffer
 * before it remembers the view. */
static int write_view(DWriter *w, JSAtom key, JSValueConst value, int kind)
{
    JSContext *ctx = w->ctx;
    JSValue buffer;
    size_t offset, length;
    if (kind == D_VIEW_DATA_VIEW) {
        JSValue got[3] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
        bool failed = false;
        for (int i = 0; i < 3 && !failed; i++) {
            got[i] = JS_Call(ctx, w->dom->data_view[1 + i]->value, value, 0, NULL);
            failed = JS_IsException(got[i]);
        }
        int64_t numbers[2] = {0, 0};
        failed = failed || JS_ToInt64(ctx, &numbers[0], got[1]) < 0 || JS_ToInt64(ctx, &numbers[1], got[2]) < 0;
        JS_FreeValue(ctx, got[1]);
        JS_FreeValue(ctx, got[2]);
        if (failed) {
            JS_FreeValue(ctx, got[0]);
            /* The getters refuse a view out of its buffer's bounds, which is
             * a DataCloneError; a runtime that must stop is let stop. */
            if (!d_exception_catchable(ctx)) return -1;
            JS_FreeValue(ctx, JS_GetException(ctx));
            data_clone_error(w, "A DataView out of its buffer's bounds cannot be cloned");
            return -1;
        }
        buffer = got[0];
        offset = (size_t)numbers[0];
        length = (size_t)numbers[1];
    } else {
        size_t bytes, per;
        buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &bytes, &per);
        if (JS_IsException(buffer)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            data_clone_error(w, "A typed array out of its buffer's bounds cannot be cloned");
            return -1;
        }
        length = per != 0 ? bytes / per : 0;
    }
    DItem *item = item_new(w, key, K_VIEW);
    if (item == NULL) { JS_FreeValue(ctx, buffer); return -1; }
    size_t index = w->out->count - 1;
    item->sub = (uint8_t)kind;
    item->count = offset;
    item->size = length;
    int written = write_value(w, JS_ATOM_NULL, buffer);
    JS_FreeValue(ctx, buffer);
    return written < 0 ? -1 : number(w, value, index);
}

static int write_blob(DWriter *w, JSAtom key, JSValueConst value, const uint8_t *bytes, size_t length,
                      const char *type)
{
    DItem *item = item_new(w, key, K_BLOB);
    DData *data = item != NULL ? data_new(w, item) : NULL;
    if (data == NULL) return -1;
    size_t index = w->out->count - 1;
    const char *name;
    data->file = d_blob_file(value, &name, &data->modified);
    data->type = copy_text(w->ctx, type);
    data->name = data->file ? copy_text(w->ctx, name) : NULL;
    if (data->type == NULL || (data->file && data->name == NULL)) return -1;
    if (length != 0) {
        data->bytes = js_malloc(w->ctx, length);
        if (data->bytes == NULL) return -1;
        os64_memcpy(data->bytes, bytes, length);
    }
    data->length = length;
    return number(w, value, index);
}

/* A RegExp's item: its source and flags, read through the getters taken
 * before any page ran. -1 with an exception pending. */
static int write_regexp(DWriter *w, JSAtom key, JSValueConst value, size_t index)
{
    JSContext *ctx = w->ctx;
    JSValue source = call_held(w, H_REGEXP_SOURCE, value);
    if (JS_IsException(source)) return -1;
    uint8_t flags = 0;
    for (int i = 0; i < 8; i++) {
        JSValue flag = call_held(w, H_FLAG_FIRST + i, value);
        if (JS_IsException(flag)) { JS_FreeValue(ctx, source); return -1; }
        if (JS_ToBool(ctx, flag)) flags |= (uint8_t)(1u << i);
        JS_FreeValue(ctx, flag);
    }
    DItem *item = item_new(w, key, K_REGEXP);
    if (item == NULL) { JS_FreeValue(ctx, source); return -1; }
    item->value = source;
    item->sub = flags;
    return number(w, value, index);
}

/* One value: a primitive's item, a reference to an object already written,
 * or an object's item, its children left to a frame. */
static int write_value(DWriter *w, JSAtom key, JSValueConst value)
{
    JSContext *ctx = w->ctx;
    if (!JS_IsObject(value)) {
        int tag = JS_VALUE_GET_NORM_TAG(value);
        if (tag == JS_TAG_SYMBOL) { data_clone_error(w, "A Symbol cannot be cloned"); return -1; }
        if (tag != JS_TAG_UNDEFINED && tag != JS_TAG_NULL && tag != JS_TAG_BOOL && tag != JS_TAG_INT &&
            tag != JS_TAG_FLOAT64 && !JS_IsString(value) && !JS_IsBigInt(ctx, value)) {
            data_clone_error(w, "That value cannot be cloned");
            return -1;
        }
        DItem *item = item_new(w, key, K_PRIMITIVE);
        if (item == NULL) return -1;
        item->value = JS_DupValue(ctx, value);
        return 0;
    }
    DSlot *seen = memory_find(w, JS_VALUE_GET_PTR(value));
    if (seen != NULL) {
        DItem *item = item_new(w, key, K_REFERENCE);
        if (item == NULL) return -1;
        item->id = seen->id;
        return 0;
    }
    os64_dom_t *dom = w->dom;
    JSClassID id = JS_GetClassID(value);
    size_t index = w->out->count;
    if (id == s_boolean || id == s_number || id == s_string || id == s_bigint) {
        int slot = id == s_boolean ? H_BOOLEAN_VALUE : id == s_number ? H_NUMBER_VALUE
                 : id == s_string ? H_STRING_VALUE : H_BIGINT_VALUE;
        JSValue primitive = call_held(w, slot, value);
        if (JS_IsException(primitive)) return -1;
        DItem *item = item_new(w, key, K_WRAPPER);
        if (item == NULL) { JS_FreeValue(ctx, primitive); return -1; }
        item->value = primitive;
        return number(w, value, index);
    }
    if (id == s_date) {
        JSValue time = call_held(w, H_DATE_TIME, value);
        if (JS_IsException(time)) return -1;
        DItem *item = item_new(w, key, K_DATE);
        if (item == NULL) { JS_FreeValue(ctx, time); return -1; }
        item->value = time;
        return number(w, value, index);
    }
    if (id == s_regexp) return write_regexp(w, key, value, index);
    if (id == s_buffer || (s_shared != 0 && id == s_shared)) {
        DItem *item = item_new(w, key, K_BUFFER);
        DData *data = item != NULL ? data_new(w, item) : NULL;
        if (data == NULL || buffer_copy(w, value, data) < 0) return -1;
        return number(w, value, index);
    }
    int view = d_view_kind(dom, ctx, value);
    if (view == -2) return -1;
    if (view >= 0) return write_view(w, key, value, view);
    if (id == s_map || id == s_set) {
        JSValue *list;
        size_t count;
        if (entries(w, value, id == s_map, &list, &count) < 0) return -1;
        DItem *item = item_new(w, key, id == s_map ? K_MAP : K_SET);
        if (item == NULL || number(w, value, index) < 0) {
            for (size_t i = 0; i < count; i++) JS_FreeValue(ctx, list[i]);
            js_free(ctx, list);
            return -1;
        }
        w->out->items[index].count = id == s_map ? count / 2 : count;
        return frame_push(w, value, index, list, count);
    }
    if (id == s_error) return write_error(w, key, value);
    const uint8_t *bytes;
    size_t length;
    const char *type, *message;
    if (d_blob_get(value, &bytes, &length, &type)) return write_blob(w, key, value, bytes, length, type);
    if (d_dom_exception_fields(value, &type, &message)) {
        DItem *item = item_new(w, key, K_EXCEPTION);
        DData *data = item != NULL ? data_new(w, item) : NULL;
        if (data == NULL) return -1;
        data->name = copy_text(ctx, type);
        data->message = copy_text(ctx, message);
        if (data->name == NULL || data->message == NULL) return -1;
        return number(w, value, index);
    }
    if (id == s_array) {
        JSValue length_value = JS_GetPropertyStr(ctx, value, "length");
        uint32_t array_length;
        if (JS_IsException(length_value) || JS_ToUint32(ctx, &array_length, length_value) < 0) {
            JS_FreeValue(ctx, length_value);
            return -1;
        }
        DItem *item = item_new(w, key, K_ARRAY);
        if (item == NULL) return -1;
        item->size = array_length;
        if (number(w, value, index) < 0) return -1;
        return frame_push(w, value, index, NULL, 0);
    }
    if (id == s_object && !JS_IsFunction(ctx, value) && !binding_object(dom, value)) {
        if (item_new(w, key, K_OBJECT) == NULL || number(w, value, index) < 0) return -1;
        return frame_push(w, value, index, NULL, 0);
    }
    data_clone_error(w, JS_IsFunction(ctx, value) ? "A function cannot be cloned" : "That object cannot be cloned");
    return -1;
}

/* The walk, frame by frame: each object's next own enumerable key, still
 * its own when it is reached, read with [[Get]]; each list's next value. */
static int walk(DWriter *w)
{
    JSContext *ctx = w->ctx;
    while (w->frame_count > 0) {
        size_t top = w->frame_count - 1;
        DFrame *frame = &w->frames[top];
        if (frame->list != NULL) {
            if (frame->at == frame->list_count) { frame_pop(w); continue; }
            JSValue value = frame->list[frame->at];
            frame->list[frame->at++] = JS_UNDEFINED;
            int written = write_value(w, JS_ATOM_NULL, value);
            JS_FreeValue(ctx, value);
            if (written < 0) return -1;
            continue;
        }
        if (frame->at == frame->key_count) {
            w->out->items[frame->header].count = frame->written;
            frame_pop(w);
            continue;
        }
        JSAtom atom = frame->keys[frame->at++].atom;
        JSValueConst object = frame->object;
        int own = JS_GetOwnProperty(ctx, NULL, object, atom);
        if (own < 0) return -1;
        if (own == 0) continue;
        JSValue value = JS_GetProperty(ctx, object, atom);
        if (JS_IsException(value)) return -1;
        /* The getter may have grown the frames: index, never hold, them. */
        w->frames[top].written++;
        int written = write_value(w, atom, value);
        JS_FreeValue(ctx, value);
        if (written < 0) return -1;
    }
    return 0;
}

static void writer_end(DWriter *w)
{
    while (w->frame_count > 0) frame_pop(w);
    js_free(w->ctx, w->frames);
    for (size_t i = 0; i < w->held_count; i++) JS_FreeValue(w->ctx, w->held[i]);
    js_free(w->ctx, w->held);
    js_free(w->ctx, w->table);
}

DClone *d_clone_write(os64_dom_t *dom, JSContext *ctx, JSValueConst value, const JSValue *transfer,
                      size_t transfer_count)
{
    if (learn_classes(dom, ctx) < 0) return NULL;
    DClone *out = js_mallocz(ctx, sizeof(*out));
    if (out == NULL) return NULL;
    DWriter w = {.dom = dom, .ctx = ctx, .rt = dom->engine, .out = out};
    int result = 0;
    /* The transfer list first: each an ArrayBuffer met once, numbered
     * before the value is walked, so the value names it by number. */
    for (size_t i = 0; result == 0 && i < transfer_count; i++) {
        JSValueConst buffer = transfer[i];
        if (!JS_IsObject(buffer) || JS_GetClassID(buffer) != s_buffer) {
            data_clone_error(&w, JS_IsObject(buffer) && JS_GetClassID(buffer) == s_shared && s_shared != 0
                                     ? "A SharedArrayBuffer cannot be transferred"
                                     : "Only an ArrayBuffer can be transferred");
            result = -1;
        } else if (memory_find(&w, JS_VALUE_GET_PTR(buffer)) != NULL) {
            data_clone_error(&w, "An ArrayBuffer is in the transfer list twice");
            result = -1;
        } else result = memory_add(&w, buffer, out->objects++);
    }
    if (result == 0) result = write_value(&w, JS_ATOM_NULL, value);
    if (result == 0) result = walk(&w);
    /* Then each is detached, its bytes the record's. */
    if (result == 0 && transfer_count != 0) {
        out->transfers = js_mallocz(ctx, transfer_count * sizeof(*out->transfers));
        result = out->transfers != NULL ? 0 : -1;
        for (size_t i = 0; result == 0 && i < transfer_count; i++) {
            out->transfers[i].max = -1;
            out->transfers[i].stack = JS_UNDEFINED;
            out->transfer_count = i + 1;
            result = buffer_copy(&w, transfer[i], &out->transfers[i]);
        }
        for (size_t i = 0; result == 0 && i < transfer_count; i++) JS_DetachArrayBuffer(ctx, transfer[i]);
    }
    writer_end(&w);
    if (result < 0) {
        d_clone_free(dom->engine, out);
        return NULL;
    }
    return out;
}

/* ── Reading ──────────────────────────────────────────────────────────── */

/* A container being filled: how many children it waits for, and what a
 * map's last key was. */
typedef struct {
    JSValue container;         /* a view's buffer, once read */
    size_t header;
    uint64_t left;
    JSValue map_key;
    bool keyed;
} DFill;

typedef struct {
    os64_dom_t *dom;
    JSContext *ctx;
    DClone *in;
    JSValue *objects;
    DFill *fills;
    size_t fill_count, fill_cap;
    JSValue root;
    bool rooted;
} DReader;

static void bytes_free(JSRuntime *rt, void *opaque, void *ptr)
{
    (void)opaque;
    js_free_rt(rt, ptr);
}

/* A buffer of `data`'s bytes, which it takes: a fixed one keeps them as
 * they are, a resizable one is made by the constructor with its maximum. */
static JSValue buffer_make(DReader *r, DData *data)
{
    JSContext *ctx = r->ctx;
    if (data->max < 0) {
        if (data->bytes == NULL) return JS_NewArrayBufferCopy(ctx, NULL, 0);
        JSValue buffer = JS_NewArrayBuffer(ctx, data->bytes, data->length, bytes_free, NULL, false);
        if (!JS_IsException(buffer)) data->bytes = NULL;
        return buffer;
    }
    JSValue options = JS_NewObject(ctx);
    if (JS_IsException(options) ||
        JS_DefinePropertyValueStr(ctx, options, "maxByteLength", JS_NewInt64(ctx, data->max), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, options);
        return JS_EXCEPTION;
    }
    JSValue args[2] = {JS_NewInt64(ctx, (int64_t)data->length), options};
    JSValue buffer = JS_CallConstructor(ctx, r->dom->clone_held[H_BUFFER]->value, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, options);
    if (JS_IsException(buffer)) return buffer;
    size_t length;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &length, buffer);
    if (bytes == NULL || length != data->length) {
        JS_FreeValue(ctx, buffer);
        return JS_IsException(buffer) ? JS_EXCEPTION : JS_ThrowRangeError(ctx, "The buffer could not be made");
    }
    if (length != 0) os64_memcpy(bytes, data->bytes, length);
    return buffer;
}

/* `value` (taken) delivered to the container being filled, or kept as the
 * copy when there is none. */
static int deliver(DReader *r, JSAtom key, JSValue value)
{
    JSContext *ctx = r->ctx;
    if (r->fill_count == 0) {
        r->root = value;
        r->rooted = true;
        return 0;
    }
    DFill *fill = &r->fills[r->fill_count - 1];
    const DItem *header = &r->in->items[fill->header];
    int result = 0;
    switch (header->kind) {
    case K_OBJECT: case K_ARRAY:
        result = JS_DefinePropertyValue(ctx, fill->container, key, value, JS_PROP_C_W_E) < 0 ? -1 : 0;
        break;
    case K_MAP:
        if (!fill->keyed) {
            fill->map_key = value;
            fill->keyed = true;
            return 0;
        } else {
            JSValueConst args[2] = {fill->map_key, value};
            JSValue done = JS_Call(ctx, r->dom->clone_held[H_MAP_SET]->value, fill->container, 2, args);
            JS_FreeValue(ctx, fill->map_key);
            JS_FreeValue(ctx, value);
            fill->map_key = JS_UNDEFINED;
            fill->keyed = false;
            result = JS_IsException(done) ? -1 : 0;
            JS_FreeValue(ctx, done);
        }
        break;
    case K_SET: {
        JSValue done = JS_Call(ctx, r->dom->clone_held[H_SET_ADD]->value, fill->container, 1, (JSValueConst *)&value);
        JS_FreeValue(ctx, value);
        result = JS_IsException(done) ? -1 : 0;
        JS_FreeValue(ctx, done);
        break;
    }
    case K_ERROR:
        /* The cause as InstallErrorCause defines it: not enumerable. */
        result = JS_DefinePropertyValueStr(ctx, fill->container, "cause", value,
                                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
        break;
    case K_VIEW:
        fill->container = value;
        break;
    default:
        JS_FreeValue(ctx, value);
        result = -1;
    }
    fill->left--;
    return result;
}

/* A view made over the buffer its fill holds: the engine's typed array
 * kind, or the DataView constructor taken before any page ran. */
static JSValue view_make(DReader *r, const DItem *item, JSValueConst buffer)
{
    JSContext *ctx = r->ctx;
    JSValue args[3] = {JS_DupValue(ctx, buffer), JS_NewInt64(ctx, (int64_t)item->count),
                       JS_NewInt64(ctx, (int64_t)item->size)};
    JSValue view = item->sub == D_VIEW_DATA_VIEW
        ? JS_CallConstructor(ctx, r->dom->data_view[0]->value, 3, (JSValueConst *)args)
        : JS_NewTypedArray(ctx, 3, (JSValueConst *)args, (JSTypedArrayEnum)item->sub);
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
    return view;
}

/* Each container whose children are all in, finished and delivered. */
static int settle(DReader *r)
{
    JSContext *ctx = r->ctx;
    while (r->fill_count > 0 && r->fills[r->fill_count - 1].left == 0) {
        DFill fill = r->fills[--r->fill_count];
        const DItem *item = &r->in->items[fill.header];
        JSValue done = fill.container;
        if (item->kind == K_VIEW) {
            done = view_make(r, item, fill.container);
            JS_FreeValue(ctx, fill.container);
            if (JS_IsException(done)) return -1;
            r->objects[item->id] = JS_DupValue(ctx, done);
        }
        if (deliver(r, item->key, done) < 0) return -1;
    }
    return 0;
}

static int fill_push(DReader *r, size_t header, JSValue container, uint64_t left)
{
    if (!grow(r->ctx, (void **)&r->fills, &r->fill_cap, r->fill_count + 1, sizeof(*r->fills))) {
        JS_FreeValue(r->ctx, container);
        return -1;
    }
    r->fills[r->fill_count++] = (DFill){container, header, left, JS_UNDEFINED, false};
    return 0;
}

/* The flags a RegExp's sub holds, as its constructor spells them. */
static JSValue flags_text(JSContext *ctx, uint8_t flags)
{
    char text[9];
    size_t n = 0;
    for (int i = 0; i < 8; i++)
        if (flags & (1u << i)) text[n++] = kFlagLetters[i];
    return JS_NewStringLen(ctx, text, n);
}

/* One item made: a value, or a container whose children follow. */
static JSValue item_make(DReader *r, DItem *item, bool *container)
{
    JSContext *ctx = r->ctx;
    os64_dom_t *dom = r->dom;
    *container = false;
    switch (item->kind) {
    case K_PRIMITIVE: return JS_DupValue(ctx, item->value);
    case K_REFERENCE: return JS_DupValue(ctx, r->objects[item->id]);
    case K_OBJECT: *container = true; return JS_NewObject(ctx);
    case K_ARRAY: {
        JSValue array = JS_NewArray(ctx);
        if (!JS_IsException(array) &&
            JS_SetPropertyStr(ctx, array, "length", JS_NewUint32(ctx, (uint32_t)item->size)) < 0) {
            JS_FreeValue(ctx, array);
            return JS_EXCEPTION;
        }
        *container = true;
        return array;
    }
    case K_WRAPPER:
        return JS_Call(ctx, dom->clone_held[H_OBJECT]->value, JS_UNDEFINED, 1, (JSValueConst *)&item->value);
    case K_DATE: {
        double time;
        if (JS_ToFloat64(ctx, &time, item->value) < 0) return JS_EXCEPTION;
        return JS_NewDate(ctx, time);
    }
    case K_REGEXP: {
        JSValue args[2] = {JS_DupValue(ctx, item->value), flags_text(ctx, item->sub)};
        JSValue regexp = JS_IsException(args[1]) ? JS_EXCEPTION
            : JS_CallConstructor(ctx, dom->clone_held[H_REGEXP]->value, 2, (JSValueConst *)args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        return regexp;
    }
    case K_BUFFER: return buffer_make(r, item->data);
    case K_VIEW: *container = true; return JS_UNDEFINED;
    case K_MAP: case K_SET:
        *container = true;
        return JS_CallConstructor(ctx, dom->clone_held[item->kind == K_MAP ? H_MAP : H_SET]->value, 0, NULL);
    case K_ERROR: {
        JSValue error = JS_NewObjectProtoClass(ctx, dom->clone_held[H_ERROR_FIRST + item->sub]->value, s_error);
        if (JS_IsException(error)) return error;
        /* message and stack as the engine defines an error's own. */
        if ((!JS_IsUndefined(item->value) &&
             JS_DefinePropertyValueStr(ctx, error, "message", JS_DupValue(ctx, item->value),
                                       JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) ||
            (!JS_IsUndefined(item->data->stack) &&
             JS_DefinePropertyValueStr(ctx, error, "stack", JS_DupValue(ctx, item->data->stack),
                                       JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)) {
            JS_FreeValue(ctx, error);
            return JS_EXCEPTION;
        }
        *container = item->count != 0;
        return error;
    }
    case K_BLOB: {
        DData *data = item->data;
        DBytes bytes = {data->bytes, data->length, data->type};
        data->bytes = NULL;
        data->type = NULL;
        if (!data->file) return d_blob_new(dom, ctx, &bytes);
        char *name = data->name;
        data->name = NULL;
        return d_file_new(dom, ctx, &bytes, name, data->modified);
    }
    case K_EXCEPTION: return d_dom_exception_new(dom, ctx, item->data->name, item->data->message);
    default: return JS_ThrowTypeError(ctx, "A clone's record is not one");
    }
}

static int read_items(DReader *r)
{
    DClone *in = r->in;
    for (size_t i = 0; i < in->transfer_count; i++) {
        r->objects[i] = buffer_make(r, &in->transfers[i]);
        if (JS_IsException(r->objects[i])) return -1;
    }
    for (size_t i = 0; i < in->count; i++) {
        DItem *item = &in->items[i];
        bool container;
        JSValue value = item_make(r, item, &container);
        if (JS_IsException(value)) return -1;
        if (item->kind != K_PRIMITIVE && item->kind != K_REFERENCE && item->kind != K_VIEW)
            r->objects[item->id] = JS_DupValue(r->ctx, value);
        if (container) {
            uint64_t left = item->kind == K_VIEW ? 1 : item->kind == K_ERROR ? 1 : item->count;
            if (fill_push(r, i, value, left) < 0) return -1;
        } else if (deliver(r, item->key, value) < 0) return -1;
        if (settle(r) < 0) return -1;
    }
    return r->rooted && r->fill_count == 0 ? 0 : -1;
}

JSValue d_clone_read(os64_dom_t *dom, JSContext *ctx, DClone *clone)
{
    if (learn_classes(dom, ctx) < 0) return JS_EXCEPTION;
    DReader r = {.dom = dom, .ctx = ctx, .in = clone, .root = JS_UNDEFINED};
    if (clone->objects != 0) {
        r.objects = js_malloc(ctx, (size_t)clone->objects * sizeof(*r.objects));
        if (r.objects == NULL) return JS_EXCEPTION;
        for (uint32_t i = 0; i < clone->objects; i++) r.objects[i] = JS_UNDEFINED;
    }
    int result = read_items(&r);
    if (result < 0 && !JS_HasException(ctx)) JS_ThrowTypeError(ctx, "A clone's record is not one");
    for (size_t i = 0; i < r.fill_count; i++) {
        JS_FreeValue(ctx, r.fills[i].container);
        JS_FreeValue(ctx, r.fills[i].map_key);
    }
    js_free(ctx, r.fills);
    for (uint32_t i = 0; i < clone->objects; i++) JS_FreeValue(ctx, r.objects[i]);
    js_free(ctx, r.objects);
    if (result < 0) {
        JS_FreeValue(ctx, r.root);
        return JS_EXCEPTION;
    }
    return r.root;
}

/* ── structuredClone ──────────────────────────────────────────────────── */

int d_clone_transfer(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, int at, JSValue **list,
                     size_t *count)
{
    *list = NULL;
    *count = 0;
    int init = d_dictionary(ctx, argc, argv, at);
    if (init <= 0) return init;
    JSValue transfer = JS_GetPropertyStr(ctx, argv[at], "transfer");
    if (JS_IsException(transfer)) return -1;
    if (JS_IsUndefined(transfer)) return 0;
    int result = d_sequence(dom, ctx, transfer, D_SEQUENCE_MAX, list, count);
    JS_FreeValue(ctx, transfer);
    return result < 0 ? -1 : d_clone_objects(ctx, *list, *count);
}

int d_clone_objects(JSContext *ctx, const JSValue *list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (!JS_IsObject(list[i])) {
            JS_ThrowTypeError(ctx, "A transfer list holds objects");
            return -1;
        }
    return 0;
}

static JSValue clone_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "structuredClone needs a value");
    JSValue *transfer;
    size_t count;
    if (d_clone_transfer(dom, ctx, argc, argv, 1, &transfer, &count) < 0) {
        d_sequence_free(ctx, transfer, count);
        return JS_EXCEPTION;
    }
    DClone *clone = d_clone_write(dom, ctx, argv[0], transfer, count);
    d_sequence_free(ctx, transfer, count);
    if (clone == NULL) return JS_EXCEPTION;
    JSValue copy = d_clone_read(dom, ctx, clone);
    d_clone_free(dom->engine, clone);
    return copy;
}

int d_clone_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (d_clone_setup(dom, ctx, global) < 0) return -1;
    return d_method_named(dom, ctx, global, "structuredClone", clone_method, 1, 0);
}
