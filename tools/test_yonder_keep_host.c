// yonder's kept drawings' store (userland/apps/yonder/keep.h; PILE3.md §
// Transforms, the painter: Kept drawings) on the host, under the
// sanitizers: what it answers each ask, what it holds and lets go, and
// that every byte it was given is freed exactly once (ASan's leak check
// at exit, and a count of the bytes outstanding).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keep.h"

// The store's memory: the heap, counted, refused when a case says so.
static long s_blocks;
static bool s_starve;

void *os64_malloc(size_t n)
{
    if (s_starve)
        return NULL;
    void *p = malloc(n);
    if (p != NULL)
        s_blocks++;
    return p;
}

void os64_free(void *p)
{
    if (p != NULL)
        s_blocks--;
    free(p);
}

void *os64_memset(void *dst, int c, size_t n)
{
    return memset(dst, c, n);
}

static int s_failed, s_checks;

static void expect(const char *what, bool ok)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL %s\n", what);
    }
}

// Boxes the keys name: only their addresses are read.
static flow_box_t s_boxes[YONDER_KEEPS_GROUPS + 2];

static yonder_keep_key_t key_of(int box, uint64_t tree, uint64_t drawn, int32_t w, int32_t h)
{
    return (yonder_keep_key_t){tree, &s_boxes[box], drawn, {-3, -4, w, h}, {0, 0}};
}

// Sheets for `key`, from the counted heap, black and white.
static void sheets_for(const yonder_keep_key_t *key, uint32_t **black, uint32_t **white)
{
    size_t n = (size_t)key->bounds.w * (size_t)key->bounds.h;
    *black = os64_malloc(n * sizeof(uint32_t));
    *white = os64_malloc(n * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) {
        (*black)[i] = 0xff000000u | (uint32_t)i;
        (*white)[i] = 0xffffffffu;
    }
}

// A group asked for until it is made and held: NO, NO again in the same
// paint, MAKE in the next, then the put. The paint begun is the tree's.
static void make(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face)
{
    expect("first ask is no", yonder_keeps_ask(k, key, face) == YONDER_KEEP_NO);
    expect("again in this paint, still no", yonder_keeps_ask(k, key, face) == YONDER_KEEP_NO);
    yonder_keeps_paint(k, key->tree);
    expect("in the next paint, make", yonder_keeps_ask(k, key, face) == YONDER_KEEP_MAKE);
    uint32_t *b, *w;
    sheets_for(key, &b, &w);
    expect("put takes them", yonder_keeps_put(k, key, face, b, w));
}

static void two_paints_then_held(void)
{
    yonder_keeps_t *k = yonder_keeps_new(1 << 20);
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t key = key_of(0, 7, 3, 10, 8);
    make(k, &key, 1);
    expect("held is have", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_HAVE);
    const uint32_t *b = NULL, *w = NULL;
    expect("its sheets", yonder_keeps_sheets(k, &key, 1, &b, &w) && b != NULL && w != NULL &&
                             b[5] == (0xff000000u | 5u) && w[5] == 0xffffffffu);
    yonder_keeps_counts_t c = yonder_keeps_counts(k);
    expect("counts", c.laid == 1 && c.made == 1 && c.seen == 1 && c.held == 1 &&
                         c.bytes == 10 * 8 * 2 * sizeof(uint32_t) && c.budget == 1 << 20);
    // A later paint of the same tree keeps it.
    yonder_keeps_paint(k, 7);
    expect("held across paints", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_HAVE);
    // Laid is counted when the sheets are handed out, not at the ask: a
    // HAVE the face then cannot open is no drawing laid.
    expect("an ask alone lays nothing", yonder_keeps_counts(k).laid == 1);
    expect("sheets handed out lay one", yonder_keeps_sheets(k, &key, 1, &b, &w) &&
                                            yonder_keeps_counts(k).laid == 2);
    // Sheets go only to the key and the stamp they were made under.
    yonder_keep_key_t drawn = key_of(0, 7, 4, 10, 8);
    expect("no sheets for the box drawn since", !yonder_keeps_sheets(k, &drawn, 1, &b, &w) &&
                                                    yonder_keeps_counts(k).laid == 2);
    expect("no sheets under another stamp", !yonder_keeps_sheets(k, &key, 2, &b, &w) &&
                                                yonder_keeps_counts(k).laid == 2);
    yonder_keeps_free(k);
    expect("all freed", s_blocks == 0);
}

