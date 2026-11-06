#include "internal.h"

/* LIVE RANGES (docs/design/pending/RANGE.md § 3, § 4).
 *
 * A live range is two boundary points, each a node and an offset: UTF-16
 * code units into a Text, Comment or PI node's data, a child index into any
 * other node. The DOM Standard keeps every live range right across every
 * mutation by steps written into the mutation algorithms, and this file
 * holds those steps. They run inside libhtml because every link and unlink
 * of a node, the parser's included, passes through h_attach and h_detach
 * (core.c), at the moments the steps are written for; a listener told after
 * the fact could not recover a moved node's old index, nor where in a text
 * an edit fell.
 *
 * A range holds both its boundary nodes, so a removed subtree a range
 * points into is not reclaimed under it. The steps run in the middle of a
 * verb, so they drop holds without collecting (`drop`): the verb's own
 * collection, after its links are stable, reclaims what is free.
 *
 * The walks here are plain-tree (->parent): a range's root is its tree's
 * root, not the shadow-including one, as the standard says. */

struct os64_html_range {
    HDoc *doc;
    HNode *node[2];         /* start, end */
    uint32_t offset[2];
    struct os64_html_range *prev, *next;
};

/* ── UTF-16 over UTF-8 ───────────────────────────────────────────────── */

uint32_t h_utf16_length(const char *s, size_t n)
{
    uint32_t units = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80)
            continue;                       /* a continuation byte */
        units += c >= 0xF0 ? 2 : 1;         /* past the BMP: a surrogate pair */
    }
    return units;
}

/* The byte offset of UTF-16 unit `unit` in `s`, snapped down to the start
 * of its code point when it falls between a surrogate pair's halves (a pair
 * is never split, RANGE.md § 4); `n` past the last unit. */
size_t h_utf16_byte(const char *s, size_t n, uint32_t unit)
{
    uint32_t at = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80)
            continue;
        uint32_t width = c >= 0xF0 ? 2 : 1;
        if (at + width > unit)
            return i;
        at += width;
    }
    return n;
}

/* `unit` snapped down to a code point's start in `s`. */
static uint32_t snap(const char *s, size_t n, uint32_t unit)
{
    size_t byte = h_utf16_byte(s, n, unit);
    return h_utf16_length(s, byte);
}

static bool character_data(const HNode *n)
{
    return n->kind == OS64_HTML_TEXT || n->kind == OS64_HTML_COMMENT || n->kind == OS64_HTML_PI;
}

uint32_t os64_html_node_length(const HNode *n)
{
    if (n == NULL || n->kind == OS64_HTML_DOCTYPE)
        return 0;
    if (character_data(n))
        return h_utf16_length(n->text, n->text_len);
    uint32_t count = 0;
    for (const HNode *c = n->first_child; c != NULL; c = c->next)
        count++;
    return count;
}

/* ── The registry ────────────────────────────────────────────────────── */

static void hold(HDoc *d, HNode *n)
{
    d_ref(d, n, false, true);
    d->held++;
}

static void drop(HDoc *d, HNode *n)
{
    d_ref(d, n, false, false);
    d->held--;
}

/* Boundary `i` of `r` becomes (n, offset), the hold moving with it. */
static void point(HDoc *d, os64_html_range_t *r, int i, HNode *n, uint32_t offset)
{
    if (r->node[i] != n) {
        hold(d, n);
        drop(d, r->node[i]);
        r->node[i] = n;
    }
    r->offset[i] = offset;
}

os64_html_range_t *os64_html_range_new(os64_html_document_t *doc)
{
    HDoc *d = (HDoc *)doc;
    if (d == NULL)
        return NULL;
    int64_t why = OS64_HTML_OK;
    os64_html_range_t *r = d_alloc(d, sizeof(*r), &why);
    if (r == NULL)
        return NULL;
    *r = (os64_html_range_t){.doc = d, .node = {&d->root, &d->root}};
    hold(d, &d->root);
    hold(d, &d->root);
    r->next = d->ranges;
    if (d->ranges != NULL)
        d->ranges->prev = r;
    d->ranges = r;
    return r;
}

void os64_html_range_free(os64_html_range_t *r)
{
    if (r == NULL)
        return;
    HDoc *d = r->doc;
    if (r->prev != NULL)
        r->prev->next = r->next;
    else
        d->ranges = r->next;
    if (r->next != NULL)
        r->next->prev = r->prev;
    drop(d, r->node[0]);
    drop(d, r->node[1]);
    h_free(d, r);
    d_collect(d);
}

int64_t os64_html_range_set(os64_html_range_t *r, int which, os64_html_node_t *node, uint32_t offset)
{
    if (r == NULL || (which != 0 && which != 1) || !os64_html_owns_node(&r->doc->pub, node))
        return OS64_HTML_BAD_ARGUMENT;
    if (node->kind == OS64_HTML_DOCTYPE)
        return OS64_HTML_INVALID_NODE_TYPE;
    if (offset > os64_html_node_length(node))
        return OS64_HTML_INDEX_SIZE;
    if (character_data(node))
        offset = snap(node->text, node->text_len, offset);
    point(r->doc, r, which, node, offset);
    d_collect(r->doc);
    return OS64_HTML_OK;
}

