#include "internal.h"
#include "os64/fmt.h"

/* CUSTOM ELEMENTS (docs/design/pending/CUSTOM_ELEMENTS.md; HTML § 4.13).
 *
 * The registry is `customElements`, one per window, and its definitions
 * live for the page's life. An element's state is on its libhtml node
 * (`ce`), so the cascade's `:defined` and yonder's census read it there.
 * What is waiting to happen to an element (an upgrade, a lifecycle
 * callback) is a DCustom record, which exists only while something waits
 * and holds the node while it does: an idle custom element costs nothing,
 * and no record can outlive its node and be found again by another node
 * reclaimed at the same address (the design's finding 6).
 *
 * THE DOCUMENT'S LISTENER (os64_html_observe) is where the tree's changes
 * become reactions: an insertion connects or tries to upgrade, a removal
 * disconnects, an observed attribute changes, a node changes documents.
 * It runs inside a libhtml verb, so it never enters the engine: it
 * appends to queues held here and returns. (define, a clone and upgrade()
 * queue their upgrades themselves.) JavaScript runs only where the
 * standard puts it: as a
 * member that pushed an element queue returns, at a microtask checkpoint
 * for the backup element queue, and in synchronous construction inside
 * createElement.
 *
 * Every [CEReactions] member the standard's IDL names is wrapped so that it
 * pushes a queue of its own ([CEReactions], below). A change no wrapper
 * reaches (an exotic setter, dataset's) still works: with the reaction
 * stack empty its reactions go to the backup element queue, which the
 * standard processes at the next microtask checkpoint. Between
 * tasks (the parser's insertions) no checkpoint is coming, so the queue is
 * OWED: the host runs it with os64_dom_reactions_run, and
 * os64_dom_timer_next answers "now" until it has (decision 6). */

/* The lifecycle callbacks a definition keeps, in the order read_definition
 * reads them. */
enum { CB_CONNECTED, CB_DISCONNECTED, CB_ADOPTED, CB_ATTRIBUTE, CB_COUNT };
/* An upgrade, or one of the CB_ callbacks. */
enum { R_UPGRADE = CB_COUNT };

#define D_CUSTOM_BUCKETS 64

typedef struct DDefinition {
    struct DDefinition *next, *hash_next;
    char *name;
    DValue *constructor, *prototype, *callbacks[CB_COUNT];
    /* The script each was handed over from (d_callback_script): whose
     * failure an exception in it is. */
    uint32_t constructor_script, scripts[CB_COUNT];
    char **observed;
    size_t observed_count;
    /* The construction stack: the elements being upgraded, innermost
     * last; NULL is the "already constructed" marker. Each is held by its
     * DCustom, which is marked constructing. */
    const os64_html_node_t **stack;
    size_t depth, stack_cap;
    /* The first failed construction's text, for the host's census. */
    char *failure;
} DDefinition;

typedef struct DReaction {
    struct DReaction *next;
    uint8_t kind;
    /* attributeChangedCallback: local name, old value, new value and
     * namespace, NULL for null, all in this record's one block. */
    const char *strings[4];
    /* adoptedCallback: the old and the new document, held. */
    const os64_html_node_t *documents[2];
} DReaction;

typedef struct DCustom {
    struct DCustom *hash_next;
    const os64_html_node_t *element;
    DReaction *first, *last;
    /* How many element queues name it, and whether a construction stack
     * does: it is freed when neither does and nothing waits. */
    size_t queued;
    bool constructing;
} DCustom;

typedef struct {
    DCustom **items;
    size_t count, cap;
} DElementQueue;

typedef struct DWaiting {
    struct DWaiting *next;
    char *name;
    DValue *promise, *resolve;
} DWaiting;

struct DCustomState {
    DValue *registry;
    JSClassID class_id;
    DDefinition *definitions, *last_definition, *by_name[D_CUSTOM_BUCKETS];
    DCustom *records[D_CUSTOM_BUCKETS];
    /* The reaction stack, one element queue per [CEReactions] frame, and
     * the backup element queue with its "processing" flag (requested). */
    DElementQueue *stack;
    size_t depth, stack_cap;
    DElementQueue backup;
    bool requested, owed, defining;
    /* Reactions the listener could not record for want of memory, reported
     * as one QuotaExceededError at the next processing. */
    uint64_t lost;
    /* Nodes define's candidate walks visited, for the page's record. */
    uint64_t visits;
    DWaiting *waiting;
    /* The last removal-only record's run and the document it left, read by
     * the insertion that follows it in the same verb (a move is two
     * records, told at one version), and compared by address alone: a
     * removed node may be reclaimed before any later record. */
    const os64_html_node_t *moving;
    size_t moving_count;
    uint64_t moving_version;
    const os64_html_node_t *moving_document;
    /* What os64_dom_reactions_run calls inside its task. */
    DValue *runner;
    /* What install made of each [CEReactions] row (D_CE_WRAPPED and the
     * rest), and how many members it wrapped, for the DOM host. */
    uint8_t *row_states;
    size_t wrapped;
};

static int registry_mark;

static size_t name_bucket(const char *name)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *c = (const unsigned char *)name; *c != '\0'; c++) hash = (hash ^ *c) * 16777619u;
    return hash % D_CUSTOM_BUCKETS;
}

static size_t node_bucket(const os64_html_node_t *node)
{
    return ((uintptr_t)node >> 4) % D_CUSTOM_BUCKETS;
}

static char *copy_text(os64_dom_t *dom, const char *text)
{
    size_t size = os64_strlen(text) + 1;
    char *out = d_alloc(dom, size);
    if (out != NULL) os64_memcpy(out, text, size);
    return out;
}

/* ── Definitions ─────────────────────────────────────────────────────── */

static DDefinition *definition_named(const struct DCustomState *s, const char *name)
{
    for (DDefinition *d = s->by_name[name_bucket(name)]; d != NULL; d = d->hash_next)
        if (os64_streq(d->name, name)) return d;
    return NULL;
}

/* The definition an element of the window's registry would have: an HTML
 * element with a valid custom name, defined. No scoped registries and no
 * undefine, so the name alone finds it. */
static DDefinition *definition_for(os64_dom_t *dom, const os64_html_node_t *element)
{
    struct DCustomState *s = dom->custom;
    if (s == NULL || s->definitions == NULL || element->kind != OS64_HTML_ELEMENT ||
        element->ns != OS64_HTML_NS_HTML || element->tag != OS64_HTML_TAG_UNKNOWN)
        return NULL;
    return definition_named(s, element->name);
}

static DDefinition *definition_constructed_by(const struct DCustomState *s, JSValueConst constructor)
{
    for (DDefinition *d = s->definitions; d != NULL; d = d->next)
        if (d_same_object(constructor, d->constructor)) return d;
    return NULL;
}

static bool observes(const DDefinition *d, const char *local)
{
    for (size_t i = 0; i < d->observed_count; i++)
        if (os64_streq(d->observed[i], local)) return true;
    return false;
}

/* ── Records and queues ──────────────────────────────────────────────── */

static DCustom *record_of(os64_dom_t *dom, const os64_html_node_t *element, bool make)
{
    struct DCustomState *s = dom->custom;
    size_t at = node_bucket(element);
    for (DCustom *r = s->records[at]; r != NULL; r = r->hash_next)
        if (r->element == element) return r;
    if (!make) return NULL;
    DCustom *r = d_alloc(dom, sizeof(*r));
    if (r == NULL) return NULL;
    os64_memset(r, 0, sizeof(*r));
    r->element = element;
    os64_html_hold(dom->document, element);
    r->hash_next = s->records[at];
    s->records[at] = r;
    return r;
}

static void reaction_free(os64_dom_t *dom, DReaction *reaction)
{
    for (int i = 0; i < 2; i++)
        if (reaction->documents[i] != NULL) os64_html_release(dom->document, reaction->documents[i]);
    d_free(dom, reaction);
}

/* A record nothing names and nothing waits on goes, and lets go of its
 * element. */
