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
 * separator is written after every one, carrying the row or table it
 * belongs to, and when that row or table closes its last separator is
 * taken back. "Last" is a fact about boxes, not about what is visible: a
 * cell or row that is rendered but hidden writes a SILENT separator, which
 * counts as last and writes nothing, so the visible one before it keeps
 * its own. A row or table marks where it opened, which bounds the search
 * for its separator, so the walk needs no look-ahead and costs what the
 * subtree costs (a caption after the rows sits between, and is passed
 * over). */

typedef enum { I_TEXT, I_SPACE, I_COUNT, I_BREAK, I_TAB, I_EDGE } ItemKind;

typedef struct {
    uint8_t kind;
    /* I_TAB and a row's I_BREAK: writes nothing (a hidden cell's or row's,
     * which still counts as last). */
    bool silent;
    uint32_t count;                 /* I_COUNT */
    const char *text;               /* I_TEXT */
    size_t length;
    /* A separator's row (I_TAB) or table (a row's I_BREAK), and an I_EDGE
     * that marks where a row or table opened; NULL for any other item. */
    const os64_html_node_t *owner;
} Item;

typedef struct {
    const os64_html_document_t *doc;
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

/* Takes back `owner`'s last separator (a row's last TAB, a table's last
 * row BREAK), searching back no further than the mark `owner` left when
 * it opened (all the way, for an owner opened outside the walk). */
static void take_back(Walk *w, ItemKind kind, const os64_html_node_t *owner)
{
    if (owner == NULL) return;          /* a row with no table: no separator is its */
    for (size_t at = w->count; at > 0; at--) {
        const Item *item = &w->items[at - 1];
        if (item->owner != owner) continue;
        if (item->kind == I_EDGE) return;
        if (item->kind != kind) continue;
        os64_memmove(&w->items[at - 1], &w->items[at], (w->count - at) * sizeof(Item));
        w->count--;
        return;
    }
}

/* The nearest ancestor of `node` whose display is `display`: a cell's row,
 * a row's table. NULL when there is none (a box CSS would wrap in an
 * anonymous one, which slice D's layout answers). */
static const os64_html_node_t *enclosing(Walk *w, const os64_html_node_t *node, uint8_t display)
{
    for (const os64_html_node_t *up = node->parent; up != NULL && up->kind == OS64_HTML_ELEMENT; up = up->parent) {
        os64_dom_text_style_t s;
        if (!facts_of(w, up, &s)) return NULL;
        if (s.display == display) return up;
    }
    return NULL;
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

/* The required line break count an element writes at both edges, 0 for
 * none: a `p`'s 2 whatever its display (the standard's step 8), a
 * block-level or table-caption box's 1. */
static uint32_t edge_count(const os64_html_node_t *node, const os64_dom_text_style_t *s)
{
    if (html_tag(node, OS64_HTML_TAG_P)) return 2;
    return s->display == OS64_DOM_TEXT_BLOCK || s->display == OS64_DOM_TEXT_TABLE ||
        s->display == OS64_DOM_TEXT_TABLE_CAPTION ? 1 : 0;
}

static bool boxed(const os64_dom_text_style_t *s)
{
    return s->display != OS64_DOM_TEXT_INLINE && s->display != OS64_DOM_TEXT_CONTENTS;
}

/* A Text node, as its white space says (§ 3.3). Its facts are its parent
 * in the flat tree's, the element whose box holds it: a host's Text child
 * that a slot takes inherits from the slot, and one no slot takes is in no
 * flat tree and gives nothing. */
static void text_items(Walk *w, const os64_html_node_t *text)
{
    if (text->text_len == 0) return;
    const os64_html_node_t *parent = os64_html_flat_parent(w->doc, text);
    os64_dom_text_style_t s;
    if (parent == NULL || parent->kind != OS64_HTML_ELEMENT || !facts_of(w, parent, &s)) return;
    if (!s.rendered || !s.visible) return;
    const char *at = text->text, *end = text->text + text->text_len;
    /* White space alone inside a table's own boxes is dropped whatever
     * `white-space` says (CSS 2.1 § 17.2.1), so it never sits between a
     * cell's tab and its row's end. */
    if (s.display == OS64_DOM_TEXT_TABLE || s.display == OS64_DOM_TEXT_TABLE_PART ||
        s.display == OS64_DOM_TEXT_TABLE_ROW) {
        const char *p = at;
        while (p < end && space_byte(*p)) p++;
        if (p == end) return;
    }
    if (s.white_space == OS64_DOM_SPACE_PRESERVE) {
        push(w, (Item){.kind = I_TEXT, .text = at, .length = text->text_len});
        return;
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
    /* A row's and a table's opening edge is the mark its take_back stops
     * at. */
    bool marks = s->display == OS64_DOM_TEXT_TABLE_ROW || s->display == OS64_DOM_TEXT_TABLE;
    if (boxed(s)) push(w, (Item){.kind = I_EDGE, .owner = marks ? node : NULL});
    uint32_t count = edge_count(node, s);
    if (count != 0 && s->visible) push(w, (Item){.kind = I_COUNT, .count = count});
    return !childless(node);
}

/* Leaving an element whose open answered true and that was gone into, or
 * that turned out to have no children. An element whose open answered
 * false (not rendered, a `br`, a childless one) is never closed. */
static void element_close(Walk *w, const os64_html_node_t *node)
{
    os64_dom_text_style_t s;
    if (!facts_of(w, node, &s)) return;
    if (s.display == OS64_DOM_TEXT_TABLE_ROW) take_back(w, I_TAB, node);
    if (s.display == OS64_DOM_TEXT_TABLE) take_back(w, I_BREAK, node);
    uint32_t count = edge_count(node, &s);
    if (count != 0 && s.visible) push(w, (Item){.kind = I_COUNT, .count = count});
    if (boxed(&s)) push(w, (Item){.kind = I_EDGE});
    if (s.display == OS64_DOM_TEXT_TABLE_CELL)
        push(w, (Item){.kind = I_TAB, .silent = !s.visible,
                       .owner = enclosing(w, node, OS64_DOM_TEXT_TABLE_ROW)});
    if (s.display == OS64_DOM_TEXT_TABLE_ROW)
        push(w, (Item){.kind = I_BREAK, .silent = !s.visible,
                       .owner = enclosing(w, node, OS64_DOM_TEXT_TABLE)});
}

/* Whether an item ends a line for a collapsible space beside it: a count,
 * a break, a tab or an edge; or preserved text that begins (`after`
 * false) or ends (`after` true) with a line feed. */
static bool line_edge(const Item *item, bool after)
{
    if (item->kind == I_TEXT)
        return item->length != 0 && line_feed(after ? item->text[item->length - 1] : item->text[0]);
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
             * not a space) goes, and so does one at the very end; the
             * spaces after it in the run go with it, unvisited. */
            size_t j = i + 1;
            while (j < w->count && w->items[j].kind == I_SPACE) j++;
            if (j == w->count || line_edge(&w->items[j], false)) {
                i = j - 1;
                break;
            }
            if (out != NULL) out[n] = ' ';
            n++;
            spaced = true;
            break;
        }
        default: {                  /* I_TEXT, I_BREAK, I_TAB */
            if (item->silent) break;
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
            line_open = item->kind == I_TEXT && !line_edge(item, true);
            break;
        }
        }
    }
    return n;
}

