// test_yonder_host.c — yonder's painter on the host: a page laid out by
// libflow at the harness's test fonts, painted through verbs that RECORD
// instead of drawing, one line per call. The expected recordings in
// tools/test_yonder_cases.inc are worked out by hand (YONDER.md § Slices);
// the corpus .paint files beside the pages are regression, not proof.

#define _DEFAULT_SOURCE
#include <fcntl.h>
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
#include "bar.h"
#include "mail.h"
#include "paint.h"
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
void *os64_malloc(size_t size)
{
    void *at = malloc(size != 0 ? size : 1);
    if (at != NULL)
        live++;
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
        if (n <= 16 || k == 0 || k == n - 1)
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
    flow_tree_t *t = flow_layout(doc, page, width, &env);
    Rec rec = {{0}, view, false, page};
    out(&rec.out, "%s", "");
    yonder_verbs_t v = {&rec,          rec_fill,     rec_text,       rec_image,
                        rec_control,   rec_backdrop, rec_group_open, rec_group_close,
                        rec_mask,      rec_pixels};
    if (t != NULL)
        yonder_paint(t, view, (flow_point_t){view.x, view.y}, kEnv.paper, s_dark_paint, &v);
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

// A blurred shadow cannot be worked pixel by pixel by hand, so what a blur
// must do is checked: the mask reaches three box blurs (and a pixel) past
// the shape on every side; just outside the box the shadow is about half on,
// as a Gaussian is at its step; nothing falls on the box itself, and an
// inset shadow falls only inside it.
// A rounded box as big as libflow takes, painted through a small view
// (Quinn, #203): the corner arithmetic must stay in range — UBSan is fatal
// here — and the view, deep inside the top-left curve's square, shows the
// paper where the curve leaves it and red where it is inside.
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
        {"box-shadow: 0 0 8px #000000", 27, 27, 46, 46, 100, 160, false},
        {"box-shadow: inset 0 0 8px #000000", 27, 27, 46, 46, 100, 255, true},
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
// colours are Python's colorsys run on the rule (HSL lightness: paper over
// half to 0.08 + (1 - L) / 2, ink under half to 0.92 - L / 2).
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
    expect("dark: its default ink and link go light, the link keeping its blue",
           painted_in(got, "plain", "#e3e3e3") && painted_in(got, "link", "#6262f4"), got);
    free(got);
    const char *own = "<!doctype html><style>body{margin:0;background:#ffffff} div{background:#000000}"
                      " h1{background:#e0e8ff;margin:0;font-size:16px}"
                      "</style><p>inside</p><div>night</div><h1>pale</h1><p style=\"color:#ff0000\">red</p>";
    got = paint_of(own, strlen(own), 300, view, NULL, true);
    expect("dark: a page's own white paper goes dark, and its dark ink light",
           starts(got, "fill 0 0 300 200 #141414\n") && painted_in(got, "inside", "#e3e3e3"), got);
    expect("dark: a dark box stays; a pale one darkens keeping its hue; a mid colour is the page's",
           got != NULL && strstr(got, " #000000\n") != NULL && strstr(got, " #000f38\n") != NULL &&
           painted_in(got, "night", "#e3e3e3") && painted_in(got, "red", "#ff0000"), got);
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
        paint_cases();
        shadow_blur_cases();
        huge_corner_case();
        shadow_split_cases();
        canvas_owner_cases();
        dark_cases();
        scale_cases();
        background_size_cases();
        bar_cases();
        mail_cases();
        cost_cases();
        agent_cases();
        corpus(false);
    }
    for (int i = 0; i < s_nfonts; i++)
        os64_text_font_release(s_fonts[i].font);
    if (os64_text_destroy(s_text) != OS64_FONT_OK)
        expect("text context: every run released", false, NULL);
    expect("nothing leaked", live == base_live, NULL);
    printf("yonder: %d checks, %d failed\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
