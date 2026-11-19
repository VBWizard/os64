#include "internal.h"
#include "os64/fmt.h"

#include <math.h>

/* THE MEDIA SESSION (W3C Media Session; AUDIO_YONDER.md § Y.6).
 * navigator.mediaSession is how a page answers the media keys: an action
 * handler a press runs, the metadata and position a player shows, and the
 * playback state it declares. The host asks two things, between tasks:
 * what the window says of itself (os64_dom_media_session_state, read with
 * the media elements' own states), and a press done (os64_dom_media_session
 * _action): the page's handler for it, or with none the browser's own
 * answer (media.c's d_media_session_default).
 *
 * A handler is a scheduled callback held by the session, one an action.
 * The one a press runs is taken out of its slot for the call and put back
 * after unless the page set that action again meanwhile, so a handler that
 * replaces or removes itself never frees the callback being called. The
 * metadata is held as a property of a private holder object, so the engine
 * keeps it alive and lets it go. */

static JSClassID session_slot, metadata_slot;

static const char *const kActions[] = {
    "play", "pause", "seekbackward", "seekforward", "previoustrack", "nexttrack", "skipad", "stop", "seekto",
    "togglemicrophone", "togglecamera", "hangup", "previousslide", "nextslide", "enterpictureinpicture",
    "voiceactivity",
};
#define ACTIONS ((int)(sizeof(kActions) / sizeof(kActions[0])))
enum { A_PLAY = 0, A_PAUSE = 1, A_PREVIOUS = 4, A_NEXT = 5, A_STOP = 7 };

static const char *const kStates[] = {"none", "paused", "playing"};
enum { S_NONE, S_PAUSED, S_PLAYING };

struct DMediaSession {
    os64_dom_t *dom;            /* the binding whose session this is */
    DScheduledCallback handlers[ACTIONS];
    uint32_t scripts[ACTIONS];
    os64_dom_t *owners[ACTIONS];
    bool calling[ACTIONS];      /* the handler is out of its slot, being called */
    bool replaced[ACTIONS];     /* and the page set this action during the call */
    DValue *object;             /* the MediaSession */
    DValue *holder;             /* its metadata, as the holder's "metadata" */
    uint8_t declared;           /* playbackState, S_* */
    bool position_set;
    double duration, position, rate;
};

/* ── MediaMetadata ───────────────────────────────────────────────────── */

/* An instance's members live on a plain object of its own (`slots`): title,
 * artist and album as strings, artwork as the frozen list read back. */
typedef struct {
    JSValue slots;
} DMetadata;

static DMetadata *metadata_of(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&metadata_slot));
}

static void metadata_finalizer(JSRuntime *rt, JSValue value)
{
    DMetadata *m = metadata_of(value);
    if (m == NULL) return;
    JS_FreeValueRT(rt, m->slots);
    js_free_rt(rt, m);
}

static void metadata_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DMetadata *m = metadata_of(value);
    if (m != NULL) JS_MarkValue(rt, m->slots, mark);
}

/* A frozen object or list as Object.freeze leaves one, without calling it:
 * every own property made unwritable and unconfigurable, then closed. */
static int freeze(JSContext *ctx, JSValueConst object, const char *const *names, uint32_t count, bool list)
{
    for (uint32_t i = 0; i < count; i++) {
        JSValue v = list ? JS_GetPropertyUint32(ctx, object, i) : JS_GetPropertyStr(ctx, object, names[i]);
        if (JS_IsException(v)) return -1;
        int done = list ? JS_DefinePropertyValueUint32(ctx, object, i, v, JS_PROP_ENUMERABLE)
                        : JS_DefinePropertyValueStr(ctx, object, names[i], v, JS_PROP_ENUMERABLE);
        if (done < 0) return -1;
    }
    if (list && JS_DefinePropertyValueStr(ctx, object, "length", JS_NewUint32(ctx, count), 0) < 0) return -1;
    return JS_PreventExtensions(ctx, object) < 0 ? -1 : 0;
}