static void record_settle(os64_dom_t *dom, DCustom *r)
{
    if (r->queued != 0 || r->constructing || r->first != NULL) return;
    DCustom **link = &dom->custom->records[node_bucket(r->element)];
    while (*link != r) link = &(*link)->hash_next;
    *link = r->hash_next;
    os64_html_release(dom->document, r->element);
    d_free(dom, r);
}

static bool queue_push(os64_dom_t *dom, DElementQueue *q, DCustom *r)
{
    if (q->count == q->cap) {
        size_t cap = q->cap != 0 ? q->cap * 2 : 8;
        DCustom **items = d_alloc(dom, cap * sizeof(*items));
        if (items == NULL) return false;
        if (q->count != 0) os64_memcpy(items, q->items, q->count * sizeof(*items));
        d_free(dom, q->items);
        q->items = items;
        q->cap = cap;
    }
    q->items[q->count++] = r;
    r->queued++;
    return true;
}

static JSValue backup_job(JSContext *ctx, int argc, JSValueConst *argv);

/* The backup element queue is processed at the next checkpoint when one is
 * coming (a microtask), and owed to the host when none is. */
static void request_backup(os64_dom_t *dom)
{
    struct DCustomState *s = dom->custom;
    if (os64_js_turn_open(dom->runtime)) {
        os64_js_outcome_t outcome;
        JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &outcome);
        if (ctx != NULL && JS_EnqueueJob(ctx, backup_job, 0, NULL) == 0) {
            s->owed = false;
            return;
        }
        if (ctx != NULL && JS_HasException(ctx) && d_exception_catchable(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    s->owed = true;
}

/* HTML's "enqueue an element on the appropriate element queue". */
static void enqueue_element(os64_dom_t *dom, DCustom *r)
{
    struct DCustomState *s = dom->custom;
    if (s->depth != 0) {
        if (!queue_push(dom, &s->stack[s->depth - 1], r)) s->lost++;
        return;
    }
    if (!queue_push(dom, &s->backup, r)) {
        s->lost++;
        return;
    }
    if (!s->requested || (s->owed && os64_js_turn_open(dom->runtime))) {
        s->requested = true;
        request_backup(dom);
    }
}

/* A reaction on an element's queue, and the element on a queue. The strings
 * are copied into the record's one block; a reaction that cannot be
 * recorded is counted lost, never dropped silently. */
static void enqueue(os64_dom_t *dom, const os64_html_node_t *element, int kind, const char *const strings[4],
                    const os64_html_node_t *const documents[2])
{
    size_t lengths[4] = {0}, size = sizeof(DReaction);
    for (int i = 0; strings != NULL && i < 4; i++)
        if (strings[i] != NULL) size += (lengths[i] = os64_strlen(strings[i]) + 1);
    DReaction *reaction = d_alloc(dom, size);
    DCustom *r = reaction != NULL ? record_of(dom, element, true) : NULL;
    if (r == NULL) {
        d_free(dom, reaction);
        dom->custom->lost++;
        return;
    }
    os64_memset(reaction, 0, sizeof(*reaction));
    reaction->kind = (uint8_t)kind;
    char *at = (char *)(reaction + 1);
    for (int i = 0; strings != NULL && i < 4; i++)
        if (strings[i] != NULL) {
            os64_memcpy(at, strings[i], lengths[i]);
            reaction->strings[i] = at;
            at += lengths[i];
        }
    for (int i = 0; documents != NULL && i < 2; i++) {
        reaction->documents[i] = documents[i];
        os64_html_hold(dom->document, documents[i]);
    }
    if (r->last != NULL) r->last->next = reaction;
    else r->first = reaction;
    r->last = reaction;
    enqueue_element(dom, r);
}

/* HTML's "enqueue a custom element callback reaction": nothing when the
 * definition has no such callback, or does not observe the attribute. */
static void enqueue_callback(os64_dom_t *dom, const os64_html_node_t *element, int kind,
                             const char *const strings[4], const os64_html_node_t *const documents[2])
{
    DDefinition *d = definition_for(dom, element);
    if (d == NULL || d->callbacks[kind] == NULL) return;
    if (kind == CB_ATTRIBUTE && !observes(d, strings[0])) return;
    enqueue(dom, element, kind, strings, documents);
}

/* HTML's "try to upgrade": an element of the window's document with a
 * definition, waiting for one. An upgrade queued twice runs once: the
 * second finds the element no longer waiting (upgrade). */
static void try_upgrade(os64_dom_t *dom, const os64_html_node_t *element)
{
    if (element->ce != OS64_HTML_CE_PLAIN || definition_for(dom, element) == NULL ||
        !d_window_document(dom, element))
        return;
    enqueue(dom, element, R_UPGRADE, NULL, NULL);
}

/* ── The listener ────────────────────────────────────────────────────── */

static const os64_html_node_t *window_document(const os64_dom_t *dom)
{
    return dom->document->document;
}

/* The document an inserted run came from: the one the removal just before
 * it in the same verb named (a move, a drained fragment), else the one
 * the node's record keeps as a root (d_leaving, d_made_in), the window's
 * for none. */
static const os64_html_node_t *came_from(os64_dom_t *dom, const os64_html_change_t *c,
                                         const os64_html_node_t *node)
{
    const struct DCustomState *s = dom->custom;
    if (s->moving != NULL && s->moving_version == c->version && s->moving == c->added &&
        s->moving_count == c->nadded)
        return s->moving_document;
    DValue *entry = d_find(dom, node);
    return entry != NULL && entry->document != NULL ? entry->document : window_document(dom);
}

static void attribute_changed(os64_dom_t *dom, const os64_html_change_t *c)
{
    const os64_html_node_t *element = c->target;
    if (element->ce != OS64_HTML_CE_CUSTOM) return;
    const os64_html_attr_t *now = os64_html_attr(element, c->attr_name);
    /* The old value is borrowed and not terminated: copied here, short. */
    char old[256];
    const char *was = NULL;
    char *long_old = NULL;
    if (c->attr_old != NULL) {
        char *into = c->attr_old_len < sizeof(old) ? old : (long_old = d_alloc(dom, c->attr_old_len + 1));
        if (into == NULL) {
            dom->custom->lost++;
            return;
        }
        os64_memcpy(into, c->attr_old, c->attr_old_len);
        into[c->attr_old_len] = '\0';
        was = into;
    }
    const char *strings[4] = {c->attr_local, was, now != NULL ? now->value : NULL, c->attr_ns};
    enqueue_callback(dom, element, CB_ATTRIBUTE, strings, NULL);
    d_free(dom, long_old);
}

static void children_changed(os64_dom_t *dom, const os64_html_change_t *c)
{
    struct DCustomState *s = dom->custom;
    if (c->nremoved != 0) {
        /* Disconnected when the parent they left is in a document. */
        if (d_connected(c->target)) {
            const os64_html_node_t *n = c->removed;
            for (size_t i = 0; i < c->nremoved && n != NULL; i++, n = n->next)
                for (const os64_html_node_t *at = n; at != NULL; at = d_walk_next(n, at))
                    if (at->kind == OS64_HTML_ELEMENT && at->ce == OS64_HTML_CE_CUSTOM)
                        enqueue_callback(dom, at, CB_DISCONNECTED, NULL, NULL);
        }
        /* A removal alone begins a move (a node leaving its parent, a
         * fragment drained); a replace's removed node goes nowhere, and must
         * not hide where the node it brings in came from. */
        if (dom->documents != 0 && c->nadded == 0) {
            s->moving = c->removed;
            s->moving_count = c->nremoved;
            s->moving_version = c->version;
            s->moving_document = d_node_document(dom, c->target);
        }
    }
    if (c->nadded == 0) return;
    bool connected = d_connected(c->target);
    const os64_html_node_t *document = dom->documents != 0 ? d_node_document(dom, c->target) : NULL;
    const os64_html_node_t *n = c->added;
    for (size_t i = 0; i < c->nadded && n != NULL; i++, n = n->next) {
        /* A node from another document is adopted before it is connected. */
        const os64_html_node_t *from = document != NULL ? came_from(dom, c, n) : NULL;
        if (from != NULL && from != document) {
            const os64_html_node_t *documents[2] = {from, document};
            for (const os64_html_node_t *at = n; at != NULL; at = d_walk_next(n, at))
                if (at->kind == OS64_HTML_ELEMENT && at->ce == OS64_HTML_CE_CUSTOM)
                    enqueue_callback(dom, at, CB_ADOPTED, NULL, documents);
        }
        if (!connected) continue;
        for (const os64_html_node_t *at = n; at != NULL; at = d_walk_next(n, at)) {
            if (at->kind != OS64_HTML_ELEMENT) continue;
            if (at->ce == OS64_HTML_CE_CUSTOM) enqueue_callback(dom, at, CB_CONNECTED, NULL, NULL);
            else try_upgrade(dom, at);
        }
    }
    s->moving = NULL;
    s->moving_count = 0;
}

static void changed(void *opaque, const os64_html_change_t *c)
{
    os64_dom_t *dom = opaque;
    /* No definitions, no work: a page that defines nothing walks nothing. */
    if (dom->closed || dom->custom == NULL || dom->custom->definitions == NULL) return;
    if (c->kind == OS64_HTML_CHANGE_ATTR) attribute_changed(dom, c);
    else if (c->kind == OS64_HTML_CHANGE_CHILDREN) children_changed(dom, c);
}

/* ── Invoking ────────────────────────────────────────────────────────── */

/* An exception a reaction ended in: reported as `script`'s failure and the
 * next reaction runs; -1 when the runtime must stop, leaving it pending. */
static int report(os64_dom_t *dom, JSContext *ctx, uint32_t script)
{
    if (!d_exception_catchable(ctx)) return -1;
    JSValue error = JS_GetException(ctx);
    d_report_as(dom, ctx, error, script);
    JS_FreeValue(ctx, error);
    return 0;
}

static void remember_failure(os64_dom_t *dom, JSContext *ctx, DDefinition *d, JSValueConst error)
{
    if (d->failure != NULL) return;
    char text[256];
    d_error_text(dom, ctx, error, text, sizeof(text));
    d->failure = copy_text(dom, text);
}

/* HTML's "upgrade an element". */
static int upgrade(os64_dom_t *dom, JSContext *ctx, DCustom *r)
{
    const os64_html_node_t *element = r->element;
    DDefinition *d = definition_for(dom, element);
    if (element->ce != OS64_HTML_CE_PLAIN || d == NULL) return 0;
    /* The wrapper first, so one made now is HTMLElement's and not the
     * failed state's HTMLUnknownElement (d_node_kind). */
    JSValue self = d_wrap(dom, ctx, element);
    if (JS_IsException(self)) return report(dom, ctx, d->constructor_script);
    /* Room on the construction stack before anything is marked or queued:
     * an upgrade the binding cannot afford leaves the element waiting with
     * nothing queued for it, and is reported. */
    if (d->depth == d->stack_cap) {
        size_t cap = d->stack_cap != 0 ? d->stack_cap * 2 : 4;
        const os64_html_node_t **stack = d_alloc(dom, cap * sizeof(*stack));
        if (stack == NULL) {
            JS_FreeValue(ctx, self);
            d_error(ctx, "QuotaExceededError", "Custom element construction stack quota exceeded");
            return report(dom, ctx, d->constructor_script);
        }
        if (d->depth != 0) os64_memcpy(stack, d->stack, d->depth * sizeof(*stack));
        d_free(dom, d->stack);
        d->stack = stack;
        d->stack_cap = cap;
    }
    os64_html_set_ce(dom->document, (os64_html_node_t *)element, OS64_HTML_CE_FAILED);
    for (const os64_html_attr_t *a = element->attrs; a != NULL; a = a->next) {
        const char *strings[4] = {d_attr_local(a->name, a->ns), NULL, a->value, a->ns};
        enqueue_callback(dom, element, CB_ATTRIBUTE, strings, NULL);
    }
    if (d_connected(element)) enqueue_callback(dom, element, CB_CONNECTED, NULL, NULL);
    d->stack[d->depth++] = element;
    r->constructing = true;
    DScripts before = d_script_enter(dom, d->constructor_script);
    JSValue result = JS_CallConstructor(ctx, d->constructor->value, 0, NULL);
    d_script_leave(dom, &before, false);
    d->depth--;
    r->constructing = false;
    bool same = !JS_IsException(result) && JS_IsObject(result) &&
                JS_VALUE_GET_PTR(result) == JS_VALUE_GET_PTR(self);
    if (!JS_IsException(result) && !same)
        JS_ThrowTypeError(ctx, "The custom element's constructor did not return the element it upgraded");
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, self);
    if (same) {
        os64_html_set_ce(dom->document, (os64_html_node_t *)element, OS64_HTML_CE_CUSTOM);
        return 0;
    }
    /* Failed: what it queued for itself goes with it. */
    while (r->first != NULL) {
        DReaction *next = r->first->next;
        reaction_free(dom, r->first);
        r->first = next;
    }
    r->last = NULL;
    if (!d_exception_catchable(ctx)) return -1;
    JSValue error = JS_GetException(ctx);
    remember_failure(dom, ctx, d, error);
    d_report_as(dom, ctx, error, d->constructor_script);
    JS_FreeValue(ctx, error);
    return 0;
}

static int callback(os64_dom_t *dom, JSContext *ctx, DCustom *r, DReaction *reaction)
{
    DDefinition *d = definition_for(dom, r->element);
    if (d == NULL || d->callbacks[reaction->kind] == NULL) return 0;
    uint32_t script = d->scripts[reaction->kind];
    JSValue self = d_wrap(dom, ctx, r->element);
    if (JS_IsException(self)) return report(dom, ctx, script);
    JSValue args[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    int argc = 0;
    if (reaction->kind == CB_ATTRIBUTE) {
        argc = 4;
        for (int i = 0; i < 4; i++)
            args[i] = reaction->strings[i] != NULL ? JS_NewString(ctx, reaction->strings[i]) : JS_NULL;
    } else if (reaction->kind == CB_ADOPTED) {
        argc = 2;
        for (int i = 0; i < 2; i++) args[i] = d_wrap(dom, ctx, reaction->documents[i]);
    }
    bool made = true;
    for (int i = 0; i < argc; i++) made = made && !JS_IsException(args[i]);
    JSValue result = JS_EXCEPTION;
    if (made) {
        DScripts before = d_script_enter(dom, script);
        result = JS_Call(ctx, d->callbacks[reaction->kind]->value, self, argc, args);
        d_script_leave(dom, &before, false);
    }
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, self);
    if (!JS_IsException(result)) {
        JS_FreeValue(ctx, result);
        return 0;
    }
    return report(dom, ctx, script);
}

/* An element's reactions, in order, until none waits: those a reaction
 * queues for its own element run in the same pass. */
static int invoke_record(os64_dom_t *dom, JSContext *ctx, DCustom *r)
{
    while (r->first != NULL) {
        DReaction *reaction = r->first;
        r->first = reaction->next;
        if (r->first == NULL) r->last = NULL;
        int status = reaction->kind == R_UPGRADE ? upgrade(dom, ctx, r) : callback(dom, ctx, r, reaction);
        reaction_free(dom, reaction);
        if (status < 0) return -1;
    }
    return 0;
}

/* Reactions lost for want of memory are said once, as the page's
 * failure. */
static void report_lost(os64_dom_t *dom, JSContext *ctx)
{
    struct DCustomState *s = dom->custom;
    if (s->lost == 0) return;
    char text[96];
    os64_snprintf(text, sizeof(text), "%llu custom element reactions were lost: the DOM quota was exceeded",
                  (unsigned long long)s->lost);
    s->lost = 0;
    d_error(ctx, "QuotaExceededError", text);
    if (d_exception_catchable(ctx)) {
        JSValue error = JS_GetException(ctx);
        d_report(dom, ctx, error);
        JS_FreeValue(ctx, error);
    }
}

/* HTML's "invoke custom element reactions" over a queue, which may grow
 * while it runs (the backup queue does). Every record it named is let go
 * of, even after a failure that stops it. */
static int invoke_queue(os64_dom_t *dom, JSContext *ctx, DElementQueue *q)
{
    report_lost(dom, ctx);
    int status = 0;
    size_t i = 0;
    for (; i < q->count && status == 0; i++) {
        DCustom *r = q->items[i];
        status = invoke_record(dom, ctx, r);
        r->queued--;
        record_settle(dom, r);
    }
    for (; i < q->count; i++) {
        q->items[i]->queued--;
        record_settle(dom, q->items[i]);
    }
    q->count = 0;
    return status;
}

static int process_backup(os64_dom_t *dom, JSContext *ctx)
{
    struct DCustomState *s = dom->custom;
    int status = invoke_queue(dom, ctx, &s->backup);
    s->requested = false;
    s->owed = false;
    return status;
}

/* The backup queue's microtask. One the queue no longer wants (the host
 * ran it in the meantime, or it is owed again) does nothing. */
static JSValue backup_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL || dom->custom == NULL) return JS_UNDEFINED;
    if (!dom->custom->requested || dom->custom->owed) return JS_UNDEFINED;
    return process_backup(dom, ctx) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* os64_dom_reactions_run's one call inside its task: the backup queue. */
