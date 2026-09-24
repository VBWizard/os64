// fonttest.c — the font backend, running in os64.
//
// The host harness (tools/test_freetype_host.py) is where the malformed
// fonts and the allocation failures are hunted, because a sanitizer can
// watch those and QEMU cannot. THIS fixture answers the question the host
// cannot: does the engine work HERE — linked as /lib/libfreetype.so, on
// os64's allocator, at ring 3, against files read off the real filesystem.
//
// It does three things, in this order:
//
//   1. ASSERTS. Both formats open, metrics are plausible, a monospace face
//      has equal advances where a proportional one does not, kerning is the
//      value the fixtures' tables say it is, and every handle is released
//      with the accounting back where it started. Failures name their step
//      in the exit code, per the house convention.
//
//   2. PRINTS A SPECIMEN ON THE TERMINAL. A glyph's coverage mask drawn as
//      characters, so a TEXT boot — where most of the suite runs — still
//      shows a human that real outlines were rasterized, not just that a
//      number came back.
//
//   3. DRAWS ONE, IF THERE IS A SCREEN. A window of antialiased text at
//      several sizes, blended into the canvas: bearings honoured, masks
//      clipped, and NO opaque per-glyph cell — a background painted once,
//      behind the whole line, so overhangs and kerned neighbours survive.
//      On a boot without the GUI this step SKIPS, because "I cannot run
//      here" is not "I failed".
//
// Run it by hand: /tests/fonttest [font-directory]

#include "os64/os64.h"
#include "os64/font_backend.h"
#include "os64/slurp.h"
#include "os64/mem.h"
#include "os64/draw.h"
#include "os64/gui.h"
#include "os64/str.h"

// Exit codes name the failing step.
#define FT_OK             0
#define FT_NO_BACKEND     2
#define FT_NO_FIXTURE     3
#define FT_ENGINE         4
#define FT_OPEN           5
#define FT_INFO           6
#define FT_LOOKUP         7
#define FT_RENDER         8
#define FT_KERN           9
#define FT_LIFETIME      10
#define FT_ACCOUNTING    11

#define FONT_DIR_DEFAULT  "/tests/fonts"
#define FONT_CAP          (8u * 1024u * 1024u)

extern int os64_font_runtime_test(void);

static const os64_font_backend_t *ft;
static int failures;

// ── the allocator the engine runs on ────────────────────────────────────────
// os64's heap, wrapped so the fixture can count what the engine asked for and
// prove it all came back. os64_free takes no size, so the size the contract
// hands us is checked and then dropped.

typedef struct
{
    size_t live;
    size_t peak;
    long   allocations;
    int    size_mismatches;
} accounting_t;

typedef struct
{
    size_t size;
    size_t pad;          // keeps the payload 16-byte aligned
} block_t;

static void *fixture_alloc(void *context, size_t bytes)
{
    accounting_t *a = context;
    block_t *b;

    if (bytes == 0)
        return NULL;

    b = os64_malloc(sizeof(*b) + bytes);
    if (b == NULL)
        return NULL;

    b->size = bytes;
    a->allocations++;
    a->live += bytes;
    if (a->live > a->peak)
        a->peak = a->live;
    return (unsigned char *)b + sizeof(*b);
}

static void fixture_free(void *context, void *allocation, size_t bytes)
{
    accounting_t *a = context;
    block_t *b;

    if (allocation == NULL)
        return;

    b = (block_t *)((unsigned char *)allocation - sizeof(*b));
    if (b->size != bytes)
        a->size_mismatches++;
    a->live -= b->size;
    os64_free(b);
}

// ── fixtures ────────────────────────────────────────────────────────────────

typedef struct
{
    const char *file;
    const char *label;
    uint8_t    *bytes;
    size_t      length;
    bool        fixed_width;
} fixture_t;

static fixture_t fonts[] = {
    { "DejaVuSans.ttf",           "DejaVu Sans (TrueType)",       NULL, 0, false },
    { "DejaVuSansMono.ttf",       "DejaVu Sans Mono (TrueType)",  NULL, 0, true  },
    { "SourceSans3-Regular.otf",  "Source Sans 3 (OpenType/CFF)", NULL, 0, false },
    { "SourceCodePro-Regular.otf","Source Code Pro (OpenType/CFF)",NULL, 0, true  },
};
#define FONT_COUNT (sizeof(fonts) / sizeof(fonts[0]))

