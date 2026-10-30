#include "internal.h"
#include "os64/fmt.h"

/* MUTATIONOBSERVER (DOM § 4.3; CUSTOM_ELEMENTS.md, the third cut; RELAYOUT.md
 * § "MutationObserver reads the log", built on the document's listener).
 *
 * The listener (os64_html_observe, custom.c's `changed`) hands every change
 * here as it happens, inside the verb, and this runs the standard's "queue a
 * mutation record": it walks the target's inclusive ancestors as the tree
 * stands at that instant, finds each registration whose options match, and
 * copies the record into each interested observer's queue, the nodes it
 * names held. Nothing here enters the engine there. The records become
 * MutationRecord objects when they are handed over: at the "notify mutation
 * observers" microtask, or by takeRecords(). Between tasks (the parser's
 * insertions) no checkpoint is coming, so the notify is OWED, and the host
 * runs it with os64_dom_reactions_run, as custom element reactions are.
 *
 * An observer stays alive while it observes something: its first
 * registration holds its object strongly (a list of the registry's, so the
 * drain lets go of it), and its last let go releases it. Its C state lives
 * for the page and is freed at teardown; the object's finalizer only
 * unlinks it. */

enum { T_ATTRIBUTES, T_CHARACTER_DATA, T_CHILD_LIST };
static const char *const kTypes[] = {"attributes", "characterData", "childList"};

/* MutationObserverInit, as observe() read it. */
enum {
    O_CHILD_LIST = 1, O_ATTRIBUTES = 2, O_CHARACTER_DATA = 4, O_SUBTREE = 8, O_ATTRIBUTE_OLD = 16,
    O_CHARACTER_DATA_OLD = 32, O_FILTER = 64
};

typedef struct DMutation {
    struct DMutation *next;
    uint8_t type;
    /* All held: the target, the siblings, and every added and removed node. */
    const os64_html_node_t *target, *previous, *following;
    const os64_html_node_t **added, **removed;
    size_t nadded, nremoved;
    /* attributes: the local name and namespace; the old value when the
     * observer asked for it. NULL for null. */
    char *name, *ns, *old;
} DMutation;

typedef struct DObserver {
    struct DObserver *next;
    os64_dom_t *dom;
    /* The object, while it lives (its finalizer clears it), and the strong
     * reference to it a registration keeps (`held`). */
    JSValue object;
    bool held;
    JSValue callback;
    uint32_t script;
    DMutation *first, *last;
    size_t registrations;
} DObserver;

typedef struct DRegistration {
    struct DRegistration *next;
    const os64_html_node_t *node;
    DObserver *observer;
    /* A transient registration's source (DOM § 4.3: one added to a removed
     * node for an ancestor's subtree observer), else NULL. */
    struct DRegistration *source;
    uint8_t options;
    char **filter;
    size_t nfilter;
} DRegistration;

#define D_OBSERVER_BUCKETS 64

struct DObservers {
    JSClassID observer_class, record_class;
    /* Every observer the page made, in the order it made them: the
     * standard's "mutation observers", which notify walks. */
    DObserver *observers, *observers_last;
    DRegistration *registrations[D_OBSERVER_BUCKETS];
    size_t count;
    /* The notify microtask: queued, or owed when no turn will drain it. */
    bool queued, owed;
    /* Records the listener could not keep for want of memory, reported at
     * the next notify. */
    uint64_t lost;
    DValue *record_prototype;
};

static JSClassID observer_slot, record_slot;

static size_t node_bucket(const os64_html_node_t *node)
{
    return ((uintptr_t)node >> 4) % D_OBSERVER_BUCKETS;
}

/* ── Records ─────────────────────────────────────────────────────────── */

static void mutation_free(os64_dom_t *dom, DMutation *m)
{
    const os64_html_node_t *nodes[] = {m->target, m->previous, m->following};
    for (size_t i = 0; i < 3; i++)
        if (nodes[i] != NULL) os64_html_release(dom->document, nodes[i]);
    for (size_t i = 0; i < m->nadded; i++) os64_html_release(dom->document, m->added[i]);
    for (size_t i = 0; i < m->nremoved; i++) os64_html_release(dom->document, m->removed[i]);
    d_free(dom, m);
}

