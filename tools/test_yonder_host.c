// test_yonder_host.c — yonder's painter on the host: a page laid out by
// libflow at the harness's test fonts, painted through verbs that RECORD
// instead of drawing, one line per call. The expected recordings in
// tools/test_yonder_cases.inc are worked out by hand (YONDER.md § Slices);
// the corpus .paint files beside the pages are regression, not proof.

#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
#include "os64/text.h"
#include "bar.h"
#include "mail.h"
#include "paint.h"
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

static void rec_fill(void *ctx, os64_gui_rect_t r, uint32_t colour)
{
    Rec *rec = ctx;
    if (!inside(r, rec->view))
        rec->escaped = true;
    out(&rec->out, "fill %d %d %d %d #%06x\n", r.x, r.y, r.w, r.h, colour);
}

static void rec_text(void *ctx, const flow_box_t *b, os64_gui_rect_t clip, uint32_t colour)
{
    (void)clip;
    Rec *rec = ctx;
    out(&rec->out, "text \"%.*s\" %d %d #%06x\n", (int)b->length, b->text, b->rect.x, b->baseline,
        colour);
}

static void rec_image(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    (void)b;
    (void)clip;
    Rec *rec = ctx;
    out(&rec->out, "image %d %d %d %d\n", c.x, c.y, c.w, c.h);
}

static void rec_control(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    (void)clip;
    Rec *rec = ctx;
    out(&rec->out, "control %d %d %d %d %d\n", b->control, c.x, c.y, c.w, c.h);
}

