#include "internal.h"
#include <math.h>

/* HIT TESTING (CSSOM View § 5.1; RANGE.md § 8): elementFromPoint and
 * elementsFromPoint on Document and, with shadow trees, ShadowRoot
 * (DocumentOrShadowRoot). The host's hit provider answers the elements
 * under the point in the page's viewport, the pointer's own hit test and
 * then that element's flat-tree ancestors (dom.h); here each is retargeted
 * against the root asked, so a hit inside a shadow tree answers its host to
 * the document, and the repeats that makes are dropped. Only the window's
 * document has a viewport: any other document, and a point outside it,
 * answers null and an empty list. elementsFromPoint lists the hit and its
 * ancestors, not every box under the point in paint order (DEBTS.md). */

void os64_dom_set_hit(os64_dom_t *dom, os64_dom_hit_provider_t provider, void *opaque)
{
    if (dom == NULL || dom->closed) return;
    dom->hit = provider;
    dom->hit_opaque = opaque;
}

/* DOM's "retarget A against B": A, climbed out of each shadow tree it is in
 * whose root is no shadow-including inclusive ancestor of B. */
static const os64_html_node_t *retarget(const os64_html_node_t *a, const os64_html_node_t *b)
{
    for (;;) {
        const os64_html_node_t *root = d_tree_root(a);
        const os64_html_node_t *host = os64_html_shadow_host(root);
        if (host == NULL) return a;
        for (const os64_html_node_t *n = b; n != NULL; n = os64_html_shadow_including_parent(n))
            if (n == root) return a;
        a = host;
    }
}

static uint64_t add_sat(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

enum { H_ONE, H_ALL };

static JSValue from_point(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                          JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *root = d_node(dom, ctx, self);
    if (root == NULL) return JS_EXCEPTION;
    if (root->kind != OS64_HTML_DOCUMENT && os64_html_shadow_host(root) == NULL)
        return JS_ThrowTypeError(ctx, "Expected a document or a shadow root");
    if (argc < 2) return JS_ThrowTypeError(ctx, "Missing coordinate");
    /* Two doubles, in order: a value that is not finite is a TypeError. */
    double xy[2];
    for (int i = 0; i < 2; i++) {
        if (JS_ToFloat64(ctx, &xy[i], argv[i]) < 0) return JS_EXCEPTION;
        if (!isfinite(xy[i])) return JS_ThrowTypeError(ctx, "A coordinate is not a finite number");
    }
    const os64_html_node_t *few[64], **out = few;
    size_t count = 0;
    dom = d_owner(dom, root);
    bool window = os64_html_shadow_including_root(root) == dom->document->document;
    if (window && dom->hit != NULL) {
        /* Each ask's cost is its own and both are charged: the first is the
         * one that makes the layout current. */
        os64_dom_geometry_stats_t cost = {0}, again_cost = {0};
        if (!d_provider_begin(dom, ctx)) return JS_EXCEPTION;
        count = dom->hit(dom->hit_opaque, xy[0], xy[1], few, 64, &cost);
        d_provider_leave(dom);
        dom->geometry_stats.layouts = add_sat(dom->geometry_stats.layouts, cost.layouts);
        dom->geometry_stats.elapsed_us = add_sat(dom->geometry_stats.elapsed_us, cost.elapsed_us);
        if (!d_require_active(dom, ctx)) return JS_EXCEPTION;
        bool refused = false;
        if (count > 64 && magic == H_ALL) {
            out = d_alloc(dom, count * sizeof(*out));
            if (out == NULL) {
                refused = true;
                out = few;
            } else {
                if (!d_provider_begin(dom, ctx)) { d_free(dom, out); return JS_EXCEPTION; }
                size_t again = dom->hit(dom->hit_opaque, xy[0], xy[1], out, count, &again_cost);
                d_provider_leave(dom);
                dom->geometry_stats.layouts = add_sat(dom->geometry_stats.layouts, again_cost.layouts);
                dom->geometry_stats.elapsed_us = add_sat(dom->geometry_stats.elapsed_us, again_cost.elapsed_us);
                if (!d_require_active(dom, ctx)) { d_free(dom, out); return JS_EXCEPTION; }
                count = again < count ? again : count;
            }
        } else if (count > 64) {
            count = 64;
        }
        if (refused) return d_error(ctx, "QuotaExceededError", "DOM hit quota exceeded");
    }
    JSValue result;
    if (magic == H_ONE) {
        result = count != 0 ? d_wrap(dom, ctx, retarget(out[0], root)) : JS_NULL;
    } else {
        result = JS_NewArray(ctx);
        const os64_html_node_t *last = NULL;
        uint32_t at = 0;
        for (size_t i = 0; !JS_IsException(result) && i < count; i++) {
            const os64_html_node_t *n = retarget(out[i], root);
            if (n == last) continue;
            last = n;
            JSValue wrapped = d_wrap(dom, ctx, n);
            if (JS_IsException(wrapped) || JS_SetPropertyUint32(ctx, result, at++, wrapped) < 0) {
                JS_FreeValue(ctx, result);
                result = JS_EXCEPTION;
            }
        }
    }
    if (out != few) d_free(dom, out);
    return result;
}

int d_hit_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    (void)global;
    JSValueConst targets[2] = {dom->prototypes[D_PROTO_DOCUMENT]->value,
                               dom->shadow_root_proto != NULL ? dom->shadow_root_proto->value : JS_UNDEFINED};
    for (int i = 0; i < 2; i++)
        if (JS_IsObject(targets[i]) &&
            (d_method_named(dom, ctx, targets[i], "elementFromPoint", from_point, 2, H_ONE) < 0 ||
             d_method_named(dom, ctx, targets[i], "elementsFromPoint", from_point, 2, H_ALL) < 0))
            return -1;
    return 0;
}
