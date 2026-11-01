// test_yonder_host.c — yonder's painter on the host: a page laid out by
// libflow at the harness's test fonts, painted through verbs that RECORD
// instead of drawing, one line per call. The expected recordings in
// tools/test_yonder_cases.inc are worked out by hand (YONDER.md § Slices);
// the corpus .paint files beside the pages are regression, not proof.

#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
#include "os64/text.h"
#include "agent.h"
#include "fetch/http.h"
#include "bar.h"
#include "download.h"
#include "mail.h"
#include "paint.h"
#include "paint_cache.h"
#include "garb/cascade.h"
#include "scale.h"
#include "test_libflow_fonts.h"
#include "os64/syscall_numbers.h"

// The formatter's writes to the terminal go nowhere; a mailbox's pipe is a
// real one, so its bytes really travel.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    if (handle <= 2)
        return (int64_t)len;
    return (int64_t)write(handle, buf, len);
}

int64_t os64_pipe(int32_t h[2])
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    h[0] = fds[0];
    h[1] = fds[1];
    return 0;
}

int64_t os64_read_for(int32_t handle, void *buf, size_t len, uint64_t timeout_ms)
{
    struct pollfd p = {handle, POLLIN, 0};
    int got = poll(&p, 1, (int)timeout_ms);
    if (got == 0)
        return OS64_ERR_TIMEOUT;
    if (got < 0)
        return -1;
    return (int64_t)read(handle, buf, len);
}

int64_t os64_close(int32_t handle)
{
    return close(handle);
}

void os64_yield(void)
{
    sched_yield();
}

static size_t live;

// libhtml ends the program when a pinned document is freed. A harness that
// reached this has found that, so it fails.
void os64_exit(int32_t code)
{
    (void)code;
    exit(3);
}
// While set, every allocation the painter asks for fails: paint_of sets it
// around yonder_paint alone when a case says the page is painted short of
// memory (s_paint_starved), so laying the page out is unaffected.
static bool s_alloc_starved;
// While not negative, how many more allocations the painter is given
// before every one fails (paint_of sets it around yonder_paint alone, as
// s_alloc_starved).
static long s_alloc_left = -1;
static long s_paint_alloc_left = -1;
// While set, the next allocation fails and clears it: memory refused once,
// then given (the paint cache asks again after letting entries go).
static bool s_alloc_refuse_once;
// Every byte asked for, and what paint_of's own painting asked for.
static size_t s_alloc_bytes, s_paint_bytes;
// Every allocation made: a buffer the scratch lends again is not one. And
// those of the scratch's size, all and paint_of's own painting's.
static uint64_t s_mallocs, s_big_mallocs, s_paint_big;

void *os64_malloc(size_t size)
{
    if (s_alloc_starved || s_alloc_left == 0)
        return NULL;
    if (s_alloc_refuse_once) {
        s_alloc_refuse_once = false;
        return NULL;
    }
    if (s_alloc_left > 0)
        s_alloc_left--;
    s_alloc_bytes += size;
    void *at = malloc(size != 0 ? size : 1);
    if (at != NULL) {
        live++;
        s_mallocs++;
        if (size >= YONDER_SCRATCH_FLOOR)
            s_big_mallocs++;
    }
    return at;
}

void *os64_calloc(size_t count, size_t size)
{
    void *at = os64_malloc(count * size);
    if (at != NULL)
        memset(at, 0, count * size);
    return at;
}

void *os64_realloc(void *ptr, size_t size)
{
    void *at = realloc(ptr, size != 0 ? size : 1);
    if (at != NULL && ptr == NULL)
        live++;
    return at;
}

void os64_free(void *ptr)
{
    if (ptr != NULL)
        live--;
    free(ptr);
}

static int checks, failures;

static void expect(const char *name, bool ok, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s%s%s\n", name, detail != NULL ? ":\n" : "",
                detail != NULL ? detail : "");
    }
}

// ── Fonts: libflow's harness resolver over the test backend ──────────────

static os64_text_context_t *s_text;

