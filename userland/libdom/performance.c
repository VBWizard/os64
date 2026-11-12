#include "internal.h"
#include "os64/date.h"

/* `performance` (High Resolution Time; Performance Timeline; User Timing;
 * Navigation Timing's `timing`, `navigation` and the `navigation` entry;
 * Resource Timing's buffer members, over an empty buffer).
 *
 * THE CLOCK. A page's time origin is when its navigation began, read once
 * from the host's timing (options.timing) when the binding is made, or the
 * binding's own creation when the host gives none; now() is the host's
 * microsecond clock (options.now_us) against it. Both now() and timeOrigin
 * are coarsened to 100 microseconds, the resolution HR-Time recommends for
 * a context that is not cross-origin isolated, which none here is.
 *
 * THE ENTRIES. Marks and measures are kept in the order made, in an engine
 * object the registry holds, so the collector sees what they hold: one
 * with no prototype, which no page is ever handed, counted in C; an
 * entry's own state (its name, times and detail) is engine memory traced
 * by the collector, as an Event's is. The navigation entry is made once and
 * reads the host's timing as it stands whenever asked. A query answers the
 * matching entries in the order of their startTime, ties in the order
 * made. Nothing else is recorded: `resource`, `paint` and every other type
 * answers an empty list, which is what a browser that records none of them
 * answers.
 *
 * THE OBSERVERS (Performance Timeline § 5). A PerformanceObserver is told
 * of the entries this file records and of no others: supportedEntryTypes
 * names mark, measure and navigation, and an observe() for any other type
 * is ignored, as the standard has a browser ignore a type it does not
 * support. A mark or measure that performance.mark() or measure() makes is
 * queued to each registered observer that observes its type; the
 * navigation entry is queued once the window's load event has run, as
 * HTML's "the end" queues it, and a `buffered` observe() is handed it only
 * from then (d_performance_loaded). The registered observers are held in C, in the
 * order registered, which keeps each alive while registered, as its
 * global's list does; the entries waiting for one are its own, traced by
 * the collector with its callback. Delivery is one task, a binding timer
 * due at once, queued when an observer is handed an entry and not already
 * queued.
 *
 * Navigation Timing's attributes come from the host's milestones. Those it
 * does not measure are given as the standard has them for a document that
 * did not wait for them: the domain lookup, the connection and the request
 * read as fetchStart (a connection already open), the secure connection as
 * fetchStart for an https page and 0 for any other, and the unload and
 * redirect pairs 0 (a redirect's times are not measured, though their
 * count, redirectCount, is the host's: os64_dom_timing_t.redirects). */

static JSClassID performance_slot, entry_slot, timing_slot, navigation_slot;

enum { K_MARK, K_MEASURE, K_NAVIGATION };
/* dom->entry_prototypes: PerformanceEntry's, then each kind's. */
enum { P_ENTRY, P_MARK, P_MEASURE, P_RESOURCE, P_NAVIGATION };

typedef struct {
    JSValue name, detail;
    double start, duration;
    int kind;
} DEntry;

static const char *const kKindNames[] = {"mark", "measure", "navigation"};

/* PerformanceTiming's attributes, in the standard's order, and where each
 * comes from: a milestone, the origin, nothing (0), or the secure
 * connection's rule. */
#define T_ORIGIN (-1)
#define T_NONE (-2)
#define T_SECURE (-3)
static const struct { const char *name; int at; } kTiming[] = {
    {"navigationStart", T_ORIGIN}, {"unloadEventStart", T_NONE}, {"unloadEventEnd", T_NONE},
    {"redirectStart", T_NONE}, {"redirectEnd", T_NONE}, {"fetchStart", OS64_DOM_TIMING_FETCH_START},
    {"domainLookupStart", OS64_DOM_TIMING_FETCH_START}, {"domainLookupEnd", OS64_DOM_TIMING_FETCH_START},
    {"connectStart", OS64_DOM_TIMING_FETCH_START}, {"connectEnd", OS64_DOM_TIMING_FETCH_START},
    {"secureConnectionStart", T_SECURE}, {"requestStart", OS64_DOM_TIMING_FETCH_START},
    {"responseStart", OS64_DOM_TIMING_RESPONSE_START}, {"responseEnd", OS64_DOM_TIMING_RESPONSE_END},
    {"domLoading", OS64_DOM_TIMING_DOM_LOADING}, {"domInteractive", OS64_DOM_TIMING_DOM_INTERACTIVE},
    {"domContentLoadedEventStart", OS64_DOM_TIMING_DOM_CONTENT_LOADED_START},
    {"domContentLoadedEventEnd", OS64_DOM_TIMING_DOM_CONTENT_LOADED_END},
    {"domComplete", OS64_DOM_TIMING_DOM_COMPLETE}, {"loadEventStart", OS64_DOM_TIMING_LOAD_START},
    {"loadEventEnd", OS64_DOM_TIMING_LOAD_END},
};
#define TIMING_COUNT (sizeof(kTiming) / sizeof(kTiming[0]))

/* Whether the page's address begins `prefix` (a scheme and its colon). */
static bool page_is(const os64_dom_t *dom, const char *prefix)
{
    size_t length = os64_strlen(prefix);
    return dom->url != NULL && os64_strlen(dom->url) >= length && os64_memcmp(dom->url, prefix, length) == 0;
}

/* ── The clock ───────────────────────────────────────────────────────── */

static uint64_t now_us(const os64_dom_t *dom)
{
    if (dom->options.now_us != NULL) return dom->options.now_us(dom->options.host_opaque);
    return dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) * 1000 : 0;
}

/* Microseconds as milliseconds, to the 100 microseconds HR-Time
 * recommends. */
static double coarse(int64_t us)
{
    int64_t tenths = us >= 0 ? us / 100 : -((-us + 99) / 100);
    return (double)tenths / 10.0;
}

double d_performance_now(const os64_dom_t *dom)
{
    return coarse((int64_t)(now_us(dom) - dom->time_origin_us));
}

static void timing_read(const os64_dom_t *dom, os64_dom_timing_t *t)
{
    os64_memset(t, 0, sizeof(*t));
    for (int i = 0; i < OS64_DOM_TIMING_COUNT; i++) t->at[i] = OS64_DOM_TIMING_NOT_YET;
    if (dom->options.timing != NULL) dom->options.timing(dom->options.host_opaque, t);
}

/* An attribute of `timing`'s, as milliseconds since the Unix epoch
 * (PerformanceTiming's unit); 0 for one that has not happened. */
static uint64_t timing_epoch(const os64_dom_t *dom, const os64_dom_timing_t *t, size_t which)
{
    int64_t origin = dom->time_origin_epoch_us;
    int at = kTiming[which].at;
    uint64_t offset;
    if (at == T_ORIGIN) offset = 0;
    else if (at == T_NONE) return 0;
    else if (at == T_SECURE) {
        if (!page_is(dom, "https:")) return 0;
        offset = t->at[OS64_DOM_TIMING_FETCH_START];
    } else offset = t->at[at];
    if (offset == OS64_DOM_TIMING_NOT_YET || origin < 0) return 0;
    return ((uint64_t)origin + offset) / 1000;
}

/* The same attribute as the navigation entry has it: milliseconds after the
 * origin, 0 for one that has not happened. */
static double timing_relative(const os64_dom_t *dom, const os64_dom_timing_t *t, size_t which)
{
    int at = kTiming[which].at;
    if (at == T_ORIGIN || at == T_NONE) return 0;
    if (at == T_SECURE) {
        if (!page_is(dom, "https:")) return 0;
        at = OS64_DOM_TIMING_FETCH_START;
    }
    return t->at[at] == OS64_DOM_TIMING_NOT_YET ? 0 : coarse((int64_t)t->at[at]);
}

static ptrdiff_t timing_index(const char *name, size_t length)
{
    for (size_t i = 0; i < TIMING_COUNT; i++)
        if (os64_strlen(kTiming[i].name) == length && os64_memcmp(kTiming[i].name, name, length) == 0)
            return (ptrdiff_t)i;
    return -1;
}

/* ── Entries ─────────────────────────────────────────────────────────── */

static void entry_finalizer(JSRuntime *rt, JSValue value)
{
    DEntry *entry = JS_GetOpaque(value, os64_js_class_id(&entry_slot));
    if (entry == NULL) return;
    JS_FreeValueRT(rt, entry->name);
    JS_FreeValueRT(rt, entry->detail);
    js_free_rt(rt, entry);
}

static void entry_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DEntry *entry = JS_GetOpaque(value, os64_js_class_id(&entry_slot));
    if (entry == NULL) return;
    JS_MarkValue(rt, entry->name, mark);
    JS_MarkValue(rt, entry->detail, mark);
}

/* A new entry of `kind` whose prototype is `prototype`; `name` is held, and
 * `detail` taken whatever happens. */
static JSValue entry_new(JSContext *ctx, JSValueConst prototype, int kind, JSValueConst name, double start,
                         double duration, JSValue detail)
{
    JSValue object = JS_NewObjectProtoClass(ctx, prototype, os64_js_class_id(&entry_slot));
    DEntry *entry = JS_IsException(object) ? NULL : js_mallocz(ctx, sizeof(*entry));
    if (entry == NULL) {
        JS_FreeValue(ctx, object);
        JS_FreeValue(ctx, detail);
        return JS_EXCEPTION;
    }
    *entry = (DEntry){JS_DupValue(ctx, name), detail, start, duration, kind};
    JS_SetOpaque(object, entry);
    return object;
}