void os64_html_range_get(const os64_html_range_t *r, int which, const os64_html_node_t **node, uint32_t *offset)
{
    *node = r->node[which != 0];
    *offset = r->offset[which != 0];
}

/* ── The mutation steps (DOM § 4.2.3, § 4.10, § 4.11) ────────────────── */

static uint32_t index_of(const HNode *n)
{
    uint32_t i = 0;
    for (const HNode *p = n->prev; p != NULL; p = p->prev)
        i++;
    return i;
}

static bool inclusive_descendant(const HNode *n, const HNode *of)
{
    for (; n != NULL; n = n->parent)
        if (n == of)
            return true;
    return false;
}

/* The removing steps, before `n` is unlinked from its parent: a boundary in
 * its subtree goes to (parent, n's index); a boundary at (parent, past
 * n's index) loses one. The index — a sibling walk — is taken only when a
 * boundary needs it. */
void d_ranges_unlinking(HDoc *d, HNode *n)
{
    HNode *parent = n->parent;
    if (parent == NULL)
        return;
    bool known = false;
    uint32_t index = 0;
    for (os64_html_range_t *r = d->ranges; r != NULL; r = r->next)
        for (int i = 0; i < 2; i++) {
            bool moved = inclusive_descendant(r->node[i], n);
            if (!moved && r->node[i] != parent)
                continue;
            if (!known) {
                index = index_of(n);
                known = true;
            }
            if (moved)
                point(d, r, i, parent, index);
            else if (r->offset[i] > index)
                r->offset[i]--;
        }
}

/* The inserting steps, before a node is linked into `parent` before
 * `before`: a boundary at (parent, past before's index) gains one. An
 * append (`before` NULL) moves nothing and computes nothing. */
void d_ranges_linking(HDoc *d, HNode *parent, HNode *before)
{
    if (before == NULL)
        return;
    bool known = false;
    uint32_t index = 0;
    for (os64_html_range_t *r = d->ranges; r != NULL; r = r->next)
        for (int i = 0; i < 2; i++) {
            if (r->node[i] != parent)
                continue;
            if (!known) {
                index = index_of(before);
                known = true;
            }
            if (r->offset[i] > index)
                r->offset[i]++;
        }
}

/* Replace data's steps (`count` units at `offset` became `length` units):
 * a boundary inside the replaced units goes to `offset`, one past them
 * shifts by the difference. */
void d_ranges_replaced(HDoc *d, HNode *n, uint32_t offset, uint32_t count, uint32_t length)
{
    for (os64_html_range_t *r = d->ranges; r != NULL; r = r->next)
        for (int i = 0; i < 2; i++) {
            if (r->node[i] != n || r->offset[i] <= offset)
                continue;
            if (r->offset[i] <= offset + count)
                r->offset[i] = offset;
            else
                r->offset[i] = r->offset[i] - count + length;
        }
}

/* Split a Text node's steps, after `tail` is inserted after `n` and before
 * `n` is truncated at `offset`: a boundary in `n` past `offset` moves into
 * `tail`; a boundary at (parent, n's index + 1) gains one. */
void d_ranges_split(HDoc *d, HNode *n, HNode *tail, uint32_t offset)
{
    HNode *parent = n->parent;
    bool known = false;
    uint32_t index = 0;
    for (os64_html_range_t *r = d->ranges; r != NULL; r = r->next)
        for (int i = 0; i < 2; i++) {
            if (r->node[i] == n && r->offset[i] > offset) {
                point(d, r, i, tail, r->offset[i] - offset);
            } else if (parent != NULL && r->node[i] == parent) {
                if (!known) {
                    index = index_of(n);
                    known = true;
                }
                if (r->offset[i] == index + 1)
                    r->offset[i]++;
            }
        }
}

/* Normalize's steps for `current`, a Text sibling merged into `into` after
 * `length` units: a boundary in `current` moves into `into`, shifted by
 * `length`; a boundary at (parent, current's index) moves to (into,
 * length). */
void d_ranges_merged(HDoc *d, HNode *into, HNode *current, uint32_t length)
{
    HNode *parent = current->parent;
    bool known = false;
    uint32_t index = 0;
    for (os64_html_range_t *r = d->ranges; r != NULL; r = r->next)
        for (int i = 0; i < 2; i++) {
            if (r->node[i] == current) {
                point(d, r, i, into, r->offset[i] + length);
            } else if (parent != NULL && r->node[i] == parent) {
                if (!known) {
                    index = index_of(current);
                    known = true;
                }
                if (r->offset[i] == index)
                    point(d, r, i, into, length);
            }
        }
}
