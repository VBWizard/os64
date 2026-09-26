// yonder — the graphical browser (YONDER.md). A libui window that fetches
// pages through libway on packet 07's work pool, lays them out with libflow,
// and paints them with paint.c.
//
//     yonder https://news.ycombinator.com/
//     yonder /tests/pages/hacker-news.html
//
// Everything about what a page means, where its boxes go and which roads a
// session takes belongs to libpage, libflow and libway; this file decides
// how the page looks on the glass, how a person moves around it, and how
// the window hears a fetch it never waits for.

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
#include "way/way.h"
#include "os64/os64.h"
#include "os64/draw.h"
#include "os64/fmt.h"
#include "os64/font_backend.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/slurp.h"
#include "os64/str.h"
#include "os64/text.h"
#include "os64/text_draw.h"
#include "os64/ui.h"
#include "os64/work.h"
#include "bar.h"
#include "mail.h"
#include "paint.h"
#include "picture.h"
#include "scale.h"
#include "trip.h"

// The largest file yonder reads: libhtml refuses by size beyond its own
// budget anyway, and says so; this only keeps a stray path from reading a
// disk image into memory first.
#define YONDER_FILE_MAX (32u << 20)

#define PAGE_INK   0x000000u
#define PAGE_LINK  0x0000eeu
#define PAGE_PAPER 0xffffffu

#define YONDER_AGENT  "yonder/1.0 (os64)"
#define YONDER_ACCEPT "text/html, application/xhtml+xml, text/*;q=0.8"

// The work pool. A navigation declares what one page can cost: libhtml's
// 8 MiB of input and 64 MiB of tree, and the model beside them; a picture
// declares PICTURE_RESERVE. The budget fits a page and four pictures, one
// per worker.
#define POOL_WORKERS 4
#define TRIP_RESERVE (80u << 20)
#define POOL_BUDGET  (768u << 20)

// Decoded pixels a page may keep; past it a picture draws as its frame.
#define PICTURES_KEPT_MAX ((size_t)256u << 20)

// The window's doorbell bits: the pool's completions, and a navigation's
// mailbox (progress and questions).
#define BELL_WORK (1u << 0)
#define BELL_MAIL (1u << 1)

// ── The page's fonts ────────────────────────────────────────────────────
//
// ONE text context for every page, apart from libui's chrome context
// (LAYOUT.md § Bounds), with each (generic, size) opened once on it. Until
// the web faces arrive (docs/yonder/04-web-faces.md) serif, bold and
// italic are drawn in the regular sans.

static void *text_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return os64_malloc(n);
}

static void text_free(void *ctx, void *p, size_t n)
{
    (void)ctx;
    (void)n;
    os64_free(p);
}

#define FACES_MAX 64

static struct {
    os64_text_context_t *text;
    uint8_t *sans, *mono;
    size_t sans_len, mono_len;
    struct {
        bool mono;
        uint32_t px;
        os64_text_font_t *font;
        os64_font_face_info_t info;
    } open[FACES_MAX];
    int nopen;
} s_faces;

static os64_font_status_t page_fonts(void *ctx, const flow_family_list_t *families, bool bold,
                                     bool italic, uint32_t px, os64_text_font_t *const **list,
                                     size_t *count, os64_font_face_info_t *primary)
{
    (void)ctx;
    (void)bold;
    (void)italic;
    bool mono = families->generic == FLOW_GENERIC_MONO;
    int i = 0;
    while (i < s_faces.nopen && !(s_faces.open[i].mono == mono && s_faces.open[i].px == px))
        i++;
    if (i == s_faces.nopen) {
        if (s_faces.nopen == FACES_MAX)
            return OS64_FONT_LIMIT;
        os64_font_face_options_t o = {px, OS64_FONT_HINT_NORMAL};
        os64_font_status_t st = os64_text_font_open(
            s_faces.text, mono ? s_faces.mono : s_faces.sans,
            mono ? s_faces.mono_len : s_faces.sans_len, &o, &s_faces.open[i].font);
        if (st != OS64_FONT_OK)
            return st;
        // The face's metrics as the engine reports them: an empty run
        // carries its primary face's.
        os64_text_layout_t lo = {.fonts = &s_faces.open[i].font, .font_count = 1,
                                 .tab_interval = 64 * 8};
        os64_text_run_t *run = NULL;
        st = os64_text_layout(s_faces.text, NULL, 0, &lo, &run);
        if (st != OS64_FONT_OK) {
            os64_text_font_release(s_faces.open[i].font);
            return st;
        }
        os64_text_run_view_t v;
        os64_text_run_view(run, &v);
        os64_memset(&s_faces.open[i].info, 0, sizeof(s_faces.open[i].info));
        s_faces.open[i].info.ascent = v.ascent;
        s_faces.open[i].info.descent = v.descent;
        s_faces.open[i].info.line_height = v.line_height;
        os64_text_run_release(run);
        s_faces.open[i].mono = mono;
        s_faces.open[i].px = px;
        s_faces.nopen++;
    }
    *list = &s_faces.open[i].font;
    *count = 1;
    *primary = s_faces.open[i].info;
    return OS64_FONT_OK;
}

static flow_env_t s_env = {
    .fonts = page_fonts,
    .viewport_font_px = 16,
    .default_generic = FLOW_GENERIC_SERIF,
    .ink = PAGE_INK,
    .link_ink = PAGE_LINK,
    .paper = PAGE_PAPER,
};

static const char *faces_open(void)
{
    if (os64_slurp("/etc/fonts/DejaVuSans.ttf", OS64_FONT_FILE_MAX, &s_faces.sans,
                   &s_faces.sans_len) != OS64_SLURP_OK)
        return "no /etc/fonts/DejaVuSans.ttf";
    if (os64_slurp("/etc/fonts/DejaVuSansMono.ttf", OS64_FONT_FILE_MAX, &s_faces.mono,
                   &s_faces.mono_len) != OS64_SLURP_OK)
        return "no /etc/fonts/DejaVuSansMono.ttf";
    os64_text_options_t o = {.memory = {NULL, text_alloc, text_free},
                             .backend = os64_freetype_backend_v1()};
    if (os64_text_create(&o, &s_faces.text) != OS64_FONT_OK)
        return "no text context for the page";
    s_env.text = s_faces.text;
    return NULL;
}

// A file arrives with no Content-Type to name its encoding, and a page
// that declares none in a <meta> is read as windows-1252 by the standard's
// default. Bytes that are valid UTF-8 and not plain ASCII are almost never
// meant as windows-1252, so they are read as what they are — the file
// detector's answer, not a rule the standard makes, and a <meta> can only
// lose to it when the bytes already contradict the <meta>.
static os64_html_document_t *parse_file(const uint8_t *bytes, size_t len)
{
    bool wide = false, valid = true;
    for (size_t i = 0; i < len && valid;) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode((const char *)bytes + i, len - i, &cp);
        valid = !(cp == OS64_UTF8_REPLACEMENT && took == 1);
        wide |= took > 1;
        i += took;
    }
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = wide && valid ? "utf-8" : NULL;
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (p == NULL)
        return NULL;
    os64_html_parser_feed(p, bytes, len);
    return os64_html_parser_finish(p);
}

// ── Pages ───────────────────────────────────────────────────────────────

// A page as the window holds it: libway's page (the tree, what it means,
// the reader's flips) and its layout at the width it was laid out at. A
// text/plain page has no tree of its own, so it is given one.
// A picture of the page's, one per ADDRESS: forty spacer GIFs are one
// fetch. Its address is the page model's, which lives as long as it does.
typedef enum { PIC_WAITING, PIC_SHOWN, PIC_FAILED, PIC_NOT_KEPT } PictureState;

typedef struct {
    const char *url;
    PictureState state;
    os64_image_t image;
    os64_work_id_t id;              // while WAITING
} Picture;

typedef struct {
    way_page_t way;
    os64_html_document_t *plain;
    os64_page_t *plain_model;
    flow_tree_t *tree;
    int32_t laid_width;
    // The pictures, and for each of libpage's pictures (os64_page_image)
    // which of these it is, -1 for one that will not be fetched.
    Picture *pics;
    int32_t npics, *pic_of;
    int32_t waiting, not_kept;
    size_t kept_bytes;
} Page;

