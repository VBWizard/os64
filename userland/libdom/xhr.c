#include "internal.h"
#include "os64/fmt.h"

/* XMLHttpRequest (the XHR Standard): the same request fetch() makes, sent
 * through the same host (dom.h § A SCRIPT'S REQUESTS), with the events an
 * XMLHttpRequest fires around it. An XMLHttpRequest and its upload object
 * are COLLECTABLE plain event targets (event.c): a page makes one per
 * request. While its request is out, the request holds it (the registry's
 * fourth list), as the standard keeps an object with pending activity.
 *
 * The host answers a request whole, so the body arrives as one chunk: the
 * readystatechange to LOADING fires for it, and the one progress event a
 * browser's 50 ms throttle leaves for a small body fires at its end.
 *
 * Everything here is engine memory: the author headers, the answer's
 * header block and body. A header list is a C array a page never reaches,
 * and each step that runs page code is followed by a look at the object's
 * state again: `generation` changes when open() or abort() starts over,
 * and what was under way stops. */

static JSClassID xhr_slot, upload_slot;

enum { UNSENT, OPENED, HEADERS_RECEIVED, LOADING, DONE };
enum { RT_EMPTY, RT_ARRAYBUFFER, RT_BLOB, RT_DOCUMENT, RT_JSON, RT_TEXT };
static const char *const kResponseTypes[] = {"", "arraybuffer", "blob", "document", "json", "text"};
static const char *const kStates[] = {"UNSENT", "OPENED", "HEADERS_RECEIVED", "LOADING", "DONE"};

typedef struct {
    char *name, *value;
    size_t name_length, value_length;
} XHeader;

typedef struct {
    DValue target;              /* first: the collectable plain target's record */
    int state;
    bool send_flag, sync, upload_complete, upload_listener, timed_out, with_credentials;
    uint32_t generation;
    char method[WAY_METHOD_MAX];
    char *url;
    XHeader *headers;
    size_t header_count, header_cap;
    uint32_t timeout, timer, token;
    uint64_t send_time;
    size_t request_length;      /* the body sent, for the upload's events */
    JSValue upload;
    int response_type;
    char *override_type;        /* NULL: none */
    /* The response: `error` is a network error. */
    bool error, has_body, response_failed;
    int32_t status;
    char *status_text, *response_url, *response_headers;
    size_t response_headers_length;
    uint8_t *body;
    size_t body_length;
    JSValue response_object;
} DXhr;

typedef struct {
    DValue target;
} DUpload;

static DXhr *xhr_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&xhr_slot));
}

static DUpload *upload_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&upload_slot));
}

/* ── State ───────────────────────────────────────────────────────────── */

static void headers_clear(JSRuntime *rt, DXhr *x)
{
    for (size_t i = 0; i < x->header_count; i++) {
        js_free_rt(rt, x->headers[i].name);
        js_free_rt(rt, x->headers[i].value);
    }
    x->header_count = 0;
}

/* The response set to a network error, its bytes freed. */
static void response_reset(JSRuntime *rt, DXhr *x)
{
    x->error = true;
    x->has_body = x->response_failed = false;
    x->status = 0;
    js_free_rt(rt, x->status_text);
    js_free_rt(rt, x->response_url);
    js_free_rt(rt, x->response_headers);
    js_free_rt(rt, x->body);
    x->status_text = x->response_url = x->response_headers = NULL;
    x->body = NULL;
    x->response_headers_length = x->body_length = 0;
    JS_FreeValueRT(rt, x->response_object);
    x->response_object = JS_UNDEFINED;
}

static void xhr_finalizer(JSRuntime *rt, JSValue value)
{
    DXhr *x = xhr_state(value);
    if (x == NULL) return;
    d_event_target_finalize(rt, &x->target);
    headers_clear(rt, x);
    js_free_rt(rt, x->headers);
    js_free_rt(rt, x->url);
    js_free_rt(rt, x->override_type);
    response_reset(rt, x);
    JS_FreeValueRT(rt, x->upload);
    js_free_rt(rt, x);
}

static void xhr_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DXhr *x = xhr_state(value);
    if (x == NULL) return;
    d_event_target_mark(rt, &x->target, mark);
    JS_MarkValue(rt, x->upload, mark);
    JS_MarkValue(rt, x->response_object, mark);
}

static void upload_finalizer(JSRuntime *rt, JSValue value)
{
    DUpload *u = upload_state(value);
    if (u == NULL) return;
    d_event_target_finalize(rt, &u->target);
    js_free_rt(rt, u);
}

static void upload_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DUpload *u = upload_state(value);
    if (u != NULL) d_event_target_mark(rt, &u->target, mark);
}

/* "terminate the ongoing fetch": the request forgotten with the host, the
 * timer stopped. Nothing fires. */
static void terminate(os64_dom_t *dom, DXhr *x)
{
    if (x->token != 0) {
        DRequest *request = d_request_find(dom, x->token);
        if (request != NULL) d_request_end(dom, request, true);
        x->token = 0;
    }
    if (x->timer != 0) {
        d_timer_cancel(dom, x->timer);
        x->timer = 0;
    }
}

static uint64_t now_ms(const os64_dom_t *dom)
{
    return dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) : 0;
}

/* ── Events ──────────────────────────────────────────────────────────── */

/* An event at `target`: a progress event (`progress`) with transmitted and
 * length, else a plain one. From a host task (`outcome`), each listener is
 * the task's call; from a page's own call, a nested one. -1 when the
 * runtime is finished with. */
static int fire(os64_dom_t *dom, JSContext *ctx, DValue *target, const char *type, bool progress, double transmitted,
                double length, os64_js_outcome_t *outcome)
{
    JSValue event = progress ? d_progress_event_new(dom, ctx, type, length != 0, transmitted, length, true)
                             : d_event_new(dom, ctx, type, false, false, true);
    if (JS_IsException(event)) return -1;
    int result = outcome != NULL ? d_dispatch_plain_task(dom, ctx, event, target, outcome)
                                 : d_dispatch_plain(dom, ctx, event, target);
    JS_FreeValue(ctx, event);
    return result < 0 ? -1 : 0;
}

static DValue *upload_target(DXhr *x)
{
    DUpload *u = upload_state(x->upload);
    return u != NULL ? &u->target : NULL;
}

/* Whether a listener waits on the upload object: what makes a request
 * with a body need a preflight. */