// Returns FT_OK when every fixture loaded, FT_SKIP when the directory holds
// none of them, and FT_NO_FIXTURE when it holds some but not all.
//
// The three answers are different on purpose. A system with no font fixtures
// installed cannot run this test, and the suite's rule is that "I cannot run
// here" is not "I failed". A system with SOME of them is a broken install,
// and calling that a skip would let the image lose a file quietly.
#define FT_SKIP  (-1)

static int load_fonts(const char *dir)
{
    size_t loaded = 0;

    for (size_t i = 0; i < FONT_COUNT; i++)
    {
        char path[512];
        os64_slurp_status_t status;

        os64_snprintf(path, sizeof(path), "%s/%s", dir, fonts[i].file);
        status = os64_slurp(path, FONT_CAP, &fonts[i].bytes, &fonts[i].length);
        if (status != OS64_SLURP_OK)
        {
            os64_hprintf(OS64_STDERR, "fonttest: %s: %s\n", path,
                         os64_slurp_status_name(status));
            continue;
        }
        loaded++;
        os64_printf("fonttest: %-32s %6lu bytes\n", fonts[i].file,
                    (unsigned long)fonts[i].length);
    }

    if (loaded == FONT_COUNT)
        return FT_OK;
    if (loaded == 0)
    {
        os64_printf("fonttest: SKIP - no font fixtures under %s\n", dir);
        return FT_SKIP;
    }
    return FT_NO_FIXTURE;
}

static void fail(const char *what)
{
    failures++;
    os64_hprintf(OS64_STDERR, "fonttest: FAIL %s\n", what);
}

// ── the specimen on the terminal ────────────────────────────────────────────
// Five levels of coverage, from none to full. A grayscale mask printed as
// characters is not decoration: it is the only evidence a text boot can give
// that an OUTLINE was rasterized, and it shows the antialiasing that a 1-bit
// renderer would not produce.

static char shade_for(uint8_t coverage)
{
    static const char ramp[] = " .:*#";

    return ramp[(coverage * 4u) / 255u];
}

static void print_glyph(os64_font_face_t *face, uint32_t scalar,
                        const char *label)
{
    uint32_t index = 0;
    os64_font_glyph_t *glyph = NULL;
    os64_font_glyph_view_t view;

    if (ft->lookup(face, scalar, &index) != OS64_FONT_OK)
        return;
    if (ft->render(face, index, &glyph) != OS64_FONT_OK)
        return;
    if (ft->glyph_view(glyph, &view) != OS64_FONT_OK)
    {
        ft->glyph_release(glyph);
        return;
    }

    os64_printf("  %s  %ux%u mask, left %d, top %d, advance %d/64 px\n",
                label, view.width, view.height, view.left, view.top,
                view.advance_x);

    for (uint32_t row = 0; row < view.height; row++)
    {
        char line[72];
        uint32_t n = 0;

        while (n < view.width && n < sizeof(line) - 1)
        {
            line[n] = shade_for(view.coverage[(size_t)row * view.stride + n]);
            n++;
        }
        line[n] = '\0';
        os64_printf("    |%s|\n", line);
    }

    ft->glyph_release(glyph);
}

// ── the assertions ──────────────────────────────────────────────────────────

// Independently derived from the fixtures' own kern/GPOS tables — the same
// values tools/test_freetype_host.c pins, recorded in fixtures/FIXTURES.md.
// DejaVu Sans answers from a legacy `kern` table, Source Sans 3 from GPOS,
// so the pair proves both roads.
typedef struct
{
    size_t   font;
    uint32_t left, right;
    uint32_t ppem;
    int32_t  expected;      // 26.6 pixels
} kern_case_t;

static const kern_case_t kern_cases[] = {
    { 0, 'A', 'V', 32, -131 },
    { 0, 'T', 'o', 32, -348 },
    { 2, 'A', 'V', 32,  -28 },
    { 2, 'L', 'T', 32, -245 },
};

