#include "internal.h"
#include "os64/fmt.h"

/* HTML's timers. The table holds the callbacks and their arguments, so it is
 * one of the registry's lists; the host drives it with its own clock, and
 * each firing is one task. LIBDOM.md § Timers. */

static uint64_t now_ms(const os64_dom_t *dom)
{
    return dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) : 0;
}

static void release_values(os64_dom_t *dom, DTimer *timer)
{
    if (!JS_IsUndefined(timer->callback)) JS_FreeValueRT(dom->engine, timer->callback);
    for (int i = 0; i < timer->argc; i++) JS_FreeValueRT(dom->engine, timer->argv[i]);
    timer->callback = JS_UNDEFINED;
    timer->argc = 0;
}

/* timers_held counts values, as the registry count promises: a callback and
 * each argument it was set with. */
static size_t held_values(const DTimer *timer)
{
    return (JS_IsUndefined(timer->callback) ? 0 : 1) + (size_t)timer->argc;
}

static void timer_free(os64_dom_t *dom, DTimer *timer)
{
    dom->timers_held -= held_values(timer);
    release_values(dom, timer);
    d_free(dom, timer->argv);
    d_free(dom, timer->source);
    d_free(dom, timer);
}

/* Ordered by due time, then by id: two timers due together fire in the order
 * they were set, as HTML's "run steps after a timeout" orders them. */
static void insert(os64_dom_t *dom, DTimer *timer)
{
    DTimer **link = &dom->timers;
    while (*link != NULL && ((*link)->due < timer->due ||
           ((*link)->due == timer->due && (*link)->id < timer->id)))
        link = &(*link)->next;
    timer->next = *link;
    *link = timer;
}

static DTimer *unlink_id(os64_dom_t *dom, uint32_t id)
{
    for (DTimer **link = &dom->timers; *link != NULL; link = &(*link)->next)
        if ((*link)->id == id) {
            DTimer *timer = *link;
            *link = timer->next;
            dom->timer_count--;
            return timer;
        }
    return NULL;
}

/* HTML clamps a deeply nested timer to 4 ms. The host's tick is coarser than
 * that, which makes the clamp moot, but a page can tell the nesting level. */
static uint64_t clamp(uint64_t delay, uint32_t nesting)
{
    return nesting > 5 && delay < 4 ? 4 : delay;
}

enum { T_TIMEOUT, T_INTERVAL, T_CLEAR };

static JSValue timer_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                            int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == T_CLEAR) {
        uint32_t id = 0;
        if (argc > 0 && JS_ToUint32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;
        DTimer *timer = id != 0 ? unlink_id(dom, id) : NULL;
        if (timer != NULL) timer_free(dom, timer);
        return JS_UNDEFINED;
    }
    if (dom->timer_count >= OS64_DOM_TIMERS_MAX || dom->timer_id == INT32_MAX)
        return d_error(ctx, "QuotaExceededError", "Too many timers");
    /* WebIDL's long: the delay wraps as a 32-bit integer, and below zero is
     * zero. */
    int32_t delay = 0;
    if (argc > 1 && JS_ToInt32(ctx, &delay, argv[1]) < 0) return JS_EXCEPTION;
    if (delay < 0) delay = 0;
    DTimer *timer = d_alloc(dom, sizeof(*timer));
    if (timer == NULL) return d_error(ctx, "QuotaExceededError", "DOM timer quota exceeded");
    timer->callback = JS_UNDEFINED;
    JSValueConst handler = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsFunction(ctx, handler)) {
        int extra = argc > 2 ? argc - 2 : 0;
        if (extra != 0) {
            timer->argv = d_alloc(dom, (size_t)extra * sizeof(*timer->argv));
            if (timer->argv == NULL) {
                d_free(dom, timer);
                return d_error(ctx, "QuotaExceededError", "DOM timer quota exceeded");
            }
            for (int i = 0; i < extra; i++) timer->argv[i] = JS_DupValue(ctx, argv[i + 2]);
            timer->argc = extra;
        }
        timer->callback = JS_DupValue(ctx, handler);
        dom->timers_held += held_values(timer);
    } else {
        /* The old web's setTimeout("tick()", 1000): a string is a script. */
        DString source = {0};
        if (!d_string(dom, ctx, handler, &source)) { d_free(dom, timer); return JS_EXCEPTION; }
        timer->source = source.data;
        timer->source_length = source.length;
    }
    timer->id = ++dom->timer_id;
    timer->interval = magic == T_INTERVAL;
    timer->nesting = dom->timer_nesting + 1;
    timer->delay = clamp((uint64_t)delay, timer->nesting);
    timer->due = now_ms(dom) + timer->delay;
    insert(dom, timer);
    dom->timer_count++;
    return JS_NewUint32(ctx, timer->id);
}

int d_timer_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (d_method(dom, ctx, global, "setTimeout", timer_method, 2, T_TIMEOUT) < 0 ||
        d_method(dom, ctx, global, "setInterval", timer_method, 2, T_INTERVAL) < 0 ||
        d_method(dom, ctx, global, "clearTimeout", timer_method, 1, T_CLEAR) < 0) return -1;
    return d_method(dom, ctx, global, "clearInterval", timer_method, 1, T_CLEAR);
}

