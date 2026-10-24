#include "internal.h"
#include "os64/fmt.h"

/* A SCRIPT'S FAILURES (MODERN_WEB.md § P2a; HTML § 8.1.4.6, runtime script
 * errors, and § 8.1.4.7, unhandled promise rejections). The binding is the
 * runtime's script host (libjs CONTRACT.md § A script host): the wrapper
 * hands it every exception it catches and leaves it the rejections a
 * checkpoint left unhandled, and the binding fires the window's error,
 * unhandledrejection and rejectionhandled events for them, inside the task
 * they happened in, each through one function (fire) called with
 * os64_js_call, so a listener runs under the wrapper's guard and the task's
 * deadline as every other listener does.
 *
 * WHEN. An exception the binding caught itself while a script was running
 * (a nested dispatch's listener, a handler that failed to compile,
 * reportError) has its error event at once, as HTML's "report an
 * exception" does. One the wrapper caught has it at the task's next turn
 * between calls: after the listener that threw and before its checkpoint,
 * or after the checkpoint whose job threw. A rejection is unhandled when a
 * checkpoint ends with it unhandled, and its unhandledrejection is fired
 * after that checkpoint, in the same task, each followed by a checkpoint
 * of its own. A reported promise a handler reaches later has its
 * rejectionhandled after the checkpoint that follows the handler. HTML
 * queues a task for each of the last two, so there a task the page queued
 * first may run before them; here none can, as nothing runs between a
 * checkpoint and the end of the task it is in.
 *
 * THE RECORD. A rejection no unhandledrejection listener cancelled is the
 * task's failure, UNHANDLED_REJECTION with "unhandled rejection: <reason>",
 * which the host writes as a FAILED line: the first a task meets is its
 * outcome, unless the task already failed, and each after that goes to
 * options.failed. With the switch off no event is fired and every
 * unhandled rejection is one. An exception's outcome is the wrapper's
 * diagnostic, whatever an error listener did with its event.
 *
 * MUTED ERRORS. What another site's script threw is told to the page as
 * "Script error." and nothing more (error_event), and its rejections fire
 * no event (unhandled), so no page reads another site's code through them.
 * The record keeps them whole: it is the person's, not the page's. */

struct DPending {
    struct DPending *next;
    JSValue value;
};

static bool sticky(os64_js_status_t status)
{
    return status == OS64_JS_LIMIT || status == OS64_JS_CANCELLED || status == OS64_JS_HOST_FAILURE ||
        status == OS64_JS_FAILED_RUNTIME;
}

/* An entry's outcome into the task's: what it says of the script, never a
 * refusal of the entry itself (a runtime that failed earlier is already
 * the task's failure, and task_end reports how). */
static void fold(os64_js_outcome_t *into, const os64_js_outcome_t *from)
{
    if (from->status == OS64_JS_FAILED_RUNTIME || from->status == OS64_JS_BAD_ARGUMENT ||
        from->status == OS64_JS_BUSY || from->status == OS64_JS_ABI_MISMATCH) return;
    d_outcome_fold(into, from);
}

static void discard(JSContext *ctx)
{
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
}

/* A value kept for later, on one of the two lists. Past the binding's
 * budget it is not kept, and its event is not fired. */
static void pending_push(os64_dom_t *dom, JSContext *ctx, JSValueConst value, bool error)
{
    struct DPending *item = d_alloc(dom, sizeof(*item));
    if (item == NULL) return;
    item->value = JS_DupValue(ctx, value);
    dom->pending_held++;
    struct DPending **head = error ? &dom->errors : &dom->handled;
    struct DPending **last = error ? &dom->errors_last : &dom->handled_last;
    if (*last != NULL) (*last)->next = item;
    else *head = item;
    *last = item;
}

static JSValue pending_pop(os64_dom_t *dom, bool error)
{
    struct DPending **head = error ? &dom->errors : &dom->handled;
    struct DPending *item = *head;
    *head = item->next;
    if (*head == NULL) *(error ? &dom->errors_last : &dom->handled_last) = NULL;
    JSValue value = item->value;
    d_free(dom, item);
    dom->pending_held--;
    return value;
}

/* THE SCRIPT HOST'S HOOKS, called inside the wrapper's guard: each keeps
 * the value and runs nothing. */
