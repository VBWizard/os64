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
 * between calls: after the listener or timer callback that threw and
 * before its checkpoint, or after the checkpoint whose job threw. A script
 * the host ran itself is the exception to that order: the wrapper opens no
 * task while jobs wait, so the jobs a script that threw had queued run
 * before its error event, where HTML fires the event first (DEBTS.md, "A
 * script that threw has its microtasks run before its error event"; a
 * string timer the same). A rejection is unhandled when a
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
 * outcome, unless the task already failed, and those after it go to
 * options.failed, sixteen one by one and the rest counted into one line
 * (D_REJECTIONS_TOLD). With the switch off no event is fired and every
 * unhandled rejection is one. An exception's outcome is the wrapper's
 * diagnostic, whatever an error listener did with its event.
 *
 * MUTED ERRORS. What another site's script threw or reported is told to
 * the page as "Script error." and nothing more (error_event), and its
 * rejections fire no event, not even a rejectionhandled when a handler
 * reaches one later (unhandled, handled_fire), so no page reads another
 * site's code through them. Whose failure it is is decided one way: by
 * the script that was running when the value was thrown, rejected or
 * reported, as the binding knows each script, by the name it was compiled
 * under (THE SCRIPTS' NAMES below), never by an address or a stack's text.
 * A rejection and reportError are asked of the engine's stack as they
 * happen (running_script). An exception has left its frames by the time
 * it is caught, so it is the script being run, or the one whose callback
 * the binding called: a listener, a timer, a queueMicrotask job, each
 * callback's script asked as it was handed over (d_callback_script). So a
 * function of another site's that the page's own code calls, and that
 * throws through it, is the page's failure, as HTML's "run a classic
 * script" reports it; a rejection that function makes is the other site's.
 * The record keeps them whole: it is the person's, not the page's. */

struct DPending {
    struct DPending *next;
    JSValue value;
    uint32_t script;        // an exception's: the script whose it was
};

/* How many rejections past a task's first failure are each told to the host
 * as a line of their own (options.failed). The rest are only counted, and
 * told as one line when the task closes (d_task_close): a page can leave
 * hundreds of thousands in one task with no script running while they are
 * taken, so what each costs the host must not be what a page sizes, and
 * sixteen lines say what kind of failure it was as well as all of them. */
#define D_REJECTIONS_TOLD 16

static bool sticky(os64_js_status_t status);

void d_task_close(os64_dom_t *dom, const char *task, os64_js_outcome_t *outcome)
{
    bool tell = !sticky(outcome->status) && dom->options.failed != NULL;
    /* The binding's own report (d_report's first error) is this task's,
     * whatever its outcome: the outcome when that is OK, under the task's
     * name, else a line told to the host; it never stands to be taken by
     * the next task under that one's name. */
    if (dom->report.status != OS64_JS_OK) {
        if (outcome->status == OS64_JS_OK) {
            *outcome = dom->report;
            if (task != NULL && task[0] != '\0')
                os64_strcopy(outcome->source_name, sizeof(outcome->source_name), task);
        } else if (tell) {
            dom->options.failed(dom->options.host_opaque, task != NULL ? task : "", dom->report.message);
        }
        os64_memset(&dom->report, 0, sizeof(dom->report));
    }
    if (dom->rejections_untold != 0 && tell) {
        char line[96];
        os64_snprintf(line, sizeof(line), "and %llu more unhandled rejections",
                      (unsigned long long)dom->rejections_untold);
        dom->options.failed(dom->options.host_opaque, task != NULL ? task : "", line);
    }
    dom->rejections_told = 0;
    dom->rejections_untold = 0;
    dom->task_script = dom->thrown_script = D_NO_SCRIPT;
}

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
static void pending_push(os64_dom_t *dom, JSContext *ctx, JSValueConst value, bool error, uint32_t script)
{
    struct DPending *item = d_alloc(dom, sizeof(*item));
    if (item == NULL) return;
    item->value = JS_DupValue(ctx, value);
    item->script = script;
    dom->pending_held++;
    struct DPending **head = error ? &dom->errors : &dom->handled;
    struct DPending **last = error ? &dom->errors_last : &dom->handled_last;
    if (*last != NULL) (*last)->next = item;
    else *head = item;
    *last = item;
}

static JSValue pending_pop(os64_dom_t *dom, bool error, uint32_t *script)
{
    struct DPending **head = error ? &dom->errors : &dom->handled;
    struct DPending *item = *head;
    *head = item->next;
    if (*head == NULL) *(error ? &dom->errors_last : &dom->handled_last) = NULL;
    JSValue value = item->value;
    if (script != NULL) *script = item->script;
    d_free(dom, item);
    dom->pending_held--;
    return value;
}

/* THE SCRIPTS' NAMES (the MUTED ERRORS paragraph above). A script's name in
 * the engine is the binding's, `os64-script:<n>`, never one the page or the
 * address gives, so a frame or a function names its script by a lookup and
 * nothing is read back out of text a page can shape. */
#define D_SCRIPT_PREFIX "os64-script:"
#define D_SCRIPT_FOREIGN 1u

static bool foreign(const os64_dom_t *dom, const char *address);

/* A record for `address` (NULL: the page's own), its number or D_NO_SCRIPT
 * when the binding's budget refused it. */
static uint32_t script_record(os64_dom_t *dom, const char *address, bool muted)
{
    if (dom->script_name_count == dom->script_name_cap) {
        uint32_t cap = dom->script_name_cap != 0 ? dom->script_name_cap * 2 : 8;
        struct DScriptName *grown = d_alloc(dom, (size_t)cap * sizeof(*grown));
        if (grown == NULL) return D_NO_SCRIPT;
        if (dom->script_name_count != 0)
            os64_memcpy(grown, dom->script_names, (size_t)dom->script_name_count * sizeof(*grown));
        d_free(dom, dom->script_names);
        dom->script_names = grown;
        dom->script_name_cap = cap;
    }
    char *copy = NULL;
    if (address != NULL) {
        size_t n = os64_strlen(address) + 1;
        copy = d_alloc(dom, n);
        if (copy == NULL) return D_NO_SCRIPT;
        os64_memcpy(copy, address, n);
    }
    dom->script_names[dom->script_name_count] = (struct DScriptName){copy, muted};
    return dom->script_name_count++;
}

/* The number a name of the binding's names, or D_NO_SCRIPT for any other
 * name (a handler attribute's, code a page compiled with eval or Function). */
static uint32_t script_named(const os64_dom_t *dom, const char *name)
{
    size_t prefix = sizeof(D_SCRIPT_PREFIX) - 1;
    if (name == NULL || os64_memcmp(name, D_SCRIPT_PREFIX, prefix) != 0 || name[prefix] == '\0')
        return D_NO_SCRIPT;
    uint64_t n = 0;
    for (const char *p = name + prefix; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || n > UINT32_MAX) return D_NO_SCRIPT;
        n = n * 10 + (uint64_t)(*p - '0');
    }
    return n < dom->script_name_count ? (uint32_t)n : D_NO_SCRIPT;
}