static void queue_clear(os64_dom_t *dom, DObserver *o)
{
    while (o->first != NULL) {
        DMutation *next = o->first->next;
        mutation_free(dom, o->first);
        o->first = next;
    }
    o->last = NULL;
}

static size_t text_size(const char *s, size_t n)
{
    return s != NULL ? n + 1 : 0;
}

static char *text_put(char **at, const char *s, size_t n)
{
    if (s == NULL) return NULL;
    char *out = *at;
    os64_memcpy(out, s, n);
    out[n] = '\0';
    *at += n + 1;
    return out;
}

/* A record for one observer, in one block: the node arrays and the
 * strings after the struct. NULL for want of memory. */
static DMutation *mutation_new(os64_dom_t *dom, const os64_html_change_t *c, int type, bool with_old)
{
    const char *old = type == T_ATTRIBUTES ? c->attr_old : type == T_CHARACTER_DATA ? c->text_old : NULL;
    size_t old_len = type == T_ATTRIBUTES ? c->attr_old_len : c->text_old_len;
    if (!with_old) old = NULL;
    const char *name = type == T_ATTRIBUTES ? c->attr_local : NULL, *ns = type == T_ATTRIBUTES ? c->attr_ns : NULL;
    size_t nodes = type == T_CHILD_LIST ? c->nadded + c->nremoved : 0;
    size_t size = sizeof(DMutation) + nodes * sizeof(const os64_html_node_t *) +
        text_size(name, name != NULL ? os64_strlen(name) : 0) + text_size(ns, ns != NULL ? os64_strlen(ns) : 0) +
        text_size(old, old_len);
    DMutation *m = d_alloc(dom, size);
    if (m == NULL) return NULL;
    os64_memset(m, 0, sizeof(*m));
    m->type = (uint8_t)type;
    m->target = c->target;
    const os64_html_node_t **list = (const os64_html_node_t **)(m + 1);
    if (type == T_CHILD_LIST) {
        m->previous = c->previous_sibling;
        m->following = c->next_sibling;
        m->added = list;
        m->removed = list + c->nadded;
        const os64_html_node_t *n = c->added;
        for (size_t i = 0; i < c->nadded && n != NULL; i++, n = n->next) m->added[m->nadded++] = n;
        n = c->removed;
        for (size_t i = 0; i < c->nremoved && n != NULL; i++, n = n->next) m->removed[m->nremoved++] = n;
    }
    char *at = (char *)(list + nodes);
    m->name = text_put(&at, name, name != NULL ? os64_strlen(name) : 0);
    m->ns = text_put(&at, ns, ns != NULL ? os64_strlen(ns) : 0);
    m->old = text_put(&at, old, old_len);
    const os64_html_node_t *held[] = {m->target, m->previous, m->following};
    for (size_t i = 0; i < 3; i++)
        if (held[i] != NULL) os64_html_hold(dom->document, held[i]);
    for (size_t i = 0; i < m->nadded; i++) os64_html_hold(dom->document, m->added[i]);
    for (size_t i = 0; i < m->nremoved; i++) os64_html_hold(dom->document, m->removed[i]);
    return m;
}

/* ── Queueing (the listener's half) ──────────────────────────────────── */

static JSValue notify_job(JSContext *ctx, int argc, JSValueConst *argv);

/* DOM's "queue a mutation observer microtask": one at a time; owed to the
 * host when no turn will drain it. */