static void host_exception(void *opaque, JSContext *ctx, JSValue error)
{
    os64_dom_t *dom = opaque;
    if (dom->closed || !dom->options.modern || dom->reporting_error) return;
    pending_push(dom, ctx, error, true);
}

static void host_handled(void *opaque, JSContext *ctx, JSValue promise)
{
    os64_dom_t *dom = opaque;
    if (dom->closed || !dom->options.modern) return;
    if (JS_VALUE_GET_PTR(promise) == dom->rejection_firing) dom->rejection_handled = true;
    else pending_push(dom, ctx, promise, false);
}

bool d_report_host(os64_dom_t *dom)
{
    const os64_js_script_host_t host = {dom, host_exception, host_handled};
    os64_js_outcome_t outcome;
    return os64_js_set_script_host(dom->runtime, OS64_JS_ABI_ID, &host, &outcome) == OS64_JS_OK;
}

void d_report_drain(os64_dom_t *dom)
{
    os64_js_outcome_t outcome;
    (void)os64_js_set_script_host(dom->runtime, OS64_JS_ABI_ID, NULL, &outcome);
    while (dom->errors != NULL) JS_FreeValueRT(dom->engine, pending_pop(dom, true));
    while (dom->handled != NULL) JS_FreeValueRT(dom->engine, pending_pop(dom, false));
}

/* A string-valued data property, the object's own or its nearest
 * prototype's, found without running anything: the walk goes only through
 * ordinary objects and errors (a proxy's trap is the page's code), stops at
 * an accessor, and gives up after a few levels. NULL when there is none. */
static const char *data_string(JSContext *ctx, JSValueConst object, const char *name)
{
    JSValue probes[2] = {JS_NewObject(ctx), JS_NewError(ctx)};
    JSClassID plain = JS_GetClassID(probes[0]), error = JS_GetClassID(probes[1]);
    bool made = !JS_IsException(probes[0]) && !JS_IsException(probes[1]);
    JS_FreeValue(ctx, probes[0]);
    JS_FreeValue(ctx, probes[1]);
    if (!made) { discard(ctx); return NULL; }
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) { discard(ctx); return NULL; }
    const char *text = NULL;
    JSValue at = JS_DupValue(ctx, object);
    for (int level = 0; level < 8 && JS_IsObject(at); level++) {
        JSClassID id = JS_GetClassID(at);
        if (id != plain && id != error) break;
        JSPropertyDescriptor descriptor;
        int present = JS_GetOwnProperty(ctx, &descriptor, at, atom);
        if (present < 0) { discard(ctx); break; }
        if (present > 0) {
            if (!(descriptor.flags & JS_PROP_GETSET) && JS_IsString(descriptor.value))
                text = JS_ToCString(ctx, descriptor.value);
            JS_FreeValue(ctx, descriptor.value);
            JS_FreeValue(ctx, descriptor.getter);
            JS_FreeValue(ctx, descriptor.setter);
            break;
        }
        JSValue next = JS_GetPrototype(ctx, at);
        JS_FreeValue(ctx, at);
        at = next;
        if (JS_IsException(at)) { discard(ctx); at = JS_UNDEFINED; }
    }
    JS_FreeValue(ctx, at);
    JS_FreeAtom(ctx, atom);
    return text;
}

static void name_and_message(char *out, size_t cap, const char *name, const char *message)
{
    if (message != NULL && message[0] != '\0')
        os64_snprintf(out, cap, "%s: %s", name != NULL ? name : "Error", message);
    else os64_strcopy(out, cap, name != NULL ? name : "Error");
}

void d_error_text(os64_dom_t *dom, JSContext *ctx, JSValueConst value, char *out, size_t cap)
{
    (void)dom;
    const char *name, *message;
    if (d_dom_exception_fields(value, &name, &message)) {
        name_and_message(out, cap, name, message);
        return;
    }
    os64_strcopy(out, cap, JS_IsObject(value) ? "an object that is not an Error" : "a symbol");
    if (JS_IsSymbol(value)) return;
    if (!JS_IsObject(value)) {
        const char *text = JS_ToCString(ctx, value);
        if (text != NULL) {
            os64_strcopy(out, cap, text);
            JS_FreeCString(ctx, text);
        } else discard(ctx);
        return;
    }
    /* An error, or an object that carries a message as one does (a
     * library's own error type, made without the Error constructor). */
    const char *own_name = data_string(ctx, value, "name");
    const char *own_message = data_string(ctx, value, "message");
    if (JS_IsError(ctx, value)) name_and_message(out, cap, own_name, own_message);
    else if (own_message != NULL) {
        if (own_name != NULL) name_and_message(out, cap, own_name, own_message);
        else os64_strcopy(out, cap, own_message);
    }
    if (own_name != NULL) JS_FreeCString(ctx, own_name);
    if (own_message != NULL) JS_FreeCString(ctx, own_message);
}