static const os64_html_document_t *page_doc(const Page *p)
{
    return p->way.doc != NULL ? p->way.doc : p->plain;
}

static os64_page_t *page_model(const Page *p)
{
    return p->way.model != NULL ? p->way.model : p->plain_model;
}

static void page_clear(Page *p)
{
    for (int32_t i = 0; i < p->npics; i++)
        os64_image_free(&p->pics[i].image);
    os64_free(p->pics);
    os64_free(p->pic_of);
    flow_free(p->tree);
    os64_page_free(p->plain_model);
    os64_html_document_free(p->plain);
    way_page_clear(&p->way);
    os64_memset(p, 0, sizeof(*p));
}

// A text/plain body is laid out by handing libhtml `<plaintext>` and then
// the bytes: the standard's own element for "everything after this is
// text", so libflow sees preformatted text and needs nothing new. The
// encoding is the one libway chose for the body.
static bool page_from_text(Page *p)
{
    static const char kOpen[] = "<!doctype html><plaintext>";
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = p->way.text_utf8 ? "utf-8" : "windows-1252";
    os64_html_parser_t *parser = os64_html_parser_new(&opt);
    if (parser == NULL)
        return false;
    os64_html_parser_feed(parser, kOpen, sizeof(kOpen) - 1);
    os64_html_parser_feed(parser, p->way.text, p->way.textlen);
    p->plain = os64_html_parser_finish(parser);
    if (p->plain != NULL)
        p->plain_model = os64_page_build(p->plain, p->way.url, NULL);
    return p->plain != NULL && p->plain_model != NULL;
}

// The size libflow asks of a picture: the one that arrived, or unknown.
// `ctx` is the page being laid out — the one on screen, or one being built
// beside it — so a picture is never measured from the wrong page.
static bool picture_size(void *ctx, const os64_html_node_t *node, int32_t *w, int32_t *h)
{
    const Page *p = ctx;
    int32_t i = p->pic_of != NULL ? os64_page_image_for(page_model(p), node) : -1;
    if (i < 0 || p->pic_of[i] < 0 || p->pics[p->pic_of[i]].state != PIC_SHOWN)
        return false;
    *w = (int32_t)p->pics[p->pic_of[i]].image.width;
    *h = (int32_t)p->pics[p->pic_of[i]].image.height;
    return true;
}

static flow_tree_t *layout_tree(Page *p, int32_t width)
{
    s_env.ctx = p;
    return flow_layout(page_doc(p), page_model(p), width, &s_env);
}

// ── The window ──────────────────────────────────────────────────────────

typedef enum {
    NAV_GO,         // somewhere new: the page left is remembered
    NAV_REFRESH,    // a page's own refresh: a redirector leaves no crumb
    NAV_RELOAD,     // the same page again, where the person was
    NAV_BACK,
    NAV_FORWARD,
} NavKind;

// Whose question the bar is showing.
typedef enum { ASK_NONE = 0, ASK_WORKER, ASK_REQUEST } Asker;

static struct {
    int64_t win;
    os64_draw_ctx_t ctx;
    os64_ui_t ui;
    os64_ui_widget_t root, status, view, back, forward, reload, stop;
    os64_ui_textfield_t field;
    os64_ui_scrollbar_t vbar, hbar;
    // The question bar: the question, and Yes and No.
    os64_ui_widget_t qpanel, qlabel, qyes, qno;
    yonder_bar_t bar;
    Asker asker;
    char qtext[WAY_SENTENCE_MAX];
    uint32_t asked;                 // numbers for questions asked on this thread
    // The request a person is being asked about before it is sent.
    os64_page_request_t pending;
    NavKind pending_kind;
    const char *pending_refused;

    char field_buf[1024];
    char status_text[512];
    // What the status line says when no link is under the pointer.
    char status_rest[512];
    int32_t pointer_x, pointer_y;   // where the pointer last was, for hover
    bool running;
    // A resize is laid out once the events that arrived with it are
    // drained: a drag sends a stream of them, and a big page takes long
    // enough to lay out that doing it per event would freeze the window.
    bool relayout_due;
    uint8_t seq;                    // the view's VT100 key-burst state

    way_session_t way;
    os64_work_pool_t *pool;
    Page page;                      // the page on screen
    int32_t sx, sy;                 // where it is scrolled to, in page pixels
    int32_t hover_link, pressed_link;
    int32_t chain;                  // refresh hops since a person last went anywhere

    // The one navigation in flight. Its mailbox is held here while it is
    // current; a bell from any other is not read.
    struct {
        os64_work_id_t id;
        yonder_mail_t *mail;
        NavKind kind;
        way_position_t crumb;       // BACK, FORWARD: where the person was
        bool has_fragment;
        char fragment[256];
    } nav;
    uint64_t generation;
    // Which page is on screen, so a picture finished for another is let go.
    uint64_t page_serial;
    // A picture arrived whose size may move the page; and when the page was
    // last laid out again for one, to lay out at most once a second while
    // more are coming.
    bool pictures_moved;
    os64_ticks_t pictures_laid;
    uint64_t laid_ms;               // the last layout's time, for the status line
} g;   // zeroed: the session's histories are large, and .data would carry them in the file

