// flow.c — the door: a document, its model and a width in, a laid-out
// tree out (LAYOUT.md § The one door).
//
// Pass 3 works in 26.6 on its own records; what a face reads is built here
// once from them — every rectangle rounded ONCE by the painter's rule, to
// nearest with ties up, and a size as round(end) - round(start) so boxes
// that meet in the layout meet on the glass — with each box's overflow
// rect, a node's first box, and the lists of pictures and controls.

#include "internal.h"

// The public kinds of the block-level boxes are pass 2's, in pass 2's order.
_Static_assert(FLOW_BOX_BLOCK == (int)FB_BLOCK && FLOW_BOX_REPLACED == (int)FB_REPLACED &&
               FLOW_BOX_TABLE == (int)FB_TABLE && FLOW_BOX_CAPTION == (int)FB_CAPTION &&
               FLOW_BOX_COLUMN_GROUP == (int)FB_COLUMN_GROUP &&
               FLOW_BOX_COLUMN == (int)FB_COLUMN && FLOW_BOX_ROW_GROUP == (int)FB_ROW_GROUP &&
               FLOW_BOX_ROW == (int)FB_ROW && FLOW_BOX_CELL == (int)FB_CELL,
               "flow_box_kind_t and f_box_kind_t disagree");

struct flow_tree {
    FStyles *styles;
    FBoxes *boxes;
    FLayout *layout;
    FArena arena;           // the public boxes
    flow_box_t *root;
    FMap first;             // node -> its first public box
    const flow_box_t **images, **controls;
    int32_t nimages, ncontrols, cap_images, cap_controls;
    bool incomplete;
};

typedef struct {
    flow_tree_t *t;
    bool failed;
} Build;

static int64_t round_px(int64_t v)
{
    int64_t q = (v + 32) / 64;
    return (v + 32) % 64 < 0 ? q - 1 : q;
}

static os64_gui_rect_t rect_of(int64_t x, int64_t y, int64_t w, int64_t h)
{
    int64_t x0 = round_px(x), y0 = round_px(y);
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(round_px(x + w) - x0),
                             (int32_t)(round_px(y + h) - y0)};
}

static os64_gui_rect_t join(os64_gui_rect_t a, os64_gui_rect_t b)
{
    int64_t x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w > (int64_t)b.x + b.w ? (int64_t)a.x + a.w : (int64_t)b.x + b.w;
    int64_t y1 = (int64_t)a.y + a.h > (int64_t)b.y + b.h ? (int64_t)a.y + a.h : (int64_t)b.y + b.h;
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
}

static bool push_list(Build *bd, const flow_box_t ***v, int32_t *n, int32_t *cap,
                      const flow_box_t *box)
{
    if (*n == *cap) {
        int32_t cap2 = *cap != 0 ? *cap * 2 : 16;
        const flow_box_t **grown = os64_realloc(*v, (size_t)cap2 * sizeof(*grown));
        if (grown == NULL) {
            bd->failed = true;
            return false;
        }
        *v = grown;
        *cap = cap2;
    }
    (*v)[(*n)++] = box;
    return true;
}

static bool is_picture(const os64_html_node_t *n)
{
    if (n == NULL || n->kind != OS64_HTML_ELEMENT || n->ns != OS64_HTML_NS_HTML)
        return false;
    if (n->tag == OS64_HTML_TAG_IMG)
        return true;
    const os64_html_attr_t *type = os64_html_attr(n, "type");
    return n->tag == OS64_HTML_TAG_INPUT && type != NULL && f_eq_nocase(type->value, "image");
}

// A new public box, attached under `parent` after `*last`, and recorded as
// its node's first box — and as a picture or a control — when it is one.
static flow_box_t *add(Build *bd, flow_box_t *parent, flow_box_t **last, flow_box_kind_t kind,
                       const os64_html_node_t *node, const flow_style_t *style)
{
    if (bd->failed)
        return NULL;
    flow_box_t *b = f_arena_alloc(&bd->t->arena, sizeof(*b));
    if (b == NULL) {
        bd->failed = true;
        return NULL;
    }
    b->kind = kind;
    b->node = node;
    b->style = style;
    b->link = b->control = -1;
    b->parent = parent;
    if (parent != NULL) {
        if (*last != NULL)
            (*last)->next = b;
        else
            parent->first = b;
        *last = b;
    }
    if (node != NULL && f_map_get(&bd->t->first, node) == NULL) {
        if (!f_map_put(&bd->t->first, node, b)) {
            bd->failed = true;
            return b;
        }
        if (is_picture(node))
            push_list(bd, &bd->t->images, &bd->t->nimages, &bd->t->cap_images, b);
    }
    return b;
}

static flow_box_t *public_box(Build *bd, const FBox *src, flow_box_t *parent, flow_box_t **last);

