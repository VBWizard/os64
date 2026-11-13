// keep.c — the kept drawings' store (keep.h).
//
// A few groups turn on a page at once, so the store is a small table
// searched end to end: one row for each group asked about, found by its
// box, the tree being the same for every row (yonder_keeps_paint lets go
// of the rest). A clock counts the asks; a paint's begin reads it, so a
// row asked since is that paint's.

#include "keep.h"
#include "os64/mem.h"
#include "os64/str.h"

typedef enum { K_EMPTY = 0, K_SEEN, K_MADE, K_NO } KeptState;

typedef struct {
    KeptState state;
    yonder_keep_key_t key;
    uint64_t face;
    uint32_t *sheet[2];
    size_t bytes;
    uint64_t asked;             // the clock at its last ask
} Kept;

struct yonder_keeps {
    Kept rows[YONDER_KEEPS_GROUPS];
    size_t budget, bytes;
    uint64_t tree, clock, paint;
    uint64_t laid, made, seen, spoiled, no_room, held;
};

yonder_keeps_t *yonder_keeps_new(size_t budget)
{
    yonder_keeps_t *k = os64_malloc(sizeof(*k));
    if (k == NULL)
        return NULL;
    os64_memset(k, 0, sizeof(*k));
    k->budget = budget;
    return k;
}

// A row's drawing let go; the row stays.
static void let_go(yonder_keeps_t *k, Kept *r)
{
    if (r->state == K_MADE) {
        os64_free(r->sheet[0]);
        os64_free(r->sheet[1]);
        r->sheet[0] = r->sheet[1] = NULL;
        k->bytes -= r->bytes;
        k->held--;
        r->bytes = 0;
    }
}

void yonder_keeps_free(yonder_keeps_t *k)
{
    if (k == NULL)
        return;
    for (int32_t i = 0; i < YONDER_KEEPS_GROUPS; i++)
        let_go(k, &k->rows[i]);
    os64_free(k);
}

void yonder_keeps_paint(yonder_keeps_t *k, uint64_t tree)
{
    if (k == NULL)
        return;
    if (tree != k->tree) {
        for (int32_t i = 0; i < YONDER_KEEPS_GROUPS; i++) {
            let_go(k, &k->rows[i]);
            k->rows[i].state = K_EMPTY;
        }
        k->tree = tree;
    }
    k->paint = ++k->clock;
}

static bool same(const Kept *r, const yonder_keep_key_t *key, uint64_t face)
{
    const yonder_keep_key_t *a = &r->key;
    return a->tree == key->tree && a->box == key->box && a->drawn == key->drawn &&
           a->bounds.x == key->bounds.x && a->bounds.y == key->bounds.y &&
           a->bounds.w == key->bounds.w && a->bounds.h == key->bounds.h &&
           a->scroll.x == key->scroll.x && a->scroll.y == key->scroll.y && r->face == face;
}

static Kept *row_of(yonder_keeps_t *k, const flow_box_t *box)
{
    for (int32_t i = 0; i < YONDER_KEEPS_GROUPS; i++)
        if (k->rows[i].state != K_EMPTY && k->rows[i].key.box == box)
            return &k->rows[i];
    return NULL;
}

// Both sheets of `bounds`, in bytes; 0 when that is past what a size can
// say.
static size_t sheet_bytes(os64_gui_rect_t bounds)
{
    if (bounds.w <= 0 || bounds.h <= 0)
        return 0;
    uint64_t px = (uint64_t)bounds.w * (uint64_t)bounds.h;
    if (px > SIZE_MAX / (2 * sizeof(uint32_t)))
        return 0;
    return (size_t)px * 2 * sizeof(uint32_t);
}