// A sentence in the house's status-row voice starts with a space; a line
// of its own does not need it.
static const char *trim(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

static void status_set(const char *text)
{
    os64_strcopy(g.status_text, sizeof(g.status_text), text);
    os64_ui_mark_dirty(&g.ui, &g.status);
}

// What just happened is said at once, even over a link the pointer rests
// on — it was the answer to what the person just did — and it is what the
// line goes back to when the pointer leaves a link.
static void status_rest(const char *text)
{
    os64_strcopy(g.status_rest, sizeof(g.status_rest), trim(text));
    status_set(g.status_rest);
}

// What libway said on this thread, and it is said.
static void say_way(void)
{
    if (g.way.status[0] != '\0')
        status_rest(g.way.status);
    g.way.status[0] = '\0';
}

static int32_t page_height(void)
{
    return g.page.tree != NULL ? flow_height(g.page.tree) : 0;
}

static int32_t page_width(void)
{
    return g.page.tree != NULL ? flow_width(g.page.tree) : 0;
}

static void clamp_scroll(void)
{
    int32_t most_y = page_height() - g.view.bounds.h, most_x = page_width() - g.view.bounds.w;
    if (g.sy > most_y)
        g.sy = most_y;
    if (g.sx > most_x)
        g.sx = most_x;
    if (g.sy < 0)
        g.sy = 0;
    if (g.sx < 0)
        g.sx = 0;
}

static void sync_bars(void)
{
    os64_ui_scrollbar_set(&g.ui, &g.vbar, page_height(), g.view.bounds.h, g.sy);
    os64_ui_scrollbar_set(&g.ui, &g.hbar, page_width(), g.view.bounds.w, g.sx);
}

static void hover(int32_t x, int32_t y);

static void scroll_to(int32_t x, int32_t y)
{
    g.sx = x;
    g.sy = y;
    clamp_scroll();
    sync_bars();
    os64_ui_mark_dirty(&g.ui, &g.view);
    // The page moved under a pointer that did not: what it rests on now.
    hover(g.pointer_x, g.pointer_y);
}

// Where the person is, as libway keeps it for the history: the scroll.
static way_position_t position_now(void)
{
    way_position_t at;
    os64_memset(&at, 0, sizeof(at));
    os64_memcpy(at.bytes, &g.sx, sizeof(g.sx));
    os64_memcpy(at.bytes + sizeof(g.sx), &g.sy, sizeof(g.sy));
    return at;
}

// A remembered position is a HINT: the page that came back need not be the
// page that was left, so it is clamped to the page that did.
static void position_restore(const way_position_t *at)
{
    os64_memcpy(&g.sx, at->bytes, sizeof(g.sx));
    os64_memcpy(&g.sy, at->bytes + sizeof(g.sx), sizeof(g.sy));
    clamp_scroll();
}

static void scroll_to_node(const os64_html_node_t *node)
{
    const flow_box_t *b = node != NULL && g.page.tree != NULL ? flow_box_for(g.page.tree, node)
                                                              : NULL;
    if (node != NULL && b == NULL) {
        status_rest("that target has no box on this page");
        return;
    }
    scroll_to(g.sx, b != NULL ? b->rect.y : 0);
}

static void scroll_to_fragment(const char *name)
{
    const os64_html_node_t *node = NULL;
    os64_page_reason_t reason = os64_page_resolve_fragment(page_model(&g.page), name, &node);
    if (reason != OS64_PAGE_REASON_OK) {
        status_rest(os64_page_reason_name(reason));
        return;
    }
    scroll_to_node(node);
}

// ── Laying the page out ─────────────────────────────────────────────────

static uint64_t ms_between(const os64_ticks_t *t0, const os64_ticks_t *t1)
{
    return t1->per_second != 0 ? (t1->ticks - t0->ticks) * 1000 / t1->per_second : 0;
}

// The page's standing line: where it came from, what the server said, and
// how long it took to lay out — the number to watch.
static void say_laid_out(uint64_t ms)
{
    g.laid_ms = ms;
    char pictures[96] = "";
    int32_t shown = 0;
    for (int32_t i = 0; i < g.page.npics; i++)
        shown += g.page.pics[i].state == PIC_SHOWN;
    if (g.page.npics > 0)
        os64_snprintf(pictures, sizeof(pictures), " - pictures %d of %d%s", shown, g.page.npics,
                      g.page.not_kept > 0 ? " (the rest past the memory kept)" : "");
    char line[512];
    os64_snprintf(line, sizeof(line), "%s%s%s - laid out at %d px in %lu ms%s%s", g.page.way.url,
                  g.page.way.note[0] ? " - " : "", g.page.way.note, g.page.laid_width,
                  (unsigned long)ms, pictures,
                  flow_incomplete(g.page.tree) ? " - INCOMPLETE: out of memory partway" : "");
    status_rest(line);
}

// The node at the top of the view, and how far below the view's top its box
// sits, so a new layout can put that node back where it was.
static const os64_html_node_t *anchor_of(int32_t *offset)
{
    if (g.page.tree == NULL)
        return NULL;
    const flow_box_t *b = flow_hit(g.page.tree, g.sx + 1, g.sy + 1);
    while (b != NULL && b->node == NULL)
        b = b->parent;
    if (b == NULL)
        return NULL;
    *offset = b->rect.y - g.sy;
    return b->node;
}

// Lays the page on screen out again at the view's width. The new tree is
// built beside the old one, which is freed only once the new one exists
// (LAYOUT.md § Bounds).
// `again` lays it out even at the width it has: a picture's size arrived.
static void relayout(bool again)
{
    int32_t width = g.view.bounds.w;
    if (page_doc(&g.page) == NULL || width <= 0 ||
        (!again && g.page.tree != NULL && width == g.page.laid_width))
        return;
    int32_t offset = 0;
    const os64_html_node_t *anchor = anchor_of(&offset);
    os64_ticks_t t0, t1;
    os64_ticks(&t0);
    flow_tree_t *fresh = layout_tree(&g.page, width);
    os64_ticks(&t1);
    if (fresh == NULL) {
        status_rest("Out of memory laying the page out; this is the last layout that fit.");
        return;
    }
    flow_free(g.page.tree);
    g.page.tree = fresh;
    g.page.laid_width = width;
    if (anchor != NULL) {
        const flow_box_t *b = flow_box_for(g.page.tree, anchor);
        if (b != NULL)
            g.sy = b->rect.y - offset;
    }
    clamp_scroll();
    sync_bars();
    say_laid_out(ms_between(&t0, &t1));
    os64_ui_mark_dirty(&g.ui, &g.view);
}

static void request_navigate(os64_page_request_t *request, NavKind kind, way_ask_t ask);
static void buttons_follow(void);
static void pictures_start(Page *p);
static void pictures_leave(Page *p);
static void open_address(const char *url, NavKind kind, const way_position_t *crumb,
                         os64_page_request_t *request);

// After a page arrives: a refresh it declares, judged by libway. A GO is a
// new navigation that leaves no crumb; the chain it belongs to is counted
// from the last place a person chose to go.
static void refresh_if_declared(void)
{
    os64_page_request_t request;
    switch (way_refresh_step(&g.way, &g.page.way, &g.chain, &request)) {
    case WAY_REFRESH_JUMP:
        scroll_to_node(request.anchor);
        os64_page_request_free(&request);
        break;
    case WAY_REFRESH_GO:
        request_navigate(&request, NAV_REFRESH, WAY_ASK_GO);
        break;
    case WAY_REFRESH_NONE:
        break;
    }
    say_way();
    // A delayed refresh is OFFERED: its address waits in the field, and
    // Enter is the person's own decision to go.
    const char *later = way_delayed_refresh(&g.page.way);
    if (later != NULL)
        os64_ui_textfield_set(&g.ui, &g.field, later);
}

// A page arrived: lay it out BESIDE the one on screen and swap only once
// the layout exists, so a page this machine cannot lay out costs a
// sentence and not the page the person was reading.
static void arrive(Page *fresh, NavKind kind, const way_position_t *crumb, const char *fragment)
{
    int32_t width = g.view.bounds.w > 0 ? g.view.bounds.w : 1;
    os64_ticks_t t0, t1;
    os64_ticks(&t0);
    fresh->tree = layout_tree(fresh, width);
    os64_ticks(&t1);
    if (fresh->tree == NULL) {
        page_clear(fresh);
        status_rest("Out of memory laying that page out; this is still the page you were on.");
        return;
    }
    fresh->laid_width = width;
    way_position_t here = position_now();
    bool had = g.page.tree != NULL;
    if (had) {
        if (kind == NAV_GO)
            way_remember(&g.way, g.page.way.url, &here);
        else if (kind == NAV_BACK)
            way_went_back(&g.way, g.page.way.url, &here);
        else if (kind == NAV_FORWARD)
            way_went_forward(&g.way, g.page.way.url, &here);
    }
    pictures_leave(&g.page);
    page_clear(&g.page);
    g.page = *fresh;
    os64_memset(fresh, 0, sizeof(*fresh));
    g.page_serial++;
    pictures_start(&g.page);
    g.hover_link = g.pressed_link = -1;
    if (kind == NAV_BACK || kind == NAV_FORWARD)
        position_restore(crumb);
    else if (kind == NAV_RELOAD)
        clamp_scroll();
    else
        g.sx = g.sy = 0;
    sync_bars();
    os64_ui_textfield_set(&g.ui, &g.field, g.page.way.url);
    say_laid_out(ms_between(&t0, &t1));
    if (fragment != NULL)
        scroll_to_fragment(fragment);
    refresh_if_declared();
    buttons_follow();
    os64_ui_mark_dirty(&g.ui, &g.view);
}

// ── Going somewhere ─────────────────────────────────────────────────────

// A file is read here, on this thread, as it always was: it is local, and
// the parse is the least of what a page costs.
static void open_local(const char *path, NavKind kind, const way_position_t *crumb)
{
    uint8_t *bytes = NULL;
    size_t len = 0;
    char line[512];
    switch (os64_slurp(path, YONDER_FILE_MAX, &bytes, &len)) {
    case OS64_SLURP_OK:
        break;
    case OS64_SLURP_NO_FILE:
        os64_snprintf(line, sizeof(line), "No such file: %s", path);
        status_rest(line);
        return;
    case OS64_SLURP_TOO_BIG:
        os64_snprintf(line, sizeof(line), "Too big to open: %s", path);
        status_rest(line);
        return;
    default:
        os64_snprintf(line, sizeof(line), "Could not read %s", path);
        status_rest(line);
        return;
    }
    Page fresh;
    os64_memset(&fresh, 0, sizeof(fresh));
    os64_snprintf(fresh.way.url, sizeof(fresh.way.url), "file://%s", path);
    fresh.way.doc = parse_file(bytes, len);
    os64_free(bytes);
    fresh.way.model = fresh.way.doc != NULL ? os64_page_build(fresh.way.doc, fresh.way.url, NULL)
                                            : NULL;
    if (fresh.way.model == NULL) {
        page_clear(&fresh);
        status_rest("Out of memory reading the page.");
        return;
    }
    arrive(&fresh, kind, crumb, NULL);
}

static void buttons_follow(void)
{
    bool loading = g.nav.id != 0;
    os64_ui_set_enabled(&g.ui, &g.stop, loading);
    os64_ui_set_enabled(&g.ui, &g.reload, !loading && g.page.tree != NULL);
}

// ── The question bar ────────────────────────────────────────────────────
//
// A question from a worker (libfetch's downgrade hop, mid-fetch) and one
// asked here before anything is sent (a form or a refresh leaving https)
// are shown the same way. The bar is born DISARMED; the loop in main arms
// it once the queue has been seen empty after it was painted (bar.h).

static void layout(void);

static void bar_show(bool up)
{
    os64_ui_set_hidden(&g.ui, &g.qpanel, !up);
    os64_ui_set_hidden(&g.ui, &g.qlabel, !up);
    os64_ui_set_hidden(&g.ui, &g.qyes, !up);
    os64_ui_set_hidden(&g.ui, &g.qno, !up);
    // Disabled until armed, which is also what it looks like.
    os64_ui_set_enabled(&g.ui, &g.qyes, g.bar.armed);
    os64_ui_set_enabled(&g.ui, &g.qno, g.bar.armed);
    layout();
    os64_ui_mark_dirty(&g.ui, &g.root);
}

static void bar_forget(void)
{
    if (g.asker == ASK_REQUEST)
        os64_page_request_free(&g.pending);
    g.asker = ASK_NONE;
    yonder_bar_lower(&g.bar);
    bar_show(false);
}

// A new question replaces one that is up: a worker's is answered No (its
// page is being left), a request waiting on this thread is dropped.
static void bar_ask(Asker asker, uint32_t number, const char *question)
{
    if (g.asker == ASK_WORKER && g.nav.mail != NULL)
        yonder_mail_answer(g.nav.mail, g.bar.number, false);
    if (g.asker == ASK_REQUEST)
        os64_page_request_free(&g.pending);
    g.asker = asker;
    os64_strcopy(g.qtext, sizeof(g.qtext), trim(question));
    yonder_bar_raise(&g.bar, number);
    bar_show(true);
}

static void start_trip(const char *url, os64_page_request_t *request, NavKind kind,
                       const way_position_t *crumb);

static void bar_answered(bool yes)
{
    Asker asker = g.asker;
    uint32_t number = g.bar.number;
    g.asker = ASK_NONE;
    yonder_bar_lower(&g.bar);
    bar_show(false);
    if (asker == ASK_WORKER && g.nav.mail != NULL) {
        yonder_mail_answer(g.nav.mail, number, yes);
    } else if (asker == ASK_REQUEST) {
        if (yes) {
            start_trip(g.pending.url, &g.pending, g.pending_kind, NULL);
        } else {
            os64_page_request_free(&g.pending);
            status_rest(g.pending_refused);
        }
    }
}

// ── The navigation in flight ────────────────────────────────────────────

// Ends the navigation in flight, if there is one. The pool's own
// cancellation reaches its fetch and any question it is waiting on; the
// mailbox this window held is let go, and outlives it for as long as the
// job still holds its own.
static void stop_trip(void)
{
    if (g.nav.id == 0)
        return;
    os64_work_cancel(g.pool, g.nav.id);
    if (g.asker == ASK_WORKER)
        bar_forget();
    yonder_mail_drop(g.nav.mail);
    os64_memset(&g.nav, 0, sizeof(g.nav));
    buttons_follow();
}

// The path a file: address names. `file:///a/b` is the standard spelling
// (an empty host, then the path), `file://localhost/a/b` names this machine
// outright, and `file://a/b` is what people type — read, as browsers read
// it, as /a/b rather than refused for naming a host called "a".
static const char *file_path(const char *after)
{
    static char path[OS64_FETCH_URL_MAX];
    if (os64_strlen(after) >= 10 && os64_memcmp(after, "localhost/", 10) == 0)
        after += 9;
    if (after[0] == '/')
        return after;
    os64_snprintf(path, sizeof(path), "/%s", after);
    return path;
}

// Sends `url` (with `request`'s body, if it is a form: MOVED into the job)
// to a worker. The navigation it replaces is stopped first: one at a time.
static void start_trip(const char *url, os64_page_request_t *request, NavKind kind,
                       const way_position_t *crumb)
{
    if (os64_strlen(url) >= 7 && os64_memcmp(url, "file://", 7) == 0) {
        os64_page_request_free(request);
        stop_trip();
        open_local(file_path(url + 7), kind, crumb);
        return;
    }
    stop_trip();
    if (g.pool == NULL) {
        os64_page_request_free(request);
        status_rest("The background workers stopped; restart yonder to fetch pages.");
        return;
    }
    yonder_mail_t *mail = yonder_mail_new(++g.generation);
    yonder_trip_t *trip = mail != NULL ? os64_calloc(1, sizeof(*trip)) : NULL;
    if (trip == NULL) {
        yonder_mail_drop(mail);
        os64_page_request_free(request);
        status_rest("Out of memory starting that page.");
        return;
    }
    yonder_mail_hold(mail);                 // the job's reference
    trip->kind = YONDER_JOB_TRIP;
    trip->mail = mail;
    trip->session = &g.way;
    trip->window = g.win;
    trip->mail_bell = BELL_MAIL;
    os64_strcopy(trip->url, sizeof(trip->url), url);
    g.nav.has_fragment = false;
    if (request != NULL) {
        g.nav.has_fragment = request->has_fragment;
        if (request->has_fragment)
            os64_strcopy(g.nav.fragment, sizeof(g.nav.fragment),
                         request->fragment != NULL ? request->fragment : "");
        trip->request = *request;
        trip->has_request = true;
        os64_memset(request, 0, sizeof(*request));
        request->control = -1;
    }
    os64_work_t work = {yonder_trip_run, yonder_trip_release, trip, TRIP_RESERVE};
    os64_work_id_t id = os64_work_submit(g.pool, &work);
    if (id == 0) {
        yonder_trip_release(trip, NULL);    // drops the job's reference
        yonder_mail_drop(mail);
        status_rest("Too busy to start another page; try again in a moment.");
        return;
    }
    g.nav.id = id;
    g.nav.mail = mail;
    g.nav.kind = kind;
    if (crumb != NULL)
        g.nav.crumb = *crumb;
    char line[512];
    os64_snprintf(line, sizeof(line), "fetching %s", url);
    status_rest(line);
    buttons_follow();
}

// A request a page makes, judged by libway: refused with its sentence,
// asked about in the bar, or sent. `request` is consumed either way.
static void request_navigate(os64_page_request_t *request, NavKind kind, way_ask_t ask)
{
    way_judgement_t j = way_judge(&g.way, request, ask);
    if (j.kind == WAY_REFUSE) {
        os64_page_request_free(request);
        say_way();
    } else if (j.kind == WAY_QUESTION) {
        bar_ask(ASK_REQUEST, ++g.asked, j.question);
        g.pending = *request;
        os64_memset(request, 0, sizeof(*request));
        g.pending_kind = kind;
        g.pending_refused = j.refused;
    } else {
        start_trip(request->url, request, kind, NULL);
    }
}

// An address a person gave: a path is a file, anything else libway's.
static void open_address(const char *typed, NavKind kind, const way_position_t *crumb,
                         os64_page_request_t *request)
{
    if (typed[0] == '/') {
        stop_trip();
        open_local(typed, kind, crumb);
        return;
    }
    char url[OS64_FETCH_URL_MAX];
    if (!way_typed_address(typed, url, sizeof(url))) {
        status_rest("That address is longer than one may be.");
        return;
    }
    start_trip(url, request, kind, crumb);
}

// ── Pictures ────────────────────────────────────────────────────────────

// Fetches the page's pictures, one job per address, in tree order: libpage's
// list is the record. A picture whose address was refused, or names a
// scheme nothing here fetches, is never asked for.
static void pictures_start(Page *p)
{
    const os64_page_t *model = page_model(p);
    int32_t n = model != NULL ? os64_page_nimages(model) : 0;
    if (n <= 0 || g.pool == NULL)
        return;
    p->pic_of = os64_calloc((size_t)n, sizeof(*p->pic_of));
    p->pics = os64_calloc((size_t)n, sizeof(*p->pics));
    if (p->pic_of == NULL || p->pics == NULL) {
        os64_free(p->pic_of);
        os64_free(p->pics);
        p->pic_of = NULL;
        p->pics = NULL;
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        p->pic_of[i] = -1;
        const char *url = os64_page_image(model, i)->src.url;
        if (url == NULL)
            continue;
        bool fetchable = os64_strlen(url) > 7 &&
                         (os64_memcmp(url, "http://", 7) == 0 ||
                          (os64_strlen(url) > 8 && os64_memcmp(url, "https://", 8) == 0) ||
                          os64_memcmp(url, "file://", 7) == 0);
        if (!fetchable)
            continue;
        int32_t same = -1;
        for (int32_t k = 0; k < p->npics && same < 0; k++)
            if (os64_streq(p->pics[k].url, url))
                same = k;
        if (same >= 0) {
            p->pic_of[i] = same;
            continue;
        }
        Picture *pic = &p->pics[p->npics];
        pic->url = url;
        pic->state = PIC_FAILED;
        yonder_picture_job_t *job = os64_calloc(1, sizeof(*job));
        if (job != NULL) {
            job->kind = YONDER_JOB_PICTURE;
            job->index = p->npics;
            job->generation = g.page_serial;
            job->agent = g.way.agent;
            os64_strcopy(job->url, sizeof(job->url), url);
            os64_work_t work = {yonder_picture_run, yonder_picture_release, job, PICTURE_RESERVE};
            pic->id = os64_work_submit(g.pool, &work);
            if (pic->id != 0) {
                pic->state = PIC_WAITING;
                p->waiting++;
            } else {
                os64_free(job);     // the table is full: this one keeps its frame
            }
        }
        p->pic_of[i] = p->npics++;
    }
}

// The page is being left: its pictures still on their way are cancelled,
// and the pool lets their inputs and products go.
static void pictures_leave(Page *p)
{
    for (int32_t i = 0; i < p->npics; i++)
        if (p->pics[i].state == PIC_WAITING && g.pool != NULL)
            os64_work_cancel(g.pool, p->pics[i].id);
    p->waiting = 0;
}

// Whether a picture's arrival can move the page: libflow sizes a picture
// whose element gives both a width and a height the same whether it has
// arrived or not.
static bool has_percent(const char *v)
{
    for (; v != NULL && *v != '\0'; v++)
        if (*v == '%')
            return true;
    return false;
}

static bool picture_moves_page(const Page *p, int32_t pic)
{
    const os64_page_t *model = page_model(p);
    for (int32_t i = 0; i < os64_page_nimages(model); i++) {
        if (p->pic_of[i] != pic)
            continue;
        const os64_html_node_t *node = os64_page_image(model, i)->node;
        const os64_html_attr_t *w = os64_html_attr(node, "width");
        const os64_html_attr_t *h = os64_html_attr(node, "height");
        if (w == NULL || h == NULL || h->value == NULL || has_percent(h->value))
            return true;
    }
    return false;
}

static void picture_arrived(yonder_picture_job_t *job, yonder_picture_t *product)
{
    if (job->generation != g.page_serial || job->index < 0 || job->index >= g.page.npics)
        return;                     // a page no longer on screen
    Picture *pic = &g.page.pics[job->index];
    if (pic->state != PIC_WAITING)
        return;
    g.page.waiting--;
    if (product == NULL || product->status != OS64_IMAGE_OK) {
        pic->state = PIC_FAILED;
        return;
    }
    size_t bytes = (size_t)product->image.width * product->image.height * 4u;
    if (g.page.kept_bytes + bytes > PICTURES_KEPT_MAX) {
        pic->state = PIC_NOT_KEPT;
        g.page.not_kept++;
        return;
    }
    pic->image = product->image;
    os64_memset(&product->image, 0, sizeof(product->image));
    pic->state = PIC_SHOWN;
    g.page.kept_bytes += bytes;
    if (picture_moves_page(&g.page, job->index))
        g.pictures_moved = true;
    os64_ui_mark_dirty(&g.ui, &g.view);
}

// After a drained batch: lay the page out again if pictures moved it — at
// once when the last one is in, otherwise at most once a second, so a page
// of forty pictures settles without forty layouts.
static void pictures_settle(void)
{
    if (!g.pictures_moved)
        return;
    os64_ticks_t now;
    os64_ticks(&now);
    if (g.page.waiting > 0 && ms_between(&g.pictures_laid, &now) < 1000)
        return;
    g.pictures_moved = false;
    g.pictures_laid = now;
    relayout(true);
}

// ── The doorbell ────────────────────────────────────────────────────────

// A job the pool finished. Only the current navigation's is looked at; any
// other finished before its cancel was noticed, and is let go unread.
static void reaped(os64_work_id_t id, void *job, void *product)
{
    if (*(const uint32_t *)job == YONDER_JOB_PICTURE) {
        picture_arrived(job, product);
        yonder_picture_release(job, product);
        return;
    }
    if (id == 0 || id != g.nav.id) {
        yonder_trip_release(job, product);
        return;
    }
    NavKind kind = g.nav.kind;
    way_position_t crumb = g.nav.crumb;
    char fragment[sizeof(g.nav.fragment)];
    bool has_fragment = g.nav.has_fragment;
    os64_strcopy(fragment, sizeof(fragment), g.nav.fragment);
    if (g.asker == ASK_WORKER)
        bar_forget();
    yonder_mail_drop(g.nav.mail);
    os64_memset(&g.nav, 0, sizeof(g.nav));
    buttons_follow();

    yonder_arrival_t *a = product;
    if (a == NULL || !a->loaded) {
        status_rest(a != NULL ? a->status : "Out of memory fetching that page.");
    } else {
        Page fresh;
        os64_memset(&fresh, 0, sizeof(fresh));
        fresh.way = a->page;
        os64_memset(&a->page, 0, sizeof(a->page));
        if (fresh.way.doc == NULL && !page_from_text(&fresh)) {
            page_clear(&fresh);
            status_rest("Out of memory reading that text.");
        } else {
            arrive(&fresh, kind, &crumb, has_fragment ? fragment : NULL);
        }
    }
    yonder_trip_release(job, product);
}

static void on_doorbell(os64_ui_t *ui, const os64_gui_event_t *ev)
{
    (void)ui;
    uint32_t mask = ev->doorbell.mask;
    if ((mask & BELL_MAIL) && g.nav.mail != NULL &&
        yonder_mail_generation(g.nav.mail) == g.generation) {
        char line[WAY_SENTENCE_MAX];
        uint32_t number = 0;
        if (yonder_mail_take_progress(g.nav.mail, line, sizeof(line)))
            status_rest(line);
        if (yonder_mail_take_question(g.nav.mail, &number, line, sizeof(line)))
            bar_ask(ASK_WORKER, number, line);
    }
    if ((mask & BELL_WORK) && g.pool != NULL) {
        os64_work_id_t id;
        int64_t verdict;
        void *job, *product;
        int32_t waiting = g.page.waiting;
        while (os64_work_reap(g.pool, &id, &verdict, &job, &product))
            reaped(id, job, product);
        // Pictures arrived without moving the page: the count still changed.
        if (g.page.waiting != waiting && !g.pictures_moved)
            say_laid_out(g.laid_ms);
        // A broken pool is destroyed, which cancels and releases what it
        // held; the window goes on showing what it has.
        if (os64_work_pool_error(g.pool) != 0) {
            if (g.asker == ASK_WORKER)
                bar_forget();
            yonder_mail_drop(g.nav.mail);
            os64_memset(&g.nav, 0, sizeof(g.nav));
            os64_work_pool_destroy(g.pool);
            g.pool = NULL;
            buttons_follow();
            status_rest("The background workers stopped; restart yonder to fetch pages.");
        }
    }
}

// ── The page view ───────────────────────────────────────────────────────
//
// The verbs, executed: page coordinates moved onto the glass by the scroll
// and the view's origin, and cut to the view (a paint hook is handed no
// clip; libui's primitives clip only to the canvas).

typedef struct {
    os64_gui_surface_t *surf;
    int32_t dx, dy;             // page -> window
    os64_gui_rect_t clip;       // the view, in window coordinates
} Glass;

static os64_gui_rect_t on_glass(const Glass *gl, os64_gui_rect_t r)
{
    r.x += gl->dx;
    r.y += gl->dy;
    os64_gui_rect_t out;
    if (!os64_rect_intersect(r, gl->clip, &out))
        return (os64_gui_rect_t){0, 0, 0, 0};
    return out;
}

static void glass_fill(void *ctx, os64_gui_rect_t r, uint32_t colour)
{
    const Glass *gl = ctx;
    r = on_glass(gl, r);
    if (r.w > 0 && r.h > 0)
        os64_draw_fill_rect(gl->surf, r, 0xff000000u | colour);
}

static void glass_text(void *ctx, const flow_box_t *b, os64_gui_rect_t clip, uint32_t colour)
{
    (void)clip;
    const Glass *gl = ctx;
    os64_text_draw(b->run, gl->surf, gl->clip, b->rect.x + gl->dx, b->baseline + gl->dy,
                   0xff000000u | colour);
}

// A picture that arrived is drawn into its box, scaled and blended
// (scale.c); one on its way, or that will not come, keeps its frame.
static void glass_image(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    (void)clip;
    const Glass *gl = ctx;
    const Page *p = &g.page;
    int32_t i = p->pic_of != NULL ? os64_page_image_for(page_model(p), b->node) : -1;
    if (i >= 0 && p->pic_of[i] >= 0 && p->pics[p->pic_of[i]].state == PIC_SHOWN) {
        const os64_image_t *img = &p->pics[p->pic_of[i]].image;
        os64_gui_rect_t box = {c.x + gl->dx, c.y + gl->dy, c.w, c.h};
        yonder_draw_picture(gl->surf->pixels, gl->surf->pitch_px, gl->clip, box, img->pixels,
                            img->width, img->height);
        return;
    }
    os64_gui_rect_t r = on_glass(gl, c);
    if (r.w <= 0 || r.h <= 0)
        return;
    os64_draw_fill_rect(gl->surf, r, 0xffe8e8e8u);
    glass_fill(ctx, (os64_gui_rect_t){c.x, c.y, c.w, 1}, 0xa0a0a0);
    glass_fill(ctx, (os64_gui_rect_t){c.x, c.y + c.h - 1, c.w, 1}, 0xa0a0a0);
    glass_fill(ctx, (os64_gui_rect_t){c.x, c.y, 1, c.h}, 0xa0a0a0);
    glass_fill(ctx, (os64_gui_rect_t){c.x + c.w - 1, c.y, 1, c.h}, 0xa0a0a0);
}

// A control is drawn inert until it becomes a widget (slice Y4): a white
// well with a sunken edge.
static void glass_control(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    (void)b;
    (void)clip;
    glass_fill(ctx, c, 0xffffff);
    glass_fill(ctx, (os64_gui_rect_t){c.x, c.y, c.w, 1}, 0x808080);
    glass_fill(ctx, (os64_gui_rect_t){c.x, c.y, 1, c.h}, 0x808080);
    glass_fill(ctx, (os64_gui_rect_t){c.x, c.y + c.h - 1, c.w, 1}, 0xd4d4d4);
    glass_fill(ctx, (os64_gui_rect_t){c.x + c.w - 1, c.y, 1, c.h}, 0xd4d4d4);
}

static void view_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    (void)t;
    Glass gl = {&ctx->surf, w->bounds.x - g.sx, w->bounds.y - g.sy, w->bounds};
    if (g.page.tree == NULL) {
        os64_draw_fill_rect(&ctx->surf, w->bounds, 0xff000000u | PAGE_PAPER);
        return;
    }
    yonder_verbs_t v = {&gl, glass_fill, glass_text, glass_image, glass_control};
    yonder_paint(g.page.tree, (os64_gui_rect_t){g.sx, g.sy, w->bounds.w, w->bounds.h}, PAGE_PAPER, &v);
}