/* The number before position `end` of `text`, read backwards and capped,
 * and where it starts; false when there is no digit there. */
static bool number_before(const char *text, const char *end, const char **start, uint32_t *value)
{
    const char *at = end;
    while (at > text && at[-1] >= '0' && at[-1] <= '9') at--;
    if (at == end) return false;
    uint64_t n = 0;
    for (const char *p = at; p < end; p++) n = n < UINT32_MAX ? n * 10 + (uint64_t)(*p - '0') : n;
    *value = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    *start = at;
    return true;
}

/* Where an error was made, from its own stack text as the engine writes it,
 * a frame a line, "    at f (name:line:col)", or "    at name:line:col" for
 * a script that did not compile: the first frame that names a place, which
 * skips the native ones; and that frame's name, the source name the script
 * was compiled under (`file`, empty when there is none), which for a `src`
 * script is its address. Only an Error's is read, and only as a data
 * property, so no page code runs; a stack the page wrote there is read the
 * same way and believed: it is the page's own error. */
static void error_location(JSContext *ctx, JSValueConst error, uint32_t *line, uint32_t *col, char *file,
                           size_t cap)
{
    *line = *col = 0;
    if (cap != 0) file[0] = '\0';
    if (!JS_IsError(ctx, error)) return;
    JSAtom atom = JS_NewAtom(ctx, "stack");
    if (atom == JS_ATOM_NULL) { discard(ctx); return; }
    JSPropertyDescriptor descriptor;
    int present = JS_GetOwnProperty(ctx, &descriptor, error, atom);
    JS_FreeAtom(ctx, atom);
    if (present < 0) { discard(ctx); return; }
    if (present == 0) return;
    const char *text = !(descriptor.flags & JS_PROP_GETSET) && JS_IsString(descriptor.value)
        ? JS_ToCString(ctx, descriptor.value) : NULL;
    JS_FreeValue(ctx, descriptor.value);
    JS_FreeValue(ctx, descriptor.getter);
    JS_FreeValue(ctx, descriptor.setter);
    if (text == NULL) return;
    for (const char *start = text; *start != '\0';) {
        const char *end = start;
        while (*end != '\0' && *end != '\n') end++;
        const char *at, *last = end > start && end[-1] == ')' ? end - 1 : end;
        uint32_t l, c;
        if (number_before(start, last, &at, &c) && at > start && at[-1] == ':' &&
            number_before(start, at - 1, &at, &l) && at > start && at[-1] == ':') {
            *line = l;
            *col = c;
            /* The name runs back to the frame's last '(', or, with none,
             * to "at ": a name holding a '(' of its own is read short. */
            const char *name_end = at - 1, *name = name_end;
            while (name > start && name[-1] != '(') name--;
            if (name == start) {
                for (const char *p = start; p + 3 <= name_end; p++)
                    if (p[0] == 'a' && p[1] == 't' && p[2] == ' ') { name = p + 3; break; }
            }
            size_t n = (size_t)(name_end - name);
            if (cap != 0) {
                if (n >= cap) n = cap - 1;
                os64_memcpy(file, name, n);
                file[n] = '\0';
            }
            break;
        }
        start = *end == '\n' ? end + 1 : end;
    }
    JS_FreeCString(ctx, text);
}

/* Where the running script is, for a value thrown with no stack of its own:
 * the place an Error made now says it was made, which is the nearest
 * script frame (the engine leaves out the constructor's own, and a
 * binding's native function has none). */
