#include "internal.h"
#include "os64/fmt.h"

/* POSTED MESSAGES (HTML § 9.3.3, "Posting messages"): window.postMessage
 * to the window itself, onmessage and onmessageerror, P2b's row
 * (WINDOW_DOCUMENT.md § P2b). It is IFRAME.md's postMessage (§ The cluster
 * and the window proxy) with the target the caller: no frame and no other
 * window exists for a page here to name, so every message a window posts
 * is to itself. Each step below still takes the window it posts to and the
 * window that posted as two things, which here are one binding, so frames
 * add the second realm and change no step's shape.
 *
 *   - postMessage(message, targetOrigin, transfer) and postMessage(message,
 *     {transfer, targetOrigin}): targetOrigin "*" is any document, "/" the
 *     poster's own origin (the default), and anything else must parse as
 *     an absolute URL, whose origin it is, or the call is a SyntaxError
 *     before the message is serialized. Then the message is written (the
 *     clone, clone.c) in the poster's realm, its transfer list's buffers
 *     detached, and queued at the target window: one task per message,
 *     oldest first, run by the host (os64_dom_message_fire).
 *   - The task: when the target's document is not of the targetOrigin's
 *     origin as it runs, the message is dropped without a word, as HTML
 *     drops it; when nothing could hear a message or a messageerror
 *     (os64_dom_listens), it is dropped without entering the engine, which
 *     no page can tell. Otherwise the clone is read in the target's realm
 *     and a MessageEvent `message` is fired at the window, its data the
 *     copy, its origin the poster's document's origin serialized, its
 *     source the poster's window, its lastEventId "" and its ports empty;
 *     a copy the target's realm cannot make (no memory for it) fires
 *     `messageerror` instead, with the same origin and source and no data.
 *   - A message posted during a message's task is a task of its own, after
 *     the task that posted it: the setImmediate trick's whole premise.
 *
 * The queue's nodes are the binding's memory; a message's clone is the
 * engine's, charged as the page's own values are; the poster's window each
 * holds is one of the registry's values (messages_held). */

/* What a message's targetOrigin asked for. An opaque origin is the same
 * as itself only: "/" from a document with one is the poster's own, which
 * the window it is posted to has, being the poster's. A URL with an
 * opaque origin is no document's. */
enum { T_ANY, T_POSTER, T_ORIGIN, T_NONE };

struct DMessage {
    DMessage *next;
    DClone *clone;
    JSValue source;
    char *origin;              /* the poster's document's origin, serialized */
    char *target;              /* T_ORIGIN: the origin the target's document must have */
    uint8_t target_kind;
    uint64_t posted;           /* the host's now_ms when it was posted */
};

static uint64_t now_ms(const os64_dom_t *dom)
{
    return dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) : 0;
}

static const char *page_address(const os64_dom_t *dom)
{
    return dom->url != NULL ? dom->url : "about:blank";
}

static char *text_copy(os64_dom_t *dom, const char *text)
{
    size_t length = os64_strlen(text);
    char *copy = d_alloc(dom, length + 1);
    if (copy != NULL) os64_memcpy(copy, text, length + 1);
    return copy;
}

/* A message's engine half released (the registry's value, the clone), and
 * its node freed. */
static void message_free(os64_dom_t *dom, DMessage *message)
{
    if (!JS_IsUndefined(message->source)) {
        JS_FreeValueRT(dom->engine, message->source);
        dom->messages_held--;
    }
    d_clone_free(dom->engine, message->clone);
    d_free(dom, message->origin);
    d_free(dom, message->target);
    d_free(dom, message);
}

/* postMessage's targetOrigin, parsed as HTML's window post message steps
 * parse it, into the message. -1 with an exception pending. */
static int target_origin(os64_dom_t *dom, JSContext *ctx, DString *text, DMessage *message)
{
    if (text->data == NULL || (text->length == 1 && text->data[0] == '/')) {
        char origin[WAY_ORIGIN_TEXT_MAX];
        bool tuple = d_origin(page_address(dom), origin, sizeof(origin)) && !os64_streq(origin, "null");
        message->target_kind = tuple ? T_ORIGIN : T_POSTER;
        message->target = tuple ? text_copy(dom, origin) : NULL;
    } else if (text->length == 1 && text->data[0] == '*') {
        message->target_kind = T_ANY;
        return 0;
    } else {
        char *parsed = d_alloc(dom, OS64_DOM_URL_MAX);
        if (parsed == NULL) {
            d_error(ctx, "QuotaExceededError", "DOM message quota exceeded");
            return -1;
        }
        int result = d_url_parse_absolute(dom, ctx, text, parsed);
        if (result <= 0) {
            d_free(dom, parsed);
            if (result == 0) d_error(ctx, "SyntaxError", "postMessage's targetOrigin is not a URL");
            return -1;
        }
        char origin[WAY_ORIGIN_TEXT_MAX];
        bool tuple = d_origin(parsed, origin, sizeof(origin)) && !os64_streq(origin, "null");
        d_free(dom, parsed);
        message->target_kind = tuple ? T_ORIGIN : T_NONE;
        message->target = tuple ? text_copy(dom, origin) : NULL;
    }
    if (message->target_kind == T_ORIGIN && message->target == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM message quota exceeded");
        return -1;
    }
    return 0;
}

/* window.postMessage: the window it is a member of is the target, and the
 * window whose script called it the poster; WebIDL's overloads told apart
 * as it tells them (a third argument, or a second that is a string, a
 * number or a boolean, is the targetOrigin form). */
