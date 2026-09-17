/* The widget text path on the host: the same ui_font.c the guest links,
 * driven against real font files and against the bitmap painter it replaces.
 *
 * THE CENTRAL CLAIM IS PIXEL IDENTITY. Widgets used to draw through
 * os64_draw_text_clipped and now draw through F2 runs, so a window holding
 * the builtin role set must put down the bytes it always did. That is
 * checkable exactly, on two surfaces, with a memcmp — and it is the check
 * that says a font migration changed no appearance by accident.
 *
 * libui's tree and the window system are stubbed: this exercises the font
 * binding, not the toolkit. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/ui.h"
#include "os64/draw.h"
#include "os64/font_psf1.h"
#ifndef UI_TEXT_REAL
#include "fake_backend.h"
#endif

static int failures;
static const char *current = "";
#define CHECK(x) do { if (!(x)) { \
        printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, current, #x); \
        ++failures; } else ++checks; } while (0)
static unsigned long checks;

/* ── the stubs ─────────────────────────────────────────────────────────── */
/* ui_font.c allocates through libos64's heap and marks the window dirty. The
 * first becomes the host allocator; the second has no window to dirty. */
void *os64_malloc(size_t n) { return malloc(n ? n : 1); }
void os64_free(void *p) { free(p); }
void os64_ui_mark_dirty(os64_ui_t *ui, os64_ui_widget_t *w) { (void)ui; (void)w; }

#ifndef UI_TEXT_REAL
/* libui asks the provider for production FreeType and has no injection point
 * — by design: an application does not choose an engine. Without --real the
 * fake backend stands in at the link, which is enough for every check that
 * only touches the builtin bitmap face. */
const os64_font_backend_t *os64_freetype_backend_v1(void)
{
    return os64_fake_font_backend();
}
#endif

/* ── surfaces ──────────────────────────────────────────────────────────── */

#define SURF_W 320
#define SURF_H 64

typedef struct { os64_gui_surface_t s; uint32_t px[SURF_W * SURF_H]; } canvas_t;

static void canvas_init(canvas_t *c, uint32_t fill)
{
    c->s.pixels = c->px;
    c->s.width = SURF_W;
    c->s.height = SURF_H;
    c->s.pitch_px = SURF_W;
    for (size_t i = 0; i < SURF_W * (size_t)SURF_H; ++i)
        c->px[i] = fill;
}

/* ── reading a font off disk ───────────────────────────────────────────── */

static uint8_t *slurp(const char *dir, const char *name, size_t *length)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); ++failures; return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bytes = malloc((size_t)size);
    *length = fread(bytes, 1, (size_t)size, f);
    fclose(f);
    return bytes;
}

/* ── the tests ─────────────────────────────────────────────────────────── */

/* Every printable string a widget is likely to hold, plus the shapes that
 * make a painter disagree with itself: empty, one glyph, leading and
 * trailing spaces, the punctuation nobody kerns. */
static const char *kSamples[] = {
    "", "A", "OK", "Cancel", "Save As", "hello, world",
    "WWWWWWWW", "iiiiiiii", "trailing ", "  leading",
    "0123456789", "~!@#$%^&*()_+", "The quick brown fox",
};

static void identity_against_bitmap(os64_ui_t *ui)
{
    /* The builtin set is what a window binds on first use, so this is the
     * default appearance, not a special mode. */
    for (size_t n = 0; n < sizeof(kSamples) / sizeof(*kSamples); ++n) {
        current = kSamples[n];
        size_t len = strlen(kSamples[n]);
        canvas_t a, b;
        canvas_init(&a, 0xff202020);
        canvas_init(&b, 0xff202020);
        os64_gui_rect_t clip = {0, 0, SURF_W, SURF_H};

        int32_t pen_old = os64_draw_text_clipped(&a.s, clip, 4, 8, kSamples[n], len,
                                                 0xffe0e0e0, 0xff202020);
        int32_t pen_new = os64_ui_draw_text(ui, OS64_FONT_ROLE_UI, &b.s, clip, 4, 8,
                                            kSamples[n], len, 0xffe0e0e0, 0xff202020);

        CHECK(pen_old == pen_new);
        CHECK(memcmp(a.px, b.px, sizeof(a.px)) == 0);
        CHECK(os64_ui_text_width(ui, OS64_FONT_ROLE_UI, kSamples[n], len) ==
              (int32_t)len * OS64_FONT_GLYPH_W);
    }
    current = "";

    /* WHERE THE TWO PAINTERS DELIBERATELY PART. The bitmap painter had no
     * idea what a byte MEANT — it drew the PSF1 glyph at that index and
     * advanced one cell, control codes included. A run obeys F0: a tab
     * advances to the next stop and draws nothing, and any other control
     * byte is one missing-glyph marker. The new answer is the contracted
     * one, so this asserts it rather than asking the old painter to agree. */
    {
        current = "tab advances to a stop";
        int32_t tabbed = os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "a\tb", 3);
        int32_t plain = os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "ab", 2);
        CHECK(tabbed > plain);
        /* Eight columns of the builtin cell, then one more glyph. */
        CHECK(tabbed == 8 * OS64_FONT_GLYPH_W + OS64_FONT_GLYPH_W);
        canvas_t old_painter;
        canvas_init(&old_painter, 0);
        CHECK(os64_draw_text_clipped(&old_painter.s, (os64_gui_rect_t){0, 0, SURF_W, SURF_H},
                                     0, 0, "a\tb", 3, 0xffffffff, 0) == 3 * OS64_FONT_GLYPH_W);

        current = "a control byte is one marker";
        CHECK(os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "\x01", 1) > 0);
        current = "";
    }

    /* Clipping must agree too: a caption wider than its button is the
     * ordinary case, not an edge one. */
    for (int32_t width = 0; width < 40; width += 7) {
        current = "clip";
        canvas_t a, b;
        canvas_init(&a, 0xff000000);
        canvas_init(&b, 0xff000000);
        os64_gui_rect_t clip = {4, 8, width, OS64_FONT_GLYPH_H};
        os64_draw_text_clipped(&a.s, clip, 4, 8, "clipped caption", 15, 0xffffffff, 0);
        os64_ui_draw_text(ui, OS64_FONT_ROLE_UI, &b.s, clip, 4, 8,
                          "clipped caption", 15, 0xffffffff, 0);
        CHECK(memcmp(a.px, b.px, sizeof(a.px)) == 0);
    }
    current = "";
}