static void public_frag(Build *bd, const FFrag *fr, flow_box_t *parent, flow_box_t **last)
{
    flow_box_kind_t kind = fr->kind == FF_TEXT ? FLOW_BOX_TEXT
                         : fr->kind == FF_ATOMIC ? FLOW_BOX_ATOMIC : FLOW_BOX_MARKER;
    flow_box_t *b = add(bd, parent, last, kind, fr->node, fr->style);
    if (b == NULL)
        return;
    b->rect = rect_of(fr->x, fr->y, fr->w, fr->h);
    b->overflow = b->rect;
    b->baseline = (int32_t)round_px(fr->baseline);
    b->link = fr->link;
    b->decoration = fr->decoration;
    b->decoration_color = fr->decoration_color;
    if (fr->kind != FF_ATOMIC) {
        b->run = fr->run;
        b->text = fr->text + fr->begin;
        b->length = fr->end - fr->begin;
        b->begin = (fr->item != NULL ? fr->item->offset : 0) + fr->begin;
    } else {
        b->control = fr->item->control;
        if (b->control >= 0)
            push_list(bd, &bd->t->controls, &bd->t->ncontrols, &bd->t->cap_controls, b);
        if (fr->item->content != NULL && fr->item->content->placed) {
            flow_box_t *inner_last = NULL;
            flow_box_t *inner = public_box(bd, fr->item->content, b, &inner_last);
            if (inner != NULL)
                b->overflow = join(b->overflow, inner->overflow);
        }
    }
}

// A laid-out box and everything under it: its lines (each line's spans,
// then its fragments), its child boxes, then an outside marker — the
// order a partial layout grows in, so a tree cut short is a prefix of the
// whole one.
static flow_box_t *public_box(Build *bd, const FBox *src, flow_box_t *parent, flow_box_t **last)
{
    flow_box_t *b = add(bd, parent, last, (flow_box_kind_t)src->kind, src->node, src->style);
    if (b == NULL)
        return NULL;
    b->rect = rect_of(src->x, src->y, src->w, src->h);
    b->overflow = b->rect;
    b->link = src->link;
    b->unfinished = src->unfinished;
    flow_box_t *kids = NULL;
    for (const FLine *ln = src->lines; ln != NULL && !bd->failed; ln = ln->next) {
        flow_box_t *line = add(bd, b, &kids, FLOW_BOX_LINE, NULL, src->style);
        if (line == NULL)
            break;
        line->rect = rect_of(ln->x, ln->y, ln->w, ln->h);
        line->overflow = line->rect;
        line->baseline = (int32_t)round_px(ln->baseline);
        line->unfinished = ln->unfinished;
        flow_box_t *pieces = NULL;
        for (const FSpan *sp = ln->spans; sp != NULL && !bd->failed; sp = sp->next) {
            flow_box_t *span = add(bd, line, &pieces, FLOW_BOX_SPAN, sp->inl->node, sp->inl->style);
            if (span == NULL)
                break;
            span->rect = rect_of(sp->x0, sp->top, sp->x1 - sp->x0, sp->bottom - sp->top);
            span->overflow = span->rect;
            span->baseline = line->baseline;
            span->link = sp->inl->link;
            line->overflow = join(line->overflow, span->overflow);
        }
        for (const FFrag *fr = ln->frags; fr != NULL && !bd->failed; fr = fr->next) {
            public_frag(bd, fr, line, &pieces);
            if (pieces != NULL)
                line->overflow = join(line->overflow, pieces->overflow);
        }
        b->overflow = join(b->overflow, line->overflow);
    }
    for (const FBox *c = src->first; c != NULL && !bd->failed; c = c->next) {
        if (!c->placed)
            continue;
        flow_box_t *child = public_box(bd, c, b, &kids);
        if (child != NULL)
            b->overflow = join(b->overflow, child->overflow);
    }
    // A marker comes after the box's content, so an unfinished box, whose
    // content stopped short, has not reached it.
    if (src->marker_frag != NULL && !src->unfinished && !bd->failed) {
        public_frag(bd, src->marker_frag, b, &kids);
        if (kids != NULL)
            b->overflow = join(b->overflow, kids->overflow);
    }
    return b;
}

void flow_free(flow_tree_t *tree)
{
    if (tree == NULL)
        return;
    f_layout_free(tree->layout);
    f_boxes_free(tree->boxes);
    f_style_free(tree->styles);
    f_map_free(&tree->first);
    f_arena_free(&tree->arena);
    os64_free(tree->images);
    os64_free(tree->controls);
    os64_free(tree);
}

flow_tree_t *flow_layout(const os64_html_document_t *doc, const os64_page_t *model,
                         int32_t width, const flow_env_t *env)
{
    if (doc == NULL || env == NULL || env->text == NULL || env->fonts == NULL || width < 0)
        return NULL;
    flow_tree_t *tree = os64_calloc(1, sizeof(*tree));
    if (tree == NULL)
        return NULL;
    tree->styles = f_style_build(doc, model, env);
    tree->boxes = tree->styles != NULL ? f_boxes_build(doc, model, tree->styles, env) : NULL;
    tree->layout = tree->boxes != NULL ? f_layout(tree->boxes, doc, model, env, width) : NULL;
    if (tree->layout == NULL) {
        flow_free(tree);
        return NULL;
    }
    tree->incomplete = tree->layout->incomplete;
    const FBox *root = tree->boxes->root;
    if (root != NULL && root->placed) {
        Build bd = {tree, false};
        flow_box_t *none = NULL;
        tree->root = public_box(&bd, root, NULL, &none);
        // Out of memory for the public boxes is out of memory for the
        // page: nothing half-built is handed over.
        if (bd.failed) {
            flow_free(tree);
            return NULL;
        }
    }
    return tree;
}