static void request_notify(os64_dom_t *dom)
{
    struct DObservers *s = dom->observers;
    if (s->queued && !(s->owed && os64_js_turn_open(dom->runtime))) return;
    s->queued = true;
    if (os64_js_turn_open(dom->runtime)) {
        os64_js_outcome_t outcome;
        JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &outcome);
        if (ctx != NULL && JS_EnqueueJob(ctx, notify_job, 0, NULL) == 0) {
            s->owed = false;
            return;
        }
        if (ctx != NULL && JS_HasException(ctx) && d_exception_catchable(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    s->owed = true;
}

static bool filter_has(const DRegistration *r, const char *name)
{
    for (size_t i = 0; i < r->nfilter; i++)
        if (os64_streq(r->filter[i], name)) return true;
    return false;
}

/* Whether a registration on `node` wants a record of this change (DOM's
 * "queue a mutation record", step 4). */
static bool interested(const DRegistration *r, const os64_html_node_t *node, const os64_html_change_t *c, int type)
{
    if (node != c->target && !(r->options & O_SUBTREE)) return false;
    if (type == T_ATTRIBUTES) {
        if (!(r->options & O_ATTRIBUTES)) return false;
        if ((r->options & O_FILTER) && (c->attr_ns != NULL || !filter_has(r, c->attr_local))) return false;
        return true;
    }
    if (type == T_CHARACTER_DATA) return (r->options & O_CHARACTER_DATA) != 0;
    return (r->options & O_CHILD_LIST) != 0;
}

/* A removed node is given a transient registration for each subtree
 * registration of the parent's inclusive ancestors, so a change inside it
 * still reaches them until they are next notified. True when it gave any:
 * the notify that ends them is then queued, record or no record (WPT's
 * transient-registered-observers). */
static bool add_transients(os64_dom_t *dom, const os64_html_change_t *c)
{
    struct DObservers *s = dom->observers;
    bool added = false;
    for (const os64_html_node_t *at = c->target; at != NULL; at = at->parent)
        for (DRegistration *r = s->registrations[node_bucket(at)]; r != NULL; r = r->next) {
            if (r->node != at || !(r->options & O_SUBTREE)) continue;
            const os64_html_node_t *n = c->removed;
            for (size_t i = 0; i < c->nremoved && n != NULL; i++, n = n->next) {
                DRegistration *t = d_alloc(dom, sizeof(*t));
                if (t == NULL) {
                    s->lost++;
                    continue;
                }
                /* A transient's source is the registration the chain began
                 * with, so observe() re-registering it drops a subtree
                 * removed from a removed subtree as well (WPT's
                 * transient-registered-observers). */
                *t = (DRegistration){.node = n, .observer = r->observer, .source = r->source != NULL ? r->source : r,
                                     .options = r->options, .filter = r->filter, .nfilter = r->nfilter};
                os64_html_hold(dom->document, n);
                size_t b = node_bucket(n);
                t->next = s->registrations[b];
                s->registrations[b] = t;
                r->observer->registrations++;
                s->count++;
                added = true;
            }
        }
    return added;
}

void d_observers_changed(os64_dom_t *dom, const os64_html_change_t *c)
{
    struct DObservers *s = dom->observers;
    if (s == NULL || s->count == 0) return;
    int type = c->kind == OS64_HTML_CHANGE_ATTR ? T_ATTRIBUTES : c->kind == OS64_HTML_CHANGE_TEXT
        ? T_CHARACTER_DATA : c->kind == OS64_HTML_CHANGE_CHILDREN ? T_CHILD_LIST : -1;
    if (type < 0) return;
    /* The interested observers, each once, and whether any registration of
     * it asked for the old value. */
    DObserver *found[32];
    bool with_old[32];
    size_t n = 0;
    for (const os64_html_node_t *at = c->target; at != NULL; at = at->parent)
        for (DRegistration *r = s->registrations[node_bucket(at)]; r != NULL; r = r->next) {
            if (r->node != at || !interested(r, at, c, type)) continue;
            size_t i = 0;
            while (i < n && found[i] != r->observer) i++;
            if (i == n) {
                if (n == sizeof(found) / sizeof(found[0])) {
                    s->lost++;
                    continue;
                }
                found[n] = r->observer;
                with_old[n++] = false;
            }
            if ((type == T_ATTRIBUTES && (r->options & O_ATTRIBUTE_OLD)) ||
                (type == T_CHARACTER_DATA && (r->options & O_CHARACTER_DATA_OLD)))
                with_old[i] = true;
        }
    for (size_t i = 0; i < n; i++) {
        DMutation *m = mutation_new(dom, c, type, with_old[i]);
        if (m == NULL) {
            s->lost++;
            continue;
        }
        DObserver *o = found[i];
        if (o->last != NULL) o->last->next = m;
        else o->first = m;
        o->last = m;
    }
    if (n != 0) request_notify(dom);
    if (type == T_CHILD_LIST && c->nremoved != 0 && add_transients(dom, c)) request_notify(dom);
}

/* ── Registrations ───────────────────────────────────────────────────── */

/* Lets go of a registration that is unlinked already. */
static void registration_release(os64_dom_t *dom, DRegistration *r)
{
    struct DObservers *s = dom->observers;
    DObserver *o = r->observer;
    os64_html_release(dom->document, r->node);
    if (r->source == NULL) d_strings_free(dom, r->filter, r->nfilter);
    d_free(dom, r);
    s->count--;
    if (--o->registrations == 0 && o->held) {
        /* Observing nothing: the object may go with the garbage. */
        o->held = false;
        dom->observers_held--;
        JS_FreeValueRT(dom->engine, o->object);
    }
}

/* Every registration `drop` answers true for, unlinked and let go of. */
static void registrations_drop(os64_dom_t *dom, bool (*drop)(const DRegistration *, const void *), const void *arg)
{
    struct DObservers *s = dom->observers;
    for (size_t b = 0; b < D_OBSERVER_BUCKETS; b++)
        for (DRegistration **link = &s->registrations[b]; *link != NULL;) {
            DRegistration *r = *link;
            if (!drop(r, arg)) {
                link = &r->next;
                continue;
            }
            *link = r->next;
            registration_release(dom, r);
        }
}

static bool transient_of(const DRegistration *r, const void *source)
{
    return r->source == source;
}

static bool transient_for(const DRegistration *r, const void *observer)
{
    return r->source != NULL && r->observer == observer;
}

static bool any_for(const DRegistration *r, const void *observer)
{
    return r->observer == observer;
}

/* ── Delivery ────────────────────────────────────────────────────────── */

typedef struct {
    JSValue fields[9];
} DRecordObject;
enum { R_TYPE, R_TARGET, R_ADDED, R_REMOVED, R_PREVIOUS, R_NEXT, R_NAME, R_NAMESPACE, R_OLD };

static void record_finalizer(JSRuntime *rt, JSValue value)
{
    DRecordObject *r = JS_GetOpaque(value, os64_js_class_id(&record_slot));
    if (r == NULL) return;
    for (int i = 0; i < 9; i++) JS_FreeValueRT(rt, r->fields[i]);
    js_free_rt(rt, r);
}

static void record_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DRecordObject *r = JS_GetOpaque(value, os64_js_class_id(&record_slot));
    if (r == NULL) return;
    for (int i = 0; i < 9; i++) JS_MarkValue(rt, r->fields[i], mark);
}

static JSValue node_or_null(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    return node != NULL ? d_wrap(dom, ctx, node) : JS_NULL;
}

/* A static NodeList, as querySelectorAll answers one. */
static JSValue node_list(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *const *nodes, size_t count)
{
    JSValue list = JS_NewObjectProto(ctx, dom->static_list->value);
    for (size_t i = 0; i < count && !JS_IsException(list); i++) {
        JSValue node = d_wrap(dom, ctx, nodes[i]);
        if (JS_IsException(node) || JS_DefinePropertyValueUint32(ctx, list, (uint32_t)i, node, JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(ctx, list);
            list = JS_EXCEPTION;
        }
    }
    if (!JS_IsException(list) && JS_DefinePropertyValueStr(ctx, list, "length", JS_NewUint32(ctx, (uint32_t)count), 0) < 0) {
        JS_FreeValue(ctx, list);
        list = JS_EXCEPTION;
    }
    return list;
}

static JSValue string_or_null(JSContext *ctx, const char *s)
{
    return s != NULL ? JS_NewString(ctx, s) : JS_NULL;
}

static JSValue record_object(os64_dom_t *dom, JSContext *ctx, const DMutation *m)
{
    JSValue object = JS_NewObjectProtoClass(ctx, dom->observers->record_prototype->value,
                                            dom->observers->record_class);
    if (JS_IsException(object)) return object;
    DRecordObject *r = js_mallocz(ctx, sizeof(*r));
    if (r == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    for (int i = 0; i < 9; i++) r->fields[i] = JS_NULL;
    JS_SetOpaque(object, r);
    static const os64_html_node_t *const none[1] = {NULL};
    JSValue values[9] = {
        JS_NewString(ctx, kTypes[m->type]), node_or_null(dom, ctx, m->target),
        node_list(dom, ctx, m->nadded ? m->added : none, m->nadded),
        node_list(dom, ctx, m->nremoved ? m->removed : none, m->nremoved),
        node_or_null(dom, ctx, m->previous), node_or_null(dom, ctx, m->following), string_or_null(ctx, m->name),
        string_or_null(ctx, m->ns), string_or_null(ctx, m->old),
    };
    bool made = true;
    for (int i = 0; i < 9; i++) {
        made = made && !JS_IsException(values[i]);
        r->fields[i] = JS_IsException(values[i]) ? JS_NULL : values[i];
    }
    if (!made) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

/* An observer's queue as an array of MutationRecords, and the queue
 * emptied, whatever happens. */
static JSValue take_records(os64_dom_t *dom, JSContext *ctx, DObserver *o)
{
    JSValue array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (DMutation *m = o->first; m != NULL && !JS_IsException(array); m = m->next) {
        JSValue record = record_object(dom, ctx, m);
        if (JS_IsException(record) || JS_DefinePropertyValueUint32(ctx, array, i++, record, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, array);
            array = JS_EXCEPTION;
        }
    }
    queue_clear(dom, o);
    return array;
}

/* Records lost for want of memory are said once, as the page's failure. */
static void report_lost(os64_dom_t *dom, JSContext *ctx)
{
    struct DObservers *s = dom->observers;
    if (s->lost == 0) return;
    char text[96];
    os64_snprintf(text, sizeof(text), "%llu mutation records were lost: the DOM quota was exceeded",
                  (unsigned long long)s->lost);
    s->lost = 0;
    d_error(ctx, "QuotaExceededError", text);
    if (d_exception_catchable(ctx)) {
        JSValue error = JS_GetException(ctx);
        d_report(dom, ctx, error);
        JS_FreeValue(ctx, error);
    }
}

/* DOM's "notify mutation observers": every observer, in the order the page
 * made them (those made by a callback meanwhile wait for the next notify),
 * handed its records if it has any; its transient registrations go either
 * way.
 * -1 when the runtime must stop. */
static int notify(os64_dom_t *dom, JSContext *ctx)
{
    struct DObservers *s = dom->observers;
    s->queued = false;
    s->owed = false;
    report_lost(dom, ctx);
    DObserver *last = s->observers_last;
    int status = 0;
    for (DObserver *o = s->observers; o != NULL && status == 0;) {
        DObserver *next = o == last ? NULL : o->next;
        bool any = o->first != NULL;
        JSValue records = take_records(dom, ctx, o);
        registrations_drop(dom, transient_for, o);
        if (JS_IsException(records)) {
            if (!d_exception_catchable(ctx)) status = -1;
            else {
                JSValue error = JS_GetException(ctx);
                d_report(dom, ctx, error);
                JS_FreeValue(ctx, error);
            }
        } else if (any && JS_IsObject(o->object) && JS_IsFunction(ctx, o->callback)) {
            JSValue args[2] = {records, o->object};
            DScripts before = d_script_enter(dom, o->script);
            JSValue result = JS_Call(ctx, o->callback, o->object, 2, args);
            d_script_leave(dom, &before, false);
            if (JS_IsException(result)) {
                if (!d_exception_catchable(ctx)) status = -1;
                else {
                    JSValue error = JS_GetException(ctx);
                    d_report_as(dom, ctx, error, o->script);
                    JS_FreeValue(ctx, error);
                }
            }
            JS_FreeValue(ctx, result);
        }
        JS_FreeValue(ctx, records);
        o = next;
    }
    return status;
}

static JSValue notify_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL || dom->observers == NULL || !dom->observers->queued || dom->observers->owed)
        return JS_UNDEFINED;
    return notify(dom, ctx) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

bool d_observers_owed(const os64_dom_t *dom)
{
    return dom != NULL && dom->observers != NULL && dom->observers->owed;
}

int d_observers_notify_owed(os64_dom_t *dom, JSContext *ctx)
{
    return d_observers_owed(dom) ? notify(dom, ctx) : 0;
}

/* ── The interface ───────────────────────────────────────────────────── */

static DObserver *observer_of(os64_dom_t *dom, JSValueConst self)
{
    return JS_GetOpaque(self, dom->observers->observer_class);
}

static void observer_finalizer(JSRuntime *rt, JSValue value)
{
    DObserver *o = JS_GetOpaque(value, os64_js_class_id(&observer_slot));
    if (o == NULL) return;
    JS_FreeValueRT(rt, o->callback);
    o->callback = JS_UNDEFINED;
    o->object = JS_UNDEFINED;
}

static void observer_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DObserver *o = JS_GetOpaque(value, os64_js_class_id(&observer_slot));
    if (o != NULL) JS_MarkValue(rt, o->callback, mark);
}

static JSValue observer_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "MutationObserver needs a callback");
    struct DObservers *s = dom->observers;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    if (!JS_IsObject(prototype)) {
        JS_FreeValue(ctx, prototype);
        prototype = JS_GetClassProto(ctx, s->observer_class);
    }
    uint32_t script = d_callback_script(dom, ctx, argv[0]);
    DObserver *o = script != D_SCRIPT_FAILED ? d_alloc(dom, sizeof(*o)) : NULL;
    JSValue object = o != NULL ? JS_NewObjectProtoClass(ctx, prototype, s->observer_class) : JS_EXCEPTION;
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) {
        d_free(dom, o);
        if (!JS_HasException(ctx)) d_error(ctx, "QuotaExceededError", "MutationObserver quota exceeded");
        return JS_EXCEPTION;
    }
    os64_memset(o, 0, sizeof(*o));
    o->dom = dom;
    o->object = object;
    o->callback = JS_DupValue(ctx, argv[0]);
    o->script = script;
    if (s->observers_last != NULL) s->observers_last->next = o;
    else s->observers = o;
    s->observers_last = o;
    JS_SetOpaque(object, o);
    return object;
}