static bool upload_listened(DXhr *x)
{
    DValue *u = upload_target(x);
    for (const DListener *l = u != NULL ? u->listeners : NULL; l != NULL; l = l->next)
        if (!l->removed && !JS_IsUndefined(l->callback)) return true;
    return false;
}

/* XHR § "request error steps": done, the response a network error, and
 * (for an asynchronous request) the events. -1 when the runtime is
 * finished with. */
static int request_error(os64_dom_t *dom, JSContext *ctx, JSValueConst self, const char *event,
                         os64_js_outcome_t *outcome)
{
    DXhr *x = xhr_state(self);
    x->state = DONE;
    x->send_flag = false;
    response_reset(JS_GetRuntime(ctx), x);
    if (x->sync) return 0;
    if (fire(dom, ctx, &x->target, "readystatechange", false, 0, 0, outcome) < 0) return -1;
    x = xhr_state(self);
    if (!x->upload_complete) {
        x->upload_complete = true;
        if (x->upload_listener && upload_target(x) != NULL &&
            (fire(dom, ctx, upload_target(x), event, true, 0, 0, outcome) < 0 ||
             fire(dom, ctx, upload_target(x), "loadend", true, 0, 0, outcome) < 0))
            return -1;
    }
    if (fire(dom, ctx, &x->target, event, true, 0, 0, outcome) < 0 ||
        fire(dom, ctx, &x->target, "loadend", true, 0, 0, outcome) < 0)
        return -1;
    return 0;
}

/* ── The answer ──────────────────────────────────────────────────────── */