static JSValue post_message(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *target = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (target == NULL) return JS_EXCEPTION;
    os64_dom_t *poster = target;
    if (argc < 1) return JS_ThrowTypeError(ctx, "postMessage needs a message");
    bool string_form = argc >= 3 ||
        (argc == 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && !JS_IsObject(argv[1]));
    DString origin_text = {0};
    JSValue *transfer = NULL;
    size_t count = 0;
    int converted = 0;
    if (string_form) {
        converted = d_string_text(poster, ctx, argv[1], &origin_text, true) ? 0 : -1;
        if (converted == 0 && argc >= 3 && !JS_IsUndefined(argv[2])) {
            converted = d_sequence(poster, ctx, argv[2], D_SEQUENCE_MAX, &transfer, &count);
            if (converted == 0) converted = d_clone_objects(ctx, transfer, count);
        }
    } else {
        /* WindowPostMessageOptions: the inherited member, transfer, read
         * before its own, targetOrigin. */
        converted = d_clone_transfer(poster, ctx, argc, argv, 1, &transfer, &count);
        if (converted == 0 && argc > 1 && JS_IsObject(argv[1])) {
            JSValue value = JS_GetPropertyStr(ctx, argv[1], "targetOrigin");
            converted = JS_IsException(value) ? -1
                      : JS_IsUndefined(value) ? 0 : d_string_text(poster, ctx, value, &origin_text, true) ? 0 : -1;
            JS_FreeValue(ctx, value);
        }
    }
    DMessage *message = converted == 0 ? d_alloc(target, sizeof(*message)) : NULL;
    if (message != NULL) {
        *message = (DMessage){.source = JS_UNDEFINED, .posted = now_ms(target)};
        char origin[WAY_ORIGIN_TEXT_MAX];
        message->origin = text_copy(target, d_origin(page_address(poster), origin, sizeof(origin)) ? origin : "null");
        if (message->origin == NULL) {
            d_error(ctx, "QuotaExceededError", "DOM message quota exceeded");
            converted = -1;
        } else converted = target_origin(poster, ctx, &origin_text, message);
    } else if (converted == 0) {
        d_error(ctx, "QuotaExceededError", "DOM message quota exceeded");
        converted = -1;
    }
    d_string_free(poster, &origin_text);
    /* Written in the poster's realm, after every argument and the
     * targetOrigin, so a refusal of either detaches nothing. */
    if (converted == 0) {
        message->clone = d_clone_write(poster, ctx, argv[0], transfer, count);
        converted = message->clone != NULL ? 0 : -1;
    }
    d_sequence_free(ctx, transfer, count);
    if (converted < 0) {
        if (message != NULL) message_free(target, message);
        return JS_EXCEPTION;
    }
    message->source = JS_GetGlobalObject(ctx);
    target->messages_held++;
    if (target->messages_last != NULL) target->messages_last->next = message;
    else target->messages = message;
    target->messages_last = message;
    return JS_UNDEFINED;
}

/* Whether the target's document is of the origin the message asked for,
 * as the task runs. */
static bool target_matches(const os64_dom_t *dom, const DMessage *message)
{
    if (message->target_kind == T_ANY || message->target_kind == T_POSTER) return true;
    if (message->target_kind == T_NONE) return false;
    char origin[WAY_ORIGIN_TEXT_MAX];
    return d_origin(page_address(dom), origin, sizeof(origin)) && os64_streq(origin, message->target);
}

/* The event a message's task fires, made in the task: its data the clone
 * read in the target's realm, or messageerror when that realm could not
 * make the copy. */
static JSValue message_make(os64_dom_t *dom, JSContext *ctx, void *arg)
{
    DMessage *message = arg;
    JSValue data = d_clone_read(dom, ctx, message->clone);
    if (!JS_IsException(data))
        return d_message_event_new(dom, ctx, "message", data, message->origin, message->source);
    if (!d_exception_catchable(ctx)) return JS_EXCEPTION;
    JS_FreeValue(ctx, JS_GetException(ctx));
    return d_message_event_new(dom, ctx, "messageerror", JS_NULL, message->origin, message->source);
}

uint64_t os64_dom_message_next(const os64_dom_t *dom)
{
    return dom != NULL && !dom->closed && dom->messages != NULL ? dom->messages->posted : UINT64_MAX;
}

bool os64_dom_message_fire(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    if (outcome != NULL) os64_memset(outcome, 0, sizeof(*outcome));
    if (outcome == NULL || dom == NULL) return false;
    while (!dom->closed && dom->messages != NULL) {
        DMessage *message = dom->messages;
        dom->messages = message->next;
        if (dom->messages == NULL) dom->messages_last = NULL;
        bool heard = target_matches(dom, message) &&
            (os64_dom_listens(dom, "message") || os64_dom_listens(dom, "messageerror"));
        if (heard) d_host_dispatch(dom, NULL, NULL, "message", message_make, message, NULL, outcome);
        message_free(dom, message);
        if (heard) return true;
    }
    return false;
}

void d_message_drain(os64_dom_t *dom)
{
    while (dom->messages != NULL) {
        DMessage *message = dom->messages;
        dom->messages = message->next;
        message_free(dom, message);
    }
    dom->messages_last = NULL;
}

int d_message_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (d_clone_setup(dom, ctx, global) < 0 || d_message_event_install(dom, ctx, global) < 0 ||
        d_method_named(dom, ctx, global, "postMessage", post_message, 1, 0) < 0)
        return -1;
    return d_event_handler(dom, ctx, global, D_EVENT_MESSAGE) < 0 ||
           d_event_handler(dom, ctx, global, D_EVENT_MESSAGEERROR) < 0 ? -1 : 0;
}