bool flow_incomplete(const flow_tree_t *tree)
{
    return tree != NULL && tree->incomplete;
}

int32_t flow_height(const flow_tree_t *tree)
{
    return tree != NULL ? (int32_t)round_px(tree->layout->height) : 0;
}

int32_t flow_width(const flow_tree_t *tree)
{
    return tree != NULL ? (int32_t)round_px(tree->layout->width) : 0;
}

const flow_box_t *flow_root(const flow_tree_t *tree)
{
    return tree != NULL ? tree->root : NULL;
}

static bool meets(os64_gui_rect_t a, os64_gui_rect_t b)
{
    return (int64_t)a.x < (int64_t)b.x + b.w && (int64_t)b.x < (int64_t)a.x + a.w &&
           (int64_t)a.y < (int64_t)b.y + b.h && (int64_t)b.y < (int64_t)a.y + a.h;
}

static bool holds(os64_gui_rect_t r, int32_t x, int32_t y)
{
    return x >= r.x && (int64_t)x < (int64_t)r.x + r.w && y >= r.y && (int64_t)y < (int64_t)r.y + r.h;
}

static bool block_level(flow_box_kind_t k)
{
    return k <= FLOW_BOX_CELL;
}

typedef struct {
    os64_gui_rect_t view;
    void (*visit)(void *ctx, const flow_box_t *box);
    void *ctx;
} Visit;

static void visit_inline(const Visit *v, const flow_box_t *b);

// Appendix E's step 4: the block-level boxes, in tree order.
static void visit_blocks(const Visit *v, const flow_box_t *b)
{
    if (!meets(b->overflow, v->view))
        return;
    v->visit(v->ctx, b);
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        if (block_level(c->kind))
            visit_blocks(v, c);
}

// Step 7: the inline content — each line's spans and fragments, an atom's
// own content painted where the atom is — and the markers.
static void visit_inline(const Visit *v, const flow_box_t *b)
{
    if (!meets(b->overflow, v->view))
        return;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        if (c->kind == FLOW_BOX_LINE) {
            if (!meets(c->overflow, v->view))
                continue;
            for (const flow_box_t *f = c->first; f != NULL; f = f->next) {
                if (!meets(f->overflow, v->view))
                    continue;
                v->visit(v->ctx, f);
                for (const flow_box_t *inner = f->first; inner != NULL; inner = inner->next) {
                    visit_blocks(v, inner);
                    visit_inline(v, inner);
                }
            }
        } else if (c->kind == FLOW_BOX_MARKER) {
            if (meets(c->overflow, v->view))
                v->visit(v->ctx, c);
        } else if (block_level(c->kind)) {
            visit_inline(v, c);
        }
    }
}

void flow_visit(const flow_tree_t *tree, os64_gui_rect_t viewport,
                void (*visit)(void *ctx, const flow_box_t *box), void *ctx)
{
    if (tree == NULL || tree->root == NULL || visit == NULL)
        return;
    Visit v = {viewport, visit, ctx};
    visit_blocks(&v, tree->root);
    visit_inline(&v, tree->root);
}

static const flow_box_t *hit(const flow_box_t *b, int32_t x, int32_t y)
{
    if (!holds(b->overflow, x, y))
        return NULL;
    const flow_box_t *found = NULL;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        const flow_box_t *h = hit(c, x, y);
        if (h != NULL)
            found = h;
    }
    if (found != NULL)
        return found;
    return holds(b->rect, x, y) ? b : NULL;
}

const flow_box_t *flow_hit(const flow_tree_t *tree, int32_t x, int32_t y)
{
    return tree != NULL && tree->root != NULL ? hit(tree->root, x, y) : NULL;
}

const flow_box_t *flow_box_for(const flow_tree_t *tree, const os64_html_node_t *node)
{
    return tree != NULL ? f_map_get(&tree->first, node) : NULL;
}

int32_t flow_nimages(const flow_tree_t *tree)
{
    return tree != NULL ? tree->nimages : 0;
}

const flow_box_t *flow_image(const flow_tree_t *tree, int32_t i)
{
    return tree != NULL && i >= 0 && i < tree->nimages ? tree->images[i] : NULL;
}

int32_t flow_ncontrols(const flow_tree_t *tree)
{
    return tree != NULL ? tree->ncontrols : 0;
}

const flow_box_t *flow_control(const flow_tree_t *tree, int32_t i)
{
    return tree != NULL && i >= 0 && i < tree->ncontrols ? tree->controls[i] : NULL;
}

int64_t flow_dump(const flow_tree_t *tree, char *out, size_t cap)
{
    if (tree == NULL) {
        if (cap > 0)
            out[0] = '\0';
        return 0;
    }
    return f_tree_dump(tree->root, flow_width(tree), flow_height(tree), tree->incomplete, out,
                       cap);
}