static DEntry *entry_of(JSContext *ctx, JSValueConst value)
{
    DEntry *entry = JS_GetOpaque(value, os64_js_class_id(&entry_slot));
    if (entry == NULL) JS_ThrowTypeError(ctx, "Expected a PerformanceEntry");
    return entry;
}

/* StructuredSerialize then StructuredDeserialize, which `detail` is given
 * as: the binding's structured clone (clone.c), so what it refuses is a
 * DataCloneError and what a getter throws is thrown. Undefined and null
 * are null. */
static JSValue detail_clone(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    if (JS_IsUndefined(value) || JS_IsNull(value)) return JS_NULL;
    DClone *clone = d_clone_write(dom, ctx, value, NULL, 0);
    if (clone == NULL) return JS_EXCEPTION;
    JSValue copy = d_clone_read(dom, ctx, clone);
    d_clone_free(dom->engine, clone);
    return copy;
}

/* A DOMHighResTimeStamp, which is a restricted double: NaN and the
 * infinities are refused. */
static int timestamp(JSContext *ctx, JSValueConst value, double *out)
{
    if (JS_ToFloat64(ctx, out, value) < 0) return -1;
    if (*out != *out || *out > 1.7976931348623157e308 || *out < -1.7976931348623157e308) {
        JS_ThrowTypeError(ctx, "A time must be a finite number");
        return -1;
    }
    return 0;
}

/* ── The buffer ──────────────────────────────────────────────────────── */

/* The buffer is an object with no prototype that no page is ever handed,
 * its entries defined at 0..performance_count-1: nothing a page does to
 * Array.prototype or Object.prototype runs when it is written or read, and
 * every walk of it is bounded by the count this file keeps. */
static JSValue buffer_new(JSContext *ctx)
{
    return JS_NewObjectProto(ctx, JS_NULL);
}

static int buffer_add(os64_dom_t *dom, JSContext *ctx, JSValueConst entry)
{
    if (dom->performance_count == UINT32_MAX - 1) {
        d_error(ctx, "QuotaExceededError", "Performance entry quota exceeded");
        return -1;
    }
    if (JS_DefinePropertyValueUint32(ctx, dom->performance_entries->value, dom->performance_count,
                                     JS_DupValue(ctx, entry), JS_PROP_C_W_E) < 0)
        return -1;
    dom->performance_count++;
    return 0;
}

/* Whether an entry's name is `name` (undefined matches every name). */
static bool named(JSContext *ctx, const DEntry *entry, JSValueConst name)
{
    return JS_IsUndefined(name) || JS_StrictEq(ctx, entry->name, name);
}

/* Takes the marks (or measures) named `name` (every one, for undefined)
 * out of the buffer. */
static int buffer_clear(os64_dom_t *dom, JSContext *ctx, int kind, JSValueConst name)
{
    JSValue kept = buffer_new(ctx);
    if (JS_IsException(kept)) return -1;
    uint32_t count = dom->performance_count, used = 0;
    for (uint32_t i = 0; i < count; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, dom->performance_entries->value, i);
        if (JS_IsException(item)) { JS_FreeValue(ctx, kept); return -1; }
        DEntry *entry = JS_GetOpaque(item, os64_js_class_id(&entry_slot));
        if (entry != NULL && entry->kind == kind && named(ctx, entry, name)) {
            JS_FreeValue(ctx, item);
            continue;
        }
        if (JS_DefinePropertyValueUint32(ctx, kept, used++, item, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, kept);
            return -1;
        }
    }
    JS_FreeValue(ctx, dom->performance_entries->value);
    dom->performance_entries->value = kept;
    dom->performance_count = used;
    return 0;
}

typedef struct { JSValue value; double start; } Found;

/* The entries of `kind` (-1: every kind) named `name` (undefined: any), as
 * a new Array in startTime order: those of `list`, `list_count` of them
 * (a PerformanceObserverEntryList's), or with `list` NULL the buffer's, the
 * navigation entry among them. */
static JSValue entries_query(os64_dom_t *dom, JSContext *ctx, int kind, JSValueConst name, const JSValue *list,
                             uint32_t list_count)
{
    uint32_t count = list != NULL ? list_count : dom->performance_count;
    /* The entries found, and as many again for the sort's scratch. */
    Found *found = d_alloc(dom, sizeof(*found) * ((size_t)count + 1) * 2);
    if (found == NULL) return d_error(ctx, "QuotaExceededError", "Performance entry quota exceeded");
    Found *scratch = found + count + 1;
    uint32_t used = 0;
    JSValue result = JS_EXCEPTION;
    const DEntry *navigation = list != NULL ? NULL
                             : JS_GetOpaque(dom->navigation_entry->value, os64_js_class_id(&entry_slot));
    if ((kind < 0 || kind == K_NAVIGATION) && navigation != NULL && named(ctx, navigation, name))
        found[used++] = (Found){JS_DupValue(ctx, dom->navigation_entry->value), 0};
    for (uint32_t i = 0; i < count; i++) {
        JSValue item = list != NULL ? JS_DupValue(ctx, list[i])
                     : JS_GetPropertyUint32(ctx, dom->performance_entries->value, i);
        if (JS_IsException(item)) goto done;
        DEntry *entry = JS_GetOpaque(item, os64_js_class_id(&entry_slot));
        if (entry == NULL || (kind >= 0 && entry->kind != kind) || !named(ctx, entry, name)) {
            JS_FreeValue(ctx, item);
            continue;
        }
        found[used] = (Found){item, entry->start};
        used++;
    }
    /* A stable merge sort: a page may make marks at any startTime it
     * likes, in any order, as many as its budget holds. */
    Found *from = found, *to = scratch;
    for (uint32_t width = 1; width < used; width *= 2) {
        for (uint32_t lo = 0; lo < used; lo += 2 * width) {
            uint32_t mid = used - lo > width ? lo + width : used;
            uint32_t hi = used - mid > width ? mid + width : used;
            uint32_t a = lo, b = mid, at = lo;
            while (a < mid && b < hi) to[at++] = from[b].start < from[a].start ? from[b++] : from[a++];
            while (a < mid) to[at++] = from[a++];
            while (b < hi) to[at++] = from[b++];
        }
        Found *swap = from;
        from = to;
        to = swap;
    }
    result = JS_NewArray(ctx);
    for (uint32_t i = 0; !JS_IsException(result) && i < used; i++) {
        if (JS_DefinePropertyValueUint32(ctx, result, i, from[i].value, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, result);
            result = JS_EXCEPTION;
        }
        from[i].value = JS_UNDEFINED;
    }
    for (uint32_t i = 0; i < used; i++) found[i].value = from[i].value;
done:
    for (uint32_t i = 0; i < used; i++) JS_FreeValue(ctx, found[i].value);
    d_free(dom, found);
    return result;
}

/* The kind an entry type names, by its exact text (the Timeline compares
 * types as they are written, case and all): -1 for a type this file
 * records none of, -2 with an exception pending. `type` is a string. */
static int kind_named(JSContext *ctx, JSValueConst type)
{
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, type);
    if (text == NULL) return -2;
    int kind = -1;
    for (int k = 0; k < 3; k++)
        if (os64_strlen(kKindNames[k]) == length && os64_memcmp(text, kKindNames[k], length) == 0) kind = k;
    JS_FreeCString(ctx, text);
    return kind;
}

/* getEntriesByType(type) and, `by_name`, getEntriesByName(name, type):
 * Performance's, and with `list` a PerformanceObserverEntryList's (as
 * entries_query has them). A type no entry has answers an empty Array. */
static JSValue query_by(os64_dom_t *dom, JSContext *ctx, bool by_name, int argc, JSValueConst *argv,
                        const JSValue *list, uint32_t list_count)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "The query needs an argument");
    JSValue first = JS_ToString(ctx, argv[0]);
    if (JS_IsException(first)) return first;
    JSValue type = !by_name ? JS_DupValue(ctx, first)
                 : argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToString(ctx, argv[1]) : JS_UNDEFINED;
    if (JS_IsException(type)) { JS_FreeValue(ctx, first); return type; }
    int kind = JS_IsUndefined(type) ? -1 : kind_named(ctx, type);
    bool known = JS_IsUndefined(type) || kind >= 0;
    JS_FreeValue(ctx, type);
    JSValue result = kind == -2 ? JS_EXCEPTION
                   : !known ? JS_NewArray(ctx)
                   : entries_query(dom, ctx, kind, by_name ? first : JS_UNDEFINED, list, list_count);
    JS_FreeValue(ctx, first);
    return result;
}

/* ── The observers' queue ────────────────────────────────────────────── */

static JSClassID observer_slot, list_slot;

/* An observer's observer type (Performance Timeline's observe()): unset until
 * its first observe(), then "single" for one that named a `type` and
 * "multiple" for one given `entryTypes`, and never changed after. */
enum { O_UNSET, O_SINGLE, O_MULTIPLE };

/* A PerformanceObserver: its callback and the script it is, the entries
 * queued for it and not yet delivered (its observer buffer, in the order
 * queued), the kinds it observes (a bit each: the `entryTypes` it was last
 * given, or the `type` of each observe() call, which is all of an
 * observe()'s options that outlives the call here), its observer type,
 * and whether its next callback is told droppedEntriesCount. */
typedef struct {
    /* The timeline and observer registry belong to the constructor realm. */
    JSValue anchor, callback;
    JSValue *entries;
    uint32_t count, cap, script;
    uint8_t mode, kinds;
    bool dropped_owed;
} DObserver;

/* A PerformanceObserverEntryList: the entries one delivery handed it. */
typedef struct {
    JSValue *entries;
    uint32_t count;
} DEntryList;

static void values_free(JSRuntime *rt, JSValue *values, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) JS_FreeValueRT(rt, values[i]);
    js_free_rt(rt, values);
}

