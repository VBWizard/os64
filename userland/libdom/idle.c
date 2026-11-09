#include "internal.h"
#include "os64/fmt.h"

/* IDLE CALLBACKS (W3C's requestIdleCallback, and HTML's event loop, whose
 * "start an idle period" it hooks): requestIdleCallback,
 * cancelIdleCallback and IdleDeadline, P2b's row (WINDOW_DOCUMENT.md
 * § P2b). The host says when its loop is idle (os64_dom_idle_run); the
 * binding keeps the lists, the periods and the deadlines.
 *
 *   - requestIdleCallback(callback, {timeout}) puts the callback on the
 *     window's list of idle request callbacks and answers its handle, the
 *     window's count of them; cancelIdleCallback(handle) takes one off
 *     whichever list holds it, and an unknown handle is nothing.
 *   - AN IDLE PERIOD starts when the host says its loop is idle, the page
 *     has no task due (no timer due, no message queued; os64_dom_idle_run
 *     refuses while it has), and an idle callback is waiting: every
 *     request made until then becomes runnable, in the order made, and
 *     they run one per task, oldest first, while the period lasts. Its
 *     deadline is 50 ms after it started, or when the window's next timer
 *     is due if that is sooner, asked again at every IdleDeadline
 *     timeRemaining() (HTML's computeDeadline), so a timer a callback sets
 *     shortens the period it is in. A callback runs only before the
 *     deadline; those left wait for the next period. A request made while
 *     a period's callback runs waits for the next period, which HTML
 *     requires; and that period starts no sooner than 50 ms after the one
 *     the request was made in, which is Chromium's rule and is what keeps
 *     a callback that asks for itself again from spinning the host's loop
 *     (it runs once in each 50 ms, as in Chromium, never once a turn). A
 *     request made anywhere else lets the next period start at once.
 *   - A timeout: when the callback has not run that many milliseconds after
 *     its request, it is called anyway from a timer of the window's
 *     (d_timer_add, so the host wakes for it as for any timer, and the
 *     timeouts run in the order they fall due), its IdleDeadline's
 *     didTimeout true and timeRemaining() 0; a callback that runs first
 *     cancels its timer.
 *   - Each callback is called with `this` the window and one argument, its
 *     IdleDeadline; what it throws is reported as its script's failure and
 *     the next one still runs.
 *
 * WHAT A PAGE CAN SIZE. The entries are one array in the order they were
 * requested, so in handle order: a cancel or a timeout finds its entry by
 * binary search, an entry run or cancelled is left as a hole that the next
 * request closes up once the holes outnumber the entries (linear in the
 * entries, amortized over the requests that made the holes), and a period
 * starts by moving one index. Each entry is the binding's memory, so a
 * page's millionth request is the binding's quota (QuotaExceededError);
 * each callback is one of the registry's values. */

struct DIdle {
    JSValue callback;          /* JS_UNDEFINED: run or cancelled, a hole */
    uint32_t handle, timer, script;
};

#define IDLE_PERIOD_MS 50

static JSClassID deadline_slot;

typedef struct {
    uint64_t start_us;
    bool timed_out;
} DDeadline;

static uint64_t now_ms(const os64_dom_t *dom)
{
    return dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) : 0;
}

static uint64_t now_us(const os64_dom_t *dom)
{
    return dom->options.now_us != NULL ? dom->options.now_us(dom->options.host_opaque) : now_ms(dom) * 1000;
}

/* HTML's computeDeadline for the period that began at `start_us`, on the
 * microsecond clock, `us` being now on it: 50 ms after the period began, or
 * the next timer or rendering deadline if sooner. Those deadlines use the
 * millisecond clock, so they are placed by their distance from now. */
static uint64_t deadline_us(const os64_dom_t *dom, uint64_t start_us, uint64_t us)
{
    uint64_t deadline = start_us + IDLE_PERIOD_MS * 1000;
    uint64_t due = os64_dom_timer_next(dom);
    if (dom->idle_render_due < due)
        due = dom->idle_render_due;
    if (due != UINT64_MAX) {
        uint64_t ms = now_ms(dom);
        uint64_t at = due <= ms ? us : us + (due - ms) * 1000;
        if (at < deadline) deadline = at;
    }
    /* A callback may request a frame after the host granted this idle
     * period. Shorten that grant immediately. A frame already overdue when
     * this period began may belong to a covered window; the host's deadline
     * decides whether such a frame prevents a new idle period. */
    uint64_t frame = os64_dom_animation_next(dom);
    if (frame != UINT64_MAX) {
        uint64_t ms = now_ms(dom), at;
        if (frame >= ms) at = us + (frame - ms) * 1000;
        else at = ms - frame > us / 1000 ? 0 : us - (ms - frame) * 1000;
        if (at >= start_us && at < deadline) deadline = at;
    }
    return deadline;
}