static char *engine_copy(JSContext *ctx, const char *s, size_t n)
{
    char *copy = js_malloc(ctx, n + 1);
    if (copy == NULL) return NULL;
    if (n != 0) os64_memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

/* The host's answer copied into the object (the runtime's memory, which a
 * body may not have room for). -1 on no memory. */
static int response_take(JSContext *ctx, DXhr *x, const os64_dom_response_t *in)
{
    response_reset(JS_GetRuntime(ctx), x);
    const char *text = in->status_text != NULL ? in->status_text : "";
    const char *url = in->url != NULL ? in->url : "";
    size_t url_length = 0;
    while (url[url_length] != '\0' && url[url_length] != '#') url_length++;
    x->status_text = engine_copy(ctx, text, os64_strlen(text));
    x->response_url = x->status_text != NULL ? engine_copy(ctx, url, url_length) : NULL;
    x->response_headers = x->response_url != NULL
        ? engine_copy(ctx, in->headers != NULL ? in->headers : "", in->headers != NULL ? in->headers_length : 0) : NULL;
    x->body = x->response_headers != NULL && in->has_body ? (uint8_t *)engine_copy(ctx, in->body, in->body_length) : NULL;
    if (x->response_headers == NULL || (in->has_body && x->body == NULL)) {
        response_reset(JS_GetRuntime(ctx), x);
        return -1;
    }
    x->error = false;
    x->status = in->status;
    x->response_headers_length = in->headers != NULL ? in->headers_length : 0;
    x->has_body = in->has_body;
    x->body_length = in->has_body ? in->body_length : 0;
    return 0;
}

/* The combined value of `name` in the answer's header block (engine
 * memory), NULL for none; `*no_memory` when that is why. */
static char *response_header(JSContext *ctx, const DXhr *x, const char *name, size_t nlen, size_t *length,
                             bool *no_memory)
{
    *no_memory = false;
    if (x->error || x->response_headers == NULL) return NULL;
    size_t total = 0, found = 0, at = 0;
    way_header_t line;
    while (way_header_next(x->response_headers, x->response_headers_length, &at, &line))
        if (d_same_nocase(line.name, line.name_len, name, nlen)) total += line.value_len + (found++ ? 2 : 0);
    if (found == 0) return NULL;
    char *out = js_malloc(ctx, total + 1);
    if (out == NULL) {
        *no_memory = true;
        return NULL;
    }
    size_t used = 0, k = 0;
    at = 0;
    while (way_header_next(x->response_headers, x->response_headers_length, &at, &line))
        if (d_same_nocase(line.name, line.name_len, name, nlen)) {
            if (k++ > 0) {
                out[used++] = ',';
                out[used++] = ' ';
            }
            os64_memcpy(out + used, line.value, line.value_len);
            used += line.value_len;
        }
    out[used] = '\0';
    *length = used;
    return out;
}

/* Fetch's "extract a length": Content-Length when its values agree on one
 * number, else 0, which a progress event reads as not computable. */
static double response_length(const DXhr *x)
{
    if (x->error || x->response_headers == NULL) return 0;
    size_t at = 0;
    way_header_t line;
    double length = -1;
    while (way_header_next(x->response_headers, x->response_headers_length, &at, &line)) {
        if (!d_same_nocase(line.name, line.name_len, "content-length", 14)) continue;
        /* A combined value lists each with ", ". */
        size_t i = 0;
        while (i <= line.value_len) {
            size_t start = i;
            while (i < line.value_len && line.value[i] != ',') i++;
            size_t a = start, b = i;
            while (a < b && (line.value[a] == ' ' || line.value[a] == '\t')) a++;
            while (b > a && (line.value[b - 1] == ' ' || line.value[b - 1] == '\t')) b--;
            if (a == b || b - a > 15) return 0;
            double n = 0;
            for (size_t k = a; k < b; k++) {
                if (line.value[k] < '0' || line.value[k] > '9') return 0;
                n = n * 10 + (line.value[k] - '0');
            }
            if (length >= 0 && n != length) return 0;
            length = n;
            i++;
        }
    }
    return length < 0 ? 0 : length;
}

/* XHR § "final MIME type" (engine memory): the override, else the
 * answer's Content-Type, else text/xml; `charset` gets its charset ("" for
 * none). */
static char *final_type(JSContext *ctx, const DXhr *x, char *charset, size_t charset_cap)
{
    char essence[WAY_MIME_ESSENCE_MAX], out[WAY_MIME_ESSENCE_MAX + WAY_MIME_CHARSET_MAX + 16];
    charset[0] = '\0';
    if (x->override_type != NULL) {
        if (way_mime_parse(x->override_type, os64_strlen(x->override_type), essence, sizeof(essence), charset, charset_cap))
            return engine_copy(ctx, x->override_type, os64_strlen(x->override_type));
        return engine_copy(ctx, "application/octet-stream", 24);
    }
    bool no_memory;
    size_t length = 0;
    char *joined = response_header(ctx, x, "content-type", 12, &length, &no_memory);
    if (no_memory) return NULL;
    /* The last type of a combined value is the one "extract a MIME type"
     * keeps. */
    const char *last = joined;
    for (size_t i = 0; joined != NULL && i < length; i++)
        if (joined[i] == ',') last = joined + i + 1;
    bool parsed = last != NULL &&
                  way_mime_parse(last, length - (size_t)(last - joined), essence, sizeof(essence), charset, charset_cap);
    js_free(ctx, joined);
    if (!parsed) {
        charset[0] = '\0';
        return engine_copy(ctx, "text/xml", 8);
    }
    os64_snprintf(out, sizeof(out), charset[0] ? "%s;charset=%s" : "%s", essence, charset);
    return engine_copy(ctx, out, os64_strlen(out));
}

/* The body as text, in the final MIME type's charset (UTF-8 without one). */
static JSValue response_text(JSContext *ctx, const DXhr *x)
{
    if (x->error || !x->has_body || x->body_length == 0) return JS_NewString(ctx, "");
    char charset[WAY_MIME_CHARSET_MAX];
    char *type = final_type(ctx, x, charset, sizeof(charset));
    if (type == NULL) return JS_EXCEPTION;
    js_free(ctx, type);
    return d_text_decode(ctx, charset[0] != '\0' ? charset : "utf-8", x->body, x->body_length);
}

/* XHR § "handle response end-of-body", from the progress at its head. */
static int end_of_body(os64_dom_t *dom, JSContext *ctx, JSValueConst self, os64_js_outcome_t *outcome)
{
    DXhr *x = xhr_state(self);
    uint32_t generation = x->generation;
    double transmitted = (double)x->body_length, length = response_length(x);
    if (!x->sync && fire(dom, ctx, &x->target, "progress", true, transmitted, length, outcome) < 0) return -1;
    x = xhr_state(self);
    if (x->generation != generation) return 0;
    x->state = DONE;
    x->send_flag = false;
    if (fire(dom, ctx, &x->target, "readystatechange", false, 0, 0, outcome) < 0) return -1;
    if (xhr_state(self)->generation != generation) return 0;
    if (fire(dom, ctx, &x->target, "load", true, transmitted, length, outcome) < 0) return -1;
    if (xhr_state(self)->generation != generation) return 0;
    return fire(dom, ctx, &x->target, "loadend", true, transmitted, length, outcome) < 0 ? -1 : 0;
}

/* The host's answer to an asynchronous request, as one task. */
static void xhr_answered(os64_dom_t *dom, JSContext *ctx, DRequest *request, const os64_dom_response_t *response,
                         os64_js_outcome_t *outcome)
{
    JSValue self = JS_DupValue(ctx, request->held[0]);
    d_request_end(dom, request, false);
    DXhr *x = xhr_state(self);
    if (x == NULL) {
        JS_FreeValue(ctx, self);
        return;
    }
    x->token = 0;
    if (x->timer != 0) {
        d_timer_cancel(dom, x->timer);
        x->timer = 0;
    }
    uint32_t generation = x->generation;
    if (!response->ok || response_take(ctx, x, response) < 0) {
        (void)request_error(dom, ctx, self, "error", outcome);
        JS_FreeValue(ctx, self);
        return;
    }
    int failed = 0;
    /* The request's body went first (processRequestEndOfBody). */
    if (!x->upload_complete) {
        x->upload_complete = true;
        double sent = (double)x->request_length;
        if (x->upload_listener && upload_target(x) != NULL) {
            failed = fire(dom, ctx, upload_target(x), "progress", true, sent, sent, outcome);
            if (failed == 0 && xhr_state(self)->generation == generation)
                failed = fire(dom, ctx, upload_target(x), "load", true, sent, sent, outcome);
            if (failed == 0 && xhr_state(self)->generation == generation)
                failed = fire(dom, ctx, upload_target(x), "loadend", true, sent, sent, outcome);
        }
    }
    /* processResponse: the headers. */
    if (failed == 0 && xhr_state(self)->generation == generation) {
        x->state = HEADERS_RECEIVED;
        failed = fire(dom, ctx, &x->target, "readystatechange", false, 0, 0, outcome);
    }
    /* The body's one chunk: LOADING. Its progress event waits for the end
     * of the body, as a browser's throttle holds one back for a small
     * body. */
    if (failed == 0 && x->generation == generation && x->state == HEADERS_RECEIVED && x->body_length != 0) {
        x->state = LOADING;
        failed = fire(dom, ctx, &x->target, "readystatechange", false, 0, 0, outcome);
    }
    if (failed == 0 && x->generation == generation && (x->state == HEADERS_RECEIVED || x->state == LOADING))
        (void)end_of_body(dom, ctx, self, outcome);
    JS_FreeValue(ctx, self);
}

/* An asynchronous request's timeout: data[1] is the object, magic its
 * generation when the timer was set. */
static JSValue timeout_fired(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DXhr *x = xhr_state(data[1]);
    if (x == NULL || x->generation != (uint32_t)magic || !x->send_flag) return JS_UNDEFINED;
    x->timer = 0;
    x->timed_out = true;
    terminate(dom, x);
    return request_error(dom, ctx, data[1], "timeout", NULL) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* The timer for an asynchronous request under way: what is left of the
 * timeout since send(), or none for 0. -1 with an exception pending. */
static int timer_set(os64_dom_t *dom, JSContext *ctx, JSValueConst self)
{
    DXhr *x = xhr_state(self);
    if (x->timer != 0) {
        d_timer_cancel(dom, x->timer);
        x->timer = 0;
    }
    if (x->timeout == 0 || x->sync || !x->send_flag) return 0;
    uint64_t now = now_ms(dom), due = x->send_time + x->timeout;
    JSValue data[2] = {dom->anchor->value, (JSValue)self};
    JSValue fired = JS_NewCFunctionData(ctx, timeout_fired, 0, (int)x->generation, 2, data);
    if (JS_IsException(fired)) return -1;
    uint32_t timer = d_timer_add(dom, ctx, fired, due > now ? due - now : 0);
    if (timer == 0) return -1;
    xhr_state(self)->timer = timer;
    return 0;
}

/* ── The script surface ──────────────────────────────────────────────── */

enum {
    X_READY_STATE, X_OPEN, X_SET_HEADER, X_TIMEOUT, X_WITH_CREDENTIALS, X_UPLOAD, X_SEND, X_ABORT,
    X_RESPONSE_URL, X_STATUS, X_STATUS_TEXT, X_GET_HEADER, X_GET_ALL_HEADERS, X_OVERRIDE_MIME, X_RESPONSE_TYPE,
    X_RESPONSE, X_RESPONSE_TEXT, X_RESPONSE_XML,
};

/* open(method, url, async, username, password). */
static JSValue xhr_open(os64_dom_t *dom, JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "open needs a method and a URL");
    char *method;
    size_t length;
    if (!d_byte_string(ctx, argv[0], &method, &length)) return JS_EXCEPTION;
    bool token = way_method_ok(method, length) && length < WAY_METHOD_MAX;
    bool forbidden = token && way_method_forbidden(method, length);
    char normalized[WAY_METHOD_MAX] = "";
    if (token) os64_memcpy(normalized, method, length + 1);
    js_free(ctx, method);
    if (!token) return d_dom_exception(dom, ctx, "SyntaxError", "That is not a method");
    if (forbidden) return d_dom_exception(dom, ctx, "SecurityError", "That method is forbidden");
    way_method_normalize(normalized);
    char *parsed;
    int got = d_url_parse(dom, ctx, argv[1], &parsed);
    if (got < 0) return JS_EXCEPTION;
    if (got == 0) return d_dom_exception(dom, ctx, "SyntaxError", "Failed to parse the URL");
    bool async = true;
    if (argc > 2) {
        int flag = JS_ToBool(ctx, argv[2]);
        if (flag < 0) {
            d_free(dom, parsed);
            return JS_EXCEPTION;
        }
        async = flag != 0;
    }
    /* A user name and password would go into the URL; this browser does
     * not send credentials from a URL, so they are read and set aside. */
    for (int i = 3; i < argc && i < 5; i++)
        if (!JS_IsUndefined(argv[i]) && !JS_IsNull(argv[i])) {
            const char *text = JS_ToCString(ctx, argv[i]);
            if (text == NULL) {
                d_free(dom, parsed);
                return JS_EXCEPTION;
            }
            JS_FreeCString(ctx, text);
        }
    DXhr *x = xhr_state(self);
    if (!async && (x->timeout != 0 || x->response_type != RT_EMPTY)) {
        d_free(dom, parsed);
        return d_dom_exception(dom, ctx, "InvalidAccessError",
                               "A synchronous request cannot have a timeout or a response type");
    }
    char *url = engine_copy(ctx, parsed, os64_strlen(parsed));
    d_free(dom, parsed);
    if (url == NULL) return JS_EXCEPTION;
    terminate(dom, x);
    x->generation++;
    x->send_flag = x->upload_listener = false;
    os64_strcopy(x->method, sizeof(x->method), normalized);
    js_free(ctx, x->url);
    x->url = url;
    x->sync = !async;
    headers_clear(JS_GetRuntime(ctx), x);
    response_reset(JS_GetRuntime(ctx), x);
    if (x->state != OPENED) {
        x->state = OPENED;
        if (fire(dom, ctx, &x->target, "readystatechange", false, 0, 0, NULL) < 0) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/* setRequestHeader(name, value): combined with any of the name already
 * set, as the standard's "combine" does. */
static JSValue xhr_set_header(os64_dom_t *dom, JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "setRequestHeader needs a name and a value");
    char *name, *value;
    size_t nlen, vlen;
    if (!d_byte_string(ctx, argv[0], &name, &nlen)) return JS_EXCEPTION;
    if (!d_byte_string(ctx, argv[1], &value, &vlen)) {
        js_free(ctx, name);
        return JS_EXCEPTION;
    }
    DXhr *x = xhr_state(self);
    const char *refusal = NULL, *refusal_name = NULL;
    const char *v = value;
    size_t n = vlen;
    way_header_value_normalize(&v, &n);
    if (x->state != OPENED || x->send_flag) {
        refusal_name = "InvalidStateError";
        refusal = "The request is not open, or has been sent";
    } else if (!way_header_name_ok(name, nlen) || !way_header_value_ok(v, n)) {
        refusal_name = "SyntaxError";
        refusal = "That is not a header";
    }
    if (refusal != NULL || way_header_forbidden_request(name, nlen, v, n)) {
        js_free(ctx, name);
        js_free(ctx, value);
        return refusal != NULL ? d_dom_exception(dom, ctx, refusal_name, refusal) : JS_UNDEFINED;
    }
    for (size_t i = 0; i < x->header_count; i++) {
        XHeader *h = &x->headers[i];
        if (!d_same_nocase(h->name, h->name_length, name, nlen)) continue;
        char *joined = js_malloc(ctx, h->value_length + 2 + n + 1);
        if (joined != NULL) {
            os64_memcpy(joined, h->value, h->value_length);
            os64_memcpy(joined + h->value_length, ", ", 2);
            os64_memcpy(joined + h->value_length + 2, v, n);
            joined[h->value_length + 2 + n] = '\0';
            js_free(ctx, h->value);
            h->value = joined;
            h->value_length += 2 + n;
        }
        js_free(ctx, name);
        js_free(ctx, value);
        return joined != NULL ? JS_UNDEFINED : JS_EXCEPTION;
    }
    if (x->header_count == x->header_cap) {
        size_t cap = x->header_cap ? x->header_cap * 2 : 8;
        XHeader *grown = js_realloc(ctx, x->headers, cap * sizeof(*grown));
        if (grown == NULL) {
            js_free(ctx, name);
            js_free(ctx, value);
            return JS_EXCEPTION;
        }
        x->headers = grown;
        x->header_cap = cap;
    }
    os64_memmove(value, v, n);
    value[n] = '\0';
    x->headers[x->header_count++] = (XHeader){name, value, nlen, n};
    return JS_UNDEFINED;
}

static XHeader *header_named(DXhr *x, const char *name, size_t nlen)
{
    for (size_t i = 0; i < x->header_count; i++)
        if (d_same_nocase(x->headers[i].name, x->headers[i].name_length, name, nlen)) return &x->headers[i];
    return NULL;
}

/* A string body's author Content-Type keeps its words, but a charset in it
 * that is not UTF-8 says UTF-8, which is what the bytes are (XHR § send(),
 * step 4.4.3; the parameter's value is replaced where it stands). -1 on no
 * memory. */
static int charset_utf8(JSContext *ctx, XHeader *h)
{
    char essence[WAY_MIME_ESSENCE_MAX], charset[WAY_MIME_CHARSET_MAX];
    if (!way_mime_parse(h->value, h->value_length, essence, sizeof(essence), charset, sizeof(charset)) ||
        charset[0] == '\0' || os64_streq_nocase(charset, "utf-8"))
        return 0;
    /* The charset parameter: its value's span, quoted or not. */
    for (size_t i = 0; i + 8 <= h->value_length; i++) {
        if (h->value[i] != ';') continue;
        size_t at = i + 1;
        while (at < h->value_length && (h->value[at] == ' ' || h->value[at] == '\t')) at++;
        if (at + 8 > h->value_length || !d_same_nocase(h->value + at, 8, "charset=", 8)) continue;
        size_t start = at + 8, end = start;
        if (end < h->value_length && h->value[end] == '"') {
            end++;
            while (end < h->value_length && h->value[end] != '"') end += h->value[end] == '\\' ? 2 : 1;
            if (end < h->value_length) end++;
            if (end > h->value_length) end = h->value_length;
        } else
            while (end < h->value_length && h->value[end] != ';') end++;
        size_t length = start + 5 + (h->value_length - end);
        char *value = js_malloc(ctx, length + 1);
        if (value == NULL) return -1;
        os64_memcpy(value, h->value, start);
        os64_memcpy(value + start, "UTF-8", 5);
        os64_memcpy(value + start + 5, h->value + end, h->value_length - end);
        value[length] = '\0';
        js_free(ctx, h->value);
        h->value = value;
        h->value_length = length;
        return 0;
    }
    return 0;
}

/* The author headers as a header block (way/origin.h's shape). */
static char *headers_block(JSContext *ctx, const DXhr *x, const char *type, size_t *length)
{
    size_t total = type != NULL ? 14 + os64_strlen(type) + 2 : 0;
    for (size_t i = 0; i < x->header_count; i++) total += x->headers[i].name_length + 2 + x->headers[i].value_length + 2;
    char *out = js_malloc(ctx, total + 1);
    if (out == NULL) return NULL;
    size_t at = 0;
    for (size_t i = 0; i < x->header_count; i++)
        at += (size_t)os64_snprintf(out + at, total + 1 - at, "%s: %s\r\n", x->headers[i].name, x->headers[i].value);
    if (type != NULL) at += (size_t)os64_snprintf(out + at, total + 1 - at, "Content-Type: %s\r\n", type);
    *length = at;
    return out;
}

static void request_of(const DXhr *x, os64_dom_t *dom, const char *headers, size_t headers_length, const DBytes *body,
                       bool has_body, os64_dom_request_t *out)
{
    *out = (os64_dom_request_t){
        .url = x->url, .method = x->method, .headers = headers, .headers_length = headers_length,
        .body = has_body ? (const void *)(body->bytes != NULL ? body->bytes : (const uint8_t *)"") : NULL,
        .body_length = has_body ? body->length : 0,
        .mode = WAY_MODE_CORS,
        .credentials = x->with_credentials ? WAY_CREDENTIALS_INCLUDE : WAY_CREDENTIALS_SAME_ORIGIN,
        .redirect = WAY_REDIRECT_FOLLOW, .policy = WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN,
        .preflight = x->upload_listener, .referrer = dom->url != NULL ? dom->url : "",
    };
}

/* A synchronous request's stop: the script task's own budget. libjs's
 * check answers no once the task's deadline has passed or the runtime was
 * cancelled, and latches it, so a request that outlives the task ends the
 * task the way any script out of time ends: one mechanism, not two. */
static bool sync_stop(void *ctx)
{
    os64_dom_t *dom = ctx;
    os64_js_outcome_t outcome;
    return os64_js_check_budget(dom->runtime, OS64_JS_ABI_ID, &outcome) != OS64_JS_OK;
}

/* A synchronous request: run by the host to its end, then the events a
 * synchronous request fires (none before; readystatechange, load and
 * loadend after), or a NetworkError thrown. */
static JSValue send_sync(os64_dom_t *dom, JSContext *ctx, JSValueConst self, const os64_dom_request_t *request)
{
    os64_dom_response_t response = {0};
    bool ran = dom->network.run != NULL &&
               dom->network.run(dom->network.opaque, request, sync_stop, dom, &response);
    DXhr *x = xhr_state(self);
    int taken = ran && response.ok ? response_take(ctx, x, &response) : -1;
    if (ran && dom->network.release != NULL) dom->network.release(dom->network.opaque, &response);
    x->upload_complete = true;
    if (taken < 0) {
        (void)request_error(dom, ctx, self, "error", NULL);
        return d_dom_exception(dom, ctx, "NetworkError", "Failed to load");
    }
    x->state = HEADERS_RECEIVED;
    return end_of_body(dom, ctx, self, NULL) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue xhr_send(os64_dom_t *dom, JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DXhr *x = xhr_state(self);
    if (x->state != OPENED || x->send_flag)
        return d_dom_exception(dom, ctx, "InvalidStateError", "The request is not open, or has been sent");
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool bodyless = os64_streq(x->method, "GET") || os64_streq(x->method, "HEAD");
    bool has_body = !bodyless && !JS_IsUndefined(value) && !JS_IsNull(value);
    DBytes body = {0};
    bool has_type = false;
    if (has_body && d_body_extract(dom, ctx, value, &body, &has_type) < 0) return JS_EXCEPTION;
    x = xhr_state(self);            /* extracting ran page code */
    if (x->state != OPENED || x->send_flag) {
        d_bytes_free(ctx, &body);
        return d_dom_exception(dom, ctx, "InvalidStateError", "The request is not open, or has been sent");
    }
    /* The body's type: the author's Content-Type, with a string's charset
     * said truly; else the type the body gave. */
    XHeader *author = has_body ? header_named(x, "content-type", 12) : NULL;
    bool text = has_body && JS_IsString(value);
    if (author != NULL && text && charset_utf8(ctx, author) < 0) {
        d_bytes_free(ctx, &body);
        return JS_EXCEPTION;
    }
    const char *type = author == NULL && has_type && body.type != NULL && body.type[0] != '\0' ? body.type : NULL;
    size_t headers_length;
    char *headers = headers_block(ctx, x, type, &headers_length);
    if (headers == NULL) {
        d_bytes_free(ctx, &body);
        return JS_EXCEPTION;
    }
    x->upload_listener = upload_listened(x);
    x->upload_complete = !has_body;
    x->timed_out = false;
    x->send_flag = true;
    x->request_length = has_body ? body.length : 0;
    os64_dom_request_t request;
    request_of(x, dom, headers, headers_length, &body, has_body, &request);
    JSValue result = JS_UNDEFINED;
    if (x->sync) {
        result = send_sync(dom, ctx, self, &request);
        goto done;
    }
    uint32_t generation = x->generation;
    if (fire(dom, ctx, &x->target, "loadstart", true, 0, 0, NULL) < 0) {
        result = JS_EXCEPTION;
        goto done;
    }
    x = xhr_state(self);
    if (!x->upload_complete && x->upload_listener && upload_target(x) != NULL &&
        fire(dom, ctx, upload_target(x), "loadstart", true, 0, (double)x->request_length, NULL) < 0) {
        result = JS_EXCEPTION;
        goto done;
    }
    x = xhr_state(self);
    if (x->state != OPENED || !x->send_flag || x->generation != generation) goto done;
    /* The URL and method the request names are the object's, which page
     * code during the events above could not have changed without open(). */
    request_of(x, dom, headers, headers_length, &body, has_body, &request);
    DRequest *out = d_request_start(dom, ctx, &request, xhr_answered);
    if (out == NULL) {
        /* Refused before it began: a network error, which fires as one. */
        JSValue error = JS_GetException(ctx);
        JS_FreeValue(ctx, error);
        if (request_error(dom, ctx, self, "error", NULL) < 0) result = JS_EXCEPTION;
        goto done;
    }
    x->token = out->token;
    out->held[0] = JS_DupValue(ctx, self);
    dom->requests_held++;
    x->send_time = now_ms(dom);
    if (timer_set(dom, ctx, self) < 0) result = JS_EXCEPTION;
done:
    js_free(ctx, headers);
    d_bytes_free(ctx, &body);
    return result;
}

static JSValue xhr_abort(os64_dom_t *dom, JSContext *ctx, JSValueConst self)
{
    DXhr *x = xhr_state(self);
    terminate(dom, x);
    x->generation++;
    if ((x->state == OPENED && x->send_flag) || x->state == HEADERS_RECEIVED || x->state == LOADING)
        if (request_error(dom, ctx, self, "abort", NULL) < 0) return JS_EXCEPTION;
    x = xhr_state(self);
    if (x->state == DONE) {
        x->state = UNSENT;
        response_reset(JS_GetRuntime(ctx), x);
    }
    return JS_UNDEFINED;
}

/* getAllResponseHeaders(): sort and combine, then in the order the
 * standard asks, names compared as if upper-cased: a merge sort over the
 * block's lines, n log n in the number of lines. */
typedef struct {
    const char *name, *value;
    size_t name_length, value_length;
} Line;

static int upper_order(const Line *a, const Line *b)
{
    for (size_t i = 0; i < a->name_length && i < b->name_length; i++) {
        unsigned char x = (unsigned char)a->name[i], y = (unsigned char)b->name[i];
        if (x >= 'a' && x <= 'z') x = (unsigned char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (unsigned char)(y - 32);
        if (x != y) return x < y ? -1 : 1;
    }
    return a->name_length == b->name_length ? 0 : a->name_length < b->name_length ? -1 : 1;
}

static JSValue all_headers(JSContext *ctx, const DXhr *x)
{
    if (x->error || x->response_headers == NULL) return JS_NewString(ctx, "");
    size_t count = 0, at = 0;
    way_header_t line;
    while (way_header_next(x->response_headers, x->response_headers_length, &at, &line)) count++;
    Line *lines = js_malloc(ctx, (2 * count + 1) * sizeof(*lines));
    char *out = lines != NULL ? js_malloc(ctx, x->response_headers_length * 2 + 1) : NULL;
    if (out == NULL) {
        js_free(ctx, lines);
        return JS_EXCEPTION;
    }
    Line *scratch = lines + count;
    at = 0;
    for (size_t i = 0; way_header_next(x->response_headers, x->response_headers_length, &at, &line); i++)
        lines[i] = (Line){line.name, line.value, line.name_len, line.value_len};
    /* Stable, so a name's values keep the order they came in. */
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t lo = 0; lo < count; lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count, hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) scratch[k++] = upper_order(&lines[j], &lines[i]) < 0 ? lines[j++] : lines[i++];
            while (i < mid) scratch[k++] = lines[i++];
            while (j < hi) scratch[k++] = lines[j++];
        }
        os64_memcpy(lines, scratch, count * sizeof(*lines));
        d_request_steps += count;
    }
    /* Each name once, lower-cased, its values joined by ", ": never longer
     * than the block's own lines, so twice its length is room enough. */
    size_t used = 0;
    for (size_t i = 0; i < count;) {
        size_t j = i + 1;
        for (size_t k = 0; k < lines[i].name_length; k++) {
            char c = lines[i].name[k];
            out[used++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
        out[used++] = ':';
        out[used++] = ' ';
        os64_memcpy(out + used, lines[i].value, lines[i].value_length);
        used += lines[i].value_length;
        while (j < count && d_same_nocase(lines[j].name, lines[j].name_length, lines[i].name, lines[i].name_length)) {
            out[used++] = ',';
            out[used++] = ' ';
            os64_memcpy(out + used, lines[j].value, lines[j].value_length);
            used += lines[j].value_length;
            j++;
        }
        out[used++] = '\r';
        out[used++] = '\n';
        d_request_steps += j - i;
        i = j;
    }
    JSValue text = d_byte_text(ctx, out, used);
    js_free(ctx, out);
    js_free(ctx, lines);
    return text;
}

/* The response attribute for a type other than text: made once, at DONE,
 * and the same object after; null when it cannot be made. */
static JSValue response_object(os64_dom_t *dom, JSContext *ctx, JSValueConst self)
{
    DXhr *x = xhr_state(self);
    if (x->state != DONE || x->error || x->response_failed) return JS_NULL;
    if (!JS_IsUndefined(x->response_object)) return JS_DupValue(ctx, x->response_object);
    const uint8_t *bytes = x->body != NULL ? x->body : (const uint8_t *)"";
    JSValue made = JS_NULL;
    switch (x->response_type) {
    case RT_ARRAYBUFFER: made = JS_NewArrayBufferCopy(ctx, bytes, x->body_length); break;
    case RT_BLOB: {
        char charset[WAY_MIME_CHARSET_MAX];
        DBytes copy = {js_malloc(ctx, x->body_length + 1), x->body_length, final_type(ctx, x, charset, sizeof(charset))};
        if (copy.bytes == NULL || copy.type == NULL) {
            d_bytes_free(ctx, &copy);
            made = JS_EXCEPTION;
        } else {
            if (x->body_length != 0) os64_memcpy(copy.bytes, bytes, x->body_length);
            made = d_blob_new(dom, ctx, &copy);
        }
        break;
    }
    case RT_JSON: {
        if (!x->has_body) return JS_NULL;
        JSValue text = d_utf8_decode(ctx, bytes, x->body_length);
        size_t n;
        const char *utf8 = JS_IsException(text) ? NULL : JS_ToCStringLen(ctx, &n, text);
        JS_FreeValue(ctx, text);
        made = utf8 != NULL ? JS_ParseJSON(ctx, utf8, n, "<response>") : JS_EXCEPTION;
        if (utf8 != NULL) JS_FreeCString(ctx, utf8);
        break;
    }
    default: return JS_NULL;    /* document: this browser makes none */
    }
    x = xhr_state(self);
    if (JS_IsException(made)) {
        /* A body that is not JSON, or no room to make the object: null,
         * and null again (the standard's "failure"). */
        JS_FreeValue(ctx, JS_GetException(ctx));
        x->response_failed = true;
        return JS_NULL;
    }
    x->response_object = JS_DupValue(ctx, made);
    return made;
}

static JSValue xhr_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    DXhr *x = xhr_state(self);
    if (x == NULL) return JS_ThrowTypeError(ctx, "Expected an XMLHttpRequest");
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    switch (magic) {
    case X_READY_STATE: return JS_NewInt32(ctx, x->state);
    case X_OPEN: return xhr_open(dom, ctx, self, argc, argv);
    case X_SET_HEADER: return xhr_set_header(dom, ctx, self, argc, argv);
    case X_TIMEOUT: {
        if (!set) return JS_NewUint32(ctx, x->timeout);
        uint32_t ms;
        if (JS_ToUint32(ctx, &ms, arg) < 0) return JS_EXCEPTION;
        x = xhr_state(self);
        if (x->sync && x->state == OPENED)
            return d_dom_exception(dom, ctx, "InvalidAccessError", "A synchronous request cannot have a timeout");
        x->timeout = ms;
        return timer_set(dom, ctx, self) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    case X_WITH_CREDENTIALS: {
        if (!set) return JS_NewBool(ctx, x->with_credentials);
        int flag = JS_ToBool(ctx, arg);
        if (flag < 0) return JS_EXCEPTION;
        x = xhr_state(self);
        if ((x->state != UNSENT && x->state != OPENED) || x->send_flag)
            return d_dom_exception(dom, ctx, "InvalidStateError", "withCredentials cannot change once sent");
        x->with_credentials = flag != 0;
        return JS_UNDEFINED;
    }
    case X_UPLOAD: return JS_DupValue(ctx, x->upload);
    case X_SEND: return xhr_send(dom, ctx, self, argc, argv);
    case X_ABORT: return xhr_abort(dom, ctx, self);
    case X_RESPONSE_URL: return JS_NewString(ctx, x->error || x->response_url == NULL ? "" : x->response_url);
    case X_STATUS: return JS_NewInt32(ctx, x->error ? 0 : x->status);
    case X_STATUS_TEXT:
        return x->error || x->status_text == NULL ? JS_NewString(ctx, "")
                                                  : d_byte_text(ctx, x->status_text, os64_strlen(x->status_text));
    case X_GET_HEADER: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "getResponseHeader needs a name");
        char *name;
        size_t nlen, length = 0;
        if (!d_byte_string(ctx, arg, &name, &nlen)) return JS_EXCEPTION;
        bool no_memory;
        char *value = response_header(ctx, xhr_state(self), name, nlen, &length, &no_memory);
        js_free(ctx, name);
        if (no_memory) return JS_EXCEPTION;
        JSValue result = value != NULL ? d_byte_text(ctx, value, length) : JS_NULL;
        js_free(ctx, value);
        return result;
    }
    case X_GET_ALL_HEADERS: return all_headers(ctx, x);
    case X_OVERRIDE_MIME: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "overrideMimeType needs a type");
        size_t n;
        const char *text = JS_ToCStringLen(ctx, &n, arg);
        if (text == NULL) return JS_EXCEPTION;
        x = xhr_state(self);
        if (x->state == LOADING || x->state == DONE) {
            JS_FreeCString(ctx, text);
            return d_dom_exception(dom, ctx, "InvalidStateError", "The response is already arriving");
        }
        char essence[WAY_MIME_ESSENCE_MAX], charset[WAY_MIME_CHARSET_MAX], out[WAY_MIME_ESSENCE_MAX + WAY_MIME_CHARSET_MAX + 16];
        if (way_mime_parse(text, n, essence, sizeof(essence), charset, sizeof(charset)))
            os64_snprintf(out, sizeof(out), charset[0] ? "%s;charset=%s" : "%s", essence, charset);
        else os64_strcopy(out, sizeof(out), "application/octet-stream");
        JS_FreeCString(ctx, text);
        char *copy = engine_copy(ctx, out, os64_strlen(out));
        if (copy == NULL) return JS_EXCEPTION;
        js_free(ctx, x->override_type);
        x->override_type = copy;
        return JS_UNDEFINED;
    }
    case X_RESPONSE_TYPE: {
        if (!set) return JS_NewString(ctx, kResponseTypes[x->response_type]);
        const char *text = JS_ToCString(ctx, arg);
        if (text == NULL) return JS_EXCEPTION;
        int found = -1;
        for (int i = 0; i < 6 && found < 0; i++)
            if (os64_streq(text, kResponseTypes[i])) found = i;
        JS_FreeCString(ctx, text);
        /* WebIDL: a value the enumeration does not name is ignored. */
        if (found < 0) return JS_UNDEFINED;
        x = xhr_state(self);
        if (x->state == LOADING || x->state == DONE)
            return d_dom_exception(dom, ctx, "InvalidStateError", "The response is already arriving");
        if (x->sync && x->state == OPENED)
            return d_dom_exception(dom, ctx, "InvalidAccessError", "A synchronous request cannot have a response type");
        x->response_type = found;
        return JS_UNDEFINED;
    }
    case X_RESPONSE:
        if (x->response_type == RT_EMPTY || x->response_type == RT_TEXT)
            return x->state == LOADING || x->state == DONE ? response_text(ctx, x) : JS_NewString(ctx, "");
        return response_object(dom, ctx, self);
    case X_RESPONSE_TEXT:
        if (x->response_type != RT_EMPTY && x->response_type != RT_TEXT)
            return d_dom_exception(dom, ctx, "InvalidStateError", "responseText is for a text response");
        return x->state == LOADING || x->state == DONE ? response_text(ctx, x) : JS_NewString(ctx, "");
    case X_RESPONSE_XML:
        if (x->response_type != RT_EMPTY && x->response_type != RT_DOCUMENT)
            return d_dom_exception(dom, ctx, "InvalidStateError", "responseXML is for a document response");
        /* This browser parses no document out of a response. */
        return JS_NULL;
    default: return JS_UNDEFINED;
    }
}