static void builtin_metrics(os64_ui_t *ui)
{
    current = "builtin metrics";
    os64_ui_font_metrics_t m;
    CHECK(os64_ui_font_metrics(ui, OS64_FONT_ROLE_UI, &m));
    CHECK(m.row_h == OS64_FONT_GLYPH_H && m.baseline == 12);
    CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT) == OS64_FONT_GLYPH_H);

    /* THE EQUALITY THE METRICS SHORTCUT RESTS ON: an unbound window answers
     * from the 8x16 fallback, a builtin-bound one from the provider's role
     * view, and a widget stamped before the first paint must not have to be
     * re-stamped after it. Same face, so the same row — asserted rather than
     * assumed, because the shortcut is invisible once it is wrong. */
    os64_ui_font_metrics_t bound, unbound;
    CHECK(os64_ui_font_metrics(ui, OS64_FONT_ROLE_UI, &bound));
    CHECK(!os64_ui_font_metrics(NULL, OS64_FONT_ROLE_UI, &unbound));
    CHECK(bound.row_h == unbound.row_h && bound.baseline == unbound.baseline);

    /* A detached widget has no window; it measures and draws as the bitmap
     * painter rather than faulting or refusing. */
    CHECK(!os64_ui_font_metrics(NULL, OS64_FONT_ROLE_UI, &m));
    CHECK(m.row_h == OS64_FONT_GLYPH_H && m.cell_w == OS64_FONT_GLYPH_W);
    CHECK(os64_ui_text_width(NULL, OS64_FONT_ROLE_UI, "abc", 3) == 3 * OS64_FONT_GLYPH_W);
    current = "";
}

#ifdef UI_TEXT_REAL
/* Prepare one role set from a file, at a nominal size. */
static os64_font_set_t *outline_set(os64_text_context_t *text, const char *dir,
                                    const char *name, uint32_t px)
{
    size_t length = 0;
    uint8_t *bytes = slurp(dir, name, &length);
    if (!bytes)
        return NULL;
    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT] = {0};
    for (int role = 0; role < OS64_FONT_ROLE_COUNT; ++role) {
        specs[role].pixel_height = px;
        specs[role].primary = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, bytes, length};
    }
    /* The terminal role rejects a proportional face, which is the provider
     * doing its job — give it the mono file so the set as a whole opens. */
    size_t mono_length = 0;
    uint8_t *mono = slurp(dir, "DejaVuSansMono.ttf", &mono_length);
    specs[OS64_FONT_ROLE_TERMINAL].primary =
        (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, mono, mono_length};

    os64_font_set_t *set = NULL;
    os64_font_status_t status = os64_font_set_prepare(text, specs, &set);
    free(bytes);
    free(mono);
    if (status != OS64_FONT_OK) {
        printf("FAIL prepare %s at %u: status %d\n", name, px, (int)status);
        ++failures;
        return NULL;
    }
    return set;
}

/* Widths under a proportional face must differ per character, or nothing
 * downstream of this layer is really measuring. */
static void proportional_widths(os64_ui_t *ui, const char *dir)
{
    current = "proportional";
    os64_text_context_t *text = os64_ui_font_context(ui);
    CHECK(text != NULL);

    int32_t mono_i = os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "iiii", 4);
    int32_t mono_w = os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "WWWW", 4);
    CHECK(mono_i == mono_w);   /* builtin: a cell is a cell */

    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);   /* the binding holds its own reference */

    int32_t prop_i = os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "iiii", 4);
    int32_t prop_w = os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "WWWW", 4);
    CHECK(prop_i < prop_w);
    CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) > 0);

    /* A larger nominal size makes a taller row and a wider string — and the
     * row is the face's line box, never the nominal number itself. */
    os64_font_set_t *big = outline_set(text, dir, "DejaVuSans.ttf", 28);
    CHECK(big != NULL);
    if (big) {
        int32_t small_row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
        int32_t small_w = os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "Cancel", 6);
        CHECK(os64_ui_font_bind(ui, big) == OS64_FONT_OK);
        os64_font_set_release(big);
        CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) > small_row);
        CHECK(os64_ui_text_width(ui, OS64_FONT_ROLE_UI, "Cancel", 6) > small_w);
        CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) != 28);
    }
    current = "";
}

