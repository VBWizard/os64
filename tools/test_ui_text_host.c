/* The widget text path on the host: the same ui_font.c the guest links,
 * driven against real font files and against the bitmap painter it replaces.
 *
 * THE CENTRAL CLAIM IS PIXEL IDENTITY. Widgets used to draw through
 * os64_draw_text_clipped and now draw through F2 runs, so a window holding
 * the builtin role set must put down the bytes it always did. That is
 * checkable exactly, on two surfaces, with a memcmp — and it is the check
 * that says a font migration changed no appearance by accident.
 *
 * THE SECOND CLAIM IS THAT FAILURE IS VISIBLE. An allocation refused while
 * measuring or painting must not quietly become the 8x16 cell's answer: a
 * window wearing DejaVu measures `WWWW` at 96 pixels, and 32 is not a worse
 * version of that number, it is a different font. Everything below that
 * denies an allocation is checking that the refusal travels.
 *
 * The toolkit is linked for real; only the window system is stubbed.
 *
 * ITS FILE-STATIC NAMES ARE SHARED. The review probes under
 * docs/fonts/f4-evidence/c1-review/ #include this file whole to reuse its
 * fixtures, so a new static here lands in their namespace too and a common
 * name collides at compile time. Anything general enough to be wanted twice
 * — a helper, a callback, a widget — takes the `ui_test_` prefix. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/ui.h"
#include "os64/draw.h"
#include "os64/text.h"
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

/* ── the stubs, and the allocation injector ────────────────────────────── */
/* ui_font.c allocates through libos64's heap and marks the window dirty. The
 * first becomes the host allocator — with a switch for denying it, since
 * "what happens when layout cannot allocate" is half of what is under test
 * — and the second has no window to dirty. */
static long deny_countdown = -1;   /* -1 never denies; 0 denies the next one */
static bool ui_test_deny_all;      /* every allocation refused while set */
static unsigned long allocations;

void *os64_malloc(size_t n)
{
    ++allocations;
    if (ui_test_deny_all) return NULL;
    if (deny_countdown == 0) { deny_countdown = -1; return NULL; }
    if (deny_countdown > 0) --deny_countdown;
    return malloc(n ? n : 1);
}
void os64_free(void *p) { free(p); }

/* The toolkit itself is linked — the height policy lives in its
 * constructors and its layout, and the editor's caret arithmetic in
 * ui_text.c — so nothing of it is stubbed. */
#include "ui_internal.h"

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

/* How many scanlines outside [top, top+h) got painted. */
static int rows_touched_outside(const canvas_t *c, uint32_t background,
                                int32_t top, int32_t h)
{
    int outside = 0;
    for (int32_t y = 0; y < SURF_H; ++y) {
        if (y >= top && y < top + h)
            continue;
        for (int32_t x = 0; x < SURF_W; ++x)
            if (c->px[y * SURF_W + x] != background) { ++outside; break; }
    }
    return outside;
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

static int32_t measured(os64_ui_t *ui, const char *s, size_t len)
{
    int32_t width = -1;
    CHECK(os64_ui_text_measure(ui, OS64_FONT_ROLE_UI, s, len, &width) == OS64_FONT_OK);
    return width;
}

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
        int32_t pen_new = os64_ui_draw_text(ui, NULL, OS64_FONT_ROLE_UI, &b.s, clip,
                                            4, 8, kSamples[n], len,
                                            0xffe0e0e0, 0xff202020);

        CHECK(pen_old == pen_new);
        CHECK(memcmp(a.px, b.px, sizeof(a.px)) == 0);
        if (len)
            CHECK(measured(ui, kSamples[n], len) == (int32_t)len * OS64_FONT_GLYPH_W);
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
        int32_t tabbed = measured(ui, "a\tb", 3);
        int32_t plain = measured(ui, "ab", 2);
        CHECK(tabbed > plain);
        /* Eight columns of the builtin cell, then one more glyph. */
        CHECK(tabbed == 8 * OS64_FONT_GLYPH_W + OS64_FONT_GLYPH_W);
        canvas_t old_painter;
        canvas_init(&old_painter, 0);
        CHECK(os64_draw_text_clipped(&old_painter.s, (os64_gui_rect_t){0, 0, SURF_W, SURF_H},
                                     0, 0, "a\tb", 3, 0xffffffff, 0) == 3 * OS64_FONT_GLYPH_W);

        current = "a control byte is one marker";
        CHECK(measured(ui, "\x01", 1) > 0);
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
        os64_ui_draw_text(ui, NULL, OS64_FONT_ROLE_UI, &b.s, clip, 4, 8,
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
    int32_t width = -1;
    CHECK(os64_ui_text_measure(NULL, OS64_FONT_ROLE_UI, "abc", 3, &width) == OS64_FONT_OK);
    CHECK(width == 3 * OS64_FONT_GLYPH_W);
    current = "";
}

/* ORDERING: a widget's geometry has to be re-derived before the app's own
 * commit runs, because an app arranges widgets by the heights they report.
 * A tree of one widget is enough to prove which side of the line it lands
 * on — and the overlapping labels this caught are not subtle on a screen. */
static int32_t stamped_row;
static void probe_metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{
    w->natural_h = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
}
static const os64_ui_class_t kProbeClass = {
    "probe", NULL, NULL, NULL,
    os64_ui_stage_caption, os64_ui_commit_caption, os64_ui_discard_caption,
    NULL, probe_metrics
};
static os64_ui_widget_t gProbe;

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

    CHECK(measured(ui, "iiii", 4) == measured(ui, "WWWW", 4));   /* a cell is a cell */

    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);   /* the binding holds its own reference */

    CHECK(measured(ui, "iiii", 4) < measured(ui, "WWWW", 4));
    CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) > 0);

    /* A larger nominal size makes a taller row and a wider string — and the
     * row is the face's line box, never the nominal number itself. */
    os64_font_set_t *big = outline_set(text, dir, "DejaVuSans.ttf", 28);
    CHECK(big != NULL);
    if (big) {
        int32_t small_row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
        int32_t small_w = measured(ui, "Cancel", 6);
        CHECK(os64_ui_font_bind(ui, big) == OS64_FONT_OK);
        os64_font_set_release(big);
        CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) > small_row);
        CHECK(measured(ui, "Cancel", 6) > small_w);
        CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) != 28);
    }
    current = "";
}

/* R2 — a denied allocation must never come back as the bitmap cell's number.
 * `WWWW` is 96 pixels in DejaVu 24 and 32 in the 8x16 face; a measurement
 * that answered 32 here would let a planner stage a layout in a font nobody
 * is wearing and report success. */
static void measurement_failure_travels(os64_ui_t *ui, const char *dir)
{
    current = "measurement failure";
    os64_text_context_t *text = os64_ui_font_context(ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 24);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);

    int32_t honest = measured(ui, "WWWW", 4);
    CHECK(honest > 4 * OS64_FONT_GLYPH_W);   /* i.e. not the bitmap answer */

    /* Walk the denial through the allocations a layout makes. Whatever
     * fails, the answer is a status — never a smaller font's number. */
    int refusals = 0;
    for (long n = 0; n < 8; ++n) {
        int32_t width = 12345;
        deny_countdown = n;
        os64_font_status_t status =
            os64_ui_text_measure(ui, OS64_FONT_ROLE_UI, "WWWW", 4, &width);
        deny_countdown = -1;
        if (status == OS64_FONT_OK) {
            CHECK(width == honest);          /* if it succeeded, it was honest */
        } else {
            ++refusals;
            CHECK(width == 0);
            CHECK(width != 4 * OS64_FONT_GLYPH_W);
        }
    }
    CHECK(refusals > 0);                     /* the injector really bit */

    /* Painting under the same denial draws nothing rather than a different
     * font's glyphs at a different size. */
    {
        canvas_t denied, background;
        canvas_init(&denied, 0xff000000);
        canvas_init(&background, 0xff000000);
        os64_gui_rect_t whole = {0, 0, SURF_W, SURF_H};
        deny_countdown = 0;
        int32_t pen = os64_ui_draw_text(ui, NULL, OS64_FONT_ROLE_UI, &denied.s,
                                        whole, 0, 0, "WWWW", 4, 0xffffffff, 0xff000000);
        deny_countdown = -1;
        CHECK(pen == 0);                                     /* nothing advanced */
        CHECK(memcmp(denied.px, background.px, sizeof(denied.px)) == 0);
    }

    /* The window is unchanged by any of it. */
    CHECK(measured(ui, "WWWW", 4) == honest);
    current = "";
}

/* R4 — the row is the ceiling. A 16px marker inside a smaller primary row
 * must be cut, not allowed to paint over the line above, even when the
 * caller hands in a clip the size of the surface. */
static void marker_stays_inside_the_row(os64_ui_t *ui, const char *dir)
{
    current = "row clip";
    os64_text_context_t *text = os64_ui_font_context(ui);
    os64_font_set_t *small = outline_set(text, dir, "DejaVuSans.ttf", 8);
    CHECK(small != NULL);
    if (!small) { current = ""; return; }
    CHECK(os64_ui_font_bind(ui, small) == OS64_FONT_OK);
    os64_font_set_release(small);

    int32_t row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
    CHECK(row > 0 && row < OS64_FONT_GLYPH_H);   /* smaller than the marker */

    canvas_t c;
    canvas_init(&c, 0xff000000);
    /* U+2603 is outside the Western profile's coverage: it draws the
     * missing-glyph marker, which is the 8x16 cell and taller than this row. */
    const char *snow = "\xe2\x98\x83";
    os64_gui_rect_t whole = {0, 0, SURF_W, SURF_H};
    os64_ui_draw_text(ui, NULL, OS64_FONT_ROLE_UI, &c.s, whole, 4, 20,
                      snow, 3, 0xffffffff, 0xff000000);
    CHECK(rows_touched_outside(&c, 0xff000000, 20, row) == 0);

    /* The row above belongs to somebody else, and must be untouched even
     * when the marker would have reached it. */
    CHECK(c.px[19 * SURF_W + 4] == 0xff000000);
    current = "";
}

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
    (void)ui; (void)user;
    ++commit_calls;
    stamped_row = gProbe.natural_h;   /* what an app's layout would read */
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
    gProbe.text = "a caption to stage";
    ui->root = &gProbe;
    os64_ui_font_restamp(ui);
    CHECK(gProbe.natural_h == row_before);

    os64_font_consumer_t consumer;
    os64_ui_font_consumer(ui, &consumer);
    size_t failed = 0;

    /* A planner that refuses fails the whole adoption, unchanged. */
    int refusals = 0;
    CHECK(os64_ui_font_planner(ui, planner_refuse, planner_commit, planner_discard,
                               &refusals) == OS64_FONT_OK);
    CHECK(os64_font_adopt(candidate, &consumer, 1, &failed) == OS64_FONT_LIMIT);
    CHECK(refusals == 1 && failed == 0);
    CHECK(os64_ui_font_set(ui) == before);
    CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) == row_before);
    CHECK(gProbe.natural_h == row_before);      /* staging was rolled back */
    CHECK(gProbe.run_staged == NULL);           /* and its runs thrown away */

    /* And once it agrees, the candidate is installed and the app's staged
     * layout is committed — with the row it measured during planning. */
    plan_calls = commit_calls = discard_calls = 0;
    CHECK(os64_ui_font_planner(ui, planner_ok, planner_commit, planner_discard,
                               NULL) == OS64_FONT_OK);
    CHECK(os64_font_adopt(candidate, &consumer, 1, &failed) == OS64_FONT_OK);
    CHECK(plan_calls == 1 && commit_calls == 1 && discard_calls == 0);
    CHECK(os64_ui_font_set(ui) == candidate);
    CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) == planned_row);
    CHECK(planned_row != row_before);
    CHECK(stamped_row == planned_row);   /* not the retired face's row */
    CHECK(gProbe.run != NULL);           /* commit swapped the staged run in */

    /* THE FIRST PAINT AFTER A COMMIT ALLOCATES NOTHING. That is the whole
     * reason prepare stages runs: a paint has nowhere to report a failure
     * to, so it must not be able to fail. */
    {
        canvas_t c;
        canvas_init(&c, 0xff000000);
        os64_gui_rect_t whole = {0, 0, SURF_W, SURF_H};
        unsigned long before_allocs = allocations;
        os64_ui_draw_text(ui, &gProbe.run, OS64_FONT_ROLE_UI, &c.s, whole, 0, 0,
                          gProbe.text, strlen(gProbe.text), 0xffffffff, 0xff000000);
        CHECK(allocations == before_allocs);
    }

    /* A caption that changes underneath the retained run is re-laid-out
     * rather than painted from the stale one. */
    {
        canvas_t stale, fresh;
        canvas_init(&stale, 0xff000000);
        canvas_init(&fresh, 0xff000000);
        os64_gui_rect_t whole = {0, 0, SURF_W, SURF_H};
        os64_ui_draw_text(ui, &gProbe.run, OS64_FONT_ROLE_UI, &stale.s, whole, 0, 0,
                          gProbe.text, strlen(gProbe.text), 0xffffffff, 0xff000000);
        gProbe.text = "a different caption";
        os64_ui_draw_text(ui, &gProbe.run, OS64_FONT_ROLE_UI, &fresh.s, whole, 0, 0,
                          gProbe.text, strlen(gProbe.text), 0xffffffff, 0xff000000);
        CHECK(memcmp(stale.px, fresh.px, sizeof(stale.px)) != 0);
        CHECK(os64_ui_run_matches(gProbe.run, gProbe.text, strlen(gProbe.text)));
    }

    /* Staging that fails partway must leave nothing behind: the set is the
     * old one, the staged runs are gone, and discard ran exactly once. */
    {
        plan_calls = commit_calls = discard_calls = 0;
        os64_font_set_t *installed = os64_ui_font_set(ui);
        int32_t installed_row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
        deny_countdown = 1;   /* somewhere inside prepare's staging */
        os64_font_status_t status = os64_font_adopt(candidate, &consumer, 1, &failed);
        deny_countdown = -1;
        if (status != OS64_FONT_OK) {
            CHECK(os64_ui_font_set(ui) == installed);
            CHECK(os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI) == installed_row);
            CHECK(gProbe.run_staged == NULL);
            CHECK(discard_calls <= 1);
        }
    }

    os64_font_set_release(candidate);
    CHECK(os64_ui_font_planner(ui, NULL, NULL, NULL, NULL) == OS64_FONT_OK);
    ui->root = NULL;
    os64_ui_run_release(gProbe.run);
    gProbe.run = NULL;
    current = "";
}