static void *text_alloc_cb(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void text_free_cb(void *ctx, void *p, size_t n)
{
    (void)ctx;
    (void)n;
    free(p);
}

static struct {
    char kind;
    uint32_t px;
    os64_text_font_t *font;
} s_fonts[64];
static int s_nfonts;

static os64_font_status_t test_fonts(void *ctx, const flow_family_list_t *families, bool bold,
                                     bool italic, uint32_t px, os64_text_font_t *const **list,
                                     size_t *count, os64_font_face_info_t *primary)
{
    (void)ctx;
    (void)bold;
    (void)italic;
    char kind = families->generic == FLOW_GENERIC_MONO ? 'M'
              : families->generic == FLOW_GENERIC_SANS ? 'A' : 'S';
    int i = 0;
    while (i < s_nfonts && !(s_fonts[i].kind == kind && s_fonts[i].px == px))
        i++;
    if (i == s_nfonts) {
        if (s_nfonts == (int)(sizeof(s_fonts) / sizeof(s_fonts[0])))
            return OS64_FONT_LIMIT;
        os64_font_face_options_t o = {.pixel_height = px, .hint = OS64_FONT_HINT_NONE};
        os64_font_status_t st = os64_text_font_open(s_text, (const uint8_t *)&kind, 1, &o,
                                                    &s_fonts[i].font);
        if (st != OS64_FONT_OK)
            return st;
        s_fonts[i].kind = kind;
        s_fonts[i].px = px;
        s_nfonts++;
    }
    *list = &s_fonts[i].font;
    *count = 1;
    memset(primary, 0, sizeof(*primary));
    primary->ascent = (int32_t)px * 48;
    primary->descent = (int32_t)px * 16;
    primary->line_height = (int32_t)px * 80;
    return OS64_FONT_OK;
}

// A picture whose src says "known" is 40x30; every other has not arrived.
static bool test_oracle(void *ctx, const os64_html_node_t *node, int32_t *w, int32_t *h)
{
    (void)ctx;
    const os64_html_attr_t *src = os64_html_attr(node, "src");
    if (src == NULL || src->value == NULL || strstr(src->value, "known") == NULL)
        return false;
    *w = 40;
    *h = 30;
    return true;
}

static flow_env_t kEnv = {
    .fonts = test_fonts,
    .replaced_size = test_oracle,
    .viewport_font_px = 16,
    .default_generic = FLOW_GENERIC_SERIF,
    .ink = 0x101010,
    .link_ink = 0x1010ee,
    .paper = 0xfefefe,
};

// ── The recording verbs ─────────────────────────────────────────────────

typedef struct {
    char *v;
    size_t n, cap;
} Out;

static void out(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void out(Out *o, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    if (o->n + (size_t)n + 1 > o->cap) {
        o->cap = (o->n + (size_t)n + 1) * 2;
        o->v = realloc(o->v, o->cap);
    }
    memcpy(o->v + o->n, line, (size_t)n + 1);
    o->n += (size_t)n;
}

static bool inside(os64_gui_rect_t r, os64_gui_rect_t clip)
{
    return r.w > 0 && r.h > 0 && r.x >= clip.x && r.y >= clip.y &&
           (int64_t)r.x + r.w <= (int64_t)clip.x + clip.w &&
           (int64_t)r.y + r.h <= (int64_t)clip.y + clip.h;
}

typedef struct {
    Out out;
    os64_gui_rect_t view;
    bool escaped;           // a fill reached past the viewport
    const os64_page_t *page;
} Rec;

// A colour as its RGB, and its alpha after a slash when it is not opaque.
static void colour_note(Rec *rec, uint32_t colour)
{
    out(&rec->out, " #%06x", colour & 0xffffffu);
    if (flow_alpha(colour) != 255)
        out(&rec->out, "/%d", (int)flow_alpha(colour));
}

static void rec_fill(void *ctx, os64_gui_rect_t r, uint32_t colour)
{
    Rec *rec = ctx;
    if (!inside(r, rec->view))
        rec->escaped = true;
    out(&rec->out, "fill %d %d %d %d", r.x, r.y, r.w, r.h);
    colour_note(rec, colour);
    out(&rec->out, "\n");
}

// A text is drawn whole and cut by its verb, so a clip narrower than the
// view — an ancestor's `overflow` — is part of what it was told.
static void clip_note(Rec *rec, os64_gui_rect_t clip)
{
    if (clip.x != rec->view.x || clip.y != rec->view.y || clip.w != rec->view.w ||
        clip.h != rec->view.h)
        out(&rec->out, " clip %d %d %d %d", clip.x, clip.y, clip.w, clip.h);
}

static void rec_text(void *ctx, const flow_box_t *b, int32_t x, int32_t baseline,
                     os64_gui_rect_t clip, uint32_t colour)
{
    Rec *rec = ctx;
    out(&rec->out, "text \"%.*s\" %d %d", (int)b->length, b->text, x, baseline);
    colour_note(rec, colour);
    clip_note(rec, clip);
    out(&rec->out, "\n");
}

static void rec_image(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    (void)b;
    Rec *rec = ctx;
    out(&rec->out, "image %d %d %d %d", c.x, c.y, c.w, c.h);
    clip_note(rec, clip);
    out(&rec->out, "\n");
}

static void rec_stand_in(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip,
                         uint32_t ink, uint32_t paper)
{
    (void)clip;
    Rec *rec = ctx;
    out(&rec->out, "stand-in %s link %d %d %d %d %d ink %06x paper %06x\n",
        b->node != NULL && b->node->name != NULL ? b->node->name : "?", b->link, c.x, c.y, c.w, c.h,
        ink & 0xffffffu, paper & 0xffffffu);
}

static void rec_control(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    (void)clip;
    Rec *rec = ctx;
    out(&rec->out, "control %d %d %d %d %d\n", b->control, c.x, c.y, c.w, c.h);
}

// A box has a picture behind it when libpage lists one for its element or
// a sheet's layer names one; the recording says which layer (-1 the
// attribute's), where it would be tiled, and from where.
static bool rec_backdrop(void *ctx, const flow_box_t *b, int32_t layer,
                         const os64_gui_rect_t *area, os64_gui_rect_t origin, int32_t ox,
                         int32_t oy, os64_gui_rect_t clip)
{
    (void)clip;
    Rec *rec = ctx;
    bool sheet = false;
    for (int32_t i = 0; i < flow_background_layers(b->style); i++)
        sheet |= flow_background_layer(b->style, i).image != NULL;
    if (!sheet && (b->node == NULL || os64_page_background_for(rec->page, b->node) < 0))
        return false;
    if (area != NULL && layer < 0)
        out(&rec->out, "backdrop %d %d %d %d from %d %d origin %d %d %d %d\n", area->x, area->y,
            area->w, area->h, ox, oy, origin.x, origin.y, origin.w, origin.h);
    else if (area != NULL)
        out(&rec->out, "backdrop %d %d %d %d layer %d origin %d %d %d %d\n", area->x, area->y,
            area->w, area->h, (int)layer, origin.x, origin.y, origin.w, origin.h);
    return true;
}

// A group's bounds when it opens, and its alpha when it closes.
static void rec_group_open(void *ctx, os64_gui_rect_t bounds)
{
    Rec *rec = ctx;
    out(&rec->out, "group %d %d %d %d\n", bounds.x, bounds.y, bounds.w, bounds.h);
}

static void rec_group_close(void *ctx, uint8_t alpha)
{
    Rec *rec = ctx;
    out(&rec->out, "end group %d\n", (int)alpha);
}

// A group laid through its matrix: its own bounds, the matrix in
// thousandths, and the clip it is laid in. Laid unless the case says the
// face has no memory for it (s_through_refused).
static bool s_through_refused;

static bool rec_through_open(void *ctx, os64_gui_rect_t bounds, const double m[6],
                             os64_gui_rect_t clip)
{
    Rec *rec = ctx;
    if (s_through_refused)
        return false;
    out(&rec->out, "through %d %d %d %d", bounds.x, bounds.y, bounds.w, bounds.h);
    for (int i = 0; i < 6; i++) {
        double v = m[i] * 1000;
        v = v < -9e18 ? -9e18 : v > 9e18 ? 9e18 : v;
        out(&rec->out, " %lld", (long long)(v < 0 ? v - 0.5 : v + 0.5));
    }
    out(&rec->out, " clip %d %d %d %d\n", clip.x, clip.y, clip.w, clip.h);
    return true;
}

static void rec_through_close(void *ctx, uint8_t alpha)
{
    Rec *rec = ctx;
    out(&rec->out, "end through %d\n", (int)alpha);
}

// A text clip (PILE3.md § The text clip): its bounds, where its mask ends
// and its group begins, and its close. Refused when the case says the
// face cannot make one (s_clip_refused).
static bool s_clip_refused;

static bool rec_clip_open(void *ctx, os64_gui_rect_t bounds)
{
    Rec *rec = ctx;
    if (s_clip_refused)
        return false;
    if (!inside(bounds, rec->view))
        rec->escaped = true;
    out(&rec->out, "clip %d %d %d %d\n", bounds.x, bounds.y, bounds.w, bounds.h);
    return true;
}

static void rec_clip_fill(void *ctx)
{
    out(&((Rec *)ctx)->out, "fill clip\n");
}

static void rec_clip_close(void *ctx)
{
    out(&((Rec *)ctx)->out, "end clip\n");
}

// A filtered group (FILTER.md): its layer when it opens, and its list's
// length, its currentColor and its alpha when it closes. Refused when the
// case says the face cannot make one (s_filter_refused).
static bool s_filter_refused;

static bool rec_filter_open(void *ctx, os64_gui_rect_t bounds)
{
    Rec *rec = ctx;
    if (s_filter_refused)
        return false;
    out(&rec->out, "filter %d %d %d %d\n", bounds.x, bounds.y, bounds.w, bounds.h);
    return true;
}

static void rec_filter_close(void *ctx, const flow_filter_t *filter, int32_t n, uint32_t current,
                             uint8_t alpha)
{
    Rec *rec = ctx;
    (void)filter;
    out(&rec->out, "end filter %d #%06x %d\n", (int)n, current, (int)alpha);
}

// A backdrop (FILTER.md § 7): its rect, its coverage (the mask's sum, or
// none), its list's length and its currentColor. Refused when the case
// says the face cannot draw one (s_backdrop_refused).
static bool s_backdrop_refused;

static bool rec_backdrop_filter(void *ctx, os64_gui_rect_t rect, const uint8_t *cover,
                                const flow_filter_t *filter, int32_t n, uint32_t current)
{
    Rec *rec = ctx;
    (void)filter;
    if (s_backdrop_refused)
        return false;
    long sum = -1;
    for (int32_t k = 0; cover != NULL && k < rect.w * rect.h; k++)
        sum += cover[k] + (k == 0);
    out(&rec->out, "backdrop %d %d %d %d cover %ld %d #%06x\n", rect.x, rect.y, rect.w, rect.h, sum,
        (int)n, current);
    return true;
}

// THE PAINT CACHE'S ORACLE (YONDER_PERF.md § perf-2): while set, a mask and
// a picture are recorded whole, every alpha and every pixel, so two
// paintings of one page compare pixel for pixel (paint_of).
static bool s_record_all;

// A shadow's mask: where, its colour, and the sum and the largest of its
// alphas, which a hand-worked case can check without listing every pixel.
// When set, every mask's alphas are also written here, a 200 x 100 page: a
// split repaint can then be compared with a whole one pixel by pixel.
static uint8_t *s_mask_canvas;

static void rec_mask(void *ctx, os64_gui_rect_t r, const uint8_t *alpha, uint32_t colour)
{
    Rec *rec = ctx;
    for (int32_t y = 0; s_mask_canvas != NULL && y < r.h; y++)
        for (int32_t x = 0; x < r.w; x++)
            if (r.x + x >= 0 && r.x + x < 200 && r.y + y >= 0 && r.y + y < 100)
                s_mask_canvas[(r.y + y) * 200 + r.x + x] = alpha[y * r.w + x];
    if (!inside(r, rec->view))
        rec->escaped = true;
    long sum = 0;
    int most = 0;
    for (int32_t k = 0; k < r.w * r.h; k++) {
        sum += alpha[k];
        most = alpha[k] > most ? alpha[k] : most;
    }
    out(&rec->out, "mask %d %d %d %d #%06x sum %ld max %d\n", r.x, r.y, r.w, r.h, colour, sum,
        most);
    for (int32_t k = 0; s_record_all && k < r.w * r.h; k++)
        out(&rec->out, "%02x%s", alpha[k], (k + 1) % r.w == 0 ? "\n" : "");
}

// A painted picture: where, and every pixel when it is small enough to be
// worked by hand, else its first and last.
static void rec_pixels(void *ctx, os64_gui_rect_t r, const uint32_t *argb)
{
    Rec *rec = ctx;
    if (!inside(r, rec->view))
        rec->escaped = true;
    out(&rec->out, "pixels %d %d %d %d", r.x, r.y, r.w, r.h);
    int32_t n = r.w * r.h;
    for (int32_t k = 0; k < n; k++)
        if (n <= 16 || k == 0 || k == n - 1 || s_record_all)
            out(&rec->out, " %08x", argb[k]);
    out(&rec->out, "\n");
}

static const char *kPage = "http://host/dir/page.html";

static os64_html_document_t *parse(const char *html, size_t len)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (p == NULL)
        return NULL;
    os64_html_parser_feed(p, html, len);
    return os64_html_parser_finish(p);
}

// Lays `html` out at `width` and paints `view`, the page scrolled to the
// view's corner; the recording, or NULL.
// With `css` the page's `style` elements are cascaded, as yonder does.
// When set, paint_of writes here whose background the canvas is
// (yonder_canvas_owner): "html", "body" or "none".
static const char **s_canvas_owner;
// When set, the page is cascaded and painted dark (paint.h's yonder_dark_t).
static const yonder_dark_t *s_dark_paint;
// When set, the page is painted with no memory to be had (s_alloc_starved).
static bool s_paint_starved;

static yonder_verbs_t verbs_of(Rec *rec)
{
    return (yonder_verbs_t){rec,          rec_fill,     rec_text,       rec_image,
                            rec_control,  rec_backdrop, rec_group_open, rec_group_close,
                            rec_mask,     rec_pixels,   rec_stand_in,   rec_through_open,
                            rec_through_close, rec_clip_open, rec_clip_fill, rec_clip_close,
                            rec_filter_open, rec_filter_close, rec_backdrop_filter};
}

// The oracle's caches: the suite's, which every page painted before fills,
// so a key that leaves out a value two pages differ in finds the other
// page's pixels; and one too small to hold a page's, which must work out
// what it cannot keep.
static yonder_paint_cache_t *s_suite_cache, *s_small_cache;
#define SMALL_CACHE_BUDGET 4096
// When set, paint_of's own painting goes through this cache: a case that
// counts what a cache did (paint_cache_cases).
static yonder_paint_cache_t *s_through;
// And through this scratch (scratch_paint_cases).
static yonder_scratch_t *s_through_scratch;
// When set, the oracle paints through this cache too: a case's own, after
// something was done to it (paint_cache_cases).
static yonder_paint_cache_t *s_oracle_extra;
// The oracle's scratch, one for the whole suite: every page paints on the
// buffers the pages before it left dirty, so a site that reads a byte it
// did not write paints something else.
static yonder_scratch_t *s_suite_scratch;

// `t` painted through `view`, every pixel recorded, through `cache` and
// `scratch` (NULL for none).
static char *paint_whole(flow_tree_t *t, os64_page_t *page, os64_gui_rect_t view,
                         yonder_paint_cache_t *cache, yonder_scratch_t *scratch)
{
    Rec rec = {{0}, view, false, page};
    out(&rec.out, "%s", "");
    yonder_verbs_t v = verbs_of(&rec);
    s_record_all = true;
    s_alloc_starved = s_paint_starved;
    yonder_paint(t, view, (flow_point_t){view.x, view.y}, kEnv.paper, s_dark_paint, cache, scratch,
                 &v);
    s_alloc_starved = false;
    s_record_all = false;
    if (scratch != NULL) {
        yonder_scratch_paint_end(scratch);
        expect("scratch: a paint gives back every buffer it takes",
               yonder_scratch_counts(scratch).lent == 0, NULL);
    }
    return rec.out.v;
}

// What the cache paints is what the painter works out: the page without
// it, then through the suite's cache twice — the second paint finding what
// the first made — and through the small one. And what the scratch paints
// too, alone and with the suite's cache: not when a case starves the
// paint, which a buffer the scratch already holds would then outlive.
static void paint_oracle(flow_tree_t *t, os64_page_t *page, os64_gui_rect_t view, const char *html)
{
    if (s_suite_cache == NULL)
        return;
    char *plain = paint_whole(t, page, view, NULL, NULL);
    yonder_paint_cache_t *through[4] = {s_suite_cache, s_suite_cache, s_small_cache, s_oracle_extra};
    for (int i = 0; i < 4 && through[i] != NULL; i++) {
        char *kept = paint_whole(t, page, view, through[i], NULL);
        bool same = plain != NULL && kept != NULL && strcmp(plain, kept) == 0;
        char what[160];
        snprintf(what, sizeof(what), "%s the paint cache paints what the painter works out: %.100s",
                 i == 3 ? "case's:" : i == 2 ? "small:" : i == 1 ? "again:" : "suite:", html);
        expect(what, same, same ? NULL : kept);
        free(kept);
    }
    for (int i = 0; i < 2 && s_suite_scratch != NULL && !s_paint_starved; i++) {
        char *lent = paint_whole(t, page, view, i == 0 ? NULL : s_suite_cache, s_suite_scratch);
        bool same = plain != NULL && lent != NULL && strcmp(plain, lent) == 0;
        char what[160];
        snprintf(what, sizeof(what), "%s the scratch paints what the painter works out: %.100s",
                 i == 0 ? "alone:" : "cached:", html);
        expect(what, same, same ? NULL : lent);
        free(lent);
    }
    free(plain);
}

// A scroll the page's first scroll container is put at before a paint_of
// paints it, when `s_paint_scrolls` says so.
static bool s_paint_scrolls;
static flow_point_t s_paint_scroll;
// The page a paint_of lays out is laid out as scripting-enabled, when set.
static bool s_paint_scripting;

static char *paint_of(const char *html, size_t len, int32_t width, os64_gui_rect_t view,
                      bool *escaped, bool css)
{
    os64_html_document_t *doc = parse(html, len);
    os64_page_t *page = os64_page_build(doc, kPage, NULL, NULL);
    garb_parsed_t sheets[8];
    garb_sheet_in_t in[8];
    int32_t n = 0;
    for (int32_t i = 0; css && i < os64_page_nsheets(page) && n < 8; i++) {
        const os64_page_sheet_t *sh = os64_page_sheet(page, i);
        if (sh->linked)
            continue;
        garb_parse_style_element(sh->node, &sheets[n]);
        in[n] = (garb_sheet_in_t){.sheet = &sheets[n], .media = sh->media};
        n++;
    }
    garb_cascade_t *c = css ? garb_cascade(in, n, doc, (garb_env_t){.width = width, .height = view.h,
                                                               .dark = s_dark_paint != NULL})
                            : NULL;
    flow_env_t env = kEnv;
    env.cascade = c;
    env.viewport_height = view.h;
    env.scripting = s_paint_scripting;
    flow_tree_t *t = flow_layout(doc, page, width, &env);
    if (t != NULL && s_paint_scrolls)
        (void)flow_scroll_set(t, 0, s_paint_scroll);
    Rec rec = {{0}, view, false, page};
    out(&rec.out, "%s", "");
    yonder_verbs_t v = verbs_of(&rec);
    s_alloc_starved = s_paint_starved;
    s_alloc_left = s_paint_alloc_left;
    size_t bytes = s_alloc_bytes;
    uint64_t big = s_big_mallocs;
    if (t != NULL)
        yonder_paint(t, view, (flow_point_t){view.x, view.y}, kEnv.paper, s_dark_paint, s_through,
                     s_through_scratch, &v);
    yonder_scratch_paint_end(s_through_scratch);
    s_paint_bytes = s_alloc_bytes - bytes;
    s_paint_big = s_big_mallocs - big;
    s_alloc_starved = false;
    s_alloc_left = -1;
    if (t != NULL)
        paint_oracle(t, page, view, html);
    if (s_canvas_owner != NULL) {
        const flow_box_t *o = t != NULL ? yonder_canvas_owner(t, &v) : NULL;
        *s_canvas_owner = o == NULL ? "none"
                          : o->node != NULL && o->node->tag == OS64_HTML_TAG_BODY ? "body" : "html";
    }
    if (escaped != NULL)
        *escaped = rec.escaped;
    flow_free(t);
    garb_cascade_free(c);
    for (int32_t k = 0; k < n; k++)
        garb_free(&sheets[k]);
    os64_page_free(page);
    os64_html_document_free(doc);
    return rec.out.v;
}

static void paint_page_case(const char *name, const char *html, int32_t width,
                            os64_gui_rect_t view, bool css, const char *expected)
{
    bool escaped = false;
    char *got = paint_of(html, strlen(html), width, view, &escaped, css);
    expect(name, got != NULL && strcmp(got, expected) == 0, got);
    expect(name, !escaped, "a fill reached past the viewport");
    free(got);
}

static void paint_case(const char *name, const char *html, int32_t width, os64_gui_rect_t view,
                       const char *expected)
{
    paint_page_case(name, html, width, view, false, expected);
}

#include "test_yonder_cases.inc"
#include "test_yonder_batch_a.inc"

// A blurred shadow cannot be worked pixel by pixel by hand, so what a blur
// must do is checked: the mask reaches three box blurs past the shape on
// every side, 3 (blur / 2); just outside the box the shadow is about half on,
// as a Gaussian is at its step; nothing falls on the box itself, and an
// inset shadow falls only inside it.
// A rounded box as big as libflow takes, painted through a small view
// (Quinn, #203): the corner arithmetic must stay in range — UBSan is fatal
// here — and the view, deep inside the top-left curve's square, shows the
// paper where the curve leaves it and red where it is inside.
// A filtered group (FILTER.md): its layer the group's bounds, grown by
// what the filter spreads, cut to the view grown the same way; laid at the
// group's opacity, so no opacity group opens beside it. A list with a
// reference, a layer the face refuses and one past the paint's budget
// (twice the view) are painted as a plain group, and the last two are
// counted.
static void filter_paint_cases(void)
{
    // blur(2px): 6 every side of the 200 x 20 box.
    paint_page_case("filter: a blur's layer is grown by what it spreads",
                    "<!doctype html><style>body { margin: 0 }"
                    " .f { filter: blur(2px); height: 20px; background: #00ff00 }</style>"
                    "<div class=f>a</div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "filter -6 -6 212 32\n"
                    "fill 0 0 200 20 #00ff00\n"
                    "text \"a\" 0 14 #101010 clip -6 -6 212 32\n"
                    "end filter 1 #101010 255\n");
    paint_page_case("filter: laid at the group's opacity, no group beside it",
                    "<!doctype html><style>body { margin: 0 }"
                    " .f { filter: grayscale(1) sepia(1); opacity: 0.5; height: 20px }</style>"
                    "<div class=f>a</div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "filter 0 0 200 20\n"
                    "text \"a\" 0 14 #101010 clip 0 0 200 20\n"
                    "end filter 2 #101010 128\n");
    static const char kPlain[] = "fill 0 0 200 100 #fefefe\n"
                                 "group 0 0 200 20\n"
                                 "text \"a\" 0 14 #101010\n"
                                 "end group 255\n";
    paint_page_case("filter: a list with a reference is a plain group",
                    "<!doctype html><style>body { margin: 0 }"
                    " .f { filter: url(#x) blur(2px); height: 20px }</style><div class=f>a</div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true, kPlain);
    expect("filter: a reference is not counted refused", yonder_paint_filters_refused == 0, NULL);
    s_filter_refused = true;
    paint_page_case("filter: a layer the face refuses is a plain group",
                    "<!doctype html><style>body { margin: 0 }"
                    " .f { filter: grayscale(1); height: 20px }</style><div class=f>a</div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true, kPlain);
    s_filter_refused = false;
    expect("filter: a refused layer is counted", yonder_paint_filters_refused == 1, NULL);
    // Each layer is charged the pixels it shows in the view: three boxes
    // filling the 200 x 100 view spend 2 x 200 x 100 with two, and the
    // third is a plain group.
    static const char kThree[] =
        "<!doctype html><style>body { margin: 0 } div { position: absolute; top: 0;"
        " width: 200px; height: 100px; filter: grayscale(1) }</style>"
        "<div>a</div><div>b</div><div>c</div>";
    bool escaped = false;
    char *got = paint_of(kThree, strlen(kThree), 200, (os64_gui_rect_t){0, 0, 200, 100}, &escaped,
                         true);
    int filters = 0, groups = 0;
    for (const char *at = got; at != NULL && (at = strstr(at, "\nfilter ")) != NULL; at++)
        filters++;
    for (const char *at = got; at != NULL && (at = strstr(at, "\ngroup ")) != NULL; at++)
        groups++;
    expect("filter: past the paint's budget, a plain group",
           filters == 2 && groups == 1 && yonder_paint_filters_refused == 1, got);
    free(got);
    // A filter inside a filter is charged what it shows in the repaint,
    // not in the outer layer's margin (Codex #445 round 1): a 10 x 10
    // repaint draws both the outer blur and the inner invert.
    static const char kNested[] = "<!doctype html><style>body { margin: 0 }"
        " .o { filter: blur(30px) } .i { filter: invert(1); height: 20px; background: red }</style>"
        "<div class=o><div class=i></div></div>";
    got = paint_of(kNested, strlen(kNested), 200, (os64_gui_rect_t){0, 0, 10, 10}, &escaped, true);
    int nested = 0;
    for (const char *at = got; at != NULL && (at = strstr(at, "\nfilter ")) != NULL; at++)
        nested++;
    expect("filter: a nested filter is charged against the repaint, not the outer margin",
           nested == 2 && yonder_paint_filters_refused == 0, got);
    free(got);
    // A shadow whose source is above the view (the cold read's first
    // finding): cast 200 down from a 100-high box, the view scrolled to
    // 150..250 shows only shadow. The layer reaches up 200 — the reach
    // mirrored — to hold the box that casts it, and the box is painted.
    static const char kCast[] = "<!doctype html><style>body { margin: 0 }"
        " .f { filter: drop-shadow(0 200px 0 red); height: 100px; background: #00ff00 }</style>"
        "<div class=f></div><div style=\"height: 2000px\"></div>";
    got = paint_of(kCast, strlen(kCast), 200, (os64_gui_rect_t){0, 150, 200, 100}, &escaped, true);
    expect("filter: a shadow's source above the view is painted into the layer",
           got != NULL && strstr(got, "filter 0 0 200 250\nfill 0 0 200 100 #00ff00\n") != NULL, got);
    free(got);
    // An overflow-clipped parent cuts the spread: a 40-high box casting 30
    // down inside a 50-high clip is filtered in a layer cut at 50.
    static const char kClipped[] = "<!doctype html><style>body { margin: 0 }"
        " .c { overflow: hidden; height: 50px }"
        " .f { filter: drop-shadow(0 30px 0 red); height: 40px; background: #00ff00 }</style>"
        "<div class=c><div class=f></div></div>";
    got = paint_of(kClipped, strlen(kClipped), 200, (os64_gui_rect_t){0, 0, 200, 100}, &escaped, true);
    expect("filter: the spread is cut where an ancestor clips the group",
           got != NULL && strstr(got, "filter 0 0 200 50\n") != NULL, got);
    free(got);
    // A small repaint beside a big blur: blur(30px) reaches 90 past a
    // 200 x 20 box, and a 10 x 10 view at its corner spends 100 of its 200,
    // not the margin, so it is drawn filtered as the rest of the page is.
    static const char kSmall[] = "<!doctype html><style>body { margin: 0 }"
                                 " .f { filter: blur(30px); height: 20px }</style><div class=f>a</div>";
    got = paint_of(kSmall, strlen(kSmall), 200, (os64_gui_rect_t){0, 0, 10, 10}, &escaped, true);
    expect("filter: a small repaint is charged what it shows, not the margin",
           got != NULL && strstr(got, "\nfilter ") != NULL && yonder_paint_filters_refused == 0, got);
    free(got);
}

// A backdrop (FILTER.md § 7): asked before the box's own background, with
// its border box cut to the view; a rounded box hands over its coverage
// (-1 is none); a list with a reference asks nothing; a refusal is counted.
static void backdrop_paint_cases(void)
{
    paint_page_case("backdrop: filtered before the box's background",
                    "<!doctype html><style>body { margin: 0 }"
                    " .h { backdrop-filter: blur(4px); height: 20px; background: #00ff00 }</style>"
                    "<div class=h></div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "backdrop 0 0 200 20 cover -1 1 #101010\n"
                    "fill 0 0 200 20 #00ff00\n");
    bool escaped = false;
    static const char kRound[] = "<!doctype html><style>body { margin: 0 }"
        " .h { backdrop-filter: grayscale(1); height: 20px; width: 20px; border-radius: 10px }"
        "</style><div class=h></div>";
    char *got = paint_of(kRound, strlen(kRound), 200, (os64_gui_rect_t){0, 0, 200, 100}, &escaped, true);
    long sum = -2;
    const char *at = got != NULL ? strstr(got, "backdrop 0 0 20 20 cover ") : NULL;
    if (at != NULL)
        sum = strtol(at + strlen("backdrop 0 0 20 20 cover "), NULL, 10);
    // A circle of radius 10 covers about pi 100 of its 400 pixels, each
    // at 255: about 80,000.
    expect("backdrop: a rounded box hands over its coverage", sum > 75000 && sum < 85000, got);
    free(got);
    static const char kUrl[] = "<!doctype html><style>body { margin: 0 }"
        " .h { backdrop-filter: url(#x) blur(2px); height: 20px }</style><div class=h></div>";
    got = paint_of(kUrl, strlen(kUrl), 200, (os64_gui_rect_t){0, 0, 200, 100}, &escaped, true);
    expect("backdrop: a list with a reference asks nothing",
           got != NULL && strstr(got, "backdrop ") == NULL && yonder_paint_filters_refused == 0, got);
    free(got);
    // The root has nothing behind it (Filter Effects 2): no backdrop.
    static const char kRoot[] = "<!doctype html><style>html { backdrop-filter: invert(1) }"
        " body { margin: 0; background: #fff }</style><p>x";
    got = paint_of(kRoot, strlen(kRoot), 200, (os64_gui_rect_t){0, 0, 200, 300}, &escaped, true);
    expect("backdrop: the root's asks nothing", got != NULL && strstr(got, "backdrop ") == NULL, got);
    free(got);
    s_backdrop_refused = true;
    static const char kPlain[] = "<!doctype html><style>body { margin: 0 }"
        " .h { backdrop-filter: blur(2px); height: 20px }</style><div class=h></div>";
    got = paint_of(kPlain, strlen(kPlain), 200, (os64_gui_rect_t){0, 0, 200, 100}, &escaped, true);
    s_backdrop_refused = false;
    expect("backdrop: a refusal is counted", yonder_paint_filters_refused == 1, got);
    free(got);
}

static void huge_corner_case(void)
{
    const char *html = "<!doctype html><style>body { margin: 0 } div { width: 31000000px;"
                       " height: 31000000px; border-radius: 50%; background: #ff0000 }"
                       "</style><div></div>";
    bool escaped = false;
    char *got = paint_of(html, strlen(html), 200, (os64_gui_rect_t){0, 0, 100, 100}, &escaped,
                         true);
    expect("radius: a box near the size limit paints its corner in range",
           got != NULL && !escaped && strstr(got, "fill 0 0 100 100 #fefefe") != NULL, got);
    free(got);
}

// ── A picture behind a rounded box (PILE3.md § Backgrounds) ─────────────
//
// A recording read back into a 200 x 100 canvas: how many times a picture
// was tiled over each pixel, and whether through a clip; and each clip's
// mask, the alpha its white fills leave.
typedef struct {
    int drawn[200 * 100];       // how many times a picture covers the pixel
    bool clipped[200 * 100];    // one of them through a clip
    int mask[200 * 100];        // a clip's mask there, 0..255; -1 none
    int fill[200 * 100];        // the alpha of the last `colour` fill; 0 none
    int clips, backdrops;
    bool from_moved;            // an attribute's tiling corner changed
    bool mask_strayed;          // a mask's fill reached past its clip
} Canvas;

static void canvas_read(Canvas *cv, const char *rec, uint32_t colour)
{
    memset(cv, 0, sizeof(*cv));
    for (int k = 0; k < 200 * 100; k++)
        cv->mask[k] = -1;
    bool in_mask = false, in_clip = false;
    os64_gui_rect_t clip = {0, 0, 0, 0};
    int fx = INT32_MIN, fy = INT32_MIN;
    for (const char *line = rec; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        int x, y, w, h, a, b;
        unsigned rgb;
        char tail[64] = "";
        if (strncmp(line, "fill clip", 9) == 0) {
            in_mask = false;
        } else if (strncmp(line, "end clip", 8) == 0) {
            in_clip = false;
        } else if (sscanf(line, "clip %d %d %d %d", &x, &y, &w, &h) == 4) {
            in_mask = in_clip = true;
            clip = (os64_gui_rect_t){x, y, w, h};
            cv->clips++;
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++)
                    if (i >= 0 && i < 200 && j >= 0 && j < 100)
                        cv->mask[j * 200 + i] = 0;
        } else if (sscanf(line, "fill %d %d %d %d #%x%63s", &x, &y, &w, &h, &rgb, tail) >= 5) {
            int alpha = tail[0] == '/' ? atoi(tail + 1) : 255;
            if (in_mask && !inside((os64_gui_rect_t){x, y, w, h}, clip))
                cv->mask_strayed = true;
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++) {
                    if (i < 0 || i >= 200 || j < 0 || j >= 100)
                        continue;
                    if (in_mask && rgb == 0xffffff)
                        cv->mask[j * 200 + i] = alpha;
                    else if (!in_mask && rgb == colour)
                        cv->fill[j * 200 + i] = alpha;
                }
        } else if (sscanf(line, "backdrop %d %d %d %d", &x, &y, &w, &h) == 4) {
            cv->backdrops++;
            if (sscanf(line, "backdrop %*d %*d %*d %*d from %d %d", &a, &b) == 2) {
                if (fx != INT32_MIN && (a != fx || b != fy))
                    cv->from_moved = true;
                fx = a;
                fy = b;
            }
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++)
                    if (i >= 0 && i < 200 && j >= 0 && j < 100) {
                        cv->drawn[j * 200 + i]++;
                        cv->clipped[j * 200 + i] |= in_clip;
                    }
        }
        line = end != NULL ? end + 1 : NULL;
    }
}

// `pic`, a page whose box has its picture, against `col`, the same box with
// a colour in its place: the picture covers each pixel at most once, and
// where the colour is laid whole it is drawn whole, unclipped or under a
// full mask; where the colour is laid in part, through a clip whose mask is
// the colour's share; where there is no colour, not at all.
static void round_picture_case(const char *name, const char *pic, const char *col,
                               os64_gui_rect_t view)
{
    static Canvas p, c;
    bool escaped = false;
    char *got = paint_of(pic, strlen(pic), 200, view, &escaped, true);
    char *ref = paint_of(col, strlen(col), 200, view, NULL, true);
    canvas_read(&p, got, 0);
    canvas_read(&c, ref, 0x0000ff);
    char why[160] = "";
    for (int j = 0; j < 100 && why[0] == '\0'; j++)
        for (int i = 0; i < 200 && why[0] == '\0'; i++) {
            int k = j * 200 + i, share = c.fill[k];
            bool full = p.drawn[k] == 1 && (!p.clipped[k] || p.mask[k] == 255);
            if (p.drawn[k] > 1)
                snprintf(why, sizeof(why), "(%d, %d) drawn %d times", i, j, p.drawn[k]);
            else if (share == 255 && !full)
                snprintf(why, sizeof(why), "(%d, %d) covered, not drawn whole", i, j);
            else if (share > 0 && share < 255 && (!p.clipped[k] || p.mask[k] != share))
                snprintf(why, sizeof(why), "(%d, %d) the colour's share %d, the mask's %d", i, j,
                         share, p.clipped[k] ? p.mask[k] : -1);
            else if (share == 0 && p.drawn[k] > 0 && (!p.clipped[k] || p.mask[k] > 0))
                snprintf(why, sizeof(why), "(%d, %d) drawn where the shape is not", i, j);
        }
    expect(name, why[0] == '\0' && p.backdrops > 0, why[0] != '\0' ? why : got);
    expect(name, !escaped, "a clip or a fill reached past the view");
    expect(name, !p.from_moved, "an attribute's tiling corner moved between the parts");
    expect(name, !p.mask_strayed, "a mask was painted past its clip");
    free(got);
    free(ref);
}