static bool script_muted(const os64_dom_t *dom, uint32_t script)
{
    return script < dom->script_name_count && dom->script_names[script].muted;
}

/* The address an error event names for a script: its own, else the page's. */
static const char *script_address(const os64_dom_t *dom, uint32_t script)
{
    const char *own = script < dom->script_name_count ? dom->script_names[script].address : NULL;
    return own != NULL ? own : dom->url != NULL ? dom->url : "about:blank";
}

/* The script whose failure it is when the stack does not say: the one
 * whose callback the binding is calling, else the last it called in this
 * task, else the one being run, else the page. */
static uint32_t current_script(const os64_dom_t *dom)
{
    if (dom->calling_script != D_NO_SCRIPT) return dom->calling_script;
    if (dom->task_script != D_NO_SCRIPT) return dom->task_script;
    if (dom->running_script != D_NO_SCRIPT) return dom->running_script;
    return D_SCRIPT_PAGE;
}

/* The running script, as the engine's stack names it: the nearest frame of
 * a script the binding named (QuickJS's JS_GetScriptOrModuleName, which
 * answers nothing for a native frame and passes over eval's), else
 * current_script. The walk stops a few native frames past the last script. */
static uint32_t running_script(const os64_dom_t *dom, JSContext *ctx)
{
    for (int level = 0, misses = 0; level < 32 && misses < 8; level++) {
        JSAtom atom = JS_GetScriptOrModuleName(ctx, level);
        if (atom == JS_ATOM_NULL) { misses++; continue; }
        misses = 0;
        const char *name = JS_AtomToCString(ctx, atom);
        JS_FreeAtom(ctx, atom);
        if (name == NULL) { discard(ctx); break; }
        uint32_t script = script_named(dom, name);
        JS_FreeCString(ctx, name);
        if (script != D_NO_SCRIPT) return script;
    }
    return current_script(dom);
}