static void here_location(os64_dom_t *dom, JSContext *ctx, uint32_t *line, uint32_t *col)
{
    JSValue here = JS_CallConstructor(ctx, dom->error_constructor->value, 0, NULL);
    if (JS_IsException(here)) { discard(ctx); return; }
    error_location(ctx, here, line, col, NULL, 0);
    JS_FreeValue(ctx, here);
}

static bool foreign(const os64_dom_t *dom, const char *address);

/* HTML's muted errors: what a script from another origin threw is told to
 * the page as "Script error." and nothing else, so no page reads another
 * site's script's failure. `muted` says the script that ran was one; an
 * Error made in another site's code (its first placed frame names a
 * script of another origin, as a listener or a function that script
 * defined does) is muted wherever it was caught. */
static JSValue error_event(os64_dom_t *dom, JSContext *ctx, JSValueConst error, const char *filename, bool running,
                           bool muted)
{
    uint32_t line, col;
    char file[OS64_JS_SOURCE_NAME_CAP];
    error_location(ctx, error, &line, &col, file, sizeof(file));
    if (muted || (file[0] != '\0' && foreign(dom, file)))
        return d_error_event_new(dom, ctx, "Script error.", "", 0, 0, JS_NULL);
    char text[OS64_JS_MESSAGE_CAP], message[OS64_JS_MESSAGE_CAP + 16];
    d_error_text(dom, ctx, error, text, sizeof(text));
    os64_snprintf(message, sizeof(message), "Uncaught %s", text);
    if (line == 0 && running) here_location(dom, ctx, &line, &col);
    const char *page = dom->url != NULL ? dom->url : "about:blank";
    return d_error_event_new(dom, ctx, message, filename != NULL ? filename : page, line, col, error);
}

/* Fires `event` at window inside the open task, through os64_js_call:
 * whether a listener cancelled it. */
static bool fire_event(os64_dom_t *dom, JSValue event, os64_js_outcome_t *outcome)
{
    bool cancelled = false;
    os64_js_outcome_t call;
    os64_js_call(dom->runtime, OS64_JS_ABI_ID, dom->report_fire->value, JS_UNDEFINED, 1, &event, &cancelled, &call);
    fold(outcome, &call);
    return cancelled;
}

/* The function fire_event calls: the event dispatched at window, answering
 * false when it was cancelled. */
static JSValue fire(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    int result = argc > 0 ? d_dispatch(dom, ctx, argv[0], NULL) : -1;
    if (result < 0) return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Nothing to fire");
    return JS_NewBool(ctx, result != 0);
}

void d_report_error(os64_dom_t *dom, JSContext *ctx, JSValueConst error)
{
    if (dom->closed || !dom->options.modern || dom->reporting_error || dom->error_event_prototype == NULL) return;
    os64_js_outcome_t probe;
    os64_js_status_t running = os64_js_check_budget(dom->runtime, OS64_JS_ABI_ID, &probe);
    if (running == OS64_JS_BAD_ARGUMENT) {
        pending_push(dom, ctx, error, true);
        return;
    }
    if (running != OS64_JS_OK || !os64_dom_listens(dom, "error")) return;
    JSValue event = error_event(dom, ctx, error, NULL, true, false);
    if (JS_IsException(event)) { discard(ctx); return; }
    dom->reporting_error = true;
    int result = d_dispatch(dom, ctx, event, NULL);
    dom->reporting_error = false;
    JS_FreeValue(ctx, event);
    /* The callers carry on as after any reported error; a runtime that
     * failed under the dispatch fails their next step again. */
    if (result < 0 && JS_HasException(ctx)) discard(ctx);
}

static void errors_fire(os64_dom_t *dom, const char *filename, bool muted, os64_js_outcome_t *outcome)
{
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    while (dom->errors != NULL) {
        JSValue error = pending_pop(dom, true);
        if (ctx != NULL && !sticky(outcome->status) && os64_dom_listens(dom, "error")) {
            JSValue event = error_event(dom, ctx, error, filename, false, muted);
            if (JS_IsException(event)) discard(ctx);
            else {
                dom->reporting_error = true;
                fire_event(dom, event, outcome);
                dom->reporting_error = false;
                JS_FreeValue(ctx, event);
            }
        }
        JS_FreeValueRT(dom->engine, error);
    }
}