// The same box with its clips refused: each pixel whose middle the ellipse
// holds is drawn once, unclipped, and none it leaves — worked here in
// doubles from the CSS, for a box at (bx, by) of bw x bh whose four corners
// are rx x ry (top-left, top-right, bottom-right, bottom-left). A middle
// within 1/50 px of the curve could go either way, and is not asked.
static void round_picture_refused_case(const char *name, const char *pic, int bx, int by,
                                       int bw, int bh, const double rx[4], const double ry[4])
{
    static Canvas p;
    s_clip_refused = true;
    char *got = paint_of(pic, strlen(pic), 200, (os64_gui_rect_t){0, 0, 200, 100}, NULL, true);
    s_clip_refused = false;
    canvas_read(&p, got, 0);
    char why[160] = "";
    for (int j = 0; j < 100 && why[0] == '\0'; j++)
        for (int i = 0; i < 200 && why[0] == '\0'; i++) {
            double x = i + 0.5, y = j + 0.5, near = 1e9;
            bool in = x > bx && x < bx + bw && y > by && y < by + bh;
            // Each corner's ellipse centre, and whether the middle is in its square.
            double cx[4] = {bx + rx[0], bx + bw - rx[1], bx + bw - rx[2], bx + rx[3]};
            double cy[4] = {by + ry[0], by + ry[1], by + bh - ry[2], by + bh - ry[3]};
            for (int c = 0; in && c < 4; c++) {
                bool left = c == 0 || c == 3, top = c < 2;
                if (rx[c] > 0 && ry[c] > 0 && (left ? x < cx[c] : x > cx[c]) &&
                    (top ? y < cy[c] : y > cy[c])) {
                    double u = (x - cx[c]) / rx[c], v = (y - cy[c]) / ry[c];
                    double edge = sqrt(u * u + v * v);
                    in = edge <= 1;
                    near = fabs(edge - 1) * fmin(rx[c], ry[c]);
                }
            }
            int k = j * 200 + i;
            if (near < 0.02)
                continue;
            if (p.drawn[k] != (in ? 1 : 0) || p.clipped[k])
                snprintf(why, sizeof(why), "(%d, %d) drawn %d times, its middle %s", i, j,
                         p.drawn[k], in ? "inside" : "outside");
        }
    expect(name, why[0] == '\0' && p.clips == 0, why[0] != '\0' ? why : got);
    free(got);
}

static void round_picture_cases(void)
{
#define ROUND_PAGE(box, bg) \
    "<!doctype html><style>body { margin: 0 } div { margin: 10px 20px; width: 160px;" \
    " height: 80px; " box "; background: " bg " }</style><div></div>"
    os64_gui_rect_t all = {0, 0, 200, 100};
    round_picture_case("a rounded box's picture follows its curve",
                       ROUND_PAGE("border-radius: 30px", "url(p.gif)"),
                       ROUND_PAGE("border-radius: 30px", "#0000ff"), all);
    round_picture_case("each corner's own ellipse",
                       ROUND_PAGE("border-radius: 30px 10px 40px 0 / 20px 30px 25px 0",
                                  "url(p.gif)"),
                       ROUND_PAGE("border-radius: 30px 10px 40px 0 / 20px 30px 25px 0",
                                  "#0000ff"), all);
    round_picture_case("a picture cut to the padding box takes the inner curve",
                       ROUND_PAGE("border-radius: 30px; border: 8px solid #00ff00;"
                                  " background-clip: padding-box", "url(p.gif)"),
                       ROUND_PAGE("border-radius: 30px; border: 8px solid #00ff00;"
                                  " background-clip: padding-box", "#0000ff"), all);
    // The top-left square, 100 x 60, and the bottom-right one meet.
    round_picture_case("radii past the middle: the whole box through one clip",
                       ROUND_PAGE("border-radius: 100px 20px / 60px 20px", "url(p.gif)"),
                       ROUND_PAGE("border-radius: 100px 20px / 60px 20px", "#0000ff"), all);
    round_picture_case("an ellipse, its squares edge to edge",
                       ROUND_PAGE("border-radius: 50%", "url(p.gif)"),
                       ROUND_PAGE("border-radius: 50%", "#0000ff"), all);
    round_picture_case("a view through a corner draws only what it shows",
                       ROUND_PAGE("border-radius: 30px", "url(p.gif)"),
                       ROUND_PAGE("border-radius: 30px", "#0000ff"),
                       (os64_gui_rect_t){0, 0, 37, 23});
    round_picture_case("a view through the middle draws the middle",
                       ROUND_PAGE("border-radius: 30px", "url(p.gif)"),
                       ROUND_PAGE("border-radius: 30px", "#0000ff"),
                       (os64_gui_rect_t){60, 0, 80, 100});
    // An attribute's picture is tiled from its area's corner, whichever
    // part is drawn.
    round_picture_case("an attribute's picture keeps one tiling corner",
                       "<!doctype html><style>body { margin: 0 } table { margin: 10px 20px;"
                       " border-spacing: 0; border-radius: 30px } td { padding: 0; width: 160px;"
                       " height: 80px }</style><table background=p.gif><tr><td></td></tr></table>",
                       "<!doctype html><style>body { margin: 0 } table { margin: 10px 20px;"
                       " border-spacing: 0; border-radius: 30px; background: #0000ff } td {"
                       " padding: 0; width: 160px; height: 80px }</style><table><tr><td></td>"
                       "</tr></table>", all);
    const double r30[4] = {30, 30, 30, 30};
    round_picture_refused_case("no clip to be had: each row's pixels whose middle it holds",
                               ROUND_PAGE("border-radius: 30px", "url(p.gif)"), 20, 10, 160,
                               80, r30, r30);
    const double ex[4] = {30, 10, 40, 0}, ey[4] = {20, 30, 25, 0};
    round_picture_refused_case("no clip to be had, each corner its own ellipse",
                               ROUND_PAGE("border-radius: 30px 10px 40px 0 / 20px 30px 25px 0",
                                          "url(p.gif)"), 20, 10, 160, 80, ex, ey);
    {
        const char *html = ROUND_PAGE("border-radius: 30px", "#0000ff");
        char *got = paint_of(html, strlen(html), 200, all, NULL, true);
        expect("a rounded box with no picture makes no clip",
               got != NULL && strstr(got, "clip") == NULL, got);
        free(got);
    }
#undef ROUND_PAGE
}

// A shadow is shaded the same whatever part is repainted (Quinn, #203): the
// whole view painted once, and its two halves painted apart, give the same
// alpha at every pixel, for blurs of 8, 20 and 40 and an inset of 20.
static void shadow_split_cases(void)
{
    static const char *const kShadows[] = {"0 0 8px #000000", "0 0 20px #000000",
                                           "0 0 40px #000000", "inset 0 0 20px #000000"};
    for (size_t i = 0; i < sizeof(kShadows) / sizeof(kShadows[0]); i++) {
        char html[256];
        snprintf(html, sizeof(html),
                 "<!doctype html><style>body { margin: 0 } div { margin: 25px 70px;"
                 " width: 50px; height: 50px; background: #ffffff; box-shadow: %s }"
                 "</style><div></div>",
                 kShadows[i]);
        static uint8_t whole[200 * 100], halves[200 * 100];
        memset(whole, 0, sizeof(whole));
        memset(halves, 0, sizeof(halves));
        s_mask_canvas = whole;
        free(paint_of(html, strlen(html), 200, (os64_gui_rect_t){0, 0, 200, 100}, NULL, true));
        s_mask_canvas = halves;
        free(paint_of(html, strlen(html), 200, (os64_gui_rect_t){0, 0, 100, 100}, NULL, true));
        free(paint_of(html, strlen(html), 200, (os64_gui_rect_t){100, 0, 100, 100}, NULL, true));
        s_mask_canvas = NULL;
        int differ = 0;
        for (int k = 0; k < 200 * 100; k++)
            differ += whole[k] != halves[k];
        char note[64];
        snprintf(note, sizeof(note), "%s: %d pixels differ", kShadows[i], differ);
        expect("shadow: a split repaint shades every pixel as a whole one", differ == 0, note);
    }
}

// Whose background the canvas is, asked apart from painting (Quinn, #206):
// the root's when it has one, else the body's — a body only 50px tall all
// the same, which a walk of the boxes a scrolled view shows would miss.
static void canvas_owner_cases(void)
{
    struct { const char *css, *want; } k[] = {
        {"html { height: 1000px } body { margin: 0; height: 50px; background: #ff0000 }", "body"},
        {"html { background: #00ff00 } body { background: #ff0000 }", "html"},
        {"body { margin: 0 }", "none"},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        char html[256];
        snprintf(html, sizeof(html), "<!doctype html><style>%s</style><p>x", k[i].css);
        const char *owner = NULL;
        s_canvas_owner = &owner;
        free(paint_of(html, strlen(html), 200, (os64_gui_rect_t){0, 100, 200, 100}, NULL, true));
        s_canvas_owner = NULL;
        expect("canvas: its owner is found apart from what the view shows",
               owner != NULL && strcmp(owner, k[i].want) == 0, owner);
    }
}

static void shadow_blur_cases(void)
{
    struct { const char *css; int x, y, w, h, lo, hi; bool inset; } k[] = {
        {"box-shadow: 0 0 8px #000000", 28, 28, 44, 44, 100, 160, false},
        {"box-shadow: inset 0 0 8px #000000", 28, 28, 44, 44, 100, 255, true},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        char html[256];
        snprintf(html, sizeof(html),
                 "<!doctype html><style>body { margin: 0 } div { margin: 40px; width: 20px;"
                 " height: 20px; %s }</style><div></div>",
                 k[i].css);
        bool escaped = false;
        char *got = paint_of(html, strlen(html), 200, (os64_gui_rect_t){0, 0, 200, 100},
                             &escaped, true);
        const char *m = got != NULL ? strstr(got, "mask ") : NULL;
        int x = 0, y = 0, w = 0, h = 0, most = 0;
        long sum = 0;
        bool read = m != NULL && sscanf(m, "mask %d %d %d %d #000000 sum %ld max %d", &x, &y, &w,
                                        &h, &sum, &most) == 6;
        expect(k[i].inset ? "shadow: an inset blur falls inside, its mask the blur's reach"
                          : "shadow: an outer blur is half on at the edge, its mask the reach",
               read && x == k[i].x && y == k[i].y && w == k[i].w && h == k[i].h &&
                   most >= k[i].lo && most <= k[i].hi && sum > 0 && !escaped,
               got);
        free(got);
    }
}

// ── Pictures into their boxes ───────────────────────────────────────────
//
// Each expected pixel worked by hand. Column x of a box w wide takes the
// source column floor((2x+1) * sw / 2w): the source pixel under its middle.

static bool pixels_are(const uint32_t *got, const uint32_t *want, int n)
{
    for (int i = 0; i < n; i++)
        if (got[i] != want[i])
            return false;
    return true;
}

// DARK PAGES (paint.h): the colour each run of `html` is drawn in, by its
// first word, and what the canvas is filled with first. The expected
// colours are Python's colorsys run on the rule (paint.c's relight: the
// luminance split at 0.18, paper to 0.4%-7%, ink to 80%-62%, the lightness
// found by halving).
static const yonder_dark_t kDarkPaint = {.paper = 0x202020};
static bool starts(const char *got, const char *head)
{
    return got != NULL && strncmp(got, head, strlen(head)) == 0;
}
static bool painted_in(const char *got, const char *word, const char *colour)
{
    char want[64];
    snprintf(want, sizeof(want), "text \"%s", word);
    const char *line = got != NULL ? strstr(got, want) : NULL;
    if (line == NULL)
        return false;
    const char *end = strchr(line, '\n');
    const char *at = strstr(line, colour);
    return at != NULL && (end == NULL || at < end);
}
static void dark_cases(void)
{
    os64_gui_rect_t view = {0, 0, 300, 200};
    s_dark_paint = &kDarkPaint;
    const char *plain = "<!doctype html><p>plain <a href=x>link</a></p>";
    char *got = paint_of(plain, strlen(plain), 300, view, NULL, true);
    expect("dark: a page that set no colours is laid on the dark paper",
           starts(got, "fill 0 0 300 200 #202020\n"), got);
    expect("dark: its default ink and link go light, the link keeping a blue tint",
           painted_in(got, "plain", "#e4e4e4") && painted_in(got, "link", "#d8d8fc"), got);
    free(got);
    const char *own = "<!doctype html><style>body{margin:0;background:#ffffff} div{background:#000000}"
                      " h1{background:#e0e8ff;margin:0;font-size:16px}"
                      "</style><p>inside</p><div>night</div><h1>pale</h1><p style=\"color:#ff0000\">red</p>";
    got = paint_of(own, strlen(own), 300, view, NULL, true);
    expect("dark: a page's own white paper goes dark, and its dark ink light",
           starts(got, "fill 0 0 300 200 #101010\n") && painted_in(got, "inside", "#e4e4e4"), got);
    expect("dark: a dark box stays; a pale one darkens keeping its hue; a mid colour is the page's",
           got != NULL && strstr(got, " #000000\n") != NULL && strstr(got, " #001962\n") != NULL &&
           painted_in(got, "night", "#e4e4e4") && painted_in(got, "red", "#ff0000"), got);
    free(got);
    // Quinn's panels (#238): yellow and green are half light by HSL and
    // look nearly white, so black on them must not become pale on bright.
    const char *panels = "<!doctype html><style>body{margin:0;background:#000000}</style>"
                         "<p style=\"background:#ffff00;color:#000\">warning</p>"
                         "<p style=\"background:#00ff00;color:#000\">confirmed</p>";
    got = paint_of(panels, strlen(panels), 300, view, NULL, true);
    expect("dark: a bright yellow or green panel goes dark under the text that went light",
           got != NULL && strstr(got, " #1a1a00\n") != NULL && strstr(got, " #003100\n") != NULL &&
           strstr(got, " #ffff00\n") == NULL && strstr(got, " #00ff00\n") == NULL &&
           painted_in(got, "warning", "#e4e4e4") && painted_in(got, "confirmed", "#e4e4e4"), got);
    free(got);
    const char *scheme = "<!doctype html><style>body{margin:0;background:#ffffff;color:#000000}"
                         "@media (prefers-color-scheme: dark){body{background:#0a0a0a;color:#c0c0c0}}"
                         "</style><p>styled</p>";
    got = paint_of(scheme, strlen(scheme), 300, view, NULL, true);
    expect("dark: a page with a dark design of its own is told to use it, and nothing moves",
           starts(got, "fill 0 0 300 200 #0a0a0a\n") && painted_in(got, "styled", "#c0c0c0"), got);
    free(got);
    s_dark_paint = NULL;
    got = paint_of(scheme, strlen(scheme), 300, view, NULL, true);
    expect("dark: off, the same page is light, and its light styles hold",
           starts(got, "fill 0 0 300 200 #ffffff\n") && painted_in(got, "styled", "#000000"), got);
    free(got);
}

