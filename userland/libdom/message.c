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
 *     a copy the reader refuses with an exception a script could catch
 *     fires `messageerror` instead, with the same origin and source and no
 *     data. No page can reach that today: everything the reader can meet
 *     in a realm that wrote the record is the allocator's refusal or the
 *     deadline, both of which end the task (d_exception_catchable) rather
 *     than fire an event. The step is kept for the second realm frames
 *     bring, as HTML has it.
 *   - A message posted during a message's task is a task of its own, after
 *     the task that posted it: the setImmediate trick's whole premise.
 *
 * MESSAGE CHANNELS (HTML § 9.4): `new MessageChannel()` makes two
 * entangled MessagePorts; what one posts goes to the other, a MessageEvent
 * with no origin, no source and no ports, fired at the port receiving it.
 * A port's messages ride the window's queue above as tasks of their own,
 * so the host runs them as it runs the window's (os64_dom_message_fire),
 * each port's in the order sent; a port that has not started (start(), or
 * its onmessage set) keeps what it is sent on a list of its own until it
 * does, and those join the queue then, behind whatever was posted
 * meanwhile, as the standard's separate task sources allow. Each port
 * holds its partner, traced, so an entangled pair lives while either is
 * reachable, as a scheduler's channel must (React posts to one port and
 * hears on the other); a queued message holds the port it goes to until
 * its task runs. close() disentangles the pair: what is posted after goes
 * nowhere, and what reached the closed port is dropped. A MessagePort in a
 * transfer list is refused (DEBTS.md).
 *
 * The queue's nodes are the binding's memory; a message's clone is the
 * engine's, charged as the page's own values are; the poster's window each
 * holds is one of the registry's values (messages_held). A port's message
 * holds its port instead, an engine value no registry counts, freed with
 * the message (d_message_drain at teardown). */

/* What a message's targetOrigin asked for. An opaque origin is the same
 * as itself only: "/" from a document with one is the poster's own, which
 * the window it is posted to has, being the poster's. A URL with an
 * opaque origin is no document's. */
enum { T_ANY, T_POSTER, T_ORIGIN, T_NONE };

struct DMessage {
    DMessage *next;
    DClone *clone;
    JSValue source;
    /* A port's message: the MessagePort it goes to, held while the message
     * is on the window's queue; JS_UNDEFINED for the window's own messages
     * and for one waiting on its port's list for the port to start. */
    JSValue port;
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
    JS_FreeValueRT(dom->engine, message->port);
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
    if (converted == 0 && !d_require_active(target, ctx)) converted = -1;
    DMessage *message = converted == 0 ? d_alloc(target, sizeof(*message)) : NULL;
    if (message != NULL) {
        *message = (DMessage){.source = JS_UNDEFINED, .port = JS_UNDEFINED, .posted = now_ms(target)};
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
    /* Serialization can run getters that retire the posting activity. */
    if (converted == 0 && !d_require_active(target, ctx)) converted = -1;
    /* Posted: the transfer list is the message's now. */
    if (converted == 0) d_clone_detach(ctx, transfer, count);
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
 * read in the target's realm, or messageerror when the reader refused with
 * a catchable exception (unreachable today; the header says why). */
static JSValue message_make(os64_dom_t *dom, JSContext *ctx, void *arg)
{
    DMessage *message = arg;
    /* A port's message has no origin and no source (HTML § 9.4.4). */
    bool port = !JS_IsUndefined(message->port);
    const char *origin = port ? "" : message->origin;
    JSValueConst source = port ? JS_NULL : message->source;
    JSValue data = d_clone_read(dom, ctx, message->clone);
    if (!JS_IsException(data))
        return d_message_event_new(dom, ctx, "message", data, origin, source);
    if (!d_exception_catchable(ctx)) return JS_EXCEPTION;
    JS_FreeValue(ctx, JS_GetException(ctx));
    return d_message_event_new(dom, ctx, "messageerror", JS_NULL, origin, source);
}

static DValue *port_target(JSValueConst port);

uint64_t os64_dom_message_next(const os64_dom_t *dom)
{
    return d_delivery_active(dom) && dom->messages != NULL ? dom->messages->posted : UINT64_MAX;
}

bool os64_dom_message_fire(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    if (outcome != NULL) os64_memset(outcome, 0, sizeof(*outcome));
    if (outcome == NULL || !d_delivery_active(dom)) return false;
    for (bool dropped = false; d_delivery_active(dom) && dom->messages != NULL; dropped = true) {
        DMessage *message = dom->messages;
        /* Past one dropped, a message posted after the page's next timer
         * fell due waits for that timer's task, which may add a listener
         * for it. */
        if (dropped && os64_dom_timer_next(dom) <= message->posted) return false;
        dom->messages = message->next;
        if (dom->messages == NULL) dom->messages_last = NULL;
        /* A port's message goes to its port unless that port has closed;
         * the window's to the window when its targetOrigin still holds and
         * something listens. */
        DValue *port = JS_IsUndefined(message->port) ? NULL : port_target(message->port);
        bool heard = !JS_IsUndefined(message->port)
            ? port != NULL
            : target_matches(dom, message) &&
              (os64_dom_listens(dom, "message") || os64_dom_listens(dom, "messageerror"));
        if (heard) d_host_dispatch(dom, NULL, port, "message", message_make, message, NULL, outcome);
        message_free(dom, message);
        if (heard) return true;
    }
    return false;
}

/* ── MessageChannel and MessagePort (HTML § 9.4) ─────────────────────── */

static JSClassID port_slot, channel_slot;

typedef struct DPort {
    DValue target;              /* first: the plain target's entry, in the object */
    os64_dom_t *dom;
    JSValue partner;            /* the entangled port, traced; JS_UNDEFINED once disentangled */
    bool started, closed;
    DMessage *pending, *pending_last;   /* sent before it started; each holding no port */
    struct DPort *previous, *next;      /* weak membership in dom->ports */
} DPort;

typedef struct {
    JSValue port1, port2;
} DChannel;

static DPort *port_of(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&port_slot));
}