/* A dictionary member read as a DOMString, `fallback` when absent; NULL
 * value (JS_EXCEPTION) with the exception pending. */
static JSValue string_member(JSContext *ctx, JSValueConst dict, const char *name, const char *fallback, bool required,
                             const char *what)
{
    JSValue v = JS_IsUndefined(dict) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, dict, name);
    if (JS_IsException(v)) return v;
    if (JS_IsUndefined(v)) {
        if (required) return JS_ThrowTypeError(ctx, "%s needs its %s", what, name);
        return JS_NewString(ctx, fallback);
    }
    JSValue s = JS_ToString(ctx, v);
    JS_FreeValue(ctx, v);
    return s;
}

/* MediaImage artwork, in two halves as WebIDL and the standard order them.
 * images_read is the sequence<MediaImage> conversion (d_sequence walks the
 * value's own iterator, a value with none is a TypeError), each image's
 * members read in lexicographic order, src required: a list of plain
 * {src, sizes, type} with src as written. images_resolve is the
 * standard's convert-artwork steps: each src parsed against the
 * document's URL (one that does not parse is a TypeError), into a frozen
 * list of frozen objects. */
static JSValue images_read(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    JSValue *items = NULL;
    size_t count = 0;
    if (d_sequence(dom, ctx, value, D_SEQUENCE_MAX, &items, &count) < 0) return JS_EXCEPTION;
    JSValue out = JS_NewArray(ctx);
    static const char *const kFields[] = {"sizes", "src", "type"};
    for (size_t i = 0; i < count && !JS_IsException(out); i++) {
        JSValue image = JS_EXCEPTION;
        if (!JS_IsObject(items[i]))
            JS_ThrowTypeError(ctx, "Each MediaImage must be a dictionary");
        else
            image = JS_NewObject(ctx);
        for (int f = 0; f < 3 && !JS_IsException(image); f++) {
            JSValue v = string_member(ctx, items[i], kFields[f], "", f == 1, "A MediaImage");
            if (JS_IsException(v) || JS_DefinePropertyValueStr(ctx, image, kFields[f], v, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, image);
                image = JS_EXCEPTION;
            }
        }
        if (JS_IsException(image) || JS_DefinePropertyValueUint32(ctx, out, (uint32_t)i, image, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            out = JS_EXCEPTION;
        }
    }
    d_sequence_free(ctx, items, count);
    return out;
}

static JSValue images_resolve(os64_dom_t *dom, JSContext *ctx, JSValueConst raw)
{
    uint32_t count = 0;
    JSValue length = JS_GetPropertyStr(ctx, raw, "length");
    if (JS_IsException(length) || JS_ToUint32(ctx, &count, length) < 0) {
        JS_FreeValue(ctx, length);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, length);
    JSValue out = JS_NewArray(ctx);
    static const char *const kFields[] = {"src", "sizes", "type"};
    for (uint32_t i = 0; i < count && !JS_IsException(out); i++) {
        JSValue image = JS_GetPropertyUint32(ctx, raw, i);
        JSValue fields[3] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
        bool ok = !JS_IsException(image);
        for (int f = 0; f < 3 && ok; f++) {
            fields[f] = JS_GetPropertyStr(ctx, image, kFields[f]);
            ok = !JS_IsException(fields[f]);
        }
        if (ok) {
            char *url = NULL;
            int parsed = d_url_parse(dom, ctx, fields[0], &url);
            if (parsed == 0) JS_ThrowTypeError(ctx, "A MediaImage's src is not a URL");
            if (parsed == 1) {
                JS_FreeValue(ctx, fields[0]);
                fields[0] = JS_NewString(ctx, url);
                d_free(dom, url);
            }
            ok = parsed == 1 && !JS_IsException(fields[0]);
        }
        JSValue made = ok ? JS_NewObject(ctx) : JS_EXCEPTION;
        for (int f = 0; f < 3; f++) {
            if (!JS_IsException(made) &&
                JS_DefinePropertyValueStr(ctx, made, kFields[f], fields[f], JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, made);
                made = JS_EXCEPTION;
            } else if (JS_IsException(made)) {
                JS_FreeValue(ctx, fields[f]);
            }
            fields[f] = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, image);
        if (!JS_IsException(made) && freeze(ctx, made, kFields, 3, false) < 0) {
            JS_FreeValue(ctx, made);
            made = JS_EXCEPTION;
        }
        if (JS_IsException(made) || JS_DefinePropertyValueUint32(ctx, out, i, made, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            out = JS_EXCEPTION;
        }
    }
    if (!JS_IsException(out) && freeze(ctx, out, NULL, count, true) < 0) {
        JS_FreeValue(ctx, out);
        out = JS_EXCEPTION;
    }
    return out;
}

static JSValue artwork_convert(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    JSValue raw = images_read(dom, ctx, value);
    if (JS_IsException(raw)) return raw;
    JSValue out = images_resolve(dom, ctx, raw);
    JS_FreeValue(ctx, raw);
    return out;
}

static JSValue metadata_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    int present = d_dictionary(ctx, argc, argv, 0);
    if (present < 0) return JS_EXCEPTION;
    JSValueConst init = present ? argv[0] : JS_UNDEFINED;
    /* MediaMetadataInit's members in lexicographic order: album, artist,
     * artwork (its sequence walked and its images' members read here),
     * title; then the constructor's steps resolve the artwork. */
    JSValue album = string_member(ctx, init, "album", "", false, "MediaMetadata");
    JSValue artist = JS_IsException(album) ? JS_EXCEPTION : string_member(ctx, init, "artist", "", false, "MediaMetadata");
    JSValue art_given = JS_IsException(artist) || JS_IsUndefined(init) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, init, "artwork");
    JSValue raw = JS_IsException(artist) || JS_IsException(art_given) ? JS_EXCEPTION
                : JS_IsUndefined(art_given) ? JS_NewArray(ctx) : images_read(dom, ctx, art_given);
    JS_FreeValue(ctx, art_given);
    JSValue title = JS_IsException(raw) ? JS_EXCEPTION : string_member(ctx, init, "title", "", false, "MediaMetadata");
    JSValue converted = JS_IsException(title) ? JS_EXCEPTION : images_resolve(dom, ctx, raw);
    JS_FreeValue(ctx, raw);
    JSValue prototype = JS_IsException(converted) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsException(prototype) && !JS_IsObject(prototype)) {
        JS_FreeValue(ctx, prototype);
        prototype = JS_GetClassProto(ctx, os64_js_class_id(&metadata_slot));
    }
    JSValue object = JS_IsException(prototype) ? JS_EXCEPTION
                   : JS_NewObjectProtoClass(ctx, prototype, os64_js_class_id(&metadata_slot));
    JS_FreeValue(ctx, prototype);
    JSValue slots = JS_IsException(object) ? JS_EXCEPTION : JS_NewObject(ctx);
    DMetadata *m = JS_IsException(slots) ? NULL : js_malloc(ctx, sizeof(*m));
    if (m == NULL) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, object);
        JS_FreeValue(ctx, album);
        JS_FreeValue(ctx, artist);
        JS_FreeValue(ctx, title);
        JS_FreeValue(ctx, converted);
        return JS_EXCEPTION;
    }
    /* Each define takes its value whether or not it succeeds, so all four
     * run before the verdict. */
    int failed = JS_DefinePropertyValueStr(ctx, slots, "title", title, JS_PROP_C_W_E) < 0;
    failed |= JS_DefinePropertyValueStr(ctx, slots, "artist", artist, JS_PROP_C_W_E) < 0;
    failed |= JS_DefinePropertyValueStr(ctx, slots, "album", album, JS_PROP_C_W_E) < 0;
    failed |= JS_DefinePropertyValueStr(ctx, slots, "artwork", converted, JS_PROP_C_W_E) < 0;
    if (failed) {
        js_free(ctx, m);
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    m->slots = slots;
    JS_SetOpaque(object, m);
    return object;
}