// The keyboard's arrows arrive as VT100 bursts (ESC [ A ...). libui's
// decoder for them is private to libos64, so the view reads the few it
// needs itself.
typedef enum { KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
               KEY_PGUP, KEY_PGDN, KEY_SPACE } Key;

static Key view_key(const os64_gui_event_t *ev)
{
    char a = ev->key.ascii;
    switch (g.seq) {
    case 1:
        g.seq = a == '[' ? 2 : 0;
        return KEY_NONE;
    case 2:
        g.seq = 0;
        switch (a) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '5': g.seq = 5; return KEY_NONE;
        case '6': g.seq = 6; return KEY_NONE;
        default: return KEY_NONE;
        }
    case 5:
    case 6: {
        Key k = a == '~' ? (g.seq == 5 ? KEY_PGUP : KEY_PGDN) : KEY_NONE;
        g.seq = 0;
        return k;
    }
    default:
        break;
    }
    if (a == 0x1b && !os64_ui_key_is_esc(ev)) {
        g.seq = 1;
        return KEY_NONE;
    }
    return a == ' ' ? KEY_SPACE : KEY_NONE;
}

// One line of scrolling: the default font's line, as the view shows it.
static int32_t line_step(void)
{
    return (int32_t)s_env.viewport_font_px * 5 / 4;
}