static JSValue run_owed(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    return dom->custom != NULL && process_backup(dom, ctx) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* ── [CEReactions] in C ──────────────────────────────────────────────── */

int d_reactions_push(os64_dom_t *dom, JSContext *ctx)
{
    struct DCustomState *s = dom->custom;
    if (s == NULL) return 0;
    if (s->depth == s->stack_cap) {
        size_t cap = s->stack_cap != 0 ? s->stack_cap * 2 : 8;
        DElementQueue *stack = d_alloc(dom, cap * sizeof(*stack));
        if (stack == NULL) {
            d_error(ctx, "QuotaExceededError", "Custom element reaction stack quota exceeded");
            return -1;
        }
        if (s->depth != 0) os64_memcpy(stack, s->stack, s->depth * sizeof(*stack));
        d_free(dom, s->stack);
        s->stack = stack;
        s->stack_cap = cap;
    }
    os64_memset(&s->stack[s->depth++], 0, sizeof(DElementQueue));
    return 0;
}

/* A queue let go of without running it: the runtime must stop. */
static void queue_release(os64_dom_t *dom, DElementQueue *q)
{
    for (size_t i = 0; i < q->count; i++) {
        q->items[i]->queued--;
        record_settle(dom, q->items[i]);
    }
    q->count = 0;
}

int d_reactions_pop(os64_dom_t *dom, JSContext *ctx)
{
    struct DCustomState *s = dom->custom;
    if (s == NULL || s->depth == 0) return 0;
    /* Popped before it runs, as the standard pops it: what its reactions
     * cause goes to the queue below, or the backup queue. */
    DElementQueue q = s->stack[--s->depth];
    /* Most wrapped calls queue nothing: an empty queue, with nothing lost
     * to report, has nothing to run and nothing to let go of. */
    if (q.count == 0 && s->lost == 0) {
        d_free(dom, q.items);
        return 0;
    }
    /* A member that threw still runs its reactions, then throws: the
     * exception is set aside meanwhile. One the runtime made uncatchable is
     * never taken, and nothing more runs. */
    bool threw = JS_HasException(ctx);
    JSValue pending = JS_UNDEFINED;
    int status = -1;
    if (threw && !d_exception_catchable(ctx)) {
        queue_release(dom, &q);
    } else {
        if (threw) pending = JS_GetException(ctx);
        status = invoke_queue(dom, ctx, &q);
        if (threw && status == 0) JS_Throw(ctx, pending);
        else JS_FreeValue(ctx, pending);
    }
    d_free(dom, q.items);
    return status;
}

/* ── The HTMLElement constructor (HTML § 3.2.3) ──────────────────────── */

JSValue d_html_element_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    struct DCustomState *s = dom->custom;
    DDefinition *d = s != NULL ? definition_constructed_by(s, new_target) : NULL;
    if (d == NULL) return JS_ThrowTypeError(ctx, "Illegal constructor");
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    if (!JS_IsObject(prototype)) {
        JS_FreeValue(ctx, prototype);
        prototype = JS_DupValue(ctx, dom->prototypes[D_PROTO_HTML_ELEMENT]->value);
    }
    JSValue result;
    if (d->depth == 0) {
        /* `new C()`: an element of the definition's name, already custom. */
        int64_t status;
        os64_html_node_t *element = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, d->name, &status);
        if (element == NULL) result = d_html_error(ctx, status);
        else {
            os64_html_set_ce(dom->document, element, OS64_HTML_CE_CUSTOM);
            result = d_wrap_constructed(dom, ctx, element, prototype);
        }
    } else if (d->stack[d->depth - 1] == NULL) {
        result = d_error(ctx, "InvalidStateError", "This custom element was already constructed");
    } else {
        /* An upgrade's super(): the element being upgraded, given the
         * definition's prototype; the marker takes its place. */
        result = d_wrap(dom, ctx, d->stack[d->depth - 1]);
        if (!JS_IsException(result) && JS_SetPrototype(ctx, result, prototype) < 0) {
            JS_FreeValue(ctx, result);
            result = JS_EXCEPTION;
        }
        if (!JS_IsException(result)) d->stack[d->depth - 1] = NULL;
    }
    JS_FreeValue(ctx, prototype);
    return result;
}