static void what_moves_it(void)
{
    yonder_keeps_t *k = yonder_keeps_new(1 << 20);
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t key = key_of(0, 7, 3, 10, 8);
    make(k, &key, 1);
    // The face's stamp moved: seen afresh, its drawing let go.
    expect("stamp moved is no", yonder_keeps_ask(k, &key, 2) == YONDER_KEEP_NO);
    expect("let go", yonder_keeps_counts(k).bytes == 0 && yonder_keeps_counts(k).held == 0);
    const uint32_t *b, *w;
    expect("no sheets under the old stamp", !yonder_keeps_sheets(k, &key, 1, &b, &w));
    expect("no sheets under the new one yet", !yonder_keeps_sheets(k, &key, 2, &b, &w));
    yonder_keep_key_t none = {0};
    expect("a key with no box is never kept", yonder_keeps_ask(k, &none, 0) == YONDER_KEEP_NO);
    yonder_keeps_paint(k, 7);
    expect("nor in a later paint", yonder_keeps_ask(k, &none, 0) == YONDER_KEEP_NO);
    expect("made again under the new stamp", yonder_keeps_ask(k, &key, 2) == YONDER_KEEP_MAKE);
    // Each part of the key starts it again.
    yonder_keep_key_t moved[6] = {key, key, key, key, key, key};
    moved[0].drawn++;
    moved[1].bounds.x++;
    moved[2].bounds.w++;
    moved[3].scroll.y++;
    moved[4].tree++;
    moved[5].bounds.h--;
    for (int i = 0; i < 6; i++) {
        uint32_t *bb, *ww;
        if (i > 0) {
            yonder_keeps_paint(k, 7);
            expect("made again", yonder_keeps_ask(k, &key, 2) == YONDER_KEEP_MAKE);
        }
        sheets_for(&key, &bb, &ww);
        expect("put", yonder_keeps_put(k, &key, 2, bb, ww));
        expect("held before the change", yonder_keeps_ask(k, &key, 2) == YONDER_KEEP_HAVE);
        expect("a part of the key changed is no", yonder_keeps_ask(k, &moved[i], 2) == YONDER_KEEP_NO);
        expect("and let go", yonder_keeps_counts(k).held == 0);
        // Back to the first key: seen afresh.
        expect("back is no", yonder_keeps_ask(k, &key, 2) == YONDER_KEEP_NO);
    }
    yonder_keeps_free(k);
    expect("all freed", s_blocks == 0);
}

static void put_must_match(void)
{
    yonder_keeps_t *k = yonder_keeps_new(1 << 20);
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t key = key_of(0, 7, 3, 10, 8), other = key_of(0, 7, 4, 10, 8);
    uint32_t *b, *w;
    sheets_for(&key, &b, &w);
    expect("never asked: refused", !yonder_keeps_put(k, &key, 1, b, w));
    yonder_keeps_ask(k, &key, 1);
    yonder_keeps_paint(k, 7);
    expect("make", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_MAKE);
    expect("another key: refused", !yonder_keeps_put(k, &other, 1, b, w));
    expect("another stamp: refused", !yonder_keeps_put(k, &key, 9, b, w));
    expect("no sheets: refused", !yonder_keeps_put(k, &key, 1, NULL, w));
    expect("the asked key: taken", yonder_keeps_put(k, &key, 1, b, w));
    uint32_t *b2, *w2;
    sheets_for(&key, &b2, &w2);
    expect("held already: refused", !yonder_keeps_put(k, &key, 1, b2, w2));
    os64_free(b2);
    os64_free(w2);
    yonder_keeps_free(k);
    expect("all freed", s_blocks == 0);
}

