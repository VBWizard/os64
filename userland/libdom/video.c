#include "internal.h"

/* HTMLVideoElement (HTML § 4.8.9; VIDEO.md § 5 for how yonder shows it).
 *
 * A video element is a media element, and everything HTMLMediaElement is
 * (media.c) it is too: the same records, sessions, events and promises,
 * so a sound given to a <video> plays as it would in an <audio>. What a
 * video element has that an audio element lacks is here: its size, poster
 * and inline playback reflected, and the picture's size as shown, which
 * the host's facts give once the resource is probed (media.c keeps it,
 * with the resize event). canPlayType is the host's, which counts the
 * picture (yonder answers "probably" for an MP4 with avc1). The picture
 * itself is the host's to draw. getVideoPlaybackQuality (Media Playback
 * Quality) reads the host's counts of pictures shown and dropped. */

enum { V_VIDEO_WIDTH = 16, V_VIDEO_HEIGHT, V_QUALITY };
enum { Q_CREATION, Q_DROPPED, Q_TOTAL, Q_CORRUPTED };

/* A VideoPlaybackQuality: what the counts were when it was made. */
typedef struct { double creation; uint64_t dropped, total; } DQuality;

static JSClassID video_quality_slot;

static void quality_free(JSRuntime *rt, JSValue value)
{
    js_free_rt(rt, JS_GetOpaque(value, os64_js_class_id(&video_quality_slot)));
}

static JSValue quality_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DQuality *q = JS_GetOpaque(self, os64_js_class_id(&video_quality_slot));
    if (q == NULL) return JS_ThrowTypeError(ctx, "Illegal invocation");
    switch (magic) {
    case Q_CREATION: return JS_NewFloat64(ctx, q->creation);
    case Q_DROPPED: return JS_NewFloat64(ctx, (double)q->dropped);
    case Q_TOTAL: return JS_NewFloat64(ctx, (double)q->total);
    default: return JS_NewUint32(ctx, 0);
    }
}

static JSValue quality_new(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    JSValue o = JS_NewObjectClass(ctx, (int)os64_js_class_id(&video_quality_slot));
    if (JS_IsException(o)) return o;
    DQuality *q = js_mallocz(ctx, sizeof(*q));
    if (q == NULL) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    q->creation = d_performance_now(dom);
    d_media_quality(dom, node, &q->total, &q->dropped);
    JS_SetOpaque(o, q);
    return o;
}

static const DReflection kRows[] = {
    {"width", "width", K_UNSIGNED, D_KIND(D_PROTO_VIDEO), NULL, NULL, NULL, 0},
    {"height", "height", K_UNSIGNED, D_KIND(D_PROTO_VIDEO), NULL, NULL, NULL, 0},
    {"poster", "poster", K_URL, D_KIND(D_PROTO_VIDEO), NULL, NULL, NULL, 0},
    {"playsInline", "playsinline", K_BOOLEAN, D_KIND(D_PROTO_VIDEO), NULL, NULL, NULL, 0},
};
_Static_assert(sizeof(kRows) / sizeof(kRows[0]) < V_VIDEO_WIDTH, "the rows are numbered below V_VIDEO_WIDTH");

static JSValue video_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, this_value);
    if (node == NULL) return JS_EXCEPTION;
    dom = d_owner(dom, node);
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (d_node_kind(node) != D_PROTO_VIDEO) return JS_ThrowTypeError(ctx, "Illegal invocation");
    /* The picture's size: 0 while there is no picture (HTML § 4.8.9). */
    if (magic == V_VIDEO_WIDTH || magic == V_VIDEO_HEIGHT) {
        uint32_t width, height;
        d_media_video_size(dom, node, &width, &height);
        return JS_NewUint32(ctx, magic == V_VIDEO_WIDTH ? width : height);
    }
    if (magic == V_QUALITY) return quality_new(dom, ctx, node);
    return set ? d_reflection_set(dom, ctx, node, &kRows[magic], argc > 0 ? argv[0] : JS_UNDEFINED)
               : d_reflection_get(dom, ctx, node, &kRows[magic]);
}

int d_video_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValueConst video = dom->prototypes[D_PROTO_VIDEO]->value;
    for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++)
        if (d_accessor(dom, ctx, video, kRows[i].property, video_member, (int)i, true) < 0) return -1;
    if (d_accessor(dom, ctx, video, "videoWidth", video_member, V_VIDEO_WIDTH, false) < 0 ||
        d_accessor(dom, ctx, video, "videoHeight", video_member, V_VIDEO_HEIGHT, false) < 0 ||
        d_method_named(dom, ctx, video, "getVideoPlaybackQuality", video_member, 0, V_QUALITY) < 0)
        return -1;
    static const JSClassDef quality_class = {.class_name = "VideoPlaybackQuality", .finalizer = quality_free};
    JSValue quality_proto;
    if (d_interface_class(dom, ctx, global, os64_js_class_id(&video_quality_slot), &quality_class, &quality_proto) < 0 ||
        d_accessor(dom, ctx, quality_proto, "creationTime", quality_member, Q_CREATION, false) < 0 ||
        d_accessor(dom, ctx, quality_proto, "droppedVideoFrames", quality_member, Q_DROPPED, false) < 0 ||
        d_accessor(dom, ctx, quality_proto, "totalVideoFrames", quality_member, Q_TOTAL, false) < 0 ||
        d_accessor(dom, ctx, quality_proto, "corruptedVideoFrames", quality_member, Q_CORRUPTED, false) < 0)
        return -1;
    JSValue parent = JS_GetPropertyStr(ctx, global, "HTMLMediaElement");
    if (JS_IsException(parent)) return -1;
    JSValue video_interface = d_interface_new(ctx, "HTMLVideoElement", 0, NULL, video, parent);
    JS_FreeValue(ctx, parent);
    if (JS_IsException(video_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "HTMLVideoElement", video_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return 0;
}