/* ── createElement and clone ─────────────────────────────────────────── */

/* Whether `value` is the wrapper of a fresh HTML element `name` of the
 * window's document, as synchronous construction must answer. */
static const char *constructed_wrong(os64_dom_t *dom, JSValueConst value, const char *name,
                                     const os64_html_node_t **element)
{
    DValue *entry = JS_IsObject(value) ? JS_GetOpaque(value, dom->node_class) : NULL;
    const os64_html_node_t *node = entry != NULL ? entry->node : NULL;
    *element = node;
    if (node == NULL || node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML)
        return "TypeError";
    if (node->attrs != NULL || node->first_child != NULL || node->parent != NULL ||
        !d_window_document(dom, node) || !os64_streq(node->name, name))
        return "NotSupportedError";
    return NULL;
}

int d_custom_create(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document, const char *name,
                    JSValue *out)
{
    struct DCustomState *s = dom->custom;
    DDefinition *d = s != NULL && document == window_document(dom) && s->definitions != NULL
        ? definition_named(s, name) : NULL;
    if (d == NULL || !os64_html_custom_name(name)) return 0;
    JSValue result = JS_CallConstructor(ctx, d->constructor->value, 0, NULL);
    if (!JS_IsException(result)) {
        const os64_html_node_t *element;
        const char *wrong = constructed_wrong(dom, result, name, &element);
        if (wrong == NULL) {
            *out = result;
            return 1;
        }
        JS_FreeValue(ctx, result);
        d_error(ctx, wrong, os64_streq(wrong, "TypeError")
                                ? "The custom element's constructor did not make an HTML element"
                                : "The custom element's constructor made an element createElement cannot answer");
    }
    /* Reported, not rethrown, and the answer an HTMLUnknownElement in the
     * failed state (HTML § 4.13.3's "create an element"). */
    if (!d_exception_catchable(ctx)) {
        *out = JS_EXCEPTION;
        return 1;
    }
    JSValue error = JS_GetException(ctx);
    remember_failure(dom, ctx, d, error);
    d_report_as(dom, ctx, error, d->constructor_script);
    JS_FreeValue(ctx, error);
    int64_t status;
    os64_html_node_t *element = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, name, &status);
    if (element == NULL) {
        *out = d_html_error(ctx, status);
        return 1;
    }
    os64_html_set_ce(dom->document, element, OS64_HTML_CE_FAILED);
    *out = d_wrap(dom, ctx, element);
    return 1;
}

int d_custom_cloned(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *copy)
{
    if (dom->custom == NULL || dom->custom->definitions == NULL || !d_window_document(dom, copy)) return 0;
    if (d_reactions_push(dom, ctx) < 0) return -1;
    for (const os64_html_node_t *at = copy; at != NULL; at = d_walk_next(copy, at))
        if (at->kind == OS64_HTML_ELEMENT) try_upgrade(dom, at);
    return d_reactions_pop(dom, ctx);
}