void d_errors_fire(os64_dom_t *dom, const char *filename, os64_js_outcome_t *outcome)
{
    errors_fire(dom, filename, false, outcome);
}

/* One rejection the runtime handed over: its unhandledrejection, the
 * task's failure unless a listener cancelled it, and, unless a handler
 * reached it during the event, its place among the promises a later
 * handler names (host_handled). One a script from another origin left
 * (`muted`), or whose reason is an Error made in such a script's code, is
 * the task's failure and no page's event, as HTML's rejection tracker
 * ignores a muted script's. */
static void unhandled(os64_dom_t *dom, JSValue promise, JSValue reason, const char *task, bool muted,
                      os64_js_outcome_t *outcome)
{
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    bool cancelled = false;
    if (ctx != NULL && !muted) {
        /* A reason made in another site's code is that site's, as an
         * error is (error_event). */
        uint32_t line, col;
        char file[OS64_JS_SOURCE_NAME_CAP];
        error_location(ctx, reason, &line, &col, file, sizeof(file));
        muted = file[0] != '\0' && foreign(dom, file);
    }
    if (ctx != NULL && !muted && dom->options.modern && dom->rejection_event_prototype != NULL &&
        !sticky(outcome->status) && os64_dom_listens(dom, "unhandledrejection")) {
        JSValue event = d_rejection_event_new(dom, ctx, "unhandledrejection", promise, reason, true);
        if (JS_IsException(event)) discard(ctx);
        else {
            dom->rejection_firing = JS_VALUE_GET_PTR(promise);
            dom->rejection_handled = false;
            cancelled = fire_event(dom, event, outcome);
            dom->rejection_firing = NULL;
            JS_FreeValue(ctx, event);
        }
    }
    if (ctx != NULL && !cancelled) {
        os64_js_outcome_t failure = {.status = OS64_JS_UNHANDLED_REJECTION};
        char text[OS64_JS_MESSAGE_CAP];
        d_error_text(dom, ctx, reason, text, sizeof(text));
        failure.diagnostic_truncated =
            (size_t)os64_snprintf(failure.message, sizeof(failure.message), "unhandled rejection: %s", text) >=
            sizeof(failure.message);
        os64_strcopy(failure.source_name, sizeof(failure.source_name), task);
        if (outcome->status == OS64_JS_OK) d_outcome_fold(outcome, &failure);
        else if (!sticky(outcome->status) && dom->options.failed != NULL)
            dom->options.failed(dom->options.host_opaque, task, failure.message);
    }
    JS_FreeValueRT(dom->engine, promise);
    JS_FreeValueRT(dom->engine, reason);
}

/* The rejectionhandled each promise a handler reached is owed: true when
 * there was one. */
static bool handled_fire(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    bool any = dom->handled != NULL;
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    while (dom->handled != NULL) {
        JSValue promise = pending_pop(dom, false);
        if (ctx != NULL && !sticky(outcome->status) && os64_dom_listens(dom, "rejectionhandled")) {
            JSValue reason = JS_PromiseResult(ctx, promise);
            JSValue event = JS_IsException(reason) ? JS_EXCEPTION
                : d_rejection_event_new(dom, ctx, "rejectionhandled", promise, reason, false);
            JS_FreeValue(ctx, reason);
            if (JS_IsException(event)) discard(ctx);
            else {
                fire_event(dom, event, outcome);
                JS_FreeValue(ctx, event);
            }
        }
        JS_FreeValueRT(dom->engine, promise);
    }
    return any;
}

void d_task_settle(os64_dom_t *dom, const char *filename, os64_js_outcome_t *outcome)
{
    for (;;) {
        if (sticky(outcome->status)) return;
        os64_js_outcome_t point;
        os64_js_status_t checked = os64_js_checkpoint(dom->runtime, OS64_JS_ABI_ID, &point);
        fold(outcome, &point);
        if (checked != OS64_JS_OK && checked != OS64_JS_EXCEPTION) return;
        bool owed = dom->errors != NULL;
        d_errors_fire(dom, filename, outcome);
        JSValue promise, reason;
        while (!sticky(outcome->status) &&
               os64_js_take_rejection(dom->runtime, OS64_JS_ABI_ID, &promise, &reason)) {
            owed = true;
            unhandled(dom, promise, reason, point.source_name, false, outcome);
        }
        if (handled_fire(dom, outcome)) owed = true;
        if (!owed) return;
    }
}