/* ORDERING: a widget's geometry has to be re-derived before the app's own
 * commit runs, because an app arranges widgets by the heights they report.
 * A tree of one widget is enough to prove which side of the line it lands
 * on — and the overlapping labels this caught are not subtle on a screen. */
static int32_t stamped_row;
static void probe_metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{
    w->bounds.h = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
}
static const os64_ui_class_t kProbeClass = {"probe", NULL, NULL, NULL, probe_metrics};
static os64_ui_widget_t gProbe;

/* The adoption transaction: a refused plan must leave the window exactly as
 * it was, and a committed one must leave the candidate installed. */
static os64_font_status_t planner_refuse(os64_ui_t *ui, void *user, void **out)
{
    (void)ui; (void)out;
    *(int *)user += 1;
    return OS64_FONT_LIMIT;
}

static int plan_calls, commit_calls, discard_calls;
static int32_t planned_row;

static os64_font_status_t planner_ok(os64_ui_t *ui, void *user, void **out)
{
    (void)user;
    ++plan_calls;
    /* The point of planning: measurement here answers with the CANDIDATE. */
    planned_row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
    int *stage = malloc(sizeof(int));
    if (!stage)
        return OS64_FONT_NO_MEMORY;
    *stage = planned_row;
    *out = stage;
    return OS64_FONT_OK;
}
static void planner_commit(os64_ui_t *ui, void *user, void *plan)
{
    (void)user;
    ++commit_calls;
    /* What an app's layout would read at this moment. */
    stamped_row = gProbe.bounds.h;
    (void)ui;
    free(plan);
}
static void planner_discard(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; ++discard_calls; free(plan); }

static void adoption(os64_ui_t *ui, const char *dir)
{
    current = "adoption";
    os64_text_context_t *text = os64_ui_font_context(ui);
    os64_font_set_t *before = os64_ui_font_set(ui);
    int32_t row_before = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);

    os64_font_set_t *candidate = outline_set(text, dir, "SourceSans3-Regular.otf", 20);
    CHECK(candidate != NULL);
    if (!candidate) { current = ""; return; }

    memset(&gProbe, 0, sizeof(gProbe));
    gProbe.cls = &kProbeClass;
    ui->root = &gProbe;
    os64_ui_font_restamp(ui);
    CHECK(gProbe.bounds.h == row_before);

    os64_font_consumer_t consumer;
    os64_ui_font_consumer(ui, &consumer);
    size_t failed = 0;

    /* A planner that refuses fails the whole adoption, unchanged. */
    int refusals = 0;
    os64_ui_font_planner(ui, planner_refuse, NULL, NULL, &refusals);
    CHECK(os64_font_adopt(candidate, &consumer, 1, &failed) == OS64_FONT_LIMIT);
    CHECK(refusals == 1 && failed == 0);
    CHECK(os64_ui_font_set(ui) == before);
    CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) == row_before);

    /* And once it agrees, the candidate is installed and the app's staged
     * layout is committed — with the row it measured during planning. */
    plan_calls = commit_calls = discard_calls = 0;
    os64_ui_font_planner(ui, planner_ok, planner_commit, planner_discard, NULL);
    CHECK(os64_font_adopt(candidate, &consumer, 1, &failed) == OS64_FONT_OK);
    CHECK(plan_calls == 1 && commit_calls == 1 && discard_calls == 0);
    CHECK(os64_ui_font_set(ui) == candidate);
    CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) == planned_row);
    CHECK(planned_row != row_before);
    CHECK(stamped_row == planned_row);   /* not the retired face's row */
    os64_font_set_release(candidate);

    /* Measurement outside a transaction answers from the installed set. */
    CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) == planned_row);
    os64_ui_font_planner(ui, NULL, NULL, NULL, NULL);
    current = "";
}
#endif

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "userland/libfreetype/fixtures";
    (void)dir;

    /* A window with no widgets: the binding is all these tests need, and it
     * is created on demand exactly as it is in a real program. */
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));

    identity_against_bitmap(&ui);
    builtin_metrics(&ui);
    CHECK(os64_ui_font_status(&ui) == OS64_FONT_OK);

#ifdef UI_TEXT_REAL
    proportional_widths(&ui, dir);
    adoption(&ui, dir);
#else
    (void)slurp;
#endif

    /* Nothing owed when the window goes. */
    os64_ui_font_release(&ui);
    CHECK(os64_ui_font_live_bytes(&ui) == 0);
    CHECK(ui.font == NULL);

    printf("test_ui_text_host: %lu checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