enum { M_TITLE, M_ARTIST, M_ALBUM, M_ARTWORK };
static const char *const kMetadataNames[] = {"title", "artist", "album", "artwork"};

static JSValue metadata_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    DMetadata *m = metadata_of(self);
    if (m == NULL) return JS_ThrowTypeError(ctx, "Expected a MediaMetadata");
    const char *name = kMetadataNames[magic];
    if (!set) return JS_GetPropertyStr(ctx, m->slots, name);
    JSValueConst given = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue value = magic == M_ARTWORK ? artwork_convert(dom, ctx, given) : JS_ToString(ctx, given);
    if (JS_IsException(value)) return value;
    return JS_SetPropertyStr(ctx, m->slots, name, value) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* ── MediaSession ────────────────────────────────────────────────────── */

enum { S_NAVIGATOR, S_METADATA, S_PLAYBACK, S_SET_ACTION, S_SET_POSITION, S_MICROPHONE, S_CAMERA };

static int action_index(const char *text)
{
    for (int i = 0; i < ACTIONS; i++)
        if (os64_streq(text, kActions[i])) return i;
    return -1;
}

/* One member of MediaPositionState: true and *out when present; false and
 * absent; -1 with the exception pending. `restricted` refuses NaN and the
 * infinities, as a WebIDL double does. */