static void observer_finalizer(JSRuntime *rt, JSValue value)
{
    DObserver *o = JS_GetOpaque(value, os64_js_class_id(&observer_slot));
    if (o == NULL) return;
    JS_FreeValueRT(rt, o->anchor);
    JS_FreeValueRT(rt, o->callback);
    values_free(rt, o->entries, o->count);
    js_free_rt(rt, o);
}

static void observer_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DObserver *o = JS_GetOpaque(value, os64_js_class_id(&observer_slot));
    if (o == NULL) return;
    JS_MarkValue(rt, o->anchor, mark);
    JS_MarkValue(rt, o->callback, mark);
    for (uint32_t i = 0; i < o->count; i++) JS_MarkValue(rt, o->entries[i], mark);
}

static void list_finalizer(JSRuntime *rt, JSValue value)
{
    DEntryList *l = JS_GetOpaque(value, os64_js_class_id(&list_slot));
    if (l == NULL) return;
    values_free(rt, l->entries, l->count);
    js_free_rt(rt, l);
}

static void list_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DEntryList *l = JS_GetOpaque(value, os64_js_class_id(&list_slot));
    if (l == NULL) return;
    for (uint32_t i = 0; i < l->count; i++) JS_MarkValue(rt, l->entries[i], mark);
}

/* Room in `o`'s buffer for `more` entries beyond those it holds; -1 with
 * an exception pending. */
static int observer_room(JSContext *ctx, DObserver *o, uint32_t more)
{
    if (more <= o->cap - o->count) return 0;
    if (more > UINT32_MAX / 2 - o->count) {
        d_error(ctx, "QuotaExceededError", "Performance entry quota exceeded");
        return -1;
    }
    uint32_t cap = o->cap != 0 ? o->cap : 8;
    while (cap < o->count + more) cap *= 2;
    JSValue *grown = js_realloc(ctx, o->entries, (size_t)cap * sizeof(*grown));
    if (grown == NULL) return -1;
    o->entries = grown;
    o->cap = cap;
    return 0;
}

static DObserver *observer_at(const os64_dom_t *dom, uint32_t i)
{
    return JS_GetOpaque(dom->performance_observers[i], os64_js_class_id(&observer_slot));
}

/* A new PerformanceObserverEntryList, which `o`'s waiting entries leave
 * `o` for. */
static JSValue list_take(JSContext *ctx, DObserver *o)
{
    JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&list_slot));
    DEntryList *l = JS_IsException(object) ? NULL : js_mallocz(ctx, sizeof(*l));
    if (l == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    *l = (DEntryList){o->entries, o->count};
    o->entries = NULL;
    o->count = o->cap = 0;
    JS_SetOpaque(object, l);
    return object;
}

/* The exception pending, reported as `script`'s failure, unless the
 * runtime is stopping, which answers false and leaves it pending. */
static bool report_pending(os64_dom_t *dom, JSContext *ctx, uint32_t script)
{
    if (!d_exception_catchable(ctx)) return false;
    JSValue error = JS_GetException(ctx);
    d_report_as(dom, ctx, error, script);
    JS_FreeValue(ctx, error);
    return true;
}

/* "Queue the PerformanceObserver task"'s task: every observer registered as it
 * begins, in the order registered, that has entries waiting is called with
 * them as a PerformanceObserverEntryList, itself (as `this` too), and its
 * callback options. What a callback throws is its script's, reported, and
 * the next is called. The callbacks share the one task, so a microtask one
 * queues runs when the task ends rather than before the next callback
 * (DEBTS.md). */
static JSValue observer_task(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    dom->performance_task_queued = false;
    uint32_t count = dom->performance_observer_count;
    if (count == 0) return JS_UNDEFINED;
    JSValue *notify = d_alloc(dom, (size_t)count * sizeof(*notify));
    if (notify == NULL) return d_error(ctx, "QuotaExceededError", "Performance observer quota exceeded");
    for (uint32_t i = 0; i < count; i++) notify[i] = JS_DupValue(ctx, dom->performance_observers[i]);
    JSValue result = JS_UNDEFINED;
    for (uint32_t i = 0; i < count && !JS_IsException(result); i++) {
        DObserver *o = JS_GetOpaque(notify[i], os64_js_class_id(&observer_slot));
        if (o == NULL || o->count == 0) continue;
        int eligible = d_delivery_eligible(dom, ctx, o->callback);
        if (eligible <= 0) {
            /* Consume obsolete entries without allocating an author-facing
             * list, retaining the observer itself and its registration. */
            values_free(dom->engine, o->entries, o->count);
            o->entries = NULL;
            o->count = o->cap = 0;
            o->dropped_owed = false;
            if (eligible < 0 && !report_pending(dom, ctx, o->script)) result = JS_EXCEPTION;
            continue;
        }
        /* This browser drops no entry (marks and measures are kept without
         * limit, and the navigation entry's buffer holds the one there
         * is), so a count owed is 0. */
        JSValue options = JS_NewObject(ctx);
        if (!JS_IsException(options) && o->dropped_owed &&
            JS_DefinePropertyValueStr(ctx, options, "droppedEntriesCount", JS_NewInt32(ctx, 0), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, options);
            options = JS_EXCEPTION;
        }
        JSValue list = JS_IsException(options) ? JS_EXCEPTION : list_take(ctx, o);
        if (JS_IsException(list)) {
            JS_FreeValue(ctx, options);
            if (!report_pending(dom, ctx, D_SCRIPT_PAGE)) result = JS_EXCEPTION;
            continue;
        }
        o->dropped_owed = false;
        uint32_t script = o->script;
        JSValue callback = JS_DupValue(ctx, o->callback);
        JSValueConst args[3] = {list, notify[i], options};
        DScripts before = d_script_enter(dom, script);
        JSValue called = JS_Call(ctx, callback, notify[i], 3, args);
        d_script_leave(dom, &before, false);
        JS_FreeValue(ctx, callback);
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, options);
        if (!JS_IsException(called)) JS_FreeValue(ctx, called);
        else if (!report_pending(dom, ctx, script)) result = JS_EXCEPTION;
    }
    for (uint32_t i = 0; i < count; i++) JS_FreeValue(ctx, notify[i]);
    d_free(dom, notify);
    return result;
}

/* "Queue the PerformanceObserver task": one binding timer due at once,
 * which the host runs as the task it is, however many entries wait for it.
 * One the timer table has no room for is not queued, and the entries wait
 * in their observers for the next: the mark or the measure that queued
 * them is made by then, and refusing it would misstate what was done. -1
 * with an exception pending only when the runtime is stopping. */
static int queue_task(os64_dom_t *dom, JSContext *ctx)
{
    if (!d_delivery_active(dom) || dom->performance_task_queued) return 0;
    JSValue data[1] = {dom->anchor->value};
    JSValue fired = JS_NewCFunctionData(ctx, observer_task, 0, 0, 1, data);
    if (!JS_IsException(fired) && d_timer_add(dom, ctx, fired, 0) != 0) {
        dom->performance_task_queued = true;
        return 0;
    }
    if (!d_exception_catchable(ctx)) return -1;
    JS_FreeValue(ctx, JS_GetException(ctx));
    return 0;
}

/* "Queue a PerformanceEntry", in two halves around the entry's
 * joining the buffer, so that one that cannot be queued is refused rather
 * than half queued: queue_room makes room for an entry of `kind` in every
 * registered observer that observes it (-1 with an exception pending),
 * and queue_entry appends `entry` there, which runs no page code and so
 * finds the same observers, then queues the task when one took it. */
static int queue_room(os64_dom_t *dom, JSContext *ctx, int kind)
{
    for (uint32_t i = 0; i < dom->performance_observer_count; i++) {
        DObserver *o = observer_at(dom, i);
        if (o != NULL && (o->kinds & (1u << kind)) != 0 && observer_room(ctx, o, 1) < 0) return -1;
    }
    return 0;
}

static int queue_entry(os64_dom_t *dom, JSContext *ctx, JSValueConst entry, int kind)
{
    bool taken = false;
    for (uint32_t i = 0; i < dom->performance_observer_count; i++) {
        DObserver *o = observer_at(dom, i);
        if (o == NULL || (o->kinds & (1u << kind)) == 0) continue;
        o->entries[o->count++] = JS_DupValue(ctx, entry);
        taken = true;
    }
    return taken ? queue_task(dom, ctx) : 0;
}

/* A mark or a measure performance.mark() or measure() made, recorded:
 * added to the buffer and queued to its observers. -1 with an exception
 * pending. */
static int entry_record(os64_dom_t *dom, JSContext *ctx, JSValueConst entry, int kind)
{
    if (queue_room(dom, ctx, kind) < 0 || buffer_add(dom, ctx, entry) < 0) return -1;
    return queue_entry(dom, ctx, entry, kind);
}

/* ── User Timing ─────────────────────────────────────────────────────── */

/* "Convert a mark to a timestamp": a name of PerformanceTiming's is that
 * attribute after navigationStart (navigationStart itself 0;
 * InvalidAccessError for one that has not happened); another name is the latest mark by that name (SyntaxError when
 * there is none); a number is itself, and may not be negative. -1 thrown. */