static JSValue xhr_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSClassID id = os64_js_class_id(&xhr_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DXhr *x = js_mallocz(ctx, sizeof(*x));
    if (x == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &x->target, object);
    x->upload = x->response_object = JS_UNDEFINED;
    x->error = true;
    JS_SetOpaque(object, x);
    JSValue upload = JS_NewObjectClass(ctx, (int)os64_js_class_id(&upload_slot));
    DUpload *u = JS_IsException(upload) ? NULL : js_mallocz(ctx, sizeof(*u));
    if (u == NULL) {
        JS_FreeValue(ctx, upload);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &u->target, upload);
    JS_SetOpaque(upload, u);
    xhr_state(object)->upload = upload;
    return object;
}

int d_xhr_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef xhr_class = {.class_name = "XMLHttpRequest", .finalizer = xhr_finalizer, .gc_mark = xhr_mark};
    static const JSClassDef upload_class = {.class_name = "XMLHttpRequestUpload", .finalizer = upload_finalizer,
                                            .gc_mark = upload_mark};
    JSClassID xhr_id = os64_js_class_id(&xhr_slot), upload_id = os64_js_class_id(&upload_slot);
    /* The events it fires are ProgressEvents, which arrive with it. */
    if (d_progress_event_install(dom, ctx, global) < 0) return -1;
    if ((!JS_IsRegisteredClass(dom->engine, xhr_id) && JS_NewClass(dom->engine, xhr_id, &xhr_class) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, upload_id) && JS_NewClass(dom->engine, upload_id, &upload_class) < 0))
        return -1;
    /* XMLHttpRequestEventTarget's prototype carries the EventTarget methods
     * and the seven handlers both inherit. */
    DValue *base = d_retain(dom, ctx, JS_NewObject(ctx));
    DValue *xhr = base != NULL ? d_retain(dom, ctx, JS_NewObjectProto(ctx, base->value)) : NULL;
    DValue *upload = xhr != NULL ? d_retain(dom, ctx, JS_NewObjectProto(ctx, base->value)) : NULL;
    if (upload == NULL) return -1;
    JS_SetClassProto(ctx, xhr_id, JS_DupValue(ctx, xhr->value));
    JS_SetClassProto(ctx, upload_id, JS_DupValue(ctx, upload->value));
    if (d_event_target(dom, ctx, base->value, xhr_id, NULL) < 0 ||
        d_event_target(dom, ctx, base->value, upload_id, NULL) < 0)
        return -1;
    static const char *const kHandlers[] = {"loadstart", "progress", "abort", "error", "load", "timeout", "loadend"};
    for (size_t i = 0; i < sizeof(kHandlers) / sizeof(kHandlers[0]); i++)
        if (d_event_handler_named(dom, ctx, base->value, kHandlers[i]) < 0) return -1;
    if (d_event_handler_named(dom, ctx, xhr->value, "readystatechange") < 0) return -1;
    static const struct { const char *name; int magic, argc; int kind; } kMembers[] = {
        {"readyState", X_READY_STATE, 0, 0}, {"open", X_OPEN, 2, 1}, {"setRequestHeader", X_SET_HEADER, 2, 1},
        {"timeout", X_TIMEOUT, 0, 2}, {"withCredentials", X_WITH_CREDENTIALS, 0, 2}, {"upload", X_UPLOAD, 0, 0},
        {"send", X_SEND, 0, 1}, {"abort", X_ABORT, 0, 1}, {"responseURL", X_RESPONSE_URL, 0, 0},
        {"status", X_STATUS, 0, 0}, {"statusText", X_STATUS_TEXT, 0, 0},
        {"getResponseHeader", X_GET_HEADER, 1, 1}, {"getAllResponseHeaders", X_GET_ALL_HEADERS, 0, 1},
        {"overrideMimeType", X_OVERRIDE_MIME, 1, 1}, {"responseType", X_RESPONSE_TYPE, 0, 2},
        {"response", X_RESPONSE, 0, 0}, {"responseText", X_RESPONSE_TEXT, 0, 0},
        {"responseXML", X_RESPONSE_XML, 0, 0},
    };
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++)
        if ((kMembers[i].kind == 1
                 ? d_method(dom, ctx, xhr->value, kMembers[i].name, xhr_member, kMembers[i].argc, kMembers[i].magic)
                 : d_accessor(dom, ctx, xhr->value, kMembers[i].name, xhr_member, kMembers[i].magic,
                              kMembers[i].kind == 2)) < 0)
            return -1;
    JSValue base_interface = d_interface_new(ctx, "XMLHttpRequestEventTarget", 0, NULL, base->value, JS_UNDEFINED);
    if (JS_IsException(base_interface)) return -1;
    JSValue upload_interface = d_interface_new(ctx, "XMLHttpRequestUpload", 0, NULL, upload->value, base_interface);
    JSValue xhr_interface = JS_IsException(upload_interface) ? JS_EXCEPTION
        : d_interface_new(ctx, "XMLHttpRequest", 0, xhr_constructor, xhr->value, base_interface);
    /* WebIDL constants: on the interface object and its prototype. */
    bool constants = !JS_IsException(xhr_interface);
    for (int i = 0; constants && i < 5; i++)
        constants = JS_DefinePropertyValueStr(ctx, xhr_interface, kStates[i], JS_NewInt32(ctx, i), JS_PROP_ENUMERABLE) >= 0 &&
                    JS_DefinePropertyValueStr(ctx, xhr->value, kStates[i], JS_NewInt32(ctx, i), JS_PROP_ENUMERABLE) >= 0;
    int result = constants ? 0 : -1;
    const char *names[] = {"XMLHttpRequestEventTarget", "XMLHttpRequestUpload", "XMLHttpRequest"};
    JSValue interfaces[] = {base_interface, upload_interface, xhr_interface};
    for (int i = 0; i < 3; i++) {
        if (result < 0 || JS_IsException(interfaces[i])) {
            JS_FreeValue(ctx, interfaces[i]);
            result = -1;
            continue;
        }
        if (JS_DefinePropertyValueStr(ctx, global, names[i], interfaces[i], JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            result = -1;
    }
    return result;
}