/* R3 — a registration that could not be recorded must say so, and must not
 * leave a later adoption installing a face with no application layout. */
static void registration_reports_failure(const char *dir)
{
    current = "registration";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));

    /* REGISTRATION CANNOT RUN OUT OF MEMORY. The trio lives in the
     * os64_ui_t the application already owns, so there is no allocation to
     * deny — which is stronger than reporting a failure, because a caller
     * that ignored the status could otherwise adopt with no layout staged. */
    int calls = 0;
    unsigned long before = allocations;
    deny_countdown = 0;
    CHECK(os64_ui_font_planner(&ui, planner_refuse, planner_commit, planner_discard,
                               &calls) == OS64_FONT_OK);
    deny_countdown = -1;
    CHECK(allocations == before);
    CHECK(ui.font_plan == planner_refuse);

    /* A partial trio is refused outright: a plan that can be applied but not
     * discarded leaks on every refusal. */
    CHECK(os64_ui_font_planner(&ui, planner_refuse, NULL, NULL, &calls)
          == OS64_FONT_BAD_ARGUMENT);
    CHECK(os64_ui_font_planner(&ui, NULL, NULL, NULL, NULL) == OS64_FONT_OK);

    /* The adoption reaches the planner, and its refusal decides the outcome. */
    CHECK(os64_ui_font_planner(&ui, planner_refuse, planner_commit, planner_discard,
                               &calls) == OS64_FONT_OK);
    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 18);
    CHECK(set != NULL);
    if (set) {
        os64_font_consumer_t consumer;
        os64_ui_font_consumer(&ui, &consumer);
        CHECK(os64_font_adopt(set, &consumer, 1, NULL) == OS64_FONT_LIMIT);
        CHECK(calls == 1);               /* the planner really was registered */
        os64_font_set_release(set);
    }
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* R1 — the binding is the context's allocator, so it must outlive anything
 * that can still call into it. */
static void release_refuses_while_busy(const char *dir)
{
    current = "busy release";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);

    /* The caller is still holding its own reference to the candidate. */
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_BUSY);
    CHECK(ui.font != NULL);                 /* the allocator owner survives */

    os64_font_set_release(set);             /* used freed memory before R1 */
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    CHECK(ui.font == NULL);

    /* The same, with a RUN outstanding rather than a set. */
    memset(&ui, 0, sizeof(ui));
    text = os64_ui_font_context(&ui);
    set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (set) {
        CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);
        os64_font_set_release(set);
        void *run = NULL;
        CHECK(os64_ui_run_layout(&ui, OS64_FONT_ROLE_UI, "held", 4, &run) == OS64_FONT_OK);
        CHECK(run != NULL);
        CHECK(os64_ui_font_release(&ui) == OS64_FONT_BUSY);
        CHECK(ui.font != NULL);
        os64_ui_run_release(run);
        CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    }
    current = "";
}

/* Q4 — several windows, one application-owned engine; and a set from
 * somewhere else is refused rather than silently measured wrong. */
static void shared_and_foreign_contexts(const char *dir)
{
    current = "shared context";
    os64_ui_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    /* One context, made the ordinary way, then lent to a second window. */
    os64_text_context_t *shared = os64_ui_font_context(&a);
    CHECK(shared != NULL);
    CHECK(os64_ui_font_borrow_context(&b, shared) == OS64_FONT_OK);
    CHECK(os64_ui_font_context(&b) == shared);

    os64_font_set_t *one = outline_set(shared, dir, "DejaVuSans.ttf", 16);
    os64_font_set_t *two = outline_set(shared, dir, "SourceSans3-Regular.otf", 24);
    CHECK(one != NULL && two != NULL);
    if (one && two) {
        CHECK(os64_ui_font_bind(&a, one) == OS64_FONT_OK);
        CHECK(os64_ui_font_bind(&b, two) == OS64_FONT_OK);
        /* Two windows, two active faces, one engine — which is what makes a
         * preview beside the live article possible without a second cache. */
        CHECK(os64_ui_font_row_height(&a, OS64_FONT_ROLE_UI) !=
              os64_ui_font_row_height(&b, OS64_FONT_ROLE_UI));
        CHECK(measured(&a, "WWWW", 4) != measured(&b, "WWWW", 4));
        os64_font_set_release(one);
        os64_font_set_release(two);
    }

    /* A window that already wears something cannot be moved to another
     * engine: its runs would be stranded in a context nobody destroys. */
    os64_ui_t c;
    memset(&c, 0, sizeof(c));
    os64_text_context_t *own = os64_ui_font_context(&c);
    CHECK(own != NULL);
    int32_t ignored = 0;
    CHECK(os64_ui_text_measure(&c, OS64_FONT_ROLE_UI, "x", 1, &ignored) == OS64_FONT_OK);
    CHECK(os64_ui_font_borrow_context(&c, shared) == OS64_FONT_BUSY);

    /* A candidate from a FOREIGN context is refused. Before this check it
     * bound happily and then measured every string in the bitmap cell,
     * because F2 rejected the mismatch and the wrapper called that "no
     * face". Wrong font, wrong widths, status OK. */
    os64_font_set_t *foreign = outline_set(own, dir, "DejaVuSans.ttf", 16);
    CHECK(foreign != NULL);
    if (foreign) {
        CHECK(os64_ui_font_bind(&a, foreign) == OS64_FONT_BAD_ARGUMENT);
        os64_font_consumer_t consumer;
        os64_ui_font_consumer(&a, &consumer);
        CHECK(os64_font_adopt(foreign, &consumer, 1, NULL) == OS64_FONT_BAD_ARGUMENT);
        os64_font_set_release(foreign);
    }

    CHECK(os64_ui_font_release(&c) == OS64_FONT_OK);
    CHECK(os64_ui_font_release(&b) == OS64_FONT_OK);   /* borrower first */
    CHECK(os64_ui_font_release(&a) == OS64_FONT_OK);   /* owner destroys it */
    current = "";
}

/* R2a — the tab interval is MEASURED, and a measurement that could not be
 * taken is a status. It used to fall back to the builtin cell's eight
 * columns, which is indistinguishable from a correct answer under a face
 * whose space happens to be 8 pixels wide — and wrong, silently and
 * persistently, under every other face. */
static void tab_interval_failure_travels(const char *dir)
{
    current = "tab interval";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    os64_text_context_t *text = os64_ui_font_context(&ui);

    /* DejaVu at 16px has a 5-pixel space, so eight of them is 40 and not
     * the 64 the bitmap cell would give — the face the original probe used
     * at 24px could hide this because its interval matched. */
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);

    int32_t space = measured(&ui, " ", 1);
    int32_t tab = measured(&ui, "\t", 1);
    CHECK(space > 0 && space != OS64_FONT_GLYPH_W);
    CHECK(tab == 8 * space);                    /* eight of THIS face's spaces */

    /* Walk a denial through binding. Every refusal must be reported, and
     * the window must still be wearing what it had — never a set whose tab
     * stop came from a different face. */
    int refusals = 0;
    for (long n = 0; n < 14; ++n) {
        os64_font_set_t *candidate = outline_set(text, dir, "DejaVuSans.ttf", 16);
        if (!candidate)
            break;
        deny_countdown = n;
        os64_font_status_t status = os64_ui_font_bind(&ui, candidate);
        deny_countdown = -1;
        if (status != OS64_FONT_OK) {
            ++refusals;
            CHECK(os64_ui_font_set(&ui) == set);          /* old face kept */
        }
        CHECK(measured(&ui, "\t", 1) == 8 * space);      /* never 64 */
        os64_font_set_release(candidate);
    }
    CHECK(refusals > 0);
    os64_font_set_release(set);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* R2b — the button's alignment and its ink come from ONE run. Measuring
 * separately allocated on every paint, and on refusal paired a zero width
 * with a caption that painted fine: a centred label silently left-aligned. */
static void button_paints_from_its_run(const char *dir)
{
    current = "button alignment";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.theme = (os64_ui_theme_t){ .pad = 6, .button_h = 20, .gap = 4,
                                  .button_fg = 0xffffffff, .button_face = 0xff000000,
                                  .button_border = 0xff000000, .panel_bg = 0xff000000 };
    os64_ui_widget_t button;
    os64_ui_button(&button, "WWWW", NULL, NULL);
    button.bounds = (os64_gui_rect_t){0, 0, 200, 50};
    os64_ui_set_root(&ui, &button);

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 24);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(&ui, &consumer);
    CHECK(os64_font_adopt(set, &consumer, 1, NULL) == OS64_FONT_OK);
    os64_font_set_release(set);

    /* The real class painter, not the draw helper: it is the one that used
     * to measure a second time. */
    canvas_t normal, denied;
    canvas_init(&normal, 0xff000000);
    canvas_init(&denied, 0xff000000);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.surf = normal.s;
    unsigned long before = allocations;
    button.cls->paint(&button, &ctx, &ui.theme);
    CHECK(allocations == before);            /* adoption prepared the run */

    ctx.surf = denied.s;
    deny_countdown = 0;
    button.cls->paint(&button, &ctx, &ui.theme);
    deny_countdown = -1;
    /* Same placement, same pixels: nothing was re-measured, so nothing
     * could disagree. */
    CHECK(memcmp(normal.px, denied.px, sizeof(normal.px)) == 0);

    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    CHECK(os64_ui_font_live_bytes(&ui) == 0);
    current = "";
}

/* R2c + R5 — a list that the adoption makes TALLER must have runs prepared
 * for the rows it will be able to show, not the rows it can show now; and
 * whatever it retains must be released when the window's fonts go. */
static os64_ui_listbox_t gUiTestList;
static const char *const kUiTestRows[] = { "first row", "second row", "third row" };
static const char *ui_test_list_label(size_t index, void *user)
{ (void)user; return index < 3 ? kUiTestRows[index] : ""; }

static os64_font_status_t grow_plan(os64_ui_t *ui, void *user, void **out)
{
    (void)ui; (void)user;
    os64_gui_rect_t grown = gUiTestList.w.bounds;
    grown.h = 58;                            /* room for three rows, not one */
    os64_ui_widget_stage_bounds(&gUiTestList.w, grown);
    *out = &gUiTestList;
    return OS64_FONT_OK;
}
static void grow_commit(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; (void)plan; }        /* libui applies the staged rect */

static void list_stages_its_candidate_rows(const char *dir)
{
    current = "list staging";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.theme = (os64_ui_theme_t){ .pad = 6, .button_h = 20, .gap = 4,
                                  .field_bg = 0xff000000, .field_fg = 0xffffffff,
                                  .text_sel_bg = 0xff222222, .text_sel_fg = 0xffffffff,
                                  .field_border = 0xff000000, .focus_ring = 0xff000000 };
    os64_ui_listbox(&gUiTestList, 3, ui_test_list_label, NULL, NULL);
    gUiTestList.w.bounds = (os64_gui_rect_t){0, 0, 200, 22};   /* one row fits today */
    os64_ui_set_root(&ui, &gUiTestList.w);
    CHECK(os64_ui_font_planner(&ui, grow_plan, grow_commit, grow_commit, NULL)
          == OS64_FONT_OK);

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 8);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(&ui, &consumer);
    CHECK(os64_font_adopt(set, &consumer, 1, NULL) == OS64_FONT_OK);
    os64_font_set_release(set);

    /* The staged height was applied, and there is a prepared run for every
     * row the box can now show. */
    CHECK(gUiTestList.w.bounds.h == 58);
    CHECK(os64_ui_listbox_rows(&gUiTestList, &ui.theme) == 3);
    CHECK(gUiTestList.row_run_count == 3);

    /* So the first paint allocates nothing, and painting with every
     * allocation refused is pixel-identical to painting normally. */
    canvas_t normal, denied;
    canvas_init(&normal, 0xff000000);
    canvas_init(&denied, 0xff000000);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.surf = normal.s;
    unsigned long before = allocations;
    gUiTestList.w.cls->paint(&gUiTestList.w, &ctx, &ui.theme);
    CHECK(allocations == before);
    ctx.surf = denied.s;
    deny_countdown = 0;
    gUiTestList.w.cls->paint(&gUiTestList.w, &ctx, &ui.theme);
    deny_countdown = -1;
    CHECK(memcmp(normal.px, denied.px, sizeof(normal.px)) == 0);

    /* R5: with nothing outstanding outside the window, teardown completes.
     * The list's runs live in its own array, which only its class can see. */
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    CHECK(gUiTestList.row_run_count == 0);
    CHECK(os64_ui_font_live_bytes(&ui) == 0);
    CHECK(ui.font == NULL);
    current = "";
}

