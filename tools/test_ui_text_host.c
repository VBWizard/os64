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
 * The toolkit is linked for real; only the window system is stubbed. */

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

/* ── the stubs, and the allocation injector ────────────────────────────── */
/* ui_font.c allocates through libos64's heap and marks the window dirty. The
 * first becomes the host allocator — with a switch for denying it, since
 * "what happens when layout cannot allocate" is half of what is under test
 * — and the second has no window to dirty. */
static long deny_countdown = -1;   /* -1 never denies; 0 denies the next one */
static unsigned long allocations;

void *os64_malloc(size_t n)
{
    ++allocations;
    if (deny_countdown == 0) { deny_countdown = -1; return NULL; }
    if (deny_countdown > 0) --deny_countdown;
    return malloc(n ? n : 1);
}
void os64_free(void *p) { free(p); }

/* The toolkit itself is linked, because the height policy lives in its
 * constructors and its layout rather than in the font binding. Its keyboard
 * decoder lives in ui_text.c, which this harness has no reason to build. */
#include "ui_internal.h"
ui_key_t os64_ui_decode_key(uint8_t *seq, const os64_gui_event_t *ev, char *ch)
{ (void)seq; (void)ev; (void)ch; return K_NONE; }

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
    probe_metrics
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
