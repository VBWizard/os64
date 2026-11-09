// scratch.c — the paint's scratch (scratch.h): a few slots, each a buffer
// lent to a paint or idle until the next asks for one its size.

#include "scratch.h"
#include <stdbool.h>
#include "os64/mem.h"
#include "os64/str.h"

#define PAGE 4096u

typedef struct {
    void *at;                       // NULL for an empty slot
    size_t cap;
    bool lent;
    uint64_t used;                  // the paint that last gave it back
} Slot;

struct yonder_scratch {
    Slot slot[YONDER_SCRATCH_SLOTS];
    uint64_t paint;                 // this paint's number; the first is 0
    yonder_scratch_counts_t counts;
};

yonder_scratch_t *yonder_scratch_new(void)
{
    yonder_scratch_t *s = os64_malloc(sizeof(*s));
    if (s != NULL)
        os64_memset(s, 0, sizeof(*s));
    return s;
}

static void slot_empty(yonder_scratch_t *s, Slot *sl)
{
    if (!sl->lent && sl->at != NULL) {
        s->counts.idle--;
        s->counts.idle_bytes -= sl->cap;
    }
    os64_free(sl->at);
    sl->at = NULL;
    sl->cap = 0;
    sl->lent = false;
}

void yonder_scratch_free(yonder_scratch_t *s)
{
    if (s == NULL)
        return;
    for (int i = 0; i < YONDER_SCRATCH_SLOTS; i++)
        os64_free(s->slot[i].at);
    os64_free(s);
}

static void *lend(yonder_scratch_t *s, Slot *sl)
{
    sl->lent = true;
    s->counts.lent++;
    return sl->at;
}

void *yonder_scratch_take(yonder_scratch_t *s, size_t bytes)
{
    // An eighth to spare, so a box that grows a little between paints
    // keeps its buffer. Past what may be kept, a buffer would be freed on
    // its way back, so pooling it saves nothing; testing `bytes` first is
    // what keeps the sum from overflowing.
    size_t cap = bytes <= YONDER_SCRATCH_KEEP ? (bytes + bytes / 8 + PAGE - 1) / PAGE * PAGE : 0;
    if (s == NULL || bytes < YONDER_SCRATCH_FLOOR || cap == 0 || cap > YONDER_SCRATCH_KEEP) {
        if (s != NULL)
            s->counts.unpooled++;
        return os64_malloc(bytes);
    }
    Slot *fit = NULL, *empty = NULL, *small = NULL;
    for (int i = 0; i < YONDER_SCRATCH_SLOTS; i++) {
        Slot *sl = &s->slot[i];
        if (sl->at == NULL) {
            if (empty == NULL)
                empty = sl;
        } else if (!sl->lent) {
            if (sl->cap >= bytes) {
                if (fit == NULL || sl->cap < fit->cap)
                    fit = sl;
            } else if (small == NULL || sl->cap < small->cap) {
                small = sl;
            }
        }
    }
    if (fit != NULL) {
        s->counts.idle--;
        s->counts.idle_bytes -= fit->cap;
        s->counts.reused++;
        return lend(s, fit);
    }
    // No idle buffer holds it: make one in an empty slot, or in place of
    // the smallest idle buffer, which is the least use to a later take.
    Slot *sl = empty != NULL ? empty : small;
    if (sl == NULL) {
        s->counts.unpooled++;
        return os64_malloc(bytes);   // every slot is lent
    }
    slot_empty(s, sl);
    sl->at = os64_malloc(cap);
    if (sl->at == NULL)
        return NULL;
    sl->cap = cap;
    s->counts.made++;
    return lend(s, sl);
}

void yonder_scratch_give(yonder_scratch_t *s, void *p)
{
    if (p == NULL)
        return;
    if (s != NULL)
        for (int i = 0; i < YONDER_SCRATCH_SLOTS; i++) {
            Slot *sl = &s->slot[i];
            if (sl->at != p || !sl->lent)
                continue;
            sl->lent = false;
            s->counts.lent--;
            if (s->counts.idle_bytes + sl->cap > YONDER_SCRATCH_KEEP) {
                os64_free(sl->at);
                sl->at = NULL;
                sl->cap = 0;
                return;
            }
            sl->used = s->paint;
            s->counts.idle++;
            s->counts.idle_bytes += sl->cap;
            return;
        }
    os64_free(p);
}

void yonder_scratch_paint_end(yonder_scratch_t *s)
{
    if (s == NULL)
        return;
    s->paint++;
    for (int i = 0; i < YONDER_SCRATCH_SLOTS; i++) {
        Slot *sl = &s->slot[i];
        if (sl->at != NULL && !sl->lent && s->paint - sl->used > YONDER_SCRATCH_IDLE_PAINTS)
            slot_empty(s, sl);
    }
}

void yonder_scratch_drop(yonder_scratch_t *s)
{
    if (s == NULL)
        return;
    for (int i = 0; i < YONDER_SCRATCH_SLOTS; i++)
        if (s->slot[i].at != NULL && !s->slot[i].lent)
            slot_empty(s, &s->slot[i]);
}

yonder_scratch_counts_t yonder_scratch_counts(const yonder_scratch_t *s)
{
    yonder_scratch_counts_t none = {0};
    return s != NULL ? s->counts : none;
}