static void scale_cases(void)
{
    const uint32_t A = 0xff112233u, B = 0xff445566u, C = 0xff778899u;
    uint32_t dst[16] = {0};
    const uint32_t two[4] = {A, B, C, A};
    yonder_draw_picture(dst, 4, (os64_gui_rect_t){0, 0, 4, 4}, (os64_gui_rect_t){1, 1, 2, 2}, false, two, 2, 2);
    const uint32_t copied[16] = {0, 0, 0, 0, 0, A, B, 0, 0, C, A, 0, 0, 0, 0, 0};
    expect("picture: a box its own size is a copy", pixels_are(dst, copied, 16), NULL);

    // A box as wide as an int32_t says, starting past zero: its right edge
    // is past INT32_MAX, which must clip and not wrap (UBSan watches).
    uint32_t wide[4] = {0};
    const uint32_t one[1] = {A};
    yonder_draw_picture(wide, 4, (os64_gui_rect_t){0, 0, 4, 1},
                        (os64_gui_rect_t){2, 0, INT32_MAX, INT32_MAX}, false, one, 1, 1);
    const uint32_t wide_want[4] = {0, 0, A, A};
    expect("picture: a box past INT32_MAX clips, its edge added in 64 bits",
           pixels_are(wide, wide_want, 4), NULL);

    uint32_t row[4] = {0};
    const uint32_t ab[2] = {A, B};
    yonder_draw_picture(row, 4, (os64_gui_rect_t){0, 0, 4, 1}, (os64_gui_rect_t){0, 0, 4, 1}, false, ab, 2, 1);
    const uint32_t doubled[4] = {A, A, B, B};
    expect("picture: twice as wide doubles each column", pixels_are(row, doubled, 4), NULL);

    uint32_t pair[2] = {0};
    const uint32_t abc[3] = {A, B, C};
    yonder_draw_picture(pair, 2, (os64_gui_rect_t){0, 0, 2, 1}, (os64_gui_rect_t){0, 0, 2, 1}, false, abc, 3, 1);
    const uint32_t shrunk[2] = {A, C};
    expect("picture: three into two takes the columns under the middles",
           pixels_are(pair, shrunk, 2), NULL);

    // 0x80 red over opaque blue: red (255*128 + 127) / 255 = 128, blue
    // (255*127 + 127) / 255 = 127.
    uint32_t blend[1] = {0xff0000ffu};
    const uint32_t red_half[1] = {0x80ff0000u};
    yonder_draw_picture(blend, 1, (os64_gui_rect_t){0, 0, 1, 1}, (os64_gui_rect_t){0, 0, 1, 1}, false, red_half, 1, 1);
    expect("picture: half-transparent red over blue", blend[0] == 0xff80007fu, NULL);

    uint32_t clear[1] = {0xff0000ffu};
    const uint32_t none[1] = {0x00ff0000u};
    yonder_draw_picture(clear, 1, (os64_gui_rect_t){0, 0, 1, 1}, (os64_gui_rect_t){0, 0, 1, 1}, false, none, 1, 1);
    expect("picture: a transparent pixel leaves what is beneath", clear[0] == 0xff0000ffu, NULL);

    // A 4x4 picture in a 4x4 box, clipped to the middle 2x2: only those
    // four pixels are written, each the source's own.
    uint32_t src[16], clip[16] = {0};
    for (int i = 0; i < 16; i++)
        src[i] = 0xff000000u | (uint32_t)i;
    yonder_draw_picture(clip, 4, (os64_gui_rect_t){1, 1, 2, 2}, (os64_gui_rect_t){0, 0, 4, 4}, false, src, 4, 4);
    const uint32_t cut[16] = {0, 0, 0, 0, 0, src[5], src[6], 0, 0, src[9], src[10], 0, 0, 0, 0, 0};
    expect("picture: a clip cuts all four sides", pixels_are(clip, cut, 16), NULL);

    // A box partly off the surface's left and top: the clip is what keeps
    // the writes inside it.
    uint32_t edge[4] = {0};
    yonder_draw_picture(edge, 2, (os64_gui_rect_t){0, 0, 2, 2}, (os64_gui_rect_t){-2, -2, 4, 4}, false, src, 4, 4);
    const uint32_t shifted[4] = {src[10], src[11], src[14], src[15]};
    expect("picture: a box hanging off the top left", pixels_are(edge, shifted, 4), NULL);

    // Tiling a 2x2 picture {A,B / C,A} unscaled from the area's corner.
    uint32_t tiles[25] = {0};
    yonder_tile_picture(tiles, 5, (os64_gui_rect_t){0, 0, 5, 5}, (os64_gui_rect_t){0, 0, 5, 5},
                        (os64_gui_rect_t){0, 0, 2, 2}, true, true, false, two, 2, 2);
    const uint32_t tiled[25] = {A, B, A, B, A, C, A, C, A, C, A, B, A, B, A,
                                C, A, C, A, C, A, B, A, B, A};
    expect("tile: whole copies, and the last cut at the area's edge", pixels_are(tiles, tiled, 25),
           NULL);

    // The same area, clipped to its middle: the tiles stay where the
    // origin put them, and nothing outside the clip is written.
    uint32_t cut_tiles[25] = {0};
    yonder_tile_picture(cut_tiles, 5, (os64_gui_rect_t){1, 1, 3, 3}, (os64_gui_rect_t){0, 0, 5, 5},
                        (os64_gui_rect_t){0, 0, 2, 2}, true, true, false, two, 2, 2);
    bool inside_same = true, outside_clear = true;
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++) {
            bool in = x >= 1 && x < 4 && y >= 1 && y < 4;
            if (in && cut_tiles[y * 5 + x] != tiled[y * 5 + x])
                inside_same = false;
            if (!in && cut_tiles[y * 5 + x] != 0)
                outside_clear = false;
        }
    expect("tile: a clip does not move the tiles", inside_same && outside_clear, NULL);

    // An area as wide as an int32_t says, starting one pixel in: its right
    // edge is past INT32_MAX, which int32 sums wrapped to a draw of nothing.
    uint32_t wide_tiles[25] = {0};
    yonder_tile_picture(wide_tiles, 5, (os64_gui_rect_t){0, 0, 5, 5},
                        (os64_gui_rect_t){1, 0, INT32_MAX, 5}, (os64_gui_rect_t){0, 0, 2, 2}, true,
                        true, false, two, 2, 2);
    bool wide_ok = true;
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
            if (wide_tiles[y * 5 + x] != (x >= 1 ? tiled[y * 5 + x] : 0))
                wide_ok = false;
    expect("tile: an area reaching past INT32_MAX still covers the clip", wide_ok, NULL);

    // An origin above and left of the area (the canvas's, for a body the
    // view is scrolled into): the area starts one pixel into a tile.
    uint32_t shifted_tiles[4] = {0};
    yonder_tile_picture(shifted_tiles, 2, (os64_gui_rect_t){0, 0, 2, 2}, (os64_gui_rect_t){0, 0, 2, 2},
                        (os64_gui_rect_t){-1, -3, 2, 2}, true, true, false, two, 2, 2);
    const uint32_t from_origin[4] = {A, C, B, A};
    expect("tile: an origin outside the area lays the tiles from there",
           pixels_are(shifted_tiles, from_origin, 4), NULL);

    uint32_t over_blue[2] = {0xff0000ffu, 0xff0000ffu};
    const uint32_t half_and_none[2] = {0x80ff0000u, 0x00ff0000u};
    yonder_tile_picture(over_blue, 2, (os64_gui_rect_t){0, 0, 2, 1}, (os64_gui_rect_t){0, 0, 2, 1},
                        (os64_gui_rect_t){0, 0, 2, 1}, true, true, false, half_and_none, 2, 1);
    expect("tile: blended by alpha over the colour beneath",
           over_blue[0] == 0xff80007fu && over_blue[1] == 0xff0000ffu, NULL);

    // repeat-x at (1, 2) in a 5x5 area: the one row of tiles, two tall
    // from y 2, laid from x 1 both ways; no-repeat: the one copy at (1, 2).
    uint32_t strip[25] = {0};
    yonder_tile_picture(strip, 5, (os64_gui_rect_t){0, 0, 5, 5}, (os64_gui_rect_t){0, 0, 5, 5},
                        (os64_gui_rect_t){1, 2, 2, 2}, true, false, false, two, 2, 2);
    const uint32_t row_want[25] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, B, A, B, A, B,
                                   A, C, A, C, A, 0, 0, 0, 0, 0};
    expect("tile: repeat-x draws one row of tiles", pixels_are(strip, row_want, 25), NULL);
    uint32_t single[25] = {0};
    yonder_tile_picture(single, 5, (os64_gui_rect_t){0, 0, 5, 5}, (os64_gui_rect_t){0, 0, 5, 5},
                        (os64_gui_rect_t){1, 2, 2, 2}, false, false, false, two, 2, 2);
    const uint32_t single_want[25] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, A, B, 0, 0,
                                   0, C, A, 0, 0, 0, 0, 0, 0, 0};
    expect("tile: no-repeat draws the one copy", pixels_are(single, single_want, 25), NULL);

    // The 2x2 picture in a 4x4 tile: column u shows source
    // floor((u + 1/2) * 2 / 4), so 0, 0, 1, 1 — each pixel doubled.
    uint32_t scaled[16] = {0};
    yonder_tile_picture(scaled, 4, (os64_gui_rect_t){0, 0, 4, 4}, (os64_gui_rect_t){0, 0, 4, 4},
                        (os64_gui_rect_t){0, 0, 4, 4}, true, true, false, two, 2, 2);
    const uint32_t each_doubled[16] = {A, A, B, B, A, A, B, B, C, C, A, A, C, C, A, A};
    expect("tile: a copy scaled to its tile", pixels_are(scaled, each_doubled, 16), NULL);

    // Smoothed: black then white, 2 px drawn 4 px wide. Column u falls at
    // (u + 1/2) * 2 / 4 - 1/2 = -1/4, 1/4, 3/4, 5/4 of the source: the ends
    // held to the edge pixels, the middle two a quarter and three quarters
    // of the way from black to white (63.75 and 191.25, to 0x40 and 0xbf).
    const uint32_t bw[2] = {0xff000000u, 0xffffffffu};
    uint32_t ramp[4] = {0};
    yonder_draw_picture(ramp, 4, (os64_gui_rect_t){0, 0, 4, 1}, (os64_gui_rect_t){0, 0, 4, 1},
                        true, bw, 2, 1);
    const uint32_t ramp_want[4] = {0xff000000u, 0xff404040u, 0xffbfbfbfu, 0xffffffffu};
    expect("smooth: a scaled picture mixes the pixels round each one", pixels_are(ramp, ramp_want, 4),
           NULL);
    // Red then nothing, over black: a transparent neighbour lends no colour,
    // only less alpha — a quarter of the way along is red at 191/255, laid
    // over black as 0xbf0000, where mixing its black in would give 0x8f.
    const uint32_t red_none[2] = {0xffff0000u, 0x00000000u};
    uint32_t fade[4] = {0};
    yonder_tile_picture(fade, 4, (os64_gui_rect_t){0, 0, 4, 1}, (os64_gui_rect_t){0, 0, 4, 1},
                        (os64_gui_rect_t){0, 0, 4, 1}, false, false, true, red_none, 2, 1);
    const uint32_t fade_want[4] = {0xffff0000u, 0xffbf0000u, 0xff400000u, 0};
    expect("smooth: a transparent neighbour lends no colour", pixels_are(fade, fade_want, 4), NULL);
    // At its own size a picture is copied, smoothing or not.
    uint32_t same[4] = {0};
    yonder_draw_picture(same, 2, (os64_gui_rect_t){0, 0, 2, 2}, (os64_gui_rect_t){0, 0, 2, 2}, true,
                        two, 2, 2);
    expect("smooth: unscaled is the picture itself", pixels_are(same, two, 4), NULL);

    // A sheet's picture, 20x10, behind a 100x50 box at (10, 20):
    // `right 10px center repeat-x` is calc(100% - 10px) and 50%, so x is
    // 10 + (100 - 20) - 10 = 80 and y is 20 + (50 - 10) / 2 = 40.
    flow_layer_t st;
    memset(&st, 0, sizeof(st));
    st.position[0] = (flow_length_t){FLOW_LENGTH_PERCENT, 100 * 64, -10 * 64};
    st.position[1] = (flow_length_t){FLOW_LENGTH_PERCENT, 50 * 64, 0};
    st.repeat = FLOW_REPEAT_X;
    os64_gui_rect_t area = {10, 20, 100, 50};
    yonder_tile_t t;
    bool got = yonder_background_tile(&st, area, 20, 10, &t);
    expect("background: a position from the far edge, and one centred",
           got && t.at.x == 80 && t.at.y == 40 && t.at.w == 20 && t.at.h == 10, NULL);
    expect("background: repeat-x repeats across and not down", t.repeat_x && !t.repeat_y, NULL);
}

static bool tile_is(const flow_layer_t *s, os64_gui_rect_t area, uint32_t iw, uint32_t ih,
                    int32_t x, int32_t y, int32_t w, int32_t h)
{
    yonder_tile_t t;
    return yonder_background_tile(s, area, iw, ih, &t) && t.at.x == x && t.at.y == y &&
           t.at.w == w && t.at.h == h;
}

// background-size worked by hand (Backgrounds 3 § 3.9), a 20x10 picture
// in a 100x80 origin box at (10, 20) unless said otherwise.
static void background_size_cases(void)
{
    os64_gui_rect_t area = {10, 20, 100, 80};
    flow_layer_t st;
    memset(&st, 0, sizeof(st));
    st.fit = FLOW_FIT_COVER;
    // Covering: the larger scale, 80 / 10 = 8 over 100 / 20 = 5.
    expect("size: cover scales to the side that covers",
           tile_is(&st, area, 20, 10, 10, 20, 160, 80), NULL);
    // Fitting: the smaller, 5, and centred, 20 + (80 - 50) / 2 = 35.
    st.fit = FLOW_FIT_CONTAIN;
    st.position[0] = st.position[1] =
        (flow_length_t){FLOW_LENGTH_PERCENT, 50 * 64, 0};
    expect("size: contain scales to fit, and is placed in the room left",
           tile_is(&st, area, 20, 10, 10, 35, 100, 50), NULL);
    // A gradient has no shape of its own: covering is the box.
    expect("size: cover with no picture size is the origin box",
           tile_is(&st, area, 0, 0, 10, 20, 100, 80), NULL);

    memset(&st, 0, sizeof(st));
    st.size[0] = (flow_length_t){FLOW_LENGTH_PX, 40 * 64, 0};
    expect("size: a width, the height keeping the picture's shape",
           tile_is(&st, area, 20, 10, 10, 20, 40, 20), NULL);
    expect("size: a width, with no shape the height is the box's",
           tile_is(&st, area, 0, 0, 10, 20, 40, 80), NULL);
    st.size[0] = (flow_length_t){FLOW_LENGTH_AUTO, 0, 0};
    st.size[1] = (flow_length_t){FLOW_LENGTH_PERCENT, 25 * 64, 0};
    expect("size: a height of 25% is 20, the width 40 to keep the shape",
           tile_is(&st, area, 20, 10, 10, 20, 40, 20), NULL);
    st.size[0] = (flow_length_t){FLOW_LENGTH_PERCENT, 50 * 64, 0};
    st.size[1] = (flow_length_t){FLOW_LENGTH_PX, 10 * 64, 0};
    expect("size: both written, both taken", tile_is(&st, area, 20, 10, 10, 20, 50, 10), NULL);
    st.size[1] = (flow_length_t){FLOW_LENGTH_PX, 0, 0};
    yonder_tile_t t;
    expect("size: a side of 0 draws nothing", !yonder_background_tile(&st, area, 20, 10, &t),
           NULL);
    st.size[0] = st.size[1] = (flow_length_t){FLOW_LENGTH_AUTO, 0, 0};
    expect("size: auto auto is the picture's own",
           tile_is(&st, area, 20, 10, 10, 20, 20, 10), NULL);
    expect("size: auto auto with no picture size is the box",
           tile_is(&st, area, 0, 0, 10, 20, 100, 80), NULL);
    // A huge picture scaled from a huge box stays inside an int32_t.
    st.fit = FLOW_FIT_COVER;
    expect("size: a scale past INT32_MAX is held there",
           tile_is(&st, (os64_gui_rect_t){0, 0, INT32_MAX, 1}, 1, 65536, 0, 0, INT32_MAX,
                   INT32_MAX),
           NULL);

    // The boxes: borders of 2, padding of 3, round a 100x80 box at (10, 20).
    flow_style_t edged;
    memset(&edged, 0, sizeof(edged));
    for (int k = 0; k < 4; k++) {
        edged.border_width[k] = 2 * 64;
        edged.padding[k] = (flow_length_t){FLOW_LENGTH_PX, 3 * 64, 0};
    }
    flow_box_t box;
    memset(&box, 0, sizeof(box));
    box.style = &edged;
    os64_gui_rect_t pad = yonder_box_edge(&box, area, FLOW_EDGE_PADDING);
    os64_gui_rect_t content = yonder_box_edge(&box, area, FLOW_EDGE_CONTENT);
    os64_gui_rect_t border = yonder_box_edge(&box, area, FLOW_EDGE_BORDER);
    expect("edge: the padding box is inside the borders",
           pad.x == 12 && pad.y == 22 && pad.w == 96 && pad.h == 76, NULL);
    expect("edge: the content box is inside the padding too",
           content.x == 15 && content.y == 25 && content.w == 90 && content.h == 70, NULL);
    expect("edge: the border box is the box",
           border.x == 10 && border.y == 20 && border.w == 100 && border.h == 80, NULL);
}

// ── A download's name (download.h) ───────────────────────────────────────

static void expect_text(const char *name, const char *got, const char *want);

static const char *download_name(const char *disposition, const char *url)
{
    static char out[YONDER_DOWNLOAD_NAME_MAX];
    yonder_download_name(disposition, url, out, sizeof(out));
    return out;
}

// Taken: the names in a NULL-ended list.
static bool download_taken(void *ctx, const char *name)
{
    for (const char *const *t = ctx; *t != NULL; t++)
        if (strcmp(*t, name) == 0)
            return true;
    return false;
}

static void download_cases(void)
{
    expect_text("download: the address's last segment", download_name(NULL, "https://a.example/files/tool.zip"),
                "tool.zip");
    expect_text("download: the query and fragment are not the name",
                download_name("", "https://a.example/get/report.pdf?id=7#page=2"), "report.pdf");
    expect_text("download: the segment percent-decoded", download_name(NULL, "http://a.example/my%20notes.txt"),
                "my notes.txt");
    expect_text("download: an address ending in / is download", download_name(NULL, "http://a.example/dir/"),
                "download");
    expect_text("download: an address with no path is download", download_name(NULL, "http://a.example"),
                "download");
    expect_text("download: an address with no authority is download",
                download_name(NULL, "data:application/zip;base64,AAAA"), "download");
    expect_text("download: Content-Disposition's quoted filename wins",
                download_name("attachment; filename=\"Q3 report.pdf\"", "https://a.example/dl?id=9"),
                "Q3 report.pdf");
    expect_text("download: a token filename, the parameter name in any case",
                download_name("attachment; FileName=data.csv ; size=10", "https://a.example/x"), "data.csv");
    expect_text("download: a quoted-string's backslash escapes",
                download_name("attachment; filename=\"a\\\"b.txt\"", NULL), "a\"b.txt");
    expect_text("download: filename* in UTF-8 wins over filename",
                download_name("attachment; filename=\"plain.txt\"; filename*=UTF-8''na%C3%AFve.txt", NULL),
                "na\xC3\xAFve.txt");
    expect_text("download: filename* in another charset is passed over",
                download_name("attachment; filename*=iso-8859-1''caf%E9.txt; filename=cafe.txt", NULL), "cafe.txt");
    expect_text("download: a parameter that only ends in filename is not it",
                download_name("attachment; xfilename=no.txt", "https://a.example/yes.txt"), "yes.txt");
    expect_text("download: a path in the name is stripped to its last part",
                download_name("attachment; filename=\"../../etc/passwd\"", NULL), "passwd");
    expect_text("download: a Windows path too", download_name("attachment; filename=\"C:\\\\tmp\\\\a.exe\"", NULL),
                "a.exe");
    expect_text("download: leading dots go", download_name("attachment; filename=\".bashrc\"", NULL), "bashrc");
    expect_text("download: trailing dots and spaces go", download_name("attachment; filename=\"a.txt. . \"", NULL),
                "a.txt");
    expect_text("download: a control byte becomes _", download_name(NULL, "http://a.example/a%0Ab%00c.txt"),
                "a_b_c.txt");
    expect_text("download: a name of only dots falls back to the address",
                download_name("attachment; filename=\"...\"", "http://a.example/real.bin"), "real.bin");
    char long_name[300], want[YONDER_DOWNLOAD_NAME_MAX];
    memset(long_name, 'a', 200);
    strcpy(long_name + 200, ".tar");
    char url[400];
    snprintf(url, sizeof(url), "http://a.example/%s", long_name);
    memset(want, 'a', YONDER_DOWNLOAD_NAME_MAX - 1 - 4);
    strcpy(want + YONDER_DOWNLOAD_NAME_MAX - 1 - 4, ".tar");
    expect_text("download: too long is cut, its extension kept", download_name(NULL, url), want);
    // 0xC3 0xA9 is one character. 121 b's, two of them and .bin is 130
    // bytes; 127 fit, so the cut falls after the first: whole, it stays.
    memset(long_name, 'b', 121);
    strcpy(long_name + 121, "%C3%A9%C3%A9.bin");
    snprintf(url, sizeof(url), "http://a.example/%s", long_name);
    memset(want, 'b', 121);
    strcpy(want + 121, "\xC3\xA9.bin");
    expect_text("download: a cut keeps a UTF-8 sequence that fits whole", download_name(NULL, url), want);
    // 122 b's: the cut would fall between 0xC3 and 0xA9, so the character goes.
    memset(long_name, 'b', 122);
    strcpy(long_name + 122, "%C3%A9%C3%A9.bin");
    snprintf(url, sizeof(url), "http://a.example/%s", long_name);
    memset(want, 'b', 122);
    strcpy(want + 122, ".bin");
    expect_text("download: a cut never splits a UTF-8 sequence", download_name(NULL, url), want);

    char out[YONDER_DOWNLOAD_NAME_MAX];
    const char *none[] = {NULL};
    expect("download: a free name stands",
           yonder_download_free_name("a.zip", download_taken, none, out, sizeof(out)) && strcmp(out, "a.zip") == 0,
           out);
    const char *two[] = {"a.tar.gz", "a-1.tar.gz", NULL};
    expect("download: taken names count up before the first dot",
           yonder_download_free_name("a.tar.gz", download_taken, two, out, sizeof(out)) &&
               strcmp(out, "a-2.tar.gz") == 0,
           out);
    const char *dotless[] = {"README", NULL};
    expect("download: a name with no dot is numbered at its end",
           yonder_download_free_name("README", download_taken, dotless, out, sizeof(out)) &&
               strcmp(out, "README-1") == 0,
           out);
    char size[32];
    yonder_download_size(1, size, sizeof(size));
    expect_text("download: one byte is 1 KB, never 0", size, "1 KB");
    yonder_download_size(1024 * 1024 - 1, size, sizeof(size));
    expect_text("download: under a megabyte is kilobytes", size, "1024 KB");
    yonder_download_size(1024 * 1024 * 3 + 1024 * 1024 / 2, size, sizeof(size));
    expect_text("download: megabytes to a tenth", size, "3.5 MB");
    yonder_download_size((uint64_t)1024 * 1024 * 1024 * 2 - 1, size, sizeof(size));
    expect_text("download: cut, never rounded up past what there is", size, "1.9 GB");
    char tiny[8];
    expect("download: a numbered name that will not fit is refused",
           !yonder_download_free_name("abcde.z", download_taken, (const char *[]){"abcde.z", NULL}, tiny,
                                      sizeof(tiny)),
           NULL);

    yonder_download_pace_t pace = {0};
    uint64_t rate = 0;
    expect("download: no notes, no speed", !yonder_download_pace_rate(&pace, 1000, 0, &rate), NULL);
    yonder_download_pace_note(&pace, 1000, 0);
    expect("download: under a second, no speed yet", !yonder_download_pace_rate(&pace, 1900, 900, &rate), NULL);
    expect("download: a second at a kilobyte a millisecond",
           yonder_download_pace_rate(&pace, 2000, 1000000, &rate) && rate == 1000000, NULL);
    for (uint64_t t = 1100; t <= 2000; t += 100)
        yonder_download_pace_note(&pace, t, (t - 1000) * 1000);
    expect("download: notes closer than a quarter second are not kept", pace.count == 4, NULL);
    // A steady megabyte a second for ten seconds, then twice that for five:
    // the window has forgotten the first pace.
    yonder_download_pace_t steady = {0};
    for (uint64_t t = 0; t <= 10000; t += 250)
        yonder_download_pace_note(&steady, t, t * 1000);
    for (uint64_t t = 10250; t <= 15000; t += 250)
        yonder_download_pace_note(&steady, t, 10000000 + (t - 10000) * 2000);
    expect("download: the speed is the last five seconds'",
           yonder_download_pace_rate(&steady, 15000, 20000000, &rate) && rate == 2000000, NULL);
    expect("download: a stall falls to nothing once the window has passed it",
           yonder_download_pace_rate(&steady, 21000, 20000000, &rate) && rate == 0, NULL);
    yonder_download_pace_note(&steady, 30000, 20000000);
    expect("download: one note in the window reaches back to the one before it",
           yonder_download_pace_rate(&steady, 30500, 20500000, &rate) && rate == 500000 * 1000 / 15500, NULL);

    char left[32];
    expect("download: no speed, no time left", !yonder_download_left(10, 0, left, sizeof(left)), NULL);
    yonder_download_left(41, 1, left, sizeof(left));
    expect_text("download: seconds", left, "41 s left");
    yonder_download_left(61, 1, left, sizeof(left));
    expect_text("download: minutes, rounded up", left, "2 min left");
    yonder_download_left(60 * 60 - 1, 1, left, sizeof(left));
    expect_text("download: an hour less a second is an hour", left, "1 h left");
    yonder_download_left(12 * 60, 1, left, sizeof(left));
    expect_text("download: whole minutes", left, "12 min left");
    yonder_download_left((uint64_t)6 << 30, 8 << 20, left, sizeof(left));
    expect_text("download: six gigabytes at eight megabytes a second", left, "13 min left");
    yonder_download_left(2 * 3600 + 5 * 60, 1, left, sizeof(left));
    expect_text("download: hours and minutes", left, "2 h 5 min left");
}