/* The entry a port's message is dispatched at, or NULL once it closed. */
static DValue *port_target(JSValueConst port)
{
    DPort *p = port_of(port);
    return p != NULL && !p->closed ? &p->target : NULL;
}

static void message_enqueue(os64_dom_t *dom, DMessage *message)
{
    message->next = NULL;
    if (dom->messages_last != NULL) dom->messages_last->next = message;
    else dom->messages = message;
    dom->messages_last = message;
}

/* What waited for the port goes on the window's queue, each now holding
 * the port, in the order it was sent. */
static void port_start(JSContext *ctx, JSValueConst self, DPort *p)
{
    if (p->started || p->closed || !d_delivery_active(p->dom)) return;
    p->started = true;
    while (p->pending != NULL) {
        DMessage *message = p->pending;
        p->pending = message->next;
        message->port = JS_DupValue(ctx, self);
        message_enqueue(p->dom, message);
    }
    p->pending_last = NULL;
}

static void port_pending_free(JSRuntime *rt, DPort *p)
{
    (void)rt;
    while (p->pending != NULL) {
        DMessage *message = p->pending;
        p->pending = message->next;
        message_free(p->dom, message);
    }
    p->pending_last = NULL;
}

/* Release task payloads without closing exported ports or destroying their
 * event-target records. Queued tasks can release the last hold on a port,
 * so finish that queue before walking the surviving weak port list. */
void d_message_cancel(os64_dom_t *dom)
{
    while (dom->messages != NULL) {
        DMessage *message = dom->messages;
        dom->messages = message->next;
        message_free(dom, message);
    }
    dom->messages_last = NULL;
    for (DPort *p = dom->ports; p != NULL; p = p->next)
        port_pending_free(dom->engine, p);
}

void d_message_drain(os64_dom_t *dom)
{
    d_message_cancel(dom);
    while (dom->ports != NULL) {
        DPort *p = dom->ports;
        dom->ports = p->next;
        p->previous = p->next = NULL;
    }
}

/* Disentangled: neither port holds the other, and what this one was sent
 * and had not started on goes. */
static void port_close(JSContext *ctx, DPort *p)
{
    p->closed = true;
    DPort *other = port_of(p->partner);
    JSValue partner = p->partner;
    p->partner = JS_UNDEFINED;
    if (other != NULL) {
        JSValue back = other->partner;
        other->partner = JS_UNDEFINED;
        JS_FreeValue(ctx, back);
    }
    JS_FreeValue(ctx, partner);
    port_pending_free(JS_GetRuntime(ctx), p);
}