static int mark_time(os64_dom_t *dom, JSContext *ctx, JSValueConst mark, double *out)
{
    if (JS_IsNumber(mark)) {
        if (timestamp(ctx, mark, out) < 0) return -1;
        if (*out < 0) {
            JS_ThrowTypeError(ctx, "A mark's time may not be negative");
            return -1;
        }
        return 0;
    }
    JSValue name = JS_ToString(ctx, mark);
    if (JS_IsException(name)) return -1;
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, name);
    if (text == NULL) { JS_FreeValue(ctx, name); return -1; }
    ptrdiff_t timing = timing_index(text, length);
    JS_FreeCString(ctx, text);
    if (timing >= 0) {
        JS_FreeValue(ctx, name);
        /* "If name is navigationStart, return 0" (User Timing § 3.2),
         * whatever the wall clock the host could or could not read. */
        if (timing == 0) {
            *out = 0;
            return 0;
        }
        os64_dom_timing_t t;
        timing_read(dom, &t);
        uint64_t at = timing_epoch(dom, &t, (size_t)timing);
        if (at == 0) {
            d_error(ctx, "InvalidAccessError", "That timing attribute has not happened yet");
            return -1;
        }
        *out = (double)at - (double)timing_epoch(dom, &t, 0);
        return 0;
    }
    for (uint32_t i = dom->performance_count; i > 0; i--) {
        JSValue item = JS_GetPropertyUint32(ctx, dom->performance_entries->value, i - 1);
        if (JS_IsException(item)) { JS_FreeValue(ctx, name); return -1; }
        DEntry *entry = JS_GetOpaque(item, os64_js_class_id(&entry_slot));
        bool hit = entry != NULL && entry->kind == K_MARK && JS_StrictEq(ctx, entry->name, name);
        if (hit) *out = entry->start;
        JS_FreeValue(ctx, item);
        if (hit) { JS_FreeValue(ctx, name); return 0; }
    }
    JS_FreeValue(ctx, name);
    d_error(ctx, "SyntaxError", "There is no mark by that name");
    return -1;
}

/* The PerformanceMark constructor's steps (User Timing § 2.2.1), which
 * mark() runs too: a new mark, not yet in the buffer. */
static JSValue mark_make(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "A mark needs a name");
    JSValue name = JS_ToString(ctx, argv[0]);
    if (JS_IsException(name)) return name;
    /* The arguments are converted first, the dictionary's members in their
     * names' order; then the constructor's steps, in theirs. */
    JSValue detail = JS_UNDEFINED, start = JS_UNDEFINED, result = JS_EXCEPTION;
    double time = 0;
    int options = d_dictionary(ctx, argc, argv, 1);
    if (options > 0) {
        detail = JS_GetPropertyStr(ctx, argv[1], "detail");
        if (JS_IsException(detail)) goto done;
        start = JS_GetPropertyStr(ctx, argv[1], "startTime");
        if (JS_IsException(start)) goto done;
    }
    if (options < 0 || (!JS_IsUndefined(start) && timestamp(ctx, start, &time) < 0)) goto done;
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, name);
    if (text == NULL) goto done;
    bool reserved = timing_index(text, length) >= 0;
    JS_FreeCString(ctx, text);
    if (reserved) {
        d_error(ctx, "SyntaxError", "A mark may not take a name of PerformanceTiming's");
        goto done;
    }
    if (JS_IsUndefined(start)) time = coarse((int64_t)(now_us(dom) - dom->time_origin_us));
    else if (time < 0) {
        JS_ThrowTypeError(ctx, "A mark's startTime may not be negative");
        goto done;
    }
    JSValue cloned = detail_clone(dom, ctx, detail);
    if (!JS_IsException(cloned)) result = entry_new(ctx, prototype, K_MARK, name, time, 0, cloned);
done:
    JS_FreeValue(ctx, detail);
    JS_FreeValue(ctx, start);
    JS_FreeValue(ctx, name);
    return result;
}

/* `duration`, already a DOMHighResTimeStamp, run through "convert a mark
 * to a timestamp" where measure() uses it: a negative one is a TypeError.
 * -1 thrown. */
static int duration_time(JSContext *ctx, double duration)
{
    if (duration >= 0) return 0;
    JS_ThrowTypeError(ctx, "A measure's duration may not be negative");
    return -1;
}

/* measure(name, startOrMeasureOptions, endMark) (User Timing § 2.1.3). */
static JSValue measure(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "A measure needs a name");
    JSValue name = JS_ToString(ctx, argv[0]);
    if (JS_IsException(name)) return name;
    /* (DOMString or PerformanceMeasureOptions): undefined, null and an
     * object are the dictionary, and anything else is a string. Its
     * members are read in their names' order. */
    JSValueConst given = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool options = JS_IsUndefined(given) || JS_IsNull(given) || JS_IsObject(given);
    static const char *const kMembers[] = {"detail", "duration", "end", "start"};
    JSValue member[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    JSValue start_text = JS_UNDEFINED;
    JSValue result = JS_EXCEPTION;
    bool any = false;
    for (int i = 0; options && JS_IsObject(given) && i < 4; i++) {
        member[i] = JS_GetPropertyStr(ctx, given, kMembers[i]);
        if (JS_IsException(member[i])) goto done;
        any = any || !JS_IsUndefined(member[i]);
        /* start and end are (DOMString or DOMHighResTimeStamp). */
        if (i >= 2 && !JS_IsUndefined(member[i]) && !JS_IsNumber(member[i])) {
            JSValue text = JS_ToString(ctx, member[i]);
            JS_FreeValue(ctx, member[i]);
            member[i] = text;
            if (JS_IsException(text)) goto done;
        }
    }
    double duration = 0;
    if (!JS_IsUndefined(member[1]) && timestamp(ctx, member[1], &duration) < 0) goto done;
    if (!options) {
        start_text = JS_ToString(ctx, given);
        if (JS_IsException(start_text)) goto done;
    }
    JSValue end_mark = argc > 2 && !JS_IsUndefined(argv[2]) ? JS_ToString(ctx, argv[2]) : JS_UNDEFINED;
    if (JS_IsException(end_mark)) goto done;
    bool has_start = !JS_IsUndefined(member[3]), has_end = !JS_IsUndefined(member[2]);
    bool has_duration = !JS_IsUndefined(member[1]);
    /* User Timing § 2.1.3 step 1 checks the options only when "at least one
     * of start, end, duration, and detail exist": an empty dictionary, or
     * one of undefineds, skips them, and an endMark beside it is used
     * (WPT measure-with-dict's measure8 and measure20). */
    if (options && any) {
        const char *why = !JS_IsUndefined(end_mark) ? "An end mark may not be given beside measure options"
                        : !has_start && !has_end ? "Measure options need a start or an end"
                        : has_start && has_end && has_duration ? "Measure options may not give start, end and duration"
                        : NULL;
        if (why != NULL) {
            JS_FreeValue(ctx, end_mark);
            JS_ThrowTypeError(ctx, "%s", why);
            goto done;
        }
    }
    double end = 0, begin = 0;
    int ok = 0;
    if (!JS_IsUndefined(end_mark)) ok = mark_time(dom, ctx, end_mark, &end);
    else if (has_end) ok = mark_time(dom, ctx, member[2], &end);
    else if (has_start && has_duration) {
        ok = mark_time(dom, ctx, member[3], &begin);
        if (ok == 0) ok = duration_time(ctx, duration);
        end = begin + duration;
    } else end = coarse((int64_t)(now_us(dom) - dom->time_origin_us));
    JS_FreeValue(ctx, end_mark);
    if (ok == 0) {
        if (has_start) ok = mark_time(dom, ctx, member[3], &begin);
        else if (has_duration && has_end) {
            ok = duration_time(ctx, duration);
            begin = end - duration;
        }
        else if (!options) ok = mark_time(dom, ctx, start_text, &begin);
        else begin = 0;
    }
    if (ok < 0) goto done;
    JSValue detail = detail_clone(dom, ctx, member[0]);
    if (JS_IsException(detail)) goto done;
    result = entry_new(ctx, dom->entry_prototypes[P_MEASURE]->value, K_MEASURE, name, begin, end - begin, detail);
    if (!JS_IsException(result) && entry_record(dom, ctx, result, K_MEASURE) < 0) {
        JS_FreeValue(ctx, result);
        result = JS_EXCEPTION;
    }
done:
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, member[i]);
    JS_FreeValue(ctx, start_text);
    JS_FreeValue(ctx, name);
    return result;
}

/* ── The objects ─────────────────────────────────────────────────────── */

enum {
    M_NOW, M_TIME_ORIGIN, M_TO_JSON, M_MARK, M_MEASURE, M_CLEAR_MARKS, M_CLEAR_MEASURES,
    M_CLEAR_RESOURCES, M_RESOURCE_SIZE,
    M_ENTRIES, M_BY_TYPE, M_BY_NAME, M_TIMING, M_NAVIGATION, M_WINDOW,
    E_NAME, E_TYPE, E_START, E_DURATION, E_DETAIL, E_TO_JSON,
    N_TYPE, N_REDIRECTS, N_TO_JSON, N_TIMING_JSON,
    R_INITIATOR, R_PROTOCOL, R_ZERO, R_SERVER_TIMING, R_NAV_TYPE, R_NAV_REDIRECTS,
    /* The navigation entry's attributes, and PerformanceTiming's: each
     * the index of a kTiming attribute past its base. */
    R_TIMING = 0x100, T_ATTRIBUTE = 0x200
};

static const char *const kTypeNames[] = {"navigate", "reload", "back_forward"};

static JSValue timing_json(os64_dom_t *dom, JSContext *ctx)
{
    os64_dom_timing_t t;
    timing_read(dom, &t);
    JSValue object = JS_NewObject(ctx);
    for (size_t i = 0; !JS_IsException(object) && i < TIMING_COUNT; i++)
        if (JS_DefinePropertyValueStr(ctx, object, kTiming[i].name,
                                      JS_NewInt64(ctx, (int64_t)timing_epoch(dom, &t, i)), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, object);
            object = JS_EXCEPTION;
        }
    return object;
}