static int check_face(os64_font_engine_t *engine, fixture_t *f, uint32_t px)
{
    os64_font_face_options_t options = { px, OS64_FONT_HINT_NORMAL };
    os64_font_face_t *face = NULL;
    os64_font_face_info_t info;
    os64_font_status_t status;
    os64_font_glyph_t *narrow = NULL, *wide = NULL;
    os64_font_glyph_view_t nv, wv;
    int rc = FT_OK;
    uint32_t ni = 0, wi = 0;

    status = ft->face_open(engine, f->bytes, f->length, &options, &face);
    if (status != OS64_FONT_OK)
    {
        os64_hprintf(OS64_STDERR, "fonttest: %s at %upx: open status %d\n",
                     f->label, px, (int)status);
        fail("face_open");
        return FT_OPEN;
    }

    if (ft->face_info(face, &info) != OS64_FONT_OK)
    {
        fail("face_info");
        rc = FT_INFO;
        goto done;
    }

    os64_printf("  %-32s %upx  %s/%s  %u glyphs  ascent %d descent %d line %d%s\n",
                f->label, px, info.family, info.style, info.glyph_count,
                info.ascent, info.descent, info.line_height,
                (info.flags & OS64_FONT_FACE_FIXED_WIDTH) ? "  fixed" : "");

    if (info.ascent < 0 || info.descent < 0 ||
        info.line_height < info.ascent + info.descent)
    {
        fail("line_height is less than ascent + descent");
        rc = FT_INFO;
        goto done;
    }
    if (((info.flags & OS64_FONT_FACE_FIXED_WIDTH) != 0) != f->fixed_width)
    {
        fail("the fixed-width flag disagrees with the fixture");
        rc = FT_INFO;
        goto done;
    }

    if (ft->lookup(face, 'i', &ni) != OS64_FONT_OK ||
        ft->lookup(face, 'W', &wi) != OS64_FONT_OK)
    {
        fail("lookup of 'i' or 'W'");
        rc = FT_LOOKUP;
        goto done;
    }
    if (ft->render(face, ni, &narrow) != OS64_FONT_OK ||
        ft->render(face, wi, &wide) != OS64_FONT_OK)
    {
        fail("render of 'i' or 'W'");
        rc = FT_RENDER;
        goto done;
    }
    ft->glyph_view(narrow, &nv);
    ft->glyph_view(wide, &wv);

    // The font's own claim about itself, checked against its own metrics.
    if (f->fixed_width ? (nv.advance_x != wv.advance_x)
                       : (nv.advance_x >= wv.advance_x))
    {
        os64_hprintf(OS64_STDERR, "fonttest: %s: i=%d W=%d\n",
                     f->label, nv.advance_x, wv.advance_x);
        fail(f->fixed_width ? "a monospace face gave unequal advances"
                            : "a proportional face gave i >= W");
        rc = FT_RENDER;
        goto done;
    }

    // Y grows DOWN: a capital's ink starts above the baseline.
    if (wv.height > 0 && wv.top >= 0)
    {
        fail("'W' has a non-negative top bearing (is Y pointing up?)");
        rc = FT_RENDER;
        goto done;
    }
    if (wv.stride != wv.width || wv.coverage == NULL)
    {
        fail("'W' mask is not tightly packed");
        rc = FT_RENDER;
        goto done;
    }
    if (wv.ink.x0 != wv.left * OS64_FONT_UNIT ||
        wv.ink.y0 != wv.top * OS64_FONT_UNIT ||
        wv.ink.x1 != (wv.left + (int32_t)wv.width) * OS64_FONT_UNIT ||
        wv.ink.y1 != (wv.top + (int32_t)wv.height) * OS64_FONT_UNIT)
    {
        fail("the ink rectangle disagrees with the bearings and dimensions");
        rc = FT_RENDER;
        goto done;
    }

done:
    ft->glyph_release(narrow);
    ft->glyph_release(wide);
    ft->face_close(face);
    return rc;
}

// ── the window ──────────────────────────────────────────────────────────────

