/* The mapping list's core (kernel/src/memory/maplist.c) beside a model.
 *
 * Nodes are malloc'd and freed exactly when a call says the caller frees,
 * so under ASan a free with a hold outstanding, or a node still reachable
 * from the list after its free, is a use-after-free report, and a node the
 * core never says to free is a leak. The model keeps each node's range,
 * holds, listed flag and link order, and the list's order, applying the
 * core's rule to it: a link goes to the head, and a hit by address moves to
 * the head unless a node ahead overlaps it and was linked after it. After
 * every step the list is walked and compared with it, and every lookup must
 * find, of the nodes holding the address, the one linked last.
 *
 *   test_maplist <random rounds> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory/maplist.h"

static int s_failed, s_checks;

static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

#define MAX_NODES 64

typedef struct {
    maplist_node_t *n;      /* NULL once freed */
    uintptr_t start, end;
    uint32_t holds;
    int listed;
    uint64_t linked;        /* link order: larger is linked later */
} model_t;

static model_t s_model[MAX_NODES];
static int s_nmodel;
static uint64_t s_clock;
static int s_order[MAX_NODES];     /* listed nodes' model indices, in list order */
static int s_norder;

static uint64_t s_rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t n)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng % n);
}

/* The list, walked, against the model: the listed nodes in the model's
 * order, each with its range and holds and links, and the count. */
static void compare(maplist_t *l, const char *when)
{
    char what[160];
    int walked = 0;
    maplist_node_t *back = NULL;
    for (maplist_node_t *n = l->head; n != NULL && walked <= MAX_NODES; n = n->next) {
        snprintf(what, sizeof(what), "%s: list position %d is the model's", when, walked);
        check(walked < s_norder && s_model[s_order[walked]].n == n, what);
        if (walked >= s_norder || s_model[s_order[walked]].n != n)
            return;
        model_t *m = &s_model[s_order[walked]];
        snprintf(what, sizeof(what), "%s: a node keeps its range and holds", when);
        check(n->start == m->start && n->end == m->end && n->holds == m->holds && n->listed && n->linked == m->linked,
              what);
        snprintf(what, sizeof(what), "%s: each node's prev is the one before it", when);
        check(n->prev == back, what);
        back = n;
        walked++;
    }
    snprintf(what, sizeof(what), "%s: the list holds every listed node, and its count says so", when);
    check(walked == s_norder && l->count == (uint32_t)s_norder, what);
}

static int order_index(model_t *m)
{
    for (int i = 0; i < s_norder; i++)
        if (&s_model[s_order[i]] == m)
            return i;
    return -1;
}

static void order_remove(model_t *m)
{
    int i = order_index(m);
    if (i < 0)
        return;
    memmove(&s_order[i], &s_order[i + 1], (size_t)(s_norder - i - 1) * sizeof(int));
    s_norder--;
}

static void order_to_front(model_t *m)
{
    order_remove(m);
    memmove(&s_order[1], &s_order[0], (size_t)s_norder * sizeof(int));
    s_order[0] = (int)(m - s_model);
    s_norder++;
}

/* The model's answer to a lookup: the first holder in list order. A
 * by-address lookup must also be the holder linked last (checked by the
 * caller through newest_holder). */
static model_t *model_find(uintptr_t addr, int start_only)
{
    for (int i = 0; i < s_norder; i++) {
        model_t *m = &s_model[s_order[i]];
        if (start_only ? m->start == addr : (addr >= m->start && addr < m->end))
            return m;
    }
    return NULL;
}

static model_t *newest_holder(uintptr_t addr)
{
    model_t *best = NULL;
    for (int i = 0; i < s_norder; i++) {
        model_t *m = &s_model[s_order[i]];
        if (addr >= m->start && addr < m->end && (best == NULL || m->linked > best->linked))
            best = m;
    }
    return best;
}

/* A lookup by address found `m`: the core's rule applied to the model's
 * order. It moves to the head unless a node ahead overlaps it and was
 * linked after it. */
static model_t *found(model_t *m)
{
    if (m == NULL)
        return NULL;
    int at = order_index(m);
    for (int i = 0; i < at; i++) {
        model_t *x = &s_model[s_order[i]];
        if (x->linked > m->linked && x->start < m->end && m->start < x->end)
            return m;
    }
    order_to_front(m);
    return m;
}