// A box has a picture behind it when libpage lists one for its element;
// the recording says where it would be tiled, and from where.
static bool rec_backdrop(void *ctx, const flow_box_t *b, const os64_gui_rect_t *area, int32_t ox,
                         int32_t oy, os64_gui_rect_t clip)
{
    (void)clip;
    Rec *rec = ctx;
    if (b->node == NULL || os64_page_background_for(rec->page, b->node) < 0)
        return false;
    if (area != NULL)
        out(&rec->out, "backdrop %d %d %d %d from %d %d\n", area->x, area->y, area->w, area->h, ox,
            oy);
    return true;
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

// Lays `html` out at `width` and paints `view`; the recording, or NULL.
static char *paint_of(const char *html, size_t len, int32_t width, os64_gui_rect_t view,
                      bool *escaped)
{
    os64_html_document_t *doc = parse(html, len);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    flow_tree_t *t = flow_layout(doc, page, width, &kEnv);
    Rec rec = {{0}, view, false, page};
    out(&rec.out, "%s", "");
    yonder_verbs_t v = {&rec, rec_fill, rec_text, rec_image, rec_control, rec_backdrop};
    if (t != NULL)
        yonder_paint(t, view, kEnv.paper, &v);
    if (escaped != NULL)
        *escaped = rec.escaped;
    flow_free(t);
    os64_page_free(page);
    os64_html_document_free(doc);
    return rec.out.v;
}

static void paint_case(const char *name, const char *html, int32_t width, os64_gui_rect_t view,
                       const char *expected)
{
    bool escaped = false;
    char *got = paint_of(html, strlen(html), width, view, &escaped);
    expect(name, got != NULL && strcmp(got, expected) == 0, got);
    expect(name, !escaped, "a fill reached past the viewport");
    free(got);
}

#include "test_yonder_cases.inc"

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

static void scale_cases(void)
{
    const uint32_t A = 0xff112233u, B = 0xff445566u, C = 0xff778899u;
    uint32_t dst[16] = {0};
    const uint32_t two[4] = {A, B, C, A};
    yonder_draw_picture(dst, 4, (os64_gui_rect_t){0, 0, 4, 4}, (os64_gui_rect_t){1, 1, 2, 2}, two, 2, 2);
    const uint32_t copied[16] = {0, 0, 0, 0, 0, A, B, 0, 0, C, A, 0, 0, 0, 0, 0};
    expect("picture: a box its own size is a copy", pixels_are(dst, copied, 16), NULL);

    uint32_t row[4] = {0};
    const uint32_t ab[2] = {A, B};
    yonder_draw_picture(row, 4, (os64_gui_rect_t){0, 0, 4, 1}, (os64_gui_rect_t){0, 0, 4, 1}, ab, 2, 1);
    const uint32_t doubled[4] = {A, A, B, B};
    expect("picture: twice as wide doubles each column", pixels_are(row, doubled, 4), NULL);

    uint32_t pair[2] = {0};
    const uint32_t abc[3] = {A, B, C};
    yonder_draw_picture(pair, 2, (os64_gui_rect_t){0, 0, 2, 1}, (os64_gui_rect_t){0, 0, 2, 1}, abc, 3, 1);
    const uint32_t shrunk[2] = {A, C};
    expect("picture: three into two takes the columns under the middles",
           pixels_are(pair, shrunk, 2), NULL);

    // 0x80 red over opaque blue: red (255*128 + 127) / 255 = 128, blue
    // (255*127 + 127) / 255 = 127.
    uint32_t blend[1] = {0xff0000ffu};
    const uint32_t red_half[1] = {0x80ff0000u};
    yonder_draw_picture(blend, 1, (os64_gui_rect_t){0, 0, 1, 1}, (os64_gui_rect_t){0, 0, 1, 1}, red_half, 1, 1);
    expect("picture: half-transparent red over blue", blend[0] == 0xff80007fu, NULL);

    uint32_t clear[1] = {0xff0000ffu};
    const uint32_t none[1] = {0x00ff0000u};
    yonder_draw_picture(clear, 1, (os64_gui_rect_t){0, 0, 1, 1}, (os64_gui_rect_t){0, 0, 1, 1}, none, 1, 1);
    expect("picture: a transparent pixel leaves what is beneath", clear[0] == 0xff0000ffu, NULL);

    // A 4x4 picture in a 4x4 box, clipped to the middle 2x2: only those
    // four pixels are written, each the source's own.
    uint32_t src[16], clip[16] = {0};
    for (int i = 0; i < 16; i++)
        src[i] = 0xff000000u | (uint32_t)i;
    yonder_draw_picture(clip, 4, (os64_gui_rect_t){1, 1, 2, 2}, (os64_gui_rect_t){0, 0, 4, 4}, src, 4, 4);
    const uint32_t cut[16] = {0, 0, 0, 0, 0, src[5], src[6], 0, 0, src[9], src[10], 0, 0, 0, 0, 0};
    expect("picture: a clip cuts all four sides", pixels_are(clip, cut, 16), NULL);

    // A box partly off the surface's left and top: the clip is what keeps
    // the writes inside it.
    uint32_t edge[4] = {0};
    yonder_draw_picture(edge, 2, (os64_gui_rect_t){0, 0, 2, 2}, (os64_gui_rect_t){-2, -2, 4, 4}, src, 4, 4);
    const uint32_t shifted[4] = {src[10], src[11], src[14], src[15]};
    expect("picture: a box hanging off the top left", pixels_are(edge, shifted, 4), NULL);

    // Tiling a 2x2 picture {A,B / C,A} unscaled from the area's corner.
    uint32_t tiles[25] = {0};
    yonder_tile_picture(tiles, 5, (os64_gui_rect_t){0, 0, 5, 5}, (os64_gui_rect_t){0, 0, 5, 5}, 0, 0,
                        two, 2, 2);
    const uint32_t tiled[25] = {A, B, A, B, A, C, A, C, A, C, A, B, A, B, A,
                                C, A, C, A, C, A, B, A, B, A};
    expect("tile: whole copies, and the last cut at the area's edge", pixels_are(tiles, tiled, 25),
           NULL);

    // The same area, clipped to its middle: the tiles stay where the
    // origin put them, and nothing outside the clip is written.
    uint32_t cut_tiles[25] = {0};
    yonder_tile_picture(cut_tiles, 5, (os64_gui_rect_t){1, 1, 3, 3}, (os64_gui_rect_t){0, 0, 5, 5},
                        0, 0, two, 2, 2);
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

    // An origin above and left of the area (the canvas's, for a body the
    // view is scrolled into): the area starts one pixel into a tile.
    uint32_t shifted_tiles[4] = {0};
    yonder_tile_picture(shifted_tiles, 2, (os64_gui_rect_t){0, 0, 2, 2}, (os64_gui_rect_t){0, 0, 2, 2},
                        -1, -3, two, 2, 2);
    const uint32_t from_origin[4] = {A, C, B, A};
    expect("tile: an origin outside the area lays the tiles from there",
           pixels_are(shifted_tiles, from_origin, 4), NULL);

    uint32_t over_blue[2] = {0xff0000ffu, 0xff0000ffu};
    const uint32_t half_and_none[2] = {0x80ff0000u, 0x00ff0000u};
    yonder_tile_picture(over_blue, 2, (os64_gui_rect_t){0, 0, 2, 1}, (os64_gui_rect_t){0, 0, 2, 1}, 0,
                        0, half_and_none, 2, 1);
    expect("tile: blended by alpha over the colour beneath",
           over_blue[0] == 0xff80007fu && over_blue[1] == 0xff0000ffu, NULL);
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
        char *top = paint_of(html, len, 800, (os64_gui_rect_t){0, 0, 800, 600}, &escaped);
        char *mid = paint_of(html, len, 800, (os64_gui_rect_t){0, 3000, 800, 600}, &escaped);
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
        char *got = paint_of(html, len, w, (os64_gui_rect_t){0, 0, w, 1 << 20}, NULL);
        fputs(got, stdout);
        free(got);
        free(html);
    } else if (argc == 2 && strcmp(argv[1], "--write-corpus") == 0) {
        corpus(true);
    } else {
        paint_cases();
        scale_cases();
        bar_cases();
        mail_cases();
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