/* ── The entries ─────────────────────────────────────────────────────── */

/* The entry with `handle`, or -1. */
static ptrdiff_t entry_find(const os64_dom_t *dom, uint32_t handle)
{
    size_t low = 0, high = dom->idle_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (dom->idle[middle].handle < handle) low = middle + 1;
        else high = middle;
    }
    return low < dom->idle_count && dom->idle[low].handle == handle ? (ptrdiff_t)low : -1;
}

/* An entry made a hole: its callback (taken into *callback, or released)
 * and its timer gone. */
static void entry_take(os64_dom_t *dom, size_t index, JSValue *callback)
{
    DIdle *entry = &dom->idle[index];
    if (callback != NULL) *callback = entry->callback;
    else JS_FreeValueRT(dom->engine, entry->callback);
    entry->callback = JS_UNDEFINED;
    dom->idle_live--;
    if (index < dom->idle_split) dom->idle_runnable--;
    if (entry->timer != 0) d_timer_cancel(dom, entry->timer);
    entry->timer = 0;
}

/* The holes closed up, the entries' order kept. */
static void compact(os64_dom_t *dom)
{
    size_t used = 0, split = 0;
    for (size_t i = 0; i < dom->idle_count; i++) {
        if (JS_IsUndefined(dom->idle[i].callback)) continue;
        if (i < dom->idle_split) split++;
        dom->idle[used++] = dom->idle[i];
    }
    dom->idle_count = used;
    dom->idle_split = split;
    dom->idle_head = 0;
}

/* Room for one more entry. False with QuotaExceededError thrown. */
static bool entry_room(os64_dom_t *dom, JSContext *ctx)
{
    if (dom->idle_count - dom->idle_live > dom->idle_live + 16) compact(dom);
    if (dom->idle_count < dom->idle_cap) return true;
    size_t cap = dom->idle_cap < 16 ? 16 : dom->idle_cap * 2;
    DIdle *grown = cap <= SIZE_MAX / sizeof(*grown) ? d_alloc(dom, cap * sizeof(*grown)) : NULL;
    if (grown == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM idle callback quota exceeded");
        return false;
    }
    if (dom->idle_count != 0) os64_memcpy(grown, dom->idle, dom->idle_count * sizeof(*grown));
    d_free(dom, dom->idle);
    dom->idle = grown;
    dom->idle_cap = cap;
    return true;
}

/* ── IdleDeadline ─────────────────────────────────────────────────────── */

static void deadline_finalizer(JSRuntime *rt, JSValue value)
{
    js_free_rt(rt, JS_GetOpaque(value, os64_js_class_id(&deadline_slot)));
}

static JSValue deadline_new(JSContext *ctx, uint64_t start_us, bool timed_out)
{
    JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&deadline_slot));
    DDeadline *deadline = JS_IsException(object) ? NULL : js_malloc(ctx, sizeof(*deadline));
    if (deadline == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    *deadline = (DDeadline){start_us, timed_out};
    JS_SetOpaque(object, deadline);
    return object;
}

enum { I_REQUEST, I_CANCEL, I_TIME_REMAINING, I_DID_TIMEOUT };

/* A callback whose timeout came first, called from its timer: its entry
 * taken, and its exception reported as its script's (the timer's own call
 * is the binding's, not the page's). */