// Blend one coverage mask into the canvas at a baseline-relative position.
// THE MASK IS BLENDED, NOT STAMPED: a glyph paints only where it has ink, so
// an overhanging or kerned neighbour is not erased by a rectangle of
// background. The background goes down ONCE, behind the whole line, before
// any glyph is drawn.
static void blend_glyph(os64_gui_surface_t *surf,
                        const os64_font_glyph_view_t *g,
                        int32_t pen_x, int32_t baseline_y, uint32_t ink)
{
    for (uint32_t row = 0; row < g->height; row++)
    {
        int32_t y = baseline_y + g->top + (int32_t)row;

        if (y < 0 || y >= (int32_t)surf->height)
            continue;

        for (uint32_t col = 0; col < g->width; col++)
        {
            int32_t x = pen_x + g->left + (int32_t)col;
            uint32_t a, dst, out;
            uint32_t sr, sg, sb, dr, dg, db;

            if (x < 0 || x >= (int32_t)surf->width)
                continue;

            a = g->coverage[(size_t)row * g->stride + col];
            if (a == 0)
                continue;

            dst = surf->pixels[(size_t)y * surf->pitch_px + (size_t)x];

            sr = (ink >> 16) & 0xFF;  sg = (ink >> 8) & 0xFF;  sb = ink & 0xFF;
            dr = (dst >> 16) & 0xFF;  dg = (dst >> 8) & 0xFF;  db = dst & 0xFF;

            // Rounded, so full coverage lands exactly on the ink colour.
            dr = (sr * a + dr * (255 - a) + 127) / 255;
            dg = (sg * a + dg * (255 - a) + 127) / 255;
            db = (sb * a + db * (255 - a) + 127) / 255;

            out = 0xFF000000u | (dr << 16) | (dg << 8) | db;
            surf->pixels[(size_t)y * surf->pitch_px + (size_t)x] = out;
        }
    }
}

// Draw one run of ASCII, with kerning, and return the pen's final x.
static int32_t draw_run(os64_gui_surface_t *surf, os64_font_face_t *face,
                        const char *text, int32_t x, int32_t baseline,
                        uint32_t ink)
{
    uint32_t previous = 0;
    int32_t pen_64 = x * OS64_FONT_UNIT;

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++)
    {
        uint32_t index = 0;
        os64_font_glyph_t *glyph = NULL;
        os64_font_glyph_view_t view;

        if (ft->lookup(face, *p, &index) != OS64_FONT_OK)
        {
            previous = 0;
            continue;
        }

        if (previous != 0)
        {
            os64_font_pos_t delta = 0;

            // Kerning arrives unrounded, which is why the pen is carried in
            // 26.6 and rounded only when a glyph is placed.
            if (ft->pair_adjust(face, previous, index, &delta) == OS64_FONT_OK)
                pen_64 += delta;
        }

        if (ft->render(face, index, &glyph) == OS64_FONT_OK)
        {
            if (ft->glyph_view(glyph, &view) == OS64_FONT_OK)
            {
                blend_glyph(surf, &view,
                            (pen_64 + OS64_FONT_UNIT / 2) / OS64_FONT_UNIT,
                            baseline, ink);
                pen_64 += view.advance_x;
            }
            ft->glyph_release(glyph);
        }
        previous = index;
    }
    return (pen_64 + OS64_FONT_UNIT / 2) / OS64_FONT_UNIT;
}

// Four faces, four sizes each, plus a gap between faces. Generous rather than
// exact: the faces' own line heights decide the real total, and a window a
// little too tall shows a margin where one too short hides a line.
#define SPECIMEN_CONTENT_H  520u

