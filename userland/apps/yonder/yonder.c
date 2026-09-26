// yonder — the graphical browser (YONDER.md). This slice: a page from a
// FILE, laid out by libflow, painted by paint.c, scrolled and resized in a
// libui window.
//
//     yonder /tests/pages/hacker-news.html
//
// Everything about what the page means or where its boxes go belongs to
// libpage and libflow; this file decides how the page looks on the glass
// and how a person moves around it.

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
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
#include "paint.h"

// The largest file yonder reads: libhtml refuses by size beyond its own
// budget anyway, and says so; this only keeps a stray path from reading a
// disk image into memory first.
#define YONDER_FILE_MAX (32u << 20)

#define PAGE_INK   0x000000u
#define PAGE_LINK  0x0000eeu
#define PAGE_PAPER 0xffffffu

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

// ── The window ──────────────────────────────────────────────────────────

static struct {
    int64_t win;
    os64_draw_ctx_t ctx;
    os64_ui_t ui;
    os64_ui_widget_t root, status, view;
    os64_ui_textfield_t field;
    os64_ui_scrollbar_t vbar, hbar;
    char field_buf[1024];
    char status_text[512];
    // What the status line says when no link is under the pointer.
    char status_rest[512];
    bool running;
    // A resize is laid out once the events that arrived with it are
    // drained: a drag sends a stream of them, and a big page takes long
    // enough to lay out that doing it per event would freeze the window.
    bool relayout_due;
    uint8_t seq;                // the view's VT100 key-burst state

    // The page: its bytes, tree, model and laid-out boxes, and where it is
    // scrolled to, in page pixels.
    char *bytes;
    os64_html_document_t *doc;
    os64_page_t *model;
    flow_tree_t *tree;
    int32_t laid_width;
    int32_t sx, sy;
    int32_t hover_link;
} g = {.hover_link = -1};

static void status_set(const char *text)
{
    os64_strcopy(g.status_text, sizeof(g.status_text), text);
    os64_ui_mark_dirty(&g.ui, &g.status);
}

static void status_rest(const char *text)
{
    os64_strcopy(g.status_rest, sizeof(g.status_rest), text);
    if (g.hover_link < 0)
        status_set(text);
}

static int32_t page_height(void)
{
    return g.tree != NULL ? flow_height(g.tree) : 0;
}