static int position_member(JSContext *ctx, JSValueConst dict, const char *name, bool restricted, double *out)
{
    JSValue v = JS_GetPropertyStr(ctx, dict, name);
    if (JS_IsException(v)) return -1;
    if (JS_IsUndefined(v)) return 0;
    int done = JS_ToFloat64(ctx, out, v);
    JS_FreeValue(ctx, v);
    if (done < 0) return -1;
    if (restricted && !isfinite(*out)) {
        JS_ThrowTypeError(ctx, "MediaPositionState's %s must be a finite number", name);
        return -1;
    }
    return 1;
}

static JSValue session_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    struct DMediaSession *ms = dom->media_session;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (magic == S_NAVIGATOR) {
        if (!JS_IsObject(self) || JS_VALUE_GET_PTR(self) != JS_VALUE_GET_PTR(dom->navigator->value))
            return JS_ThrowTypeError(ctx, "Expected a Navigator");
        return JS_DupValue(ctx, ms->object->value);
    }
    /* The receiver's session, which a method borrowed from another
     * window's prototype must act on, not its own binding's. */
    ms = JS_GetOpaque(self, os64_js_class_id(&session_slot));
    if (ms == NULL) return JS_ThrowTypeError(ctx, "Expected a MediaSession");
    dom = ms->dom;
    switch (magic) {
    case S_METADATA: {
        if (!set) return JS_GetPropertyStr(ctx, ms->holder->value, "metadata");
        JSValueConst given = argc > 0 ? argv[0] : JS_UNDEFINED;
        if (!JS_IsUndefined(given) && !JS_IsNull(given) && metadata_of(given) == NULL)
            return JS_ThrowTypeError(ctx, "metadata must be a MediaMetadata or null");
        JSValue held = JS_IsUndefined(given) ? JS_NULL : JS_DupValue(ctx, given);
        return JS_SetPropertyStr(ctx, ms->holder->value, "metadata", held) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    case S_PLAYBACK: {
        if (!set) return JS_NewString(ctx, kStates[ms->declared]);
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (text == NULL) return JS_EXCEPTION;
        /* An enum attribute ignores a value outside its enum (WebIDL). */
        for (uint8_t i = 0; i < 3; i++)
            if (os64_streq(text, kStates[i])) ms->declared = i;
        JS_FreeCString(ctx, text);
        return JS_UNDEFINED;
    }
    case S_SET_ACTION: {
        if (argc < 2) return JS_ThrowTypeError(ctx, "setActionHandler needs an action and a handler");
        const char *text = JS_ToCString(ctx, argv[0]);
        if (text == NULL) return JS_EXCEPTION;
        int at = action_index(text);
        JS_FreeCString(ctx, text);
        if (at < 0) return JS_ThrowTypeError(ctx, "That is not a MediaSessionAction");
        bool clear = JS_IsUndefined(argv[1]) || JS_IsNull(argv[1]);
        if (!clear && !JS_IsFunction(ctx, argv[1]))
            return JS_ThrowTypeError(ctx, "A media session action handler must be a function or null");
        DScheduledCallback callback = D_SCHEDULED_CALLBACK_INIT;
        os64_dom_t *owner = dom;
        uint32_t script = 0;
        if (!clear) {
            if (!d_scheduled_capture(dom, ctx, argv[1], &callback)) return JS_EXCEPTION;
            if (!d_require_active(dom, ctx) || !d_scheduled_live(ctx, &callback) ||
                (script = d_callback_record(dom, ctx, argv[1], &owner)) == D_SCRIPT_FAILED) {
                d_scheduled_clear(dom, &callback);
                return JS_EXCEPTION;
            }
        }
        d_scheduled_clear(dom, &ms->handlers[at]);
        ms->handlers[at] = callback;
        ms->scripts[at] = script;
        ms->owners[at] = owner;
        if (ms->calling[at]) ms->replaced[at] = true;
        return JS_UNDEFINED;
    }
    case S_SET_POSITION: {
        int present = d_dictionary(ctx, argc, argv, 0);
        if (present < 0) return JS_EXCEPTION;
        double duration = 0, rate = 1, position = 0;
        /* Members in lexicographic order: duration, playbackRate, position. */
        int has_duration = present ? position_member(ctx, argv[0], "duration", false, &duration) : 0;
        int has_rate = has_duration < 0 ? -1 : present ? position_member(ctx, argv[0], "playbackRate", true, &rate) : 0;
        int has_position = has_rate < 0 ? -1 : present ? position_member(ctx, argv[0], "position", true, &position) : 0;
        if (has_position < 0) return JS_EXCEPTION;
        if (!has_duration && !has_rate && !has_position) {
            ms->position_set = false;   /* an empty state clears it */
            return JS_UNDEFINED;
        }
        if (!has_duration) return JS_ThrowTypeError(ctx, "setPositionState needs a duration");
        if (isnan(duration) || duration < 0) return JS_ThrowTypeError(ctx, "The duration must not be negative");
        if (position < 0 || position > duration)
            return JS_ThrowTypeError(ctx, "The position must be between 0 and the duration");
        if (rate == 0) return JS_ThrowTypeError(ctx, "The playback rate must not be 0");
        ms->position_set = true;
        ms->duration = duration;
        ms->position = position;
        ms->rate = rate;
        return JS_UNDEFINED;
    }
    default:
        /* setMicrophoneActive and setCameraActive: os64 has neither. */
        return JS_UNDEFINED;
    }
}