/* ── The registry (HTML § 4.13.4) ────────────────────────────────────── */

enum { M_DEFINE, M_GET, M_GET_NAME, M_WHEN_DEFINED, M_UPGRADE };

/* WebIDL's sequence<DOMString> from an iterable: each string copied, the
 * array and the strings d_alloc'd. -1 with an exception pending. */
static int string_sequence(os64_dom_t *dom, JSContext *ctx, JSValueConst value, char ***out, size_t *count)
{
    *out = NULL;
    *count = 0;
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "Expected a sequence");
        return -1;
    }
    JSAtom symbol = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
    if (symbol == JS_ATOM_NULL) return -1;
    JSValue method = JS_GetProperty(ctx, value, symbol);
    JS_FreeAtom(ctx, symbol);
    if (JS_IsException(method)) return -1;
    if (!JS_IsFunction(ctx, method)) {
        JS_FreeValue(ctx, method);
        JS_ThrowTypeError(ctx, "Expected a sequence");
        return -1;
    }
    JSValue iterator = JS_Call(ctx, method, value, 0, NULL);
    JS_FreeValue(ctx, method);
    if (JS_IsException(iterator)) return -1;
    JSValue next = JS_GetPropertyStr(ctx, iterator, "next");
    int status = JS_IsException(next) ? -1 : 0;
    size_t cap = 0;
    while (status == 0) {
        JSValue step = JS_Call(ctx, next, iterator, 0, NULL);
        if (JS_IsException(step) || !JS_IsObject(step)) {
            if (!JS_IsException(step)) JS_ThrowTypeError(ctx, "An iterator's result is not an object");
            JS_FreeValue(ctx, step);
            status = -1;
            break;
        }
        JSValue done = JS_GetPropertyStr(ctx, step, "done");
        int finished = JS_IsException(done) ? -1 : JS_ToBool(ctx, done);
        JS_FreeValue(ctx, done);
        if (finished != 0) {
            JS_FreeValue(ctx, step);
            status = finished < 0 ? -1 : 0;
            break;
        }
        JSValue item = JS_GetPropertyStr(ctx, step, "value");
        JS_FreeValue(ctx, step);
        DString text = {0};
        bool ok = !JS_IsException(item) && d_string(dom, ctx, item, &text);
        JS_FreeValue(ctx, item);
        if (!ok) {
            status = -1;
            break;
        }
        if (*count == cap) {
            size_t grown = cap != 0 ? cap * 2 : 8;
            char **array = d_alloc(dom, grown * sizeof(*array));
            if (array == NULL) {
                d_string_free(dom, &text);
                d_error(ctx, "QuotaExceededError", "Custom element definition quota exceeded");
                status = -1;
                break;
            }
            if (*count != 0) os64_memcpy(array, *out, *count * sizeof(*array));
            d_free(dom, *out);
            *out = array;
            cap = grown;
        }
        (*out)[(*count)++] = text.data;
    }
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, iterator);
    if (status < 0) {
        for (size_t i = 0; i < *count; i++) d_free(dom, (*out)[i]);
        d_free(dom, *out);
        *out = NULL;
        *count = 0;
    }
    return status;
}

static void strings_free(os64_dom_t *dom, char **strings, size_t count)
{
    for (size_t i = 0; i < count; i++) d_free(dom, strings[i]);
    d_free(dom, strings);
}

/* A value read off `from` and converted to a callback function: NULL for
 * undefined, *failed with an exception pending for anything else that is
 * not callable. */
static JSValue callback_value(JSContext *ctx, JSValueConst from, const char *name, bool *failed)
{
    JSValue value = JS_GetPropertyStr(ctx, from, name);
    if (JS_IsException(value)) {
        *failed = true;
        return value;
    }
    if (!JS_IsUndefined(value) && !JS_IsFunction(ctx, value)) {
        JS_FreeValue(ctx, value);
        JS_ThrowTypeError(ctx, "%s is not a function", name);
        *failed = true;
        return JS_EXCEPTION;
    }
    return value;
}

/* What define reads from the constructor and its prototype, in the
 * standard's order (step 14), before anything is recorded. */
typedef struct {
    JSValue prototype, callbacks[CB_COUNT];
    char **observed;
    size_t observed_count;
} DReading;

static int read_definition(os64_dom_t *dom, JSContext *ctx, JSValueConst constructor, DReading *r)
{
    r->prototype = JS_GetPropertyStr(ctx, constructor, "prototype");
    if (JS_IsException(r->prototype)) return -1;
    if (!JS_IsObject(r->prototype)) {
        JS_ThrowTypeError(ctx, "The constructor's prototype is not an object");
        return -1;
    }
    bool failed = false;
    /* connectedMoveCallback, moveBefore's, is not read: there is no
     * moveBefore, and WPT at the pin reads these four alone. */
    static const char *const kOrder[CB_COUNT] = {"connectedCallback", "disconnectedCallback", "adoptedCallback",
                                                 "attributeChangedCallback"};
    for (int i = 0; i < CB_COUNT && !failed; i++)
        r->callbacks[i] = callback_value(ctx, r->prototype, kOrder[i], &failed);
    if (failed) return -1;
    if (!JS_IsUndefined(r->callbacks[CB_ATTRIBUTE])) {
        JSValue observed = JS_GetPropertyStr(ctx, constructor, "observedAttributes");
        if (JS_IsException(observed)) return -1;
        int status = JS_IsUndefined(observed) ? 0 : string_sequence(dom, ctx, observed, &r->observed, &r->observed_count);
        JS_FreeValue(ctx, observed);
        if (status < 0) return -1;
    }
    /* disabledFeatures and formAssociated are read as the standard reads
     * them, and kept by nothing: there is no ElementInternals (DEBTS.md). */
    JSValue disabled = JS_GetPropertyStr(ctx, constructor, "disabledFeatures");
    if (JS_IsException(disabled)) return -1;
    if (!JS_IsUndefined(disabled)) {
        char **features;
        size_t count;
        int status = string_sequence(dom, ctx, disabled, &features, &count);
        JS_FreeValue(ctx, disabled);
        if (status < 0) return -1;
        strings_free(dom, features, count);
    }
    JSValue associated = JS_GetPropertyStr(ctx, constructor, "formAssociated");
    if (JS_IsException(associated)) return -1;
    int form = JS_ToBool(ctx, associated);
    JS_FreeValue(ctx, associated);
    if (form < 0) return -1;
    static const char *const kForm[] = {"formAssociatedCallback", "formResetCallback", "formDisabledCallback",
                                        "formStateRestoreCallback"};
    for (size_t i = 0; form && i < sizeof(kForm) / sizeof(kForm[0]) && !failed; i++)
        JS_FreeValue(ctx, callback_value(ctx, r->prototype, kForm[i], &failed));
    return failed ? -1 : 0;
}

static void reading_free(os64_dom_t *dom, JSContext *ctx, DReading *r)
{
    JS_FreeValue(ctx, r->prototype);
    for (int i = 0; i < CB_COUNT; i++) JS_FreeValue(ctx, r->callbacks[i]);
    strings_free(dom, r->observed, r->observed_count);
}

/* The definition recorded from a reading (step 16 on): every engine value
 * it keeps is the registry's for the page's life. */