static JSValue idle_timeout(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, data[1]) < 0) return JS_EXCEPTION;
    ptrdiff_t index = entry_find(dom, handle);
    if (index < 0 || JS_IsUndefined(dom->idle[index].callback)) return JS_UNDEFINED;
    uint32_t script = dom->idle[index].script;
    /* Its timer is the one firing, which has already left the table. */
    dom->idle[index].timer = 0;
    JSValue callback;
    entry_take(dom, (size_t)index, &callback);
    JSValue deadline = deadline_new(ctx, 0, true);
    if (JS_IsException(deadline)) {
        JS_FreeValue(ctx, callback);
        return JS_EXCEPTION;
    }
    DScripts before = d_script_enter(dom, script);
    JSValue result = JS_Call(ctx, callback, dom->window->value, 1, (JSValueConst *)&deadline);
    d_script_leave(dom, &before, false);
    JS_FreeValue(ctx, deadline);
    JS_FreeValue(ctx, callback);
    if (!JS_IsException(result)) {
        JS_FreeValue(ctx, result);
        return JS_UNDEFINED;
    }
    if (!d_exception_catchable(ctx)) return JS_EXCEPTION;
    JSValue error = JS_GetException(ctx);
    d_report_as(dom, ctx, error, script);
    JS_FreeValue(ctx, error);
    return JS_UNDEFINED;
}

static JSValue idle_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                           JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == I_TIME_REMAINING || magic == I_DID_TIMEOUT) {
        DDeadline *deadline = JS_GetOpaque(self, os64_js_class_id(&deadline_slot));
        if (deadline == NULL) return JS_ThrowTypeError(ctx, "Expected an IdleDeadline");
        if (magic == I_DID_TIMEOUT) return JS_NewBool(ctx, deadline->timed_out);
        if (deadline->timed_out) return JS_NewFloat64(ctx, 0);
        uint64_t now = now_us(dom), end = deadline_us(dom, deadline->start_us, now);
        return JS_NewFloat64(ctx, end > now ? (double)(end - now) / 1000.0 : 0);
    }
    if (magic == I_CANCEL) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "cancelIdleCallback needs a handle");
        uint32_t handle;
        if (JS_ToUint32(ctx, &handle, argv[0]) < 0) return JS_EXCEPTION;
        ptrdiff_t index = entry_find(dom, handle);
        if (index >= 0 && !JS_IsUndefined(dom->idle[index].callback)) entry_take(dom, (size_t)index, NULL);
        return JS_UNDEFINED;
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "requestIdleCallback needs a function");
    /* IdleRequestOptions' timeout, an unsigned long, converted before the
     * request is made: its conversion may run a page's code. */
    uint32_t timeout = 0;
    int init = d_dictionary(ctx, argc, argv, 1);
    if (init < 0) return JS_EXCEPTION;
    if (init > 0) {
        JSValue value = JS_GetPropertyStr(ctx, argv[1], "timeout");
        int converted = JS_IsException(value) ? -1 : JS_IsUndefined(value) ? 0 : JS_ToUint32(ctx, &timeout, value);
        JS_FreeValue(ctx, value);
        if (converted < 0) return JS_EXCEPTION;
    }
    if (dom->idle_handle == UINT32_MAX) return d_error(ctx, "QuotaExceededError", "Too many idle callbacks");
    uint32_t script = d_callback_script(dom, ctx, argv[0]);
    if (script == D_SCRIPT_FAILED || !entry_room(dom, ctx)) return JS_EXCEPTION;
    uint32_t handle = dom->idle_handle + 1, timer = 0;
    if (timeout > 0) {
        JSValue held[2] = {dom->anchor->value, JS_NewUint32(ctx, handle)};
        JSValue call = JS_NewCFunctionData(ctx, idle_timeout, 0, 0, 2, held);
        if (JS_IsException(call)) return JS_EXCEPTION;
        timer = d_timer_add(dom, ctx, call, timeout);
        if (timer == 0) return JS_EXCEPTION;
    }
    dom->idle_handle = handle;
    dom->idle[dom->idle_count++] =
        (DIdle){JS_DupValue(ctx, argv[0]), handle, timer, script};
    dom->idle_live++;
    /* One a period's callback asks for waits for the next period, 50 ms
     * after this one began; any other lets the next begin at once. */
    if (dom->idle_running) dom->idle_rested_ms = dom->idle_start_ms + IDLE_PERIOD_MS;
    else dom->idle_fresh = true;
    return JS_NewUint32(ctx, handle);
}

/* ── The host's side ──────────────────────────────────────────────────── */

/* Whether the open period can still run its next callback. */
static bool period_runs(const os64_dom_t *dom)
{
    if (!dom->idle_open || dom->idle_runnable == 0) return false;
    uint64_t us = now_us(dom);
    return us < deadline_us(dom, dom->idle_start_us, us);
}

