#include "inner_text.h"

#include "os64/str.h"

/* THE RENDERED TEXT COLLECTION STEPS (HTML § 3.2.7), the first cut
 * (WHEEL_FILELIST_INNERTEXT.md § 3). The walk turns the tree under the
 * root into ITEMS, the standard's list of strings (TEXT, BREAK, TAB) and
 * required line break counts (COUNT) plus two of the cut's own (SPACE,
 * EDGE), and a last pass joins them.
 *
 *   TEXT     bytes of a Text node, borrowed from it (no script runs during
 *            a read, so the tree cannot change under the list)
 *   SPACE    a run of collapsible white space, written as one space unless
 *            the last pass drops it (§ 3.3)
 *   COUNT    a required line break count: 2 at a `p`'s edges, 1 at any
 *            other block-level or table-caption box's
 *   BREAK    "\n" that is a string, not a count: a `br`, a preserved line
 *            feed of `pre-line`, and a table row's separator
 *   TAB      "\t" after a table cell
 *   EDGE     nothing at all: the edge of a box that is not inline, which
 *            is where a line starts or ends, so a collapsible space beside
 *            it goes (layout drops the spaces at a line's ends)
 *
 * A row's last cell and a table's last row write no separator: the
 * separator is written after every one, and taken back when the row or
 * the table closes while it is still the last thing written (EDGEs aside).
 * So the walk needs no look-ahead, and costs what the subtree costs. */

typedef enum { I_TEXT, I_SPACE, I_COUNT, I_BREAK, I_TAB, I_EDGE } ItemKind;

typedef struct {
    uint8_t kind;
    /* I_BREAK: whether it is a row's separator, which its table may take
     * back. */
    bool row;
    uint32_t count;                 /* I_COUNT */
    const char *text;               /* I_TEXT */
    size_t length;
} Item;

typedef struct {
    os64_dom_text_style_provider_t facts;
    void *opaque;
    const d_inner_text_memory_t *memory;
    Item *items;
    size_t count, cap;
    bool failed;                    /* out of memory */
    bool refused;                   /* the provider had no answer */
    /* The last element asked about, which every Text child of it asks
     * again. */
    const os64_html_node_t *asked;
    os64_dom_text_style_t answer;
} Walk;

static bool facts_of(Walk *w, const os64_html_node_t *element, os64_dom_text_style_t *out)
{
    if (element != w->asked) {
        os64_dom_text_style_t answer = {0};
        if (!w->facts(w->opaque, element, &answer)) {
            w->refused = true;
            return false;
        }
        w->asked = element;
        w->answer = answer;
    }
    *out = w->answer;
    return true;
}

static void push(Walk *w, Item item)
{
    if (w->failed) return;
    if (w->count == w->cap) {
        size_t cap = w->cap != 0 ? w->cap * 2 : 64;
        if (cap > SIZE_MAX / sizeof(Item)) { w->failed = true; return; }
        Item *grown = w->memory->grow(w->memory->ctx, w->items, w->cap * sizeof(Item), cap * sizeof(Item));
        if (grown == NULL) { w->failed = true; return; }
        w->items = grown;
        w->cap = cap;
    }
    w->items[w->count++] = item;
}

/* Takes back the separator a closing row (a TAB) or table (a row's BREAK)
 * wrote last, EDGEs aside, which are nothing to the text. */
static void take_back(Walk *w, ItemKind kind)
{
    size_t at = w->count;
    while (at > 0 && w->items[at - 1].kind == I_EDGE) at--;
    if (at == 0) return;
    const Item *last = &w->items[at - 1];
    if (last->kind != kind || (kind == I_BREAK && !last->row)) return;
    os64_memmove(&w->items[at - 1], &w->items[at], (w->count - at) * sizeof(Item));
    w->count--;
}

static bool html_tag(const os64_html_node_t *node, os64_html_tag_t tag)
{
    return node->ns == OS64_HTML_NS_HTML && node->tag == tag;
}

/* An element whose children layout does not render: a replaced element's
 * fallback, a form control's own text, an SVG drawing. Their text is no
 * text on the screen (slice D asks layout instead of this list). */
static bool childless(const os64_html_node_t *node)
{
    if (node->ns == OS64_HTML_NS_SVG) return true;
    if (node->ns != OS64_HTML_NS_HTML) return false;
    switch (node->tag) {
    case OS64_HTML_TAG_IMG: case OS64_HTML_TAG_VIDEO: case OS64_HTML_TAG_AUDIO: case OS64_HTML_TAG_CANVAS:
    case OS64_HTML_TAG_IFRAME: case OS64_HTML_TAG_OBJECT: case OS64_HTML_TAG_EMBED: case OS64_HTML_TAG_INPUT:
    case OS64_HTML_TAG_TEXTAREA:
        return true;
    default:
        return false;
    }
}

