// The damage list's rule (shared/damage.c, os64/damage.h), on the host under
// the sanitizers, three ways:
//
//   - against the compositor's own code as it stood before the rule was
//     shared (reference_add: gui_damage_add_locked from
//     kernel/src/gui/compositor.c at eb32340a, word for word, with its two
//     constants made parameters), on random sequences of marks, list by
//     list and answer by answer — the move changes nothing;
//   - against a pixel oracle: every pixel any mark covered is covered by
//     the list, and the list never holds more than its capacity;
//   - and by name, one case per rule.
#include "os64/damage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef os64_gui_rect_t rect_t;

static int failures, checks;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

// ── The reference: the kernel's helpers (gui/gui_types.h) and
// gui_damage_add_locked, as they were. ──────────────────────────────────────

static inline bool rect_is_empty(rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

static inline rect_t rect_union(rect_t a, rect_t b)
{
    if (rect_is_empty(a))
        return b;
    if (rect_is_empty(b))
        return a;

    int32_t x1 = a.x < b.x ? a.x : b.x;
    int32_t y1 = a.y < b.y ? a.y : b.y;
    int32_t x2 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y2 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (rect_t){x1, y1, x2 - x1, y2 - y1};
}

static inline int64_t rect_area(rect_t r)
{
    return rect_is_empty(r) ? 0 : (int64_t)r.w * (int64_t)r.h;
}

static uint32_t DAMAGE_MAX_RECTS;
static int64_t DAMAGE_MERGE_SLACK;
static rect_t kPendingDamage[16];
static uint32_t kPendingDamageCount;
static struct { uint64_t collapses; } s_composite_counts;

static inline bool damage_merge_is_cheap(rect_t a, rect_t b)
{
    return rect_area(rect_union(a, b)) <= rect_area(a) + rect_area(b) + DAMAGE_MERGE_SLACK;
}

static void reference_add(rect_t screen_rect)
{
    if (rect_is_empty(screen_rect))
        return;

    for (uint32_t i = 0; i < kPendingDamageCount; i++) {
        if (damage_merge_is_cheap(kPendingDamage[i], screen_rect)) {
            kPendingDamage[i] = rect_union(kPendingDamage[i], screen_rect);
            return;
        }
    }

    if (kPendingDamageCount < DAMAGE_MAX_RECTS) {
        kPendingDamage[kPendingDamageCount++] = screen_rect;
        return;
    }

    s_composite_counts.collapses++;
    rect_t all = screen_rect;
    for (uint32_t i = 0; i < kPendingDamageCount; i++)
        all = rect_union(all, kPendingDamage[i]);
    kPendingDamage[0] = all;
    kPendingDamageCount = 1;
}

// ── Random marks ────────────────────────────────────────────────────────────

static uint64_t s_seed = 0x2545F4914F6CDD1Dull;

static uint32_t next(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return (uint32_t)s_seed;
}

static int32_t between(int32_t lo, int32_t hi)
{
    return lo + (int32_t)(next() % (uint32_t)(hi - lo + 1));
}

// Mostly small marks anywhere on and off a 400x400 grid, some empty, some
// wide.
static rect_t random_mark(void)
{
    rect_t r = {between(-40, 380), between(-40, 380), between(-4, 60), between(-4, 60)};
    if (next() % 8 == 0)
        r.w = between(100, 400);
    return r;
}

// The pixel oracle's grid: [-40, 840) both ways holds every mark.
enum { GRID_LO = -40, GRID = 880 };
static uint8_t s_marked[GRID][GRID];

static void mark_pixels(rect_t r)
{
    if (rect_is_empty(r))
        return;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++)
            s_marked[y - GRID_LO][x - GRID_LO] = 1;
}

static bool covered(const rect_t *list, uint32_t n, int32_t x, int32_t y)
{
    for (uint32_t i = 0; i < n; i++)
        if (x >= list[i].x && x < list[i].x + list[i].w && y >= list[i].y && y < list[i].y + list[i].h)
            return true;
    return false;
}