// ── The question bar: no click made before a question may answer it ─────

static void bar_cases(void)
{
    yonder_bar_t b = {0};
    yonder_bar_raise(&b, 1);
    yonder_bar_press(&b, BAR_ON_YES);
    yonder_bar_settled(&b);
    expect("bar: a press before arming does not complete after it",
           yonder_bar_release(&b, BAR_ON_YES) == BAR_NOTHING, NULL);

    yonder_bar_press(&b, BAR_ON_YES);
    expect("bar: a press and release on Yes while armed is Yes",
           yonder_bar_release(&b, BAR_ON_YES) == BAR_YES, NULL);

    yonder_bar_press(&b, BAR_ON_YES);
    expect("bar: pressed on Yes, released on No, is nothing",
           yonder_bar_release(&b, BAR_ON_NO) == BAR_NOTHING, NULL);

    yonder_bar_press(&b, BAR_ON_NO);
    expect("bar: No is No", yonder_bar_release(&b, BAR_ON_NO) == BAR_NO, NULL);

    yonder_bar_press(&b, BAR_ON_YES);
    yonder_bar_raise(&b, 2);
    yonder_bar_settled(&b);
    expect("bar: a press on a replaced question does not answer the new one",
           yonder_bar_release(&b, BAR_ON_YES) == BAR_NOTHING && b.number == 2, NULL);

    yonder_bar_t down = {0};
    yonder_bar_press(&down, BAR_ON_YES);
    yonder_bar_raise(&down, 3);
    expect("bar: a press queued before the question existed is nothing",
           yonder_bar_release(&down, BAR_ON_YES) == BAR_NOTHING, NULL);
    expect("bar: Escape is No, armed or not", yonder_bar_escape(&down) == BAR_NO, NULL);
    yonder_bar_lower(&down);
    expect("bar: nothing answers a bar that is down",
           yonder_bar_escape(&down) == BAR_NOTHING &&
               yonder_bar_press(&down, BAR_ON_YES) == BAR_NOTHING &&
               yonder_bar_release(&down, BAR_ON_YES) == BAR_NOTHING, NULL);
}

// ── The mailbox ─────────────────────────────────────────────────────────

static bool never(void *ctx)
{
    (void)ctx;
    return false;
}

static bool always(void *ctx)
{
    (void)ctx;
    return true;
}

// How many descriptors this process has open: a mailbox holds its pipe's
// two until the last reference goes.
static int open_fds(void)
{
    int n = 0;
    for (int fd = 0; fd < 1024; fd++)
        n += fcntl(fd, F_GETFD) != -1;
    return n;
}

static void mail_cases(void)
{
    // The window and the job each hold it, and a drop does not know which
    // side it is: it stays whole, pipe and all, until the last one goes.
    {
        size_t before = live;
        int fds = open_fds();
        yonder_mail_t *m = yonder_mail_new(7);
        yonder_mail_hold(m);
        yonder_mail_progress(m, "first");
        yonder_mail_drop(m);
        char out[64];
        bool still = yonder_mail_take_progress(m, out, sizeof(out)) &&
                     strcmp(out, "first") == 0 && yonder_mail_generation(m) == 7 &&
                     open_fds() == fds + 2;
        yonder_mail_drop(m);
        expect("mail: whole until the last holder lets go, then gone with its pipe",
               still && live == before && open_fds() == fds, NULL);
    }

    yonder_mail_t *m = yonder_mail_new(1);
    yonder_mail_progress(m, "reading 64 KB");
    yonder_mail_progress(m, "reading 128 KB");
    char out[64];
    expect("mail: progress is the newest, once",
           yonder_mail_take_progress(m, out, sizeof(out)) && strcmp(out, "reading 128 KB") == 0 &&
               !yonder_mail_take_progress(m, out, sizeof(out)), NULL);

    uint32_t first = yonder_mail_ask(m, "one?");
    uint32_t second = yonder_mail_ask(m, "two?");
    uint32_t number = 0;
    expect("mail: the window sees the newest question and its number",
           yonder_mail_take_question(m, &number, out, sizeof(out)) && number == second &&
               strcmp(out, "two?") == 0, NULL);
    yonder_mail_answer(m, first, true);     // an answer to another question
    yonder_mail_answer(m, second, false);
    expect("mail: only the answer to THIS question is taken",
           yonder_mail_wait(m, second, never, NULL) == false, NULL);
    yonder_mail_answer(m, second, true);
    expect("mail: yes is yes", yonder_mail_wait(m, second, never, NULL) == true, NULL);
    expect("mail: a cancelled wait ends, unanswered, as No",
           yonder_mail_wait(m, yonder_mail_ask(m, "three?"), always, NULL) == false, NULL);
    yonder_mail_drop(m);
}

// ── A paint costs the viewport it paints ────────────────────────────────

static void paint_hung(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL a paint walked what the viewport does not show\n";
    (void)!write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

// Sizes a page may ask for and no viewport shows: a border a million
// pixels thick, and a bullet a third of a font sixty `<big>`s deep (which
// holds at the million-pixel clamp). Painted under UBSan and an alarm, each
// must finish, and every fill must stay inside the viewport: a painter that
// walked every row, or multiplied in 32 bits, fails one or the other.
static void cost_cases(void)
{
    static char big[4096];
    size_t at = (size_t)snprintf(big, sizeof(big), "<!doctype html>");
    for (int i = 0; i < 60; i++)
        at += (size_t)snprintf(big + at, sizeof(big) - at, "<big>");
    snprintf(big + at, sizeof(big) - at, "<ul><li>x</ul>");
    static const char *const pages[] = {
        "<!doctype html><table border=1000000><tr><td>x</table>",
        "<!doctype html><hr size=1000000 noshade>",
        big,
    };
    // A line at the foot of a page as tall as an int32_t says: 2147
    // pictures a million pixels tall (each line 1000006 with its strut) and
    // one of 470741 put the text's top at INT32_MAX - 8, so its box meets
    // the view while its baseline is held at INT32_MAX. A decoration's
    // offset from that baseline must not wrap.
    size_t tall_cap = 2148 * 48 + 64, tall_at = 0;
    char *tall = malloc(tall_cap);
    tall_at += (size_t)snprintf(tall, tall_cap, "<!doctype html>");
    for (int i = 0; i < 2147; i++)
        tall_at += (size_t)snprintf(tall + tall_at, tall_cap - tall_at,
                                    "<img src=known.png height=1000000><br>");
    snprintf(tall + tall_at, tall_cap - tall_at,
             "<img src=known.png height=470741><br><u>x</u><s>y</s>");
    bool tall_escaped = false;
    signal(SIGALRM, paint_hung);
    alarm(60);
    char *foot = paint_of(tall, strlen(tall), 800, (os64_gui_rect_t){0, INT32_MAX - 600, 800, 600},
                          &tall_escaped, false);
    alarm(0);
    expect("decorations at the foot of a page past int32_t paint without wrapping",
           foot != NULL && !tall_escaped, NULL);
    free(foot);
    free(tall);
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        bool escaped = false;
        alarm(60);
        char *got = paint_of(pages[i], strlen(pages[i]), 800, (os64_gui_rect_t){0, 0, 800, 600},
                             &escaped, false);
        alarm(0);
        expect("a hostile size paints the viewport and no more", got != NULL && !escaped,
               pages[i]);
        free(got);
    }
}

// ── The corpus ──────────────────────────────────────────────────────────

static const char *const kCorpus[] = {
    "68k-news", "example", "floodgap", "hacker-news", "textfiles-computers", "wikipedia-html",
};

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf != NULL) {
        buf[n] = '\0';
        *len = (size_t)n;
    }
    return buf;
}