/* The leftmost painted column, or the surface width when nothing was
 * painted — which is how "the caption is not there at all" is spelled. */
static int32_t ui_test_ink_left(const canvas_t *c, uint32_t background)
{
    for (int32_t x = 0; x < SURF_W; ++x)
        for (int32_t y = 0; y < SURF_H; ++y)
            if (c->px[y * SURF_W + x] != background)
                return x;
    return SURF_W;
}

/* R2b follow-up — A WIDTH THAT DOES NOT EXIST IS NOT A POSITION. When a
 * caption changes, the new text needs a new layout. If that layout is
 * refused the width is zero, and the draw's own retry could still succeed
 * and paint the new caption at the place a zero width implied. */
static void changed_caption_is_not_placed_from_nothing(const char *dir)
{
    current = "caption change";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.theme = (os64_ui_theme_t){ .pad = 6, .button_h = 20, .gap = 4,
                                  .button_fg = 0xffffffff, .button_face = 0xff000000,
                                  .button_border = 0xff000000, .panel_bg = 0xff000000 };
    os64_ui_widget_t button;
    os64_ui_button(&button, "iiii", NULL, NULL);
    button.bounds = (os64_gui_rect_t){0, 0, 200, 50};
    os64_ui_set_root(&ui, &button);

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 24);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(&ui, &consumer);
    CHECK(os64_font_adopt(set, &consumer, 1, NULL) == OS64_FONT_OK);
    os64_font_set_release(set);

    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    canvas_t correct, denied;

    /* Where the new caption belongs, painted with nothing in the way. */
    button.text = "WWWW";
    canvas_init(&correct, 0xff000000);
    ctx.surf = correct.s;
    button.cls->paint(&button, &ctx, &ui.theme);
    int32_t want = ui_test_ink_left(&correct, 0xff000000);
    CHECK(want > 0 && want < SURF_W);

    /* Now the same change from scratch, with one allocation denied and the
     * rest available — the shape that used to paint it at X=100. */
    os64_ui_run_release(button.run);
    button.run = NULL;
    button.text = "iiii";
    canvas_init(&denied, 0xff000000);
    ctx.surf = denied.s;
    button.cls->paint(&button, &ctx, &ui.theme);   /* settle on the old caption */
    canvas_init(&denied, 0xff000000);
    button.text = "WWWW";
    deny_countdown = 0;
    button.cls->paint(&button, &ctx, &ui.theme);
    deny_countdown = -1;

    /* Either the caption is where it belongs, or it is not painted. What it
     * must never be is somewhere a failed measurement put it. */
    int32_t got = ui_test_ink_left(&denied, 0xff000000);
    CHECK(got == want || got == SURF_W);

    /* And the paint after it puts the caption back where it belongs. */
    canvas_t recovered;
    canvas_init(&recovered, 0xff000000);
    ctx.surf = recovered.s;
    button.cls->paint(&button, &ctx, &ui.theme);
    CHECK(memcmp(recovered.px, correct.px, sizeof(correct.px)) == 0);

    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    CHECK(os64_ui_font_live_bytes(&ui) == 0);
    current = "";
}

/* R6 — children belong inside the parent THIS layout is about. A planner
 * that moves a panel and then stacks it used to commit the panel's new
 * rectangle with children measured from its old one. */
static os64_ui_widget_t gStagePanel, gStageChild;

static os64_font_status_t move_panel_plan(os64_ui_t *ui, void *user, void **out)
{
    (void)user;
    os64_ui_widget_stage_bounds(&gStagePanel, (os64_gui_rect_t){40, 20, 160, 220});
    os64_ui_stack_vertical_staged(ui, &gStagePanel);
    *out = &gStagePanel;
    return OS64_FONT_OK;
}
static os64_font_status_t refuse_after_staging(os64_ui_t *ui, void *user, void **out)
{
    move_panel_plan(ui, user, out);
    *out = NULL;
    return OS64_FONT_LIMIT;
}
static void stage_nop(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; (void)plan; }

static void staged_children_follow_their_staged_parent(const char *dir)
{
    current = "staged parent";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.theme = (os64_ui_theme_t){ .pad = 6, .button_h = 20, .gap = 4,
                                  .panel_bg = 0xff000000, .panel_border = 0xff000000,
                                  .label_fg = 0xffffffff };
    os64_ui_panel(&gStagePanel);
    gStagePanel.bounds = (os64_gui_rect_t){0, 0, 200, 240};
    os64_ui_label(&gStageChild, "inside");
    os64_ui_add_child(&gStagePanel, &gStageChild);
    os64_ui_set_root(&ui, &gStagePanel);
    os64_ui_stack_vertical(&ui, &gStagePanel);

    os64_gui_rect_t live_parent = gStagePanel.bounds;
    os64_gui_rect_t live_child = gStageChild.bounds;

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(&ui, &consumer);

    /* An adoption that REFUSES after staging must leave both rectangles
     * exactly as they were. */
    os64_font_set_t *first = outline_set(text, dir, "DejaVuSans.ttf", 20);
    CHECK(first != NULL);
    if (first) {
        CHECK(os64_ui_font_planner(&ui, refuse_after_staging, stage_nop, stage_nop,
                                   NULL) == OS64_FONT_OK);
        CHECK(os64_font_adopt(first, &consumer, 1, NULL) == OS64_FONT_LIMIT);
        CHECK(gStagePanel.bounds.x == live_parent.x && gStagePanel.bounds.y == live_parent.y);
        CHECK(gStageChild.bounds.x == live_child.x && gStageChild.bounds.y == live_child.y);
        CHECK(gStageChild.bounds.w == live_child.w);
        os64_font_set_release(first);
    }

    /* And one that succeeds must put the child inside the parent's NEW
     * rectangle, not the one it is leaving. */
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 20);
    CHECK(set != NULL);
    if (set) {
        CHECK(os64_ui_font_planner(&ui, move_panel_plan, stage_nop, stage_nop,
                                   NULL) == OS64_FONT_OK);
        CHECK(os64_font_adopt(set, &consumer, 1, NULL) == OS64_FONT_OK);
        os64_font_set_release(set);

        CHECK(gStagePanel.bounds.x == 40 && gStagePanel.bounds.y == 20);
        CHECK(gStagePanel.bounds.w == 160);
        CHECK(gStageChild.bounds.x == gStagePanel.bounds.x + ui.theme.pad);
        CHECK(gStageChild.bounds.y == gStagePanel.bounds.y + ui.theme.pad);
        CHECK(gStageChild.bounds.w == gStagePanel.bounds.w - 2 * ui.theme.pad);
        /* The child is inside its parent, which is the whole point. */
        CHECK(gStageChild.bounds.x >= gStagePanel.bounds.x);
        CHECK(gStageChild.bounds.x + gStageChild.bounds.w <=
              gStagePanel.bounds.x + gStagePanel.bounds.w);
    }
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* ── the editor ──────────────────────────────────────────────────────────
 * A textview's caret, its highlight and its glyphs have to be the same
 * measurement of the same bytes. These drive the real widget against a
 * small model and check that they agree — in pixels, because under a
 * proportional face a column is not a distance. */

static const char *const kUiTestDoc[] = {
    "iiii WWWW",             /* two words no column count can reconcile */
    "kerning AV To Ta We",
    "short",
    "caf\xc3\xa9 composed",   /* a two-byte scalar: one cluster, one caret */
};
#define UI_TEST_DOC_LINES 4

static size_t ui_test_doc_count(void *user) { (void)user; return UI_TEST_DOC_LINES; }
static const char *ui_test_doc_line(void *user, size_t i, size_t *len)
{
    (void)user;
    const char *s = i < UI_TEST_DOC_LINES ? kUiTestDoc[i] : "";
    *len = strlen(s);
    return s;
}
static const os64_ui_textbuf_t kUiTestBuf = {
    NULL, ui_test_doc_count, ui_test_doc_line, NULL, NULL, NULL, NULL, NULL
};

static os64_ui_textview_t gUiTestView;

static void ui_test_view_theme(os64_ui_theme_t *t)
{
    memset(t, 0, sizeof(*t));
    t->pad = 6; t->button_h = 20; t->gap = 4;
    t->text_bg = 0xff000000; t->text_fg = 0xffffffff;
    t->text_sel_bg = 0xff0000ff; t->text_sel_fg = 0xffffff00;
    t->text_caret = 0xffff0000;
    t->font_w = OS64_FONT_GLYPH_W; t->font_h = OS64_FONT_GLYPH_H;
}

static void textview_geometry_and_painting(const char *dir)
{
    current = "textview";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);

    os64_ui_textview(&gUiTestView, &kUiTestBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 300, 120};
    os64_ui_set_root(&ui, &gUiTestView.w);
    gUiTestView.w.focused = true;

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(&ui, &consumer);
    CHECK(os64_font_adopt(set, &consumer, 1, NULL) == OS64_FONT_OK);
    os64_font_set_release(set);

    /* Rows come from the DOCUMENT face's line box, and the view's width is
     * a pixel count because there is no column to count. */
    int32_t pitch = os64_ui_font_row_height(&ui, OS64_FONT_ROLE_DOCUMENT);
    CHECK(pitch > 0);
    CHECK(os64_ui_textview_rows(&gUiTestView, &ui.theme) == (120 - 4) / pitch);
    CHECK(os64_ui_textview_width(&gUiTestView) == 300 - 4);

    /* Adoption staged a run for every visible line and for the caret's, so
     * the first paint allocates nothing. */
    CHECK(gUiTestView.row_run_count > 0);
    CHECK(gUiTestView.w.run != NULL);
    canvas_t painted;
    canvas_init(&painted, 0xff000000);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.surf = painted.s;
    unsigned long before = allocations;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    CHECK(allocations == before);
    CHECK(ui_test_ink_left(&painted, 0xff000000) < SURF_W);   /* it drew */

    /* THE CARET AND THE HIT TEST ARE INVERSES. Every legal boundary on a
     * line must round-trip: where the caret sits, and what byte a click
     * there lands on. `iiii WWWW` is the case a column count cannot do. */
    void *run = gUiTestView.w.run;
    CHECK(os64_ui_run_matches(run, kUiTestDoc[0], strlen(kUiTestDoc[0])));
    int32_t prev = -1;
    for (size_t n = 0; n <= strlen(kUiTestDoc[0]); ++n) {
        int32_t x = -1;
        CHECK(os64_ui_run_caret(run, n, false, &x) == OS64_FONT_OK);
        CHECK(x > prev);                     /* strictly increasing */
        prev = x;
        size_t back = SIZE_MAX;
        CHECK(os64_ui_run_hit(run, x, &back) == OS64_FONT_OK);
        CHECK(back == n);
    }
    /* Four narrow letters are not as wide as four wide ones — the whole
     * reason the coordinate had to stop being a column. */
    int32_t after_i = 0, after_w = 0;
    CHECK(os64_ui_run_caret(run, 4, false, &after_i) == OS64_FONT_OK);
    CHECK(os64_ui_run_caret(run, 9, false, &after_w) == OS64_FONT_OK);
    CHECK(after_w - after_i > after_i);

    /* A selection's rectangle is the span between two carets, no wider. */
    os64_gui_rect_t sel;
    CHECK(os64_ui_run_selection(run, 0, 4, &sel) == OS64_FONT_OK);
    CHECK(sel.x == 0 && sel.w == after_i);

    current = "";
}

/* A PARTLY SELECTED LINE IS LIT ONLY WHERE IT IS SELECTED. The rectangle
 * arithmetic was right and still the whole line painted blue, because the
 * first pass filled the line's box with the selection colour; the rest of the
 * line then showed ordinary glyphs on a highlight. Checking the rectangle does
 * not catch that — only looking at the pixels past the selection's end does. */
static void textview_partial_selection_paints_only_the_span(const char *dir)
{
    current = "partial selection";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    os64_ui_textview(&gUiTestView, &kUiTestBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 300, 60};
    os64_ui_set_root(&ui, &gUiTestView.w);

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);

    /* Select "iiii" out of "iiii WWWW": the selection ends mid-line. */
    gUiTestView.sel = true;
    gUiTestView.sel_line = 0; gUiTestView.sel_col = 0;
    gUiTestView.cur_line = 0; gUiTestView.cur_col = 4;

    canvas_t c;
    canvas_init(&c, 0xff000000);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.surf = c.s;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);

    void *run = NULL;
    size_t len;
    const char *ln = ui_test_doc_line(NULL, 0, &len);
    CHECK(os64_ui_run_layout(&ui, OS64_FONT_ROLE_DOCUMENT, ln, len, &run) == OS64_FONT_OK);
    int32_t sel_end = 0, line_end = 0;
    if (run) {
        CHECK(os64_ui_run_caret(run, 4, false, &sel_end) == OS64_FONT_OK);
        CHECK(os64_ui_run_caret(run, len, false, &line_end) == OS64_FONT_OK);
        os64_ui_run_release(run);
    }
    int32_t origin = gUiTestView.w.bounds.x + 2;     /* VIEW_INSET */
    int32_t pitch = os64_ui_font_row_height(&ui, OS64_FONT_ROLE_DOCUMENT);
    int32_t top = gUiTestView.w.bounds.y + 2;

    /* The selected span is lit... */
    int lit_inside = 0, lit_outside = 0;
    for (int32_t y = top; y < top + pitch && y < SURF_H; ++y) {
        for (int32_t x = origin; x < origin + line_end && x < SURF_W; ++x) {
            if (c.px[y * SURF_W + x] != 0xff0000ff)     /* text_sel_bg */
                continue;
            if (x < origin + sel_end) ++lit_inside;
            else ++lit_outside;
        }
    }
    CHECK(lit_inside > 0);
    /* ...and not one pixel of the unselected rest of the line is. */
    CHECK(lit_outside == 0);

    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* THE BYTES AND THE RUN AGREE ON WHERE A LETTER ENDS. Editing keys step by
 * os64_ui_text_step, which reads the bytes and needs no layout; the caret,
 * the click and the highlight come from a run. If the two ever disagreed a
 * caret could sit where no run has a caret, so every boundary the decoder
 * walks must be a caret of the line's run and every caret a boundary — in
 * the outline face and the bitmap one, since the claim is that the face
 * does not matter. The strings are the cases that make a cluster more than
 * one byte, or a byte less than a letter. */