static JSValue navigation_json(os64_dom_t *dom, JSContext *ctx)
{
    os64_dom_timing_t t;
    timing_read(dom, &t);
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object)) return object;
    if (JS_DefinePropertyValueStr(ctx, object, "type", JS_NewInt32(ctx, (int32_t)t.type), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, object, "redirectCount", JS_NewUint32(ctx, t.redirects), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

/* An entry's toJSON, WebIDL's [Default]: every attribute of the interfaces
 * on its chain that declare one, which for a mark or a measure is
 * PerformanceEntry's four alone, and for the navigation entry all of them. */
static JSValue entry_json(JSContext *ctx, JSValueConst self, const DEntry *entry);

/* serverTiming: a FrozenArray, a new one at each read, and os64 reads no
 * Server-Timing header, so it is empty. Frozen by definition (its length
 * made unwritable, the array closed), never by calling Object.freeze or
 * setting anything, which a page's script could have replaced. */
static JSValue frozen_empty_array(JSContext *ctx)
{
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    JSAtom length = JS_NewAtom(ctx, "length");
    int done = length == JS_ATOM_NULL ? -1
             : JS_DefineProperty(ctx, array, length, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                                 JS_PROP_HAS_WRITABLE | JS_PROP_THROW);
    if (length != JS_ATOM_NULL) JS_FreeAtom(ctx, length);
    if (done < 0 || JS_PreventExtensions(ctx, array) < 0) {
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    return array;
}

static JSValue performance_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                  JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (magic == M_WINDOW) {
        /* window.performance is [Replaceable]. */
        if (set) {
            JSValue global = JS_GetGlobalObject(ctx);
            int done = JS_DefinePropertyValueStr(ctx, global, "performance",
                                                 JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED), JS_PROP_C_W_E);
            JS_FreeValue(ctx, global);
            return done < 0 ? JS_EXCEPTION : JS_UNDEFINED;
        }
        return JS_DupValue(ctx, dom->performance->value);
    }
    if (magic >= N_TYPE && magic <= N_TIMING_JSON) {
        JSClassID want = magic == N_TIMING_JSON ? os64_js_class_id(&timing_slot) : os64_js_class_id(&navigation_slot);
        if (JS_GetOpaque(self, want) == NULL) return JS_ThrowTypeError(ctx, "Illegal invocation");
        if (magic == N_TIMING_JSON) return timing_json(dom, ctx);
        if (magic == N_TO_JSON) return navigation_json(dom, ctx);
        os64_dom_timing_t t;
        timing_read(dom, &t);
        return magic == N_TYPE ? JS_NewInt32(ctx, (int32_t)t.type) : JS_NewUint32(ctx, t.redirects);
    }
    if (magic >= T_ATTRIBUTE) {
        if (JS_GetOpaque(self, os64_js_class_id(&timing_slot)) == NULL)
            return JS_ThrowTypeError(ctx, "Illegal invocation");
        os64_dom_timing_t t;
        timing_read(dom, &t);
        return JS_NewInt64(ctx, (int64_t)timing_epoch(dom, &t, (size_t)(magic - T_ATTRIBUTE)));
    }
    if (magic >= E_NAME) {
        DEntry *entry = entry_of(ctx, self);
        if (entry == NULL) return JS_EXCEPTION;
        bool navigation = entry->kind == K_NAVIGATION;
        if (magic >= R_INITIATOR && !navigation) return JS_ThrowTypeError(ctx, "Illegal invocation");
        if (magic == E_DETAIL && navigation) return JS_ThrowTypeError(ctx, "Illegal invocation");
        os64_dom_timing_t t;
        if (navigation) timing_read(dom, &t);
        switch (magic) {
        case E_NAME: return JS_DupValue(ctx, entry->name);
        case E_TYPE: return JS_NewString(ctx, kKindNames[entry->kind]);
        case E_START: return JS_NewFloat64(ctx, entry->start);
        case E_DURATION:
            if (!navigation) return JS_NewFloat64(ctx, entry->duration);
            return JS_NewFloat64(ctx, timing_relative(dom, &t, TIMING_COUNT - 1));
        case E_DETAIL: return JS_DupValue(ctx, entry->detail);
        case E_TO_JSON: return entry_json(ctx, self, entry);
        case R_INITIATOR: return JS_NewString(ctx, "navigation");
        case R_PROTOCOL:
            return JS_NewString(ctx, page_is(dom, "http:") || page_is(dom, "https:") ? "http/1.1" : "");
        case R_SERVER_TIMING: return frozen_empty_array(ctx);
        case R_NAV_TYPE: return JS_NewString(ctx, kTypeNames[t.type <= 2 ? t.type : 0]);
        case R_NAV_REDIRECTS: return JS_NewUint32(ctx, t.redirects);
        case R_ZERO: return JS_NewInt32(ctx, 0);
        default:
            if (magic >= R_TIMING) return JS_NewFloat64(ctx, timing_relative(dom, &t, (size_t)(magic - R_TIMING)));
            return JS_UNDEFINED;
        }
    }
    if (JS_GetOpaque(self, os64_js_class_id(&performance_slot)) == NULL)
        return JS_ThrowTypeError(ctx, "Expected the Performance object");
    switch (magic) {
    case M_NOW: return JS_NewFloat64(ctx, d_performance_now(dom));
    case M_TIME_ORIGIN: return JS_NewFloat64(ctx, coarse(dom->time_origin_epoch_us));
    case M_TIMING: return JS_DupValue(ctx, dom->performance_timing->value);
    case M_NAVIGATION: return JS_DupValue(ctx, dom->performance_navigation->value);
    case M_TO_JSON: {
        JSValue object = JS_NewObject(ctx);
        JSValue parts[3] = {JS_NewFloat64(ctx, coarse(dom->time_origin_epoch_us)), timing_json(dom, ctx),
                            navigation_json(dom, ctx)};
        static const char *const kNames[] = {"timeOrigin", "timing", "navigation"};
        bool ok = !JS_IsException(object);
        for (int i = 0; i < 3; i++) {
            if (ok && !JS_IsException(parts[i]))
                ok = JS_DefinePropertyValueStr(ctx, object, kNames[i], parts[i], JS_PROP_C_W_E) >= 0;
            else { JS_FreeValue(ctx, parts[i]); ok = false; }
        }
        if (!ok) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
        return object;
    }
    case M_MARK: {
        JSValue made = mark_make(dom, ctx, dom->entry_prototypes[P_MARK]->value, argc, argv);
        if (!JS_IsException(made) && entry_record(dom, ctx, made, K_MARK) < 0) {
            JS_FreeValue(ctx, made);
            return JS_EXCEPTION;
        }
        return made;
    }
    case M_MEASURE: return measure(dom, ctx, argc, argv);
    case M_CLEAR_MARKS: case M_CLEAR_MEASURES: {
        JSValue name = argc > 0 && !JS_IsUndefined(argv[0]) ? JS_ToString(ctx, argv[0]) : JS_UNDEFINED;
        if (JS_IsException(name)) return name;
        int done = buffer_clear(dom, ctx, magic == M_CLEAR_MARKS ? K_MARK : K_MEASURE, name);
        JS_FreeValue(ctx, name);
        return done < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    /* Resource Timing's buffer, which holds nothing: no resource entry is
     * recorded (supportedEntryTypes), so clearing it clears none and its
     * size is never reached. The size is still converted, as WebIDL's
     * `unsigned long` is, so a call that would throw elsewhere throws here. */
    case M_CLEAR_RESOURCES: return JS_UNDEFINED;
    case M_RESOURCE_SIZE: {
        uint32_t size = 0;
        if (argc < 1) return JS_ThrowTypeError(ctx, "setResourceTimingBufferSize needs a size");
        if (JS_ToUint32(ctx, &size, argv[0]) < 0) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    case M_ENTRIES: return entries_query(dom, ctx, -1, JS_UNDEFINED, NULL, 0);
    case M_BY_TYPE: case M_BY_NAME: return query_by(dom, ctx, magic == M_BY_NAME, argc, argv, NULL, 0);
    default: return JS_ThrowTypeError(ctx, "Unknown Performance member");
    }
}

static JSValue entry_json(JSContext *ctx, JSValueConst self, const DEntry *entry)
{
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object)) return object;
    static const char *const kAll[] = {"name", "entryType", "startTime", "duration"};
    static const char *const kResource[] = {
        "initiatorType", "nextHopProtocol", "workerStart", "redirectStart", "redirectEnd", "fetchStart",
        "domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd", "secureConnectionStart",
        "requestStart", "responseStart", "responseEnd", "transferSize", "encodedBodySize",
        "decodedBodySize", "serverTiming", "unloadEventStart", "unloadEventEnd", "domInteractive",
        "domContentLoadedEventStart", "domContentLoadedEventEnd", "domComplete", "loadEventStart",
        "loadEventEnd", "type", "redirectCount"};
    size_t count = sizeof(kAll) / sizeof(kAll[0]) +
                   (entry->kind == K_NAVIGATION ? sizeof(kResource) / sizeof(kResource[0]) : 0);
    for (size_t i = 0; i < count; i++) {
        const char *name = i < 4 ? kAll[i] : kResource[i - 4];
        JSValue value = JS_GetPropertyStr(ctx, self, name);
        if (JS_IsException(value) ||
            JS_DefinePropertyValueStr(ctx, object, name, value, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
    }
    return object;
}

/* new PerformanceMark(name, options): the mark, with new.target's
 * prototype, not added to the buffer. */
static JSValue mark_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue made = mark_make(dom, ctx, JS_IsObject(prototype) ? prototype : dom->entry_prototypes[P_MARK]->value,
                             argc, argv);
    JS_FreeValue(ctx, prototype);
    return made;
}

static void clear(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

static int class_of(os64_dom_t *dom, JSClassID id, const JSClassDef *definition)
{
    return !JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, definition) < 0 ? -1 : 0;
}

/* A prototype held by the registry, inheriting `parent` when it is an
 * object; NULL with an exception pending. */
static DValue *prototype_new(os64_dom_t *dom, JSContext *ctx, JSValueConst parent)
{
    JSValue prototype = JS_IsObject(parent) ? JS_NewObjectProto(ctx, parent) : JS_NewObject(ctx);
    return d_retain(dom, ctx, prototype);
}

/* An interface object for `prototype`, named on window. Answers the
 * object, borrowed from the prototype's `constructor`, or JS_EXCEPTION. */
static JSValue interface_of(JSContext *ctx, JSValueConst global, const char *name, int length,
                            JSCFunction *constructor, JSValueConst prototype, JSValueConst parent)
{
    JSValue f = d_interface_new(ctx, name, length, constructor, prototype, parent);
    if (JS_IsException(f)) return f;
    JSValue borrowed = f;
    if (JS_DefinePropertyValueStr(ctx, global, name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return JS_EXCEPTION;
    /* The prototype's `constructor` holds it from here. */
    return borrowed;
}

int d_performance_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef performance_class = {.class_name = "Performance", .finalizer = clear};
    static const JSClassDef entry_class = {.class_name = "PerformanceEntry", .finalizer = entry_finalizer,
                                           .gc_mark = entry_mark};
    static const JSClassDef timing_class = {.class_name = "PerformanceTiming", .finalizer = clear};
    static const JSClassDef navigation_class = {.class_name = "PerformanceNavigation", .finalizer = clear};
    JSClassID performance_id = os64_js_class_id(&performance_slot), entry_id = os64_js_class_id(&entry_slot);
    JSClassID timing_id = os64_js_class_id(&timing_slot), navigation_id = os64_js_class_id(&navigation_slot);
    if (class_of(dom, performance_id, &performance_class) < 0 || class_of(dom, entry_id, &entry_class) < 0 ||
        class_of(dom, timing_id, &timing_class) < 0 || class_of(dom, navigation_id, &navigation_class) < 0)
        return -1;
    /* A mark's and a measure's detail are structured clones. */
    if (d_clone_setup(dom, ctx, global) < 0) return -1;
    /* The time origin, read once. */
    os64_dom_timing_t t;
    timing_read(dom, &t);
    if (dom->options.timing != NULL) {
        dom->time_origin_us = t.origin_us;
        dom->time_origin_epoch_us = t.origin_epoch_us;
    } else {
        os64_time_t wall;
        dom->time_origin_us = now_us(dom);
        dom->time_origin_epoch_us = os64_time(&wall) == 0 && wall.ticks_per_second != 0
            ? wall.epoch * 1000000 + (int64_t)((uint64_t)wall.ticks_into_second * 1000000 / wall.ticks_per_second)
            : 0;
    }

    /* The prototypes: Performance's, the entries', timing's, navigation's. */
    DValue *performance = prototype_new(dom, ctx, JS_UNDEFINED);
    if (performance == NULL) return -1;
    for (int p = P_ENTRY; p <= P_NAVIGATION; p++) {
        int parent = p == P_ENTRY ? -1 : p == P_NAVIGATION ? P_RESOURCE : P_ENTRY;
        dom->entry_prototypes[p] = prototype_new(dom, ctx, parent < 0 ? JS_UNDEFINED : dom->entry_prototypes[parent]->value);
        if (dom->entry_prototypes[p] == NULL) return -1;
    }
    DValue *timing = prototype_new(dom, ctx, JS_UNDEFINED), *navigation = timing != NULL ? prototype_new(dom, ctx, JS_UNDEFINED) : NULL;
    if (navigation == NULL) return -1;
    JS_SetClassProto(ctx, performance_id, JS_DupValue(ctx, performance->value));
    JS_SetClassProto(ctx, entry_id, JS_DupValue(ctx, dom->entry_prototypes[P_ENTRY]->value));
    JS_SetClassProto(ctx, timing_id, JS_DupValue(ctx, timing->value));
    JS_SetClassProto(ctx, navigation_id, JS_DupValue(ctx, navigation->value));

    /* Performance: an EventTarget, with HR-Time's, the Timeline's, User
     * Timing's and Resource Timing's members, and Navigation Timing's two
     * legacy objects. */
    JSValueConst on = performance->value;
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"now", M_NOW, 0}, {"toJSON", M_TO_JSON, 0}, {"mark", M_MARK, 1}, {"measure", M_MEASURE, 1},
        {"clearMarks", M_CLEAR_MARKS, 0}, {"clearMeasures", M_CLEAR_MEASURES, 0},
        {"getEntries", M_ENTRIES, 0}, {"getEntriesByType", M_BY_TYPE, 1}, {"getEntriesByName", M_BY_NAME, 1},
        {"clearResourceTimings", M_CLEAR_RESOURCES, 0}, {"setResourceTimingBufferSize", M_RESOURCE_SIZE, 1},
    };
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method_named(dom, ctx, on, kMethods[i].name, performance_member, kMethods[i].argc, kMethods[i].magic) < 0)
            return -1;
    if (d_accessor(dom, ctx, on, "timeOrigin", performance_member, M_TIME_ORIGIN, false) < 0 ||
        d_accessor(dom, ctx, on, "timing", performance_member, M_TIMING, false) < 0 ||
        d_accessor(dom, ctx, on, "navigation", performance_member, M_NAVIGATION, false) < 0 ||
        d_event_target(dom, ctx, on, performance_id, NULL) < 0 ||
        d_event_handler_named(dom, ctx, on, "resourcetimingbufferfull") < 0)
        return -1;

    /* PerformanceEntry and its kinds. */
    static const struct { const char *name; int magic; } kEntry[] = {
        {"name", E_NAME}, {"entryType", E_TYPE}, {"startTime", E_START}, {"duration", E_DURATION},
    };
    JSValueConst entry = dom->entry_prototypes[P_ENTRY]->value;
    for (size_t i = 0; i < sizeof(kEntry) / sizeof(kEntry[0]); i++)
        if (d_accessor(dom, ctx, entry, kEntry[i].name, performance_member, kEntry[i].magic, false) < 0) return -1;
    if (d_method(dom, ctx, entry, "toJSON", performance_member, 0, E_TO_JSON) < 0 ||
        d_accessor(dom, ctx, dom->entry_prototypes[P_MARK]->value, "detail", performance_member, E_DETAIL, false) < 0 ||
        d_accessor(dom, ctx, dom->entry_prototypes[P_MEASURE]->value, "detail", performance_member, E_DETAIL, false) < 0)
        return -1;
    JSValueConst resource = dom->entry_prototypes[P_RESOURCE]->value;
    JSValueConst nav = dom->entry_prototypes[P_NAVIGATION]->value;
    static const struct { const char *name; int magic; bool navigation; } kResource[] = {
        {"initiatorType", R_INITIATOR, false}, {"nextHopProtocol", R_PROTOCOL, false},
        {"workerStart", R_ZERO, false}, {"redirectStart", R_TIMING + 3, false},
        {"redirectEnd", R_TIMING + 4, false}, {"fetchStart", R_TIMING + 5, false},
        {"domainLookupStart", R_TIMING + 6, false}, {"domainLookupEnd", R_TIMING + 7, false},
        {"connectStart", R_TIMING + 8, false}, {"connectEnd", R_TIMING + 9, false},
        {"secureConnectionStart", R_TIMING + 10, false}, {"requestStart", R_TIMING + 11, false},
        {"responseStart", R_TIMING + 12, false}, {"responseEnd", R_TIMING + 13, false},
        {"transferSize", R_ZERO, false}, {"encodedBodySize", R_ZERO, false},
        {"decodedBodySize", R_ZERO, false}, {"serverTiming", R_SERVER_TIMING, false},
        {"unloadEventStart", R_TIMING + 1, true}, {"unloadEventEnd", R_TIMING + 2, true},
        {"domInteractive", R_TIMING + 15, true}, {"domContentLoadedEventStart", R_TIMING + 16, true},
        {"domContentLoadedEventEnd", R_TIMING + 17, true}, {"domComplete", R_TIMING + 18, true},
        {"loadEventStart", R_TIMING + 19, true}, {"loadEventEnd", R_TIMING + 20, true},
        {"type", R_NAV_TYPE, true}, {"redirectCount", R_NAV_REDIRECTS, true},
    };
    for (size_t i = 0; i < sizeof(kResource) / sizeof(kResource[0]); i++)
        if (d_accessor(dom, ctx, kResource[i].navigation ? nav : resource, kResource[i].name, performance_member,
                       kResource[i].magic, false) < 0)
            return -1;
    if (d_method(dom, ctx, resource, "toJSON", performance_member, 0, E_TO_JSON) < 0 ||
        d_method(dom, ctx, nav, "toJSON", performance_member, 0, E_TO_JSON) < 0)
        return -1;

    /* PerformanceTiming and PerformanceNavigation. */
    for (size_t i = 0; i < TIMING_COUNT; i++)
        if (d_accessor(dom, ctx, timing->value, kTiming[i].name, performance_member, T_ATTRIBUTE + (int)i, false) < 0)
            return -1;
    if (d_method(dom, ctx, timing->value, "toJSON", performance_member, 0, N_TIMING_JSON) < 0 ||
        d_accessor(dom, ctx, navigation->value, "type", performance_member, N_TYPE, false) < 0 ||
        d_accessor(dom, ctx, navigation->value, "redirectCount", performance_member, N_REDIRECTS, false) < 0 ||
        d_method(dom, ctx, navigation->value, "toJSON", performance_member, 0, N_TO_JSON) < 0)
        return -1;
    static const char *const kNavigationConstants[] = {"TYPE_NAVIGATE", "TYPE_RELOAD", "TYPE_BACK_FORWARD"};

    /* The interface objects. PerformanceResourceTiming and
     * PerformanceNavigationTiming are the navigation entry's chain, on
     * window as in other browsers (a page asks the entry `instanceof
     * PerformanceNavigationTiming`); that no resource entry is recorded is
     * supportedEntryTypes' to say. */
    JSValue entry_interface = interface_of(ctx, global, "PerformanceEntry", 0, NULL, entry, JS_UNDEFINED);
    if (JS_IsException(entry_interface)) return -1;
    JSValue resource_interface = interface_of(ctx, global, "PerformanceResourceTiming", 0, NULL, resource,
                                              entry_interface);
    if (JS_IsException(resource_interface) ||
        JS_IsException(interface_of(ctx, global, "PerformanceMark", 1, mark_constructor,
                                    dom->entry_prototypes[P_MARK]->value, entry_interface)) ||
        JS_IsException(interface_of(ctx, global, "PerformanceMeasure", 0, NULL,
                                    dom->entry_prototypes[P_MEASURE]->value, entry_interface)) ||
        JS_IsException(interface_of(ctx, global, "PerformanceNavigationTiming", 0, NULL, nav,
                                    resource_interface)) ||
        JS_IsException(interface_of(ctx, global, "Performance", 0, NULL, on, JS_UNDEFINED)) ||
        JS_IsException(interface_of(ctx, global, "PerformanceTiming", 0, NULL, timing->value, JS_UNDEFINED)))
        return -1;
    JSValue navigation_interface = interface_of(ctx, global, "PerformanceNavigation", 0, NULL, navigation->value,
                                                JS_UNDEFINED);
    if (JS_IsException(navigation_interface)) return -1;
    for (int i = 0; i < 3; i++)
        if (JS_DefinePropertyValueStr(ctx, navigation_interface, kNavigationConstants[i], JS_NewInt32(ctx, i),
                                      JS_PROP_ENUMERABLE) < 0 ||
            JS_DefinePropertyValueStr(ctx, navigation->value, kNavigationConstants[i], JS_NewInt32(ctx, i),
                                      JS_PROP_ENUMERABLE) < 0)
            return -1;

    /* The objects: performance (a plain event target, whose opaque is its
     * registry entry), its buffer, timing, navigation, and the navigation
     * entry, named for the page's address. */
    JSValue object = JS_NewObjectClass(ctx, (int)performance_id);
    dom->performance = d_retain(dom, ctx, object);
    if (dom->performance == NULL) return -1;
    JS_SetOpaque(object, dom->performance);
    dom->performance_entries = d_retain(dom, ctx, buffer_new(ctx));
    if (dom->performance_entries == NULL) return -1;
    JSValue timing_object = JS_NewObjectClass(ctx, (int)timing_id);
    dom->performance_timing = d_retain(dom, ctx, timing_object);
    if (dom->performance_timing == NULL) return -1;
    JS_SetOpaque(timing_object, dom->performance_timing);
    JSValue navigation_object = JS_NewObjectClass(ctx, (int)navigation_id);
    dom->performance_navigation = d_retain(dom, ctx, navigation_object);
    if (dom->performance_navigation == NULL) return -1;
    JS_SetOpaque(navigation_object, dom->performance_navigation);
    JSValue address = JS_NewString(ctx, dom->url != NULL ? dom->url : "about:blank");
    if (JS_IsException(address)) return -1;
    JSValue navigation_entry = entry_new(ctx, nav, K_NAVIGATION, address, 0, 0, JS_NULL);
    JS_FreeValue(ctx, address);
    dom->navigation_entry = d_retain(dom, ctx, navigation_entry);
    if (dom->navigation_entry == NULL) return -1;
    return d_accessor(dom, ctx, global, "performance", performance_member, M_WINDOW, true);
}

