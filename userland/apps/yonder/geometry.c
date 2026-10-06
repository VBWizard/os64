#include "geometry.h"
#include <limits.h>
#include <os64/str.h>

typedef struct {
    bool present;
    int64_t left, top, right, bottom;
} Bounds;

static const flow_box_t *next_box(const flow_box_t *box)
{
    if (box->first != NULL) return box->first;
    while (box != NULL && box->next == NULL) box = box->parent;
    return box != NULL ? box->next : NULL;
}

static void include(Bounds *bounds, os64_gui_rect_t rect)
{
    int64_t right = (int64_t)rect.x + rect.w, bottom = (int64_t)rect.y + rect.h;
    if (!bounds->present) {
        *bounds = (Bounds){true, rect.x, rect.y, right, bottom};
        return;
    }
    if (rect.x < bounds->left) bounds->left = rect.x;
    if (rect.y < bounds->top) bounds->top = rect.y;
    if (right > bounds->right) bounds->right = right;
    if (bottom > bounds->bottom) bounds->bottom = bottom;
}

static int32_t css_integer(int64_t device, uint32_t zoom)
{
    int64_t value = device * 1000;
    value = value >= 0 ? (value + zoom / 2) / zoom : -((-value + zoom / 2) / zoom);
    return value > INT32_MAX ? INT32_MAX : value < INT32_MIN ? INT32_MIN : (int32_t)value;
}

static int32_t border(const flow_box_t *box, unsigned side)
{
    return box->style != NULL ? (int32_t)(((int64_t)box->style->border_width[side] + 32) / 64) : 0;
}

static void offset_position(const flow_tree_t *tree, const flow_box_t *box,
                            flow_point_t scroll, int64_t *x, int64_t *y)
{
    os64_gui_rect_t rect = flow_box_doc_rect(box, scroll);
    *x = (int64_t)rect.x - (box->fixed ? scroll.x : 0);
    *y = (int64_t)rect.y - (box->fixed ? scroll.y : 0);
    // Offset positions ignore container scrolling but keep sticky placement.
    // Follow containing-block frames, which can differ from DOM ancestors.
    for (int32_t i = flow_box_scroller(box); i >= 0;) {
        flow_point_t at = flow_scroll_at(tree, i);
        *x += at.x;
        *y += at.y;
        i = flow_box_scroller(flow_scroller(tree, i));
    }
}

static bool named(const os64_html_node_t *node, const char *name)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML &&
           os64_streq(node->name, name);
}

bool yonder_geometry_snapshot(const os64_html_document_t *document,
    const flow_tree_t *tree, const os64_html_node_t *node, int32_t width,
    int32_t height, uint32_t zoom, flow_point_t scroll, os64_dom_geometry_t *out)
{
    if (out == NULL) return false;
    os64_memset(out, 0, sizeof(*out));
    if (document == NULL || node == NULL || tree == NULL ||
        flow_incomplete(tree) || !os64_html_owns_node(document, node)) return false;
    zoom = zoom != 0 ? zoom : 1000;
    const os64_html_node_t *root = node;
    while (root->parent != NULL) root = root->parent;
    if (root != document->document) return true;
    const flow_box_t *first = NULL;
    const flow_box_t *principal = flow_box_for(tree, node);
    Bounds raw = {0}, visual = {0};
    os64_gui_rect_t first_visual = {0};
    for (const flow_box_t *box = flow_root(tree); box != NULL; box = next_box(box)) {
        // Text and marker boxes may name an element (generated text/alt).
        // They are content, not another border-box fragment of that element.
        bool caption = principal != NULL && principal->kind == FLOW_BOX_TABLE &&
                       box->kind == FLOW_BOX_CAPTION && box->parent == principal;
        if ((box->node != node && !caption) || box->kind == FLOW_BOX_TEXT || box->kind == FLOW_BOX_MARKER ||
            box->kind == FLOW_BOX_LINE) continue;
        os64_gui_rect_t rect = flow_box_doc_rect(box, scroll);
        if (first == NULL) { first = box; first_visual = rect; }
        include(&raw, box->rect);
        if (rect.w > 0 && rect.h > 0) include(&visual, rect);
    }
    bool is_root = node == document->html;
    bool is_body = node == document->body;
    if ((is_root && document->quirks != OS64_HTML_QUIRKS) ||
        (is_body && document->quirks == OS64_HTML_QUIRKS)) {
        out->client_width = css_integer(width, zoom);
        out->client_height = css_integer(height, zoom);
    }
    if (first == NULL) return true;
    if (!visual.present) include(&visual, first_visual);
    double scale = 1000.0 / zoom;
    out->x = (visual.left - scroll.x) * scale;
    out->y = (visual.top - scroll.y) * scale;
    out->width = (visual.right - visual.left) * scale;
    out->height = (visual.bottom - visual.top) * scale;
    out->offset_width = css_integer(raw.right - raw.left, zoom);
    out->offset_height = css_integer(raw.bottom - raw.top, zoom);

    bool fixed = first->style != NULL && first->style->position == FLOW_POSITION_FIXED;
    const flow_box_t *parent = NULL;
    if (!is_root && !is_body) {
        bool static_position = first->style == NULL || first->style->position == FLOW_POSITION_STATIC;
        for (const os64_html_node_t *at = fixed ? NULL : node->parent; at != NULL; at = at->parent) {
            const flow_box_t *box = flow_box_for(tree, at);
            if (box == NULL || box->style == NULL) continue;
            if (at == document->body || box->style->position != FLOW_POSITION_STATIC ||
                (static_position && (named(at, "table") || named(at, "td") || named(at, "th")))) {
                parent = box;
                break;
            }
        }
        out->offset_parent = parent != NULL ? parent->node : NULL;
        int64_t x, y;
        offset_position(tree, first, scroll, &x, &y);
        // A static body's offset children use the document origin; its
        // margin/padding is not an origin to subtract from their offsets.
        bool static_body = parent != NULL && parent->node == document->body &&
                           parent->style->position == FLOW_POSITION_STATIC;
        if (parent != NULL && !static_body) {
            int64_t px, py;
            offset_position(tree, parent, scroll, &px, &py);
            x -= px + border(parent, FLOW_LEFT);
            y -= py + border(parent, FLOW_TOP);
        }
        out->offset_left = css_integer(x, zoom);
        out->offset_top = css_integer(y, zoom);
    }

    bool viewport_client = (is_root && document->quirks != OS64_HTML_QUIRKS) ||
                           (is_body && document->quirks == OS64_HTML_QUIRKS);
    if (!viewport_client && first->kind != FLOW_BOX_SPAN) {
        int32_t left = border(first, FLOW_LEFT), top = border(first, FLOW_TOP);
        int64_t w = (int64_t)first->rect.w - left - border(first, FLOW_RIGHT);
        int64_t h = (int64_t)first->rect.h - top - border(first, FLOW_BOTTOM);
        out->client_left = css_integer(left, zoom);
        out->client_top = css_integer(top, zoom);
        // Yonder's scrollbars overlay content rather than consuming its box.
        out->client_width = css_integer(w > 0 ? w : 0, zoom);
        out->client_height = css_integer(h > 0 ? h : 0, zoom);
    }
    return true;
}