static DDefinition *record_definition(os64_dom_t *dom, JSContext *ctx, const char *name, JSValueConst constructor,
                                      DReading *reading)
{
    DDefinition *d = d_alloc(dom, sizeof(*d));
    if (d == NULL) {
        d_error(ctx, "QuotaExceededError", "Custom element definition quota exceeded");
        return NULL;
    }
    os64_memset(d, 0, sizeof(*d));
    d->name = copy_text(dom, name);
    d->constructor_script = d_callback_script(dom, ctx, constructor);
    bool ok = d->name != NULL && d->constructor_script != D_SCRIPT_FAILED;
    if (ok) {
        d->constructor = d_retain(dom, ctx, JS_DupValue(ctx, constructor));
        d->prototype = d_retain(dom, ctx, JS_DupValue(ctx, reading->prototype));
        ok = d->constructor != NULL && d->prototype != NULL;
    }
    for (int i = 0; ok && i < CB_COUNT; i++) {
        if (JS_IsUndefined(reading->callbacks[i])) continue;
        d->scripts[i] = d_callback_script(dom, ctx, reading->callbacks[i]);
        d->callbacks[i] = d->scripts[i] != D_SCRIPT_FAILED
            ? d_retain(dom, ctx, JS_DupValue(ctx, reading->callbacks[i])) : NULL;
        ok = d->callbacks[i] != NULL;
    }
    if (!ok) {
        if (!JS_HasException(ctx)) d_error(ctx, "QuotaExceededError", "Custom element definition quota exceeded");
        d_free(dom, d->name);
        d_free(dom, d);
        return NULL;
    }
    d->observed = reading->observed;
    d->observed_count = reading->observed_count;
    reading->observed = NULL;
    reading->observed_count = 0;
    struct DCustomState *s = dom->custom;
    size_t at = name_bucket(name);
    d->hash_next = s->by_name[at];
    s->by_name[at] = d;
    if (s->last_definition != NULL) s->last_definition->next = d;
    else s->definitions = d;
    s->last_definition = d;
    return d;
}

static JSValue define(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    struct DCustomState *s = dom->custom;
    if (argc < 2) return JS_ThrowTypeError(ctx, "define needs a name and a constructor");
    DString name = {0};
    if (!d_string(dom, ctx, argv[0], &name)) return JS_EXCEPTION;
    JSValue result = JS_EXCEPTION;
    bool extends = false;
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_ThrowTypeError(ctx, "The constructor is not a function");
        goto done;
    }
    int options = d_dictionary(ctx, argc, argv, 2);
    if (options < 0) goto done;
    if (options > 0) {
        JSValue value = JS_GetPropertyStr(ctx, argv[2], "extends");
        if (JS_IsException(value)) goto done;
        if (!JS_IsUndefined(value)) {
            DString text = {0};
            bool ok = d_string(dom, ctx, value, &text);
            d_string_free(dom, &text);
            JS_FreeValue(ctx, value);
            if (!ok) goto done;
            extends = true;
        }
    }
    if (!JS_IsConstructor(ctx, argv[1])) {
        JS_ThrowTypeError(ctx, "The constructor is not a constructor");
        goto done;
    }
    if (!os64_html_custom_name(name.data)) {
        d_error(ctx, "SyntaxError", "Not a valid custom element name");
        goto done;
    }
    if (definition_named(s, name.data) != NULL) {
        d_error(ctx, "NotSupportedError", "This name is already defined");
        goto done;
    }
    if (definition_constructed_by(s, argv[1]) != NULL) {
        d_error(ctx, "NotSupportedError", "This constructor already defines an element");
        goto done;
    }
    /* Customized built-in elements are not supported (decision 5). */
    if (extends) {
        d_error(ctx, "NotSupportedError", "Customized built-in elements are not supported");
        goto done;
    }
    if (s->defining) {
        d_error(ctx, "NotSupportedError", "A definition is already being made");
        goto done;
    }
    s->defining = true;
    DReading reading = {JS_UNDEFINED, {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED}, NULL, 0};
    int read = read_definition(dom, ctx, argv[1], &reading);
    s->defining = false;
    DDefinition *d = read < 0 ? NULL : record_definition(dom, ctx, name.data, argv[1], &reading);
    reading_free(dom, ctx, &reading);
    if (d == NULL) goto done;
    /* The window's document's elements of that name, in tree order, each
     * queued for an upgrade on define's own element queue (its caller's
     * [CEReactions]), which runs them as define returns: after the promise
     * below is resolved, as the standard orders the two. */
    const os64_html_node_t *document = window_document(dom);
    for (const os64_html_node_t *at = document; at != NULL; at = d_walk_next(document, at)) {
        s->visits++;
        if (at->kind == OS64_HTML_ELEMENT && at->ns == OS64_HTML_NS_HTML && os64_streq(at->name, d->name))
            try_upgrade(dom, at);
    }
    /* Last, the promise whenDefined handed out for it. */
    for (DWaiting **link = &s->waiting; *link != NULL; link = &(*link)->next) {
        DWaiting *w = *link;
        if (!os64_streq(w->name, d->name)) continue;
        *link = w->next;
        JSValue resolved = JS_Call(ctx, w->resolve->value, JS_UNDEFINED, 1, &argv[1]);
        d_free(dom, w->name);
        d_free(dom, w);
        if (JS_IsException(resolved)) goto done;
        JS_FreeValue(ctx, resolved);
        break;
    }
    result = JS_UNDEFINED;
done:
    d_string_free(dom, &name);
    return result;
}

/* whenDefined(name): rejected (SyntaxError) for a name that is no valid
 * custom element name, resolved with the constructor for a defined one,
 * and otherwise the one promise for that name, which define resolves. */
static JSValue when_defined(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    struct DCustomState *s = dom->custom;
    if (argc < 1) return JS_ThrowTypeError(ctx, "whenDefined needs a name");
    DString name = {0};
    if (!d_string(dom, ctx, argv[0], &name)) return JS_EXCEPTION;
    bool valid = os64_html_custom_name(name.data);
    DDefinition *d = valid ? definition_named(s, name.data) : NULL;
    DWaiting *w = s->waiting;
    while (valid && d == NULL && w != NULL && !os64_streq(w->name, name.data)) w = w->next;
    if (valid && d == NULL && w != NULL) {
        d_string_free(dom, &name);
        return JS_DupValue(ctx, w->promise->value);
    }
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue settled = JS_UNDEFINED;
    if (JS_IsException(promise)) {
        settled = JS_EXCEPTION;
    } else if (!valid) {
        d_error(ctx, "SyntaxError", "Not a valid custom element name");
        if (d_exception_catchable(ctx)) {
            JSValue reason = JS_GetException(ctx);
            settled = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &reason);
            JS_FreeValue(ctx, reason);
        } else settled = JS_EXCEPTION;
    } else if (d != NULL) {
        JSValue constructor = d->constructor->value;
        settled = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &constructor);
    } else {
        w = d_alloc(dom, sizeof(*w));
        if (w != NULL) {
            os64_memset(w, 0, sizeof(*w));
            w->name = copy_text(dom, name.data);
            w->promise = w->name != NULL ? d_retain(dom, ctx, JS_DupValue(ctx, promise)) : NULL;
            w->resolve = w->promise != NULL ? d_retain(dom, ctx, JS_DupValue(ctx, resolving[0])) : NULL;
        }
        if (w == NULL || w->resolve == NULL) {
            if (w != NULL) d_free(dom, w->name);
            d_free(dom, w);
            if (!JS_HasException(ctx)) d_error(ctx, "QuotaExceededError", "Custom element registry quota exceeded");
            settled = JS_EXCEPTION;
        } else {
            w->next = s->waiting;
            s->waiting = w;
        }
    }
    if (!JS_IsException(promise)) {
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
    }
    d_string_free(dom, &name);
    if (JS_IsException(settled)) {
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, settled);
    return promise;
}