// The link under a point of the view, or -1.
static int32_t link_at(int32_t x, int32_t y)
{
    os64_gui_rect_t v = g.view.bounds;
    if (g.page.tree == NULL || x < v.x || y < v.y || x >= v.x + v.w || y >= v.y + v.h)
        return -1;
    const flow_box_t *b = flow_hit(g.page.tree, x - v.x + g.sx, y - v.y + g.sy);
    return b != NULL ? b->link : -1;
}

// A link followed: libpage says what it is — a place in this page is a
// move, anywhere else is judged by libway — and a person pressed it, so a
// link is never asked about.
static void follow_link(int32_t link)
{
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_LINK, link, 0, 0};
    os64_page_request_t request;
    os64_page_verdict_t verdict = os64_page_activate(page_model(&g.page), what, &request);
    if (verdict == OS64_PAGE_FRAGMENT) {
        scroll_to_node(request.anchor);
        os64_page_request_free(&request);
    } else if (verdict == OS64_PAGE_NAVIGATE) {
        g.chain = 0;
        request_navigate(&request, NAV_GO, WAY_ASK_NEVER);
    } else {
        status_rest(os64_page_reason_name(request.reason));
        os64_page_request_free(&request);
    }
}

static bool view_event(os64_ui_widget_t *w, os64_ui_t *ui, const os64_gui_event_t *ev)
{
    (void)ui;
    int32_t page = w->bounds.h - line_step();
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_WHEEL:
        // Three lines a notch, as libui's lists scroll; a tilt goes sideways.
        if (ev->mouse.dy == 0 && ev->mouse.dx == 0)
            return false;
        scroll_to(g.sx + ev->mouse.dx * 3 * line_step(), g.sy + ev->mouse.dy * 3 * line_step());
        return true;
    case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN:
        os64_ui_set_focus(&g.ui, w);
        g.pressed_link = ev->mouse.button == OS64_GUI_MOUSE_LEFT
                             ? link_at(ev->mouse.x, ev->mouse.y) : -1;
        return true;
    case OS64_GUI_EVENT_MOUSE_BUTTON_UP: {
        // A click is a press and a release on the same link.
        int32_t link = link_at(ev->mouse.x, ev->mouse.y);
        if (link >= 0 && link == g.pressed_link && ev->mouse.button == OS64_GUI_MOUSE_LEFT)
            follow_link(link);
        g.pressed_link = -1;
        return true;
    }
    case OS64_GUI_EVENT_KEY_DOWN:
        switch (view_key(ev)) {
        case KEY_UP: scroll_to(g.sx, g.sy - line_step()); return true;
        case KEY_DOWN: scroll_to(g.sx, g.sy + line_step()); return true;
        case KEY_LEFT: scroll_to(g.sx - line_step(), g.sy); return true;
        case KEY_RIGHT: scroll_to(g.sx + line_step(), g.sy); return true;
        case KEY_PGUP: scroll_to(g.sx, g.sy - page); return true;
        case KEY_PGDN:
        case KEY_SPACE: scroll_to(g.sx, g.sy + page); return true;
        case KEY_HOME: scroll_to(0, 0); return true;
        case KEY_END: scroll_to(g.sx, page_height()); return true;
        default: return g.seq != 0;
        }
    default:
        return false;
    }
}

