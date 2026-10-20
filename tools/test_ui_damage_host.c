/* test_ui_damage_host.c — libui's dirty list on the host (UI_DAMAGE.md):
 * the real ui.c, the real panel, the real shared rule, on a canvas in
 * memory. What it holds libui to:
 *
 *   - marks far apart stay apart, and a render paints only what they mark:
 *     a canary on every pixel outside them survives;
 *   - a mark that holds entries swallows them (a root mark is one rectangle);
 *   - a full list collapses into one rectangle, counted;
 *   - a paints_part class is painted once for each rectangle it meets, cut to
 *     it; any other class once, with the box around the ones it meets; and a
 *     parent is painted for all of them before any child is;
 *   - the panel fills and outlines only the rectangle it is given;
 *   - clearing any_dirty by hand clears the list;
 *   - os64_ui_render hands back the box, os64_ui_dirty_covers asks of one
 *     rectangle.
 *
 * Publishing is a guest syscall, so the render is driven through
 * os64_ui_render_list, which is what os64_ui_paint publishes from. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/ui.h"
#include "os64/draw.h"
#include "fake_backend.h"

static int failures;
static unsigned long checks;
static const char *current = "";
#define CHECK(x) do { if (!(x)) { \
        printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, current, #x); \
        ++failures; } else ++checks; } while (0)

/* libui's heap and engine, as the other libui host suites stand them in. */
void *os64_malloc(size_t n) { return malloc(n ? n : 1); }
void os64_free(void *p) { free(p); }
const os64_font_backend_t *os64_freetype_backend_v1(void) { return os64_fake_font_backend(); }
/* ui_text.c's clipboard; nothing here copies or pastes. */
int64_t os64_open(const char *path, const char *mode) { (void)path; (void)mode; return -1; }
int64_t os64_read(int32_t fd, void *p, size_t n) { (void)fd; (void)p; (void)n; return -1; }
int64_t os64_write(int32_t fd, const void *p, size_t n) { (void)fd; (void)p; (void)n; return -1; }
int64_t os64_close(int32_t fd) { (void)fd; return -1; }

/* ── the canvas ─────────────────────────────────────────────────────────── */

#define W 320
#define H 100
#define CANARY 0xff123456u
#define PANEL_BG 0xff202020u
#define PANEL_BORDER 0xffe0e0e0u
#define INK 0xff00ff00u

static uint32_t px[W * H];
static os64_draw_ctx_t dc;
static os64_ui_t ui;

static void canary(void)
{
    for (size_t i = 0; i < (size_t)W * H; i++)
        px[i] = CANARY;
}

static uint32_t at(int32_t x, int32_t y) { return px[(size_t)y * W + (size_t)x]; }

static bool inside(os64_gui_rect_t r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* Pixels outside every rectangle of the list that are not the canary. */
static int litter(const os64_gui_rect_t *list, uint32_t count)
{
    int n = 0;
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            bool in = false;
            for (uint32_t i = 0; i < count && !in; i++)
                in = inside(list[i], x, y);
            if (!in && at(x, y) != CANARY)
                n++;
        }
    return n;
}

static bool same(os64_gui_rect_t a, os64_gui_rect_t b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* ── widgets that say how they were painted ─────────────────────────────── */

/* Every paint, in order: which widget, and what `dirty` was. */
typedef struct { const os64_ui_widget_t *w; os64_gui_rect_t dirty; } paint_t;
static paint_t paints[64];
static int npaints;

static void record(os64_ui_widget_t *w)
{
    if (npaints < 64)
        paints[npaints++] = (paint_t){w, w->ui->dirty};
}

static int paints_of(const os64_ui_widget_t *w)
{
    int n = 0;
    for (int i = 0; i < npaints; i++)
        n += paints[i].w == w;
    return n;
}

static int first_paint(const os64_ui_widget_t *w)
{
    for (int i = 0; i < npaints; i++)
        if (paints[i].w == w)
            return i;
    return -1;
}

static int last_paint(const os64_ui_widget_t *w)
{
    int last = -1;
    for (int i = 0; i < npaints; i++)
        if (paints[i].w == w)
            last = i;
    return last;
}

/* Paints its whole bounds, as most classes do. */
static void whole_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    (void)t;
    record(w);
    os64_draw_fill_rect(&ctx->surf, w->bounds, INK);
}

/* Paints what `dirty` covers of it. */
static void part_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    (void)t;
    record(w);
    os64_gui_rect_t part;
    if (os64_rect_intersect(w->bounds, w->ui->dirty, &part))
        os64_draw_fill_rect(&ctx->surf, part, INK);
}

static const os64_ui_class_t kWhole = {.name = "whole", .paint = whole_paint};
static const os64_ui_class_t kPart = {.name = "part", .paint = part_paint, .paints_part = true};
static const os64_ui_class_t kBare = {.name = "bare"};   /* marks, paints nothing */

static os64_ui_widget_t root, kids[12];

static void widget(os64_ui_widget_t *w, const os64_ui_class_t *cls, os64_gui_rect_t r)
{
    memset(w, 0, sizeof(*w));
    w->cls = cls;
    w->bounds = r;
}