/* MutationObserverInit, its members read in their WebIDL order
 * (alphabetical), each only when present. 0 with the options, -1 with an
 * exception pending. */
static int read_options(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, uint8_t *options,
                        char ***filter, size_t *nfilter)
{
    *options = 0;
    *filter = NULL;
    *nfilter = 0;
    int present = d_dictionary(ctx, argc, argv, 1);
    if (present < 0) return -1;
    if (present == 0) return 0;
    static const struct { const char *name; uint8_t bit; } kMembers[] = {
        {"attributeFilter", O_FILTER}, {"attributeOldValue", O_ATTRIBUTE_OLD}, {"attributes", O_ATTRIBUTES},
        {"characterData", O_CHARACTER_DATA}, {"characterDataOldValue", O_CHARACTER_DATA_OLD},
        {"childList", O_CHILD_LIST}, {"subtree", O_SUBTREE},
    };
    /* Which optional members were given at all, beside their values. */
    uint8_t given = 0;
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++) {
        JSValue value = JS_GetPropertyStr(ctx, argv[1], kMembers[i].name);
        if (JS_IsException(value)) return -1;
        if (JS_IsUndefined(value)) continue;
        given |= kMembers[i].bit;
        int status;
        if (kMembers[i].bit == O_FILTER) {
            status = d_string_sequence(dom, ctx, value, filter, nfilter);
            if (status == 0) *options |= O_FILTER;
        } else {
            status = JS_ToBool(ctx, value);
            if (status > 0) *options |= kMembers[i].bit;
        }
        JS_FreeValue(ctx, value);
        if (status < 0) return -1;
    }
    /* DOM § 4.3.1, observe() steps 1 and 2: an old value or a filter asked
     * for implies the kind they belong to, when that was not given. */
    if (((given & O_ATTRIBUTE_OLD) || (given & O_FILTER)) && !(given & O_ATTRIBUTES)) *options |= O_ATTRIBUTES;
    if ((given & O_CHARACTER_DATA_OLD) && !(given & O_CHARACTER_DATA)) *options |= O_CHARACTER_DATA;
    return 0;
}