/* ── PerformanceObserver ─────────────────────────────────────────────── */

/* new PerformanceObserver(callback). */
static JSValue observer_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "A PerformanceObserver needs a callback function");
    uint32_t script = d_callback_script(dom, ctx, argv[0]);
    if (script == D_SCRIPT_FAILED) return JS_EXCEPTION;
    JSClassID id = os64_js_class_id(&observer_slot);
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    DObserver *o = JS_IsException(object) ? NULL : js_mallocz(ctx, sizeof(*o));
    if (o == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    o->anchor = JS_DupValue(ctx, dom->anchor->value);
    o->callback = JS_DupValue(ctx, argv[0]);
    o->script = script;
    JS_SetOpaque(object, o);
    return object;
}

/* `self` on the registered list, at its end unless it is there already;
 * -1 with an exception pending. */
static int observer_register(os64_dom_t *dom, JSContext *ctx, JSValueConst self)
{
    uint32_t count = dom->performance_observer_count;
    for (uint32_t i = 0; i < count; i++)
        if (JS_VALUE_GET_PTR(dom->performance_observers[i]) == JS_VALUE_GET_PTR(self)) return 0;
    if (count == dom->performance_observer_cap) {
        uint32_t cap = count < 8 ? 8 : count < UINT32_MAX / 2 ? count * 2 : 0;
        JSValue *grown = cap != 0 ? d_alloc(dom, (size_t)cap * sizeof(*grown)) : NULL;
        if (grown == NULL) {
            d_error(ctx, "QuotaExceededError", "Performance observer quota exceeded");
            return -1;
        }
        if (count != 0) os64_memcpy(grown, dom->performance_observers, (size_t)count * sizeof(*grown));
        d_free(dom, dom->performance_observers);
        dom->performance_observers = grown;
        dom->performance_observer_cap = cap;
    }
    dom->performance_observers[dom->performance_observer_count++] = JS_DupValue(ctx, self);
    return 0;
}