/* A window: a panel root over the canvas, and the children named. */
static void window(int nkids)
{
    memset(&ui, 0, sizeof(ui));
    ui.theme = (os64_ui_theme_t){.pad = 6, .button_h = 20, .gap = 4,
                                 .panel_bg = PANEL_BG, .panel_border = PANEL_BORDER};
    dc.surf = (os64_gui_surface_t){.pixels = px, .width = W, .height = H, .pitch_px = W};
    ui.ctx = &dc;
    os64_ui_panel(&root);
    root.bounds = (os64_gui_rect_t){0, 0, W, H};
    for (int i = 0; i < nkids; i++)
        os64_ui_add_child(&root, &kids[i]);
    os64_ui_set_root(&ui, &root);
    os64_gui_rect_t list[OS64_UI_DAMAGE_MAX];
    (void)os64_ui_render_list(&ui, list);   /* the first frame, set_root's */
    npaints = 0;
    canary();
}

/* ── the cases ──────────────────────────────────────────────────────────── */

static void far_apart(void)
{
    current = "marks far apart";
    widget(&kids[0], &kWhole, (os64_gui_rect_t){10, 10, 10, 10});
    widget(&kids[1], &kWhole, (os64_gui_rect_t){290, 70, 12, 12});
    widget(&kids[2], &kWhole, (os64_gui_rect_t){150, 40, 20, 20});
    window(3);
    os64_ui_mark_dirty(&ui, &kids[0]);
    os64_ui_mark_dirty(&ui, &kids[1]);
    CHECK(ui.any_dirty && ui.damage_count == 2);
    CHECK(same(ui.dirty, (os64_gui_rect_t){10, 10, 292, 72}));   /* the box */
    os64_gui_rect_t list[OS64_UI_DAMAGE_MAX];
    uint32_t n = os64_ui_render_list(&ui, list);
    CHECK(n == 2 && same(list[0], kids[0].bounds) && same(list[1], kids[1].bounds));
    CHECK(paints_of(&kids[0]) == 1 && paints_of(&kids[1]) == 1);
    CHECK(paints_of(&kids[2]) == 0);                      /* between them: untouched */
    CHECK(litter(list, n) == 0);                          /* the panel too */
    CHECK(at(15, 15) == INK && at(295, 75) == INK && at(160, 50) == CANARY);
    CHECK(!ui.any_dirty && ui.damage_count == 0);
    CHECK(os64_ui_render_list(&ui, list) == 0);           /* clean: nothing */
}

static void root_swallows(void)
{
    current = "a mark that holds entries";
    widget(&kids[0], &kWhole, (os64_gui_rect_t){10, 10, 10, 10});
    widget(&kids[1], &kWhole, (os64_gui_rect_t){290, 70, 12, 12});
    window(2);
    os64_ui_mark_dirty(&ui, &kids[0]);
    os64_ui_mark_dirty(&ui, &kids[1]);
    os64_ui_mark_dirty(&ui, &root);
    CHECK(ui.damage_count == 1 && same(ui.damage[0], root.bounds));
    CHECK(os64_ui_dirty_covers(&ui, root.bounds));
    os64_gui_rect_t list[OS64_UI_DAMAGE_MAX];
    CHECK(os64_ui_render_list(&ui, list) == 1);
    CHECK(paints_of(&kids[0]) == 1 && paints_of(&kids[1]) == 1 && paints_of(&root) == 0);
    CHECK(at(100, 50) == PANEL_BG && at(0, 50) == PANEL_BORDER && at(W - 1, 0) == PANEL_BORDER);
}

static void collapse(void)
{
    current = "a full list";
    for (int i = 0; i < 9; i++)
        widget(&kids[i], &kWhole, (os64_gui_rect_t){4 + i * 34, (i % 2) * 60 + 4, 6, 6});
    window(9);
    for (int i = 0; i < OS64_UI_DAMAGE_MAX; i++)
        os64_ui_mark_dirty(&ui, &kids[i]);
    CHECK(ui.damage_count == OS64_UI_DAMAGE_MAX && ui.damage_collapses == 0);
    os64_ui_mark_dirty(&ui, &kids[8]);
    CHECK(ui.damage_count == 1 && ui.damage_collapses == 1);
    CHECK(same(ui.damage[0], (os64_gui_rect_t){4, 4, 278, 66}));
    os64_gui_rect_t list[OS64_UI_DAMAGE_MAX];
    CHECK(os64_ui_render_list(&ui, list) == 1);
    for (int i = 0; i < 9; i++)
        CHECK(paints_of(&kids[i]) == 1);
}

