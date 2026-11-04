#include "geometry.h"
#include <limits.h>
#include <os64/mem.h>
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
    flow_point_t o = flow_box_layout_offset(box, scroll);
    *x = (int64_t)box->rect.x + o.x - (box->fixed ? scroll.x : 0);
    *y = (int64_t)box->rect.y + o.y - (box->fixed ? scroll.y : 0);
    // Offset positions ignore transforms and container scrolling but keep
    // sticky placement.
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
        // They are content, not another border-box fragment of that element,
        // and neither is a box its ::before or ::after made, which names it
        // too (GENERATED_CONTENT.md § 4).
        bool caption = principal != NULL && principal->kind == FLOW_BOX_TABLE &&
                       box->kind == FLOW_BOX_CAPTION && box->parent == principal;
        if ((box->node != node && !caption) || box->pseudo != 0 || box->kind == FLOW_BOX_TEXT ||
            box->kind == FLOW_BOX_MARKER || box->kind == FLOW_BOX_LINE) continue;
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
    // Whose scroll an element's scroll members read, by the same rule the
    // binding's setters write by (view.c's scroll_owner, CSSOM View § 7):
    // the view's for the root in no-quirks mode, and for the body in quirks
    // mode when it is not potentially scrollable, which is when it makes no
    // scroll container of its own (its overflow went to the viewport);
    // nothing for the root in quirks mode; else the container the element
    // makes (flow_scroller_for, the one the setters move: an inline-block's
    // container, not its atom).
    bool quirks = document->quirks == OS64_HTML_QUIRKS;
    int32_t own = flow_scroller_for(tree, node);
    bool views = (is_root && !quirks) || (is_body && quirks && own < 0);
    if (views) {
        out->scroll_left = css_integer(scroll.x, zoom);
        out->scroll_top = css_integer(scroll.y, zoom);
    } else if (own >= 0 && !(is_root && quirks)) {
        flow_point_t at = flow_scroll_at(tree, own);
        out->scroll_left = css_integer(at.x, zoom);
        out->scroll_top = css_integer(at.y, zoom);
    }
    // scrollWidth and scrollHeight (CSSOM View § 7, "scrolling area"): the
    // view's element answers the page's, never less than the viewport; a
    // scroll container its padding box and as far as it can scroll; any
    // other box its padding box, or as far as its content reaches past its
    // padding edge: its in-flow children's overflow, and its absolute
    // children only where it is their containing block. No end padding is
    // added: the standard adds it past the children's MARGIN boxes, which a
    // public box does not carry, and adding it past their border boxes
    // miscounts every negative margin. Positioned descendants deeper down
    // are not counted either (DEBTS). Its own border is no part of it.
    if (views) {
        int32_t w = flow_width(tree), h = flow_height(tree);
        out->scroll_width = css_integer(w > width ? w : width, zoom);
        out->scroll_height = css_integer(h > height ? h : height, zoom);
    } else if (principal != NULL && principal->kind != FLOW_BOX_SPAN && !(is_root && quirks)) {
        int64_t pad_w = (int64_t)principal->rect.w - border(principal, FLOW_LEFT) - border(principal, FLOW_RIGHT);
        int64_t pad_h = (int64_t)principal->rect.h - border(principal, FLOW_TOP) - border(principal, FLOW_BOTTOM);
        int64_t w = pad_w, h = pad_h;
        if (own >= 0) {
            flow_point_t range = flow_scroll_range(tree, own);
            w += range.x;
            h += range.y;
        } else {
            int64_t left = (int64_t)principal->rect.x + border(principal, FLOW_LEFT);
            int64_t top = (int64_t)principal->rect.y + border(principal, FLOW_TOP);
            for (const flow_box_t *c = principal->first; c != NULL; c = c->next) {
                const flow_style_t *cs = c->style;
                bool out_of_flow = cs != NULL && c->node != NULL && c->node != node &&
                                   (cs->position == FLOW_POSITION_ABSOLUTE || cs->position == FLOW_POSITION_FIXED);
                if (out_of_flow && (flow_clip_parent(tree, c->node) != node || c->fixed != principal->fixed))
                    continue;
                int64_t right = (int64_t)c->overflow.x + c->overflow.w - left;
                int64_t bottom = (int64_t)c->overflow.y + c->overflow.h - top;
                if (right > w) w = right;
                if (bottom > h) h = bottom;
            }
        }
        out->scroll_width = css_integer(w > 0 ? w : 0, zoom);
        out->scroll_height = css_integer(h > 0 ? h : 0, zoom);
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

/* ── Intersection observations (INTERSECTION_OBSERVER.md) ─────────────── */

// The elements a batch asks about, each once, with the bounds the walk
// gathers for it: open addressing over a power of two at least twice their
// number, so a probe ends at an empty slot.
typedef struct {
    const os64_html_node_t *node;
    const flow_box_t *principal, *first;
    Bounds visual;
    os64_gui_rect_t first_visual;
} Asked;

typedef struct {
    Asked *slots;
    size_t mask;
} AskedSet;

static size_t asked_hash(const os64_html_node_t *node, size_t mask)
{
    uintptr_t v = (uintptr_t)node >> 4;
    v ^= v >> 17;
    return (size_t)(v * 0x9E3779B97F4A7C15ull) & mask;
}

static Asked *asked_find(const AskedSet *set, const os64_html_node_t *node)
{
    for (size_t i = asked_hash(node, set->mask);; i = (i + 1) & set->mask) {
        if (set->slots[i].node == node) return &set->slots[i];
        if (set->slots[i].node == NULL) return NULL;
    }
}

static void asked_add(AskedSet *set, const flow_tree_t *tree, const os64_html_node_t *node)
{
    if (node == NULL) return;
    for (size_t i = asked_hash(node, set->mask);; i = (i + 1) & set->mask) {
        if (set->slots[i].node == node) return;
        if (set->slots[i].node == NULL) {
            set->slots[i].node = node;
            set->slots[i].principal = flow_box_for(tree, node);
            return;
        }
    }
}

// Whether `box` is one of its element's own border-box fragments, as
// yonder_geometry_snapshot counts them: a table's caption is the table's;
// a text, a marker, a line and a pseudo-element's box are no element's.
static const os64_html_node_t *fragment_of(const flow_box_t *box)
{
    if (box->pseudo != 0 || box->kind == FLOW_BOX_TEXT || box->kind == FLOW_BOX_MARKER ||
        box->kind == FLOW_BOX_LINE)
        return NULL;
    return box->node;
}

static void css_rect(const Bounds *b, flow_point_t scroll, double scale, double out[4])
{
    out[0] = (double)(b->left - scroll.x) * scale;
    out[1] = (double)(b->top - scroll.y) * scale;
    out[2] = (double)(b->right - scroll.x) * scale;
    out[3] = (double)(b->bottom - scroll.y) * scale;
}

// A box's padding box, where it is painted, in document pixels: inset in
// the box's own coordinates first, then taken out through its transforms,
// so a scaled border insets by its painted width.
static Bounds padding_bounds(const flow_box_t *box, flow_point_t scroll)
{
    int64_t w = (int64_t)box->rect.w - border(box, FLOW_LEFT) - border(box, FLOW_RIGHT);
    int64_t h = (int64_t)box->rect.h - border(box, FLOW_TOP) - border(box, FLOW_BOTTOM);
    os64_gui_rect_t local = {box->rect.x + border(box, FLOW_LEFT), box->rect.y + border(box, FLOW_TOP),
                             (int32_t)(w > 0 ? w : 0), (int32_t)(h > 0 ? h : 0)};
    os64_gui_rect_t r = flow_box_doc_rect_of(box, local, scroll);
    return (Bounds){true, r.x, r.y, (int64_t)r.x + r.w, (int64_t)r.y + r.h};
}

static bool connected(const os64_html_document_t *document, const os64_html_node_t *node)
{
    if (!os64_html_owns_node(document, node)) return false;
    while (node->parent != NULL) node = node->parent;
    return node == document->document;
}

bool yonder_intersections(const os64_html_document_t *document, const flow_tree_t *tree,
    os64_dom_intersection_t *items, size_t count, uint32_t zoom, flow_point_t scroll)
{
    if (document == NULL || tree == NULL || flow_incomplete(tree)) return false;
    zoom = zoom != 0 ? zoom : 1000;
    double scale = 1000.0 / zoom;
    size_t cap = 16;
    while (cap < count * 4 && cap < ((size_t)1 << 40)) cap <<= 1;
    AskedSet set = {os64_calloc(cap, sizeof(Asked)), cap - 1};
    if (set.slots == NULL) return false;
    for (size_t i = 0; i < count; i++) {
        if (items[i].target != NULL && connected(document, items[i].target)) asked_add(&set, tree, items[i].target);
        if (items[i].root != NULL && connected(document, items[i].root)) asked_add(&set, tree, items[i].root);
    }
    // One walk of the boxes: each asked element's fragments joined, a
    // table's caption joined to the table's as well as its own.
    for (const flow_box_t *box = flow_root(tree); box != NULL; box = next_box(box)) {
        const os64_html_node_t *node = fragment_of(box);
        if (node == NULL) continue;
        Asked *own = asked_find(&set, node);
        Asked *table = box->kind == FLOW_BOX_CAPTION && box->parent != NULL && box->parent->kind == FLOW_BOX_TABLE &&
                       box->parent->node != NULL ? asked_find(&set, box->parent->node) : NULL;
        if (table != NULL && table->principal != box->parent) table = NULL;
        if (own == NULL && table == NULL) continue;
        os64_gui_rect_t rect = flow_box_doc_rect(box, scroll);
        Asked *joined[2] = {own, table};
        for (int j = 0; j < 2; j++) {
            Asked *a = joined[j];
            if (a == NULL) continue;
            if (a->first == NULL) {
                a->first = box;
                a->first_visual = rect;
            }
            if (rect.w > 0 && rect.h > 0) include(&a->visual, rect);
        }
    }
    for (size_t i = 0; i < count; i++) {
        os64_dom_intersection_t *it = &items[i];
        it->boxed = it->contained = it->root_boxed = false;
        Asked *t = it->target != NULL ? asked_find(&set, it->target) : NULL;
        if (t == NULL || t->first == NULL) continue;
        if (!t->visual.present) include(&t->visual, t->first_visual);
        it->boxed = true;
        css_rect(&t->visual, scroll, scale, it->rect);
        // The clips of the chain up to the root, the root's own left out.
        Bounds clip = {true, -INT64_MAX / 4, -INT64_MAX / 4, INT64_MAX / 4, INT64_MAX / 4};
        bool cut_x = false, cut_y = false;
        const os64_html_node_t *at = flow_clip_parent(tree, it->target);
        for (; at != NULL && at != it->root; at = flow_clip_parent(tree, at)) {
            const flow_box_t *box = flow_box_for(tree, at);
            bool x = flow_box_clips(box, true), y = flow_box_clips(box, false);
            if (!x && !y) continue;
            Bounds pad = padding_bounds(box, scroll);
            if (x) {
                if (pad.left > clip.left) clip.left = pad.left;
                if (pad.right < clip.right) clip.right = pad.right;
                cut_x = true;
            }
            if (y) {
                if (pad.top > clip.top) clip.top = pad.top;
                if (pad.bottom < clip.bottom) clip.bottom = pad.bottom;
                cut_y = true;
            }
        }
        it->contained = it->root == NULL || at == it->root;
        css_rect(&clip, scroll, scale, it->clip);
        if (!cut_x) it->clip[0] = -OS64_DOM_UNCLIPPED, it->clip[2] = OS64_DOM_UNCLIPPED;
        if (!cut_y) it->clip[1] = -OS64_DOM_UNCLIPPED, it->clip[3] = OS64_DOM_UNCLIPPED;
        if (it->root == NULL) continue;
        Asked *r = asked_find(&set, it->root);
        if (r == NULL || r->first == NULL) continue;
        it->root_boxed = true;
        const flow_box_t *own = r->principal != NULL ? r->principal : r->first;
        if (flow_box_clips(own, true) || flow_box_clips(own, false)) {
            Bounds pad = padding_bounds(own, scroll);
            css_rect(&pad, scroll, scale, it->root_rect);
        } else {
            if (!r->visual.present) include(&r->visual, r->first_visual);
            css_rect(&r->visual, scroll, scale, it->root_rect);
        }
    }
    os64_free(set.slots);
    return true;
}

/* Image-map coordinates belong to the image's content origin. They are
 * CSS pixels, independent of its intrinsic raster size and browser zoom. */
static const os64_html_node_t *next_node(const os64_html_node_t *node,
                                        const os64_html_node_t *root)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != root && node->next == NULL) node = node->parent;
    return node != root ? node->next : NULL;
}