uint64_t os64_dom_idle_next(const os64_dom_t *dom)
{
    if (dom == NULL || dom->closed || dom->idle_live == 0) return UINT64_MAX;
    /* Runnable callbacks run in this period or, past its deadline, start
     * the next at once. */
    if (dom->idle_runnable != 0 || dom->idle_fresh) return 0;
    return dom->idle_rested_ms;
}

bool os64_dom_idle_run(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    return os64_dom_idle_before(dom, UINT64_MAX, outcome);
}

bool os64_dom_idle_before(os64_dom_t *dom, uint64_t render_due, os64_js_outcome_t *outcome)
{
    if (outcome != NULL) os64_memset(outcome, 0, sizeof(*outcome));
    if (outcome == NULL || dom == NULL || dom->closed || dom->idle_live == 0) return false;
    dom->idle_render_due = render_due;
    uint64_t ms = now_ms(dom);
    if (render_due <= ms) return false;
    /* The page's own work first: an idle callback waits for its tasks. */
    if (os64_dom_timer_next(dom) <= ms || dom->messages != NULL) return false;
    if (!period_runs(dom)) {
        /* The open period is over: the next starts when it may (above). */
        dom->idle_open = false;
        if (dom->idle_runnable == 0 && !dom->idle_fresh && ms < dom->idle_rested_ms) return false;
        dom->idle_open = true;
        dom->idle_start_ms = ms;
        dom->idle_start_us = now_us(dom);
        dom->idle_split = dom->idle_count;
        dom->idle_runnable = dom->idle_live;
        dom->idle_fresh = false;
        dom->idle_rested_ms = 0;
        if (!period_runs(dom)) return false;
    }
    while (JS_IsUndefined(dom->idle[dom->idle_head].callback)) dom->idle_head++;
    size_t index = dom->idle_head++;
    uint32_t handle = dom->idle[index].handle, script = dom->idle[index].script;
    JSValue callback;
    entry_take(dom, index, &callback);
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#idle-%u", (int)(sizeof(name) - 24),
                  dom->url != NULL ? dom->url : "about:blank", handle);
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, name, outcome) == OS64_JS_OK) {
        os64_js_outcome_t access, call;
        JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
        JSValue deadline = ctx != NULL ? deadline_new(ctx, dom->idle_start_us, false) : JS_EXCEPTION;
        if (!JS_IsException(deadline)) {
            /* What the callback throws is its script's (report.c). */
            dom->idle_running = true;
            DScripts before = d_script_enter(dom, script);
            os64_js_call(dom->runtime, OS64_JS_ABI_ID, callback, dom->window->value, 1, &deadline, NULL, &call);
            d_script_leave(dom, &before, true);
            dom->idle_running = false;
            JS_FreeValue(ctx, deadline);
            d_outcome_fold(outcome, &call);
            if (call.status == OS64_JS_EXCEPTION) d_errors_fire(dom, outcome);
        }
        d_task_finish(dom, outcome);
    }
    JS_FreeValueRT(dom->engine, callback);
    d_task_close(dom, name, outcome);
    return true;
}

void d_idle_drain(os64_dom_t *dom)
{
    for (size_t i = 0; i < dom->idle_count; i++) {
        JS_FreeValueRT(dom->engine, dom->idle[i].callback);
        dom->idle[i].callback = JS_UNDEFINED;
    }
    dom->idle_live = dom->idle_runnable = 0;
}

void d_idle_free(os64_dom_t *dom)
{
    d_free(dom, dom->idle);
    dom->idle = NULL;
    dom->idle_count = dom->idle_cap = 0;
}

int d_idle_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    dom->idle_render_due = UINT64_MAX;
    static const JSClassDef deadline_class = {.class_name = "IdleDeadline", .finalizer = deadline_finalizer};
    JSValue prototype;
    if (d_interface_class(dom, ctx, global, os64_js_class_id(&deadline_slot), &deadline_class, &prototype) < 0 ||
        d_method_named(dom, ctx, prototype, "timeRemaining", idle_member, 0, I_TIME_REMAINING) < 0 ||
        d_accessor(dom, ctx, prototype, "didTimeout", idle_member, I_DID_TIMEOUT, false) < 0 ||
        d_method_named(dom, ctx, global, "requestIdleCallback", idle_member, 1, I_REQUEST) < 0)
        return -1;
    return d_method_named(dom, ctx, global, "cancelIdleCallback", idle_member, 1, I_CANCEL);
}