static void observer_unregister(os64_dom_t *dom, JSContext *ctx, JSValueConst self)
{
    uint32_t count = dom->performance_observer_count;
    for (uint32_t i = 0; i < count; i++) {
        if (JS_VALUE_GET_PTR(dom->performance_observers[i]) != JS_VALUE_GET_PTR(self)) continue;
        JSValue gone = dom->performance_observers[i];
        os64_memmove(dom->performance_observers + i, dom->performance_observers + i + 1,
                     (size_t)(count - i - 1) * sizeof(*dom->performance_observers));
        dom->performance_observer_count--;
        JS_FreeValue(ctx, gone);
        return;
    }
}

/* An entryTypes member's string, its kind added to the set `opaque`
 * points at; a type this file records none of is left out. */
static int entry_type_each(void *opaque, JSContext *ctx, JSValueConst item)
{
    JSValue text = JS_ToString(ctx, item);
    if (JS_IsException(text)) return -1;
    int kind = kind_named(ctx, text);
    JS_FreeValue(ctx, text);
    if (kind == -2) return -1;
    if (kind >= 0) *(uint8_t *)opaque |= (uint8_t)(1u << kind);
    return 0;
}

/* observe(options), the standard's steps. The members are read and
 * converted in their names' order. A type this file does not record is
 * left out of `entryTypes`, and an observe() left with no type ends
 * without a word, as the standard has a browser that does not support one
 * do (it SHOULD warn; the console is the page's). -1 with an exception
 * pending. */