static void port_finalizer(JSRuntime *rt, JSValue value)
{
    DPort *p = port_of(value);
    if (p == NULL) return;
    if (p->previous != NULL) p->previous->next = p->next;
    else if (p->dom->ports == p) p->dom->ports = p->next;
    if (p->next != NULL) p->next->previous = p->previous;
    port_pending_free(rt, p);
    JS_FreeValueRT(rt, p->partner);
    d_event_target_finalize(rt, &p->target);
    js_free_rt(rt, p);
}

static void port_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DPort *p = port_of(value);
    if (p == NULL) return;
    d_event_target_mark(rt, &p->target, mark);
    JS_MarkValue(rt, p->partner, mark);
}

static JSValue port_new(os64_dom_t *dom, JSContext *ctx)
{
    JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&port_slot));
    if (JS_IsException(object)) return object;
    DPort *p = js_mallocz(ctx, sizeof(*p));
    if (p == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    p->dom = dom;
    p->partner = JS_UNDEFINED;
    d_event_target_init(dom, &p->target, object);
    JS_SetOpaque(object, p);
    p->next = dom->ports;
    if (p->next != NULL) p->next->previous = p;
    dom->ports = p;
    return object;
}

enum { P_POST, P_START, P_CLOSE };

static bool same_object(JSValueConst a, JSValueConst b)
{
    return JS_IsObject(a) && JS_IsObject(b) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

/* WebIDL's overload resolution between a sequence and a dictionary: an
 * object with an @@iterator is the sequence (an array, a Set, any
 * iterable). 1, 0, or -1 with the exception pending. */
static int is_sequence(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    if (!JS_IsObject(value) || dom->iterator_symbol == NULL) return 0;
    JSAtom key = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
    if (key == JS_ATOM_NULL) return -1;
    JSValue method = JS_GetProperty(ctx, value, key);
    JS_FreeAtom(ctx, key);
    if (JS_IsException(method)) return -1;
    int sequence = !JS_IsUndefined(method) && !JS_IsNull(method);
    JS_FreeValue(ctx, method);
    return sequence;
}

/* postMessage(message, transfer) and postMessage(message, {transfer})
 * (HTML's message port post message steps): the overloads told apart as
 * WebIDL tells them (is_sequence), the message serialized whatever becomes
 * of it, then sent to the port still entangled once it is written, or
 * nowhere when there is none. */
static JSValue port_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                           JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DPort *p = port_of(self);
    if (p == NULL) return JS_ThrowTypeError(ctx, "Expected a MessagePort");
    dom = p->dom;
    if (magic == P_START) {
        port_start(ctx, self, p);
        return JS_UNDEFINED;
    }
    if (magic == P_CLOSE) {
        port_close(ctx, p);
        return JS_UNDEFINED;
    }
    if (argc < 1) return JS_ThrowTypeError(ctx, "postMessage needs a message");
    JSValue *transfer = NULL;
    size_t count = 0;
    int sequence = argc > 1 ? is_sequence(dom, ctx, argv[1]) : 0;
    int converted = sequence < 0 ? -1 : 0;
    if (sequence > 0) {
        converted = d_sequence(dom, ctx, argv[1], D_SEQUENCE_MAX, &transfer, &count);
        if (converted == 0) converted = d_clone_objects(ctx, transfer, count);
    } else if (converted == 0) {
        converted = d_clone_transfer(dom, ctx, argc, argv, 1, &transfer, &count);
    }
    for (size_t i = 0; converted == 0 && i < count; i++)
        if (port_of(transfer[i]) != NULL) {
            d_error(ctx, "DataCloneError", port_of(transfer[i]) == p ? "A port cannot transfer itself"
                                                                     : "A MessagePort is not transferred here");
            converted = -1;
        }
    if (converted == 0 && !d_require_active(dom, ctx)) converted = -1;
    /* The receiver held while the message is written: serializing runs the
     * page's getters, which may close either port, and closing drops the
     * last other hold on it. The node first, so a quota refusal detaches
     * nothing; then the message written and the transfer list detached,
     * which happens even with no port to receive it, as the standard
     * serializes it all the same; then the pair asked again. */
    JSValue receiver = JS_DupValue(ctx, p->partner);
    DPort *target = port_of(receiver);
    bool live = target != NULL && !target->closed;
    DMessage *message = converted == 0 && live ? d_alloc(dom, sizeof(*message)) : NULL;
    if (converted == 0 && live && message == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM message quota exceeded");
        converted = -1;
    }
    DClone *clone = converted == 0 ? d_clone_write(dom, ctx, argv[0], transfer, count) : NULL;
    if (clone != NULL && !d_require_active(dom, ctx)) {
        d_clone_free(dom->engine, clone);
        clone = NULL;
    }
    if (clone != NULL) d_clone_detach(ctx, transfer, count);
    d_sequence_free(ctx, transfer, count);
    live = live && !p->closed && !target->closed && d_delivery_active(target->dom) &&
           same_object(p->partner, receiver);
    if (clone == NULL || !live) {
        d_free(dom, message);
        if (clone != NULL) d_clone_free(dom->engine, clone);
        JS_FreeValue(ctx, receiver);
        return clone == NULL ? JS_EXCEPTION : JS_UNDEFINED;
    }
    *message = (DMessage){.clone = clone, .source = JS_UNDEFINED, .port = JS_UNDEFINED, .target_kind = T_ANY,
                          .posted = now_ms(dom)};
    if (target->started) {
        message->port = receiver;
        message_enqueue(target->dom, message);
    } else {
        JS_FreeValue(ctx, receiver);
        if (target->pending_last != NULL) target->pending_last->next = message;
        else target->pending = message;
        target->pending_last = message;
    }
    return JS_UNDEFINED;
}