static const char *const kUiTestClusterLines[] = {
    "caf\xc3\xa9",                /* precomposed: 2 bytes, 1 cluster */
    "cafe\xcc\x81",               /* decomposed: e + U+0301, 1 cluster */
    "a\xcc\x81\xcc\x88z",         /* two marks on one base */
    "1\xcc\x81",                  /* a mark after a non-letter stands alone */
    "\xcc\x81x",                  /* ...and so does one that starts a line */
    "\xff\xc3(\xe2\x82",          /* malformed and truncated sequences */
    "tab\there\x01\x7f",          /* a tab and control bytes */
    "\xe2\x82\xac 5 \xe2\x80\x94 x", /* three-byte scalars */
    "",
};

static void ui_test_boundaries_match_runs(os64_ui_t *ui)
{
    for (size_t i = 0; i < sizeof(kUiTestClusterLines) / sizeof(*kUiTestClusterLines); ++i) {
        const char *s = kUiTestClusterLines[i];
        size_t len = strlen(s);
        void *run = NULL;
        CHECK(os64_ui_run_layout(ui, OS64_FONT_ROLE_DOCUMENT, s, len, &run) == OS64_FONT_OK);
        CHECK(run != NULL);
        if (!run)
            continue;
        os64_text_run_view_t rv;
        CHECK(os64_text_run_view((const os64_text_run_t *)run, &rv) == OS64_FONT_OK);

        /* Forward from zero: the decoder's walk IS the caret list. */
        size_t at = 0, n = 0;
        CHECK(rv.caret_count > 0 && rv.carets[0].byte_offset == 0);
        for (;;) {
            CHECK(n < rv.caret_count && rv.carets[n].byte_offset == at);
            if (at == len)
                break;
            size_t next = os64_ui_text_step(s, len, at, true);
            CHECK(next > at);
            if (next <= at)
                break;
            at = next;
            ++n;
        }
        CHECK(n + 1 == rv.caret_count);

        /* Backward from every caret lands on the one before it, and every
         * byte in between snaps to the carets either side. */
        for (size_t c = 1; c < rv.caret_count; ++c) {
            size_t lo = rv.carets[c - 1].byte_offset, hi = rv.carets[c].byte_offset;
            CHECK(os64_ui_text_step(s, len, hi, false) == lo);
            for (size_t b = lo + 1; b < hi; ++b) {
                CHECK(os64_ui_text_snap(s, len, b, false) == lo);
                CHECK(os64_ui_text_snap(s, len, b, true) == hi);
                CHECK(os64_ui_text_step(s, len, b, false) == lo);
                CHECK(os64_ui_text_step(s, len, b, true) == hi);
            }
            CHECK(os64_ui_text_snap(s, len, hi, false) == hi);
        }
        /* The ends clamp. */
        CHECK(os64_ui_text_step(s, len, 0, false) == 0);
        CHECK(os64_ui_text_step(s, len, len, true) == len);
        CHECK(os64_ui_text_snap(s, len, len + 7, false) == len);
        os64_ui_run_release(run);
    }
}

/* C3 — THE SCANNER BACKS UP ONLY TO A CERTAIN EDGE, and must still agree
 * with F2 everywhere. The strings are built from the byte patterns that make
 * cluster edges hard: bases that absorb marks and ones that do not, marks in
 * runs, lone leads, stray continuations, overlong and surrogate encodings,
 * invalid bytes, truncations. For every offset, both directions of step and
 * snap must name the carets the builtin face's run publishes. So must the
 * bounded cluster helpers, measured against the true cluster lengths. */
static const char *const kUiTestTokens[] = {
    "a", "e", "z", " ", "\t", "1", "(", "\x01",
    "\xc3\xa9",                    /* é precomposed */
    "\xcc\x81", "\xcc\x88", "\xcc\x80", "\xcd\xaf",  /* marks at both ends of the range */
    "\xcd\xb0",                    /* just past the marks */
    "\xe2\x82\xac", "\xf0\x9f\x98\x80",
    "\xc3", "\xe2", "\xe2\x82", "\xf0\x9f", "\xf0\x9f\x98",   /* truncated */
    "\x80", "\xbf", "\x8f\x8f",    /* stray continuations */
    "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", /* overlong, surrogate, too big */
    "\xff", "\xf5",
};

static uint32_t ui_test_rng = 0x2545f491u;
static uint32_t ui_test_rand(void)
{
    ui_test_rng ^= ui_test_rng << 13;
    ui_test_rng ^= ui_test_rng >> 17;
    ui_test_rng ^= ui_test_rng << 5;
    return ui_test_rng;
}

static void boundaries_without_the_prefix(void)
{
    current = "anchor scanner";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    unsigned long mismatch = 0, strings = 0;
    char s[200];
    for (int n = 0; n < 4000; ++n) {
        size_t len = 0;
        int tokens = 1 + (int)(ui_test_rand() % 40);
        for (int t = 0; t < tokens; ++t) {
            const char *tok = kUiTestTokens[ui_test_rand() % (sizeof(kUiTestTokens) / sizeof(*kUiTestTokens))];
            size_t tl = strlen(tok);
            if (len + tl > sizeof(s))
                break;
            memcpy(s + len, tok, tl);
            len += tl;
        }
        void *run = NULL;
        if (os64_ui_run_layout(&ui, OS64_FONT_ROLE_DOCUMENT, s, len, &run) != OS64_FONT_OK || !run) {
            ++mismatch;
            continue;
        }
        os64_text_run_view_t rv;
        os64_text_run_view((const os64_text_run_t *)run, &rv);
        ++strings;
        for (size_t p = 0; p <= len; ++p) {
            size_t before = 0, after = len, prev = 0, next = len;
            for (size_t c = 0; c < rv.caret_count; ++c) {
                size_t b = rv.carets[c].byte_offset;
                if (b <= p) before = b;
                if (b >= p && after == len) after = b;
                if (b < p) prev = b;
                if (b > p && next == len) next = b;
            }
            if (os64_ui_text_snap(s, len, p, false) != before ||
                os64_ui_text_snap(s, len, p, true) != after ||
                os64_ui_text_step(s, len, p, false) != prev ||
                os64_ui_text_step(s, len, p, true) != next) {
                if (!mismatch)
                    printf("  first mismatch: string %d offset %zu\n", n, p);
                ++mismatch;
            }
        }
        /* The bounded helpers, at every caret: a limit at the cluster's own
         * length finds it, one byte less reports it as too long. */
        for (size_t c = 0; c + 1 < rv.caret_count; ++c) {
            size_t a = rv.carets[c].byte_offset, b = rv.carets[c + 1].byte_offset;
            size_t got = 0;
            if (!ui_text_cluster_after(s, len, a, b - a, &got) || got != b) ++mismatch;
            if (b - a > 1 && ui_text_cluster_after(s, len, a, b - a - 1, &got)) ++mismatch;
            if (!ui_text_cluster_before(s, len, b, b - a, &got) || got != a) ++mismatch;
            if (b - a > 1 && ui_text_cluster_before(s, len, b, b - a - 1, &got)) ++mismatch;
        }
        os64_ui_run_release(run);
    }
    CHECK(strings == 4000);
    CHECK(mismatch == 0);

    /* A megabyte of marks after a base is ONE cluster. Its edges are found,
     * and the bounded helpers give up after their limit rather than walking
     * it: the costs are counted in bytes the scanner could have touched. */
    size_t big = 1u << 20;
    char *line = malloc(2 + 2 * big + 1);
    line[0] = 'x';
    line[1] = 'e';
    for (size_t i = 0; i < big; ++i) { line[2 + 2 * i] = (char)0xcc; line[3 + 2 * i] = (char)0x81; }
    line[2 + 2 * big] = 'y';
    size_t L = 3 + 2 * big;
    CHECK(os64_ui_text_step(line, L, 1, true) == 2 + 2 * big);        /* over it */
    CHECK(os64_ui_text_step(line, L, 2 + 2 * big, false) == 1);        /* back */
    CHECK(os64_ui_text_snap(line, L, 1000, false) == 1);
    CHECK(os64_ui_text_snap(line, L, 1000, true) == 2 + 2 * big);
    size_t e = 0;
    CHECK(!ui_text_cluster_after(line, L, 1, 4096, &e));
    CHECK(!ui_text_cluster_before(line, L, 2 + 2 * big, 4096, &e));
    CHECK(ui_text_cluster_after(line, L, 2 + 2 * big, 4096, &e) && e == L);
    free(line);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

static void textview_motion(const char *dir)
{
    current = "cluster boundaries";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_boundaries_match_runs(&ui);           /* the builtin face */

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);
    ui_test_boundaries_match_runs(&ui);           /* an outline face */

    /* ...and asking allocates nothing, so no refusal can reach it. */
    unsigned long before = allocations;
    CHECK(os64_ui_text_step("caf\xc3\xa9", 5, 3, true) == 5);
    CHECK(os64_ui_text_step("caf\xc3\xa9", 5, 5, false) == 3);
    CHECK(allocations == before);

    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    CHECK(os64_ui_font_live_bytes(&ui) == 0);
    current = "";
}

/* Keys the way the keyboard sends them: a byte, or a VT100 burst. */
static void ui_test_key(os64_ui_widget_t *w, os64_ui_t *ui, char ascii)
{
    os64_gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = OS64_GUI_EVENT_KEY_DOWN;
    ev.key.ascii = ascii;
    ev.key.scancode = 0x0e;
    w->cls->event(w, ui, &ev);
}
static void ui_test_burst(os64_ui_widget_t *w, os64_ui_t *ui, const char *seq)
{
    ui_test_key(w, ui, 27);
    for (const char *p = seq; *p; ++p)
        ui_test_key(w, ui, *p);
}

/* THE KEYS DO NOT NEED A LAYOUT TO STAY ON LETTERS. Left and Right read the
 * bytes; Up and Down need a pixel, and when the line they are going to
 * cannot be laid out they refuse — the caret, the selection and the scroll
 * all stay exactly where they were, rather than landing on a guessed byte. */
static const char *const kUiTestEncLines[] = { "abc", "\xc3\xa9\xc3\xa9", "z" };
static size_t ui_test_enc_count(void *user) { (void)user; return 3; }
static const char *ui_test_enc_line(void *user, size_t i, size_t *len)
{
    (void)user;
    const char *s = i < 3 ? kUiTestEncLines[i] : "";
    *len = strlen(s);
    return s;
}
static const os64_ui_textbuf_t kUiTestEncBuf = {
    NULL, ui_test_enc_count, ui_test_enc_line, NULL, NULL, NULL, NULL, NULL
};