static int observe(os64_dom_t *dom, JSContext *ctx, JSValueConst self, DObserver *o, int argc, JSValueConst *argv)
{
    int options = d_dictionary(ctx, argc, argv, 0);
    if (options < 0) return -1;
    bool buffered = false, has_list = false, has_type = false;
    uint8_t list = 0;
    int type = -1;
    if (options > 0) {
        JSValue value = JS_GetPropertyStr(ctx, argv[0], "buffered");
        if (JS_IsException(value)) return -1;
        buffered = JS_ToBool(ctx, value) > 0;
        JS_FreeValue(ctx, value);
        value = JS_GetPropertyStr(ctx, argv[0], "entryTypes");
        if (JS_IsException(value)) return -1;
        has_list = !JS_IsUndefined(value);
        int done = has_list ? d_sequence_each(dom, ctx, value, JS_UNDEFINED, entry_type_each, &list) : 0;
        JS_FreeValue(ctx, value);
        if (done < 0) return -1;
        value = JS_GetPropertyStr(ctx, argv[0], "type");
        if (JS_IsException(value)) return -1;
        has_type = !JS_IsUndefined(value);
        JSValue text = has_type ? JS_ToString(ctx, value) : JS_UNDEFINED;
        JS_FreeValue(ctx, value);
        if (JS_IsException(text)) return -1;
        if (has_type) type = kind_named(ctx, text);
        JS_FreeValue(ctx, text);
        if (type == -2) return -1;
    }
    if (!has_list && !has_type) {
        JS_ThrowTypeError(ctx, "observe() needs entryTypes or a type");
        return -1;
    }
    /* The standard refuses `entryTypes` beside any other member. Beside
     * `buffered` other browsers do not: they warn, ignore `buffered` and
     * observe the list (WPT's tentative buffered-flag-with-entryTypes),
     * and a page written against them would lose the rest of its script to
     * the throw. So `type` alone is refused there, and `buffered` is never
     * read by the `entryTypes` steps below. */
    if (has_list && has_type) {
        JS_ThrowTypeError(ctx, "observe() may not be given entryTypes beside a type");
        return -1;
    }
    if (o->mode == O_UNSET) o->mode = has_list ? O_MULTIPLE : O_SINGLE;
    if ((o->mode == O_SINGLE && has_list) || (o->mode == O_MULTIPLE && has_type)) {
        d_error(ctx, "InvalidModificationError", o->mode == O_SINGLE
                ? "This observer observes by type, and cannot be given entryTypes"
                : "This observer observes entryTypes, and cannot be given a type");
        return -1;
    }
    o->dropped_owed = true;
    if (o->mode == O_MULTIPLE) {
        if (list == 0) return 0;
        if (observer_register(dom, ctx, self) < 0) return -1;
        o->kinds = list;
        return 0;
    }
    if (type < 0) return 0;
    /* With `buffered`, the type's buffer, in the order buffered: for
     * navigation, its one entry once that has been queued
     * (d_performance_loaded), and nothing before. Counted and room made
     * before the observer is registered, so one that cannot hold them is
     * refused whole. Reading the buffer runs no page code. */
    uint32_t found = 0;
    if (buffered && type == K_NAVIGATION) found = dom->navigation_queued ? 1 : 0;
    for (uint32_t i = 0; buffered && type != K_NAVIGATION && i < dom->performance_count; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, dom->performance_entries->value, i);
        if (JS_IsException(item)) return -1;
        const DEntry *entry = JS_GetOpaque(item, os64_js_class_id(&entry_slot));
        if (entry != NULL && entry->kind == type) found++;
        JS_FreeValue(ctx, item);
    }
    if (found != 0 && observer_room(ctx, o, found) < 0) return -1;
    if (observer_register(dom, ctx, self) < 0) return -1;
    o->kinds |= (uint8_t)(1u << type);
    if (found == 0) return 0;
    if (type == K_NAVIGATION) o->entries[o->count++] = JS_DupValue(ctx, dom->navigation_entry->value);
    for (uint32_t i = 0; type != K_NAVIGATION && i < dom->performance_count; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, dom->performance_entries->value, i);
        if (JS_IsException(item)) return -1;
        const DEntry *entry = JS_GetOpaque(item, os64_js_class_id(&entry_slot));
        if (entry != NULL && entry->kind == type) o->entries[o->count++] = item;
        else JS_FreeValue(ctx, item);
    }
    return queue_task(dom, ctx);
}

/* HTML's "the end" queues the navigation timing entry once the window's
 * load event has run, and os64_dom_dispatch says so here when that event
 * has been dispatched; a binding made for a page already complete counts it
 * queued from the start. Each registered observer of `navigation` is handed
 * the entry, in a host task this opens to queue the delivery, and from
 * here a buffered observe() is handed it too. The entry reads the host's
 * milestones when asked, so a callback reads loadEventEnd as the host
 * marked it after the dispatch. */
void d_performance_loaded(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    if (dom->navigation_queued || dom->navigation_entry == NULL) return;
    dom->navigation_queued = true;
    bool wanted = false;
    for (uint32_t i = 0; i < dom->performance_observer_count; i++) {
        const DObserver *o = observer_at(dom, i);
        if (o != NULL && (o->kinds & (1u << K_NAVIGATION)) != 0) wanted = true;
    }
    if (!wanted) return;
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, "navigation timing", outcome) != OS64_JS_OK) return;
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
    if (ctx != NULL && (queue_room(dom, ctx, K_NAVIGATION) < 0 ||
                        queue_entry(dom, ctx, dom->navigation_entry->value, K_NAVIGATION) < 0))
        report_pending(dom, ctx, D_SCRIPT_PAGE);
    d_task_finish(dom, outcome);
}

enum { O_OBSERVE, O_DISCONNECT, O_TAKE_RECORDS, O_SUPPORTED, L_ENTRIES, L_BY_TYPE, L_BY_NAME };

static JSValue observer_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    magic &= ~D_SET;
    /* A static attribute, read on the interface object. */
    if (magic == O_SUPPORTED) return JS_DupValue(ctx, dom->supported_entry_types->value);
    if (magic >= L_ENTRIES) {
        DEntryList *l = JS_GetOpaque(self, os64_js_class_id(&list_slot));
        if (l == NULL) return JS_ThrowTypeError(ctx, "Expected a PerformanceObserverEntryList");
        if (magic == L_ENTRIES) return entries_query(dom, ctx, -1, JS_UNDEFINED, l->entries, l->count);
        return query_by(dom, ctx, magic == L_BY_NAME, argc, argv, l->entries, l->count);
    }
    DObserver *o = JS_GetOpaque(self, os64_js_class_id(&observer_slot));
    if (o == NULL) return JS_ThrowTypeError(ctx, "Expected a PerformanceObserver");
    dom = d_callback(ctx, &o->anchor, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == O_OBSERVE) return observe(dom, ctx, self, o, argc, argv) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    /* takeRecords() answers the buffer in the order queued; disconnect()
     * empties it, and forgets the types observed. */
    JSValue records = magic == O_TAKE_RECORDS ? JS_NewArray(ctx) : JS_UNDEFINED;
    if (JS_IsException(records)) return records;
    for (uint32_t i = 0; magic == O_TAKE_RECORDS && i < o->count; i++)
        if (JS_DefinePropertyValueUint32(ctx, records, i, JS_DupValue(ctx, o->entries[i]), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, records);
            return JS_EXCEPTION;
        }
    if (magic == O_DISCONNECT) {
        observer_unregister(dom, ctx, self);
        o->kinds = 0;
    }
    values_free(dom->engine, o->entries, o->count);
    o->entries = NULL;
    o->count = o->cap = 0;
    return records;
}

/* supportedEntryTypes' FrozenArray: the types this file records, in the
 * alphabetical order the standard keeps them in (kKindNames' own), made
 * once, so every read is the same object. Frozen by definition, as
 * serverTiming's. */
static int supported_install(os64_dom_t *dom, JSContext *ctx)
{
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return -1;
    JSAtom length = JS_NewAtom(ctx, "length");
    int result = length == JS_ATOM_NULL ? -1 : 0;
    for (uint32_t k = 0; result == 0 && k < 3; k++)
        if (JS_DefinePropertyValueUint32(ctx, array, k, JS_NewString(ctx, kKindNames[k]), JS_PROP_ENUMERABLE) < 0)
            result = -1;
    if (result == 0 &&
        (JS_DefineProperty(ctx, array, length, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_PROP_HAS_WRITABLE) < 0 ||
         JS_PreventExtensions(ctx, array) < 0))
        result = -1;
    if (length != JS_ATOM_NULL) JS_FreeAtom(ctx, length);
    if (result < 0) {
        JS_FreeValue(ctx, array);
        return -1;
    }
    dom->supported_entry_types = d_retain(dom, ctx, array);
    return dom->supported_entry_types != NULL ? 0 : -1;
}

int d_performance_observers_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef observer_class = {.class_name = "PerformanceObserver", .finalizer = observer_finalizer,
                                              .gc_mark = observer_mark};
    static const JSClassDef list_class = {.class_name = "PerformanceObserverEntryList", .finalizer = list_finalizer,
                                          .gc_mark = list_mark};
    JSClassID observer_id = os64_js_class_id(&observer_slot), list_id = os64_js_class_id(&list_slot);
    if (class_of(dom, observer_id, &observer_class) < 0 || class_of(dom, list_id, &list_class) < 0) return -1;
    DValue *observer = prototype_new(dom, ctx, JS_UNDEFINED);
    DValue *list = observer != NULL ? prototype_new(dom, ctx, JS_UNDEFINED) : NULL;
    if (list == NULL) return -1;
    JS_SetClassProto(ctx, observer_id, JS_DupValue(ctx, observer->value));
    JS_SetClassProto(ctx, list_id, JS_DupValue(ctx, list->value));
    static const struct { const char *name; int magic, argc; bool on_list; } kMethods[] = {
        {"observe", O_OBSERVE, 0, false}, {"disconnect", O_DISCONNECT, 0, false},
        {"takeRecords", O_TAKE_RECORDS, 0, false}, {"getEntries", L_ENTRIES, 0, true},
        {"getEntriesByType", L_BY_TYPE, 1, true}, {"getEntriesByName", L_BY_NAME, 1, true},
    };
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method(dom, ctx, kMethods[i].on_list ? list->value : observer->value, kMethods[i].name,
                     observer_member, kMethods[i].argc, kMethods[i].magic) < 0)
            return -1;
    if (supported_install(dom, ctx) < 0) return -1;
    dom->navigation_queued = dom->readiness == OS64_DOM_COMPLETE;
    JSValue observer_interface = interface_of(ctx, global, "PerformanceObserver", 1, observer_constructor,
                                              observer->value, JS_UNDEFINED);
    if (JS_IsException(observer_interface) ||
        d_accessor(dom, ctx, observer_interface, "supportedEntryTypes", observer_member, O_SUPPORTED, false) < 0)
        return -1;
    return JS_IsException(interface_of(ctx, global, "PerformanceObserverEntryList", 0, NULL, list->value,
                                       JS_UNDEFINED)) ? -1 : 0;
}

/* Before the engine goes: the registered observers, which the list holds. */
void d_performance_drain(os64_dom_t *dom)
{
    for (uint32_t i = 0; i < dom->performance_observer_count; i++)
        JS_FreeValueRT(dom->engine, dom->performance_observers[i]);
    dom->performance_observer_count = 0;
}

/* After it: the list itself. */
void d_performance_free(os64_dom_t *dom)
{
    d_free(dom, dom->performance_observers);
    dom->performance_observers = NULL;
    dom->performance_observer_cap = 0;
}