uint64_t os64_dom_timer_next(const os64_dom_t *dom)
{
    return dom != NULL && !dom->closed && dom->timers != NULL ? dom->timers->due : UINT64_MAX;
}

static void fold(os64_js_outcome_t *into, const os64_js_outcome_t *from)
{
    if (from->status == OS64_JS_OK || from->status == OS64_JS_MORE_JOBS) return;
    bool sticky = from->status == OS64_JS_LIMIT || from->status == OS64_JS_CANCELLED ||
        from->status == OS64_JS_HOST_FAILURE || from->status == OS64_JS_FAILED_RUNTIME;
    if (into->status == OS64_JS_OK || sticky) *into = *from;
}

bool os64_dom_timer_fire(os64_dom_t *dom, uint64_t now, os64_js_outcome_t *outcome)
{
    if (outcome != NULL) os64_memset(outcome, 0, sizeof(*outcome));
    if (outcome == NULL || dom == NULL || dom->closed || dom->timers == NULL || dom->timers->due > now)
        return false;
    DTimer *timer = dom->timers;
    dom->timers = timer->next;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#timer-%u", (int)(sizeof(name) - 24),
                  dom->url != NULL ? dom->url : "about:blank", timer->id);
    /* The call holds its own references: a callback that clears its own
     * timer frees the table's, not the ones running. */
    JSValue held = JS_UNDEFINED;
    JSValue *args = NULL;
    int argc = timer->argc;
    char *source = NULL;
    size_t source_length = timer->source_length;
    bool refused = false;
    if (timer->interval) {
        timer->nesting++;
        timer->delay = clamp(timer->delay, timer->nesting);
        timer->due = now + timer->delay;
        insert(dom, timer);
        if (!JS_IsUndefined(timer->callback)) {
            held = JS_DupValueRT(dom->engine, timer->callback);
            if (argc != 0) {
                args = d_alloc(dom, (size_t)argc * sizeof(*args));
                refused = args == NULL;
                if (refused) argc = 0;
                for (int i = 0; i < argc; i++) args[i] = JS_DupValueRT(dom->engine, timer->argv[i]);
            }
        } else if (timer->source != NULL) {
            source = d_alloc(dom, source_length + 1);
            refused = source == NULL;
            if (source != NULL) os64_memcpy(source, timer->source, source_length + 1);
        }
    } else {
        /* A timeout leaves the table before it runs; its values move into
         * this call and leave the registry with it. */
        dom->timer_count--;
        dom->timers_held -= held_values(timer);
        held = timer->callback;
        args = timer->argv;
        source = timer->source;
        timer->callback = JS_UNDEFINED;
        timer->argv = NULL;
        timer->argc = 0;
        timer->source = NULL;
    }
    uint32_t nesting = timer->nesting;
    if (!timer->interval) d_free(dom, timer);
    dom->timer_nesting = nesting;
    if (refused) {
        /* A firing the binding cannot copy is reported, never run short of
         * its arguments; the interval stays armed for its next turn. */
        outcome->status = OS64_JS_EXCEPTION;
        os64_strcopy(outcome->message, sizeof(outcome->message), "QuotaExceededError: DOM timer quota exceeded");
        os64_strcopy(outcome->source_name, sizeof(outcome->source_name), name);
    } else if (!JS_IsUndefined(held)) {
        os64_js_outcome_t call;
        if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, name, outcome) == OS64_JS_OK) {
            os64_js_call(dom->runtime, OS64_JS_ABI_ID, held, dom->window->value, argc, args, NULL, &call);
            fold(outcome, &call);
            os64_js_task_end(dom->runtime, OS64_JS_ABI_ID, &call);
            fold(outcome, &call);
        }
    } else if (source != NULL) {
        os64_js_run(dom->runtime, source, source_length, name, outcome);
    }
    dom->timer_nesting = 0;
    if (!JS_IsUndefined(held)) JS_FreeValueRT(dom->engine, held);
    for (int i = 0; i < argc; i++) JS_FreeValueRT(dom->engine, args[i]);
    d_free(dom, args);
    d_free(dom, source);
    if (outcome->status == OS64_JS_OK && dom->report.status != OS64_JS_OK) {
        *outcome = dom->report;
        os64_memset(&dom->report, 0, sizeof(dom->report));
    }
    return true;
}

void d_timer_drain(os64_dom_t *dom)
{
    for (DTimer *timer = dom->timers; timer != NULL; timer = timer->next)
        release_values(dom, timer);
    dom->timers_held = 0;
}

void d_timer_free(os64_dom_t *dom)
{
    while (dom->timers != NULL) {
        DTimer *timer = dom->timers;
        dom->timers = timer->next;
        d_free(dom, timer->argv);
        d_free(dom, timer->source);
        d_free(dom, timer);
    }
    dom->timer_count = 0;
}