static void clear_opaque(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

int d_media_session_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef session_class = {.class_name = "MediaSession", .finalizer = clear_opaque};
    static const JSClassDef metadata_class = {.class_name = "MediaMetadata", .finalizer = metadata_finalizer,
                                              .gc_mark = metadata_mark};
    struct DMediaSession *ms = d_alloc(dom, sizeof(*ms));
    if (ms == NULL) {
        d_error(ctx, "QuotaExceededError", "MediaSession quota exceeded");
        return -1;
    }
    os64_memset(ms, 0, sizeof(*ms));
    for (int i = 0; i < ACTIONS; i++) ms->handlers[i] = (DScheduledCallback)D_SCHEDULED_CALLBACK_INIT;
    ms->dom = dom;
    dom->media_session = ms;
    JSClassID session_id = os64_js_class_id(&session_slot), metadata_id = os64_js_class_id(&metadata_slot);
    JSValue session_prototype;
    if (d_interface_class(dom, ctx, global, session_id, &session_class, &session_prototype) < 0) return -1;
    if (!JS_IsRegisteredClass(dom->engine, metadata_id) && JS_NewClass(dom->engine, metadata_id, &metadata_class) < 0)
        return -1;
    DValue *metadata_prototype = d_retain(dom, ctx, JS_NewObject(ctx));
    if (metadata_prototype == NULL) return -1;
    JS_SetClassProto(ctx, metadata_id, JS_DupValue(ctx, metadata_prototype->value));
    for (int i = 0; i < 4; i++)
        if (d_accessor(dom, ctx, metadata_prototype->value, kMetadataNames[i], metadata_member, i, true) < 0) return -1;
    JSValue metadata_interface = d_interface_new(ctx, "MediaMetadata", 0, metadata_constructor,
                                                 metadata_prototype->value, JS_UNDEFINED);
    if (JS_IsException(metadata_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "MediaMetadata", metadata_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    if (d_accessor(dom, ctx, session_prototype, "metadata", session_member, S_METADATA, true) < 0 ||
        d_accessor(dom, ctx, session_prototype, "playbackState", session_member, S_PLAYBACK, true) < 0 ||
        d_method_named(dom, ctx, session_prototype, "setActionHandler", session_member, 2, S_SET_ACTION) < 0 ||
        d_method_named(dom, ctx, session_prototype, "setPositionState", session_member, 0, S_SET_POSITION) < 0 ||
        d_method_named(dom, ctx, session_prototype, "setMicrophoneActive", session_member, 1, S_MICROPHONE) < 0 ||
        d_method_named(dom, ctx, session_prototype, "setCameraActive", session_member, 1, S_CAMERA) < 0)
        return -1;
    JSValue session = JS_NewObjectClass(ctx, (int)session_id);
    ms->object = d_retain(dom, ctx, session);
    if (ms->object == NULL) return -1;
    JS_SetOpaque(session, ms);
    JSValue holder = JS_NewObject(ctx);
    ms->holder = d_retain(dom, ctx, holder);
    if (ms->holder == NULL || JS_SetPropertyStr(ctx, holder, "metadata", JS_NULL) < 0) return -1;
    /* navigator.mediaSession, a member of Navigator.prototype, which the
     * window objects' row made navigator's own prototype level. */
    JSValue navigator_prototype = JS_GetPrototype(ctx, dom->navigator->value);
    if (JS_IsException(navigator_prototype)) return -1;
    int done = d_accessor(dom, ctx, navigator_prototype, "mediaSession", session_member, S_NAVIGATOR, false);
    JS_FreeValue(ctx, navigator_prototype);
    return done;
}

/* The handlers let go, and the session object's opaque cleared: another
 * window's script may still hold the object after this binding has gone,
 * and its next call must be refused, not reach the memory freed below. */
void d_media_session_drain(os64_dom_t *dom)
{
    struct DMediaSession *ms = dom->media_session;
    if (ms == NULL) return;
    for (int i = 0; i < ACTIONS; i++) d_scheduled_clear(dom, &ms->handlers[i]);
    if (ms->object != NULL && JS_IsObject(ms->object->value)) JS_SetOpaque(ms->object->value, NULL);
}

/* After the runtime is gone: native memory only. The drain has let go of
 * every handler. */
void d_media_session_free(os64_dom_t *dom)
{
    if (dom->media_session == NULL) return;
    d_free(dom, dom->media_session);
    dom->media_session = NULL;
}

/* ── The host's side ─────────────────────────────────────────────────── */

bool os64_dom_media_session_state(os64_dom_t *dom, os64_dom_media_session_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (dom == NULL || !d_delivery_active(dom)) return false;
    /* No session (the Modern Web off) is an answer: stopped, wanting none. */
    if (!dom->options.modern || dom->media_session == NULL) return true;
    struct DMediaSession *ms = dom->media_session;
    bool playing = false, paused_after_play = false, played = false;
    d_media_session_facts(dom, &playing, &paused_after_play, &played);
    bool handled = false;
    for (int i = 0; i < ACTIONS; i++) handled = handled || ms->calling[i] || !JS_IsUndefined(ms->handlers[i].value);
    /* The standard's actual playback state, over the elements a person can
     * hear: playing when one plays, or the page says so of one that has
     * played; else paused when one that played is paused, or the page says
     * so. A declaration with nothing audible behind it (a library marking a
     * muted preview "playing") says nothing. */
    if (playing || (ms->declared == S_PLAYING && played)) out->state = OS64_DOM_MEDIA_SESSION_PLAYING;
    else if (paused_after_play || (ms->declared == S_PAUSED && played)) out->state = OS64_DOM_MEDIA_SESSION_PAUSED;
    else out->state = OS64_DOM_MEDIA_SESSION_STOPPED;
    out->wanted = handled || played;
    for (int i = 0; i < 5; i++) {
        static const int kHostActions[5] = {A_PLAY, A_PAUSE, A_NEXT, A_PREVIOUS, A_STOP};
        int at = kHostActions[i];
        if (ms->calling[at] || !JS_IsUndefined(ms->handlers[at].value)) out->handled |= 1u << i;
    }
    return true;
}

bool os64_dom_media_session_action(os64_dom_t *dom, os64_dom_media_session_action_t action, os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (dom == NULL || !d_delivery_active(dom) || !dom->options.modern || dom->media_session == NULL) return false;
    int at;
    switch (action) {
    case OS64_DOM_MEDIA_SESSION_PLAY: at = A_PLAY; break;
    case OS64_DOM_MEDIA_SESSION_PAUSE: at = A_PAUSE; break;
    case OS64_DOM_MEDIA_SESSION_NEXT: at = A_NEXT; break;
    case OS64_DOM_MEDIA_SESSION_PREVIOUS: at = A_PREVIOUS; break;
    case OS64_DOM_MEDIA_SESSION_STOP: at = A_STOP; break;
    default: return false;
    }
    struct DMediaSession *ms = dom->media_session;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#media-session", (int)(sizeof(name) - 40),
                  dom->url != NULL ? dom->url : "about:blank");
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, name, out) == OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
        if (ctx == NULL) d_outcome_fold(out, &access);
        if (ctx != NULL && !JS_IsUndefined(ms->handlers[at].value) && !ms->calling[at]) {
            /* The handler out of its slot for the call (the header says why). */
            DScheduledCallback callback = ms->handlers[at];
            ms->handlers[at] = (DScheduledCallback)D_SCHEDULED_CALLBACK_INIT;
            uint32_t script = ms->scripts[at];
            os64_dom_t *owner = ms->owners[at];
            ms->calling[at] = true;
            ms->replaced[at] = false;
            JSValue details = JS_NewObject(ctx);
            if (!JS_IsException(details) &&
                JS_DefinePropertyValueStr(ctx, details, "action", JS_NewString(ctx, kActions[at]), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, details);
                details = JS_EXCEPTION;
            }
            if (!JS_IsException(details)) {
                os64_js_outcome_t call;
                DScripts before = d_script_enter(owner, script);
                d_scheduled_call(owner, &callback, callback.captured.runtime ? JS_UNDEFINED : d_window_this(dom), 1,
                                 &details, &call);
                d_script_leave(owner, &before, true);
                JS_FreeValue(ctx, details);
                d_outcome_fold(out, &call);
                if (call.status == OS64_JS_EXCEPTION) d_errors_fire(owner, out);
            } else {
                JSValue error = JS_GetException(ctx);
                JS_FreeValue(ctx, error);
            }
            ms->calling[at] = false;
            if (!ms->replaced[at] && JS_IsUndefined(ms->handlers[at].value)) {
                ms->handlers[at] = callback;
            } else {
                d_scheduled_clear(dom, &callback);
            }
            d_task_settle(dom, out);
        } else if (ctx != NULL && (at == A_PLAY || at == A_PAUSE || at == A_STOP)) {
            /* No handler: the browser's own answer. */
            d_media_session_default(dom, ctx, at == A_PLAY);
            d_task_settle(dom, out);
        }
        d_task_finish(dom, out);
    }
    d_task_close(dom, name, out);
    return true;
}