/* Whether a row group read as the root holds its table's last row: no
 * rendered row or row group follows it in the table. */
static bool last_row_group(Walk *w, const os64_html_node_t *group)
{
    for (const os64_html_node_t *n = group->next; n != NULL; n = n->next) {
        os64_dom_text_style_t s;
        if (n->kind != OS64_HTML_ELEMENT) continue;
        if (!facts_of(w, n, &s)) return false;
        if (s.rendered && (s.display == OS64_DOM_TEXT_TABLE_PART || s.display == OS64_DOM_TEXT_TABLE_ROW))
            return false;
    }
    return true;
}

d_inner_text_status_t d_inner_text(const os64_html_document_t *doc, const os64_html_node_t *root,
                                   os64_dom_text_style_provider_t facts, void *opaque,
                                   const d_inner_text_memory_t *memory, char **out, size_t *length)
{
    *out = NULL;
    *length = 0;
    if (doc == NULL || root == NULL || root->kind != OS64_HTML_ELEMENT || facts == NULL)
        return D_INNER_TEXT_NOT_RENDERED;
    /* The provider makes its style current once, before any element is
     * asked about (dom/text_style.h). */
    os64_dom_text_style_t s;
    if (!facts(opaque, NULL, &s)) return D_INNER_TEXT_NOT_RENDERED;
    Walk w = {.doc = doc, .facts = facts, .opaque = opaque, .memory = memory};
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
            text_items(&w, node);
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
     * alone takes back its separator as its close would have, and so does
     * a row group holding its table's last row. */
    if (s.display == OS64_DOM_TEXT_TABLE_ROW) take_back(&w, I_TAB, root);
    if (s.display == OS64_DOM_TEXT_TABLE) take_back(&w, I_BREAK, root);
    if (s.display == OS64_DOM_TEXT_TABLE_PART && last_row_group(&w, root))
        take_back(&w, I_BREAK, enclosing(&w, root, OS64_DOM_TEXT_TABLE));
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