static void draw_specimen(os64_font_engine_t *engine)
{
    uint32_t screen_w = 0, screen_h = 0;
    int64_t win;
    os64_gui_surface_t surf;
    os64_gui_rect_t all;
    static const uint32_t sizes[] = { 12, 16, 22, 30 };
    int32_t y = 12;

    if (os64_gui_screen_info(&screen_w, &screen_h) != 0)
    {
        os64_printf("fonttest: SKIP specimen - no GUI on this boot\n");
        return;
    }

    // ASK FOR THE CONTENT, NOT THE FRAME: the specimen's height is the sum of
    // four faces' line heights at four sizes, and a window sized by eye
    // truncates the last line behind the border. The WM adds the current
    // decoration in the same transaction as creation.
    win = os64_gui_window_create_content("fonttest",90,40,620,SPECIMEN_CONTENT_H,0);
    if (win <= 0)
    {
        os64_printf("fonttest: SKIP specimen - window_create %ld\n", (long)win);
        return;
    }
    if (os64_gui_window_get_surface(win, &surf) != 0 || surf.pixels == NULL)
    {
        os64_printf("fonttest: SKIP specimen - no canvas\n");
        os64_gui_window_destroy(win);
        return;
    }

    // The paper, once, behind everything. Nothing below paints a rectangle.
    all.x = 0; all.y = 0; all.w = surf.width; all.h = surf.height;
    os64_draw_fill_rect(&surf, all, 0xFFF6F3EC);

    for (size_t f = 0; f < FONT_COUNT; f++)
    {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        {
            os64_font_face_options_t options = { sizes[s], OS64_FONT_HINT_NORMAL };
            os64_font_face_t *face = NULL;
            os64_font_face_info_t info;

            if (ft->face_open(engine, fonts[f].bytes, fonts[f].length,
                              &options, &face) != OS64_FONT_OK)
                continue;
            if (ft->face_info(face, &info) != OS64_FONT_OK)
            {
                ft->face_close(face);
                continue;
            }

            y += (info.ascent + OS64_FONT_UNIT - 1) / OS64_FONT_UNIT;
            draw_run(&surf, face, "Wavy AVATAR To jog 123 quick", 10, y,
                     0xFF1A1A22);
            y += (info.line_height - info.ascent + OS64_FONT_UNIT - 1)
                     / OS64_FONT_UNIT;

            ft->face_close(face);
        }
        y += 6;
    }

    os64_gui_window_publish(win, NULL);
    os64_printf("fonttest: specimen window open for 20 seconds\n");

    // Hold it on screen long enough to be looked at and screenshotted, but
    // answer a close request at once.
    for (int tenth = 0; tenth < 200; tenth++)
    {
        os64_gui_event_t event;

        while (os64_gui_event_poll(win, &event) > 0)
        {
            if (event.type == OS64_GUI_EVENT_WINDOW_CLOSE)
            {
                tenth = 200;
                break;
            }
        }
        os64_sleep(100);
    }

    os64_gui_window_destroy(win);
}