static void textview_keys_without_layout(const char *dir)
{
    current = "keys without layout";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    os64_ui_textview(&gUiTestView, &kUiTestBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 300, 120};
    os64_ui_set_root(&ui, &gUiTestView.w);
    os64_font_set_t *set = outline_set(os64_ui_font_context(&ui), dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);

    /* "café composed", caret before é, no run anywhere to be had. */
    gUiTestView.cur_line = 3;
    gUiTestView.cur_col = 3;
    ui_test_deny_all = true;
    ui_test_burst(&gUiTestView.w, &ui, "[C");               /* Right */
    CHECK(gUiTestView.cur_col == 5);
    ui_test_burst(&gUiTestView.w, &ui, "[D");               /* Left */
    CHECK(gUiTestView.cur_col == 3);

    /* Up to a line that cannot be laid out: nothing moves. */
    gUiTestView.sel = true;
    gUiTestView.sel_line = 3; gUiTestView.sel_col = 0;
    size_t top = gUiTestView.top;
    ui_test_burst(&gUiTestView.w, &ui, "[A");               /* Up */
    ui_test_deny_all = false;
    CHECK(gUiTestView.cur_line == 3 && gUiTestView.cur_col == 3);
    CHECK(gUiTestView.sel && gUiTestView.sel_line == 3 && gUiTestView.sel_col == 0);
    CHECK(gUiTestView.top == top);

    /* ...and with memory back, the same key goes where the lane says. */
    ui_test_burst(&gUiTestView.w, &ui, "[A");
    CHECK(gUiTestView.cur_line == 2);
    CHECK(!gUiTestView.sel);

    /* A REPAINT OF UNCHANGED TEXT LAYS NOTHING OUT. This view never
     * adopted a face — it was bound — so the first paint finds its row
     * slots; the second must find every run it needs already in them. */
    canvas_t c;
    canvas_init(&c, 0xff000000);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.surf = c.s;
    gUiTestView.w.focused = true;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    unsigned long before = allocations;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    CHECK(allocations == before);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* A textfield edits by CLUSTER. Backspace over "é" used to remove one byte,
 * leaving half a UTF-8 sequence behind — valid bytes in, malformed bytes
 * out, from nothing but a keystroke. */
static void ui_test_field_key(os64_ui_textfield_t *tf, os64_ui_t *ui, char ascii)
{
    ui_test_key(&tf->w, ui, ascii);
}
static void ui_test_field_burst(os64_ui_textfield_t *tf, os64_ui_t *ui, char final)
{
    char seq[3] = { '[', final, '\0' };
    ui_test_burst(&tf->w, ui, seq);
}
static void ui_test_field_delete(os64_ui_textfield_t *tf, os64_ui_t *ui)
{
    ui_test_burst(&tf->w, ui, "[3~");
}

static void textfield_edits_by_cluster(const char *dir)
{
    current = "textfield";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    char buf[64];
    os64_ui_textfield_t tf;
    os64_ui_textfield(&tf, buf, sizeof(buf), NULL, NULL, NULL);
    tf.w.bounds = (os64_gui_rect_t){0, 0, 200, 28};
    os64_ui_set_root(&ui, &tf.w);

    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);

    os64_ui_textfield_set(&ui, &tf, "caf\xc3\xa9");     /* 5 bytes, 4 letters */
    CHECK(tf.len == 5 && tf.cursor == 5);

    ui_test_field_key(&tf, &ui, '\b');
    CHECK(tf.len == 3 && tf.cursor == 3);          /* both bytes of é gone */
    CHECK(memcmp(buf, "caf", 4) == 0);              /* ...and nothing else */

    ui_test_field_key(&tf, &ui, '\b');
    CHECK(tf.len == 2 && memcmp(buf, "ca", 3) == 0);

    /* A click lands on a boundary the run publishes, never inside a letter. */
    os64_ui_textfield_set(&ui, &tf, "\xc3\xa9t\xc3\xa9");   /* é t é */
    void *run = NULL;
    CHECK(os64_ui_run_layout(&ui, OS64_FONT_ROLE_UI, buf, tf.len, &run) == OS64_FONT_OK);
    if (run) {
        int32_t x = 0;
        CHECK(os64_ui_run_caret(run, 2, false, &x) == OS64_FONT_OK);   /* after é */
        os64_ui_run_release(run);
        os64_gui_event_t click;
        memset(&click, 0, sizeof(click));
        click.type = OS64_GUI_EVENT_MOUSE_BUTTON_DOWN;
        click.mouse.button = OS64_GUI_MOUSE_LEFT;
        click.mouse.x = tf.w.bounds.x + 4 + x - tf.left_px;   /* field_inset 4 */
        tf.w.cls->event(&tf.w, &ui, &click);
        CHECK(tf.cursor == 2);
    }

    /* NO LAYOUT, NO PROBLEM. The run for "café" is cold (set while every
     * allocation was refused) and stays unobtainable through the keys, and
     * the edits are still whole letters: they read the bytes, not a run. */
    ui_test_deny_all = true;
    os64_ui_textfield_set(&ui, &tf, "caf\xc3\xa9");
    ui_test_field_key(&tf, &ui, '\b');
    ui_test_deny_all = false;
    CHECK(tf.len == 3 && tf.cursor == 3 && memcmp(buf, "caf", 4) == 0);

    os64_ui_textfield_set(&ui, &tf, "\xc3\xa9t");
    ui_test_deny_all = true;
    tf.cursor = 0;
    ui_test_field_burst(&tf, &ui, 'C');                   /* Right */
    CHECK(tf.cursor == 2);                                 /* past é, not into it */
    tf.cursor = 0;
    ui_test_field_delete(&tf, &ui);
    ui_test_deny_all = false;
    CHECK(tf.len == 1 && tf.cursor == 0 && memcmp(buf, "t", 2) == 0);

    /* A letter typed in front of a combining mark becomes one cluster with
     * it, and the caret goes after that cluster, never between its bytes. */
    os64_ui_textfield_set(&ui, &tf, "\xcc\x81x");
    tf.cursor = 0;
    ui_test_field_key(&tf, &ui, 'e');
    CHECK(tf.len == 4 && memcmp(buf, "e\xcc\x81x", 5) == 0);
    CHECK(tf.cursor == 3);

    /* C2-R7 — A DELETION CAN MAKE A LETTER TOO. In "e1" + U+0301 the digit
     * keeps the e and the accent apart; delete it, by either key, and they
     * are one letter with edges at 0 and 3. The caret goes after it, so the
     * next key takes the whole letter or nothing — never the base alone,
     * which would orphan the accent. */
    os64_ui_textfield_set(&ui, &tf, "e1\xcc\x81");
    tf.cursor = 1;
    ui_test_field_delete(&tf, &ui);
    CHECK(tf.len == 3 && memcmp(buf, "e\xcc\x81", 4) == 0 && tf.cursor == 3);
    ui_test_field_delete(&tf, &ui);                        /* at the end: nothing */
    CHECK(tf.len == 3 && tf.cursor == 3);
    os64_ui_textfield_set(&ui, &tf, "e1\xcc\x81");
    tf.cursor = 2;
    ui_test_field_key(&tf, &ui, '\b');
    CHECK(tf.len == 3 && memcmp(buf, "e\xcc\x81", 4) == 0 && tf.cursor == 3);
    ui_test_field_key(&tf, &ui, '\b');                     /* the whole letter */
    CHECK(tf.len == 0 && tf.cursor == 0);

    /* Malformed bytes that a deletion makes whole: C3, a digit, A9 are
     * three one-byte letters; without the digit they are é. */
    os64_ui_textfield_set(&ui, &tf, "\xc3" "1" "\xa9");
    tf.cursor = 1;
    ui_test_field_delete(&tf, &ui);
    CHECK(tf.len == 2 && memcmp(buf, "\xc3\xa9", 3) == 0 && tf.cursor == 2);
    ui_test_field_key(&tf, &ui, '\b');
    CHECK(tf.len == 0 && tf.cursor == 0);

    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    CHECK(os64_ui_font_live_bytes(&ui) == 0);

    /* Text longer than the field is cut where a letter ends: six bytes of
     * "abcdé" do not fit a field of six (the NUL takes one), and the fifth
     * byte is the first half of é. */
    char small[6];
    os64_ui_textfield_t tiny;
    os64_ui_textfield(&tiny, small, sizeof(small), NULL, NULL, NULL);
    os64_ui_set_root(&ui, &tiny.w);               /* so teardown finds its run */
    os64_ui_textfield_set(&ui, &tiny, "abcd\xc3\xa9");
    CHECK(tiny.len == 4 && memcmp(small, "abcd", 5) == 0);
    os64_ui_textfield_set(&ui, &tiny, "abcde\xcc\x81");   /* e + mark straddles */
    CHECK(tiny.len == 4 && memcmp(small, "abcd", 5) == 0);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* C2-R4 — a font change works the field's scroll out again. The scroll is
 * pixels of the face being retired; after the change, the caret must be
 * inside the field in the new face's pixels, the first paint must lay
 * nothing out, and a refused change must leave the old scroll standing. */
static os64_ui_textfield_t gUiTestField;
static int32_t ui_test_field_staged_w;

static os64_font_status_t ui_test_field_plan(os64_ui_t *ui, void *user, void **out)
{
    (void)ui; (void)user;
    *out = NULL;
    if (ui_test_field_staged_w) {
        os64_gui_rect_t r = gUiTestField.w.bounds;
        r.w = ui_test_field_staged_w;
        os64_ui_widget_stage_bounds(&gUiTestField.w, r);
    }
    return OS64_FONT_OK;
}
static void ui_test_field_plan_done(os64_ui_t *ui, void *user, void *plan)
{
    (void)ui; (void)user; (void)plan;
}
static os64_font_status_t ui_test_refusing_barrier(void *user, void *plan)
{
    (void)user; (void)plan;
    return OS64_FONT_LIMIT;
}

static bool ui_test_caret_shows(os64_ui_t *ui, os64_ui_textfield_t *tf)
{
    int32_t cx = 0;
    if (!tf->w.run || os64_ui_run_caret(tf->w.run, tf->cursor, false, &cx) != OS64_FONT_OK)
        return false;
    int32_t inner = tf->w.bounds.w - 2 * 4;               /* field_inset 4 */
    (void)ui;
    return cx - tf->left_px >= 0 && cx - tf->left_px + 2 <= inner;
}

static os64_font_status_t ui_test_adopt(os64_ui_t *ui, const char *dir,
                                              uint32_t size)
{
    os64_font_set_t *set = outline_set(os64_ui_font_context(ui), dir, "DejaVuSans.ttf", size);
    CHECK(set != NULL);
    if (!set)
        return OS64_FONT_NO_MEMORY;
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(ui, &consumer);
    os64_font_status_t status = os64_font_adopt(set, &consumer, 1, NULL);
    os64_font_set_release(set);
    return status;
}

static void textfield_scroll_follows_the_face(const char *dir)
{
    current = "field scroll";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    char buf[64];
    os64_ui_textfield(&gUiTestField, buf, sizeof(buf), NULL, NULL, NULL);
    gUiTestField.w.bounds = (os64_gui_rect_t){0, 0, 100, 40};   /* 92 inside */
    os64_ui_set_root(&ui, &gUiTestField.w);
    gUiTestField.w.focused = true;
    CHECK(os64_ui_font_planner(&ui, ui_test_field_plan, ui_test_field_plan_done,
                               ui_test_field_plan_done, NULL) == OS64_FONT_OK);

    CHECK(ui_test_adopt(&ui, dir, 8) == OS64_FONT_OK);
    os64_ui_textfield_set(&ui, &gUiTestField, "WWWWWWWW");
    CHECK(gUiTestField.left_px == 0);                     /* fits at 8px */
    CHECK(ui_test_caret_shows(&ui, &gUiTestField));

    /* Bigger: the caret is far past the old scroll's reach. */
    CHECK(ui_test_adopt(&ui, dir, 24) == OS64_FONT_OK);
    CHECK(gUiTestField.left_px > 0);
    CHECK(ui_test_caret_shows(&ui, &gUiTestField));

    /* The first paint in the new face lays nothing out. */
    canvas_t c;
    canvas_init(&c, 0xff000000);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.surf = c.s;
    unsigned long before = allocations;
    gUiTestField.w.cls->paint(&gUiTestField.w, &ctx, &ui.theme);
    CHECK(allocations == before);

    /* Narrower staged bounds: the caret shows in the rectangle it gets. */
    ui_test_field_staged_w = 60;
    CHECK(ui_test_adopt(&ui, dir, 20) == OS64_FONT_OK);
    ui_test_field_staged_w = 0;
    CHECK(gUiTestField.w.bounds.w == 60);
    CHECK(ui_test_caret_shows(&ui, &gUiTestField));

    /* Smaller again, small enough that the whole text fits: none of it is
     * scrolled away. Keeping the retired scroll and moving it only as far
     * as the caret needed would leave the caret at the left edge of an
     * empty-looking field, every letter scrolled off to its left. */
    gUiTestField.w.bounds.w = 100;
    CHECK(ui_test_adopt(&ui, dir, 8) == OS64_FONT_OK);
    CHECK(gUiTestField.left_px == 0);
    CHECK(ui_test_caret_shows(&ui, &gUiTestField));

    /* Refused AFTER the field has staged its new scroll — a barrier says no
     * once every prepare is done — and the field keeps its scroll, its
     * caret and its bytes. */
    CHECK(ui_test_adopt(&ui, dir, 24) == OS64_FONT_OK);
    int32_t left = gUiTestField.left_px;
    size_t cursor = gUiTestField.cursor;
    os64_font_set_t *set = outline_set(os64_ui_font_context(&ui), dir, "DejaVuSans.ttf", 40);
    CHECK(set != NULL);
    if (set) {
        os64_font_consumer_t consumer;
        os64_ui_font_consumer(&ui, &consumer);
        consumer.barrier = ui_test_refusing_barrier;
        CHECK(os64_font_adopt(set, &consumer, 1, NULL) == OS64_FONT_LIMIT);
        os64_font_set_release(set);
    }
    CHECK(gUiTestField.left_staged != left);      /* it DID stage a change */
    CHECK(gUiTestField.left_px == left);
    CHECK(gUiTestField.cursor == cursor);
    CHECK(memcmp(buf, "WWWWWWWW", 9) == 0);
    CHECK(ui_test_caret_shows(&ui, &gUiTestField));

    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* The textview's half of the same rule. Its caret at the end of a line too
 * wide for it at 24px scrolls; at 8px the whole line fits, and so the view
 * shows it from its first letter rather than keeping a retired number. */
static void textview_scroll_follows_the_face(const char *dir)
{
    current = "view scroll";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    os64_ui_textview(&gUiTestView, &kUiTestBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 100, 60};      /* 96 inside */
    os64_ui_set_root(&ui, &gUiTestView.w);
    gUiTestView.cur_line = 0;
    gUiTestView.cur_col = strlen(kUiTestDoc[0]);                   /* "iiii WWWW|" */

    CHECK(ui_test_adopt(&ui, dir, 24) == OS64_FONT_OK);
    CHECK(gUiTestView.left_px > 0);
    int32_t cx = -1;
    CHECK(gUiTestView.w.run != NULL);
    if (gUiTestView.w.run)
        CHECK(os64_ui_run_caret(gUiTestView.w.run, gUiTestView.cur_col, false, &cx) == OS64_FONT_OK);
    CHECK(cx - gUiTestView.left_px >= 0 && cx - gUiTestView.left_px + 2 <= 96);

    CHECK(ui_test_adopt(&ui, dir, 8) == OS64_FONT_OK);
    CHECK(gUiTestView.left_px == 0);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* C2-R6 — A WINDOW WITH NO FACE IS STILL AN EDITOR. When the engine cannot be
 * built, text draws in the bitmap cell, and the caret, the highlight, a click
 * and the scroll must agree with THAT: eight pixels a byte, stopping only
 * where a letter ends. A window WEARING a face whose layout is refused is the
 * other state, and must not borrow the cell's answers — they would put the
 * caret where a different font's letters are. */
static void ui_test_no_engine(os64_ui_t *ui)
{
    deny_countdown = 1;             /* the binding is made; its engine is not */
    CHECK(os64_ui_font_context(ui) == NULL);
    deny_countdown = -1;
    CHECK(os64_ui_font_status(ui) != OS64_FONT_OK);
}
static size_t ui_test_count(const canvas_t *c, uint32_t colour)
{
    size_t n = 0;
    for (size_t i = 0; i < SURF_W * (size_t)SURF_H; ++i)
        n += c->px[i] == colour;
    return n;
}
static void ui_test_click(os64_ui_widget_t *w, os64_ui_t *ui, int32_t x, int32_t y)
{
    os64_gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = OS64_GUI_EVENT_MOUSE_BUTTON_DOWN;
    ev.mouse.button = OS64_GUI_MOUSE_LEFT;
    ev.mouse.x = x;
    ev.mouse.y = y;
    w->cls->event(w, ui, &ev);
}

/* C2-R6 FOLLOW-UP — ONE RENDERING PER PAINT. A binding that could not be
 * made is tried again at the next lookup, so a paint that looked twice could
 * place its caret in bitmap cells and then draw a run, or the reverse. Each
 * painter now chooses once. The test needs no coordinates. A paint that
 * meets ONE refused binding must be pixel-identical to a paint where the
 * binding is refused throughout, which is all bitmap. The repaint after it
 * must be pixel-identical to a paint by a window whose binding already
 * exists, which is all run. `é` is what tells the two renderings apart: two
 * bitmap cells, one glyph in a run. */
enum { UI_TEST_REFUSED_THROUGHOUT, UI_TEST_REFUSED_ONCE, UI_TEST_BOUND };
static canvas_t gUiTestPaint[3];

static void ui_test_paint_view(int mode, canvas_t *first, canvas_t *second)
{
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    os64_ui_textview(&gUiTestView, &kUiTestBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 300, 20};  /* one row */
    os64_ui_set_root(&ui, &gUiTestView.w);
    gUiTestView.w.focused = true;
    gUiTestView.top = 3;                                  /* "café composed" */
    gUiTestView.cur_line = 3; gUiTestView.cur_col = 5;
    gUiTestView.sel = true;
    gUiTestView.sel_line = 3; gUiTestView.sel_col = 3;    /* é selected */
    /* Its row slot exists already, so the binding is the paint's first
     * allocation and the one the single refusal lands on. */
    gUiTestView.row_runs = os64_malloc(sizeof(*gUiTestView.row_runs));
    gUiTestView.row_runs[0] = NULL;
    gUiTestView.row_run_count = 1;
    if (mode == UI_TEST_BOUND)
        CHECK(os64_ui_font_context(&ui) != NULL);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    canvas_init(first, 0xff000000);
    ctx.surf = first->s;
    if (mode == UI_TEST_REFUSED_THROUGHOUT) ui_test_deny_all = true;
    if (mode == UI_TEST_REFUSED_ONCE) deny_countdown = 0;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    ui_test_deny_all = false;
    deny_countdown = -1;
    if (second) {
        canvas_init(second, 0xff000000);
        ctx.surf = second->s;
        gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    }
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
}

static void ui_test_paint_field(int mode, canvas_t *first, canvas_t *second)
{
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    ui.theme.field_fg = 0xffffffff;
    char buf[32];
    os64_ui_textfield_t tf;
    os64_ui_textfield(&tf, buf, sizeof(buf), NULL, NULL, NULL);
    tf.w.bounds = (os64_gui_rect_t){0, 0, 150, 32};
    os64_ui_set_root(&ui, &tf.w);
    tf.w.focused = true;
    ui_test_deny_all = true;               /* set without making a binding */
    os64_ui_textfield_set(&ui, &tf, "caf\xc3\xa9");
    ui_test_deny_all = false;
    if (mode == UI_TEST_BOUND)
        CHECK(os64_ui_font_context(&ui) != NULL);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    canvas_init(first, 0xff000000);
    ctx.surf = first->s;
    if (mode == UI_TEST_REFUSED_THROUGHOUT) ui_test_deny_all = true;
    if (mode == UI_TEST_REFUSED_ONCE) deny_countdown = 0;
    tf.w.cls->paint(&tf.w, &ctx, &ui.theme);
    ui_test_deny_all = false;
    deny_countdown = -1;
    if (second) {
        canvas_init(second, 0xff000000);
        ctx.surf = second->s;
        tf.w.cls->paint(&tf.w, &ctx, &ui.theme);
    }
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
}

static void ui_test_paint_button(int mode, canvas_t *first, canvas_t *second)
{
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    ui.theme.button_fg = 0xffffffff;
    os64_ui_widget_t btn;
    os64_ui_button(&btn, "Caf\xc3\xa9", NULL, NULL);
    btn.bounds = (os64_gui_rect_t){0, 0, 100, 30};
    os64_ui_set_root(&ui, &btn);
    if (mode == UI_TEST_BOUND)
        CHECK(os64_ui_font_context(&ui) != NULL);
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    canvas_init(first, 0xff000000);
    ctx.surf = first->s;
    if (mode == UI_TEST_REFUSED_THROUGHOUT) ui_test_deny_all = true;
    if (mode == UI_TEST_REFUSED_ONCE) deny_countdown = 0;
    btn.cls->paint(&btn, &ctx, &ui.theme);
    ui_test_deny_all = false;
    deny_countdown = -1;
    if (second) {
        canvas_init(second, 0xff000000);
        ctx.surf = second->s;
        btn.cls->paint(&btn, &ctx, &ui.theme);
    }
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
}

static bool ui_test_same(const canvas_t *a, const canvas_t *b)
{
    return memcmp(a->px, b->px, sizeof(a->px)) == 0;
}

static void one_rendering_per_paint(void)
{
    current = "one rendering per paint";
    canvas_t *bitmap = &gUiTestPaint[0], *run = &gUiTestPaint[1], *once = &gUiTestPaint[2];
    static canvas_t after;
    void (*const painters[])(int, canvas_t *, canvas_t *) = {
        ui_test_paint_view, ui_test_paint_field, ui_test_paint_button,
    };
    for (size_t i = 0; i < 3; ++i) {
        painters[i](UI_TEST_REFUSED_THROUGHOUT, bitmap, NULL);
        painters[i](UI_TEST_BOUND, run, NULL);
        CHECK(!ui_test_same(bitmap, run));     /* the renderings really differ */
        painters[i](UI_TEST_REFUSED_ONCE, once, &after);
        CHECK(ui_test_same(once, bitmap));     /* the first paint: all bitmap */
        CHECK(ui_test_same(&after, run));      /* the next: all run */
    }

    /* The view's own numbers, for the record: in that bitmap paint the
     * caret starts after five cells and the highlight ends with é's second
     * cell, 2 + 40 and 2 + 39 with the view's inset of 2. */
    ui_test_paint_view(UI_TEST_REFUSED_ONCE, once, NULL);
    int32_t caret_x = -1, lit_right = -1;
    for (int32_t y = 2; y < 18; ++y)
        for (int32_t x = 2; x < 298; ++x) {
            uint32_t p = once->px[y * SURF_W + x];
            if (p == 0xffff0000 && (caret_x < 0 || x < caret_x)) caret_x = x;
            if (p == 0xff0000ff && x > lit_right) lit_right = x;
        }
    CHECK(caret_x == 2 + 40);
    CHECK(lit_right == 2 + 39);
    current = "";
}

/* THE ROW CONTRACT — a 12-pixel primary row (DejaVu Sans at 9), and a line
 * holding markers taller than it (U+2603 is outside the Western profile, so
 * it draws the 16-pixel missing-glyph marker). Rows keep the primary pitch:
 * the neighbours paint exactly as they would beside an empty line, the
 * caret moves one pitch per line, a click finds its row by the pitch, and
 * the marker line's extent is its run's advance. */
static const char *gUiTestMarkerLines[4] = {
    "above", "\xe2\x98\x83 snow \xe2\x98\x83", "below", "x",
};
static size_t ui_test_marker_count(void *user) { (void)user; return 4; }
static const char *ui_test_marker_line(void *user, size_t i, size_t *len)
{
    (void)user;
    const char *s = i < 4 ? gUiTestMarkerLines[i] : "";
    *len = strlen(s);
    return s;
}
static const os64_ui_textbuf_t kUiTestMarkerBuf = {
    NULL, ui_test_marker_count, ui_test_marker_line, NULL, NULL, NULL, NULL, NULL
};

static int32_t ui_test_caret_top(const canvas_t *c)
{
    for (int32_t y = 0; y < SURF_H; ++y)
        for (int32_t x = 0; x < SURF_W; ++x)
            if (c->px[y * SURF_W + x] == 0xffff0000)       /* text_caret */
                return y;
    return -1;
}

static void rows_keep_the_primary_pitch(const char *dir)
{
    current = "row contract";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    os64_ui_textview(&gUiTestView, &kUiTestMarkerBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 300, 60};
    os64_ui_set_root(&ui, &gUiTestView.w);
    gUiTestView.w.focused = true;
    CHECK(ui_test_adopt(&ui, dir, 9) == OS64_FONT_OK);
    int32_t pitch = os64_ui_font_row_height(&ui, OS64_FONT_ROLE_DOCUMENT);
    CHECK(pitch == 12 && pitch < OS64_FONT_GLYPH_H);       /* shorter than a marker */
    gUiTestView.w.bounds.h = 4 + 4 * pitch;
    CHECK(os64_ui_textview_rows(&gUiTestView, &ui.theme) == 4);

    /* The neighbours: every row but the marker line's paints exactly as it
     * does when that line is empty. */
    canvas_t with, without;
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    gUiTestView.w.focused = false;                          /* no caret */
    canvas_init(&with, 0xff000000);
    ctx.surf = with.s;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    gUiTestMarkerLines[1] = "";
    canvas_init(&without, 0xff000000);
    ctx.surf = without.s;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    gUiTestMarkerLines[1] = "\xe2\x98\x83 snow \xe2\x98\x83";
    int differ_outside = 0, differ_inside = 0;
    for (int32_t y = 0; y < SURF_H; ++y)
        for (int32_t x = 0; x < SURF_W; ++x) {
            bool own = y >= 2 + pitch && y < 2 + 2 * pitch;
            bool diff = with.px[y * SURF_W + x] != without.px[y * SURF_W + x];
            if (diff) { if (own) ++differ_inside; else ++differ_outside; }
        }
    CHECK(differ_inside > 0);                               /* it drew */
    CHECK(differ_outside == 0);                             /* and only there */

    /* Down one line at a time: the caret's row moves one pitch each time,
     * across the marker line as across any other. */
    gUiTestView.w.focused = true;
    os64_ui_textview_goto(&ui, &gUiTestView, 0, 0, false);
    for (int li = 0; li < 4; ++li) {
        canvas_init(&with, 0xff000000);
        ctx.surf = with.s;
        gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
        CHECK(gUiTestView.cur_line == (size_t)li);
        CHECK(ui_test_caret_top(&with) == 2 + li * pitch);
        ui_test_burst(&gUiTestView.w, &ui, "[B");
    }

    /* A click finds its row by the pitch: just inside the marker row, and
     * just inside the row below it. */
    ui_test_click(&gUiTestView.w, &ui, 10, 2 + pitch + 1);
    CHECK(gUiTestView.cur_line == 1);
    ui_test_click(&gUiTestView.w, &ui, 10, 2 + 2 * pitch + 1);
    CHECK(gUiTestView.cur_line == 2);

    /* The marker line's extent is its run's advance, and the row shows
     * exactly that much. */
    int64_t extent = 0, shown = 0;
    bool whole = false;
    size_t len;
    const char *s = ui_test_marker_line(NULL, 1, &len);
    CHECK(os64_ui_textview_line_width(&ui, &gUiTestView, s, len, &extent, &whole) == OS64_FONT_OK);
    CHECK(whole && extent > 0);
    CHECK(os64_ui_textview_row_width(&ui, &gUiTestView, 1, &shown) == OS64_FONT_OK);
    CHECK(shown == extent);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* C3 — A LONG LINE IS SHOWN THROUGH A WINDOW. A three-megabyte line, a
 * line holding a 1.4 MB cluster (one e and 700,000 combining acutes) between
 * ordinary letters, and a line that is nothing but such a cluster, in a
 * read-only view. Most checks use a 64-byte window so the windows, their
 * ends and their decorations fit the test canvas; the rest use the default
 * window, or one past F2's run limit. */
#define UI_TEST_LONG_LINES 5
static char *gUiTestLong[UI_TEST_LONG_LINES];
static size_t gUiTestLongLen[UI_TEST_LONG_LINES];
static size_t ui_test_long_count(void *user) { (void)user; return UI_TEST_LONG_LINES; }
static const char *ui_test_long_line(void *user, size_t i, size_t *len)
{
    (void)user;
    *len = i < UI_TEST_LONG_LINES ? gUiTestLongLen[i] : 0;
    return i < UI_TEST_LONG_LINES ? gUiTestLong[i] : "";
}
static const os64_ui_textbuf_t kUiTestLongBuf = {
    NULL, ui_test_long_count, ui_test_long_line, NULL, NULL, NULL, NULL, NULL
};
#define UI_TEST_GIANT_MARKS 700000u

static void ui_test_long_model(void)
{
    static const char word[] = "caf\xc3\xa9 words ";   /* 12 bytes, é in it */
    gUiTestLong[0] = malloc(5);
    memcpy(gUiTestLong[0], "short", 5);
    gUiTestLongLen[0] = 5;
    size_t n1 = 3u << 20;
    gUiTestLong[1] = malloc(n1);
    for (size_t i = 0; i < n1; ++i)
        gUiTestLong[1][i] = word[i % 12];
    gUiTestLongLen[1] = n1 - n1 % 12;                  /* ends between words */
    size_t n2 = 4 + 2 * UI_TEST_GIANT_MARKS + 3;
    gUiTestLong[2] = malloc(n2);
    memcpy(gUiTestLong[2], "abce", 4);
    for (size_t i = 0; i < UI_TEST_GIANT_MARKS; ++i) {
        gUiTestLong[2][4 + 2 * i] = (char)0xcc;
        gUiTestLong[2][5 + 2 * i] = (char)0x81;
    }
    memcpy(gUiTestLong[2] + 4 + 2 * UI_TEST_GIANT_MARKS, "xyz", 3);
    gUiTestLongLen[2] = n2;
    gUiTestLong[3] = malloc(20000);
    memset(gUiTestLong[3], 'x', 20000);
    gUiTestLongLen[3] = 20000;
    size_t n4 = 1 + 2 * UI_TEST_GIANT_MARKS;             /* one cluster, all of it */
    gUiTestLong[4] = malloc(n4);
    gUiTestLong[4][0] = 'e';
    for (size_t i = 0; i < UI_TEST_GIANT_MARKS; ++i) {
        gUiTestLong[4][1 + 2 * i] = (char)0xcc;
        gUiTestLong[4][2 + 2 * i] = (char)0x81;
    }
    gUiTestLongLen[4] = n4;
}

static size_t ui_test_run_bytes(void *run)
{
    os64_text_run_view_t rv;
    if (!run || os64_text_run_view((const os64_text_run_t *)run, &rv) != OS64_FONT_OK)
        return 0;
    return rv.byte_count;
}

/* Is the caret on a letter edge of its line? */
static bool ui_test_caret_legal(const os64_ui_textview_t *tv)
{
    size_t len;
    const char *s = tv->buf->line(tv->buf->user, tv->cur_line, &len);
    return tv->cur_col <= len && os64_ui_text_snap(s, len, tv->cur_col, false) == tv->cur_col;
}

static void ui_test_long_view(os64_ui_t *ui, size_t window)
{
    memset(ui, 0, sizeof(*ui));
    ui_test_view_theme(&ui->theme);
    ui->theme.scroll_track = 0xff404040;
    ui->theme.scroll_thumb = 0xffc0c0c0;
    os64_ui_textview(&gUiTestView, &kUiTestLongBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 300, 64};
    gUiTestView.window_bytes = window;
    os64_ui_set_root(ui, &gUiTestView.w);
    gUiTestView.w.focused = true;
}

static void long_lines_are_windowed(const char *dir)
{
    current = "long lines";
    ui_test_long_model();
    os64_ui_t ui;
    canvas_t c;
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    size_t len1 = gUiTestLongLen[1];

    /* The bitmap cell first: its widths are exact, so the row's geometry
     * can be checked to the pixel. A window of 64 bytes from the line's
     * start is 64 cells, with a "more" decoration of one row's height
     * after it. The short line has no decoration at all. */
    ui_test_long_view(&ui, 64);
    ui_test_no_engine(&ui);
    int64_t width = 0;
    CHECK(os64_ui_textview_row_width(&ui, &gUiTestView, 0, &width) == OS64_FONT_OK);
    CHECK(width == 5 * 8);
    CHECK(os64_ui_textview_row_width(&ui, &gUiTestView, 3, &width) == OS64_FONT_OK);
    CHECK(width == 64 * 8 + 16);
    bool whole = true;
    int64_t unknown = -7;
    CHECK(os64_ui_textview_line_width(&ui, &gUiTestView, gUiTestLong[3], 20000,
                                      &unknown, &whole) == OS64_FONT_OK);
    CHECK(!whole && unknown == -7);                   /* unknown, not zero */
    /* The cluster too long to lay out stands as a placeholder two squares
     * wide, with a "more" of one outside it because the line goes on past
     * it: before it, "abc", the placeholder and the "more"; after it, the
     * "more", the placeholder and "xyz". */
    os64_ui_textview_goto(&ui, &gUiTestView, 2, 0, false);
    CHECK(os64_ui_textview_row_width(&ui, &gUiTestView, 2, &width) == OS64_FONT_OK);
    CHECK(width == 3 * 8 + 32 + 16);
    os64_ui_textview_goto(&ui, &gUiTestView, 2, 4 + 2 * UI_TEST_GIANT_MARKS, false);
    CHECK(gUiTestView.cur_col == 4 + 2 * UI_TEST_GIANT_MARKS);
    CHECK(os64_ui_textview_row_width(&ui, &gUiTestView, 2, &width) == OS64_FONT_OK);
    CHECK(width == 16 + 32 + 3 * 8);
    /* A line that is one long cluster has nothing beyond it at either end
     * that anything knows of, so its placeholder stands alone. */
    os64_ui_textview_goto(&ui, &gUiTestView, 4, 0, false);
    CHECK(os64_ui_textview_row_width(&ui, &gUiTestView, 4, &width) == OS64_FONT_OK);
    CHECK(width == 32);
    os64_ui_textview_goto(&ui, &gUiTestView, 4, gUiTestLongLen[4], false);
    CHECK(gUiTestView.cur_col == gUiTestLongLen[4]);
    CHECK(os64_ui_textview_row_width(&ui, &gUiTestView, 4, &width) == OS64_FONT_OK);
    CHECK(width == 32);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);

    /* A face from here on. */
    ui_test_long_view(&ui, 64);
    os64_font_set_t *set = outline_set(os64_ui_font_context(&ui), dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);

    /* The caret deep in the long line: its row lays out a window around
     * it, and the window's run holds no more than the window. */
    os64_ui_textview_goto(&ui, &gUiTestView, 1, 1008, false);   /* a word's start */
    CHECK(gUiTestView.cur_line == 1 && gUiTestView.cur_col == 1008);
    CHECK(gUiTestView.win_line == 1);
    CHECK(gUiTestView.win_from <= 1008 && 1008 - gUiTestView.win_from <= 32);
    size_t bytes = ui_test_run_bytes(gUiTestView.w.run);
    CHECK(bytes > 0 && bytes <= 64);

    /* Painted with the line at the top: a "more" decoration at each end
     * of the row, in the window's chrome colours, not the text's. */
    gUiTestView.top = 1;
    gUiTestView.left_px = 0;
    canvas_init(&c, 0xff000000);
    ctx.surf = c.s;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    int lead_track = 0, lead_arrow = 0;
    for (int32_t y = 2; y < 18; ++y)
        for (int32_t x = 2; x < 2 + 16; ++x) {
            lead_track += c.px[y * SURF_W + x] == 0xff404040;
            lead_arrow += c.px[y * SURF_W + x] == 0xffc0c0c0;
        }
    CHECK(lead_track > 0 && lead_arrow > 0);

    /* Right across the window's end, forty letters: every step lands on a
     * letter edge inside the window, and the window moves to keep up. */
    size_t from = gUiTestView.win_from;
    bool moved = false;
    for (int i = 0; i < 40; ++i) {
        size_t before = gUiTestView.cur_col;
        ui_test_burst(&gUiTestView.w, &ui, "[C");
        CHECK(gUiTestView.cur_col > before);
        CHECK(ui_test_caret_legal(&gUiTestView));
        CHECK(gUiTestView.cur_col >= gUiTestView.win_from &&
              gUiTestView.cur_col - gUiTestView.win_from <= 64);
        moved |= gUiTestView.win_from != from;
    }
    CHECK(moved);
    CHECK(ui_test_run_bytes(gUiTestView.w.run) <= 64);

    /* And back, forty letters Left, the same way. */
    from = gUiTestView.win_from;
    moved = false;
    for (int i = 0; i < 40; ++i) {
        size_t before = gUiTestView.cur_col;
        ui_test_burst(&gUiTestView.w, &ui, "[D");
        CHECK(gUiTestView.cur_col < before);
        CHECK(ui_test_caret_legal(&gUiTestView));
        CHECK(gUiTestView.cur_col >= gUiTestView.win_from &&
              gUiTestView.cur_col - gUiTestView.win_from <= 64);
        moved |= gUiTestView.win_from != from;
    }
    CHECK(moved);

    /* A click on the leading decoration puts the caret at the window's
     * start, against the omitted text, and the window moves back past it. */
    from = gUiTestView.win_from;
    gUiTestView.left_px = 0;
    ui_test_click(&gUiTestView.w, &ui, 2 + 4, 2 + 8);
    CHECK(gUiTestView.cur_line == 1 && gUiTestView.cur_col == from);
    CHECK(gUiTestView.win_from < from);

    /* End and Home take the caret, and the window, to the line's ends. */
    ui_test_burst(&gUiTestView.w, &ui, "[F");
    CHECK(gUiTestView.cur_col == len1);
    CHECK(len1 - gUiTestView.win_from <= 64);
    ui_test_burst(&gUiTestView.w, &ui, "[H");
    CHECK(gUiTestView.cur_col == 0 && gUiTestView.win_from == 0);

    /* A SELECTION MAY SPAN OMITTED BYTES. Selected from near the line's
     * start to near its end, with the caret at the end: the row shows the
     * window around the caret, and its leading decoration, standing for the
     * selected bytes before it, is lit. */
    os64_ui_textview_select(&ui, &gUiTestView, 1, 12, 1, len1 - 12);
    CHECK(gUiTestView.sel && gUiTestView.cur_col == len1 - 12);
    gUiTestView.top = 1;
    gUiTestView.left_px = 0;
    canvas_init(&c, 0xff000000);
    ctx.surf = c.s;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    int lead_lit = 0;
    for (int32_t y = 2; y < 18; ++y)
        for (int32_t x = 2; x < 2 + 16; ++x)
            lead_lit += c.px[y * SURF_W + x] == 0xff0000ff;       /* text_sel_bg */
    CHECK(lead_lit > 0);
    gUiTestView.sel = false;

    /* A CLUSTER LONGER THAN A WINDOW is never laid out: at its start the
     * row ends in a placeholder, Right crosses the whole of it in one step,
     * and Left comes back to its start. The caret stops only at its ends. */
    os64_ui_textview_goto(&ui, &gUiTestView, 2, 0, false);
    for (int i = 0; i < 3; ++i)
        ui_test_burst(&gUiTestView.w, &ui, "[C");
    CHECK(gUiTestView.cur_col == 3);
    ui_test_burst(&gUiTestView.w, &ui, "[C");
    CHECK(gUiTestView.cur_col == 4 + 2 * UI_TEST_GIANT_MARKS);
    CHECK(gUiTestView.win_from == gUiTestView.cur_col);   /* just after it */
    CHECK(ui_test_run_bytes(gUiTestView.w.run) <= 64);
    gUiTestView.top = 2;
    gUiTestView.left_px = 0;
    canvas_init(&c, 0xff000000);
    ctx.surf = c.s;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    int arrow = 0, box = 0;              /* "more" outside, the outline inside */
    for (int32_t y = 2; y < 18; ++y) {
        for (int32_t x = 2; x < 2 + 16; ++x)
            arrow += c.px[y * SURF_W + x] == 0xffc0c0c0;
        for (int32_t x = 2 + 16; x < 2 + 16 + 32; ++x)
            box += c.px[y * SURF_W + x] == 0xffc0c0c0;
    }
    CHECK(arrow > 0 && box > 0);
    ui_test_burst(&gUiTestView.w, &ui, "[D");
    CHECK(gUiTestView.cur_col == 3);

    /* Up and Down between long lines land on letter edges of the rows
     * they arrive in. */
    os64_ui_textview_goto(&ui, &gUiTestView, 1, len1 - 60, false);
    ui_test_burst(&gUiTestView.w, &ui, "[A");
    CHECK(gUiTestView.cur_line == 0 && ui_test_caret_legal(&gUiTestView));
    ui_test_burst(&gUiTestView.w, &ui, "[B");
    CHECK(gUiTestView.cur_line == 1 && ui_test_caret_legal(&gUiTestView));
    ui_test_burst(&gUiTestView.w, &ui, "[B");
    CHECK(gUiTestView.cur_line == 2 && ui_test_caret_legal(&gUiTestView));
    CHECK(gUiTestView.cur_col <= 3);            /* not inside the long cluster */

    /* A font change with long lines on screen lays out windows, not
     * lines: it succeeds, and the caret's run holds no more than a window. */
    os64_ui_textview_goto(&ui, &gUiTestView, 1, len1 / 2, false);
    gUiTestView.top = 0;
    CHECK(ui_test_adopt(&ui, dir, 20) == OS64_FONT_OK);
    CHECK(ui_test_run_bytes(gUiTestView.w.run) <= 64);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);

    /* THE DEFAULT WINDOW. End on the three-megabyte line lays out at most
     * OS64_UI_TEXTVIEW_WINDOW bytes, and a font change with it on screen
     * succeeds too. */
    ui_test_long_view(&ui, 0);
    CHECK(ui_test_adopt(&ui, dir, 16) == OS64_FONT_OK);
    os64_ui_textview_goto(&ui, &gUiTestView, 1, 0, false);
    ui_test_burst(&gUiTestView.w, &ui, "[F");
    CHECK(gUiTestView.cur_col == len1);
    bytes = ui_test_run_bytes(gUiTestView.w.run);
    CHECK(bytes > OS64_UI_TEXTVIEW_WINDOW / 2 && bytes <= OS64_UI_TEXTVIEW_WINDOW);
    CHECK(ui_test_adopt(&ui, dir, 24) == OS64_FONT_OK);
    CHECK(ui_test_run_bytes(gUiTestView.w.run) <= OS64_UI_TEXTVIEW_WINDOW);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);

    /* A WINDOW THE RUN CALLS TOO BIG is halved, not left blank: a window of
     * a megabyte and a half is past F2's limit, so the row lays out half of
     * that and the caret still has a place. */
    ui_test_long_view(&ui, 1536u << 10);
    set = outline_set(os64_ui_font_context(&ui), dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (set) {
        CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);
        os64_font_set_release(set);
    }
    os64_ui_textview_goto(&ui, &gUiTestView, 1, len1 / 2, false);
    CHECK(os64_ui_textview_row_width(&ui, &gUiTestView, 1, &width) == OS64_FONT_OK);
    bytes = ui_test_run_bytes(gUiTestView.w.run);
    CHECK(bytes > 0 && bytes <= (768u << 10));
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);

    for (int i = 0; i < UI_TEST_LONG_LINES; ++i)
        free(gUiTestLong[i]);
    current = "";
}