static JSValue observe(os64_dom_t *dom, JSContext *ctx, DObserver *o, int argc, JSValueConst *argv)
{
    struct DObservers *s = dom->observers;
    if (argc < 1) return JS_ThrowTypeError(ctx, "observe needs a node");
    const os64_html_node_t *target = d_node(dom, ctx, argv[0]);
    if (target == NULL) return JS_EXCEPTION;
    uint8_t options;
    char **filter;
    size_t nfilter;
    if (read_options(dom, ctx, argc, argv, &options, &filter, &nfilter) < 0) return JS_EXCEPTION;
    const char *wrong = !(options & (O_CHILD_LIST | O_ATTRIBUTES | O_CHARACTER_DATA))
        ? "observe needs childList, attributes or characterData"
        : (options & O_ATTRIBUTE_OLD) && !(options & O_ATTRIBUTES) ? "attributeOldValue needs attributes"
        : (options & O_FILTER) && !(options & O_ATTRIBUTES) ? "attributeFilter needs attributes"
        : (options & O_CHARACTER_DATA_OLD) && !(options & O_CHARACTER_DATA)
        ? "characterDataOldValue needs characterData" : NULL;
    if (wrong != NULL) {
        d_strings_free(dom, filter, nfilter);
        return JS_ThrowTypeError(ctx, "%s", wrong);
    }
    /* Observing a node again replaces the options, and its transient
     * registrations go. */
    for (DRegistration *r = s->registrations[node_bucket(target)]; r != NULL; r = r->next)
        if (r->node == target && r->observer == o && r->source == NULL) {
            registrations_drop(dom, transient_of, r);
            d_strings_free(dom, r->filter, r->nfilter);
            r->options = options;
            r->filter = filter;
            r->nfilter = nfilter;
            return JS_UNDEFINED;
        }
    DRegistration *r = d_alloc(dom, sizeof(*r));
    if (r == NULL) {
        d_strings_free(dom, filter, nfilter);
        return d_error(ctx, "QuotaExceededError", "MutationObserver quota exceeded");
    }
    *r = (DRegistration){.node = target, .observer = o, .options = options, .filter = filter, .nfilter = nfilter};
    os64_html_hold(dom->document, target);
    size_t b = node_bucket(target);
    /* Last on the node's list, as the standard appends. */
    DRegistration **link = &s->registrations[b];
    while (*link != NULL) link = &(*link)->next;
    *link = r;
    s->count++;
    if (o->registrations++ == 0 && !o->held) {
        o->held = true;
        dom->observers_held++;
        JS_DupValue(ctx, o->object);
    }
    return JS_UNDEFINED;
}