static bool coord_space(char c)
{
    return c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static bool coord(const char **cursor, double *out)
{
    const char *s = *cursor;
    while (coord_space(*s)) s++;
    bool negative = *s == '-';
    if (*s == '-' || *s == '+') s++;
    bool digits = false;
    double value = 0;
    while (*s >= '0' && *s <= '9') {
        digits = true;
        value = value * 10 + (*s++ - '0');
        if (value > 1e12) return false;
    }
    if (*s == '.') {
        s++;
        double fraction = 0.1;
        while (*s >= '0' && *s <= '9') {
            digits = true;
            value += (*s++ - '0') * fraction;
            fraction *= 0.1;
        }
    }
    if (!digits) return false;
    if (*s == 'e' || *s == 'E') {
        s++;
        bool minus = *s == '-';
        if (*s == '-' || *s == '+') s++;
        if (*s < '0' || *s > '9') return false;
        unsigned exponent = 0;
        while (*s >= '0' && *s <= '9') {
            exponent = exponent * 10 + (*s++ - '0');
            if (exponent > 12) return false;
        }
        while (exponent--) value = minus ? value * 0.1 : value * 10;
    }
    if (*s != '\0' && !coord_space(*s)) return false;
    if (value > 1e12) return false;
    *cursor = s;
    *out = negative ? -value : value;
    return true;
}

static bool edge_hit(double ax, double ay, double bx, double by, double x, double y)
{
    return (x-ax)*(by-ay) == (y-ay)*(bx-ax) &&
        x >= (ax < bx ? ax : bx) && x <= (ax > bx ? ax : bx) &&
        y >= (ay < by ? ay : by) && y <= (ay > by ? ay : by);
}

static bool area_hit(const os64_html_node_t *area, double x, double y)
{
    const os64_html_attr_t *shape = os64_html_attr(area, "shape");
    const char *kind = shape != NULL ? shape->value : "rect";
    if (os64_streq_nocase(kind, "default")) return true;
    const os64_html_attr_t *coords = os64_html_attr(area, "coords");
    if (coords == NULL || os64_strlen(coords->value) > 65536) return false;
    const char *s = coords->value;
    double a, b, c, d;
    if (!coord(&s, &a) || !coord(&s, &b)) return false;
    if (os64_streq_nocase(kind, "circle") || os64_streq_nocase(kind, "circ"))
        return coord(&s, &c) && c >= 0 && (x-a)*(x-a)+(y-b)*(y-b) <= c*c;
    if (os64_streq_nocase(kind, "rect") || os64_streq_nocase(kind, "rectangle"))
        return coord(&s, &c) && coord(&s, &d) &&
            x >= (a < c ? a : c) && x <= (a > c ? a : c) &&
            y >= (b < d ? b : d) && y <= (b > d ? b : d);
    if (!os64_streq_nocase(kind, "poly") && !os64_streq_nocase(kind, "polygon")) return false;
    double first_x = a, first_y = b;
    bool inside = false, boundary = false;
    unsigned points = 1;
    while (coord(&s, &c) && coord(&s, &d)) {
        boundary |= edge_hit(a,b,c,d,x,y);
        if ((b > y) != (d > y) && x < (c-a)*(y-b)/(d-b)+a) inside = !inside;
        a = c; b = d; points++;
    }
    if (points < 3) return false;
    boundary |= edge_hit(a,b,first_x,first_y,x,y);
    if ((b > y) != (first_y > y) && x < (first_x-a)*(y-b)/(first_y-b)+a) inside = !inside;
    return boundary || inside;
}

const os64_html_node_t *yonder_image_map_hit(const os64_html_document_t *document,
    const os64_html_node_t *image, double x, double y)
{
    if (document == NULL || !named(image,"img") || x < 0 || y < 0) return NULL;
    const os64_html_attr_t *usemap = os64_html_attr(image,"usemap");
    if (usemap == NULL || usemap->value[0] != '#' || usemap->value[1] == '\0') return NULL;
    const os64_html_node_t *map = NULL;
    for (const os64_html_node_t *at = document->document; at != NULL; at = next_node(at,document->document)) {
        if (!named(at,"map")) continue;
        const os64_html_attr_t *name = os64_html_attr(at,"name");
        if (name != NULL && os64_streq(name->value,usemap->value+1)) { map = at; break; }
    }
    if (map == NULL) return NULL;
    for (const os64_html_node_t *at = map->first_child; at != NULL; at = next_node(at,map))
        if (named(at,"area") && area_hit(at,x,y)) return at;
    return NULL;
}