static void editors_without_a_face(const char *dir)
{
    current = "no face";
    canvas_t c;
    os64_draw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* THE FIELD. Inset 4, caret row 8..24 in a 32-pixel field. */
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    char buf[64];
    os64_ui_textfield_t tf;
    os64_ui_textfield(&tf, buf, sizeof(buf), NULL, NULL, NULL);
    tf.w.bounds = (os64_gui_rect_t){0, 0, 150, 32};          /* 142 inside */
    os64_ui_set_root(&ui, &tf.w);
    tf.w.focused = true;
    ui_test_no_engine(&ui);

    os64_ui_textfield_set(&ui, &tf, "abc");
    canvas_init(&c, 0xff000000);
    ctx.surf = c.s;
    tf.w.cls->paint(&tf.w, &ctx, &ui.theme);
    CHECK(c.px[16 * SURF_W + 4 + 24] == ui.theme.text_caret);  /* after 3 cells */

    /* A click inside é's two cells goes to the nearer of its edges. */
    os64_ui_textfield_set(&ui, &tf, "\xc3\xa9t");            /* edges 0, 16, 24 */
    ui_test_click(&tf.w, &ui, 4 + 9, 16);
    CHECK(tf.cursor == 2);
    ui_test_click(&tf.w, &ui, 4 + 5, 16);
    CHECK(tf.cursor == 0);

    /* Motion and deletion take the whole letter. */
    ui_test_field_burst(&tf, &ui, 'C');                       /* Right */
    CHECK(tf.cursor == 2);
    ui_test_field_key(&tf, &ui, '\b');
    CHECK(tf.len == 1 && tf.cursor == 0 && memcmp(buf, "t", 2) == 0);

    /* A long field scrolls to keep its caret, counted in cells: forty W's
     * put it at 320, and 142 pixels show. */
    os64_ui_textfield_set(&ui, &tf, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    CHECK(tf.left_px == 320 - 142 + 2);
    canvas_init(&c, 0xff000000);
    ctx.surf = c.s;
    tf.w.cls->paint(&tf.w, &ctx, &ui.theme);
    CHECK(c.px[16 * SURF_W + 4 + 320 - tf.left_px] == ui.theme.text_caret);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);

    /* THE VIEW. Inset 2, rows of 16. */
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    os64_ui_textview(&gUiTestView, &kUiTestBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 300, 60};  /* three rows */
    os64_ui_set_root(&ui, &gUiTestView.w);
    gUiTestView.w.focused = true;
    ui_test_no_engine(&ui);

    /* "iiii WWWW", its first four letters selected and the caret after
     * them: the highlight is exactly four cells and the caret is at the
     * fifth. */
    gUiTestView.sel = true;
    gUiTestView.sel_line = 0; gUiTestView.sel_col = 0;
    gUiTestView.cur_line = 0; gUiTestView.cur_col = 4;
    canvas_init(&c, 0xff000000);
    ctx.surf = c.s;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    int lit_in = 0, lit_out = 0;
    for (int32_t y = 2; y < 18; ++y)
        for (int32_t x = 0; x < SURF_W; ++x)
            if (c.px[y * SURF_W + x] == ui.theme.text_sel_bg) {
                if (x >= 2 && x < 2 + 32) ++lit_in;
                else ++lit_out;
            }
    CHECK(lit_in > 0 && lit_out == 0);
    CHECK(c.px[10 * SURF_W + 2 + 32] == ui.theme.text_caret);

    /* Clicks inside é's cells ("café composed", é across x 24..40) go to the
     * nearer edge, 3 or 5 — never 4. */
    gUiTestView.top = 1;                                      /* its row is 2 */
    ui_test_click(&gUiTestView.w, &ui, 2 + 31, 2 + 2 * 16 + 8);
    CHECK(gUiTestView.cur_line == 3 && gUiTestView.cur_col == 3);
    ui_test_click(&gUiTestView.w, &ui, 2 + 33, 2 + 2 * 16 + 8);
    CHECK(gUiTestView.cur_line == 3 && gUiTestView.cur_col == 5);
    ui_test_burst(&gUiTestView.w, &ui, "[D");                 /* Left */
    CHECK(gUiTestView.cur_col == 3);

    /* Up and Down hold a lane in cells too. End on "abc" remembers x=24,
     * which falls between the second é's edges at 16 and 32: a tie, and it
     * goes to the later edge as a run's does. Never 3, inside a letter. */
    gUiTestView.buf = &kUiTestEncBuf;
    gUiTestView.top = 0;
    gUiTestView.sel = false;
    gUiTestView.cur_line = 0; gUiTestView.cur_col = 0;
    ui_test_burst(&gUiTestView.w, &ui, "[F");                 /* End */
    CHECK(gUiTestView.cur_col == 3 && gUiTestView.goal_x == 24);
    ui_test_burst(&gUiTestView.w, &ui, "[B");                 /* Down */
    CHECK(gUiTestView.cur_line == 1 && gUiTestView.cur_col == 4);

    /* A line wider than the view scrolls to its caret, in cells:
     * "kerning AV To Ta We" is 19 of them, 152 pixels, in 96. */
    gUiTestView.buf = &kUiTestBuf;
    gUiTestView.w.bounds.w = 100;
    gUiTestView.cur_line = 1; gUiTestView.cur_col = 0;
    gUiTestView.left_px = 0;
    ui_test_burst(&gUiTestView.w, &ui, "[F");
    CHECK(gUiTestView.left_px == 152 - 96 + 2);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);

    /* THE OTHER STATE: a face installed, its layout refused. Nothing is
     * drawn from the cell — no caret, no highlight — in either editor. */
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    os64_ui_textfield(&tf, buf, sizeof(buf), NULL, NULL, NULL);
    tf.w.bounds = (os64_gui_rect_t){0, 0, 150, 32};
    os64_ui_set_root(&ui, &tf.w);
    tf.w.focused = true;
    os64_font_set_t *set = outline_set(os64_ui_font_context(&ui), dir, "DejaVuSans.ttf", 16);
    CHECK(set != NULL);
    if (!set) { current = ""; return; }
    CHECK(os64_ui_font_bind(&ui, set) == OS64_FONT_OK);
    os64_font_set_release(set);
    ui_test_deny_all = true;
    os64_ui_textfield_set(&ui, &tf, "abc");                    /* never laid out */
    canvas_init(&c, 0xff000000);
    ctx.surf = c.s;
    tf.w.cls->paint(&tf.w, &ctx, &ui.theme);
    ui_test_deny_all = false;
    CHECK(ui_test_count(&c, ui.theme.text_caret) == 0);

    os64_ui_textview(&gUiTestView, &kUiTestBuf, NULL, NULL, NULL);
    gUiTestView.w.bounds = (os64_gui_rect_t){0, 0, 300, 60};
    os64_ui_set_root(&ui, &gUiTestView.w);
    gUiTestView.w.focused = true;
    gUiTestView.sel = true;
    gUiTestView.sel_line = 0; gUiTestView.sel_col = 0;
    gUiTestView.cur_line = 0; gUiTestView.cur_col = 4;
    ui_test_deny_all = true;
    canvas_init(&c, 0xff000000);
    ctx.surf = c.s;
    gUiTestView.w.cls->paint(&gUiTestView.w, &ctx, &ui.theme);
    ui_test_deny_all = false;
    CHECK(ui_test_count(&c, ui.theme.text_caret) == 0);
    CHECK(ui_test_count(&c, ui.theme.text_sel_bg) == 0);
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    current = "";
}