static void spoiled_until_moved(void)
{
    yonder_keeps_t *k = yonder_keeps_new(1 << 20);
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t key = key_of(0, 7, 3, 10, 8);
    yonder_keeps_ask(k, &key, 1);
    yonder_keeps_paint(k, 7);
    expect("make", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_MAKE);
    yonder_keeps_spoiled(k, &key, 9);       // another stamp: not this one's
    yonder_keeps_paint(k, 7);
    expect("still make", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_MAKE);
    yonder_keeps_spoiled(k, &key, 1);
    yonder_keeps_paint(k, 7);
    expect("spoiled is no", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_NO);
    yonder_keeps_paint(k, 7);
    expect("and stays no", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_NO);
    uint32_t *b, *w;
    sheets_for(&key, &b, &w);
    expect("a spoiled one takes no put", !yonder_keeps_put(k, &key, 1, b, w));
    os64_free(b);
    os64_free(w);
    expect("counted", yonder_keeps_counts(k).spoiled == 1);
    key.drawn++;
    expect("a new key is seen afresh", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_NO);
    yonder_keeps_paint(k, 7);
    expect("and made", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_MAKE);
    yonder_keeps_free(k);
    expect("all freed", s_blocks == 0);
}

static void the_budget(void)
{
    // Room for two 10x10 drawings (800 bytes each), not three.
    yonder_keeps_t *k = yonder_keeps_new(1600);
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t a = key_of(0, 7, 0, 10, 10), b = key_of(1, 7, 0, 10, 10),
                      c = key_of(2, 7, 0, 10, 10), big = key_of(3, 7, 0, 21, 10),
                      d = key_of(4, 7, 0, 10, 10);
    make(k, &a, 0);
    make(k, &b, 0);
    yonder_keeps_ask(k, &c, 0);
    yonder_keeps_ask(k, &big, 0);
    // A paint that asks for both held drawings first: no room for a third,
    // and it stays NO.
    yonder_keeps_paint(k, 7);
    expect("a held", yonder_keeps_ask(k, &a, 0) == YONDER_KEEP_HAVE);
    expect("b held", yonder_keeps_ask(k, &b, 0) == YONDER_KEEP_HAVE);
    expect("no room for c this paint", yonder_keeps_ask(k, &c, 0) == YONDER_KEEP_NO);
    expect("counted", yonder_keeps_counts(k).no_room == 1);
    // One past the whole budget is never kept.
    expect("past the budget", yonder_keeps_ask(k, &big, 0) == YONDER_KEEP_NO);
    yonder_keeps_paint(k, 7);
    expect("c stays no", yonder_keeps_ask(k, &c, 0) == YONDER_KEEP_NO);
    // A paint that asks for b only: a is let go for d, the oldest asked.
    yonder_keeps_paint(k, 7);
    expect("b held still", yonder_keeps_ask(k, &b, 0) == YONDER_KEEP_HAVE);
    yonder_keeps_ask(k, &d, 0);
    yonder_keeps_paint(k, 7);
    expect("b held again", yonder_keeps_ask(k, &b, 0) == YONDER_KEEP_HAVE);
    expect("d made, room found", yonder_keeps_ask(k, &d, 0) == YONDER_KEEP_MAKE);
    uint32_t *bb, *ww;
    sheets_for(&d, &bb, &ww);
    expect("d put", yonder_keeps_put(k, &d, 0, bb, ww));
    expect("two held", yonder_keeps_counts(k).held == 2 && yonder_keeps_counts(k).bytes == 1600);
    // a was let go; b and d are this paint's, so there is no room for it.
    expect("a let go, no room for it now", yonder_keeps_ask(k, &a, 0) == YONDER_KEEP_NO);
    yonder_keeps_free(k);
    expect("all freed", s_blocks == 0);
}

// A put re-checks the room: what this paint made since the ask may hold
// it now.
static void put_finds_no_room(void)
{
    yonder_keeps_t *k = yonder_keeps_new(800);
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t a = key_of(0, 7, 0, 10, 10), b = key_of(1, 7, 0, 10, 10);
    yonder_keeps_ask(k, &a, 0);
    yonder_keeps_ask(k, &b, 0);
    yonder_keeps_paint(k, 7);
    expect("a make", yonder_keeps_ask(k, &a, 0) == YONDER_KEEP_MAKE);
    expect("b make", yonder_keeps_ask(k, &b, 0) == YONDER_KEEP_MAKE);
    uint32_t *ab, *aw, *bb, *bw;
    sheets_for(&a, &ab, &aw);
    sheets_for(&b, &bb, &bw);
    expect("a put", yonder_keeps_put(k, &a, 0, ab, aw));
    expect("b no room", !yonder_keeps_put(k, &b, 0, bb, bw));
    os64_free(bb);
    os64_free(bw);
    yonder_keeps_paint(k, 7);
    expect("b no", yonder_keeps_ask(k, &b, 0) == YONDER_KEEP_NO);
    yonder_keeps_free(k);
    expect("all freed", s_blocks == 0);
}