static JSValue registry_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    struct DCustomState *s = dom->custom;
    if (JS_GetOpaque(self, s->class_id) != &registry_mark)
        return JS_ThrowTypeError(ctx, "Expected a CustomElementRegistry");
    switch (magic) {
    case M_DEFINE: {
        if (d_reactions_push(dom, ctx) < 0) return JS_EXCEPTION;
        JSValue result = define(dom, ctx, argc, argv);
        /* [CEReactions]: what define queued runs before it returns, though
         * it threw. */
        if (d_reactions_pop(dom, ctx) < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        return result;
    }
    case M_GET: case M_GET_NAME: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "This needs an argument");
        if (magic == M_GET_NAME) {
            if (!JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected a constructor");
            DDefinition *d = definition_constructed_by(s, argv[0]);
            return d != NULL ? JS_NewString(ctx, d->name) : JS_NULL;
        }
        DString name = {0};
        if (!d_string(dom, ctx, argv[0], &name)) return JS_EXCEPTION;
        DDefinition *d = definition_named(s, name.data);
        d_string_free(dom, &name);
        return d != NULL ? JS_DupValue(ctx, d->constructor->value) : JS_UNDEFINED;
    }
    case M_WHEN_DEFINED: return when_defined(dom, ctx, argc, argv);
    case M_UPGRADE: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "upgrade needs a node");
        const os64_html_node_t *root = d_node(dom, ctx, argv[0]);
        if (root == NULL) return JS_EXCEPTION;
        if (d_reactions_push(dom, ctx) < 0) return JS_EXCEPTION;
        for (const os64_html_node_t *at = root; at != NULL; at = d_walk_next(root, at))
            if (at->kind == OS64_HTML_ELEMENT) try_upgrade(dom, at);
        return d_reactions_pop(dom, ctx) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    default: return JS_ThrowTypeError(ctx, "Unknown registry member");
    }
}

/* ── [CEReactions] (cut b2) ──────────────────────────────────────────── *
 *
 * Every [CEReactions] member the standard's IDL names (tools/ce_reactions.py
 * writes the table from WPT's dom.idl, html.idl and cssom.idl) is wrapped by
 * one trampoline at install: it pushes an element queue, calls the member,
 * and pops the queue, which runs what the member queued before it returns,
 * and though it threw (d_reactions_pop). A method's function is replaced;
 * an accessor keeps its getter, so a read pays nothing, and gets a wrapped
 * setter. A wrapper keeps the original's name, length and property
 * attributes, and holds the original as its data. */

static const DReactionRow kReactions[] = {
#include "ce_reactions.inc"
};

size_t d_reactions_rows(void)
{
    return sizeof(kReactions) / sizeof(kReactions[0]);
}

bool d_reactions_row_name(size_t i, char *out, size_t cap)
{
    if (i >= d_reactions_rows() || out == NULL || cap == 0) return false;
    os64_snprintf(out, cap, "%s.%s", kReactions[i].interface, kReactions[i].member);
    return true;
}

const char *d_reactions_row_state(const os64_dom_t *dom, size_t i)
{
    static const char *const kStates[] = {"absent", "wrapped", "shared", "unwrappable"};
    const struct DCustomState *s = dom != NULL ? dom->custom : NULL;
    return s != NULL && s->row_states != NULL && i < d_reactions_rows() ? kStates[s->row_states[i]] : "absent";
}