// ── main ────────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : FONT_DIR_DEFAULT;
    accounting_t accounting = { 0, 0, 0, 0 };
    os64_font_engine_options_t options;
    os64_font_engine_t *engine = NULL;
    os64_font_engine_stats_t stats;
    os64_font_status_t status;
    os64_font_face_t *face = NULL;
    int rc = FT_OK;

    ft = os64_freetype_backend_v1();
    if (ft == NULL || ft->revision != OS64_FONT_BACKEND_REVISION ||
        ft->struct_size != sizeof(os64_font_backend_t))
    {
        os64_hprintf(OS64_STDERR, "fonttest: backend table missing or stale\n");
        return FT_NO_BACKEND;
    }
    os64_printf("fonttest: backend revision %u, table %lu bytes\n",
                ft->revision, (unsigned long)ft->struct_size);

    int runtime_status = os64_font_runtime_test();
    if (runtime_status != 0) {
        os64_hprintf(OS64_STDERR, "fonttest: private target runtime FAIL %d\n", runtime_status);
        return FT_ENGINE;
    }
    os64_printf("fonttest: private target runtime PASS\n");

    rc = load_fonts(dir);
    if (rc == FT_SKIP)
        return FT_OK;
    if (rc != FT_OK)
        return rc;

    os64_memset(&options, 0, sizeof(options));
    options.memory.context = &accounting;
    options.memory.alloc   = fixture_alloc;
    options.memory.free    = fixture_free;
    options.memory_cap     = 0;          // the built-in default

    status = ft->engine_create(&options, &engine);
    if (status != OS64_FONT_OK)
    {
        os64_hprintf(OS64_STDERR, "fonttest: engine_create status %d\n",
                     (int)status);
        return FT_ENGINE;
    }

    os64_printf("fonttest: faces --------------------------------------------\n");
    for (size_t f = 0; f < FONT_COUNT && rc == FT_OK; f++)
    {
        static const uint32_t sizes[] = { 16, 24, 32 };

        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        {
            rc = check_face(engine, &fonts[f], sizes[s]);
            if (rc != FT_OK)
                break;
        }
    }

    // Kerning, against values decoded from the fixtures' own tables.
    if (rc == FT_OK)
    {
        os64_printf("fonttest: kerning ------------------------------------------\n");
        for (size_t i = 0; i < sizeof(kern_cases) / sizeof(kern_cases[0]); i++)
        {
            const kern_case_t *k = &kern_cases[i];
            os64_font_face_options_t options2 = { k->ppem, OS64_FONT_HINT_NORMAL };
            uint32_t l = 0, r = 0;
            os64_font_pos_t delta = 0;

            if (ft->face_open(engine, fonts[k->font].bytes,
                              fonts[k->font].length, &options2, &face)
                    != OS64_FONT_OK)
            {
                fail("kerning: face_open");
                rc = FT_KERN;
                break;
            }
            ft->lookup(face, k->left, &l);
            ft->lookup(face, k->right, &r);
            if (ft->pair_adjust(face, l, r, &delta) != OS64_FONT_OK ||
                delta < k->expected - 2 || delta > k->expected + 2)
            {
                os64_hprintf(OS64_STDERR,
                             "fonttest: %s '%c%c' @%upx kerned %d, expected %d\n",
                             fonts[k->font].file, (char)k->left, (char)k->right,
                             k->ppem, delta, k->expected);
                fail("kerning value");
                rc = FT_KERN;
            }
            else
                os64_printf("  %-28s '%c%c' @%upx  %d/64 px\n",
                            fonts[k->font].file, (char)k->left, (char)k->right,
                            k->ppem, delta);
            ft->face_close(face);
            face = NULL;
            if (rc != FT_OK)
                break;
        }
    }

    // A glyph must outlive the face it came from, and the engine must refuse
    // to go while either is alive.
    if (rc == FT_OK)
    {
        os64_font_face_options_t options3 = { 24, OS64_FONT_HINT_NORMAL };
        os64_font_glyph_t *glyph = NULL;
        os64_font_glyph_view_t view;
        uint32_t index = 0;

        if (ft->face_open(engine, fonts[0].bytes, fonts[0].length, &options3,
                          &face) == OS64_FONT_OK &&
            ft->lookup(face, 'g', &index) == OS64_FONT_OK &&
            ft->render(face, index, &glyph) == OS64_FONT_OK)
        {
            ft->face_close(face);
            face = NULL;

            if (ft->glyph_view(glyph, &view) != OS64_FONT_OK ||
                view.coverage == NULL)
            {
                fail("a glyph did not survive its face");
                rc = FT_LIFETIME;
            }
            else if (ft->engine_destroy(engine) != OS64_FONT_BUSY)
            {
                fail("the engine was destroyable with a live glyph");
                rc = FT_LIFETIME;
            }
            ft->glyph_release(glyph);
        }
        else
        {
            fail("lifetime setup");
            rc = FT_LIFETIME;
        }
    }

    // The specimens, once the assertions have had their say.
    if (rc == FT_OK)
    {
        os64_font_face_options_t options4 = { 20, OS64_FONT_HINT_NORMAL };

        os64_printf("fonttest: specimen -----------------------------------------\n");
        if (ft->face_open(engine, fonts[0].bytes, fonts[0].length, &options4,
                          &face) == OS64_FONT_OK)
        {
            print_glyph(face, 'R', "DejaVu Sans 'R' at 20px");
            print_glyph(face, 'e', "DejaVu Sans 'e' at 20px");
            ft->face_close(face);
            face = NULL;
        }

        ft->engine_stats(engine, &stats);
        os64_printf("fonttest: engine live %lu bytes, peak %lu, %ld allocations\n",
                    (unsigned long)stats.live_bytes,
                    (unsigned long)stats.peak_bytes, accounting.allocations);

        draw_specimen(engine);
    }

    // Everything back: the engine destroys cleanly and the heap is level.
    if (ft->engine_destroy(engine) != OS64_FONT_OK)
    {
        fail("engine_destroy with nothing outstanding");
        if (rc == FT_OK)
            rc = FT_LIFETIME;
    }
    if (accounting.live != 0 || accounting.size_mismatches != 0)
    {
        os64_hprintf(OS64_STDERR,
                     "fonttest: %lu bytes leaked, %d frees given a wrong size\n",
                     (unsigned long)accounting.live, accounting.size_mismatches);
        fail("allocation accounting");
        if (rc == FT_OK)
            rc = FT_ACCOUNTING;
    }

    for (size_t f = 0; f < FONT_COUNT; f++)
        os64_free(fonts[f].bytes);

    if (rc == FT_OK && failures == 0)
    {
        os64_printf("fonttest: PASS (peak %lu bytes, %ld allocations)\n",
                    (unsigned long)accounting.peak, accounting.allocations);
        return FT_OK;
    }

    os64_hprintf(OS64_STDERR, "fonttest: FAILED with %d problem(s), exit %d\n",
                 failures, rc == FT_OK ? FT_ENGINE : rc);
    return rc == FT_OK ? FT_ENGINE : rc;
}
