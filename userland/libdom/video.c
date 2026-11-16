#include "internal.h"

/* HTMLVideoElement (HTML § 4.8.9) AS A STAND-IN THAT PLAYS NO VIDEO
 * (AUDIO_YONDER.md § Y.4, which says what the video arc changes).
 *
 * A video element is a media element, and everything HTMLMediaElement is
 * (media.c) it is too: the same records, sessions, events and promises,
 * so a sound given to a <video> plays as it would in an <audio>. Players
 * do that: JW Player makes a <video> for an MP3 and asks it canPlayType.
 * What a video element has that an audio element lacks is here: its
 * size, poster and inline playback reflected, and the picture's size,
 * which is 0 because no picture is ever decoded. canPlayType is the
 * host's, which answers "" for a video type, so a player with a choice
 * takes another; a video file given anyway fails as one not supported. */

enum { V_VIDEO_WIDTH = 16, V_VIDEO_HEIGHT };

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
    /* The picture's size: 0 while there is no picture (HTML § 4.8.9),
     * which here is always. */
    if (magic == V_VIDEO_WIDTH || magic == V_VIDEO_HEIGHT) return JS_NewUint32(ctx, 0);
    return set ? d_reflection_set(dom, ctx, node, &kRows[magic], argc > 0 ? argv[0] : JS_UNDEFINED)
               : d_reflection_get(dom, ctx, node, &kRows[magic]);
}

int d_video_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValueConst video = dom->prototypes[D_PROTO_VIDEO]->value;
    for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++)
        if (d_accessor(dom, ctx, video, kRows[i].property, video_member, (int)i, true) < 0) return -1;
    if (d_accessor(dom, ctx, video, "videoWidth", video_member, V_VIDEO_WIDTH, false) < 0 ||
        d_accessor(dom, ctx, video, "videoHeight", video_member, V_VIDEO_HEIGHT, false) < 0)
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