// The first screen of each page at 800x600, and a screen from its middle:
// what a change moved, as a diff to read.
static void corpus(bool write)
{
    size_t same = 0;
    for (size_t i = 0; i < sizeof(kCorpus) / sizeof(kCorpus[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "tools/html_corpus/%s.html", kCorpus[i]);
        size_t len = 0;
        char *html = slurp(path, &len);
        if (html == NULL) {
            expect("corpus: page present", false, path);
            continue;
        }
        bool escaped = false;
        char *top = paint_of(html, len, 800, (os64_gui_rect_t){0, 0, 800, 600}, &escaped, false);
        char *mid = paint_of(html, len, 800, (os64_gui_rect_t){0, 3000, 800, 600}, &escaped, false);
        expect("corpus: every fill inside the viewport", !escaped, kCorpus[i]);
        size_t n = strlen(top) + strlen(mid) + 64;
        char *both = malloc(n);
        snprintf(both, n, "%s-- 3000\n%s", top, mid);
        snprintf(path, sizeof(path), "tools/html_corpus/%s.paint", kCorpus[i]);
        if (write) {
            FILE *f = fopen(path, "wb");
            fputs(both, f);
            fclose(f);
        } else {
            size_t had_len = 0;
            char *had = slurp(path, &had_len);
            bool ok = had != NULL && strcmp(had, both) == 0;
            same += ok;
            expect("corpus: the page paints as recorded", ok, path);
            free(had);
        }
        free(both);
        free(top);
        free(mid);
        free(html);
    }
    if (!write)
        printf("yonder corpus: %zu paints match\n", same);
}

// ── Who yonder says it is ───────────────────────────────────────────────

static void expect_text(const char *name, const char *got, const char *want)
{
    char detail[1024];
    snprintf(detail, sizeof(detail), "got \"%s\"\nwant \"%s\"", got != NULL ? got : "(null)", want);
    expect(name, got != NULL && strcmp(got, want) == 0, detail);
}

// A preset by the name a person picks it by.
static const yonder_agent_preset_t *preset_named(const char *name)
{
    for (size_t i = 0; i < yonder_agent_npresets(); i++)
        if (strcmp(yonder_agent_preset(i)->name, name) == 0)
            return yonder_agent_preset(i);
    return NULL;
}

// The navigation block a preset's agent writes for a hop to `to`, by `by`
// from the page at `from`.
static const char *navigation_block(const yonder_agent_preset_t *p, way_initiator_t by, const char *from,
                                    const char *to)
{
    static char out[WAY_IDENTITY_MAX];
    way_metadata_t m;
    way_origin_t f, t;
    way_origin_of(from, 0, &f);
    way_origin_of(to, 0, &t);
    way_metadata_navigation(&m, by, &f);
    way_metadata_hop(&m, &t);
    if (!way_identity_lines(yonder_agent_identity(p->agent), &m, &t, false, out, sizeof(out)))
        snprintf(out, sizeof(out), "<did not fit>");
    return out;
}

// The navigation block a preset's agent writes for the browser's Reload of
// an entry `by` asked for from `from`, to `to` (reload_as replays it).
static const char *reload_block(const yonder_agent_preset_t *p, way_initiator_t by, const char *from, const char *to)
{
    static char out[WAY_IDENTITY_MAX];
    way_metadata_t m;
    way_origin_t f, t;
    way_origin_of(from, 0, &f);
    way_origin_of(to, 0, &t);
    way_metadata_navigation(&m, by, &f);
    m.reload = true;
    way_metadata_hop(&m, &t);
    if (!way_identity_lines(yonder_agent_identity(p->agent), &m, &t, 0, out, sizeof(out)))
        snprintf(out, sizeof(out), "<did not fit>");
    return out;
}

// EACH PRESET BYTE FOR BYTE AGAINST WHAT THAT BROWSER SENT: the W1
// capture files, raw request bytes Chris's Chrome 156 and Firefox 158 on
// Windows sent a local listener (WIRE.md § 1 as built names them). Chrome:
// capture-01-chrome-win-get-root (a typed navigation),
// capture-25-chrome-win-get-clicked (a link clicked),
// capture-26-chrome-win-get-clicked (the browser's Reload of it),
// capture-27-chrome-win-get-root (Back to the typed entry). Firefox on
// Windows: run1-overwritten/capture-09 (a typed navigation), -17 (a link
// clicked), -02 to -08 (the subresources and fetches); its Reload and Back
// are capture-43-firefox-linux-get-clicked and
// capture-44-firefox-linux-get-root, which send the same lines. The navigation blocks are those requests'
// lines that libway writes, in the order they came. Safari's are cited,
// not captured (agent.c says from where), and yonder's own language is the
// same list.
static void agent_identity_cases(void)
{
    const yonder_agent_preset_t *chrome = preset_named("Chrome on Windows");
    const yonder_agent_preset_t *firefox = preset_named("Firefox on Windows");
    const yonder_agent_preset_t *safari = preset_named("Safari on iPhone");
    expect("identity: the three browsers are presets", chrome && firefox && safari, NULL);
    if (!chrome || !firefox || !safari)
        return;
    expect_text("identity: Chrome's agent, captured",
                chrome->agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                               "Chrome/156.0.0.0 Safari/537.36");
    expect_text("identity: Chrome's client hints, captured", chrome->identity.hints,
                "sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n"
                "sec-ch-ua-mobile: ?0\r\n"
                "sec-ch-ua-platform: \"Windows\"\r\n");
    expect_text("identity: Chrome's language, captured", chrome->identity.language, "en-US,en;q=0.9");
    expect_text("identity: Firefox's agent, captured", firefox->agent,
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0");
    expect("identity: Firefox sends no client hints", firefox->identity.hints == NULL, NULL);
    expect_text("identity: Firefox's language, captured", firefox->identity.language, "en-US,en;q=0.9");
    // Firefox's Priority for each destination it was asked for:
    // run1-overwritten/capture-09 and -17 (navigations), -02 (style), -03
    // (script), -04 (image), -05 to -08 (fetch, the cross-site fetch, the
    // preflight and its PUT). No font was asked for, so a font goes without.
    static const char *const firefox_rows[WAY_DESTS] = {
        [WAY_DEST_DOCUMENT] = "u=0, i", [WAY_DEST_STYLE] = "u=2", [WAY_DEST_SCRIPT] = "u=2",
        [WAY_DEST_IMAGE] = "u=5, i", [WAY_DEST_EMPTY] = "u=4",
    };
    for (int d = 0; d < WAY_DESTS; d++) {
        const char *got = firefox->identity.priority[d], *want = firefox_rows[d];
        char name[96];
        snprintf(name, sizeof(name), "identity: Firefox's Priority for destination %d, captured", d);
        expect(name, (got == NULL && want == NULL) || (got != NULL && want != NULL && strcmp(got, want) == 0),
               got != NULL ? got : "(none)");
    }
    expect("identity: Safari sends no client hints", safari->identity.hints == NULL, NULL);
    expect_text("identity: Safari's language, cited", safari->identity.language, "en-US,en;q=0.9");
    const yonder_agent_preset_t *own = yonder_agent_preset(0);
    expect_text("identity: yonder's own asks for English", own->identity.language, "en-US,en;q=0.9");
    bool no_priority = true, costumes = true;
    for (size_t i = 0; i < yonder_agent_npresets(); i++) {
        const yonder_agent_preset_t *p = yonder_agent_preset(i);
        for (int d = 0; d < WAY_DESTS && p != firefox; d++)
            no_priority &= p->identity.priority[d] == NULL;
        if (p != chrome && p != firefox && p != safari && p != own)
            costumes &= p->identity.hints == NULL && p->identity.language == NULL;
        expect("identity: a preset's agent finds its own lines", yonder_agent_identity(p->agent) == &p->identity,
               p->name);
    }
    expect("identity: only Firefox sends a Priority (Chrome sends none over HTTP/1.1)", no_priority, NULL);
    expect("identity: yonder's own and Safari send no hints",
           own->identity.hints == NULL && safari->identity.hints == NULL, NULL);
    expect("identity: Netscape and Lynx claim nothing beyond their strings", costumes, NULL);
    // A default saved as an earlier yonder's preset is that preset now.
    expect("identity: a saved Chrome 154 preset is Chrome's preset now",
           yonder_agent_current("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                                "Chrome/154.0.0.0 Safari/537.36") == chrome->agent, NULL);
    expect("identity: a saved Firefox 156 preset is Firefox's preset now",
           yonder_agent_current("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:156.0) Gecko/20100101 Firefox/156.0") ==
               firefox->agent, NULL);
    const char *typed = "Mozilla/3.0 (compatible)";
    expect("identity: anything else comes back as it is",
           yonder_agent_current(typed) == typed && yonder_agent_current(chrome->agent) == chrome->agent &&
               yonder_agent_current(NULL) == NULL, NULL);
    expect("identity: a typed agent claims nothing", yonder_agent_identity("Mozilla/3.0 (compatible)") == NULL &&
                                                         yonder_agent_identity(NULL) == NULL, NULL);

    // The navigations, as the captures had them: an address given from
    // outside the browser (and Back to it, which sends the same lines,
    // capture-27-chrome-win-get-root), a same-origin link clicked, and the
    // browser's Reload of that, which leads with Cache-Control.
    expect_text("identity: Chrome, the browser's Reload of a clicked link",
                reload_block(chrome, WAY_BY_PERSON, "https://a.test/", "https://a.test/clicked"),
                "Cache-Control: max-age=0\r\n"
                "sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n"
                "sec-ch-ua-mobile: ?0\r\n"
                "sec-ch-ua-platform: \"Windows\"\r\n"
                "Upgrade-Insecure-Requests: 1\r\n"
                "Sec-Fetch-Site: same-origin\r\n"
                "Sec-Fetch-Mode: navigate\r\n"
                "Sec-Fetch-User: ?1\r\n"
                "Sec-Fetch-Dest: document\r\n"
                "Accept-Language: en-US,en;q=0.9\r\n");
    expect_text("identity: Firefox, the browser's Reload of a clicked link (the set)",
                reload_block(firefox, WAY_BY_PERSON, "https://a.test/", "https://a.test/clicked"),
                "Upgrade-Insecure-Requests: 1\r\n"
                "Sec-Fetch-Site: same-origin\r\n"
                "Sec-Fetch-Mode: navigate\r\n"
                "Sec-Fetch-User: ?1\r\n"
                "Sec-Fetch-Dest: document\r\n"
                "Accept-Language: en-US,en;q=0.9\r\n"
                "Priority: u=0, i\r\n");
    expect_text("identity: Chrome, an address given",
                navigation_block(chrome, WAY_BY_BROWSER, NULL, "https://a.test/"),
                "sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n"
                "sec-ch-ua-mobile: ?0\r\n"
                "sec-ch-ua-platform: \"Windows\"\r\n"
                "Upgrade-Insecure-Requests: 1\r\n"
                "Sec-Fetch-Site: none\r\n"
                "Sec-Fetch-Mode: navigate\r\n"
                "Sec-Fetch-User: ?1\r\n"
                "Sec-Fetch-Dest: document\r\n"
                "Accept-Language: en-US,en;q=0.9\r\n");
    expect_text("identity: Chrome, a link clicked",
                navigation_block(chrome, WAY_BY_PERSON, "https://a.test/", "https://a.test/clicked"),
                "sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n"
                "sec-ch-ua-mobile: ?0\r\n"
                "sec-ch-ua-platform: \"Windows\"\r\n"
                "Upgrade-Insecure-Requests: 1\r\n"
                "Sec-Fetch-Site: same-origin\r\n"
                "Sec-Fetch-Mode: navigate\r\n"
                "Sec-Fetch-User: ?1\r\n"
                "Sec-Fetch-Dest: document\r\n"
                "Accept-Language: en-US,en;q=0.9\r\n");
    // Firefox writes the same set in another order (Accept-Language
    // before the rest, Fetch Metadata Dest-Mode-Site-User, Priority last):
    // the set is proved here, the order in whole requests
    // (wire_request_cases).
    expect_text("identity: Firefox, an address given (the set)",
                navigation_block(firefox, WAY_BY_BROWSER, NULL, "https://a.test/"),
                "Upgrade-Insecure-Requests: 1\r\n"
                "Sec-Fetch-Site: none\r\n"
                "Sec-Fetch-Mode: navigate\r\n"
                "Sec-Fetch-User: ?1\r\n"
                "Sec-Fetch-Dest: document\r\n"
                "Accept-Language: en-US,en;q=0.9\r\n"
                "Priority: u=0, i\r\n");

    // THE MARGIN. libway gives a hop's identity WAY_IDENTITY_MAX of
    // libfetch's per-hop cap; the longest preset's longest block (the
    // browser's Reload of a person's same-origin navigation) must fit with
    // room for a brand list that grows, which is the cap's own comment.
    size_t longest = 0;
    for (size_t i = 0; i < yonder_agent_npresets(); i++) {
        const char *b = reload_block(yonder_agent_preset(i), WAY_BY_PERSON, "https://a.test/", "https://a.test/n");
        expect("identity: every preset's block fits", strcmp(b, "<did not fit>") != 0, yonder_agent_preset(i)->name);
        if (strlen(b) > longest)
            longest = strlen(b);
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "the longest is %zu bytes", longest);
    expect("identity: the longest block leaves 150 bytes of WAY_IDENTITY_MAX spare",
           longest + 1 + 150 <= WAY_IDENTITY_MAX, detail);
    expect("identity: the Cookie and Referer lines keep their room beside it",
           OS64_FETCH_HOP_FIELDS_MAX - WAY_IDENTITY_MAX >= WAY_COOKIE_LINE_MAX + WAY_REFERER_LINE_MAX, NULL);
}

// One request as yonder writes it under preset `p`: the preset's lines for
// metadata `m` hopping to `url` (way_identity_lines), the caller's own lines
// `extra` after them as libfetch is handed them, the preset's Accept for
// the destination (or `accept` when the caller sets its own), its order for
// the kind of request, its Connection line, all through libfetch's writer
// (http_request).
static const char *wire_request(const yonder_agent_preset_t *p, const way_metadata_t *m, const char *method,
                                const char *url, const char *accept, const char *extra)
{
    static char out[4096];
    const way_identity_t *id = yonder_agent_identity(p->agent);
    way_origin_t to;
    way_origin_of(url, 0, &to);
    char lines[2048];
    if (!way_identity_lines(id, m, &to, 0, lines, sizeof(lines)))
        return "<lines did not fit>";
    strncat(lines, extra, sizeof(lines) - strlen(lines) - 1);
    http_url_t u;
    if (http_url_parse(url, &u) != HTTP_URL_OK)
        return "<bad url>";
    const char *preset = way_identity_accept(id, m->preflight ? WAY_DEST_EMPTY : m->dest);
    http_request_extras_t x = {
        .user_agent = p->agent,
        .accept = accept != NULL ? accept : preset,
        .extra_headers = lines,
        .method_name = method,
        .body_present = strcmp(method, "PUT") == 0,
        .keep_alive = way_identity_keep_alive(id),
        .order = way_identity_order(id, m),
    };
    if (!http_request(out, sizeof(out), &u, false, &x))
        return "<did not write>";
    return out;
}

// EACH BROWSER'S WHOLE REQUEST, BYTE FOR BYTE, as its capture had it
// (WIRE.md § 2 as built): every line in the order the browser wrote it.
// What yonder sends differently is changed in the expectation, and only
// this: Accept-Encoding is `gzip, identity` (no brotli, DIVERGENCES), and
// Accept is the browser's LESS the types yonder cannot decode (agent.c).
// The captures were made against http://localhost:8765, which yonder does
// not count trustworthy (DIVERGENCES); the requests here go to
// https://localhost:8765, whose Host and request line are the same, so the
// Fetch Metadata the browsers sent is written. A page's own line (the
// captures' `X-W1`) goes after every line the browser's order names, where
// Chrome and Firefox each put it among them: those two requests are matched
// without it, and its place checked apart.
static void wire_request_cases(void)
{
    const yonder_agent_preset_t *chrome = preset_named("Chrome on Windows");
    const yonder_agent_preset_t *firefox = preset_named("Firefox on Windows");
    if (chrome == NULL || firefox == NULL)
        return;
    const char *page = "https://localhost:8765/", *other = "https://127.0.0.1:8765";
    way_origin_t from;
    way_origin_of(page, 0, &from);
    way_metadata_t typed, clicked, reload, style, script, image, same, cross, put;
    way_metadata_navigation(&typed, WAY_BY_BROWSER, NULL);
    way_metadata_hop(&typed, &from);
    way_metadata_navigation(&clicked, WAY_BY_PERSON, &from);
    way_metadata_hop(&clicked, &from);
    reload = clicked;
    reload.reload = true;
    way_metadata_request(&style, &from, WAY_DEST_STYLE, WAY_MODE_NO_CORS);
    way_metadata_request(&script, &from, WAY_DEST_SCRIPT, WAY_MODE_NO_CORS);
    way_metadata_request(&image, &from, WAY_DEST_IMAGE, WAY_MODE_NO_CORS);
    way_metadata_request(&same, &from, WAY_DEST_EMPTY, WAY_MODE_CORS);
    way_metadata_request(&cross, &from, WAY_DEST_EMPTY, WAY_MODE_CORS);
    way_origin_t there;
    way_origin_of(other, 0, &there);
    way_metadata_hop(&same, &from);
    way_metadata_hop(&cross, &there);
    put = cross;
    way_metadata_t preflight = way_metadata_preflight(&cross);
    const char *ref = "Referer: http://localhost:8765/\r\n";
    const char *origin = "Origin: http://localhost:8765\r\n";
    char extra[512];

#define CHROME_HINTS_NAV                                                                       \
    "sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n" \
    "sec-ch-ua-mobile: ?0\r\nsec-ch-ua-platform: \"Windows\"\r\n"
#define CHROME_UA "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
                  "(KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36\r\n"
#define CHROME_NAV_ACCEPT "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
#define CHROME_SUB_HEAD(path, host) "GET " path " HTTP/1.1\r\nHost: " host "\r\nConnection: keep-alive\r\n" \
    "sec-ch-ua-platform: \"Windows\"\r\n" CHROME_UA \
    "sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n" \
    "sec-ch-ua-mobile: ?0\r\n"
#define CHROME_TAIL "Referer: http://localhost:8765/\r\nAccept-Encoding: gzip, identity\r\n" \
                    "Accept-Language: en-US,en;q=0.9\r\n\r\n"

    // capture-01-chrome-win-get-root (and -27, Back to it: the same bytes).
    expect_text("wire: Chrome, an address typed", wire_request(chrome, &typed, "GET", page, NULL, ""),
                "GET / HTTP/1.1\r\nHost: localhost:8765\r\nConnection: keep-alive\r\n" CHROME_HINTS_NAV
                "Upgrade-Insecure-Requests: 1\r\n" CHROME_UA CHROME_NAV_ACCEPT
                "Sec-Fetch-Site: none\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-User: ?1\r\n"
                "Sec-Fetch-Dest: document\r\nAccept-Encoding: gzip, identity\r\n"
                "Accept-Language: en-US,en;q=0.9\r\n\r\n");
    // capture-25-chrome-win-get-clicked.
    expect_text("wire: Chrome, a link clicked",
                wire_request(chrome, &clicked, "GET", "https://localhost:8765/clicked", NULL, ref),
                "GET /clicked HTTP/1.1\r\nHost: localhost:8765\r\nConnection: keep-alive\r\n" CHROME_HINTS_NAV
                "Upgrade-Insecure-Requests: 1\r\n" CHROME_UA CHROME_NAV_ACCEPT
                "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-User: ?1\r\n"
                "Sec-Fetch-Dest: document\r\n" CHROME_TAIL);
    // capture-26-chrome-win-get-clicked: the browser's Reload of it.
    expect_text("wire: Chrome, the browser's Reload",
                wire_request(chrome, &reload, "GET", "https://localhost:8765/clicked", NULL, ref),
                "GET /clicked HTTP/1.1\r\nHost: localhost:8765\r\nConnection: keep-alive\r\n"
                "Cache-Control: max-age=0\r\n" CHROME_HINTS_NAV
                "Upgrade-Insecure-Requests: 1\r\n" CHROME_UA CHROME_NAV_ACCEPT
                "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-User: ?1\r\n"
                "Sec-Fetch-Dest: document\r\n" CHROME_TAIL);
    // capture-02, -03, -04: the sheet, the script, the picture.
    expect_text("wire: Chrome, a sheet",
                wire_request(chrome, &style, "GET", "https://localhost:8765/style.css", NULL, ref),
                CHROME_SUB_HEAD("/style.css", "localhost:8765") "Accept: text/css,*/*;q=0.1\r\n"
                "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: no-cors\r\nSec-Fetch-Dest: style\r\n" CHROME_TAIL);
    expect_text("wire: Chrome, a script",
                wire_request(chrome, &script, "GET", "https://localhost:8765/script.js", NULL, ref),
                CHROME_SUB_HEAD("/script.js", "localhost:8765") "Accept: */*\r\n"
                "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: no-cors\r\nSec-Fetch-Dest: script\r\n" CHROME_TAIL);
    expect_text("wire: Chrome, a picture",
                wire_request(chrome, &image, "GET", "https://localhost:8765/image.png", NULL, ref),
                CHROME_SUB_HEAD("/image.png", "localhost:8765") "Accept: image/*,*/*;q=0.8\r\n"
                "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: no-cors\r\nSec-Fetch-Dest: image\r\n" CHROME_TAIL);
    // capture-05 and -06: a fetch to its own origin, and a cross-site one,
    // whose Origin the fetch job adds.
    expect_text("wire: Chrome, a same-origin fetch",
                wire_request(chrome, &same, "GET", "https://localhost:8765/fetch-same", NULL, ref),
                CHROME_SUB_HEAD("/fetch-same", "localhost:8765") "Accept: */*\r\n"
                "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Dest: empty\r\n" CHROME_TAIL);
    snprintf(extra, sizeof(extra), "%s%s", ref, origin);
    expect_text("wire: Chrome, a cross-site fetch",
                wire_request(chrome, &cross, "GET", "https://127.0.0.1:8765/fetch-cross", NULL, extra),
                CHROME_SUB_HEAD("/fetch-cross", "127.0.0.1:8765") "Accept: */*\r\n"
                "Origin: http://localhost:8765\r\n"
                "Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Dest: empty\r\n" CHROME_TAIL);
    // capture-07: its preflight.
    snprintf(extra, sizeof(extra), "Access-Control-Request-Method: PUT\r\nAccess-Control-Request-Headers: x-w1\r\n%s%s",
             origin, ref);
    expect_text("wire: Chrome, a preflight",
                wire_request(chrome, &preflight, "OPTIONS", "https://127.0.0.1:8765/fetch-cross-preflight", "*/*",
                             extra),
                "OPTIONS /fetch-cross-preflight HTTP/1.1\r\nHost: 127.0.0.1:8765\r\nConnection: keep-alive\r\n"
                "Accept: */*\r\nAccess-Control-Request-Method: PUT\r\nAccess-Control-Request-Headers: x-w1\r\n"
                "Origin: http://localhost:8765\r\n" CHROME_UA
                "Sec-Fetch-Mode: cors\r\nSec-Fetch-Site: cross-site\r\nSec-Fetch-Dest: empty\r\n" CHROME_TAIL);
    // capture-08: the PUT, matched without the page's X-W1, which goes last.
    snprintf(extra, sizeof(extra), "%s%s", ref, origin);
    expect_text("wire: Chrome, the PUT the preflight allowed",
                wire_request(chrome, &put, "PUT", "https://127.0.0.1:8765/fetch-cross-preflight", NULL, extra),
                "PUT /fetch-cross-preflight HTTP/1.1\r\nHost: 127.0.0.1:8765\r\nConnection: keep-alive\r\n"
                "Content-Length: 0\r\nsec-ch-ua-platform: \"Windows\"\r\n" CHROME_UA
                "sec-ch-ua: \"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Google Chrome\";v=\"156\"\r\n"
                "sec-ch-ua-mobile: ?0\r\nAccept: */*\r\nOrigin: http://localhost:8765\r\n"
                "Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Dest: empty\r\n" CHROME_TAIL);
    snprintf(extra, sizeof(extra), "X-W1: 1\r\n%s%s", ref, origin);
    const char *with_page = wire_request(chrome, &put, "PUT", "https://127.0.0.1:8765/fetch-cross-preflight",
                                         NULL, extra);
    expect("wire: a line the order does not name goes after those it does",
           strstr(with_page, "Accept-Language: en-US,en;q=0.9\r\nX-W1: 1\r\n\r\n") != NULL, with_page);

#define FIREFOX_UA "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0\r\n"
#define FIREFOX_LANG_ENC "Accept-Language: en-US,en;q=0.9\r\nAccept-Encoding: gzip, identity\r\n"
#define FIREFOX_NAV_ACCEPT "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"

    // run1-overwritten/capture-09: an address typed.
    expect_text("wire: Firefox, an address typed", wire_request(firefox, &typed, "GET", page, NULL, ""),
                "GET / HTTP/1.1\r\nHost: localhost:8765\r\n" FIREFOX_UA FIREFOX_NAV_ACCEPT FIREFOX_LANG_ENC
                "Connection: keep-alive\r\nUpgrade-Insecure-Requests: 1\r\nSec-Fetch-Dest: document\r\n"
                "Sec-Fetch-Mode: navigate\r\nSec-Fetch-Site: none\r\nSec-Fetch-User: ?1\r\n"
                "Priority: u=0, i\r\n\r\n");
    // run1-overwritten/capture-17: a link clicked.
    expect_text("wire: Firefox, a link clicked",
                wire_request(firefox, &clicked, "GET", "https://localhost:8765/clicked", NULL, ref),
                "GET /clicked HTTP/1.1\r\nHost: localhost:8765\r\n" FIREFOX_UA FIREFOX_NAV_ACCEPT FIREFOX_LANG_ENC
                "Connection: keep-alive\r\nReferer: http://localhost:8765/\r\nUpgrade-Insecure-Requests: 1\r\n"
                "Sec-Fetch-Dest: document\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-Site: same-origin\r\n"
                "Sec-Fetch-User: ?1\r\nPriority: u=0, i\r\n\r\n");
    // run1-overwritten/capture-10, -11, -12: the sheet, the script, the
    // picture (the clean run; -02 to -04 are the same with the cache's
    // Pragma and Cache-Control, which yonder does not send).
    expect_text("wire: Firefox, a sheet",
                wire_request(firefox, &style, "GET", "https://localhost:8765/style.css", NULL, ref),
                "GET /style.css HTTP/1.1\r\nHost: localhost:8765\r\n" FIREFOX_UA "Accept: text/css,*/*;q=0.1\r\n"
                FIREFOX_LANG_ENC "Connection: keep-alive\r\nReferer: http://localhost:8765/\r\n"
                "Sec-Fetch-Dest: style\r\nSec-Fetch-Mode: no-cors\r\nSec-Fetch-Site: same-origin\r\n"
                "Priority: u=2\r\n\r\n");
    expect_text("wire: Firefox, a script",
                wire_request(firefox, &script, "GET", "https://localhost:8765/script.js", NULL, ref),
                "GET /script.js HTTP/1.1\r\nHost: localhost:8765\r\n" FIREFOX_UA "Accept: */*\r\n"
                FIREFOX_LANG_ENC "Connection: keep-alive\r\nReferer: http://localhost:8765/\r\n"
                "Sec-Fetch-Dest: script\r\nSec-Fetch-Mode: no-cors\r\nSec-Fetch-Site: same-origin\r\n"
                "Priority: u=2\r\n\r\n");
    expect_text("wire: Firefox, a picture",
                wire_request(firefox, &image, "GET", "https://localhost:8765/image.png", NULL, ref),
                "GET /image.png HTTP/1.1\r\nHost: localhost:8765\r\n" FIREFOX_UA
                "Accept: image/png,image/*;q=0.8,*/*;q=0.5\r\n"
                FIREFOX_LANG_ENC "Connection: keep-alive\r\nReferer: http://localhost:8765/\r\n"
                "Sec-Fetch-Dest: image\r\nSec-Fetch-Mode: no-cors\r\nSec-Fetch-Site: same-origin\r\n"
                "Priority: u=5, i\r\n\r\n");
    // -13 and -14: the fetches, whose Referer comes before Connection.
    expect_text("wire: Firefox, a same-origin fetch",
                wire_request(firefox, &same, "GET", "https://localhost:8765/fetch-same", NULL, ref),
                "GET /fetch-same HTTP/1.1\r\nHost: localhost:8765\r\n" FIREFOX_UA "Accept: */*\r\n"
                FIREFOX_LANG_ENC "Referer: http://localhost:8765/\r\nConnection: keep-alive\r\n"
                "Sec-Fetch-Dest: empty\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Site: same-origin\r\n"
                "Priority: u=4\r\n\r\n");
    snprintf(extra, sizeof(extra), "%s%s", ref, origin);
    expect_text("wire: Firefox, a cross-site fetch",
                wire_request(firefox, &cross, "GET", "https://127.0.0.1:8765/fetch-cross", NULL, extra),
                "GET /fetch-cross HTTP/1.1\r\nHost: 127.0.0.1:8765\r\n" FIREFOX_UA "Accept: */*\r\n"
                FIREFOX_LANG_ENC "Referer: http://localhost:8765/\r\nOrigin: http://localhost:8765\r\n"
                "Connection: keep-alive\r\nSec-Fetch-Dest: empty\r\nSec-Fetch-Mode: cors\r\n"
                "Sec-Fetch-Site: cross-site\r\nPriority: u=4\r\n\r\n");
    // -15: the preflight.
    snprintf(extra, sizeof(extra), "Access-Control-Request-Method: PUT\r\nAccess-Control-Request-Headers: x-w1\r\n%s%s",
             origin, ref);
    expect_text("wire: Firefox, a preflight",
                wire_request(firefox, &preflight, "OPTIONS", "https://127.0.0.1:8765/fetch-cross-preflight", "*/*",
                             extra),
                "OPTIONS /fetch-cross-preflight HTTP/1.1\r\nHost: 127.0.0.1:8765\r\n" FIREFOX_UA "Accept: */*\r\n"
                FIREFOX_LANG_ENC "Access-Control-Request-Method: PUT\r\nAccess-Control-Request-Headers: x-w1\r\n"
                "Referer: http://localhost:8765/\r\nOrigin: http://localhost:8765\r\nConnection: keep-alive\r\n"
                "Sec-Fetch-Dest: empty\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Site: cross-site\r\n"
                "Priority: u=4\r\n\r\n");
    // -16: the PUT, matched without the page's X-W1, with its length last.
    snprintf(extra, sizeof(extra), "%s%s", ref, origin);
    expect_text("wire: Firefox, the PUT the preflight allowed",
                wire_request(firefox, &put, "PUT", "https://127.0.0.1:8765/fetch-cross-preflight", NULL, extra),
                "PUT /fetch-cross-preflight HTTP/1.1\r\nHost: 127.0.0.1:8765\r\n" FIREFOX_UA "Accept: */*\r\n"
                FIREFOX_LANG_ENC "Referer: http://localhost:8765/\r\nOrigin: http://localhost:8765\r\n"
                "Connection: keep-alive\r\nSec-Fetch-Dest: empty\r\nSec-Fetch-Mode: cors\r\n"
                "Sec-Fetch-Site: cross-site\r\nPriority: u=4\r\nContent-Length: 0\r\n\r\n");
#undef CHROME_HINTS_NAV
#undef CHROME_UA
#undef CHROME_NAV_ACCEPT
#undef CHROME_SUB_HEAD
#undef CHROME_TAIL
#undef FIREFOX_UA
#undef FIREFOX_LANG_ENC
#undef FIREFOX_NAV_ACCEPT
}

// THE TABLE ITSELF: each browser's Accept per destination is the one it
// sent less what yonder cannot decode; the browsers and yonder's own say
// keep-alive, and no costume does; the orders are the captures'.
static void wire_table_cases(void)
{
    const yonder_agent_preset_t *chrome = preset_named("Chrome on Windows");
    const yonder_agent_preset_t *firefox = preset_named("Firefox on Windows");
    const yonder_agent_preset_t *safari = preset_named("Safari on iPhone");
    const yonder_agent_preset_t *own = yonder_agent_preset(0);
    if (chrome == NULL || firefox == NULL || safari == NULL)
        return;
    // What no Accept of a preset names: the types yonder does not decode.
    static const char *const kNever[] = {"image/avif", "image/jxl", "image/webp", "image/apng",
                                         "image/svg+xml", "application/signed-exchange", "video/"};
    bool honest = true;
    for (size_t i = 0; i < yonder_agent_npresets(); i++)
        for (int d = 0; d < WAY_DESTS; d++)
            for (size_t k = 0; k < sizeof(kNever) / sizeof(kNever[0]); k++) {
                const char *a = yonder_agent_preset(i)->identity.accept[d];
                honest &= a == NULL || strstr(a, kNever[k]) == NULL;
            }
    expect("wire: no preset's Accept names a type yonder cannot decode", honest, NULL);
    expect("wire: the browsers and yonder's own say keep-alive; the costumes say nothing",
           chrome->identity.keep_alive && firefox->identity.keep_alive && safari->identity.keep_alive &&
               own->identity.keep_alive && !preset_named("Netscape 4 on Windows 98")->identity.keep_alive &&
               !preset_named("Lynx")->identity.keep_alive, NULL);
    expect_text("wire: yonder's own asks for a page, and anything else less",
                own->identity.accept[WAY_DEST_DOCUMENT], "text/html,application/xhtml+xml,text/*;q=0.9,*/*;q=0.8");
    expect_text("wire: Safari's navigation, cited", safari->identity.accept[WAY_DEST_DOCUMENT],
                "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    expect("wire: no font Accept was captured, so none is claimed",
           chrome->identity.accept[WAY_DEST_FONT] == NULL && firefox->identity.accept[WAY_DEST_FONT] == NULL, NULL);
    // The kind a request is: way_order_of picks the order.
    way_origin_t from;
    way_origin_of("https://a.test/", 0, &from);
    way_metadata_t m;
    way_metadata_navigation(&m, WAY_BY_BROWSER, NULL);
    const char *nav = way_identity_order(&chrome->identity, &m);
    way_metadata_request(&m, &from, WAY_DEST_IMAGE, WAY_MODE_NO_CORS);
    const char *nocors = way_identity_order(&firefox->identity, &m);
    way_metadata_request(&m, &from, WAY_DEST_EMPTY, WAY_MODE_SAME_ORIGIN);
    const char *cors = way_identity_order(&firefox->identity, &m);
    way_metadata_t pre = way_metadata_preflight(&m);
    const char *preflight = way_identity_order(&firefox->identity, &pre);
    expect("wire: each kind of request takes its own order",
           nav == chrome->identity.order[WAY_ORDER_NAVIGATE] && nocors == firefox->identity.order[WAY_ORDER_NO_CORS] &&
               cors == firefox->identity.order[WAY_ORDER_CORS] &&
               preflight == firefox->identity.order[WAY_ORDER_PREFLIGHT] && nav && nocors && cors && preflight &&
               nocors != cors && cors != preflight, NULL);
    expect("wire: yonder's own, Safari and the costumes keep libfetch's order",
           way_identity_order(&own->identity, &m) == NULL && way_identity_order(&safari->identity, &m) == NULL &&
               way_identity_order(NULL, &m) == NULL, NULL);
}

static void agent_cases(void)
{
    bool presets_valid = yonder_agent_npresets() > 1 &&
                         strcmp(yonder_agent_preset(0)->agent, YONDER_AGENT) == 0;
    for (size_t i = 0; i < yonder_agent_npresets(); i++)
        presets_valid &= yonder_agent_valid(yonder_agent_preset(i)->agent) &&
                         strcmp(yonder_agent_name(yonder_agent_preset(i)->agent),
                                yonder_agent_preset(i)->name) == 0;
    expect("agent: yonder's own is first, and every preset can be sent and saved as it is",
           presets_valid, NULL);
    expect("agent: one typed is valid and has no preset's name",
           yonder_agent_valid("Mozilla/3.0 (compatible)") &&
               yonder_agent_name("Mozilla/3.0 (compatible)") == NULL, NULL);
    // YONDER_AGENT_MAX counts the NUL: one that long is a byte too long.
    char longest[YONDER_AGENT_MAX + 1];
    memset(longest, 'a', YONDER_AGENT_MAX);
    longest[YONDER_AGENT_MAX] = '\0';
    bool refused = !yonder_agent_valid(longest);
    longest[YONDER_AGENT_MAX - 1] = '\0';
    bool fits = yonder_agent_valid(longest);
    expect("agent: one that would not fit a fetch is refused, one a byte shorter is not",
           refused && fits, NULL);
    expect("agent: what a header or the config file would not keep is refused",
           !yonder_agent_valid("") && !yonder_agent_valid(NULL) &&
               !yonder_agent_valid(" leading") && !yonder_agent_valid("trailing ") &&
               !yonder_agent_valid("a # comment") && !yonder_agent_valid("two\nlines") &&
               !yonder_agent_valid("tab\there") && !yonder_agent_valid("caf\xc3\xa9"), NULL);
    agent_identity_cases();
    wire_request_cases();
    wire_table_cases();
}

// What a cache did, painting `html` through `view` with it.
static yonder_paint_cache_counts_t paint_through(yonder_paint_cache_t *c, const char *html,
                                                 os64_gui_rect_t view)
{
    s_through = c;
    free(paint_of(html, strlen(html), 400, view, NULL, true));
    s_through = NULL;
    return yonder_paint_cache_counts(c);
}

// A box with a gradient and a blurred shadow, 200 x 50.
#define CACHE_BOX "div { width: 200px; height: 50px; margin: 20px; " \
                  "background: linear-gradient(90deg, #ff0000, #0000ff); box-shadow: 4px 4px 10px #000000 }"

// Boxes alike but for one value each that makes their pixels — a stop's
// colour, place or hint (a hint's colour is 0, so .g16's black stop where
// .g4's hint is), the line's angle, a repeat, the ray's shape and centre,
// currentColor's colour, the tile's size — a ray sized in px is the same
// ray in any tile — and a shadow's blur, offset, spread, inset and corners
// — and a small tile placed in from the box's corner, so the box's first
// columns and rows are the tile's last, wrapped back — painted light, then
// dark, through the
// suite's cache in one paint: a key that left one out would hand the
// second box the first's pixels, and the oracle (paint_oracle) sees it.
static const char *kCacheKeys =
    "<!doctype html><style>body { margin: 0 } div { display: inline-block; width: 60px; height: 30px;"
    " margin: 6px; color: #ff0000 }"
    " .g0 { background: linear-gradient(90deg, #ff0000, #0000ff) }"
    " .g1 { background: linear-gradient(45deg, #ff0000, #0000ff) }"
    " .g2 { background: linear-gradient(90deg, #ff0000, #00ff00) }"
    " .g3 { background: linear-gradient(90deg, #ff0000 20%, #0000ff) }"
    " .g4 { background: linear-gradient(90deg, #ff0000, 30%, #0000ff) }"
    " .g5 { background: linear-gradient(90deg, #ff0000, #0000ff 20px) }"
    " .g6 { background: repeating-linear-gradient(90deg, #ff0000, #0000ff 20px) }"
    " .g7 { background: radial-gradient(#ff0000, #0000ff) }"
    " .g8 { background: radial-gradient(circle, #ff0000, #0000ff) }"
    " .g9 { background: radial-gradient(closest-side, #ff0000, #0000ff) }"
    " .g10 { background: radial-gradient(at 20% 30%, #ff0000, #0000ff) }"
    " .g11 { background: linear-gradient(90deg, currentColor, #0000ff) }"
    " .g12 { background: linear-gradient(90deg, currentColor, #0000ff); color: #00ff00 }"
    " .g13 { background: linear-gradient(90deg, #ff0000, #0000ff); width: 61px }"
    " .g14 { background: linear-gradient(90deg, #ff0000, #0000ff); height: 31px }"
    " .g15 { background: linear-gradient(90deg, rgba(255,0,0,0.5), #0000ff) }"
    " .g16 { background: linear-gradient(90deg, #ff0000, #000000 30%, #0000ff) }"
    " .g17 { background: radial-gradient(10px 10px at 5px 5px, #ff0000, #0000ff) }"
    " .g18 { background: radial-gradient(10px 10px at 5px 5px, #ff0000, #0000ff); width: 70px }"
    " .g19 { background: radial-gradient(10px 10px at 5px 5px, #ff0000, #0000ff); height: 31px }"
    " .g20 { background: radial-gradient(#ff0000, #0000ff) 7px 5px / 20px 20px }"
    " .s0 { box-shadow: 4px 4px 6px #000000 }"
    " .s1 { box-shadow: 4px 4px 10px #000000 }"
    " .s2 { box-shadow: 6px 4px 6px #000000 }"
    " .s3 { box-shadow: 4px 4px 6px 3px #000000 }"
    " .s4 { box-shadow: inset 4px 4px 6px #000000 }"
    " .s5 { box-shadow: 4px 4px 6px #000000; border-radius: 8px }"
    " .s6 { box-shadow: 4px 4px 6px #000000; border: 3px solid #808080 }"
    " .s7 { box-shadow: inset 4px 4px 6px #000000; border: 3px solid #808080 }"
    " .s8 { box-shadow: inset 6px 4px 6px #000000 }"
    " .s9 { box-shadow: inset 4px 4px 6px 2px #000000 }"
    "</style>"
    "<div class=g0></div><div class=g1></div><div class=g2></div><div class=g3></div>"
    "<div class=g4></div><div class=g5></div><div class=g6></div><div class=g7></div>"
    "<div class=g8></div><div class=g9></div><div class=g10></div><div class=g11></div>"
    "<div class=g12></div><div class=g13></div><div class=g14></div><div class=g15></div>"
    "<div class=g16></div><div class=g17></div><div class=g18></div><div class=g19></div>"
    "<div class=g20></div>"
    "<div class=s0></div><div class=s1></div><div class=s2></div><div class=s3></div>"
    "<div class=s4></div><div class=s5></div><div class=s6></div><div class=s7></div>"
    "<div class=s8></div><div class=s9></div>";

static void paint_cache_cases(void)
{
    const os64_gui_rect_t view = {0, 0, 400, 400};
    free(paint_of(kCacheKeys, strlen(kCacheKeys), 400, view, NULL, true));
    s_dark_paint = &kDarkPaint;
    free(paint_of(kCacheKeys, strlen(kCacheKeys), 400, view, NULL, true));
    s_dark_paint = NULL;

    // The budget is the screen's: eight screens of four-byte pixels, never
    // under the floor, never over the ceiling, and 32-bit sides that would
    // overflow multiplied out still answer the ceiling — on a machine with
    // memory to spare (the P5's 24 GiB).
    const uint64_t roomy = (uint64_t)24 << 30;
    expect("cache: the budget is eight screens, between the floor and the ceiling",
           yonder_paint_cache_budget(0, 0, roomy) == YONDER_PAINT_CACHE_FLOOR &&
               yonder_paint_cache_budget(1024, 768, roomy) == YONDER_PAINT_CACHE_FLOOR &&
               yonder_paint_cache_budget(2560, 1440, roomy) == (size_t)2560 * 1440 * 4 * 8 &&
               yonder_paint_cache_budget(1024, 1024, roomy) == (size_t)32 << 20 &&
               yonder_paint_cache_budget(1024, 1025, roomy) == (size_t)1024 * 1025 * 32 &&
               yonder_paint_cache_budget(5120, 2880, roomy) == (size_t)5120 * 2880 * 32 &&
               yonder_paint_cache_budget(8192, 4096, roomy) == YONDER_PAINT_CACHE_CEILING &&
               yonder_paint_cache_budget(8192, 4097, roomy) == YONDER_PAINT_CACHE_CEILING &&
               yonder_paint_cache_budget(UINT32_MAX, UINT32_MAX, roomy) == YONDER_PAINT_CACHE_CEILING,
           NULL);
    // And never more than a sixteenth of the memory installed (a 4K panel
    // on 2 GiB gets 128 MiB, not 253), never under the floor even so, and
    // memory not known is the floor.
    expect("cache: the budget is never more than a share of the machine's memory",
           yonder_paint_cache_budget(3840, 2160, (uint64_t)2 << 30) == (size_t)128 << 20 &&
               yonder_paint_cache_budget(3840, 2160, (uint64_t)256 << 20) == YONDER_PAINT_CACHE_FLOOR &&
               yonder_paint_cache_budget(3840, 2160, 0) == YONDER_PAINT_CACHE_FLOOR &&
               yonder_paint_cache_budget(2560, 1440, (uint64_t)4 << 30) == (size_t)2560 * 1440 * 32 &&
               yonder_paint_cache_budget(UINT32_MAX, UINT32_MAX, UINT64_MAX) == YONDER_PAINT_CACHE_CEILING,
           NULL);

    // Memory refused below the budget: an entry no recent paint used is let
    // go and the make asked again; one this paint uses never is.
    {
        yonder_paint_cache_t *m = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
        const uint8_t ka[] = "a", kb[] = "b", kc[] = "c";
        paint_cache_begin(m);
        bool made_a = paint_cache_make(m, ka, 1, 1, 1000) != NULL;
        paint_cache_begin(m);
        paint_cache_begin(m);
        s_alloc_refuse_once = true;
        bool made_b = paint_cache_make(m, kb, 1, 2, 1000) != NULL;
        bool a_gone = paint_cache_find(m, ka, 1, 1) == NULL;
        s_alloc_refuse_once = true;
        bool c_refused = paint_cache_make(m, kc, 1, 3, 1000) == NULL;
        bool b_kept = paint_cache_find(m, kb, 1, 2) != NULL;
        s_alloc_refuse_once = false;
        expect("cache: memory refused lets go of what no recent paint used, never this paint's",
               made_a && made_b && a_gone && c_refused && b_kept, NULL);
        yonder_paint_cache_free(m);
    }

    // A 200 x 50 tile is two blocks, 128 and 72 wide; its 230 x 80 mask
    // (the blur's reach of 15 each side) two more.
    const char *one = "<!doctype html><style>" CACHE_BOX "</style><div></div>";
    yonder_paint_cache_t *c = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    yonder_paint_cache_counts_t n = paint_through(c, one, view);
    expect("cache: a first paint makes the tile's blocks and the mask's",
           n.made == 4 && n.hits == 0 && n.entries == 4 && n.bytes > 200 * 50 * 4, NULL);
    n = paint_through(c, one, view);
    expect("cache: a second paint finds them, and makes nothing", n.made == 4 && n.hits == 4, NULL);
    yonder_paint_cache_empty(c);
    n = yonder_paint_cache_counts(c);
    expect("cache: emptied, it holds nothing and keeps its counts",
           n.entries == 0 && n.bytes == 0 && n.made == 4, NULL);
    yonder_paint_cache_counts_restart(c);

    // Four of one box, one hanging off the page's left edge: a mask moved
    // by whole pixels is the same mask.
    const char *four = "<!doctype html><style>" CACHE_BOX " .off { margin-left: -30px }</style>"
                       "<div></div><div></div><div class=off></div><div></div>";
    n = paint_through(c, four, view);
    expect("cache: boxes of one size and one gradient and shadow share their blocks",
           n.made == 4 && n.hits == 12, NULL);
    yonder_paint_cache_free(c);

    // Room for one tile: a paint's own blocks — found or made — are never
    // let go for another picture it wants, so .b is worked out; and every
    // paint after keeps .a and works .b out, rather than trading them.
    const char *a = "<!doctype html><style>div { width: 200px; height: 50px;"
                    " background: linear-gradient(#ff0000, #00ff00) }</style><div></div>";
    const char *two = "<!doctype html><style>div { width: 200px; height: 50px }"
                      " .a { background: linear-gradient(#ff0000, #00ff00) }"
                      " .b { background: linear-gradient(#0000ff, #00ff00) }</style>"
                      "<div class=a></div><div class=b></div>";
    const char *other = "<!doctype html><style>div { width: 200px; height: 50px;"
                        " background: linear-gradient(#ff00ff, #00ff00) }</style><div></div>";
    const size_t one_tile = 200 * 50 * 4 + 4096;
    c = yonder_paint_cache_new(one_tile);
    n = paint_through(c, two, view);
    expect("cache: a paint's own tile is never let go for another of its own",
           n.made == 2 && n.no_room == 1 && n.evicted == 0, NULL);
    n = paint_through(c, two, view);
    expect("cache: a paint past the budget keeps the same tile and works the same one out",
           n.made == 2 && n.hits == 2 && n.no_room == 2 && n.evicted == 0, NULL);
    // .a was used by the paint before: not let go for the next.
    n = paint_through(c, other, view);
    expect("cache: what the last paint used is not let go", n.made == 2 && n.no_room == 3, NULL);
    n = paint_through(c, other, view);
    expect("cache: what no paint of the last two used is let go for room",
           n.evicted == 2 && n.made == 4, NULL);
    yonder_paint_cache_free(c);

    // A picture whose blocks cannot all fit takes none of the budget: the
    // small tile after it is kept.
    const char *big_first = "<!doctype html><style>div { width: 200px; height: 50px }"
                            " .big { width: 380px; height: 200px; background: linear-gradient(#ffff00, #00ff00) }"
                            " .a { background: linear-gradient(#ff0000, #00ff00) }</style>"
                            "<div class=big></div><div class=a></div>";
    c = yonder_paint_cache_new(one_tile);
    n = paint_through(c, big_first, view);
    expect("cache: a picture that cannot be kept whole keeps none of itself",
           n.no_room == 1 && n.made == 2 && n.entries == 2, NULL);
    yonder_paint_cache_free(c);

    // A picture refused for room releases the blocks it found: the paint
    // after may let them go at once. A 300 px tile is three blocks across;
    // the budget holds one row of them, so a view moved onto a second row
    // finds the first and cannot have the second.
    const char *tall_tile = "<!doctype html><style>body { margin: 0 } div { width: 300px; height: 20000px;"
                            " background: linear-gradient(#ff0000, #0000ff) }</style><div></div>";
    c = yonder_paint_cache_new(160000);
    n = paint_through(c, tall_tile, (os64_gui_rect_t){0, 10000, 400, 100});
    expect("cache: one row of a tall tile's blocks", n.made == 3, NULL);
    n = paint_through(c, tall_tile, (os64_gui_rect_t){0, 10020, 400, 100});
    expect("cache: two rows do not fit", n.no_room == 1 && n.hits == 3 && n.made == 3, NULL);
    n = paint_through(c, a, view);
    expect("cache: what a refused picture found is let go first", n.made == 5 && n.evicted >= 2, NULL);
    yonder_paint_cache_free(c);

    // No memory part of the way through: on a page of one gradient, two
    // blocks, the run whose memory gives out after the first block is made
    // counts that picture refused for memory; and the next paint, given
    // memory, finds the released block and makes the other.
    bool partial = false, counted = false, kept = false;
    for (long left = 0; left < 24 && !partial; left++) {
        c = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
        s_paint_alloc_left = left;
        n = paint_through(c, a, view);
        s_paint_alloc_left = -1;
        if (n.made == 1) {
            partial = true;
            counted = n.no_memory == 1;
            n = paint_through(c, a, view);
            kept = n.no_memory == 1 && n.no_room == 0 && n.hits == 1 && n.made == 2;
        }
        yonder_paint_cache_free(c);
    }
    expect("cache: memory refused after a block is made is counted, and the next paint keeps the picture",
           partial && counted && kept, NULL);

    // A shadow's blocks are published only once nothing can fail before
    // they are filled: whatever allocation gives out in a paint of one, the
    // paint after, with memory, draws exactly what the painter works out
    // (an entry left unfilled would draw ASan's fill pattern).
    const char *shade = "<!doctype html><style>div { width: 200px; height: 50px; margin: 20px;"
                        " box-shadow: 4px 4px 10px #000000 }</style><div></div>";
    // And a paint that kept none of the shadow counted why.
    bool every_refusal_counted = true;
    for (long left = 0; left < 16; left++) {
        c = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
        s_paint_alloc_left = left;
        n = paint_through(c, shade, view);
        s_paint_alloc_left = -1;
        every_refusal_counted &= n.made > 0 || n.no_memory >= 1;
        s_oracle_extra = c;
        free(paint_of(shade, strlen(shade), 400, view, NULL, true));
        s_oracle_extra = NULL;
        yonder_paint_cache_free(c);
    }
    expect("cache: a shadow kept in no block, for memory, is counted", every_refusal_counted, NULL);

    // The blocks a paint is missing are blurred together, as the uncached
    // path blurs the view's part: a 200 px blur's reach is 300, and a block
    // blurring its own would ask for about ten times what the whole does.
    const char *wide_blur = "<!doctype html><style>body { margin: 0 } div { width: 300px; height: 300px;"
                            " box-shadow: 0 0 200px #000000 }</style><div></div>";
    free(paint_of(wide_blur, strlen(wide_blur), 400, view, NULL, true));
    size_t plain = s_paint_bytes;
    c = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    n = paint_through(c, wide_blur, view);
    char said[96];
    snprintf(said, sizeof(said), "%zu bytes against %zu, %lu blocks made", s_paint_bytes, plain,
             (unsigned long)n.made);
    expect("cache: a first paint blurs its missing blocks once, as the uncached path does",
           n.made == 16 && s_paint_bytes < 2 * plain, said);
    yonder_paint_cache_free(c);

    // Room for two tiles: the one found again is the newest, so a third
    // lets the other go — once no paint of the last two has used it.
    const char *x = a, *y = other;
    const char *z = "<!doctype html><style>div { width: 200px; height: 50px;"
                    " background: linear-gradient(#00ffff, #00ff00) }</style><div></div>";
    c = yonder_paint_cache_new(2 * 200 * 50 * 4 + 4096);
    (void)paint_through(c, x, view);
    (void)paint_through(c, y, view);
    (void)paint_through(c, x, view);
    (void)paint_through(c, z, view);
    n = paint_through(c, x, view);
    expect("cache: the least recently used is let go first", n.made == 6 && n.evicted == 2 && n.hits == 4, NULL);
    yonder_paint_cache_free(c);

    // More tiles than the table's first buckets: a table grown finds what
    // it held.
    char many[300 * 96 + 64];
    size_t at = (size_t)snprintf(many, sizeof(many), "<!doctype html><style>div { width: 10px; height: 10px;"
                                 " display: inline-block }</style>");
    for (int i = 0; i < 300; i++)
        at += (size_t)snprintf(many + at, sizeof(many) - at,
                               "<div style=\"background: linear-gradient(%ddeg, #ff0000, #0000ff)\"></div>", i);
    c = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    (void)paint_through(c, many, (os64_gui_rect_t){0, 0, 400, 2000});
    n = paint_through(c, many, (os64_gui_rect_t){0, 0, 400, 2000});
    expect("cache: a table grown past its first buckets finds every tile it holds",
           n.made == 300 && n.hits == 300, NULL);
    yonder_paint_cache_free(c);

    // One colour is a fill: a repeat shorter than a pixel is its average.
    const char *solid = "<!doctype html><style>div { width: 200px; height: 50px; background:"
                        " repeating-linear-gradient(90deg, #ff0000 0px, #0000ff 0.5px) }</style><div></div>";
    c = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    n = paint_through(c, solid, view);
    expect("cache: a gradient of one colour is not kept", n.made == 0 && n.no_room == 0, NULL);
    yonder_paint_cache_free(c);

    // Far wider than the view: the tile's blocks and the mask's (its reach
    // of 9 each side) that the view's 400 columns read, four of each.
    const char *wide = "<!doctype html><style>div { width: 70000px; height: 20px;"
                       " background: linear-gradient(90deg, #ff0000, #0000ff); box-shadow: 0 0 6px #000000 }"
                       "</style><div></div>";
    c = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    n = paint_through(c, wide, view);
    expect("cache: a picture far wider than the view is kept by the blocks the view reads",
           n.made == 8 && n.bytes < 8 * (128 * 128 * 4 + 4096), NULL);
    yonder_paint_cache_free(c);

    // A box 20000 rows tall, seen 100 rows of: three blocks across its 300
    // px tile and three across its 318 px mask, from block row 78 (rows
    // 9984 to 10111); the mask begins 9 rows above the box. A view moved
    // down 20 rows reads row 79 of both as well.
    const char *tall = "<!doctype html><style>body { margin: 0 } div { width: 300px; height: 20000px;"
                       " background: linear-gradient(#ff0000, #0000ff); box-shadow: 0 0 6px #000000 }"
                       "</style><div></div>";
    c = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    n = paint_through(c, tall, (os64_gui_rect_t){0, 10000, 400, 100});
    expect("cache: a tall tile and mask are kept by the blocks the view reads", n.made == 6, NULL);
    n = paint_through(c, tall, (os64_gui_rect_t){0, 10020, 400, 100});
    expect("cache: a view moved finds the blocks it shares and makes the rest", n.made == 12 && n.hits == 6, NULL);
    yonder_paint_cache_free(c);
}

// The scratch's rules (YONDER_PERF.md § perf-5), a row of its table at a
// time. The shim's own counts are the oracle: a buffer lent again is no
// allocation, and one freed is no longer live.
static void scratch_cases(void)
{
    const size_t big = 100 * 1024;
    yonder_scratch_t *s = yonder_scratch_new();
    expect("scratch: made", s != NULL, NULL);
    if (s == NULL)
        return;
    size_t live0 = live;

    // Below the floor, the heap's: taken and freed, never kept.
    uint64_t m0 = s_mallocs;
    void *small = yonder_scratch_take(s, YONDER_SCRATCH_FLOOR - 1);
    yonder_scratch_give(s, small);
    yonder_scratch_counts_t c = yonder_scratch_counts(s);
    expect("scratch: a small request is the heap's", small != NULL && s_mallocs == m0 + 1 &&
           c.unpooled == 1 && c.made == 0 && c.idle == 0 && live == live0, NULL);

    // Empty slot, then lent, then idle; the same size lent again allocates
    // nothing and is the same buffer.
    m0 = s_mallocs;
    char *a = yonder_scratch_take(s, big);
    c = yonder_scratch_counts(s);
    expect("scratch: a big request makes a buffer, lent", a != NULL && s_mallocs == m0 + 1 &&
           c.made == 1 && c.lent == 1 && c.idle == 0, NULL);
    memset(a, 0x5a, big);
    yonder_scratch_give(s, a);
    c = yonder_scratch_counts(s);
    expect("scratch: given back, it is idle", c.lent == 0 && c.idle == 1 && c.idle_bytes >= big, NULL);
    m0 = s_mallocs;
    char *again = yonder_scratch_take(s, big);
    c = yonder_scratch_counts(s);
    expect("scratch: the same size is lent again, dirty, with no allocation",
           again == a && s_mallocs == m0 && c.reused == 1 && c.lent == 1 && c.idle == 0 &&
           c.idle_bytes == 0 && (unsigned char)again[big - 1] == 0x5a, NULL);
    yonder_scratch_give(s, again);

    // A little bigger fits the eighth to spare.
    m0 = s_mallocs;
    void *grown = yonder_scratch_take(s, big + big / 10);
    expect("scratch: a box a little bigger keeps its buffer", grown == a && s_mallocs == m0, NULL);
    yonder_scratch_give(s, grown);

    // The smallest idle buffer that holds the request is the one lent.
    void *two = yonder_scratch_take(s, 4 * big);
    yonder_scratch_give(s, two);
    void *fits = yonder_scratch_take(s, big / 2 + YONDER_SCRATCH_FLOOR);
    expect("scratch: the smallest idle buffer that holds it is lent", fits == a, NULL);
    void *next = yonder_scratch_take(s, big);
    expect("scratch: and the next is the bigger one", next == two, NULL);
    yonder_scratch_give(s, fits);
    yonder_scratch_give(s, next);

    // Idle buffers too small, and no empty slot: the smallest is freed and
    // remade. Every slot lent: the heap's, freed on its way back.
    yonder_scratch_drop(s);
    expect("scratch: dropped, nothing is held", live == live0 && yonder_scratch_counts(s).idle == 0, NULL);
    void *held[YONDER_SCRATCH_SLOTS];
    for (int i = 0; i < YONDER_SCRATCH_SLOTS; i++)
        held[i] = yonder_scratch_take(s, big + (size_t)i * 8192);
    m0 = s_mallocs;
    void *over = yonder_scratch_take(s, big);
    c = yonder_scratch_counts(s);
    expect("scratch: every slot lent, the heap's", over != NULL && s_mallocs == m0 + 1 &&
           c.lent == YONDER_SCRATCH_SLOTS, NULL);
    size_t live_lent = live;
    yonder_scratch_give(s, over);
    expect("scratch: a buffer no slot holds is freed", live == live_lent - 1, NULL);
    for (int i = 0; i < YONDER_SCRATCH_SLOTS; i++)
        yonder_scratch_give(s, held[i]);
    m0 = s_mallocs;
    size_t live_idle = live;
    void *huge = yonder_scratch_take(s, 8 * big);
    c = yonder_scratch_counts(s);
    expect("scratch: none fits and none is empty: the smallest idle one is remade",
           huge != NULL && huge != held[0] && s_mallocs == m0 + 1 && live == live_idle &&
           c.idle == YONDER_SCRATCH_SLOTS - 1, NULL);
    m0 = s_mallocs;
    void *second = yonder_scratch_take(s, big + 8192);
    expect("scratch: the next smallest is still kept", second == held[1] && s_mallocs == m0, NULL);
    yonder_scratch_give(s, second);
    yonder_scratch_give(s, huge);

    // Idle long enough, freed at a paint's end; a lent one is not. Some
    // paints go by first, so the paint it was given back in is not the
    // first one.
    yonder_scratch_drop(s);
    for (int i = 0; i < 5; i++)
        yonder_scratch_paint_end(s);
    void *idle = yonder_scratch_take(s, big);
    yonder_scratch_give(s, idle);
    void *busy = yonder_scratch_take(s, 2 * big);
    for (int i = 0; i < YONDER_SCRATCH_IDLE_PAINTS; i++)
        yonder_scratch_paint_end(s);
    c = yonder_scratch_counts(s);
    expect("scratch: idle for the allowed paints, still kept", c.idle == 1 && c.lent == 1, NULL);
    yonder_scratch_paint_end(s);
    c = yonder_scratch_counts(s);
    expect("scratch: one paint more, freed; the lent one kept", c.idle == 0 && c.lent == 1 &&
           c.idle_bytes == 0, NULL);
    yonder_scratch_drop(s);
    expect("scratch: another page frees no lent buffer", yonder_scratch_counts(s).lent == 1, NULL);
    yonder_scratch_give(s, busy);

    // Idle buffers held to the keep: one given back past it is freed.
    yonder_scratch_drop(s);
    const size_t part = YONDER_SCRATCH_KEEP / 4;
    void *quarter[5];
    for (int i = 0; i < 5; i++)
        quarter[i] = yonder_scratch_take(s, part - part / 8 - 8192);
    for (int i = 0; i < 5; i++)
        yonder_scratch_give(s, quarter[i]);
    c = yonder_scratch_counts(s);
    expect("scratch: idle buffers never hold more than the keep",
           c.idle == 4 && c.idle_bytes <= YONDER_SCRATCH_KEEP && c.lent == 0, NULL);
    m0 = s_mallocs;
    c = yonder_scratch_counts(s);
    void *past = yonder_scratch_take(s, YONDER_SCRATCH_KEEP);
    yonder_scratch_counts_t d = yonder_scratch_counts(s);
    yonder_scratch_give(s, past);
    expect("scratch: a request past the keep is the heap's",
           past != NULL && s_mallocs == m0 + 1 && d.unpooled == c.unpooled + 1 && d.made == c.made &&
           d.lent == 0 && yonder_scratch_counts(s).idle == 4, NULL);

    // No scratch at all: the heap's, every time.
    m0 = s_mallocs;
    void *none = yonder_scratch_take(NULL, big);
    yonder_scratch_give(NULL, none);
    yonder_scratch_counts_t z = yonder_scratch_counts(NULL);
    expect("scratch: NULL takes from the heap and frees", none != NULL && s_mallocs == m0 + 1 &&
           z.made == 0 && z.lent == 0, NULL);

    yonder_scratch_free(s);
    expect("scratch: freed, nothing is left", live == live0 - 1, NULL);
}

// What the painter takes from its scratch: a page painted again, with the
// cache and without it, allocates no buffer of the scratch's size, so
// every site that takes one gives it back to be lent again.
static void scratch_paint_cases(void)
{
    static const char html[] =
        "<style>body{margin:0}div{width:400px;height:300px;margin:30px;"
        "background:linear-gradient(red,blue);box-shadow:0 0 20px 5px black}</style><div></div>";
    for (int cached = 0; cached < 2; cached++) {
        yonder_scratch_t *sc = yonder_scratch_new();
        s_through = cached ? yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR) : NULL;
        s_through_scratch = sc;
        os64_gui_rect_t view = {0, 0, 500, 400};
        free(paint_of(html, sizeof(html) - 1, 500, view, NULL, true));
        uint64_t first = s_paint_big;
        yonder_scratch_counts_t c = yonder_scratch_counts(sc);
        free(paint_of(html, sizeof(html) - 1, 500, view, NULL, true));
        yonder_scratch_counts_t d = yonder_scratch_counts(sc);
        char what[120];
        snprintf(what, sizeof(what), "scratch: the first paint %stakes its big buffers from the scratch",
                 cached ? "through the cache " : "");
        expect(what, first > 0 && c.made > 0 && c.lent == 0, NULL);
        snprintf(what, sizeof(what), "scratch: painted again %sit allocates none",
                 cached ? "through the cache " : "");
        expect(what, s_paint_big == 0 && d.made == c.made && d.reused > c.reused && d.lent == 0, NULL);
        s_through_scratch = NULL;
        yonder_paint_cache_free(s_through);
        s_through = NULL;
        yonder_scratch_free(sc);
    }
    // A shadow's mask is prepared when its blocks are missing, which with
    // the cache is the first paint only: the cache emptied in between, the
    // mask is made again, and from the scratch. (A shadow's blocks are
    // smaller than the scratch's floor, so the cache's own entries are not
    // counted.)
    static const char shadow[] =
        "<style>body{margin:0}div{width:400px;height:300px;margin:30px;"
        "box-shadow:0 0 20px 5px black}</style><div></div>";
    yonder_scratch_t *sc = yonder_scratch_new();
    s_through = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    s_through_scratch = sc;
    os64_gui_rect_t view = {0, 0, 500, 400};
    free(paint_of(shadow, sizeof(shadow) - 1, 500, view, NULL, true));
    uint64_t first = s_paint_big;
    yonder_paint_cache_empty(s_through);
    free(paint_of(shadow, sizeof(shadow) - 1, 500, view, NULL, true));
    expect("scratch: a shadow's mask made again is the scratch's",
           first > 0 && s_paint_big == 0 && yonder_paint_cache_counts(s_through).made > 0, NULL);
    s_through_scratch = NULL;
    yonder_paint_cache_free(s_through);
    s_through = NULL;
    yonder_scratch_free(sc);
}

int main(int argc, char **argv)
{
    os64_text_options_t o = {
        .memory = {NULL, text_alloc_cb, text_free_cb},
        .backend = flow_test_backend(),
    };
    if (os64_text_create(&o, &s_text) != OS64_FONT_OK)
        return 2;
    kEnv.text = s_text;
    size_t base_live = live;
    if (argc == 4 && strcmp(argv[1], "--paint") == 0) {
        // `--paint FILE WIDTH`: the whole page, painted, for reading.
        size_t len = 0;
        char *html = slurp(argv[2], &len);
        if (html == NULL)
            return 2;
        int32_t w = atoi(argv[3]);
        char *got = paint_of(html, len, w, (os64_gui_rect_t){0, 0, w, 1 << 20}, NULL, false);
        fputs(got, stdout);
        free(got);
        free(html);
    } else if (argc == 2 && strcmp(argv[1], "--write-corpus") == 0) {
        corpus(true);
    } else {
        s_suite_cache = yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
        s_small_cache = yonder_paint_cache_new(SMALL_CACHE_BUDGET);
        s_suite_scratch = yonder_scratch_new();
        scratch_cases();
        scratch_paint_cases();
        paint_cases();
        batch_a_paint_cases();
        shadow_blur_cases();
        huge_corner_case();
        filter_paint_cases();
        backdrop_paint_cases();
        round_picture_cases();
        shadow_split_cases();
        canvas_owner_cases();
        dark_cases();
        scale_cases();
        background_size_cases();
        bar_cases();
        download_cases();
        mail_cases();
        cost_cases();
        agent_cases();
        corpus(false);
        paint_cache_cases();
        yonder_paint_cache_free(s_suite_cache);
        yonder_paint_cache_free(s_small_cache);
        yonder_scratch_counts_t sc = yonder_scratch_counts(s_suite_scratch);
        expect("scratch: the suite's paints lent buffers again", sc.reused > 0, NULL);
        yonder_scratch_free(s_suite_scratch);
        s_suite_scratch = NULL;
    }
    for (int i = 0; i < s_nfonts; i++)
        os64_text_font_release(s_fonts[i].font);
    if (os64_text_destroy(s_text) != OS64_FONT_OK)
        expect("text context: every run released", false, NULL);
    expect("nothing leaked", live == base_live, NULL);
    printf("yonder: %d checks, %d failed\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