uint32_t d_callback_script(os64_dom_t *dom, JSContext *ctx, JSValueConst function)
{
    uint32_t script = D_NO_SCRIPT;
    /* The getter is the engine's own, so asking runs no page code; it
     * answers undefined for any function that is not a script's. */
    if (dom->filename_getter != NULL && JS_IsFunction(ctx, function)) {
        JSValue name = JS_Call(ctx, dom->filename_getter->value, function, 0, NULL);
        const char *text = JS_IsException(name) ? NULL : JS_IsString(name) ? JS_ToCString(ctx, name) : NULL;
        if (JS_IsException(name)) discard(ctx);
        JS_FreeValue(ctx, name);
        script = script_named(dom, text);
        if (text != NULL) JS_FreeCString(ctx, text);
    }
    return script != D_NO_SCRIPT ? script : running_script(dom, ctx);
}

DScripts d_script_enter(os64_dom_t *dom, uint32_t script)
{
    DScripts before = {dom->running_script, dom->calling_script, dom->task_script, dom->thrown_script};
    dom->calling_script = script;
    if (script != D_NO_SCRIPT) dom->task_script = script;
    return before;
}

void d_script_leave(os64_dom_t *dom, const DScripts *before, bool top)
{
    dom->running_script = before->running;
    dom->calling_script = before->calling;
    dom->thrown_script = before->thrown;
    if (!top) dom->task_script = before->task;
}

bool os64_dom_script_name(os64_dom_t *dom, const char *address, char *out, size_t cap)
{
    if (dom == NULL || dom->closed || out == NULL || cap == 0) return false;
    uint32_t script = D_SCRIPT_PAGE;
    if (address != NULL) {
        bool muted = foreign(dom, address);
        script = script_record(dom, address, muted);
        if (script == D_NO_SCRIPT) script = muted ? D_SCRIPT_FOREIGN : D_SCRIPT_PAGE;
    }
    dom->running_script = script;
    return (size_t)os64_snprintf(out, cap, D_SCRIPT_PREFIX "%u", script) < cap;
}

/* THE SCRIPT HOST'S HOOKS, called inside the wrapper's guard: each keeps
 * the value, or answers, and runs nothing. */
static void host_exception(void *opaque, JSContext *ctx, JSValue error)
{
    os64_dom_t *dom = opaque;
    uint32_t script = dom->thrown_script != D_NO_SCRIPT ? dom->thrown_script : current_script(dom);
    dom->thrown_script = D_NO_SCRIPT;
    if (dom->closed || !dom->options.modern || dom->reporting_error) return;
    pending_push(dom, ctx, error, true, script);
}