/* A port's onmessage starts it the first time it is set, as HTML's
 * MessagePort has it; addEventListener does not. data[1] is the generic
 * handler setter this one wraps. */
static JSValue port_onmessage_set(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                  JSValue *data)
{
    (void)magic;
    JSValue result = JS_Call(ctx, data[1], self, argc, argv);
    DPort *p = port_of(self);
    if (!JS_IsException(result) && p != NULL) port_start(ctx, self, p);
    return result;
}

static int port_onmessage(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype)
{
    if (d_event_handler_named(dom, ctx, prototype, "message") < 0) return -1;
    JSAtom atom = JS_NewAtom(ctx, "onmessage");
    if (atom == JS_ATOM_NULL) return -1;
    JSPropertyDescriptor desc;
    int found = JS_GetOwnProperty(ctx, &desc, prototype, atom);
    if (found <= 0) {
        JS_FreeAtom(ctx, atom);
        return -1;
    }
    JSValue wrap_data[2] = {dom->anchor->value, desc.setter};
    JSValue set = JS_NewCFunctionData(ctx, port_onmessage_set, 1, 0, 2, wrap_data);
    JS_FreeValue(ctx, desc.setter);
    JS_FreeValue(ctx, desc.value);
    int result = JS_IsException(set) ? -1
               : JS_DefinePropertyGetSet(ctx, prototype, atom, desc.getter, set,
                                         JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    if (JS_IsException(set)) JS_FreeValue(ctx, desc.getter);
    JS_FreeAtom(ctx, atom);
    return result < 0 ? -1 : 0;
}

static void channel_finalizer(JSRuntime *rt, JSValue value)
{
    DChannel *c = JS_GetOpaque(value, os64_js_class_id(&channel_slot));
    if (c == NULL) return;
    JS_FreeValueRT(rt, c->port1);
    JS_FreeValueRT(rt, c->port2);
    js_free_rt(rt, c);
}

static void channel_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DChannel *c = JS_GetOpaque(value, os64_js_class_id(&channel_slot));
    if (c == NULL) return;
    JS_MarkValue(rt, c->port1, mark);
    JS_MarkValue(rt, c->port2, mark);
}

