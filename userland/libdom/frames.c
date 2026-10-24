#include "internal.h"

/* THE FRAMES ROW (IFRAME.md § The cluster and the window proxy, whose
 * HTMLIFrameElement this is the start of): the interface over the
 * binding's iframe prototype, D_PROTO_IFRAME, and the members it has so
 * far. A modern row, so the switch takes the interface and every member
 * away whole; the prototype itself is every page's, as each kind's is, and
 * with the switch off an iframe has HTMLElement's members alone.
 *
 * `src` is the one member: the address the frame's stand-in names, which
 * libpage lists as a link, so a frame a script inserts and gives its
 * address this way, as scripts most often do, is the stand-in a parsed
 * one is. Setting it sets the attribute and loads nothing: no document is
 * loaded inside a document yet. */
int d_frames_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValueConst prototype = dom->prototypes[D_PROTO_IFRAME]->value;
    if (d_url_reflection(dom, ctx, prototype, "src") < 0) return -1;
    /* HTMLElement's interface, the interfaces row's, read before any page
     * ran. */
    JSValue parent = JS_GetPropertyStr(ctx, global, "HTMLElement");
    if (JS_IsException(parent)) return -1;
    JSValue f = d_interface_new(ctx, "HTMLIFrameElement", 0, NULL, prototype, parent);
    JS_FreeValue(ctx, parent);
    if (JS_IsException(f)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, "HTMLIFrameElement", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0
        ? -1 : 0;
}
