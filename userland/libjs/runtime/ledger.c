/* ledger.c — the JS speed ledger's arithmetic (ledger.h says what each
 * frame kind holds and why the totals add up). */
#include "ledger.h"

static void clear(void *p, size_t n)
{
    unsigned char *b = p;
    while (n--)
        *b++ = 0;
}

static void copy_place(ledger_place_t *to, const ledger_place_t *from)
{
    if (from == NULL) {
        clear(to, sizeof(*to));
        return;
    }
    *to = *from;
    to->name[LEDGER_NAME - 1] = '\0';
    to->file[LEDGER_FILE - 1] = '\0';
}

static int callee_row(ledger_t *l, const void *function, int magic);

void ledger_reset(ledger_t *l, uint64_t now)
{
    /* Open frames survive a reset with their clocks restarted, so a window
     * begun inside a task charges only the task's time after it. */
    unsigned depth = l->depth;
    uint64_t resets = l->resets + 1;
    ledger_frame_t stack[LEDGER_DEPTH];
    unsigned kept = depth < LEDGER_DEPTH ? depth : LEDGER_DEPTH;
    for (unsigned i = 0; i < kept; i++)
        stack[i] = l->stack[i];
    clear(l, sizeof(*l));
    l->depth = depth;
    l->resets = resets;
    for (unsigned i = 0; i < kept; i++) {
        l->stack[i] = stack[i];
        l->stack[i].start = now;
        l->stack[i].children = 0;
        if (l->stack[i].kind == LEDGER_NATIVE) {
            l->stack[i].callee = callee_row(l, l->stack[i].function, l->stack[i].magic);
            if (l->stack[i].callee < 0)
                l->callees_lost++;
        }
    }
}

/* The callee row for (function, magic), made if absent; -1 when full. */
static int callee_row(ledger_t *l, const void *function, int magic)
{
    uint64_t h = ((uint64_t)(uintptr_t)function ^ (uint64_t)(unsigned)magic * 0x9E3779B97F4A7C15ull)
                 * 0x9E3779B97F4A7C15ull;
    for (unsigned probe = 0; probe < LEDGER_CALLEES; probe++) {
        unsigned i = (unsigned)((h >> 32) + probe) % LEDGER_CALLEES;
        ledger_callee_t *c = &l->callees[i];
        if (!c->used) {
            c->used = true;
            c->function = function;
            c->magic = magic;
            return (int)i;
        }
        if (c->function == function && c->magic == magic)
            return (int)i;
    }
    return -1;
}

void ledger_push(ledger_t *l, ledger_kind_t kind, uint64_t now, const void *function, int magic)
{
    if (l->depth >= LEDGER_DEPTH) {
        l->depth++;
        l->frames_lost++;
        return;
    }
    ledger_frame_t *f = &l->stack[l->depth++];
    f->kind = kind < LEDGER_KINDS ? kind : LEDGER_ENGINE;
    f->start = now;
    f->children = 0;
    f->callee = -1;
    f->function = function;
    f->magic = magic;
    if (f->kind == LEDGER_NATIVE) {
        f->callee = callee_row(l, function, magic);
        if (f->callee < 0)
            l->callees_lost++;
    }
}

void ledger_pop(ledger_t *l, uint64_t now)
{
    if (l->depth == 0)
        return;
    if (l->depth > LEDGER_DEPTH) {
        l->depth--;
        return;
    }
    ledger_frame_t *f = &l->stack[--l->depth];
    uint64_t elapsed = now > f->start ? now - f->start : 0;
    uint64_t own = elapsed > f->children ? elapsed - f->children : 0;
    l->self[f->kind] += own;
    l->count[f->kind]++;
    if (f->kind == LEDGER_NATIVE && f->callee >= 0) {
        l->callees[f->callee].calls++;
        l->callees[f->callee].self += own;
    }
    if (f->kind == LEDGER_LAYOUT) {
        for (unsigned i = l->depth; i-- > 0;)
            if (l->stack[i].kind == LEDGER_NATIVE) {
                if (l->stack[i].callee >= 0) {
                    l->callees[l->stack[i].callee].forced++;
                    l->callees[l->stack[i].callee].forced_ticks += elapsed;
                }
                break;
            }
    }
    if (l->depth > 0)
        l->stack[l->depth - 1].children += elapsed;
}

uint64_t ledger_engine_live(const ledger_t *l, uint64_t now)
{
    uint64_t total = l->self[LEDGER_ENGINE];
    unsigned open = l->depth < LEDGER_DEPTH ? l->depth : LEDGER_DEPTH;
    for (unsigned i = 0; i < open; i++) {
        const ledger_frame_t *f = &l->stack[i];
        if (f->kind != LEDGER_ENGINE)
            continue;
        /* An open frame's own time so far: up to its open child's start,
         * or now for the innermost, less its closed children. */
        uint64_t end = i + 1 < open ? l->stack[i + 1].start : now;
        uint64_t elapsed = end > f->start ? end - f->start : 0;
        total += elapsed > f->children ? elapsed - f->children : 0;
    }
    return total;
}

static bool same_text(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* A freed function's address can be reused by another, so the pointer
 * alone would charge the new function's samples to the old one's row. A
 * row is the pointer and the definition it reports: a reused address with
 * the same name and place is the same source evaluated again, and merges. */
static bool same_function(const ledger_place_t *a, const ledger_place_t *b)
{
    return a->function == b->function && a->line == b->line && a->column == b->column
           && same_text(a->name, b->name) && same_text(a->file, b->file);
}

bool ledger_sample(ledger_t *l, uint64_t now, const ledger_place_t *at, const ledger_place_t *caller)
{
    uint64_t live = ledger_engine_live(l, now);
    uint64_t charge = live > l->engine_marked ? live - l->engine_marked : 0;
    l->engine_marked = live;
    l->samples++;
    ledger_place_t here, from;
    copy_place(&here, at);
    copy_place(&from, caller);
    uint64_t h = ((uint64_t)(uintptr_t)here.function * 31u + (uint64_t)(uintptr_t)from.function)
                 * 0x9E3779B97F4A7C15ull;
    for (unsigned probe = 0; probe < LEDGER_SAMPLES; probe++) {
        ledger_sample_t *s = &l->profile[(unsigned)((h >> 32) + probe) % LEDGER_SAMPLES];
        if (!s->used) {
            s->used = true;
            s->at = here;
            s->caller = from;
        } else if (!same_function(&s->at, &here) || !same_function(&s->caller, &from)) {
            continue;
        }
        s->samples++;
        s->ticks += charge;
        return true;
    }
    l->samples_lost++;
    return false;
}