static model_t *model_of(maplist_node_t *n)
{
    for (int i = 0; i < s_nmodel; i++)
        if (s_model[i].n == n && n != NULL)
            return &s_model[i];
    return NULL;
}

static void free_node(model_t *m)
{
    free(m->n);
    m->n = NULL;
}

static model_t *add(maplist_t *l, uintptr_t start, uintptr_t end)
{
    if (s_nmodel == MAX_NODES)
        return NULL;
    model_t *m = &s_model[s_nmodel++];
    m->n = malloc(sizeof(*m->n));
    memset(m->n, 0xA5, sizeof(*m->n));     /* the core sets every field it reads */
    m->start = start;
    m->end = end;
    m->holds = 0;
    m->listed = 1;
    m->linked = ++s_clock;
    maplist_link(l, m->n, start, end);
    order_to_front(m);
    return m;
}

static void unlink_one(maplist_t *l, model_t *m)
{
    int frees = maplist_unlink(l, m->n);
    int want = m->listed && m->holds == 0;
    check(frees == want, "unlink: says to free exactly when nothing holds it");
    if (m->listed)
        order_remove(m);
    m->listed = 0;
    if (frees)
        free_node(m);
}

static void release_one(model_t *m)
{
    int frees = maplist_release(m->n);
    int want = m->holds == 1 && !m->listed;
    if (m->holds > 0)
        m->holds--;
    check(frees == want, "release: says to free exactly at the last hold of a delisted node");
    if (frees)
        free_node(m);
}

static void bury(maplist_t *l)
{
    uint64_t held = 0, want_held = 0;
    for (int i = 0; i < s_nmodel; i++)
        if (s_model[i].n != NULL && s_model[i].listed)
            want_held += s_model[i].holds;
    maplist_node_t *chain = maplist_take_all(l, &held);
    check(held == want_held, "take all: counts the holds outstanding on the listed nodes");
    check(l->head == NULL && l->count == 0, "take all: leaves the list empty");
    int taken = 0;
    for (maplist_node_t *n = chain; n != NULL;) {
        maplist_node_t *next = n->next;
        model_t *m = NULL;
        for (int i = 0; i < s_nmodel; i++)
            if (s_model[i].n == n)
                m = &s_model[i];
        check(m != NULL && m->listed, "take all: hands back listed nodes only");
        if (m == NULL)
            break;
        check(taken < s_norder && &s_model[s_order[taken]] == m, "take all: the chain runs in the list's order");
        check(!n->listed, "take all: every node handed back is delisted");
        m->listed = 0;
        free_node(m);
        taken++;
        n = next;
    }
    for (int i = 0; i < s_nmodel; i++)
        check(s_model[i].n == NULL || !s_model[i].listed, "take all: no listed node is left behind");
    s_norder = 0;
    (void)taken;
}

static void reset(maplist_t *l)
{
    bury(l);
    for (int i = 0; i < s_nmodel; i++)
        if (s_model[i].n != NULL)
            free_node(&s_model[i]);     /* delisted and held: the test is their holder */
    s_nmodel = 0;
    s_norder = 0;
    s_clock = 0;    /* a fresh list numbers its links from 1 again */
    maplist_init(l);
}