static const os64_ui_class_t kViewClass = {
    .name = "pageview",
    .paint = view_paint,
    .event = view_event,
};

// Which link, if any, is under the pointer, said on the status line.
static void hover(int32_t x, int32_t y)
{
    g.pointer_x = x;
    g.pointer_y = y;
    int32_t link = link_at(x, y);
    if (link == g.hover_link)
        return;
    g.hover_link = link;
    if (link < 0) {
        status_set(g.status_rest);
        return;
    }
    const os64_page_link_t *l = os64_page_link(page_model(&g.page), link);
    if (l == NULL)
        return;
    if (l->href.url != NULL) {
        status_set(l->href.url);
        return;
    }
    // Refused: libpage's reason, then what the page wrote — the reason
    // first, because an address can be longer than the line.
    const os64_html_attr_t *href = os64_html_attr(l->node, "href");
    char line[512];
    os64_snprintf(line, sizeof(line), "%s: %s", os64_page_reason_name(l->href.refused),
                  href != NULL && href->value != NULL ? href->value : "(no address)");
    status_set(line);
}

// ── The toolbar ─────────────────────────────────────────────────────────

static void click_back(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    way_crumb_t crumb;
    if (!way_last(&g.way, &crumb)) {
        say_way();
        return;
    }
    g.chain = 0;
    start_trip(crumb.url, NULL, NAV_BACK, &crumb.position);
}