static bool space_byte(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool line_feed(char c)
{
    return c == '\n' || c == '\r';
}

/* The required line break count an element's box writes at both edges, 0
 * for none. */
static uint32_t edge_count(const os64_html_node_t *node, const os64_dom_text_style_t *s)
{
    if (s->display == OS64_DOM_TEXT_INLINE || s->display == OS64_DOM_TEXT_CONTENTS) return 0;
    if (html_tag(node, OS64_HTML_TAG_P)) return 2;
    return s->display == OS64_DOM_TEXT_BLOCK || s->display == OS64_DOM_TEXT_TABLE ||
        s->display == OS64_DOM_TEXT_TABLE_CAPTION ? 1 : 0;
}

static bool boxed(const os64_dom_text_style_t *s)
{
    return s->display != OS64_DOM_TEXT_INLINE && s->display != OS64_DOM_TEXT_CONTENTS;
}

/* A Text node under `parent`, as its white space says (§ 3.3). */
static void text_items(Walk *w, const os64_html_node_t *text, const os64_html_node_t *parent)
{
    if (text->text_len == 0) return;
    os64_dom_text_style_t s;
    if (parent == NULL || parent->kind != OS64_HTML_ELEMENT || !facts_of(w, parent, &s)) return;
    if (!s.rendered || !s.visible) return;
    const char *at = text->text, *end = text->text + text->text_len;
    if (s.white_space == OS64_DOM_SPACE_PRESERVE) {
        push(w, (Item){.kind = I_TEXT, .text = at, .length = text->text_len});
        return;
    }
    /* White space alone inside a table's own boxes is dropped (CSS 2.1
     * § 17.2.1), so it never sits between a cell's tab and its row's end. */
    if (s.display == OS64_DOM_TEXT_TABLE || s.display == OS64_DOM_TEXT_TABLE_PART ||
        s.display == OS64_DOM_TEXT_TABLE_ROW) {
        const char *p = at;
        while (p < end && space_byte(*p)) p++;
        if (p == end) return;
    }
    bool pre_line = s.white_space == OS64_DOM_SPACE_PRE_LINE;
    while (at < end && !w->failed) {
        if (!space_byte(*at)) {
            const char *run = at;
            while (at < end && !space_byte(*at)) at++;
            push(w, (Item){.kind = I_TEXT, .text = run, .length = (size_t)(at - run)});
            continue;
        }
        /* A run of white space: one SPACE, or under pre-line a BREAK for
         * each line feed in it (CR LF one), the spaces beside each going
         * at the last pass as spaces beside any break do. */
        bool spaced = false;
        while (at < end && space_byte(*at)) {
            if (pre_line && line_feed(*at)) {
                if (*at == '\r' && at + 1 < end && at[1] == '\n') at++;
                push(w, (Item){.kind = I_BREAK});
                spaced = false;
            } else if (!spaced) {
                push(w, (Item){.kind = I_SPACE});
                spaced = true;
            }
            at++;
        }
    }
}

/* Entering an element: false when its subtree gives nothing, so the walk
 * does not go down into it. */
static bool element_open(Walk *w, const os64_html_node_t *node, os64_dom_text_style_t *s)
{
    if (!facts_of(w, node, s) || !s->rendered) return false;
    if (html_tag(node, OS64_HTML_TAG_BR)) {
        if (s->visible) push(w, (Item){.kind = I_BREAK});
        return false;
    }
    if (boxed(s)) push(w, (Item){.kind = I_EDGE});
    uint32_t count = edge_count(node, s);
    if (count != 0 && s->visible) push(w, (Item){.kind = I_COUNT, .count = count});
    return !childless(node);
}

/* Leaving an element whose open answered true (or that has no children). */
static void element_close(Walk *w, const os64_html_node_t *node)
{
    os64_dom_text_style_t s;
    if (!facts_of(w, node, &s)) return;
    if (s.visible) {
        if (s.display == OS64_DOM_TEXT_TABLE_ROW) take_back(w, I_TAB);
        if (s.display == OS64_DOM_TEXT_TABLE) take_back(w, I_BREAK);
        uint32_t count = edge_count(node, &s);
        if (count != 0) push(w, (Item){.kind = I_COUNT, .count = count});
    }
    if (boxed(&s)) push(w, (Item){.kind = I_EDGE});
    if (!s.visible) return;
    if (s.display == OS64_DOM_TEXT_TABLE_CELL) push(w, (Item){.kind = I_TAB});
    if (s.display == OS64_DOM_TEXT_TABLE_ROW) push(w, (Item){.kind = I_BREAK, .row = true});
}

/* Whether an item ends a line for a collapsible space beside it. */
static bool line_edge(const Item *item)
{
    return item->kind == I_COUNT || item->kind == I_BREAK || item->kind == I_TAB || item->kind == I_EDGE;
}

/* The last pass: which SPACEs are written, then the standard's steps 3 to
 * 6 (empties gone, leading and trailing counts gone, each run of counts one
 * run of line feeds as long as its largest). Measures when `out` is NULL. */
static size_t assemble(const Walk *w, char *out)
{
    size_t n = 0;
    bool wrote = false;             /* anything written yet */
    bool line_open = false;         /* something written since the last edge */
    bool spaced = false;            /* the last thing written was a space */
    uint32_t pending = 0;           /* the run of counts waiting */
    for (size_t i = 0; i < w->count; i++) {
        const Item *item = &w->items[i];
        switch (item->kind) {
        case I_EDGE:
            line_open = false;
            break;
        case I_COUNT:
            if (item->count > pending) pending = item->count;
            line_open = false;
            break;
        case I_SPACE: {
            if (!line_open || spaced) break;
            /* A space with a line's end after it (the next thing that is
             * not a space) goes, and so does one at the very end. */
            size_t j = i + 1;
            while (j < w->count && w->items[j].kind == I_SPACE) j++;
            if (j == w->count || line_edge(&w->items[j])) break;
            if (out != NULL) out[n] = ' ';
            n++;
            spaced = true;
            break;
        }
        default: {                  /* I_TEXT, I_BREAK, I_TAB */
            if (pending != 0 && wrote) {
                if (out != NULL) for (uint32_t k = 0; k < pending; k++) out[n + k] = '\n';
                n += pending;
            }
            pending = 0;
            const char *bytes = item->kind == I_TEXT ? item->text : item->kind == I_TAB ? "\t" : "\n";
            size_t length = item->kind == I_TEXT ? item->length : 1;
            if (out != NULL) os64_memcpy(out + n, bytes, length);
            n += length;
            wrote = true;
            spaced = false;
            line_open = item->kind == I_TEXT;
            break;
        }
        }
    }
    return n;
}

d_inner_text_status_t d_inner_text(const os64_html_node_t *root, os64_dom_text_style_provider_t facts, void *opaque,
                                   const d_inner_text_memory_t *memory, char **out, size_t *length)
{
    *out = NULL;
    *length = 0;
    if (root == NULL || root->kind != OS64_HTML_ELEMENT || facts == NULL) return D_INNER_TEXT_NOT_RENDERED;
    Walk w = {.facts = facts, .opaque = opaque, .memory = memory};
    os64_dom_text_style_t s;
    if (!facts_of(&w, root, &s) || !s.rendered) return D_INNER_TEXT_NOT_RENDERED;
    /* The walk is the standard's: the root's children in tree order, a
     * shadow root's contents no part of it (a host's children are walked,
     * and those no slot takes have no style, so are not rendered). Down to
     * a node's first child when its open lets it in; otherwise on to its
     * next sibling, closing each parent on the way up until one has a
     * next sibling or the root is reached. */
    const os64_html_node_t *node = childless(root) ? NULL : root->first_child;
    while (node != NULL && !w.failed && !w.refused) {
        bool down = false;
        if (node->kind == OS64_HTML_TEXT) {
            text_items(&w, node, node->parent);
        } else if (node->kind == OS64_HTML_ELEMENT) {
            os64_dom_text_style_t es;
            if (element_open(&w, node, &es)) {
                const os64_html_node_t *child = node->first_child;
                if (child != NULL) {
                    node = child;
                    down = true;
                } else {
                    element_close(&w, node);
                }
            }
        }
        if (down) continue;
        for (;;) {
            const os64_html_node_t *next = node->next;
            if (next != NULL) {
                node = next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == root) {
                node = NULL;
                break;
            }
            element_close(&w, node);
        }
    }
    /* The root is not closed (its own steps are not the standard's to run),
     * but its last row and last cell are still last: a table or a row read
     * alone takes back its separator as its close would have. */
    if (s.visible && s.display == OS64_DOM_TEXT_TABLE_ROW) take_back(&w, I_TAB);
    if (s.visible && s.display == OS64_DOM_TEXT_TABLE) take_back(&w, I_BREAK);
    d_inner_text_status_t status = w.refused ? D_INNER_TEXT_NOT_RENDERED : w.failed ? D_INNER_TEXT_NO_MEMORY
                                                                                     : D_INNER_TEXT_OK;
    char *text = NULL;
    size_t n = 0;
    if (status == D_INNER_TEXT_OK) {
        n = assemble(&w, NULL);
        text = memory->grow(memory->ctx, NULL, 0, n + 1);
        if (text == NULL) {
            status = D_INNER_TEXT_NO_MEMORY;
        } else {
            assemble(&w, text);
            text[n] = '\0';
        }
    }
    memory->release(memory->ctx, w.items);
    if (status != D_INNER_TEXT_OK) return status;
    *out = text;
    *length = n;
    return D_INNER_TEXT_OK;
}