static int32_t page_width(void)
{
    return g.tree != NULL ? flow_width(g.tree) : 0;
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

static void scroll_to(int32_t x, int32_t y)
{
    g.sx = x;
    g.sy = y;
    clamp_scroll();
    sync_bars();
    os64_ui_mark_dirty(&g.ui, &g.view);
}

// ── Laying the page out ─────────────────────────────────────────────────

// The node at the top of the view, and how far below the view's top its box
// sits, so a new layout can put that node back where it was.
static const os64_html_node_t *anchor_of(int32_t *offset)
{
    if (g.tree == NULL)
        return NULL;
    const flow_box_t *b = flow_hit(g.tree, g.sx + 1, g.sy + 1);
    while (b != NULL && b->node == NULL)
        b = b->parent;
    if (b == NULL)
        return NULL;
    *offset = b->rect.y - g.sy;
    return b->node;
}

// Lays the page out at the view's width. The new tree is built beside the
// old one, which is freed only once the new one exists (LAYOUT.md § Bounds).
static void relayout(bool keep_place)
{
    int32_t width = g.view.bounds.w;
    if (g.doc == NULL || width <= 0 || (g.tree != NULL && width == g.laid_width))
        return;
    int32_t offset = 0;
    const os64_html_node_t *anchor = keep_place ? anchor_of(&offset) : NULL;
    os64_ticks_t t0, t1;
    os64_ticks(&t0);
    flow_tree_t *fresh = flow_layout(g.doc, g.model, width, &s_env);
    os64_ticks(&t1);
    if (fresh == NULL) {
        status_rest("Out of memory laying the page out; this is the last layout that fit.");
        return;
    }
    flow_free(g.tree);
    g.tree = fresh;
    g.laid_width = width;
    if (anchor != NULL) {
        const flow_box_t *b = flow_box_for(g.tree, anchor);
        if (b != NULL)
            g.sy = b->rect.y - offset;
    }
    clamp_scroll();
    sync_bars();
    char line[512];
    uint64_t ms = t1.per_second != 0 ? (t1.ticks - t0.ticks) * 1000 / t1.per_second : 0;
    os64_snprintf(line, sizeof(line), "%s - laid out at %d px in %lu ms%s", g.field_buf, width,
                  (unsigned long)ms,
                  flow_incomplete(g.tree) ? " - INCOMPLETE: out of memory partway" : "");
    status_rest(line);
    os64_ui_mark_dirty(&g.ui, &g.view);
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

static void page_drop(void)
{
    flow_free(g.tree);
    os64_page_free(g.model);
    os64_html_document_free(g.doc);
    os64_free(g.bytes);
    g.tree = NULL;
    g.model = NULL;
    g.doc = NULL;
    g.bytes = NULL;
    g.laid_width = 0;
}

// Opens `path` as the page. The page shown is replaced only when the new
// one parses; a file that will not open costs a sentence, not the page.
static void page_open(const char *path)
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
    os64_html_document_t *doc = parse_file(bytes, len);
    char url[1100];
    os64_snprintf(url, sizeof(url), "file://%s", path);
    os64_page_t *model = doc != NULL ? os64_page_build(doc, url, NULL) : NULL;
    if (model == NULL) {
        os64_page_free(model);
        os64_html_document_free(doc);
        os64_free(bytes);
        status_rest("Out of memory reading the page.");
        return;
    }
    page_drop();
    g.bytes = (char *)bytes;
    g.doc = doc;
    g.model = model;
    g.sx = g.sy = 0;
    g.hover_link = -1;
    os64_ui_textfield_set(&g.ui, &g.field, path);
    relayout(false);
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

// A picture has not arrived until images do (YONDER.md slice Y5): its
// place, framed.
static void glass_image(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    (void)b;
    (void)clip;
    const Glass *gl = ctx;
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
    if (g.tree == NULL) {
        os64_draw_fill_rect(&ctx->surf, w->bounds, 0xff000000u | PAGE_PAPER);
        return;
    }
    yonder_verbs_t v = {&gl, glass_fill, glass_text, glass_image, glass_control};
    yonder_paint(g.tree, (os64_gui_rect_t){g.sx, g.sy, w->bounds.w, w->bounds.h}, PAGE_PAPER, &v);
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
        if (g.hover_link >= 0)
            status_set("Following a link arrives with the network (YONDER.md slice Y3).");
        return true;
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
    int32_t link = -1;
    os64_gui_rect_t v = g.view.bounds;
    if (g.tree != NULL && x >= v.x && y >= v.y && x < v.x + v.w && y < v.y + v.h) {
        const flow_box_t *b = flow_hit(g.tree, x - v.x + g.sx, y - v.y + g.sy);
        if (b != NULL)
            link = b->link;
    }
    if (link == g.hover_link)
        return;
    g.hover_link = link;
    if (link < 0) {
        status_set(g.status_rest);
        return;
    }
    const os64_page_link_t *l = os64_page_link(g.model, link);
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

// ── The window's layout ─────────────────────────────────────────────────

static void layout(void)
{
    const os64_ui_theme_t *t = &g.ui.theme;
    os64_gui_rect_t area = os64_ui_widget_planned_bounds(&g.root);
    int32_t pad = t->pad, bh = os64_ui_control_min_height(&g.ui), sw = t->scroll_w;
    g.field.w.bounds = (os64_gui_rect_t){pad, pad, area.w - 2 * pad, bh};
    int32_t top = pad + bh + pad;
    int32_t bottom = area.h - pad - bh;
    int32_t vh = bottom - pad - sw - top, vw = area.w - 2 * pad - sw;
    if (vh < 1)
        vh = 1;
    if (vw < 1)
        vw = 1;
    g.view.bounds = (os64_gui_rect_t){pad, top, vw, vh};
    g.vbar.w.bounds = (os64_gui_rect_t){pad + vw, top, sw, vh};
    g.hbar.w.bounds = (os64_gui_rect_t){pad, top + vh, vw, sw};
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
    char path[sizeof(g.field_buf)];
    os64_strcopy(path, sizeof(path), tf->buf);
    page_open(path);
    os64_ui_set_focus(&g.ui, &g.view);
}

static void field_cancel(os64_ui_textfield_t *tf, void *user)
{
    (void)tf;
    (void)user;
    os64_ui_set_focus(&g.ui, &g.view);
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
    // page given on the command line is read before the window exists.
    const char *first = argc > 1 ? argv[1] : NULL;
    char title[OS64_GUI_TITLE_MAX] = "yonder";
    if (first != NULL) {
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
    os64_ui_init(&g.ui, &g.ctx);
    g.ui.on_resize = on_resize;
    g.ui.on_close = on_close;
    os64_ui_panel(&g.root);
    g.root.bounds = (os64_gui_rect_t){0, 0, (int32_t)g.ctx.surf.width, (int32_t)g.ctx.surf.height};
    os64_ui_textfield(&g.field, g.field_buf, sizeof(g.field_buf), field_submit, field_cancel, NULL);
    os64_ui_label(&g.status, g.status_text);
    g.view.cls = &kViewClass;
    g.view.focusable = true;
    os64_ui_scrollbar(&g.vbar, scrolled_v, NULL);
    os64_ui_scrollbar(&g.hbar, scrolled_h, NULL);
    g.hbar.horizontal = true;
    os64_ui_add_child(&g.root, &g.field.w);
    os64_ui_add_child(&g.root, &g.view);
    os64_ui_add_child(&g.root, &g.vbar.w);
    os64_ui_add_child(&g.root, &g.hbar.w);
    os64_ui_add_child(&g.root, &g.status);
    layout();
    os64_ui_set_root(&g.ui, &g.root);
    (void)os64_ui_font_follow(&g.ui);

    if (first != NULL)
        page_open(first);
    else
        status_rest("Type a path to an HTML file and press Enter.");
    os64_ui_set_focus(&g.ui, first != NULL ? &g.view : &g.field.w);
    sync_bars();

    // libui's loop, with the pointer read first: a widget is handed moves
    // only while a button is held, and the status line wants every one.
    g.running = true;
    os64_ui_paint(&g.ui);
    while (g.running && !g.ui.quit) {
        os64_gui_event_t ev;
        if (os64_gui_event_wait(g.win, &ev) != 1)
            break;
        do {
            if (ev.type == OS64_GUI_EVENT_MOUSE_MOVE)
                hover(ev.mouse.x, ev.mouse.y);
            else if (ev.type == OS64_GUI_EVENT_POINTER_STATE)
                hover(ev.pointer.inside ? ev.pointer.x : -1, ev.pointer.inside ? ev.pointer.y : -1);
            os64_ui_dispatch(&g.ui, &ev);
        } while (g.running && os64_gui_event_poll(g.win, &ev) == 1);
        if (g.relayout_due) {
            g.relayout_due = false;
            relayout(true);
        }
        os64_ui_paint(&g.ui);
    }

    page_drop();
    os64_ui_font_release(&g.ui);
    for (int i = 0; i < s_faces.nopen; i++)
        os64_text_font_release(s_faces.open[i].font);
    os64_text_destroy(s_faces.text);
    os64_gui_window_destroy(g.win);
    return 0;
}