enum { M_OBSERVE, M_DISCONNECT, M_TAKE };

static JSValue observer_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DObserver *o = observer_of(dom, self);
    if (o == NULL) return JS_ThrowTypeError(ctx, "Expected a MutationObserver");
    switch (magic) {
    case M_OBSERVE: return observe(dom, ctx, o, argc, argv);
    case M_DISCONNECT:
        registrations_drop(dom, any_for, o);
        queue_clear(dom, o);
        return JS_UNDEFINED;
    case M_TAKE: return take_records(dom, ctx, o);
    default: return JS_ThrowTypeError(ctx, "Unknown MutationObserver member");
    }
}

static JSValue record_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DRecordObject *r = JS_GetOpaque(self, dom->observers->record_class);
    if (r == NULL) return JS_ThrowTypeError(ctx, "Expected a MutationRecord");
    return JS_DupValue(ctx, r->fields[magic]);
}

/* The MutationObserver row (modern.c): MutationObserver, which constructs,
 * and MutationRecord, an Illegal constructor. The document's listener is
 * the custom elements row's, which hands every change here first. */
int d_observers_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef observer_def = {.class_name = "MutationObserver", .finalizer = observer_finalizer,
                                            .gc_mark = observer_mark};
    static const JSClassDef record_def = {.class_name = "MutationRecord", .finalizer = record_finalizer,
                                          .gc_mark = record_mark};
    struct DObservers *s = d_alloc(dom, sizeof(*s));
    if (s == NULL) {
        d_error(ctx, "QuotaExceededError", "MutationObserver quota exceeded");
        return -1;
    }
    os64_memset(s, 0, sizeof(*s));
    dom->observers = s;
    s->observer_class = os64_js_class_id(&observer_slot);
    s->record_class = os64_js_class_id(&record_slot);
    if ((!JS_IsRegisteredClass(dom->engine, s->observer_class) &&
         JS_NewClass(dom->engine, s->observer_class, &observer_def) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, s->record_class) && JS_NewClass(dom->engine, s->record_class, &record_def) < 0))
        return -1;
    DValue *observer = d_retain(dom, ctx, JS_NewObject(ctx));
    s->record_prototype = observer != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (s->record_prototype == NULL) return -1;
    JS_SetClassProto(ctx, s->observer_class, JS_DupValue(ctx, observer->value));
    JS_SetClassProto(ctx, s->record_class, JS_DupValue(ctx, s->record_prototype->value));
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"observe", M_OBSERVE, 1}, {"disconnect", M_DISCONNECT, 0}, {"takeRecords", M_TAKE, 0},
    };
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method_named(dom, ctx, observer->value, kMethods[i].name, observer_member, kMethods[i].argc,
                           kMethods[i].magic) < 0)
            return -1;
    static const char *const kFields[] = {"type", "target", "addedNodes", "removedNodes", "previousSibling",
                                          "nextSibling", "attributeName", "attributeNamespace", "oldValue"};
    for (int i = 0; i < 9; i++)
        if (d_accessor(dom, ctx, s->record_prototype->value, kFields[i], record_member, i, false) < 0) return -1;
    JSValue observer_interface = d_interface_new(ctx, "MutationObserver", 1, observer_constructor, observer->value,
                                                 JS_UNDEFINED);
    if (JS_IsException(observer_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "MutationObserver", observer_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    JSValue record_interface = d_interface_new(ctx, "MutationRecord", 0, NULL, s->record_prototype->value,
                                               JS_UNDEFINED);
    if (JS_IsException(record_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "MutationRecord", record_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return 0;
}

/* Teardown: the objects registrations hold, let go with the engine's
 * values (os64_dom_drain); the records, registrations and observers'
 * memory after the engine. */
void d_observers_drain(os64_dom_t *dom)
{
    struct DObservers *s = dom->observers;
    if (s == NULL) return;
    for (DObserver *o = s->observers; o != NULL; o = o->next)
        if (o->held) {
            o->held = false;
            JS_FreeValueRT(dom->engine, o->object);
        }
    dom->observers_held = 0;
}

void d_observers_free(os64_dom_t *dom)
{
    struct DObservers *s = dom->observers;
    if (s == NULL) return;
    for (size_t b = 0; b < D_OBSERVER_BUCKETS; b++)
        while (s->registrations[b] != NULL) {
            DRegistration *r = s->registrations[b];
            s->registrations[b] = r->next;
            os64_html_release(dom->document, r->node);
            if (r->source == NULL) d_strings_free(dom, r->filter, r->nfilter);
            d_free(dom, r);
        }
    while (s->observers != NULL) {
        DObserver *o = s->observers;
        s->observers = o->next;
        queue_clear(dom, o);
        d_free(dom, o);
    }
    d_free(dom, s);
    dom->observers = NULL;
}