static void cases(void)
{
    maplist_t l;
    maplist_init(&l);
    check(l.head == NULL && l.count == 0 && l.lookups == 0, "init: an empty list");
    check(maplist_find(&l, 0x1000) == NULL, "find: nothing in an empty list");

    /* Link at the head; a lookup by address moves what it finds there. */
    model_t *a = add(&l, 0x1000, 0x3000);
    model_t *b = add(&l, 0x5000, 0x6000);
    compare(&l, "two linked");
    check(l.head == b->n, "link: a fresh node is the head");
    check(maplist_find_start(&l, 0x1000) == a->n && maplist_find_start(&l, 0x1001) == NULL,
          "find start: an exact start only");
    check(l.head == b->n, "find start: what it finds stays where it is");
    compare(&l, "found by start");

    /* The walk is counted: a lookup of the older node steps over both, then
     * looks again at the one ahead of it before moving (may_lead), and the
     * next lookup of it steps over one. */
    uint64_t before = l.lookups, steps = l.steps;
    check(found(model_of(maplist_find(&l, 0x2fff))) == a, "find: the node whose range holds it");
    check(l.lookups == before + 1 && l.steps == steps + 3 && l.steps_most >= 3,
          "the walk: a lookup counted with the nodes it stepped over, the second look included");
    check(l.head == a->n && a->n->prev == NULL && b->n->prev == a->n && b->n->next == NULL,
          "move to front: a hit at the tail is the head, its links whole");
    compare(&l, "moved to the head");
    steps = l.steps;
    check(maplist_find(&l, 0x1000) == found(a)->n && l.steps == steps + 1, "move to front: found at once after");
    uint64_t misses = l.misses;
    steps = l.steps;
    check(maplist_find(&l, 0x3000) == NULL && maplist_find(&l, 0xfff) == NULL, "find: a range is [start, end)");
    check(l.steps == steps + 4 && l.misses == misses + 2, "a miss steps over every node, and is counted");
    check(l.head == a->n, "a miss moves nothing");
    compare(&l, "after misses");
    steps = l.steps;
    uint64_t most = l.steps_most;
    found(a);   /* the model's clock: a is head, finding it again keeps it there */
    maplist_find(&l, 0x1800);
    check(l.steps == steps + 1 && l.steps_most == most, "the walk: the longest walk is kept, not the last");
    check(maplist_find(&l, 0x5000) == found(b)->n && l.head == b->n, "move to front: the other node back to the head");
    compare(&l, "back again");

    /* A read: the walks since the list began, and since the last read,
     * whose window starts again. */
    {
        maplist_walks_t all, since;
        uint64_t lk = l.lookups, ms = l.misses, st = l.steps, mo = l.steps_most;
        maplist_read_walks(&l, &all, &since);
        check(all.lookups == lk && all.misses == ms && all.steps == st && all.steps_most == mo,
              "read: the walks since the list began");
        check(since.lookups == lk && since.misses == ms && since.steps == st && since.steps_most == 3,
              "read: the first read's window is the whole life");
        maplist_find(&l, 0x5000);
        maplist_find(&l, 0x9000);
        maplist_read_walks(&l, &all, &since);
        check(since.lookups == 2 && since.misses == 1 && since.steps == 3 && since.steps_most == 2 &&
              all.misses == ms + 1 && all.steps_most == 3,
              "read: the next read counts only what came after the last, its longest walk its own");
        maplist_read_walks(&l, &all, &since);
        check(since.lookups == 0 && since.misses == 0 && since.steps == 0 && since.steps_most == 0 &&
              all.lookups == lk + 2,
              "read: nothing since the last read reads as nothing");
    }

    /* Two segments sharing a boundary page: the one linked last governs it,
     * and a hit on the older one does not move it ahead of the newer. */
    {
        model_t *rx = add(&l, 0x400000, 0x403000);
        model_t *rw = add(&l, 0x402000, 0x404000);
        check(maplist_find(&l, 0x400000) == found(rx)->n && l.head == rw->n,
              "overlap: a hit on the older node stays behind the newer one it overlaps");
        compare(&l, "overlap kept");
        check(maplist_find(&l, 0x402800) == found(rw)->n, "overlap: the shared page is the newer node's");
        unlink_one(&l, rw);
        check(maplist_find(&l, 0x402800) == found(rx)->n && l.head == rx->n,
              "overlap: with the newer gone, the older governs and leads");
        compare(&l, "overlap gone");
        unlink_one(&l, rx);
    }

    /* Three nodes: a hit in the middle keeps both neighbours linked. */
    {
        model_t *m3 = add(&l, 0x60000, 0x61000);    /* m3, b, a */
        check(maplist_find(&l, 0x5000) == found(b)->n, "find: the middle node");
        check(l.head == b->n && b->n->next == m3->n && m3->n->prev == b->n && m3->n->next == a->n &&
              a->n->prev == m3->n, "move to front: a hit in the middle leaves its neighbours joined");
        compare(&l, "middle moved");
        unlink_one(&l, m3);
        compare(&l, "third unlinked");
    }

    /* A hold keeps a delisted node alive until it is given back. */
    check(maplist_find_hold(&l, 0x1800) == a->n && a->n->holds == 1, "find hold: a hold taken on what it found");
    a->holds = 1;
    check(maplist_find_hold(&l, 0x9000) == NULL, "find hold: nothing found, nothing held");
    unlink_one(&l, a);
    compare(&l, "a delisted while held");
    check(maplist_find(&l, 0x1800) == NULL, "unlink: a delisted node is never found again");
    check(maplist_find_start(&l, 0x1000) == NULL, "unlink: nor by its start");
    release_one(a);
    check(a->n == NULL, "release: the last hold of a delisted node frees it");

    /* An unlisted node unlinked again: nothing changes, nothing freed. */
    maplist_hold(b->n);
    b->holds = 1;
    unlink_one(&l, b);
    check(maplist_unlink(&l, b->n) == false && l.count == 0, "unlink: a second unlink changes nothing");
    release_one(b);

    /* A release with no hold is refused, so a double release cannot free twice. */
    model_t *c = add(&l, 0x8000, 0x9000);
    check(maplist_release(c->n) == false && c->n->holds == 0, "release: none to give back is refused");
    maplist_hold(c->n);
    maplist_hold(c->n);
    c->holds = 2;
    release_one(c);
    check(c->n != NULL, "release: a listed node is never freed by a release");
    unlink_one(&l, c);
    check(c->n != NULL, "unlink: a node still held is not freed");
    release_one(c);
    check(c->n == NULL, "release: the holds given back, the delisted node is freed");

    /* Middle, head and tail unlinks keep the links whole. */
    model_t *d = add(&l, 0x10000, 0x11000);
    model_t *e = add(&l, 0x20000, 0x21000);
    model_t *f = add(&l, 0x30000, 0x31000);
    unlink_one(&l, e);
    compare(&l, "middle unlinked");
    unlink_one(&l, f);
    compare(&l, "head unlinked");
    unlink_one(&l, d);
    compare(&l, "tail unlinked");

    /* Burial takes every node, whatever its holds. */
    model_t *g = add(&l, 0x40000, 0x41000);
    add(&l, 0x50000, 0x51000);
    maplist_hold(g->n);
    maplist_hold(g->n);
    g->holds = 2;
    bury(&l);
    compare(&l, "buried");

    reset(&l);
}