/* data[0] is the binding's anchor (d_callback), data[1] the original. */
static JSValue trampoline(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                          JSValue *data)
{
    (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (d_reactions_push(dom, ctx) < 0) return JS_EXCEPTION;
    JSValue result = JS_Call(ctx, data[1], self, argc, argv);
    if (d_reactions_pop(dom, ctx) < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* A wrapper for `original`, named and as long as it is. */
static JSValue wrapper_for(os64_dom_t *dom, JSContext *ctx, JSValueConst original)
{
    int32_t length = 0;
    JSValue value = JS_GetPropertyStr(ctx, original, "length");
    if (JS_IsException(value) || JS_ToInt32(ctx, &length, value) < 0) {
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, value);
    JSValue data[2] = {dom->anchor->value, original};
    JSValue wrapper = JS_NewCFunctionData(ctx, trampoline, length, 0, 2, data);
    if (JS_IsException(wrapper)) return wrapper;
    JSValue name = JS_GetPropertyStr(ctx, original, "name");
    if (JS_IsException(name) || JS_DefinePropertyValueStr(ctx, wrapper, "name", name, JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, wrapper);
        return JS_EXCEPTION;
    }
    return wrapper;
}

/* The members wrapped so far, by the object that has them and the key:
 * mixin rows (ParentNode's append on Document, Element and
 * DocumentFragment) can reach one member through several interfaces, and
 * it is wrapped once. Chained in buckets, since install meets every CSS
 * property's setter. Each key is held by the property it names. */
#define D_WRAPPED_BUCKETS 512
typedef struct DWrapped {
    struct DWrapped *next;
    void *owner;
    JSAtom key;
} DWrapped;
typedef struct {
    DWrapped *buckets[D_WRAPPED_BUCKETS];
    size_t count;
} DWrapList;

static size_t wrapped_bucket(const void *owner, JSAtom key)
{
    return (((uintptr_t)owner >> 4) ^ (uintptr_t)key * 2654435761u) % D_WRAPPED_BUCKETS;
}

static bool wrapped_already(const DWrapList *list, JSValueConst owner, JSAtom key)
{
    for (const DWrapped *w = list->buckets[wrapped_bucket(JS_VALUE_GET_PTR(owner), key)]; w != NULL; w = w->next)
        if (w->owner == JS_VALUE_GET_PTR(owner) && w->key == key) return true;
    return false;
}

static bool wrapped_add(os64_dom_t *dom, DWrapList *list, JSValueConst owner, JSAtom key)
{
    DWrapped *w = d_alloc(dom, sizeof(*w));
    if (w == NULL) return false;
    w->owner = JS_VALUE_GET_PTR(owner);
    w->key = key;
    size_t at = wrapped_bucket(w->owner, key);
    w->next = list->buckets[at];
    list->buckets[at] = w;
    list->count++;
    return true;
}

static void wrapped_free(os64_dom_t *dom, DWrapList *list)
{
    for (size_t i = 0; i < D_WRAPPED_BUCKETS; i++)
        while (list->buckets[i] != NULL) {
            DWrapped *next = list->buckets[i]->next;
            d_free(dom, list->buckets[i]);
            list->buckets[i] = next;
        }
}

/* Wraps `key` where `from`'s prototype chain has it, as `kind` says: 1
 * wrapped now, 0 wrapped already through another row, -2 absent or not
 * the kind of member the row names, -1 with an exception pending. */
static int wrap_member(os64_dom_t *dom, JSContext *ctx, JSValueConst from, JSAtom key, int kind, DWrapList *list)
{
    JSValue owner = JS_DupValue(ctx, from);
    JSPropertyDescriptor desc;
    int found = 0;
    while (JS_IsObject(owner)) {
        found = JS_GetOwnProperty(ctx, &desc, owner, key);
        if (found != 0) break;
        JSValue up = JS_GetPrototype(ctx, owner);
        JS_FreeValue(ctx, owner);
        owner = up;
        if (JS_IsException(owner)) return -1;
    }
    int status = -2;
    if (found < 0) status = -1;
    else if (found > 0) {
        bool method = kind == D_CE_METHOD && !(desc.flags & JS_PROP_GETSET) && JS_IsFunction(ctx, desc.value);
        bool setter = kind == D_CE_SETTER && (desc.flags & JS_PROP_GETSET) && JS_IsFunction(ctx, desc.setter);
        if ((method || setter) && wrapped_already(list, owner, key)) status = 0;
        else if ((method || setter) && !wrapped_add(dom, list, owner, key)) {
            d_error(ctx, "QuotaExceededError", "Custom element reactions quota exceeded");
            status = -1;
        } else if (method || setter) {
            JSValue wrapper = wrapper_for(dom, ctx, method ? desc.value : desc.setter);
            int flags = desc.flags & (JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
            int defined = -1;
            if (!JS_IsException(wrapper))
                defined = method
                    ? JS_DefinePropertyValue(ctx, owner, key, wrapper, flags)
                    : JS_DefinePropertyGetSet(ctx, owner, key, JS_DupValue(ctx, desc.getter), wrapper,
                                              flags & ~JS_PROP_WRITABLE);
            status = defined < 0 ? -1 : 1;
        }
        JS_FreeValue(ctx, desc.value);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
    }
    JS_FreeValue(ctx, owner);
    return status;
}

/* Every setter CSSStyleDeclaration's prototype has itself: its property
 * attributes, which CSSOM makes [CEReactions] as a class. */
static int wrap_all_setters(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, DWrapList *list)
{
    JSPropertyEnum *names = NULL;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &names, &n, prototype, JS_GPN_STRING_MASK) < 0) return -1;
    int status = 0;
    for (uint32_t i = 0; i < n && status >= 0; i++)
        if (wrap_member(dom, ctx, prototype, names[i].atom, D_CE_SETTER, list) == -1) status = -1;
    JS_FreePropertyEnum(ctx, names, n);
    return status;
}

/* The table, row by row, against the interfaces the rows before this one
 * installed: each row's state is kept for the DOM host, which names the
 * members it expects wrapped and the exotic paths no wrapper reaches. */
static int reactions_wrap(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    struct DCustomState *s = dom->custom;
    size_t rows = d_reactions_rows();
    s->row_states = d_alloc(dom, rows);
    DWrapList *list = d_alloc(dom, sizeof(*list));
    if (s->row_states == NULL || list == NULL) {
        d_free(dom, list);
        d_error(ctx, "QuotaExceededError", "Custom element reactions quota exceeded");
        return -1;
    }
    os64_memset(list, 0, sizeof(*list));
    int status = 0;
    for (size_t i = 0; i < rows && status == 0; i++) {
        const DReactionRow *row = &kReactions[i];
        s->row_states[i] = D_CE_ABSENT;
        if (row->kind == D_CE_EXOTIC) {
            s->row_states[i] = D_CE_UNWRAPPABLE;
            continue;
        }
        JSValue interface = JS_GetPropertyStr(ctx, global, row->interface);
        JSValue prototype = JS_IsObject(interface) ? JS_GetPropertyStr(ctx, interface, "prototype") : JS_UNDEFINED;
        JS_FreeValue(ctx, interface);
        if (JS_IsException(prototype)) {
            status = -1;
            break;
        }
        if (JS_IsObject(prototype)) {
            if (row->kind == D_CE_ALL_SETTERS) {
                status = wrap_all_setters(dom, ctx, prototype, list);
                s->row_states[i] = D_CE_WRAPPED;
            } else {
                JSAtom key = JS_NewAtom(ctx, row->member);
                int wrapped = key == JS_ATOM_NULL ? -1 : wrap_member(dom, ctx, prototype, key, row->kind, list);
                JS_FreeAtom(ctx, key);
                if (wrapped == -1) status = -1;
                else if (wrapped >= 0) s->row_states[i] = wrapped == 1 ? D_CE_WRAPPED : D_CE_SHARED;
            }
        }
        JS_FreeValue(ctx, prototype);
    }
    s->wrapped = list->count;
    wrapped_free(dom, list);
    d_free(dom, list);
    return status;
}

size_t d_reactions_wrapped(const os64_dom_t *dom)
{
    return dom != NULL && dom->custom != NULL ? dom->custom->wrapped : 0;
}

/* The custom elements row (modern.c): CustomElementRegistry, an Illegal
 * constructor; window.customElements, [SameObject]; the [CEReactions]
 * wrappers over the members the rows before it made; and the listener, so
 * everything here exists only with the Modern Web switch on. The
 * HTMLElement constructor is the interfaces row's, which answers Illegal
 * constructor until a definition names its new.target. */
int d_custom_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static JSClassID slot;
    static const JSClassDef registry_class = {.class_name = "CustomElementRegistry"};
    dom->custom = d_alloc(dom, sizeof(*dom->custom));
    if (dom->custom == NULL) {
        d_error(ctx, "QuotaExceededError", "Custom element registry quota exceeded");
        return -1;
    }
    struct DCustomState *s = dom->custom;
    os64_memset(s, 0, sizeof(*s));
    s->class_id = os64_js_class_id(&slot);
    JSValue prototype;
    if (d_interface_class(dom, ctx, global, s->class_id, &registry_class, &prototype) < 0) return -1;
    static const struct { const char *name; int magic, argc; } kMembers[] = {
        {"define", M_DEFINE, 2}, {"get", M_GET, 1}, {"getName", M_GET_NAME, 1},
        {"whenDefined", M_WHEN_DEFINED, 1}, {"upgrade", M_UPGRADE, 1},
    };
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++)
        if (d_method_named(dom, ctx, prototype, kMembers[i].name, registry_member, kMembers[i].argc,
                           kMembers[i].magic) < 0)
            return -1;
    JSValue registry = JS_NewObjectClass(ctx, (int)s->class_id);
    s->registry = d_retain(dom, ctx, registry);
    if (s->registry == NULL) return -1;
    JS_SetOpaque(registry, &registry_mark);
    s->runner = d_retain(dom, ctx, JS_NewCFunction(ctx, run_owed, "customElementReactions", 0));
    if (s->runner == NULL ||
        d_held_getter(dom, ctx, global, "customElements", registry, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    if (reactions_wrap(dom, ctx, global) < 0) return -1;
    os64_html_observe(dom->document, changed, dom);
    return 0;
}

/* ── The host's side ─────────────────────────────────────────────────── */

bool os64_dom_reactions_owed(const os64_dom_t *dom)
{
    return dom != NULL && !dom->closed && dom->custom != NULL && dom->custom->owed;
}

bool os64_dom_reactions_run(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    if (outcome != NULL) os64_memset(outcome, 0, sizeof(*outcome));
    if (outcome == NULL || !os64_dom_reactions_owed(dom)) return false;
    static const char kTask[] = "custom element reactions";
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, kTask, outcome) != OS64_JS_OK) return false;
    /* One call, so the wrapper bounds and reports it as any task's call;
     * the reactions inside it are the binding's own raw calls. */
    os64_js_outcome_t call;
    os64_js_call(dom->runtime, OS64_JS_ABI_ID, dom->custom->runner->value, dom->window->value, 0, NULL, NULL,
                 &call);
    d_outcome_fold(outcome, &call);
    if (call.status == OS64_JS_EXCEPTION) d_errors_fire(dom, outcome);
    d_task_finish(dom, outcome);
    d_task_close(dom, kTask, outcome);
    return true;
}

bool os64_dom_custom_failure(const os64_dom_t *dom, const char *name, char *out, size_t cap)
{
    const struct DCustomState *s = dom != NULL ? dom->custom : NULL;
    DDefinition *d = s != NULL && name != NULL ? definition_named(s, name) : NULL;
    if (d == NULL || d->failure == NULL || out == NULL || cap == 0) return false;
    os64_strcopy(out, cap, d->failure);
    return true;
}

uint64_t os64_dom_custom_visits(const os64_dom_t *dom)
{
    return dom != NULL && dom->custom != NULL ? dom->custom->visits : 0;
}

/* Teardown: the listener goes with the engine's values (os64_dom_drain),
 * and the records and definitions' own memory after the engine. */
void d_custom_drain(os64_dom_t *dom)
{
    if (dom->custom != NULL) os64_html_observe(dom->document, NULL, NULL);
}

void d_custom_free(os64_dom_t *dom)
{
    struct DCustomState *s = dom->custom;
    if (s == NULL) return;
    for (size_t i = 0; i < D_CUSTOM_BUCKETS; i++)
        for (DCustom *r = s->records[i]; r != NULL;) {
            DCustom *next = r->hash_next;
            while (r->first != NULL) {
                DReaction *after = r->first->next;
                reaction_free(dom, r->first);
                r->first = after;
            }
            os64_html_release(dom->document, r->element);
            d_free(dom, r);
            r = next;
        }
    for (DDefinition *d = s->definitions; d != NULL;) {
        DDefinition *next = d->next;
        strings_free(dom, d->observed, d->observed_count);
        d_free(dom, d->stack);
        d_free(dom, d->failure);
        d_free(dom, d->name);
        d_free(dom, d);
        d = next;
    }
    for (DWaiting *w = s->waiting; w != NULL;) {
        DWaiting *next = w->next;
        d_free(dom, w->name);
        d_free(dom, w);
        w = next;
    }
    for (size_t i = 0; i < s->depth; i++) d_free(dom, s->stack[i].items);
    d_free(dom, s->stack);
    d_free(dom, s->backup.items);
    d_free(dom, s->row_states);
    d_free(dom, s);
    dom->custom = NULL;
}