static void differential_cases(void)
{
    static const int64_t kSlacks[] = {0, 1, 128, 1000, 1 << 20};
    int mismatches = 0, uncovered = 0, over_cap = 0;
    for (int trial = 0; trial < 20000; trial++) {
        uint32_t cap = 1 + next() % 16;
        int64_t slack = kSlacks[next() % 5];
        DAMAGE_MAX_RECTS = cap;
        DAMAGE_MERGE_SLACK = slack;
        kPendingDamageCount = 0;
        s_composite_counts.collapses = 0;
        rect_t list[16];
        uint32_t count = 0;
        bool oracle = trial % 40 == 0;
        if (oracle)
            memset(s_marked, 0, sizeof(s_marked));
        int marks = 1 + (int)(next() % 40);
        for (int m = 0; m < marks; m++) {
            rect_t r = random_mark();
            uint64_t before = s_composite_counts.collapses;
            reference_add(r);
            bool collapsed = os64_damage_add(list, &count, cap, slack, r);
            if (collapsed != (s_composite_counts.collapses != before) || count != kPendingDamageCount ||
                memcmp(list, kPendingDamage, count * sizeof(rect_t)) != 0)
                mismatches++;
            if (count > cap)
                over_cap++;
            if (oracle)
                mark_pixels(r);
        }
        if (oracle)
            for (int y = 0; y < GRID; y++)
                for (int x = 0; x < GRID; x++)
                    if (s_marked[y][x] && !covered(list, count, x + GRID_LO, y + GRID_LO))
                        uncovered++;
    }
    check(mismatches == 0, "random marks: the shared rule answers exactly as the compositor's own did");
    check(uncovered == 0, "random marks: every pixel a mark covered, the list covers");
    check(over_cap == 0, "random marks: the list never holds more than its capacity");
}

static bool same(rect_t a, rect_t b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static void named_cases(void)
{
    rect_t l[4];
    uint32_t n = 0;

    os64_damage_add(l, &n, 4, 0, (rect_t){0, 0, 20, 20});
    bool c = os64_damage_add(l, &n, 4, 0, (rect_t){5, 5, 10, 10});
    check(!c && n == 1 && same(l[0], (rect_t){0, 0, 20, 20}), "a mark inside an entry merges, even with no slack");

    n = 0;
    os64_damage_add(l, &n, 4, 0, (rect_t){0, 0, 10, 10});
    os64_damage_add(l, &n, 4, 0, (rect_t){5, 5, 10, 10});
    check(n == 2, "a diagonal overlap stays apart with no slack: its union wastes 25 pixels");
    n = 0;
    os64_damage_add(l, &n, 4, 25, (rect_t){0, 0, 10, 10});
    os64_damage_add(l, &n, 4, 25, (rect_t){5, 5, 10, 10});
    check(n == 1 && same(l[0], (rect_t){0, 0, 15, 15}), "and merges once the slack covers that waste");

    n = 0;
    os64_damage_add(l, &n, 4, 0, (rect_t){0, 0, 10, 10});
    os64_damage_add(l, &n, 4, 0, (rect_t){10, 0, 10, 10});
    check(n == 1 && same(l[0], (rect_t){0, 0, 20, 10}), "touching marks merge: their union wastes nothing");

    n = 0;
    os64_damage_add(l, &n, 4, 128, (rect_t){0, 0, 10, 10});
    os64_damage_add(l, &n, 4, 128, (rect_t){100, 100, 10, 10});
    check(n == 2, "marks far apart stay apart");

    n = 0;
    os64_damage_add(l, &n, 4, 128, (rect_t){0, 0, 10, 10});
    os64_damage_add(l, &n, 4, 128, (rect_t){12, 0, 10, 10});
    check(n == 1 && same(l[0], (rect_t){0, 0, 22, 10}), "a gap the slack covers is merged");

    n = 0;
    os64_damage_add(l, &n, 4, 0, (rect_t){0, 0, 10, 10});
    os64_damage_add(l, &n, 4, 0, (rect_t){0, 20, 10, 10});
    c = os64_damage_add(l, &n, 4, 1 << 20, (rect_t){0, 10, 10, 10});
    check(!c && n == 2 && same(l[0], (rect_t){0, 0, 10, 20}) && same(l[1], (rect_t){0, 20, 10, 10}),
          "a mark cheap with two entries merges into the first");

    n = 0;
    c = os64_damage_add(l, &n, 4, 0, (rect_t){5, 5, 0, 10}) || os64_damage_add(l, &n, 4, 0, (rect_t){5, 5, 10, -1});
    check(!c && n == 0, "an empty mark changes nothing");

    n = 0;
    os64_damage_add(l, &n, 2, 0, (rect_t){0, 0, 10, 10});
    os64_damage_add(l, &n, 2, 0, (rect_t){100, 0, 10, 10});
    c = os64_damage_add(l, &n, 2, 0, (rect_t){0, 100, 10, 10});
    check(c && n == 1 && same(l[0], (rect_t){0, 0, 110, 110}),
          "a full list that nothing merges into collapses to the union of all, and says so");

    n = 0;
    os64_damage_add(l, &n, 1, 0, (rect_t){0, 0, 10, 10});
    c = os64_damage_add(l, &n, 1, 0, (rect_t){50, 50, 10, 10});
    check(c && n == 1 && same(l[0], (rect_t){0, 0, 60, 60}), "a list of one holds one: the union");
}

int main(void)
{
    named_cases();
    differential_cases();
    printf("damage list: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
