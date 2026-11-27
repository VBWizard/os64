/* The mapping list's core (kernel/src/memory/maplist.c) beside a model.
 *
 * Nodes are malloc'd and freed exactly when a call says the caller frees,
 * so under ASan a free with a hold outstanding, or a node still reachable
 * from the list after its free, is a use-after-free report, and a node the
 * core never says to free is a leak. The model keeps each node's range,
 * holds and listed flag, and the list's order (newest first); after every
 * step the list is walked and compared with it.
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
    uint64_t born;          /* link order: larger is newer */
} model_t;

static model_t s_model[MAX_NODES];
static int s_nmodel;
static uint64_t s_clock;

static uint64_t s_rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t n)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng % n);
}

/* The list, walked, against the model: the listed nodes, newest first,
 * each with its range and holds, and the count. */
static void compare(maplist_t *l, const char *when)
{
    char what[160];
    uint64_t prev_born = UINT64_MAX;
    uint32_t walked = 0;
    maplist_node_t *back = NULL;
    for (maplist_node_t *n = l->head; n != NULL && walked <= MAX_NODES; n = n->next) {
        walked++;
        model_t *m = NULL;
        for (int i = 0; i < s_nmodel; i++)
            if (s_model[i].n == n)
                m = &s_model[i];
        snprintf(what, sizeof(what), "%s: every node on the list is one the model has listed", when);
        check(m != NULL && m->listed, what);
        if (m == NULL)
            return;
        snprintf(what, sizeof(what), "%s: the list runs newest first", when);
        check(m->born < prev_born, what);
        prev_born = m->born;
        snprintf(what, sizeof(what), "%s: a node keeps its range and holds", when);
        check(n->start == m->start && n->end == m->end && n->holds == m->holds && n->listed, what);
        snprintf(what, sizeof(what), "%s: each node's prev is the one before it", when);
        check(n->prev == back, what);
        back = n;
    }
    uint32_t listed = 0;
    for (int i = 0; i < s_nmodel; i++)
        listed += s_model[i].n != NULL && s_model[i].listed;
    snprintf(what, sizeof(what), "%s: the list holds every listed node, and its count says so", when);
    check(walked == listed && l->count == listed, what);
}

/* The model's answer to a lookup: the newest listed node holding `addr`. */
static model_t *model_find(uintptr_t addr, int start_only)
{
    model_t *best = NULL;
    for (int i = 0; i < s_nmodel; i++) {
        model_t *m = &s_model[i];
        if (m->n == NULL || !m->listed)
            continue;
        int hit = start_only ? m->start == addr : (addr >= m->start && addr < m->end);
        if (hit && (best == NULL || m->born > best->born))
            best = m;
    }
    return best;
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
    m->born = ++s_clock;
    maplist_link(l, m->n, start, end);
    return m;
}

static void unlink_one(maplist_t *l, model_t *m)
{
    int frees = maplist_unlink(l, m->n);
    int want = m->listed && m->holds == 0;
    check(frees == want, "unlink: says to free exactly when nothing holds it");
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
    uint64_t prev_born = UINT64_MAX;
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
        check(m->born < prev_born, "take all: the chain runs newest first");
        check(!n->listed, "take all: every node handed back is delisted");
        prev_born = m->born;
        m->listed = 0;
        free_node(m);
        taken++;
        n = next;
    }
    for (int i = 0; i < s_nmodel; i++)
        check(s_model[i].n == NULL || !s_model[i].listed, "take all: no listed node is left behind");
    (void)taken;
}

static void reset(maplist_t *l)
{
    bury(l);
    for (int i = 0; i < s_nmodel; i++)
        if (s_model[i].n != NULL)
            free_node(&s_model[i]);     /* delisted and held: the test is their holder */
    s_nmodel = 0;
    maplist_init(l);
}

static void cases(void)
{
    maplist_t l;
    maplist_init(&l);
    check(l.head == NULL && l.count == 0 && l.lookups == 0, "init: an empty list");
    check(maplist_find(&l, 0x1000) == NULL, "find: nothing in an empty list");

    /* Link at the head; find newest first. */
    model_t *a = add(&l, 0x1000, 0x3000);
    model_t *b = add(&l, 0x5000, 0x6000);
    compare(&l, "two linked");
    check(l.head == b->n, "link: a fresh node is the head");
    check(maplist_find(&l, 0x2fff) == a->n && maplist_find(&l, 0x3000) == NULL &&
          maplist_find(&l, 0x1000) == a->n && maplist_find(&l, 0xfff) == NULL,
          "find: a range is [start, end)");
    check(maplist_find_start(&l, 0x5000) == b->n && maplist_find_start(&l, 0x5001) == NULL,
          "find start: an exact start only");

    /* The walk is counted: a lookup of the older node steps over both. */
    uint64_t before = l.lookups, steps = l.steps;
    maplist_find(&l, 0x1000);
    check(l.lookups == before + 1 && l.steps == steps + 2 && l.steps_most >= 2,
          "the walk: a lookup counted with the nodes it stepped over");
    steps = l.steps;
    maplist_find(&l, 0x9000);
    check(l.steps == steps + 2, "the walk: a lookup that finds nothing steps over every node");
    steps = l.steps;
    uint64_t most = l.steps_most;
    maplist_find(&l, 0x5000);
    check(l.steps == steps + 1 && l.steps_most == most, "the walk: the longest walk is kept, not the last");

    /* A read: the walks since the list began, and since the last read,
     * whose window starts again. */
    {
        maplist_walks_t all, since;
        uint64_t lk = l.lookups, st = l.steps, mo = l.steps_most;
        maplist_read_walks(&l, &all, &since);
        check(all.lookups == lk && all.steps == st && all.steps_most == mo,
              "read: the walks since the list began");
        check(since.lookups == lk && since.steps == st && since.steps_most == 2,
              "read: the first read's window is the whole life");
        maplist_find(&l, 0x5000);
        maplist_read_walks(&l, &all, &since);
        check(since.lookups == 1 && since.steps == 1 && since.steps_most == 1 && all.steps_most == 2,
              "read: the next read counts only what came after the last, its longest walk its own");
        maplist_read_walks(&l, &all, &since);
        check(since.lookups == 0 && since.steps == 0 && since.steps_most == 0 && all.lookups == lk + 1,
              "read: nothing since the last read reads as nothing");
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
            maplist_node_t *got = maplist_find_hold(&l, addr);
            check(got == (want ? want->n : NULL), "random: a lookup finds the newest listed node holding it");
            if (want != NULL)
                want->holds++;
        } else if (op < 52) {
            uintptr_t start = (uintptr_t)(rnd(64) + 1) * 0x1000;
            model_t *want = model_find(start, 1);
            check(maplist_find_start(&l, start) == (want ? want->n : NULL),
                  "random: a lookup by start finds the newest listed node starting there");
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