/* Whether a script came from another origin than the page's, which HTML's
 * muted errors are for: a classic script fetched without CORS, as the host
 * fetches every script. Only a name that is an address with a tuple origin
 * can be another site's script: an opaque one (data:), and a name that is
 * no address at all (a handler attribute's, one the address parser
 * refuses), is not. Against a page whose own origin cannot be read, such
 * an address is taken to be another site's. */
static bool origin_of(const char *address, char *out, size_t cap)
{
    /* The query and the fragment name no part of an origin, and the
     * address parser may refuse one (a space the host left unescaped). */
    char head[OS64_DOM_URL_MAX];
    size_t n = 0;
    while (address[n] != '\0' && address[n] != '?' && address[n] != '#' && n + 1 < sizeof(head)) {
        head[n] = address[n];
        n++;
    }
    head[n] = '\0';
    return d_origin(head, out, cap);
}

static bool foreign(const os64_dom_t *dom, const char *address)
{
    char script[OS64_DOM_URL_MAX], page[OS64_DOM_URL_MAX];
    if (!origin_of(address, script, sizeof(script)) || os64_streq(script, "null")) return false;
    return !origin_of(dom->url != NULL ? dom->url : "about:blank", page, sizeof(page)) || !os64_streq(page, script);
}

void d_script_settle(os64_dom_t *dom, const char *name, const char *filename, os64_js_outcome_t *outcome)
{
    JSValue promise, reason;
    bool taken = os64_js_take_rejection(dom->runtime, OS64_JS_ABI_ID, &promise, &reason);
    if (!taken && dom->errors == NULL && dom->handled == NULL) return;
    /* The bracket's failures go after the script's own, so a rejection
     * after a script that threw is told to the host (options.failed). */
    os64_js_outcome_t begin, task = *outcome;
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, name, &begin) != OS64_JS_OK) {
        if (taken) {
            JS_FreeValueRT(dom->engine, promise);
            JS_FreeValueRT(dom->engine, reason);
        }
        return;
    }
    /* The script's own exception came before the checkpoint that left the
     * rejection; both are said to come from where the script came from. A
     * script from another origin has its failures muted: every rejection it
     * left is taken first, firing nothing, so none is left for the page's
     * listeners after its error event. */
    bool muted = filename != NULL && foreign(dom, filename);
    if (muted) {
        for (bool more = taken; more; more = os64_js_take_rejection(dom->runtime, OS64_JS_ABI_ID, &promise, &reason))
            unhandled(dom, promise, reason, name, true, &task);
        errors_fire(dom, filename, true, &task);
    } else {
        errors_fire(dom, filename, false, &task);
        if (taken) unhandled(dom, promise, reason, name, false, &task);
    }
    d_task_finish(dom, &task);
    *outcome = task;
}

bool os64_dom_script_ended(os64_dom_t *dom, const char *name, const char *address, os64_js_outcome_t *outcome)
{
    if (dom == NULL || dom->closed || name == NULL || outcome == NULL) return false;
    d_script_settle(dom, name, address, outcome);
    return true;
}

static JSValue report_error(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "reportError needs the error to report");
    d_report(dom, ctx, argv[0]);
    return JS_UNDEFINED;
}

int d_report_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    dom->report_fire = d_retain(dom, ctx, JS_NewCFunctionData(ctx, fire, 1, 0, 1, &dom->anchor->value));
    dom->error_constructor = dom->report_fire != NULL ? d_retain(dom, ctx, JS_GetPropertyStr(ctx, global, "Error"))
                                                      : NULL;
    if (dom->error_constructor == NULL) return -1;
    return d_error_events_install(dom, ctx, global) < 0 ||
        d_event_handler(dom, ctx, global, D_EVENT_ERROR) < 0 ||
        d_event_handler(dom, ctx, global, D_EVENT_UNHANDLEDREJECTION) < 0 ||
        d_event_handler(dom, ctx, global, D_EVENT_REJECTIONHANDLED) < 0 ||
        d_event_handler(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, D_EVENT_ERROR) < 0 ||
        d_method_named(dom, ctx, global, "reportError", report_error, 1, 0) < 0 ? -1 : 0;
}