static uint32_t host_rejected(void *opaque, JSContext *ctx, JSValue promise)
{
    (void)promise;
    os64_dom_t *dom = opaque;
    if (dom->closed || !dom->options.modern) return D_SCRIPT_PAGE;
    return running_script(dom, ctx);
}

static void host_handled(void *opaque, JSContext *ctx, JSValue promise)
{
    os64_dom_t *dom = opaque;
    if (dom->closed || !dom->options.modern) return;
    if (JS_VALUE_GET_PTR(promise) == dom->rejection_firing) dom->rejection_handled = true;
    else pending_push(dom, ctx, promise, false, D_SCRIPT_PAGE);
}

bool d_report_host(os64_dom_t *dom)
{
    dom->running_script = dom->calling_script = dom->task_script = dom->thrown_script = D_NO_SCRIPT;
    if (script_record(dom, NULL, false) != D_SCRIPT_PAGE || script_record(dom, NULL, true) != D_SCRIPT_FOREIGN)
        return false;
    const os64_js_script_host_t host = {dom, host_exception, host_handled, host_rejected};
    os64_js_outcome_t outcome;
    return os64_js_set_script_host(dom->runtime, OS64_JS_ABI_ID, &host, &outcome) == OS64_JS_OK;
}

void d_report_drain(os64_dom_t *dom)
{
    os64_js_outcome_t outcome;
    (void)os64_js_set_script_host(dom->runtime, OS64_JS_ABI_ID, NULL, &outcome);
    while (dom->errors != NULL) JS_FreeValueRT(dom->engine, pending_pop(dom, true, NULL));
    while (dom->handled != NULL) JS_FreeValueRT(dom->engine, pending_pop(dom, false, NULL));
}