static void per_rectangle(void)
{
    current = "paints_part, once a rectangle";
    /* A part view and a whole widget, each across the window, each with a
     * child; two marks far apart, both inside both. */
    widget(&kids[0], &kPart, (os64_gui_rect_t){0, 0, W, 50});
    widget(&kids[1], &kWhole, (os64_gui_rect_t){0, 50, W, 50});
    widget(&kids[2], &kWhole, (os64_gui_rect_t){5, 5, 10, 10});      /* the view's child */
    widget(&kids[3], &kWhole, (os64_gui_rect_t){300, 85, 10, 10});   /* the whole one's */
    widget(&kids[4], &kBare, (os64_gui_rect_t){4, 4, 12, 50});      /* a mark across both */
    widget(&kids[5], &kBare, (os64_gui_rect_t){298, 30, 14, 66});
    window(2);
    os64_ui_add_child(&kids[0], &kids[2]);
    os64_ui_add_child(&kids[1], &kids[3]);
    os64_ui_mark_dirty(&ui, &kids[4]);
    os64_ui_mark_dirty(&ui, &kids[5]);
    CHECK(ui.damage_count == 2);
    os64_gui_rect_t list[OS64_UI_DAMAGE_MAX];
    uint32_t n = os64_ui_render_list(&ui, list);
    CHECK(n == 2);
    /* The part view: once for each rectangle, `dirty` that rectangle. */
    CHECK(paints_of(&kids[0]) == 2);
    int a = first_paint(&kids[0]), b = last_paint(&kids[0]);
    CHECK(a >= 0 && same(paints[a].dirty, kids[4].bounds) && same(paints[b].dirty, kids[5].bounds));
    /* The whole one: once, `dirty` the box around both. */
    CHECK(paints_of(&kids[1]) == 1);
    CHECK(same(paints[first_paint(&kids[1])].dirty, (os64_gui_rect_t){4, 4, 308, 92}));
    /* A parent for every rectangle before any child of it. */
    CHECK(b < first_paint(&kids[2]) && first_paint(&kids[1]) < first_paint(&kids[3]));
    CHECK(paints_of(&kids[2]) == 1 && paints_of(&kids[3]) == 1);
    /* The part view painted only the rectangles: its band between them,
     * and the panel's, are the canary still. */
    CHECK(at(150, 20) == CANARY && at(20, 20) == CANARY && at(8, 40) == INK);
    /* The whole one painted all of its band, as it always did, and only
     * the rectangles are published. */
    CHECK(at(150, 70) == INK);
}

static void panel_cut(void)
{
    current = "the panel's part";
    widget(&kids[0], &kBare, (os64_gui_rect_t){0, 30, 5, 5});   /* at the left edge */
    window(1);
    os64_ui_mark_dirty(&ui, &kids[0]);
    os64_gui_rect_t list[OS64_UI_DAMAGE_MAX];
    CHECK(os64_ui_render_list(&ui, list) == 1);
    CHECK(at(0, 32) == PANEL_BORDER && at(2, 32) == PANEL_BG);
    CHECK(at(0, 20) == CANARY && at(0, 40) == CANARY && at(6, 32) == CANARY);
    CHECK(litter(list, 1) == 0);
}

static void hand_clear(void)
{
    current = "any_dirty cleared by hand";
    widget(&kids[0], &kWhole, (os64_gui_rect_t){10, 10, 10, 10});
    widget(&kids[1], &kWhole, (os64_gui_rect_t){290, 70, 12, 12});
    window(2);
    os64_ui_mark_dirty(&ui, &kids[0]);
    ui.any_dirty = false;
    CHECK(!os64_ui_dirty_covers(&ui, kids[0].bounds));
    os64_ui_mark_dirty(&ui, &kids[1]);
    CHECK(ui.damage_count == 1 && same(ui.damage[0], kids[1].bounds) && same(ui.dirty, kids[1].bounds));
    os64_gui_rect_t list[OS64_UI_DAMAGE_MAX];
    CHECK(os64_ui_render_list(&ui, list) == 1 && paints_of(&kids[0]) == 0);
}

static void box_and_covers(void)
{
    current = "the box, and covers";
    widget(&kids[0], &kWhole, (os64_gui_rect_t){10, 10, 10, 10});
    widget(&kids[1], &kWhole, (os64_gui_rect_t){290, 70, 12, 12});
    widget(&kids[2], &kBare, (os64_gui_rect_t){0, 0, 0, 0});
    window(3);
    os64_ui_mark_dirty(&ui, &kids[2]);                     /* empty: nothing */
    CHECK(!ui.any_dirty && ui.damage_count == 0);
    os64_ui_mark_dirty(&ui, &kids[0]);
    os64_ui_mark_dirty(&ui, &kids[1]);
    CHECK(os64_ui_dirty_covers(&ui, (os64_gui_rect_t){12, 12, 4, 4}));
    CHECK(!os64_ui_dirty_covers(&ui, (os64_gui_rect_t){10, 10, 292, 72}));
    os64_gui_rect_t box;
    CHECK(os64_ui_render(&ui, &box) && same(box, (os64_gui_rect_t){10, 10, 292, 72}));
    CHECK(!os64_ui_render(&ui, &box) && !os64_ui_render(&ui, NULL));
}

int main(void)
{
    far_apart();
    root_swallows();
    collapse();
    per_rectangle();
    panel_cut();
    hand_clear();
    box_and_covers();
    printf("test_ui_damage_host: %lu checks, %d failures\n", checks, failures);
    return failures != 0;
}