static void random_rounds(uint32_t rounds)
{
    maplist_t l;
    maplist_init(&l);
    for (uint32_t r = 0; r < rounds; r++) {
        uint32_t op = rnd(100);
        if (op < 25) {
            uintptr_t start = (uintptr_t)(rnd(64) + 1) * 0x1000;
            add(&l, start, start + (uintptr_t)(rnd(4) + 1) * 0x1000);
        } else if (op < 45) {
            uintptr_t addr = (uintptr_t)rnd(70 * 0x1000);
            model_t *want = model_find(addr, 0);
            check(want == newest_holder(addr), "random: the first holder in list order is the one linked last");
            uint64_t misses = l.misses;
            maplist_node_t *got = maplist_find_hold(&l, addr);
            check(got == (want ? want->n : NULL), "random: a lookup finds the holder linked last");
            check(l.misses == misses + (want == NULL), "random: a lookup that finds nothing is a miss");
            if (want != NULL) {
                want->holds++;
                found(want);
            }
        } else if (op < 52) {
            uintptr_t start = (uintptr_t)(rnd(64) + 1) * 0x1000;
            model_t *want = model_find(start, 1);
            check(maplist_find_start(&l, start) == (want ? want->n : NULL),
                  "random: a lookup by start finds the first listed node starting there");
        } else if (op < 72) {
            if (s_nmodel > 0) {
                model_t *m = &s_model[rnd((uint32_t)s_nmodel)];
                if (m->n != NULL && m->listed)
                    unlink_one(&l, m);
            }
        } else if (op < 97) {
            if (s_nmodel > 0) {
                model_t *m = &s_model[rnd((uint32_t)s_nmodel)];
                if (m->n != NULL && m->holds > 0)
                    release_one(m);
            }
        } else {
            reset(&l);
        }
        compare(&l, "random");
        if (s_nmodel == MAX_NODES)
            reset(&l);
        if (s_failed > 20)
            return;
    }
    reset(&l);
}

int main(int argc, char **argv)
{
    uint32_t rounds = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 20000;
    cases();
    random_rounds(rounds);
    printf("maplist: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