/* new MessageChannel(): two new ports, entangled. */
static JSValue channel_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    if (!d_require_active(dom, ctx)) {
        JS_FreeValue(ctx, prototype);
        return JS_EXCEPTION;
    }
    JSClassID id = os64_js_class_id(&channel_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DChannel *c = js_mallocz(ctx, sizeof(*c));
    if (c == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    c->port1 = c->port2 = JS_UNDEFINED;
    JS_SetOpaque(object, c);
    c->port1 = port_new(dom, ctx);
    c->port2 = JS_IsException(c->port1) ? JS_EXCEPTION : port_new(dom, ctx);
    if (JS_IsException(c->port2)) {
        if (JS_IsException(c->port1)) c->port1 = JS_UNDEFINED;
        c->port2 = JS_UNDEFINED;
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    port_of(c->port1)->partner = JS_DupValue(ctx, c->port2);
    port_of(c->port2)->partner = JS_DupValue(ctx, c->port1);
    return object;
}

static JSValue channel_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    (void)argc; (void)argv;
    if (d_callback(ctx, data, OS64_JS_ABI_ID) == NULL) return JS_EXCEPTION;
    DChannel *c = JS_GetOpaque(self, os64_js_class_id(&channel_slot));
    if (c == NULL) return JS_ThrowTypeError(ctx, "Expected a MessageChannel");
    return JS_DupValue(ctx, magic == 0 ? c->port1 : c->port2);
}

/* The two interfaces: MessageChannel constructs; MessagePort is an Illegal
 * constructor whose prototype inherits EventTarget's (the targets row makes
 * the interface inherit EventTarget), with postMessage, start, close,
 * onmessage and onmessageerror. */
static int channel_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef port_def = {.class_name = "MessagePort", .finalizer = port_finalizer,
                                        .gc_mark = port_mark};
    static const JSClassDef channel_def = {.class_name = "MessageChannel", .finalizer = channel_finalizer,
                                           .gc_mark = channel_mark};
    JSClassID port_id = os64_js_class_id(&port_slot), channel_id = os64_js_class_id(&channel_slot);
    if ((!JS_IsRegisteredClass(dom->engine, port_id) && JS_NewClass(dom->engine, port_id, &port_def) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, channel_id) && JS_NewClass(dom->engine, channel_id, &channel_def) < 0))
        return -1;
    DValue *port = d_retain(dom, ctx, JS_NewObjectProto(ctx, dom->event_target_prototype->value));
    DValue *channel = port != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (channel == NULL) return -1;
    JS_SetClassProto(ctx, port_id, JS_DupValue(ctx, port->value));
    JS_SetClassProto(ctx, channel_id, JS_DupValue(ctx, channel->value));
    if (d_event_target(dom, ctx, port->value, port_id, NULL) < 0 ||
        d_method_named(dom, ctx, port->value, "postMessage", port_member, 1, P_POST) < 0 ||
        d_method_named(dom, ctx, port->value, "start", port_member, 0, P_START) < 0 ||
        d_method_named(dom, ctx, port->value, "close", port_member, 0, P_CLOSE) < 0 ||
        port_onmessage(dom, ctx, port->value) < 0 ||
        d_event_handler_named(dom, ctx, port->value, "messageerror") < 0 ||
        d_accessor(dom, ctx, channel->value, "port1", channel_member, 0, false) < 0 ||
        d_accessor(dom, ctx, channel->value, "port2", channel_member, 1, false) < 0)
        return -1;
    JSValue port_interface = d_interface_new(ctx, "MessagePort", 0, NULL, port->value, JS_UNDEFINED);
    if (JS_IsException(port_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "MessagePort", port_interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    JSValue channel_interface = d_interface_new(ctx, "MessageChannel", 0, channel_construct, channel->value,
                                                JS_UNDEFINED);
    return JS_IsException(channel_interface) ||
           JS_DefinePropertyValueStr(ctx, global, "MessageChannel", channel_interface,
                                     JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
}

int d_message_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (d_clone_setup(dom, ctx, global) < 0 || d_message_event_install(dom, ctx, global) < 0 ||
        d_method_named(dom, ctx, global, "postMessage", post_message, 1, 0) < 0 ||
        channel_install(dom, ctx, global) < 0)
        return -1;
    return d_event_handler(dom, ctx, global, D_EVENT_MESSAGE) < 0 ||
           d_event_handler(dom, ctx, global, D_EVENT_MESSAGEERROR) < 0 ? -1 : 0;
}