void d_report_free(os64_dom_t *dom)
{
    for (uint32_t i = 0; i < dom->script_name_count; i++) d_free(dom, dom->script_names[i].address);
    d_free(dom, dom->script_names);
    dom->script_names = NULL;
    dom->script_name_count = dom->script_name_cap = 0;
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

/* Where an error was made, its line and column, from its own stack text as
 * the engine writes it, a frame a line, "    at f (name:line:col)", or
 * "    at name:line:col" for a script that did not compile: the first frame
 * that names a place, which skips the native ones. Only an Error's is
 * read, and only as a data property, so no page code runs; a stack the
 * page wrote there is read the same way and believed: it is the page's own
 * error. It says where, never whose: that is the script's name's
 * (running_script), which no text decides. */
static void error_location(JSContext *ctx, JSValueConst error, uint32_t *line, uint32_t *col)
{
    *line = *col = 0;
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
    error_location(ctx, here, line, col);
    JS_FreeValue(ctx, here);
}

/* The error event for `error`, `script`'s: HTML's muted errors make one of
 * another site's script "Script error." and nothing else, so no page reads
 * another site's failure; any other names the script's address and the
 * place the error was made. */
static JSValue error_event(os64_dom_t *dom, JSContext *ctx, JSValueConst error, uint32_t script, bool running)
{
    if (script_muted(dom, script)) return d_error_event_new(dom, ctx, "Script error.", "", 0, 0, JS_NULL);
    uint32_t line, col;
    error_location(ctx, error, &line, &col);
    char text[OS64_JS_MESSAGE_CAP], message[OS64_JS_MESSAGE_CAP + 16];
    d_error_text(dom, ctx, error, text, sizeof(text));
    os64_snprintf(message, sizeof(message), "Uncaught %s", text);
    if (line == 0 && running) here_location(dom, ctx, &line, &col);
    return d_error_event_new(dom, ctx, message, script_address(dom, script), line, col, error);
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

void d_report_error(os64_dom_t *dom, JSContext *ctx, JSValueConst error, uint32_t script)
{
    if (dom->closed || !dom->options.modern || dom->reporting_error || dom->error_event_prototype == NULL) return;
    os64_js_outcome_t probe;
    os64_js_status_t running = os64_js_check_budget(dom->runtime, OS64_JS_ABI_ID, &probe);
    if (running == OS64_JS_BAD_ARGUMENT) {
        pending_push(dom, ctx, error, true, script);
        return;
    }
    if (running != OS64_JS_OK || !os64_dom_listens(dom, "error")) return;
    JSValue event = error_event(dom, ctx, error, script, true);
    if (JS_IsException(event)) { discard(ctx); return; }
    dom->reporting_error = true;
    int result = d_dispatch(dom, ctx, event, NULL);
    dom->reporting_error = false;
    JS_FreeValue(ctx, event);
    /* The callers carry on as after any reported error; a runtime that
     * failed under the dispatch fails their next step again. */
    if (result < 0 && JS_HasException(ctx)) discard(ctx);
}

void d_errors_fire(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    while (dom->errors != NULL) {
        uint32_t script;
        JSValue error = pending_pop(dom, true, &script);
        if (ctx != NULL && !sticky(outcome->status) && os64_dom_listens(dom, "error")) {
            JSValue event = error_event(dom, ctx, error, script, false);
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

/* HTML puts no muted promise among those a later handler is owed a
 * rejectionhandled for: the binding's set holds each muted promise it
 * took, made with the intrinsic WeakSet at the first, and handled_fire
 * asks it. False when the promise could not be kept (the budget refused
 * the set, or the engine its memory), which the caller remembers as
 * muted_forgotten: from then on no rejectionhandled fires at all, since
 * the binding can no longer tell which are another site's. */
static bool remember_muted(os64_dom_t *dom, JSContext *ctx, JSValueConst promise)
{
    if (dom->weak_set[0] == NULL) return false;
    if (dom->muted_promises == NULL) {
        JSValue set = JS_CallConstructor(ctx, dom->weak_set[0]->value, 0, NULL);
        dom->muted_promises = d_retain(dom, ctx, set);
        if (dom->muted_promises == NULL) {
            discard(ctx);
            return false;
        }
    }
    JSValue added = JS_Call(ctx, dom->weak_set[1]->value, dom->muted_promises->value, 1, &promise);
    bool kept = !JS_IsException(added);
    if (!kept) discard(ctx);
    JS_FreeValue(ctx, added);
    return kept;
}

/* One rejection the runtime handed over: its unhandledrejection, the
 * task's failure unless a listener cancelled it, and, unless a handler
 * reached it during the event, its place among the promises a later
 * handler names (host_handled). One another site's script made (`script`,
 * the running script when it was rejected: host_rejected) is the task's
 * failure and no page's event, as HTML's rejection tracker ignores a muted
 * script's. */
static void unhandled(os64_dom_t *dom, JSValue promise, JSValue reason, const char *task, uint32_t script,
                      os64_js_outcome_t *outcome)
{
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    bool cancelled = false, muted = script_muted(dom, script);
    if (ctx != NULL && muted && !remember_muted(dom, ctx, promise)) dom->muted_forgotten = true;
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
    bool first = ctx != NULL && !cancelled && outcome->status == OS64_JS_OK;
    bool told = ctx != NULL && !cancelled && !first && !sticky(outcome->status) && dom->options.failed != NULL;
    if (told && dom->rejections_told == D_REJECTIONS_TOLD) {
        dom->rejections_untold++;
        told = false;
    } else if (told) dom->rejections_told++;
    if (first || told) {
        os64_js_outcome_t failure = {.status = OS64_JS_UNHANDLED_REJECTION};
        char text[OS64_JS_MESSAGE_CAP];
        d_error_text(dom, ctx, reason, text, sizeof(text));
        failure.diagnostic_truncated =
            (size_t)os64_snprintf(failure.message, sizeof(failure.message), "unhandled rejection: %s", text) >=
            sizeof(failure.message);
        os64_strcopy(failure.source_name, sizeof(failure.source_name), task);
        if (first) d_outcome_fold(outcome, &failure);
        else dom->options.failed(dom->options.host_opaque, task, failure.message);
    }
    JS_FreeValueRT(dom->engine, promise);
    JS_FreeValueRT(dom->engine, reason);
}

/* The rejectionhandled each promise on `due` is owed (a list taken whole
 * from the binding's, `last` its end, whose entries this frees). HTML fires
 * each in a task of its own, so the microtasks one event's listeners queue
 * run before the next: once an event leaves a job runnable, the entries
 * after it go back to the head of the binding's list, for the pass after
 * the next checkpoint, as os64_js_take_rejection waits for one. */
static void handled_fire(os64_dom_t *dom, struct DPending *due, struct DPending *last, os64_js_outcome_t *outcome)
{
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    while (due != NULL) {
        if (JS_IsJobPending(dom->engine)) {
            last->next = dom->handled;
            if (dom->handled == NULL) dom->handled_last = last;
            dom->handled = due;
            return;
        }
        struct DPending *item = due;
        due = item->next;
        JSValue promise = item->value;
        d_free(dom, item);
        dom->pending_held--;
        /* A muted promise leaves the set as its handler is heard; one the
         * set could not be asked about is taken for muted. */
        bool muted = dom->muted_forgotten;
        if (!muted && ctx != NULL && dom->muted_promises != NULL) {
            JSValue gone = JS_Call(ctx, dom->weak_set[2]->value, dom->muted_promises->value, 1, &promise);
            if (JS_IsException(gone)) discard(ctx);
            muted = JS_IsException(gone) || JS_ToBool(ctx, gone) != 0;
            JS_FreeValue(ctx, gone);
        }
        if (ctx != NULL && !muted && !sticky(outcome->status) && os64_dom_listens(dom, "rejectionhandled")) {
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
}

void d_task_settle(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    for (;;) {
        if (sticky(outcome->status)) return;
        os64_js_outcome_t point;
        os64_js_status_t checked = os64_js_checkpoint(dom->runtime, OS64_JS_ABI_ID, &point);
        fold(outcome, &point);
        if (checked != OS64_JS_OK && checked != OS64_JS_EXCEPTION) return;
        /* The rejectionhandled owed now are those a handler reached before
         * this checkpoint: HTML queues each as a task, so a handler added by
         * a listener fired below has its reaction run (the next checkpoint)
         * before its event, which waits for the pass after it. */
        struct DPending *due = dom->handled, *last = dom->handled_last;
        dom->handled = dom->handled_last = NULL;
        bool owed = dom->errors != NULL || due != NULL;
        d_errors_fire(dom, outcome);
        handled_fire(dom, due, last, outcome);
        JSValue promise, reason;
        uint32_t script;
        while (!sticky(outcome->status) &&
               os64_js_take_rejection(dom->runtime, OS64_JS_ABI_ID, &promise, &reason, &script)) {
            owed = true;
            unhandled(dom, promise, reason, point.source_name, script, outcome);
        }
        if (!owed && dom->handled == NULL && dom->errors == NULL) return;
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

void d_script_settle(os64_dom_t *dom, const char *name, os64_js_outcome_t *outcome)
{
    JSValue promise, reason;
    uint32_t script;
    bool taken = os64_js_take_rejection(dom->runtime, OS64_JS_ABI_ID, &promise, &reason, &script);
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
     * rejection. Each is muted or not by its own script. */
    d_errors_fire(dom, &task);
    if (taken) unhandled(dom, promise, reason, name, script, &task);
    d_task_finish(dom, &task);
    *outcome = task;
}

bool os64_dom_script_ended(os64_dom_t *dom, const char *name, os64_js_outcome_t *outcome)
{
    if (dom == NULL || dom->closed || name == NULL || outcome == NULL) return false;
    d_script_settle(dom, name, outcome);
    /* A script that left nothing owed opened no task, and a report of its
     * (a nested dispatch's error) is told here all the same. */
    d_task_close(dom, name, outcome);
    dom->running_script = D_NO_SCRIPT;
    return true;
}

static JSValue report_error(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "reportError needs the error to report");
    /* The script that called it is the one reporting (HTML's "report an
     * exception" from the running script). */
    d_report_as(dom, ctx, argv[0], running_script(dom, ctx));
    return JS_UNDEFINED;
}

/* `object`'s own property `name` as the engine made it, before any page
 * ran: its getter when `getter`, else its value; NULL when there is none
 * or no memory. */
static DValue *intrinsic(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const char *name, bool getter)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) return NULL;
    JSPropertyDescriptor descriptor;
    int present = JS_GetOwnProperty(ctx, &descriptor, object, atom);
    JS_FreeAtom(ctx, atom);
    if (present <= 0) return NULL;
    JSValue kept = getter ? descriptor.getter : descriptor.value;
    if (getter) descriptor.getter = JS_UNDEFINED;
    else descriptor.value = JS_UNDEFINED;
    JS_FreeValue(ctx, descriptor.value);
    JS_FreeValue(ctx, descriptor.getter);
    JS_FreeValue(ctx, descriptor.setter);
    if (!JS_IsFunction(ctx, kept)) {
        JS_FreeValue(ctx, kept);
        return NULL;
    }
    return d_retain(dom, ctx, kept);
}

/* What muting needs of the engine, taken now, before any page ran:
 * Function.prototype's fileName getter (d_callback_script), and WeakSet
 * with WeakSet.prototype's add and delete (remember_muted, handled_fire). */
static int muting_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue function = JS_GetPropertyStr(ctx, global, "Function");
    JSValue prototype = JS_IsException(function) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, function, "prototype");
    JS_FreeValue(ctx, function);
    if (JS_IsException(prototype)) return -1;
    dom->filename_getter = intrinsic(dom, ctx, prototype, "fileName", true);
    JS_FreeValue(ctx, prototype);
    /* The set itself is made at the first muted rejection (remember_muted):
     * a call here could hear an allocation refused earlier in the
     * installer and fail the runtime under it. */
    dom->weak_set[0] = intrinsic(dom, ctx, global, "WeakSet", false);
    JSValue weak_prototype = dom->weak_set[0] != NULL ? JS_GetPropertyStr(ctx, dom->weak_set[0]->value, "prototype")
                                                      : JS_EXCEPTION;
    if (!JS_IsException(weak_prototype)) {
        dom->weak_set[1] = intrinsic(dom, ctx, weak_prototype, "add", false);
        dom->weak_set[2] = intrinsic(dom, ctx, weak_prototype, "delete", false);
    }
    JS_FreeValue(ctx, weak_prototype);
    /* Muting is all of it or no binding: a part missing would tell a page
     * what another site's script failed with. */
    return dom->filename_getter != NULL && dom->weak_set[1] != NULL && dom->weak_set[2] != NULL ? 0 : -1;
}

int d_report_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    dom->report_fire = d_retain(dom, ctx, JS_NewCFunctionData(ctx, fire, 1, 0, 1, &dom->anchor->value));
    dom->error_constructor = dom->report_fire != NULL ? d_retain(dom, ctx, JS_GetPropertyStr(ctx, global, "Error"))
                                                      : NULL;
    if (dom->error_constructor == NULL || muting_install(dom, ctx, global) < 0) return -1;
    return d_error_events_install(dom, ctx, global) < 0 ||
        d_event_handler(dom, ctx, global, D_EVENT_ERROR) < 0 ||
        d_event_handler(dom, ctx, global, D_EVENT_UNHANDLEDREJECTION) < 0 ||
        d_event_handler(dom, ctx, global, D_EVENT_REJECTIONHANDLED) < 0 ||
        d_event_handler(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, D_EVENT_ERROR) < 0 ||
        d_method_named(dom, ctx, global, "reportError", report_error, 1, 0) < 0 ? -1 : 0;
}