/* Q3 — automatic and explicit heights, across attach, relayout and a font
 * change. A padded control is in the mix so the check is not satisfied by
 * the builtin face happening to reproduce a 16px label. */
static void height_policy(const char *dir)
{
    current = "height policy";
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.theme.pad = 6;
    ui.theme.button_h = 20;
    ui.theme.gap = 4;

    os64_ui_widget_t root, lbl, btn;
    os64_ui_panel(&root);
    root.bounds = (os64_gui_rect_t){0, 0, 200, 400};
    os64_ui_label(&lbl, "auto");
    os64_ui_button(&btn, "padded", NULL, NULL);
    os64_ui_add_child(&root, &lbl);
    os64_ui_add_child(&root, &btn);
    os64_ui_set_root(&ui, &root);

    /* Attach stamps what each widget NEEDS, and leaves the rectangle alone. */
    CHECK(lbl.auto_h && btn.auto_h);
    CHECK(lbl.natural_h == OS64_FONT_GLYPH_H);
    CHECK(btn.natural_h == OS64_FONT_GLYPH_H + 2 * ui.theme.pad);
    CHECK(btn.natural_h > lbl.natural_h);   /* the padding is really there */

    os64_ui_stack_vertical(&ui, &root);
    CHECK(lbl.bounds.h == lbl.natural_h);
    CHECK(btn.bounds.h == btn.natural_h);

    /* An application height is the application's, and survives a relayout. */
    os64_ui_widget_fixed_height(&lbl, 41);
    CHECK(!lbl.auto_h);
    os64_ui_stack_vertical(&ui, &root);
    CHECK(lbl.bounds.h == 41);

    /* ...and a font change. The auto one follows the face; the explicit one
     * is not repaired, re-derived, or quietly overwritten. */
    os64_text_context_t *text = os64_ui_font_context(&ui);
    os64_font_set_t *set = outline_set(text, dir, "DejaVuSans.ttf", 28);
    CHECK(set != NULL);
    if (set) {
        os64_font_consumer_t consumer;
        os64_ui_font_consumer(&ui, &consumer);
        CHECK(os64_font_adopt(set, &consumer, 1, NULL) == OS64_FONT_OK);
        os64_font_set_release(set);
        os64_ui_stack_vertical(&ui, &root);
        CHECK(btn.natural_h > OS64_FONT_GLYPH_H + 2 * ui.theme.pad);
        CHECK(btn.bounds.h == btn.natural_h);
        CHECK(lbl.bounds.h == 41);
        CHECK(!lbl.auto_h);
    }
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
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
    measurement_failure_travels(&ui, dir);
    marker_stays_inside_the_row(&ui, dir);
    adoption(&ui, dir);
    registration_reports_failure(dir);
    release_refuses_while_busy(dir);
    shared_and_foreign_contexts(dir);
    height_policy(dir);
    tab_interval_failure_travels(dir);
    button_paints_from_its_run(dir);
    list_stages_its_candidate_rows(dir);
    changed_caption_is_not_placed_from_nothing(dir);
    staged_children_follow_their_staged_parent(dir);
    textview_geometry_and_painting(dir);
    textview_partial_selection_paints_only_the_span(dir);
    textview_motion(dir);
    boundaries_without_the_prefix();
    textview_keys_without_layout(dir);
    textfield_edits_by_cluster(dir);
    textfield_scroll_follows_the_face(dir);
    textview_scroll_follows_the_face(dir);
    editors_without_a_face(dir);
    one_rendering_per_paint();
    rows_keep_the_primary_pitch(dir);
    long_lines_are_windowed(dir);
#else
    (void)slurp;
    (void)rows_touched_outside;
    (void)kProbeClass;
    (void)gProbe;
    (void)stamped_row;
    (void)probe_metrics;
#endif

    /* Nothing owed when the window goes. */
    ui.root = NULL;
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    CHECK(os64_ui_font_live_bytes(&ui) == 0);
    CHECK(ui.font == NULL);

    printf("test_ui_text_host: %lu checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