static void click_forward(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    way_crumb_t crumb;
    if (!way_next(&g.way, &crumb)) {
        say_way();
        return;
    }
    g.chain = 0;
    start_trip(crumb.url, NULL, NAV_FORWARD, &crumb.position);
}

static void click_reload(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    if (g.page.tree == NULL)
        return;
    g.chain = 0;
    char url[OS64_FETCH_URL_MAX];
    os64_strcopy(url, sizeof(url), g.page.way.url);
    start_trip(url, NULL, NAV_RELOAD, NULL);
}

static void click_stop(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    if (g.nav.id == 0)
        return;
    stop_trip();
    status_rest("stopped");
}

// ── The window's layout ─────────────────────────────────────────────────

static int32_t button_width(const char *caption)
{
    int32_t tw = 0;
    if (os64_ui_text_measure(&g.ui, OS64_FONT_ROLE_UI, caption, os64_strlen(caption), &tw) !=
        OS64_FONT_OK)
        tw = (int32_t)os64_strlen(caption) * 8;
    return tw + 4 * g.ui.theme.pad;
}

static void layout(void)
{
    const os64_ui_theme_t *t = &g.ui.theme;
    os64_gui_rect_t area = os64_ui_widget_planned_bounds(&g.root);
    int32_t pad = t->pad, gap = t->gap, bh = os64_ui_control_min_height(&g.ui), sw = t->scroll_w;
    int32_t x = pad;
    os64_ui_widget_t *tools[] = {&g.back, &g.forward, &g.reload, &g.stop};
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        int32_t bw = button_width(tools[i]->text);
        tools[i]->bounds = (os64_gui_rect_t){x, pad, bw, bh};
        x += bw + gap;
    }
    g.field.w.bounds = (os64_gui_rect_t){x, pad, area.w - pad - x, bh};
    int32_t top = pad + bh + pad;
    int32_t bottom = area.h - pad - bh;
    int32_t bar_h = g.bar.up ? bh + gap : 0;
    int32_t vh = bottom - pad - sw - top - bar_h, vw = area.w - 2 * pad - sw;
    if (vh < 1)
        vh = 1;
    if (vw < 1)
        vw = 1;
    g.view.bounds = (os64_gui_rect_t){pad, top, vw, vh};
    g.vbar.w.bounds = (os64_gui_rect_t){pad + vw, top, sw, vh};
    g.hbar.w.bounds = (os64_gui_rect_t){pad, top + vh, vw, sw};
    int32_t qy = top + vh + sw + gap;
    int32_t yes = button_width(g.qyes.text), no = button_width(g.qno.text);
    g.qpanel.bounds = (os64_gui_rect_t){pad, qy, area.w - 2 * pad, bh};
    g.qno.bounds = (os64_gui_rect_t){area.w - pad - no, qy, no, bh};
    g.qyes.bounds = (os64_gui_rect_t){area.w - pad - no - gap - yes, qy, yes, bh};
    g.qlabel.bounds = (os64_gui_rect_t){2 * pad, qy, g.qyes.bounds.x - 3 * pad, bh};
    g.status.bounds = (os64_gui_rect_t){pad, bottom, area.w - 2 * pad, bh};
}

static void on_resize(os64_ui_t *ui)
{
    (void)ui;
    layout();
    g.relayout_due = true;
    clamp_scroll();
    sync_bars();
    os64_ui_mark_dirty(&g.ui, &g.root);
}

static void on_close(os64_ui_t *ui)
{
    (void)ui;
    g.running = false;
}

static void scrolled_v(os64_ui_scrollbar_t *sb, void *user)
{
    (void)user;
    scroll_to(g.sx, (int32_t)sb->pos);
}

static void scrolled_h(os64_ui_scrollbar_t *sb, void *user)
{
    (void)user;
    scroll_to((int32_t)sb->pos, g.sy);
}

static void field_submit(os64_ui_textfield_t *tf, void *user)
{
    (void)user;
    char typed[sizeof(g.field_buf)];
    os64_strcopy(typed, sizeof(typed), tf->buf);
    g.chain = 0;
    open_address(typed, NAV_GO, NULL, NULL);
    os64_ui_set_focus(&g.ui, &g.view);
}

static void field_cancel(os64_ui_textfield_t *tf, void *user)
{
    (void)tf;
    (void)user;
    os64_ui_set_focus(&g.ui, &g.view);
}

// Where an event lands on the question bar, if it is up.
static yonder_bar_spot_t bar_spot(int32_t x, int32_t y)
{
    const os64_gui_rect_t *yes = &g.qyes.bounds, *no = &g.qno.bounds;
    if (x >= yes->x && y >= yes->y && x < yes->x + yes->w && y < yes->y + yes->h)
        return BAR_ON_YES;
    if (x >= no->x && y >= no->y && x < no->x + no->w && y < no->y + no->h)
        return BAR_ON_NO;
    return BAR_ELSEWHERE;
}

static bool in_bar(int32_t x, int32_t y)
{
    const os64_gui_rect_t *r = &g.qpanel.bounds;
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

// The bar's own events, before libui sees them: its buttons answer only
// through bar.h's rules, never through a button's own click. True when the
// event was the bar's.
static bool bar_event(const os64_gui_event_t *ev)
{
    if (!g.bar.up)
        return false;
    yonder_bar_answer_t answer = BAR_NOTHING;
    bool mine = false;
    if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN) {
        answer = yonder_bar_press(&g.bar, bar_spot(ev->mouse.x, ev->mouse.y));
        mine = in_bar(ev->mouse.x, ev->mouse.y);
    } else if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP) {
        answer = yonder_bar_release(&g.bar, bar_spot(ev->mouse.x, ev->mouse.y));
        mine = in_bar(ev->mouse.x, ev->mouse.y);
    } else if (ev->type == OS64_GUI_EVENT_KEY_DOWN && os64_ui_key_is_esc(ev)) {
        answer = yonder_bar_escape(&g.bar);
        mine = true;
    }
    if (answer != BAR_NOTHING)
        bar_answered(answer == BAR_YES);
    return mine;
}