static void another_tree(void)
{
    yonder_keeps_t *k = yonder_keeps_new(1 << 20);
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t key = key_of(0, 7, 3, 10, 8);
    make(k, &key, 1);
    yonder_keeps_paint(k, 8);
    expect("let go for another tree", yonder_keeps_counts(k).held == 0 &&
                                          yonder_keeps_counts(k).bytes == 0 && s_blocks == 1);
    key.tree = 8;
    expect("seen afresh in it", yonder_keeps_ask(k, &key, 1) == YONDER_KEEP_NO);
    yonder_keeps_free(k);
    expect("all freed", s_blocks == 0);
}

static void the_rows(void)
{
    yonder_keeps_t *k = yonder_keeps_new(1 << 24);
    yonder_keeps_paint(k, 7);
    // Every row this paint's: one more is not remembered.
    for (int i = 0; i < YONDER_KEEPS_GROUPS; i++) {
        yonder_keep_key_t key = key_of(i, 7, 0, 2, 2);
        yonder_keeps_ask(k, &key, 0);
    }
    yonder_keep_key_t extra = key_of(YONDER_KEEPS_GROUPS, 7, 0, 2, 2);
    expect("no row for it", yonder_keeps_ask(k, &extra, 0) == YONDER_KEEP_NO);
    // The next paint: row 0, seen in the last, made and held; the oldest
    // row this paint has not asked for is taken for the extra one.
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t first = key_of(0, 7, 0, 2, 2);
    expect("row 0 made", yonder_keeps_ask(k, &first, 0) == YONDER_KEEP_MAKE);
    uint32_t *fb, *fw;
    sheets_for(&first, &fb, &fw);
    expect("row 0 put", yonder_keeps_put(k, &first, 0, fb, fw));
    expect("a row found, seen afresh", yonder_keeps_ask(k, &extra, 0) == YONDER_KEEP_NO);
    yonder_keeps_paint(k, 7);
    expect("made in the next paint", yonder_keeps_ask(k, &extra, 0) == YONDER_KEEP_MAKE);
    expect("not the held one", yonder_keeps_ask(k, &first, 0) == YONDER_KEEP_HAVE);
    yonder_keep_key_t second = key_of(1, 7, 0, 2, 2);
    expect("the oldest was forgotten", yonder_keeps_ask(k, &second, 0) == YONDER_KEEP_NO);
    yonder_keeps_free(k);
    expect("all freed", s_blocks == 0);
}

static void counts_restart_and_null(void)
{
    yonder_keeps_t *k = yonder_keeps_new(1 << 20);
    yonder_keeps_paint(k, 7);
    yonder_keep_key_t key = key_of(0, 7, 3, 10, 8);
    make(k, &key, 1);
    yonder_keeps_counts_restart(k);
    yonder_keeps_counts_t c = yonder_keeps_counts(k);
    expect("restarted, what is held still said",
           c.laid == 0 && c.made == 0 && c.seen == 0 && c.held == 1 && c.bytes > 0);
    yonder_keeps_free(k);
    // NULL everywhere.
    expect("null ask", yonder_keeps_ask(NULL, &key, 1) == YONDER_KEEP_NO);
    const uint32_t *b, *w;
    expect("null sheets", !yonder_keeps_sheets(NULL, &key, 1, &b, &w));
    expect("null put", !yonder_keeps_put(NULL, &key, 1, NULL, NULL));
    yonder_keeps_spoiled(NULL, &key, 1);
    yonder_keeps_paint(NULL, 1);
    yonder_keeps_counts_restart(NULL);
    yonder_keeps_free(NULL);
    c = yonder_keeps_counts(NULL);
    expect("null counts", c.laid == 0 && c.budget == 0);
    s_starve = true;
    expect("no memory, no store", yonder_keeps_new(1 << 20) == NULL);
    s_starve = false;
    expect("all freed", s_blocks == 0);
}

int main(void)
{
    two_paints_then_held();
    what_moves_it();
    put_must_match();
    spoiled_until_moved();
    the_budget();
    put_finds_no_room();
    another_tree();
    the_rows();
    counts_restart_and_null();
    printf("yonder keep: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