// Room for `need` more bytes, letting go of drawings this paint has not
// asked for, least recently asked first, never `keep`'s. False when even
// that is not enough, and nothing is let go.
static bool room(yonder_keeps_t *k, size_t need, const Kept *keep)
{
    if (need > k->budget)
        return false;
    size_t could = k->budget - k->bytes;
    for (int32_t i = 0; i < YONDER_KEEPS_GROUPS; i++) {
        const Kept *r = &k->rows[i];
        if (r != keep && r->state == K_MADE && r->asked <= k->paint)
            could += r->bytes;
    }
    if (could < need)
        return false;
    while (k->budget - k->bytes < need) {
        Kept *oldest = NULL;
        for (int32_t i = 0; i < YONDER_KEEPS_GROUPS; i++) {
            Kept *r = &k->rows[i];
            if (r != keep && r->state == K_MADE && r->asked <= k->paint &&
                (oldest == NULL || r->asked < oldest->asked))
                oldest = r;
        }
        let_go(k, oldest);
        oldest->state = K_SEEN;
    }
    return true;
}

int yonder_keeps_ask(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face)
{
    if (k == NULL || key == NULL || key->box == NULL)
        return YONDER_KEEP_NO;
    Kept *r = row_of(k, key->box);
    if (r == NULL) {
        // A free row, or the one asked longest ago that this paint has not.
        for (int32_t i = 0; i < YONDER_KEEPS_GROUPS; i++) {
            Kept *c = &k->rows[i];
            if (c->state == K_EMPTY) {
                r = c;
                break;
            }
            if (c->asked <= k->paint && (r == NULL || c->asked < r->asked))
                r = c;
        }
        if (r == NULL)
            return YONDER_KEEP_NO;
        let_go(k, r);
        r->state = K_EMPTY;
    }
    bool earlier = r->asked <= k->paint;
    r->asked = ++k->clock;
    if (r->state == K_EMPTY || !same(r, key, face)) {
        let_go(k, r);
        r->state = K_SEEN;
        r->key = *key;
        r->face = face;
        k->seen++;
        return YONDER_KEEP_NO;
    }
    if (r->state == K_MADE)
        return YONDER_KEEP_HAVE;
    // Seen under this key once already, but only in this paint: a group
    // the paint met in two of its parts has not yet held still.
    if (r->state == K_NO || !earlier)
        return YONDER_KEEP_NO;
    size_t need = sheet_bytes(key->bounds);
    if (need == 0 || !room(k, need, r)) {
        r->state = K_NO;
        k->no_room++;
        return YONDER_KEEP_NO;
    }
    return YONDER_KEEP_MAKE;
}

bool yonder_keeps_sheets(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face,
                         const uint32_t **black, const uint32_t **white)
{
    Kept *r = k != NULL && key != NULL ? row_of(k, key->box) : NULL;
    if (r == NULL || r->state != K_MADE || !same(r, key, face))
        return false;
    *black = r->sheet[0];
    *white = r->sheet[1];
    k->laid++;
    return true;
}

bool yonder_keeps_put(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face,
                      uint32_t *black, uint32_t *white)
{
    Kept *r = k != NULL && key != NULL ? row_of(k, key->box) : NULL;
    if (r == NULL || r->state != K_SEEN || !same(r, key, face) || black == NULL || white == NULL)
        return false;
    size_t need = sheet_bytes(key->bounds);
    if (need == 0 || !room(k, need, r)) {
        r->state = K_NO;
        k->no_room++;
        return false;
    }
    r->sheet[0] = black;
    r->sheet[1] = white;
    r->bytes = need;
    r->state = K_MADE;
    k->bytes += need;
    k->held++;
    k->made++;
    return true;
}

void yonder_keeps_spoiled(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face)
{
    Kept *r = k != NULL && key != NULL ? row_of(k, key->box) : NULL;
    if (r == NULL || r->state != K_SEEN || !same(r, key, face))
        return;
    r->state = K_NO;
    k->spoiled++;
}

yonder_keeps_counts_t yonder_keeps_counts(const yonder_keeps_t *k)
{
    yonder_keeps_counts_t c = {0};
    if (k == NULL)
        return c;
    c.laid = k->laid;
    c.made = k->made;
    c.seen = k->seen;
    c.spoiled = k->spoiled;
    c.no_room = k->no_room;
    c.bytes = k->bytes;
    c.held = k->held;
    c.budget = k->budget;
    return c;
}

void yonder_keeps_counts_restart(yonder_keeps_t *k)
{
    if (k == NULL)
        return;
    k->laid = k->made = k->seen = k->spoiled = k->no_room = 0;
}