// The page's <title>, whitespace collapsed, for the window's name.
static void title_of(const os64_html_document_t *doc, char *out, size_t cap)
{
    out[0] = '\0';
    const os64_html_node_t *t = NULL;
    for (const os64_html_node_t *n = doc != NULL && doc->head != NULL ? doc->head->first_child
                                                                        : NULL;
         n != NULL && t == NULL; n = n->next)
        if (n->kind == OS64_HTML_ELEMENT && n->tag == OS64_HTML_TAG_TITLE)
            t = n;
    size_t at = 0;
    bool space = false;
    for (const os64_html_node_t *c = t != NULL ? t->first_child : NULL; c != NULL; c = c->next) {
        if (c->kind != OS64_HTML_TEXT)
            continue;
        for (size_t i = 0; i < c->text_len && at + 1 < cap; i++) {
            char ch = c->text[i];
            bool ws = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
            if (ws) {
                space = at > 0;
                continue;
            }
            if (space && at + 2 < cap)
                out[at++] = ' ';
            space = false;
            out[at++] = ch;
        }
    }
    out[at] = '\0';
}

// "<page> - yonder" in the title's capacity, which the boundary refuses to
// exceed rather than cutting (gui.h): a long page title is shortened with
// "..." first. Anything past ASCII becomes '?', because the title bar is
// drawn in the kernel's face, not the page's.
static void fit_title(const char *page, char out[OS64_GUI_TITLE_MAX])
{
    static const char kTail[] = " - yonder";
    size_t room = OS64_GUI_TITLE_MAX - 1 - (sizeof(kTail) - 1);
    size_t n = os64_strlen(page), at = 0;
    bool cut = n > room;
    size_t keep = cut ? room - 3 : n;
    for (size_t i = 0; i < keep; i++)
        out[at++] = (unsigned char)page[i] < 0x80 ? page[i] : '?';
    if (cut)
        for (int i = 0; i < 3; i++)
            out[at++] = '.';
    for (size_t i = 0; i < sizeof(kTail) - 1; i++)
        out[at++] = kTail[i];
    out[at] = '\0';
}

int main(int argc, char **argv)
{
    const char *why = faces_open();
    if (why != NULL) {
        os64_printf("yonder: %s\n", why);
        return 1;
    }
    // The window is named after the page, and a window is named once, so a
    // FILE given on the command line is read before the window exists. A
    // page from the network arrives after it, and the window keeps its name.
    const char *first = argc > 1 ? argv[1] : NULL;
    char title[OS64_GUI_TITLE_MAX] = "yonder";
    if (first != NULL && first[0] == '/') {
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (os64_slurp(first, YONDER_FILE_MAX, &bytes, &len) == OS64_SLURP_OK) {
            os64_html_document_t *doc = parse_file(bytes, len);
            char named[120];
            title_of(doc, named, sizeof(named));
            if (named[0] != '\0')
                fit_title(named, title);
            os64_html_document_free(doc);
            os64_free(bytes);
        }
    }
    g.win = os64_gui_window_create(title, 60, 40, 860, 640, 0);
    if (g.win < 0) {
        os64_printf("yonder: no window (is the desktop running?)\n");
        return 1;
    }
    if (os64_draw_ctx_init(&g.ctx, g.win) != 0) {
        os64_gui_window_destroy(g.win);
        return 1;
    }
    g.hover_link = g.pressed_link = -1;
    s_env.replaced_size = picture_size;
    g.way.name = "yonder";
    g.way.agent = YONDER_AGENT;
    g.way.accept = YONDER_ACCEPT;
    g.way.delayed_hint = " - it is in the address field; press Enter to go";

    os64_ui_init(&g.ui, &g.ctx);
    g.ui.on_resize = on_resize;
    g.ui.on_close = on_close;
    g.ui.on_doorbell = on_doorbell;
    os64_ui_panel(&g.root);
    g.root.bounds = (os64_gui_rect_t){0, 0, (int32_t)g.ctx.surf.width, (int32_t)g.ctx.surf.height};
    os64_ui_button(&g.back, "Back", click_back, NULL);
    os64_ui_button(&g.forward, "Forward", click_forward, NULL);
    os64_ui_button(&g.reload, "Reload", click_reload, NULL);
    os64_ui_button(&g.stop, "Stop", click_stop, NULL);
    os64_ui_textfield(&g.field, g.field_buf, sizeof(g.field_buf), field_submit, field_cancel, NULL);
    os64_ui_label(&g.status, g.status_text);
    g.view.cls = &kViewClass;
    g.view.focusable = true;
    os64_ui_scrollbar(&g.vbar, scrolled_v, NULL);
    os64_ui_scrollbar(&g.hbar, scrolled_h, NULL);
    g.hbar.horizontal = true;
    os64_ui_panel(&g.qpanel);
    os64_ui_label(&g.qlabel, g.qtext);
    os64_ui_button(&g.qyes, "Yes", NULL, NULL);
    os64_ui_button(&g.qno, "No", NULL, NULL);
    os64_ui_widget_t *kids[] = {&g.back, &g.forward, &g.reload, &g.stop, &g.field.w, &g.view,
                                &g.vbar.w, &g.hbar.w, &g.qpanel, &g.qlabel, &g.qyes, &g.qno,
                                &g.status};
    for (size_t i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        os64_ui_add_child(&g.root, kids[i]);
    layout();
    os64_ui_set_root(&g.ui, &g.root);
    (void)os64_ui_font_follow(&g.ui);
    bar_show(false);
    layout();

    g.pool = os64_work_pool_create(POOL_WORKERS, POOL_BUDGET, g.win, BELL_WORK);
    if (g.pool == NULL)
        status_rest("No background workers; pages from the network cannot be fetched.");
    buttons_follow();

    if (first != NULL)
        open_address(first, NAV_GO, NULL, NULL);
    else
        status_rest("Type an address, or a path to an HTML file, and press Enter.");
    os64_ui_set_focus(&g.ui, first != NULL ? &g.view : &g.field.w);
    sync_bars();

    // libui's loop, with two things read first: the pointer, because a
    // widget is handed moves only while a button is held and the status
    // line wants every one; and the question bar, which answers only by its
    // own rules. After painting, a bar not yet armed is armed only if the
    // queue is EMPTY: anything waiting may have been done before the bar
    // could be seen, so it is dispatched to a disarmed bar first.
    g.running = true;
    os64_ui_paint(&g.ui);
    os64_gui_event_t ev;
    bool held = false;              // an event taken while looking for an empty queue
    while (g.running && !g.ui.quit) {
        if (!held && os64_gui_event_wait(g.win, &ev) != 1)
            break;
        held = false;
        do {
            if (ev.type == OS64_GUI_EVENT_MOUSE_MOVE)
                hover(ev.mouse.x, ev.mouse.y);
            else if (ev.type == OS64_GUI_EVENT_POINTER_STATE)
                hover(ev.pointer.inside ? ev.pointer.x : -1, ev.pointer.inside ? ev.pointer.y : -1);
            if (!bar_event(&ev))
                os64_ui_dispatch(&g.ui, &ev);
        } while (g.running && os64_gui_event_poll(g.win, &ev) == 1);
        if (g.relayout_due) {
            g.relayout_due = false;
            relayout(false);
        }
        pictures_settle();
        os64_ui_paint(&g.ui);
        if (g.bar.up && !g.bar.armed) {
            if (os64_gui_event_poll(g.win, &ev) == 1) {
                held = true;
            } else {
                yonder_bar_settled(&g.bar);
                bar_show(true);
                os64_ui_paint(&g.ui);
            }
        }
    }

    // The workers first: destroy cancels them — a fetch and a question
    // alike hear the pool's own predicate — and joins them, and releases
    // what they held, before anything they could touch goes away.
    os64_work_pool_destroy(g.pool);
    if (g.asker == ASK_REQUEST)
        os64_page_request_free(&g.pending);
    yonder_mail_drop(g.nav.mail);
    page_clear(&g.page);
    os64_ui_font_release(&g.ui);
    for (int i = 0; i < s_faces.nopen; i++)
        os64_text_font_release(s_faces.open[i].font);
    os64_text_destroy(s_faces.text);
    os64_gui_window_destroy(g.win);
    return 0;
}
