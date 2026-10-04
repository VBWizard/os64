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

#include <stddef.h>

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
#include "garb/cascade.h"
#include "way/way.h"
#include "os64/os64.h"
#include "os64/draw.h"
#include "os64/fmt.h"
#include "os64/font_backend.h"
#include "os64/font_config.h"
#include "os64/font_provider.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/slurp.h"
#include "os64/str.h"
#include "os64/text.h"
#include "os64/text_draw.h"
#include "os64/url.h"
#include "os64/ui.h"
#include "os64/work.h"
#include "agent.h"
#include "bar.h"
#include "mail.h"
#include "paint.h"
#include "picture.h"
#include "sheet.h"
#include "scale.h"
#include "settings.h"
#include "ticker.h"
#include "trip.h"

// The largest file yonder reads: libhtml refuses by size beyond its own
// budget anyway, and says so; this only keeps a stray path from reading a
// disk image into memory first.
#define YONDER_FILE_MAX (32u << 20)

#define PAGE_INK   0x000000u
#define PAGE_LINK  0x0000eeu
#define PAGE_PAPER 0xffffffu

#define YONDER_ACCEPT "text/html, application/xhtml+xml, text/*;q=0.8"

// The work pool. A navigation declares what one page can cost: libhtml's
// 8 MiB of input and 64 MiB of tree, and the model beside them; a picture
// declares PICTURE_RESERVE. The budget fits a page and PICTURES_AT_ONCE.
#define POOL_WORKERS 4
#define TRIP_RESERVE (80u << 20)
#define POOL_BUDGET  (768u << 20)

// What a page's pictures may cost to keep — a still one's pixels, a moving
// one's whole sequence; past it a picture draws as its frame.
#define PICTURES_KEPT_MAX ((size_t)256u << 20)

// A page's pictures in the pool at once: one fewer than its workers. The
// pool admits in the order it was given work, so a navigation submitted
// after a page's pictures would wait for every one of them; yonder keeps
// the rest itself and hands over the next as each comes back, which keeps
// a worker free for the person — once any pictures of a page left behind
// have seen they were cancelled.
#define PICTURES_AT_ONCE (POOL_WORKERS - 1)

// A layout at most this slow is repeated for every batch of pictures that
// moves the page. A slower one waits for the page's last picture, or until
// its last layout is PICTURES_STALE_MS old and twice its own cost — at
// most a third of the time spent laying out while pictures trickle in.
#define PICTURES_CHEAP_LAYOUT_MS 100
#define PICTURES_STALE_MS 2000

// The window's doorbell bits: the pool's completions, and a navigation's
// mailbox (progress and questions).
#define BELL_WORK (1u << 0)
#define BELL_MAIL (1u << 1)
#define BELL_TICK (1u << 2)       // a moving picture's next frame is due
#define BELL_SETTINGS (1u << 3)   // something arrived at the Settings window

// A GIF frame that asks for 10 ms or less is shown for this long, as every
// browser shows the ones that ask for "as fast as you can".
#define FRAME_FLOOR_MS 100

// ── The page's fonts ────────────────────────────────────────────────────
//
// ONE text context for every page, apart from libui's chrome context
// (LAYOUT.md § Bounds), and on it the provider's family cache (brief 04):
// serif, sans and mono, each regular, bold, italic and bold italic, at any
// size, from `fonts.conf`'s family lines or the shipped DejaVu faces.

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

static struct {
    os64_text_context_t *text;
    os64_font_family_cache_t *families;
} s_faces;

// libflow's family list and the provider's are the same shape in the same
// generic order, so a style's list is handed over as it is.
_Static_assert(sizeof(flow_family_name_t) == sizeof(os64_font_family_name_t) &&
               offsetof(flow_family_name_t, name) == offsetof(os64_font_family_name_t, name) &&
               offsetof(flow_family_name_t, len) == offsetof(os64_font_family_name_t, len),
               "a family name is one shape on both sides");
_Static_assert((int)FLOW_GENERIC_SERIF == (int)OS64_FONT_FAMILY_SERIF &&
               (int)FLOW_GENERIC_SANS == (int)OS64_FONT_FAMILY_SANS &&
               (int)FLOW_GENERIC_MONO == (int)OS64_FONT_FAMILY_MONO,
               "the generics are in one order on both sides");

// The list is borrowed until the next call, which is libflow's contract
// for this callback too (flow_env_t.fonts). The metrics are the primary
// face's, as an empty run would report them: a line at least as tall as
// its ascent and descent.
static os64_font_status_t page_fonts(void *ctx, const flow_family_list_t *families, bool bold,
                                     bool italic, uint32_t px, os64_text_font_t *const **list,
                                     size_t *count, os64_font_face_info_t *primary)
{
    (void)ctx;
    os64_font_family_list_t asked = {
        .names = (const os64_font_family_name_t *)families->names,
        .count = families->count,
        .generic = (os64_font_family_t)families->generic,
    };
    os64_font_role_view_t view;
    os64_font_status_t st = os64_font_family_open(s_faces.families, &asked, bold, italic, px, &view);
    if (st != OS64_FONT_OK)
        return st;
    *list = view.fonts;
    *count = view.font_count;
    *primary = view.primary;
    if (primary->line_height < primary->ascent + primary->descent)
        primary->line_height = primary->ascent + primary->descent;
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

// The shipped DejaVu set, as fonts.conf's defaults name it: what yonder
// falls back on when the configured families will not all open.
static const char *const kShipped[OS64_FONT_FAMILY_COUNT][OS64_FONT_FAMILY_STYLES] = {
    {"/etc/fonts/DejaVuSerif.ttf", "/etc/fonts/DejaVuSerif-Bold.ttf",
     "/etc/fonts/DejaVuSerif-Italic.ttf", "/etc/fonts/DejaVuSerif-BoldItalic.ttf"},
    {"/etc/fonts/DejaVuSans.ttf", "/etc/fonts/DejaVuSans-Bold.ttf",
     "/etc/fonts/DejaVuSans-Oblique.ttf", "/etc/fonts/DejaVuSans-BoldOblique.ttf"},
    {"/etc/fonts/DejaVuSansMono.ttf", "/etc/fonts/DejaVuSansMono-Bold.ttf",
     "/etc/fonts/DejaVuSansMono-Oblique.ttf", "/etc/fonts/DejaVuSansMono-BoldOblique.ttf"},
};

// Whatever of the shipped set is readable: a missing style takes its
// family's regular, a missing family the sans regular, and with none of
// them the engine's bitmap face — a page drawn plainly rather than no
// yonder at all. Says on the terminal what was missing.
static bool faces_fallback(void)
{
    uint8_t *bytes[OS64_FONT_FAMILY_COUNT][OS64_FONT_FAMILY_STYLES] = {{0}};
    size_t len[OS64_FONT_FAMILY_COUNT][OS64_FONT_FAMILY_STYLES] = {{0}};
    int missing = 0;
    const char *first = NULL;
    for (int f = 0; f < (int)OS64_FONT_FAMILY_COUNT; f++)
        for (int st = 0; st < (int)OS64_FONT_FAMILY_STYLES; st++)
            if (os64_slurp(kShipped[f][st], OS64_FONT_FILE_MAX, &bytes[f][st], &len[f][st]) !=
                OS64_SLURP_OK) {
                bytes[f][st] = NULL;
                missing++;
                if (first == NULL)
                    first = kShipped[f][st];
            }
    os64_font_family_spec_t specs[OS64_FONT_FAMILY_COUNT];
    os64_memset(specs, 0, sizeof(specs));
    for (int f = 0; f < (int)OS64_FONT_FAMILY_COUNT; f++)
        for (int st = 0; st < (int)OS64_FONT_FAMILY_STYLES; st++) {
            int from_f = f, from_s = st;
            if (bytes[from_f][from_s] == NULL)
                from_s = 0;
            if (bytes[from_f][from_s] == NULL)
                from_f = OS64_FONT_FAMILY_SANS;
            if (bytes[from_f][from_s] != NULL)
                specs[f].styles[st] = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,
                                                           bytes[from_f][from_s],
                                                           len[from_f][from_s]};
        }
    // The cache keeps its own copy of every file it takes.
    bool ok = os64_font_family_cache_create(s_faces.text, specs, &s_faces.families, NULL, NULL) ==
              OS64_FONT_OK;
    for (int f = 0; f < (int)OS64_FONT_FAMILY_COUNT; f++)
        for (int st = 0; st < (int)OS64_FONT_FAMILY_STYLES; st++)
            os64_free(bytes[f][st]);
    if (ok && missing > 0)
        os64_printf("yonder: %d of 12 faces missing from /etc/fonts (the first: %s); their "
                    "families are drawn in what is there. `os64get` the fonts lot to fill them.\n",
                    missing, first);
    return ok;
}

static const char *faces_open(void)
{
    os64_text_options_t o = {.memory = {NULL, text_alloc, text_free},
                             .backend = os64_freetype_backend_v1()};
    if (os64_text_create(&o, &s_faces.text) != OS64_FONT_OK)
        return "no text context for the page";
    s_env.text = s_faces.text;
    os64_font_config_t config;
    os64_font_config_error_t error;
    if (os64_font_config_read(&config, &error) == OS64_FONT_CONFIG_OK &&
        os64_font_config_family_prepare(s_faces.text, &config, &s_faces.families, &error) ==
            OS64_FONT_CONFIG_OK)
        return NULL;
    // Whatever failed — a line that will not parse, a family named twice, a
    // face that will not open, or the file itself unreadable — is said
    // before the fallback speaks, by the config's own name for it.
    const char *why = os64_font_config_status_name(error.status);
    if (error.line > 0)
        os64_printf("yonder: fonts.conf line %u could not be used (%s); using the shipped "
                    "faces\n", (unsigned)error.line, why);
    else
        os64_printf("yonder: fonts.conf could not be used (%s); using the shipped faces\n", why);
    return faces_fallback() ? NULL : "no faces for the page, not even the built-in one";
}

// A file arrives with no Content-Type to name its encoding, and a page
// that declares none in a <meta> is read as windows-1252 by the standard's
// default. Bytes that are valid UTF-8 and not plain ASCII are almost never
// meant as windows-1252, so they are read as what they are — the file
// detector's answer, not a rule the standard makes. It is handed to
// libhtml as the transport's label, which outranks the page: when it fires,
// the page's <meta> is not read at all. A page that declares
// windows-1252 and is UTF-8 is the case it exists for.
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

// A picture of the page's, one per ADDRESS: forty spacer GIFs are one
// fetch. Its address is the page model's, which lives as long as it does —
// or, for one a sheet names, the table's own copy (`owned`). QUEUED is
// yonder's to hand over; WAITING is in the pool.
typedef enum { PIC_QUEUED, PIC_WAITING, PIC_SHOWN, PIC_FAILED, PIC_NOT_KEPT } PictureState;

typedef struct {
    const char *url;
    bool owned;
    PictureState state;
    os64_image_t image;             // a still picture's pixels
    os64_image_sequence_t *moving;  // or a moving one's, its frame on show
    uint64_t due;                   // when its next frame is, while it plays
    bool playing;                   // false once it stops: its loops done, or a frame failed
    os64_work_id_t id;              // while WAITING
    // An image naming it gives no width and height of its own, so its
    // arrival moves the page. Decided once, as the pictures are gathered.
    bool moves;
} Picture;

// One of a page's style sheets (GARB.md, G3 and G4): a `style` element's,
// a linked one, or one an @import brought. Entries never move once made —
// the cascade's input points at their parses — and an @import's entry is
// made when its importer is ready.
#define SHEETS_MAX 64           // a page's sheets, @imports included
#define SHEET_IMPORTS_MAX 16    // the @imports one sheet may make
#define SHEETS_WAIT_MS 3000     // how long a page waits for its sheets before it is shown

typedef struct {
    garb_parsed_t parsed;
    bool ready;                     // parsed: it is in the cascade
    bool waiting;                   // its job is out, as `id`
    // The first paint waits for it: its media holds on this glass now, or
    // it is an import of a sheet that does. One that does not is fetched
    // all the same, and laid in when it lands.
    bool holds;
    os64_work_id_t id;
    const char *media;              // a sheet the page names: its element's; NULL otherwise
    int32_t importer, import_index; // -1 for a sheet the page names; else its @import
    char *url;                      // where it came from, its @imports' base; NULL for a `style`
    garb_import_t *imports;         // once ready: its @imports, and each one's entry or -1
    int32_t *child;
    int32_t nimports;
} Sheet;

// Where a person scrolled a box, by its element: a layout starts every box
// at the top, so each new one is set again from these (page_lay_out).
typedef struct {
    const os64_html_node_t *node;
    flow_point_t at;
} BoxScroll;

// A page as the window holds it: libway's page (the tree, what it means,
// the reader's flips) and its layout at the size it was laid out at. A
// text/plain page has no tree of its own, so it is given one.
typedef struct {
    way_page_t way;
    os64_html_document_t *plain;
    os64_page_t *plain_model;
    flow_tree_t *tree;
    int32_t laid_width, laid_height;
    // The page's sheets, and their cascade at the size the tree was laid
    // out at; it is judged again when the size or the sheets change. The
    // tree points into both. `serial` names the page to a sheet's job.
    // `sheets_waiting` counts the sheets out that the first paint waits for
    // (Sheet.holds), not every sheet out.
    Sheet *sheets;
    int32_t nsheets, sheets_waiting, sheets_ready;
    bool sheets_changed;
    garb_cascade_t *cascade;
    // For each of the cascade's inputs, the sheet entry it is: what a url()
    // in a winner is resolved against (flow_layer_t.sheet).
    int32_t cascade_entry[SHEETS_MAX];
    uint64_t serial;
    // The pictures, one per address, and for each of libpage's pictures
    // (os64_page_image) and backgrounds (os64_page_background) which of
    // these it is, -1 for one that will not be fetched.
    Picture *pics;
    int32_t npics, cap_pics, *pic_of, *bg_of;
    // The table's index by address (picture_for): open-addressed, at most
    // half full, grown with the table.
    int32_t *pic_slot;
    size_t pic_slot_cap;
    // Still to come (QUEUED or WAITING), and of those, in the pool; the
    // next picture to hand it.
    int32_t waiting, in_flight, next_pic;
    int32_t not_kept;
    size_t kept_bytes;
    // The form whose reply this is, when the reply was to a POST: what
    // Reload sends again. Its url is NULL otherwise.
    os64_page_request_t sent;
    BoxScroll *box_scrolls;
    int32_t nbox_scrolls, cap_box_scrolls;
} Page;

static void pictures_schedule(void);
static uint64_t frame_delay(const Picture *pic);

// A shown picture's pixels: the still image, or the frame a moving one is on.
static const uint32_t *picture_pixels(const Picture *pic, uint32_t *w, uint32_t *h)
{
    if (pic->moving != NULL) {
        const os64_image_frame_t *f = os64_image_sequence_frame(pic->moving);
        *w = f->width;
        *h = f->height;
        return f->pixels;
    }
    *w = pic->image.width;
    *h = pic->image.height;
    return pic->image.pixels;
}

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
    for (int32_t i = 0; i < p->npics; i++) {
        os64_image_free(&p->pics[i].image);
        os64_image_sequence_free(p->pics[i].moving);
        if (p->pics[i].owned)
            os64_free((char *)p->pics[i].url);
    }
    os64_free(p->pics);
    os64_free(p->pic_slot);
    os64_free(p->pic_of);
    os64_free(p->bg_of);
    flow_free(p->tree);
    garb_cascade_free(p->cascade);
    for (int32_t i = 0; i < p->nsheets; i++) {
        garb_free(&p->sheets[i].parsed);
        os64_free(p->sheets[i].url);
        os64_free(p->sheets[i].imports);
        os64_free(p->sheets[i].child);
    }
    os64_free(p->sheets);
    os64_page_free(p->plain_model);
    os64_html_document_free(p->plain);
    way_page_clear(&p->way);
    os64_page_request_free(&p->sent);
    os64_free(p->box_scrolls);
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
        p->plain_model = os64_page_build(p->plain, p->way.url, NULL, NULL);
    return p->plain != NULL && p->plain_model != NULL;
}

// An entry of the cascade's input for sheet `e` and, before it, for the
// ready sheets its @imports brought, each naming it as `parent`
// (garb/cascade.h). Its index, or -1 when it is not ready.
// The cascade writes into each sheet's arena as it reads a rule's block
// (garb_rules_of), so the page is not const here.
static int32_t sheet_emit(Page *p, int32_t e, garb_sheet_in_t *in, int32_t *entry, int32_t *n)
{
    Sheet *sh = &p->sheets[e];
    if (!sh->ready)
        return -1;
    int32_t kids[SHEET_IMPORTS_MAX], nkids = 0;
    for (int32_t k = 0; k < sh->nimports; k++) {
        int32_t at = sh->child[k] >= 0 ? sheet_emit(p, sh->child[k], in, entry, n) : -1;
        if (at >= 0)
            kids[nkids++] = at;
    }
    int32_t at = (*n)++;
    entry[at] = e;
    in[at] = (garb_sheet_in_t){
        .sheet = &sh->parsed,
        .media = sh->media,
        .via = sh->importer >= 0 ? &p->sheets[sh->importer].imports[sh->import_index] : NULL,
    };
    for (int32_t k = 0; k < nkids; k++)
        in[kids[k]].parent = at;
    return at;
}

// Lays the page out at `width` x `height`, cascading its sheets again when
// they changed or the size is not the one they were cascaded at — a media
// query or a vw reads it. The new tree replaces the old only once it
// exists (LAYOUT.md § Bounds), and the cascade the old one points into is
// freed with it. False on no memory, the page on screen untouched.
static bool page_lay_out(Page *p, int32_t width, int32_t height)
{
    garb_cascade_t *cascade = p->cascade;
    int32_t entry[SHEETS_MAX] = {0};
    garb_env_t view = {width, height};
    if (p->sheets_changed || (cascade != NULL && (garb_cascade_env(cascade).width != view.width ||
                                                  garb_cascade_env(cascade).height != view.height))) {
        garb_sheet_in_t in[SHEETS_MAX];
        int32_t n = 0;
        for (int32_t e = 0; e < p->nsheets; e++)
            if (p->sheets[e].importer < 0)
                (void)sheet_emit(p, e, in, entry, &n);
        cascade = n > 0 ? garb_cascade(in, n, page_doc(p), view) : NULL;
        if (n > 0 && cascade == NULL)
            return false;
    }
    s_env.ctx = p;
    s_env.cascade = cascade;
    // One height for `vh` and the initial containing block (flow_env_t).
    s_env.viewport_height = height;
    flow_tree_t *fresh = flow_layout(page_doc(p), page_model(p), width, &s_env);
    s_env.cascade = NULL;
    if (fresh == NULL) {
        if (cascade != p->cascade)
            garb_cascade_free(cascade);
        return false;
    }
    flow_free(p->tree);
    if (cascade != p->cascade) {
        garb_cascade_free(p->cascade);
        p->cascade = cascade;
        os64_memcpy(p->cascade_entry, entry, sizeof(entry));
    }
    p->tree = fresh;
    // A box keeps where it was scrolled to, as near as the new layout lets
    // it: the range may have shrunk, and the element may make no scroll
    // container now.
    for (int32_t i = 0; i < p->nbox_scrolls; i++) {
        int32_t s = flow_scroller_for(fresh, p->box_scrolls[i].node);
        if (s >= 0)
            flow_scroll_set(fresh, s, p->box_scrolls[i].at);
    }
    p->laid_width = width;
    p->laid_height = height;
    p->sheets_changed = false;
    return true;
}

// Remembers where a box was scrolled to, for the layouts after this one.
// Out of memory, it is forgotten: the next layout starts the box at the
// top, which is what it did before a person scrolled it.
static void page_keep_box_scroll(Page *p, const os64_html_node_t *node, flow_point_t at)
{
    for (int32_t i = 0; i < p->nbox_scrolls; i++)
        if (p->box_scrolls[i].node == node) {
            p->box_scrolls[i].at = at;
            return;
        }
    if (p->nbox_scrolls == p->cap_box_scrolls) {
        int32_t cap = p->cap_box_scrolls != 0 ? p->cap_box_scrolls * 2 : 8;
        BoxScroll *grown = os64_realloc(p->box_scrolls, (size_t)cap * sizeof(*grown));
        if (grown == NULL)
            return;
        p->box_scrolls = grown;
        p->cap_box_scrolls = cap;
    }
    p->box_scrolls[p->nbox_scrolls++] = (BoxScroll){node, at};
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

// A control's widget (YONDER.md § Y4). The storage never moves once the
// widget is in the window's tree, so a page's are allocated together.
typedef enum { FW_TEXT, FW_PASSWORD, FW_CHECK, FW_BUTTON, FW_LIST, FW_FILE } FormKind;

typedef struct {
    FormKind kind;
    int32_t control;                // libpage's index
    union {
        os64_ui_textfield_t field;
        os64_ui_checkbox_t check;
        os64_ui_widget_t button;
        os64_ui_listbox_t list;
    } u;
    os64_ui_widget_t *w;
    char text[512];                 // a field's buffer, a button's caption
    char secret[512];               // a password's value: the field shows bullets
    size_t secret_len;
} FormWidget;

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
    // The browser's cache (CACHE.md), shared by every picture and sheet
    // job; NULL keeps nothing.
    way_cache_t *cache;
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
    uint64_t pages_made;            // the last Page.serial handed out
    // A page that arrived and is waiting for its linked sheets, with what
    // arriving needs; the page on screen stays until it is shown (GARB.md,
    // G4). `due` is when it is shown whatever is still coming.
    struct {
        bool active;
        Page page;
        NavKind kind;
        way_position_t crumb;
        bool has_fragment;
        char fragment[256];
        uint64_t due;
    } coming;
    // A picture arrived whose size may move the page, since the page was
    // last laid out (pictures_settle); cleared by any layout, whether or not
    // it fit.
    bool pictures_moved;
    // The window's clock, for moving pictures and a slow page's owed
    // layout; NULL, and nothing moves and a layout waits for an event.
    yonder_ticker_t *ticker;
    bool covered;                   // nobody can see the window: nothing moves
    // When a slow page's deferred layout is owed (pictures_settle), in
    // yonder_now_ms's milliseconds; YONDER_NEVER for none.
    uint64_t settle_due;
    // The last layout: how long it took, for the status line and for
    // pictures_settle, and when it finished.
    uint64_t laid_ms;
    os64_ticks_t laid_at;
    // The page's control widgets, after every permanent widget in the
    // window's tree; `by_control` finds one from libpage's index.
    FormWidget *fw;
    int32_t nfw, *by_control, ncontrols;
} g;   // zeroed: the session's histories are large, and .data would carry them in the file

// A select's list shows this many rows when the page does not say: enough
// to pick with the pointer, since yonder has no drop-down (YONDER.md § Y4).
#define SELECT_ROWS 4

// Keeps a size summed in 64 bits inside a widget's int32 rect.
static int32_t clamp32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

// A select's `size`, by HTML's rules for a non-negative integer (leading
// white space, an optional `+`, digits), saturating at INT32_MAX: the page
// writes as many digits as it likes. Anything else is 0, the default.
static int64_t size_rows(const char *v)
{
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\f' || *v == '\r')
        v++;
    if (*v == '+')
        v++;
    int64_t n = 0;
    for (; *v >= '0' && *v <= '9'; v++)
        n = n > (INT32_MAX - 9) / 10 ? INT32_MAX : n * 10 + (*v - '0');
    return n;
}

// A select's box: the rows its list shows, as wide as its longest option.
// Measured in the window's own face, the one its widget paints in. The
// page chooses `size` (at most INT32_MAX rows, by size_rows), so the
// arithmetic is 64-bit and the answer clamped.
static void select_size(const os64_page_control_t *c, const os64_html_node_t *node,
                        int32_t *w, int32_t *h)
{
    const os64_html_attr_t *size = os64_html_attr(node, "size");
    int64_t rows = size != NULL && size->value != NULL ? size_rows(size->value) : 0;
    if (rows <= 1)
        rows = c->noptions < SELECT_ROWS ? c->noptions : SELECT_ROWS;
    if (rows < 1)
        rows = 1;
    int32_t widest = 0;
    for (int32_t k = 0; k < c->noptions; k++) {
        int32_t px = 0;
        const char *label = c->options[k].label != NULL ? c->options[k].label : "";
        if (os64_ui_text_measure(&g.ui, OS64_FONT_ROLE_UI, label, os64_strlen(label), &px) ==
                OS64_FONT_OK && px > widest)
            widest = px;
    }
    // The listbox's own arithmetic: a two-pixel frame, rows eight pixels
    // taller than the face's, labels six pixels in.
    *h = clamp32(rows * ((int64_t)os64_ui_font_row_height(&g.ui, OS64_FONT_ROLE_UI) + 8) + 4);
    *w = clamp32((int64_t)widest + 2 * 6 + 4);
    if (*w < 48)
        *w = 48;
}

// The size libflow asks of a replaced box. A picture: the one that
// arrived, or unknown. A control yonder draws as a widget: the size that
// widget needs, since libflow's own guess is a text field's for every
// control, and a tick drawn across ten ems is a slab while a list one line
// high shows no rows at all. `ctx` is the page being laid out — the one on
// screen, or one being built beside it — so nothing is measured from the
// wrong page.
static bool replaced_size(void *ctx, const os64_html_node_t *node, int32_t *w, int32_t *h)
{
    const Page *p = ctx;
    const os64_page_t *model = page_model(p);
    int32_t control = model != NULL ? os64_page_control_for(model, node) : -1;
    const os64_page_control_t *c = control >= 0 ? os64_page_control(model, control) : NULL;
    if (c != NULL && (c->input == OS64_PAGE_INPUT_CHECKBOX || c->input == OS64_PAGE_INPUT_RADIO) &&
        g.ui.theme.checkbox_size > 0) {
        *w = *h = g.ui.theme.checkbox_size;
        return true;
    }
    if (c != NULL && c->element == OS64_PAGE_EL_SELECT) {
        select_size(c, node, w, h);
        return true;
    }
    int32_t i = p->pic_of != NULL ? os64_page_image_for(model, node) : -1;
    if (i < 0 || p->pic_of[i] < 0 || p->pics[p->pic_of[i]].state != PIC_SHOWN)
        return false;
    uint32_t pw, ph;
    (void)picture_pixels(&p->pics[p->pic_of[i]], &pw, &ph);
    *w = (int32_t)pw;
    *h = (int32_t)ph;
    return true;
}

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

// Where the page is scrolled to, as libflow asks for it: what a box in a
// fixed subtree is moved by to be where it is on the page now.
static flow_point_t scroll_now(void)
{
    return (flow_point_t){g.sx, g.sy};
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
static void forms_place(void);

static void scroll_to(int32_t x, int32_t y)
{
    g.sx = x;
    g.sy = y;
    clamp_scroll();
    sync_bars();
    os64_ui_mark_dirty(&g.ui, &g.view);
    forms_place();
    pictures_schedule();
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

// A box's padding box, from where its border box `r` is.
static os64_gui_rect_t padding_of(const flow_box_t *b, os64_gui_rect_t r)
{
    const flow_unit_t *bw = b->style->border_width;
    int32_t l = (bw[FLOW_LEFT] + 32) / 64, t = (bw[FLOW_TOP] + 32) / 64;
    return (os64_gui_rect_t){r.x + l, r.y + t, r.w - l - (bw[FLOW_RIGHT] + 32) / 64,
                             r.h - t - (bw[FLOW_BOTTOM] + 32) / 64};
}

// A target is revealed in every box it is scrolled inside
// (flow_scroll_reveal: its top at each box's top, and across as little as
// shows it) — `hidden` boxes too, since what a person cannot scroll a link
// may — and then in the page by the same rule, as Chrome and Firefox do.
static void scroll_to_node(const os64_html_node_t *node)
{
    flow_tree_t *t = g.page.tree;
    const flow_box_t *b = node != NULL && t != NULL ? flow_box_for(t, node) : NULL;
    if (node != NULL && b == NULL) {
        status_rest("that target has no box on this page");
        return;
    }
    if (b != NULL) {
        flow_scroll_reveal(t, b);
        for (int32_t i = flow_box_scroller(b); i >= 0; i = flow_box_scroller(flow_scroller(t, i)))
            page_keep_box_scroll(&g.page, flow_scroller(t, i)->node, flow_scroll_at(t, i));
    }
    // A target in a fixed box is on the glass wherever the page is: no
    // scroll of the page brings it nearer, so none is made (as Chrome
    // does), though the boxes it is in may have moved. Any other is where
    // its boxes' scrolls put it on the page unscrolled, so a sticky one is
    // where the flow put it, not where the scroll now pushes it.
    if (b != NULL && b->fixed) {
        scroll_to(g.sx, g.sy);
        return;
    }
    os64_gui_rect_t r = b != NULL ? flow_box_doc_rect(b, (flow_point_t){0, 0})
                                  : (os64_gui_rect_t){0, 0, 0, 0};
    int32_t x = g.sx;
    if (r.x < g.sx || r.w > g.view.bounds.w)
        x = r.x;
    else if ((int64_t)r.x + r.w > (int64_t)g.sx + g.view.bounds.w)
        x = r.x + r.w - g.view.bounds.w;
    scroll_to(x, r.y);
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
    // Positioning off is a mode that outlives the page it was turned off
    // on, so it is said on every page while it is in force: a page laid
    // out in document order must never pass for the page as designed. So
    // is an agent other than yonder's, or a page answering an assumed
    // identity could pass for one answering yonder.
    char asking[80] = "";
    if (!os64_streq(g.way.agent, YONDER_AGENT)) {
        const char *name = yonder_agent_name(g.way.agent);
        os64_snprintf(asking, sizeof(asking), " - asking as %s", name != NULL ? name : "a typed agent");
    }
    os64_snprintf(line, sizeof(line), "%s%s%s - laid out at %d px in %lu ms%s%s%s%s",
                  g.page.way.url, g.page.way.note[0] ? " - " : "", g.page.way.note,
                  g.page.laid_width, (unsigned long)ms, pictures, asking,
                  s_env.static_only ? " - POSITIONING OFF (p)" : "",
                  flow_incomplete(g.page.tree) ? " - INCOMPLETE: the layout stopped partway" : "");
    status_rest(line);
}

// An agent applied from the Settings window: every fetch from now on sends
// it, and the page's standing line says so.
static void agent_use(const char *agent)
{
    g.way.agent = agent;
    if (g.page.tree != NULL)
        say_laid_out(g.laid_ms);
}

// The node at the top of the view, and how far below the view's top its box
// sits, so a new layout can put that node back where it was: a node in the
// flow, never an overlay drawn over it, which a new layout can put anywhere.
static const os64_html_node_t *anchor_of(int32_t *offset)
{
    if (g.page.tree == NULL)
        return NULL;
    const flow_box_t *b = flow_hit_in_flow(g.page.tree, g.sx + 1, g.sy + 1);
    while (b != NULL && b->node == NULL)
        b = b->parent;
    if (b == NULL)
        return NULL;
    *offset = b->rect.y - g.sy;
    return b->node;
}

static void css_pictures(Page *p);

// Lays the page on screen out again at the view's size. `again` lays it
// out even at the size it has: a picture's size arrived. Without sheets
// or positioned boxes only the width moves anything; with sheets the
// height does too, which is what their media queries and vh units read,
// and with a positioned box, whose initial containing block is the view.
static void relayout(bool again)
{
    int32_t width = g.view.bounds.w, height = g.view.bounds.h;
    bool height_matters = g.page.sheets_ready != 0 || flow_npositioned(g.page.tree) > 0;
    if (page_doc(&g.page) == NULL || width <= 0 ||
        (!again && g.page.tree != NULL && width == g.page.laid_width &&
         (!height_matters || height == g.page.laid_height)))
        return;
    int32_t offset = 0;
    const os64_html_node_t *anchor = anchor_of(&offset);
    os64_ticks_t t0, t1;
    os64_ticks(&t0);
    bool laid = page_lay_out(&g.page, width, height);
    os64_ticks(&t1);
    // The attempt consumes the moves whether or not it fits: kept, they
    // would retry a layout that just failed at every event batch.
    g.pictures_moved = false;
    g.settle_due = YONDER_NEVER;       // the deadline goes with the moves it was for
    if (!laid) {
        status_rest("Out of memory laying the page out; this is the last layout that fit.");
        return;
    }
    g.laid_at = t1;
    css_pictures(&g.page);
    if (anchor != NULL) {
        const flow_box_t *b = flow_box_for(g.page.tree, anchor);
        if (b != NULL)
            g.sy = b->rect.y - offset;
    }
    clamp_scroll();
    sync_bars();
    forms_place();
    pictures_schedule();
    say_laid_out(ms_between(&t0, &t1));
    os64_ui_mark_dirty(&g.ui, &g.view);
}

static void request_navigate(os64_page_request_t *request, NavKind kind, way_ask_t ask);
static void buttons_follow(void);
static void pictures_start(Page *p);
static void pictures_leave(Page *p);
static void sheets_start(Page *p);
static void sheets_leave(Page *p);
static void coming_drop(void);
static void forms_build(void);
static void forms_drop(void);
static void bar_forget(void);
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

// A page is shown: laid out BESIDE the one on screen and swapped in only
// once the layout exists, so a page this machine cannot lay out costs a
// sentence and not the page the person was reading.
static void arrive_now(Page *fresh, NavKind kind, const way_position_t *crumb,
                       const char *fragment)
{
    int32_t width = g.view.bounds.w > 0 ? g.view.bounds.w : 1;
    int32_t height = g.view.bounds.h > 0 ? g.view.bounds.h : 1;
    os64_ticks_t t0, t1;
    os64_ticks(&t0);
    bool laid = page_lay_out(fresh, width, height);
    os64_ticks(&t1);
    if (!laid) {
        sheets_leave(fresh);
        page_clear(fresh);
        status_rest("Out of memory laying that page out; this is still the page you were on.");
        return;
    }
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
    // The page on screen is being replaced, and its question and its
    // pictures in flight with it: a page that fails to lay out (above)
    // leaves them all where they were.
    bar_forget();
    pictures_leave(&g.page);
    sheets_leave(&g.page);
    forms_drop();
    page_clear(&g.page);
    g.page = *fresh;
    os64_memset(fresh, 0, sizeof(*fresh));
    g.page_serial++;
    g.pictures_moved = false;
    g.settle_due = YONDER_NEVER;       // the deadline goes with the moves it was for
    g.laid_at = t1;
    pictures_start(&g.page);
    css_pictures(&g.page);
    forms_build();
    g.hover_link = g.pressed_link = -1;
    if (kind == NAV_BACK || kind == NAV_FORWARD)
        position_restore(crumb);
    else if (kind == NAV_RELOAD)
        clamp_scroll();
    else
        g.sx = g.sy = 0;
    sync_bars();
    forms_place();
    pictures_schedule();
    os64_ui_textfield_set(&g.ui, &g.field, g.page.way.url);
    say_laid_out(ms_between(&t0, &t1));
    if (fragment != NULL)
        scroll_to_fragment(fragment);
    refresh_if_declared();
    buttons_follow();
    os64_ui_mark_dirty(&g.ui, &g.view);
}

// A page arrived. Its linked sheets are sent for first, and the page on
// screen stays while they come — for up to SHEETS_WAIT_MS — so the new one
// is not drawn once bare and then again dressed; a sheet still coming after
// that is laid in when it arrives (GARB.md, G4).
static void arrive(Page *fresh, NavKind kind, const way_position_t *crumb, const char *fragment)
{
    coming_drop();
    fresh->serial = ++g.pages_made;
    sheets_start(fresh);
    if (fresh->sheets_waiting == 0) {
        arrive_now(fresh, kind, crumb, fragment);
        return;
    }
    g.coming.active = true;
    g.coming.page = *fresh;
    os64_memset(fresh, 0, sizeof(*fresh));
    g.coming.kind = kind;
    if (crumb != NULL)
        g.coming.crumb = *crumb;
    g.coming.has_fragment = fragment != NULL;
    if (fragment != NULL)
        os64_strcopy(g.coming.fragment, sizeof(g.coming.fragment), fragment);
    g.coming.due = yonder_now_ms() + SHEETS_WAIT_MS;
    char line[160];
    os64_snprintf(line, sizeof(line), "Fetching %d style sheet%s...",
                  (int)g.coming.page.sheets_waiting, g.coming.page.sheets_waiting == 1 ? "" : "s");
    status_rest(line);
    buttons_follow();
    pictures_schedule();
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
    fresh.way.model = fresh.way.doc != NULL ? os64_page_build(fresh.way.doc, fresh.way.url, NULL, NULL)
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
    bool loading = g.nav.id != 0 || g.coming.active;
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
// page is being left), a request waiting on this thread is dropped. Its
// number comes from whoever asks — the worker's mailbox, or this thread's
// own count for a request — and `asker` says which, so the two counts never
// have to agree.
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

// Ends the navigation in flight, if there is one, and lets go of a page
// that arrived and is waiting for its sheets: somewhere else was asked
// for. The pool's own cancellation reaches its fetch and any question it
// is waiting on; the mailbox this window held is let go, and outlives it
// for as long as the job still holds its own.
static void stop_trip(void)
{
    coming_drop();
    if (g.nav.id == 0) {
        buttons_follow();
        return;
    }
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
    // A QUESTION IS ABOUT THE PAGE ON SCREEN, and going anywhere leaves it:
    // whoever asked, it goes (a form's request with it) before this goes.
    bar_forget();
    if (os64_strlen(url) >= 7 && os64_memcmp(url, "file://", 7) == 0) {
        // `url` may be the request's own: the path is taken before it goes.
        char path[OS64_FETCH_URL_MAX];
        os64_strcopy(path, sizeof(path), file_path(url + 7));
        os64_page_request_free(request);
        stop_trip();
        open_local(path, kind, crumb);
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
    trip->agent = g.way.agent;
    trip->window = g.win;
    trip->mail_bell = BELL_MAIL;
    os64_strcopy(trip->url, sizeof(trip->url), url);
    // What the page asked for names the page in its Referer; a Reload,
    // even one sending a form again, names nothing.
    if (request != NULL && kind != NAV_RELOAD && g.page.tree != NULL)
        os64_strcopy(trip->referrer, sizeof(trip->referrer), g.page.way.url);
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

// A copy of a request, owning its own storage as libpage's do, so that
// os64_page_request_free takes it apart. False when memory ran out.
static bool request_copy(const os64_page_request_t *from, os64_page_request_t *to)
{
    *to = *from;
    to->url = to->fragment = to->content_type = NULL;
    to->body = NULL;
    to->has_fragment = false;
    to->anchor = NULL;
    size_t n = os64_strlen(from->url) + 1;
    char *url = os64_malloc(n);
    char *type = from->content_type != NULL ? os64_malloc(os64_strlen(from->content_type) + 1)
                                            : NULL;
    void *body = from->body_len > 0 ? os64_malloc(from->body_len) : NULL;
    if (url == NULL || (from->content_type != NULL && type == NULL) ||
        (from->body_len > 0 && body == NULL)) {
        os64_free(url);
        os64_free(type);
        os64_free(body);
        to->body_len = 0;
        return false;
    }
    os64_memcpy(url, from->url, n);
    if (type != NULL)
        os64_strcopy(type, os64_strlen(from->content_type) + 1, from->content_type);
    if (body != NULL)
        os64_memcpy(body, from->body, from->body_len);
    to->url = url;
    to->content_type = type;
    to->body = body;
    return true;
}

// Reload on the reply to a form: the form sent again, which is asked
// first, since whatever it did — an order, a post — it does twice. When
// the form goes out in the clear, libway's own question is the one put:
// it says that, and a yes to it is a yes to sending.
static void resend_form(void)
{
    os64_page_request_t again;
    if (!request_copy(&g.page.sent, &again)) {
        status_rest("Out of memory sending that form again.");
        return;
    }
    way_judgement_t j = way_judge(&g.way, &again, WAY_ASK_SEND);
    if (j.kind == WAY_REFUSE) {
        os64_page_request_free(&again);
        say_way();
        return;
    }
    bar_ask(ASK_REQUEST, ++g.asked,
            j.kind == WAY_QUESTION ? j.question
                                   : "This page is the reply to a form. Send the form again?");
    g.pending = again;
    g.pending_kind = NAV_RELOAD;
    g.pending_refused = j.kind == WAY_QUESTION ? j.refused : "The form was not sent again.";
}

// An address a person gave: a path is a file, anything else libway's.
static void open_address(const char *typed, NavKind kind, const way_position_t *crumb,
                         os64_page_request_t *request)
{
    if (typed[0] == '/') {
        bar_forget();           // the page on screen is being left (start_trip)
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

static char *copy_text(const char *text)
{
    size_t n = os64_strlen(text) + 1;
    char *out = os64_malloc(n);
    if (out != NULL)
        os64_memcpy(out, text, n);
    return out;
}

// Whether an arrival can move the page: libflow's rule, asked of the box
// the element was laid out in. A picture with no box has nothing fixed.
static bool picture_sized(const Page *p, const os64_html_node_t *node)
{
    const flow_box_t *b = flow_box_for(p->tree, node);
    return b != NULL && flow_replaced_fixed(b->style);
}

// Hands the pool the page's next pictures, in table order, while fewer than
// PICTURES_AT_ONCE are in it. One it cannot take keeps its frame.
static void pictures_feed(Page *p)
{
    while (p->in_flight < PICTURES_AT_ONCE && p->next_pic < p->npics) {
        int32_t index = p->next_pic++;
        Picture *pic = &p->pics[index];
        if (pic->state != PIC_QUEUED)
            continue;
        pic->state = PIC_FAILED;
        p->waiting--;
        yonder_picture_job_t *job = g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
        if (job == NULL)
            continue;
        job->kind = YONDER_JOB_PICTURE;
        job->index = index;
        job->generation = g.page_serial;
        job->agent = g.way.agent;
        os64_strcopy(job->url, sizeof(job->url), pic->url);
        job->hooks.jar = g.way.jar;
        job->hooks.cache = g.cache;
        os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), p->way.url);
        os64_work_t work = {yonder_picture_run, yonder_picture_release, job, PICTURE_RESERVE};
        pic->id = os64_work_submit(g.pool, &work);
        if (pic->id == 0) {
            os64_free(job);
            continue;
        }
        pic->state = PIC_WAITING;
        p->waiting++;
        p->in_flight++;
    }
}

static size_t url_hash(const char *url)
{
    uint64_t h = 1469598103934665603ull;       // FNV-1a over the address
    for (const char *c = url; *c != '\0'; c++)
        h = (h ^ (uint8_t)*c) * 1099511628211ull;
    return (size_t)h;
}

// The table's index slot for `url`: where it is, or the empty slot where it
// would go. The index is never full (picture_for keeps it half empty).
static size_t picture_slot(const Page *p, const char *url)
{
    size_t at = url_hash(url) & (p->pic_slot_cap - 1);
    while (p->pic_slot[at] >= 0 && !os64_streq(p->pics[p->pic_slot[at]].url, url))
        at = (at + 1) & (p->pic_slot_cap - 1);
    return at;
}

// The picture at `url` in the page's table, or -1: found through the index,
// never by a walk of the table — a page names every picture it likes, and a
// walk per picture is quadratic in them.
static int32_t picture_find(const Page *p, const char *url)
{
    if (p->pic_slot == NULL || url == NULL)
        return -1;
    return p->pic_slot[picture_slot(p, url)];
}

// The picture at `url`, gathered once however many times the page or its
// sheets name it: its index in the page's table, QUEUED for pictures_feed
// when it is new, or -1 for an address nothing here fetches (or no memory).
// `own` keeps a copy of the address, for one no page model holds (a
// sheet's). The index grows with the table, so a sheet's pictures found
// after a layout join the same one.
static int32_t picture_for(Page *p, const char *url, bool own)
{
    if (url == NULL)
        return -1;
    bool fetchable = os64_strlen(url) > 7 &&
                     (os64_memcmp(url, "http://", 7) == 0 ||
                      (os64_strlen(url) > 8 && os64_memcmp(url, "https://", 8) == 0) ||
                      os64_memcmp(url, "file://", 7) == 0);
    if (!fetchable)
        return -1;
    int32_t found = picture_find(p, url);
    if (found >= 0)
        return found;
    if ((size_t)(p->npics + 1) * 2 > p->pic_slot_cap) {
        size_t cap = 16;
        while (cap < (size_t)(p->npics + 1) * 2)
            cap *= 2;
        int32_t *slot = os64_malloc(cap * sizeof(*slot));
        if (slot == NULL)
            return -1;
        for (size_t k = 0; k < cap; k++)
            slot[k] = -1;
        os64_free(p->pic_slot);
        p->pic_slot = slot;
        p->pic_slot_cap = cap;
        for (int32_t k = 0; k < p->npics; k++)
            p->pic_slot[picture_slot(p, p->pics[k].url)] = k;
    }
    if (p->npics == p->cap_pics) {
        int32_t cap = p->cap_pics > 0 ? p->cap_pics * 2 : 8;
        Picture *grown = os64_realloc(p->pics, (size_t)cap * sizeof(*grown));
        if (grown == NULL)
            return -1;
        os64_memset(grown + p->cap_pics, 0, (size_t)(cap - p->cap_pics) * sizeof(*grown));
        p->pics = grown;
        p->cap_pics = cap;
    }
    const char *kept = own ? copy_text(url) : url;
    if (kept == NULL)
        return -1;
    p->pic_slot[picture_slot(p, kept)] = p->npics;
    Picture *pic = &p->pics[p->npics];
    pic->url = kept;
    pic->owned = own;
    pic->state = PIC_QUEUED;
    p->waiting++;
    return p->npics++;
}

// ── The page's sheets ───────────────────────────────────────────────────

// Whether two addresses share an origin: scheme, host and port.
static bool same_origin(const char *a, const char *b)
{
    os64_url_t ua, ub;
    return os64_url_parse(a, &ua) == OS64_URL_OK && os64_url_parse(b, &ub) == OS64_URL_OK &&
           os64_streq(ua.scheme, ub.scheme) && os64_streq(ua.host, ub.host) &&
           ua.port == ub.port;
}

// A new entry, NULL when the page has as many as it may.
static Sheet *sheet_new(Page *p, const char *media, int32_t importer, int32_t import_index)
{
    if (p->sheets == NULL)
        p->sheets = os64_calloc(SHEETS_MAX, sizeof(Sheet));
    if (p->sheets == NULL || p->nsheets == SHEETS_MAX)
        return NULL;
    Sheet *sh = &p->sheets[p->nsheets++];
    sh->media = media;
    sh->importer = importer;
    sh->import_index = import_index;
    // Whether the first paint waits for it: its media holds on this glass
    // now — an import's own media list as well as its importer's. An
    // import's supports() is the cascade's to judge and is not asked here,
    // so one that fails is waited for all the same; rare, and never wrong
    // about what applies.
    garb_env_t view = {g.view.bounds.w > 0 ? g.view.bounds.w : 1,
                       g.view.bounds.h > 0 ? g.view.bounds.h : 1};
    if (importer >= 0) {
        const garb_import_t *im = &p->sheets[importer].imports[import_index];
        sh->holds = p->sheets[importer].holds && garb_media_matches(im->media, im->nmedia, view);
    } else {
        sh->holds = media == NULL || garb_media_text_matches(media, view);
    }
    return sh;
}

// Sends a sheet's address to a worker. One that cannot be fetched stays
// out of the cascade, and the page is drawn without it.
static void sheet_fetch(Page *p, Sheet *sh, const char *url)
{
    sh->url = copy_text(url);
    bool fetchable = sh->url != NULL &&
                     ((os64_strlen(url) > 7 && os64_memcmp(url, "http://", 7) == 0) ||
                      (os64_strlen(url) > 8 && os64_memcmp(url, "https://", 8) == 0) ||
                      (os64_strlen(url) > 7 && os64_memcmp(url, "file://", 7) == 0));
    yonder_sheet_job_t *job = fetchable && g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
    if (job == NULL)
        return;
    const os64_html_document_t *doc = page_doc(p);
    job->kind = YONDER_JOB_SHEET;
    job->page = p->serial;
    job->index = (int32_t)(sh - p->sheets);
    job->agent = g.way.agent;
    os64_strcopy(job->url, sizeof(job->url), url);
    if (doc->charset != NULL)
        os64_strcopy(job->environment, sizeof(job->environment), doc->charset);
    // Judged on the address asked for; HTML reads the response's, so a
    // same-origin sheet that redirects elsewhere is read whatever its type.
    job->any_type = doc->quirks == OS64_HTML_QUIRKS && same_origin(url, p->way.url);
    job->hooks.jar = g.way.jar;
    job->hooks.cache = g.cache;
    os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), p->way.url);
    os64_work_t work = {yonder_sheet_run, yonder_sheet_release, job, SHEET_RESERVE};
    sh->id = os64_work_submit(g.pool, &work);
    if (sh->id == 0) {
        os64_free(job);         // the table is full: the page goes without it
        return;
    }
    sh->waiting = true;
    if (sh->holds)
        p->sheets_waiting++;
}

// A sheet is ready: it joins the cascade, and its @imports are sent for,
// each resolved against the sheet's own address — a `style` element's
// against the page's base — and none that is already on the chain of
// sheets importing it, which would import itself for ever.
static void sheet_ready(Page *p, int32_t e)
{
    Sheet *sh = &p->sheets[e];
    sh->ready = true;
    p->sheets_ready++;
    p->sheets_changed = true;
    int32_t n = garb_sheet_imports(&sh->parsed, NULL, 0);
    if (n > SHEET_IMPORTS_MAX)
        n = SHEET_IMPORTS_MAX;
    if (n == 0)
        return;
    sh->imports = os64_calloc((size_t)n, sizeof(*sh->imports));
    sh->child = os64_calloc((size_t)n, sizeof(*sh->child));
    if (sh->imports == NULL || sh->child == NULL)
        return;
    sh->nimports = garb_sheet_imports(&sh->parsed, sh->imports, n);
    if (sh->nimports > n)
        sh->nimports = n;
    const char *base = sh->url != NULL ? sh->url : os64_page_base(page_model(p));
    for (int32_t k = 0; k < sh->nimports; k++) {
        sh->child[k] = -1;
        char written[OS64_FETCH_URL_MAX], url[OS64_FETCH_URL_MAX];
        const garb_import_t *im = &sh->imports[k];
        if (im->len == 0 || im->len >= sizeof(written))
            continue;
        os64_memcpy(written, im->url, im->len);
        written[im->len] = '\0';
        if (!os64_page_url_absolute(base, written, url, sizeof(url)))
            continue;
        bool looping = false;
        for (int32_t up = e; up >= 0 && !looping; up = p->sheets[up].importer)
            looping = p->sheets[up].url != NULL && os64_streq(p->sheets[up].url, url);
        Sheet *child = looping ? NULL : sheet_new(p, NULL, e, k);
        if (child == NULL)
            continue;
        sh->child[k] = (int32_t)(child - p->sheets);
        sheet_fetch(p, child, url);
    }
}

// Every sheet libpage lists, in document order: a `style` element's parsed
// here, a linked one sent for. A sheet that will not parse — too big, or no
// memory — is left out, and the page is drawn without it.
static void sheets_start(Page *p)
{
    const os64_page_t *model = page_model(p);
    int32_t n = model != NULL ? os64_page_nsheets(model) : 0;
    for (int32_t i = 0; i < n; i++) {
        const os64_page_sheet_t *one = os64_page_sheet(model, i);
        if (one->linked && one->href.url == NULL)
            continue;
        Sheet *sh = sheet_new(p, one->media, -1, -1);
        if (sh == NULL)
            break;
        if (one->linked) {
            sheet_fetch(p, sh, one->href.url);
        } else if (garb_parse_style_element(one->node, &sh->parsed) == GARB_OK) {
            sheet_ready(p, (int32_t)(sh - p->sheets));
        } else {
            garb_free(&sh->parsed);
        }
    }
}

// The page is going: what it is still waiting for is not wanted.
static void sheets_leave(Page *p)
{
    for (int32_t e = 0; e < p->nsheets; e++)
        if (p->sheets[e].waiting && g.pool != NULL)
            os64_work_cancel(g.pool, p->sheets[e].id);
}

// The page waiting for its sheets is shown, with what has come.
static void coming_show(void)
{
    if (!g.coming.active)
        return;
    Page page = g.coming.page;
    NavKind kind = g.coming.kind;
    way_position_t crumb = g.coming.crumb;
    char fragment[sizeof(g.coming.fragment)];
    bool has_fragment = g.coming.has_fragment;
    os64_strcopy(fragment, sizeof(fragment), g.coming.fragment);
    os64_memset(&g.coming, 0, sizeof(g.coming));
    arrive_now(&page, kind, &crumb, has_fragment ? fragment : NULL);
}

// The page waiting for its sheets is not wanted after all.
static void coming_drop(void)
{
    if (!g.coming.active)
        return;
    sheets_leave(&g.coming.page);
    page_clear(&g.coming.page);
    os64_memset(&g.coming, 0, sizeof(g.coming));
}

// A sheet's job finished. It belongs to the page waiting for its sheets —
// which is shown once the last is in — or to the page on screen, which is
// laid out again with it; any other page's is let go.
static void sheet_arrived(const yonder_sheet_job_t *job, yonder_sheet_t *got)
{
    Page *p = g.coming.active && g.coming.page.serial == job->page ? &g.coming.page
            : g.page.serial == job->page ? &g.page : NULL;
    if (p == NULL || job->index < 0 || job->index >= p->nsheets)
        return;
    Sheet *sh = &p->sheets[job->index];
    if (!sh->waiting)
        return;
    sh->waiting = false;
    sh->id = 0;
    if (sh->holds)
        p->sheets_waiting--;
    if (got != NULL && got->ok) {
        sh->parsed = got->parsed;               // moved: the release frees nothing
        os64_memset(&got->parsed, 0, sizeof(got->parsed));
        char *url = copy_text(got->url);        // after its redirects
        if (url != NULL) {
            os64_free(sh->url);
            sh->url = url;
        }
        sheet_ready(p, job->index);
    }
    if (p == &g.coming.page) {
        if (p->sheets_waiting == 0)
            coming_show();
    } else if (p->sheets_changed) {
        relayout(true);
    }
}

// ── Pictures a sheet names ──────────────────────────────────────────────

// A background layer's url() resolved, as the cascade that chose it saw
// it: against the sheet it was written in after that sheet's redirects, or
// the page's base for a `style` attribute or a `style` element.
static bool css_url(const Page *p, const flow_layer_t *l, char *out, size_t cap)
{
    char written[OS64_FETCH_URL_MAX];
    if (l->image == NULL || l->image_len >= sizeof(written))
        return false;
    os64_memcpy(written, l->image, l->image_len);
    written[l->image_len] = '\0';
    const char *base = os64_page_base(page_model(p));
    int32_t in = l->sheet;
    if (in >= 0) {
        if (in >= SHEETS_MAX || p->cascade == NULL)
            return false;
        const Sheet *sh = &p->sheets[p->cascade_entry[in]];
        if (sh->url != NULL)
            base = sh->url;
    }
    return os64_page_url_absolute(base, written, out, cap);
}

// The picture a background layer names, in the page's table, or -1.
// Asked for every layer with a picture on every paint: a copy, a resolve
// and one index probe, small beside drawing the box. The layout-time walk
// has the same answer; keeping it per box would mean a field on libflow's
// tree for yonder's table, and one to keep true across relayouts.
static int32_t css_picture(const Page *p, const flow_layer_t *l)
{
    char url[OS64_FETCH_URL_MAX];
    if (!css_url(p, l, url, sizeof(url)))
        return -1;
    return picture_find(p, url);
}

// Whether any of a style's layers shows picture `k`.
static bool css_shows(const Page *p, const flow_style_t *s, int32_t k)
{
    for (int32_t i = 0; i < flow_background_layers(s); i++) {
        flow_layer_t l = flow_background_layer(s, i);
        if (l.image != NULL && css_picture(p, &l) == k)
            return true;
    }
    return false;
}

// After a layout: every picture the laid-out boxes' sheets put behind them
// is sent for, once — a resize whose media query brings a new one fetches
// it then.
static void css_pictures_under(Page *p, const flow_box_t *b)
{
    for (; b != NULL; b = b->next) {
        for (int32_t i = 0; b->node != NULL && i < flow_background_layers(b->style); i++) {
            flow_layer_t l = flow_background_layer(b->style, i);
            char url[OS64_FETCH_URL_MAX];
            if (l.image != NULL && css_url(p, &l, url, sizeof(url)))
                (void)picture_for(p, url, true);
        }
        css_pictures_under(p, b->first);
    }
}

static void css_pictures(Page *p)
{
    if (p->tree != NULL && p->sheets_ready > 0 && g.pool != NULL) {
        css_pictures_under(p, flow_root(p->tree));
        pictures_feed(p);
    }
}

// Gathers the page's pictures and the pictures behind its boxes, one per
// address, in tree order — libpage's lists are the record — and starts
// feeding them to the pool. A picture whose address was refused, or names
// a scheme nothing here fetches, is never asked for.
static void pictures_start(Page *p)
{
    const os64_page_t *model = page_model(p);
    int32_t n = model != NULL ? os64_page_nimages(model) : 0;
    int32_t nb = model != NULL ? os64_page_nbackgrounds(model) : 0;
    if (n + nb <= 0 || g.pool == NULL)
        return;
    p->pic_of = os64_calloc((size_t)(n > 0 ? n : 1), sizeof(*p->pic_of));
    p->bg_of = os64_calloc((size_t)(nb > 0 ? nb : 1), sizeof(*p->bg_of));
    p->pics = os64_calloc((size_t)(n + nb), sizeof(*p->pics));
    p->cap_pics = p->pics != NULL ? n + nb : 0;
    if (p->pic_of == NULL || p->bg_of == NULL || p->pics == NULL) {
        os64_free(p->pic_of);
        os64_free(p->bg_of);
        os64_free(p->pics);
        p->pic_of = p->bg_of = NULL;
        p->pics = NULL;
        p->cap_pics = 0;
        return;
    }
    // A picture moves the page when any element naming it leaves its box
    // to the picture; a background never does, being painted, not laid out.
    for (int32_t i = 0; i < n; i++) {
        const os64_page_image_t *img = os64_page_image(model, i);
        int32_t k = picture_for(p, img->src.url, false);
        p->pic_of[i] = k;
        if (k >= 0 && !picture_sized(p, img->node))
            p->pics[k].moves = true;
    }
    for (int32_t i = 0; i < nb; i++)
        p->bg_of[i] = picture_for(p, os64_page_background(model, i)->src.url, false);
    pictures_feed(p);
}

// The page is being left: its pictures in the pool are cancelled, and the
// pool lets their inputs and products go; those not handed over are never
// asked for.
static void pictures_leave(Page *p)
{
    for (int32_t i = 0; i < p->npics; i++)
        if (p->pics[i].state == PIC_WAITING && g.pool != NULL)
            os64_work_cancel(g.pool, p->pics[i].id);
    p->waiting = p->in_flight = 0;
    p->next_pic = p->npics;
}

// Whether a picture's arrival can move the page (Picture.moves).
static bool picture_moves_page(const Page *p, int32_t pic)
{
    return p->pics[pic].moves;
}

static void picture_arrived(yonder_picture_job_t *job, yonder_picture_t *product)
{
    if (job->generation != g.page_serial || job->index < 0 || job->index >= g.page.npics)
        return;                     // a page no longer on screen
    Picture *pic = &g.page.pics[job->index];
    if (pic->state != PIC_WAITING)
        return;
    g.page.waiting--;
    g.page.in_flight--;
    if (product == NULL || product->status != OS64_IMAGE_OK) {
        pic->state = PIC_FAILED;
        return;
    }
    size_t bytes = product->cost;
    if (g.page.kept_bytes + bytes > PICTURES_KEPT_MAX) {
        pic->state = PIC_NOT_KEPT;
        g.page.not_kept++;
        return;
    }
    pic->image = product->image;
    os64_memset(&product->image, 0, sizeof(product->image));
    pic->moving = product->sequence;
    product->sequence = NULL;
    pic->state = PIC_SHOWN;
    g.page.kept_bytes += bytes;
    if (pic->moving != NULL) {
        pic->playing = true;
        pic->due = yonder_now_ms() + frame_delay(pic);
        pictures_schedule();
    }
    if (picture_moves_page(&g.page, job->index))
        g.pictures_moved = true;
    os64_ui_mark_dirty(&g.ui, &g.view);
}

// After a drained batch: lay the page out again if pictures moved it —
// once per batch when laying it out is cheap, so a slow page of forty
// pictures is not laid out forty times; otherwise when its last picture is
// in, or once the last layout is stale. A slow page that is waiting hands
// the ticker the moment it goes stale (pictures_schedule), so it settles
// on time with nobody at the mouse — or, with no ticker, at the first
// batch after that moment.
static void pictures_settle(void)
{
    uint64_t due = YONDER_NEVER;
    if (g.pictures_moved) {
        bool now_ok = true;
        if (g.page.waiting > 0 && g.laid_ms > PICTURES_CHEAP_LAYOUT_MS) {
            os64_ticks_t now;
            os64_ticks(&now);
            uint64_t since = ms_between(&g.laid_at, &now);
            uint64_t stale = 2 * g.laid_ms > PICTURES_STALE_MS ? 2 * g.laid_ms : PICTURES_STALE_MS;
            if (since < stale) {
                now_ok = false;
                due = yonder_now_ms() + (stale - since);
            }
        }
        if (now_ok)
            relayout(true);
    }
    if (due != g.settle_due) {
        g.settle_due = due;
        pictures_schedule();
    }
}

// ── Moving pictures ─────────────────────────────────────────────────────

// How long the frame on show stays: its own delay, or the floor for one
// that asks for none.
static uint64_t frame_delay(const Picture *pic)
{
    uint32_t ms = os64_image_sequence_frame(pic->moving)->delay_ms;
    return ms <= 10 ? FRAME_FLOOR_MS : ms;
}

// Marks a part of the view, in page coordinates, for repainting: libui
// marks a widget's bounds, so a stand-in with the part's is how.
static void mark_part(os64_gui_rect_t r)
{
    os64_ui_widget_t part = {0};
    part.bounds = (os64_gui_rect_t){g.view.bounds.x + r.x - g.sx, g.view.bounds.y + r.y - g.sy,
                                    r.w, r.h};
    os64_ui_mark_dirty(&g.ui, &part);
}

typedef struct {
    int32_t k;
    bool mark;
    os64_gui_rect_t view;       // page coordinates
    bool seen;
    const flow_box_t *owner;    // the canvas's: asked apart from the walk
} SheetPictureSeen;

// One box of the view's walk: does a sheet put picture `k` behind it?
static void sheet_picture_seen(void *ctx, const flow_box_t *b)
{
    SheetPictureSeen *look = ctx;
    if (b == look->owner || (look->seen && !look->mark) || !css_shows(&g.page, b->style, look->k))
        return;
    os64_gui_rect_t meet;
    if (!os64_rect_intersect(flow_box_doc_rect(b, scroll_now()), look->view, &meet))
        return;
    look->seen = true;
    if (look->mark)
        mark_part(meet);
}

static bool glass_backdrop(void *ctx, const flow_box_t *b, int32_t layer,
                           const os64_gui_rect_t *area, os64_gui_rect_t origin, int32_t ox,
                           int32_t oy, os64_gui_rect_t clip);

// A `background` attribute's picture behind a box, or -1.
static int32_t attribute_picture(const Page *p, const flow_box_t *b)
{
    int32_t i = b->node != NULL ? os64_page_background_for(page_model(p), b->node) : -1;
    return i >= 0 && p->bg_of != NULL ? p->bg_of[i] : -1;
}

// Whether picture `k` is behind a box as glass_backdrop draws it: in a
// sheet's layer, or the attribute's when no sheet gives the box a picture
// or a gradient.
static bool box_shows(const Page *p, const flow_box_t *b, int32_t k)
{
    if (flow_background_has_image(b->style))
        return css_shows(p, b->style, k);
    return attribute_picture(p, b) == k;
}

// Whether any box showing picture `k` meets the view — one scrolled away is
// not advanced, since decoding a frame nobody sees is only cost — and, when
// `mark`, each such box's part of the glass marked for repainting.
static bool picture_on_screen(int32_t k, bool mark)
{
    const Page *p = &g.page;
    const os64_page_t *model = page_model(p);
    if (p->tree == NULL || model == NULL)
        return false;
    os64_gui_rect_t view = {g.sx, g.sy, g.view.bounds.w, g.view.bounds.h}, meet;
    bool seen = false;
    for (int32_t i = 0; p->pic_of != NULL && i < flow_nimages(p->tree); i++) {
        const flow_box_t *b = flow_image(p->tree, i);
        int32_t at = os64_page_image_for(model, b->node);
        if (at < 0 || p->pic_of[at] != k ||
            !os64_rect_intersect(flow_box_doc_rect(b, scroll_now()), view, &meet))
            continue;
        seen = true;
        if (!mark)
            return true;
        mark_part(meet);
    }
    // The canvas's picture is behind the whole view wherever its owner's
    // box has gone — a body only 50px tall still paints the page — so its
    // owner is found by the painter's own rule, apart from any walk of what
    // the view shows (Quinn, #206).
    const yonder_verbs_t asking = {.backdrop = glass_backdrop};
    const flow_box_t *owner = yonder_canvas_owner(p->tree, &asking);
    if (owner != NULL && box_shows(p, owner, k)) {
        seen = true;
        if (!mark)
            return true;
        mark_part(view);
    }
    // Behind any other box: a `background` attribute's picture, and then a
    // sheet's behind any box the view shows (the walk visits only those).
    for (int32_t i = 0; p->bg_of != NULL && i < os64_page_nbackgrounds(model); i++) {
        if (p->bg_of[i] != k)
            continue;
        const flow_box_t *b = flow_box_for(p->tree, os64_page_background(model, i)->node);
        if (b == NULL || b == owner ||
            !os64_rect_intersect(flow_box_doc_rect(b, scroll_now()), view, &meet))
            continue;
        seen = true;
        if (!mark)
            return true;
        mark_part(meet);
    }
    SheetPictureSeen look = {k, mark, view, false, owner};
    flow_visit(p->tree, view, scroll_now(), sheet_picture_seen, &look);
    return seen || look.seen;
}

// Hands the ticker the earliest of three deadlines: the next frame among
// the pictures on screen and the moment a slow page's layout goes stale
// (pictures_settle) — neither for a covered window — and the moment a page
// waiting for its sheets is shown whatever is still coming, covered or
// not, since that page has to arrive either way.
static void pictures_schedule(void)
{
    uint64_t next = g.covered ? YONDER_NEVER : g.settle_due;
    if (g.coming.active && g.coming.due < next)
        next = g.coming.due;
    for (int32_t k = 0; !g.covered && k < g.page.npics; k++) {
        const Picture *pic = &g.page.pics[k];
        if (pic->playing && pic->due < next && picture_on_screen(k, false))
            next = pic->due;
    }
    yonder_ticker_set(g.ticker, next);
}

// The ticker rang: every picture that is due and on screen shows its next
// frame, and only its boxes are repainted.
static void pictures_tick(void)
{
    uint64_t now = yonder_now_ms();
    for (int32_t k = 0; !g.covered && k < g.page.npics; k++) {
        Picture *pic = &g.page.pics[k];
        if (!pic->playing || pic->due > now || !picture_on_screen(k, false))
            continue;
        // END holds the last frame and a failure the last good one: either
        // way the picture stays as it is, and stops.
        if (os64_image_sequence_next(pic->moving) != OS64_IMAGE_OK) {
            pic->playing = false;
            continue;
        }
        pic->due = now + frame_delay(pic);
        (void)picture_on_screen(k, true);
    }
    pictures_schedule();
}

// The window was covered or uncovered. The flag is the truth and the event
// only the nudge, since a full queue drops events.
static void window_seen(void)
{
    os64_gui_window_state_t st;
    if (os64_gui_window_get_state(g.win, &st) == 0)
        g.covered = (st.flags & OS64_GUI_WINDOW_COVERED) != 0;
    pictures_schedule();
}

// ── Forms ───────────────────────────────────────────────────────────────

static FormWidget *form_widget(int32_t control)
{
    return g.by_control != NULL && control >= 0 && control < g.ncontrols &&
                   g.by_control[control] >= 0
               ? &g.fw[g.by_control[control]]
               : NULL;
}

// The node after `n` in tree order, staying inside `root`.
static const os64_html_node_t *next_within(const os64_html_node_t *n, const os64_html_node_t *root)
{
    if (n->first_child != NULL)
        return n->first_child;
    while (n != root && n->next == NULL)
        n = n->parent;
    return n != root ? n->next : NULL;
}

// A button's caption: its value, or the element's own text for a `button` —
// all of it, however deep (`<button><span>Search</span></button>`) — or the
// name the standard gives a submit or reset with neither.
static void button_caption(const os64_page_control_t *c, char *out, size_t cap)
{
    out[0] = '\0';
    if (c->element == OS64_PAGE_EL_BUTTON) {
        size_t at = 0;
        bool space = false;
        for (const os64_html_node_t *n = next_within(c->node, c->node); n != NULL && at + 2 < cap;
             n = next_within(n, c->node))
            for (size_t i = 0; n->kind == OS64_HTML_TEXT && i < n->text_len && at + 2 < cap; i++) {
                char ch = n->text[i];
                if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
                    space = at > 0;
                    continue;
                }
                if (space)
                    out[at++] = ' ';
                space = false;
                out[at++] = ch;
            }
        out[at] = '\0';
    } else if (c->value_len > 0) {
        size_t n = c->value_len < cap - 1 ? c->value_len : cap - 1;
        os64_memcpy(out, c->value, n);
        out[n] = '\0';
    }
    if (out[0] == '\0')
        os64_strcopy(out, cap, c->input == OS64_PAGE_INPUT_RESET ? "Reset"
                               : c->input == OS64_PAGE_INPUT_FILE ? "Choose a file"
                               : c->submits ? "Submit" : "Button");
}

static const char *option_label(size_t i, void *user)
{
    const FormWidget *fw = user;
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), fw->control);
    return c != NULL && (int32_t)i < c->noptions && c->options[i].label != NULL
               ? c->options[i].label : "";
}

static void form_send(int32_t control, os64_page_activation_t how);

static void field_submitted(os64_ui_textfield_t *tf, void *user)
{
    (void)tf;
    form_send(((FormWidget *)user)->control, OS64_PAGE_ACTIVATE_IMPLICIT);
}

static void field_cancelled(os64_ui_textfield_t *tf, void *user)
{
    (void)tf;
    (void)user;
    os64_ui_set_focus(&g.ui, &g.view);
}

static void forms_sync_from_model(bool text);

// A tick: the model's, then every tick's again — a radio's group is
// libpage's, so picking one may have cleared others.
static void check_changed(os64_ui_checkbox_t *cb, void *user)
{
    const FormWidget *fw = user;
    if (os64_page_set_checked(page_model(&g.page), fw->control, cb->checked) < 0)
        status_rest("the page keeps that one as it is");
    forms_sync_from_model(false);
}

static void list_changed(os64_ui_listbox_t *list, void *user)
{
    const FormWidget *fw = user;
    if (list->selected >= 0)
        (void)os64_page_set_chosen(page_model(&g.page), fw->control, (int32_t)list->selected, true);
    forms_sync_from_model(false);
}

static void button_clicked(os64_ui_widget_t *w, void *user)
{
    (void)w;
    const FormWidget *fw = user;
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), fw->control);
    if (c != NULL && c->resets) {
        (void)os64_page_reset(page_model(&g.page), c->form);
        forms_sync_from_model(true);
        return;
    }
    form_send(fw->control, OS64_PAGE_ACTIVATE_CONTROL);
}

// A password field shows one bullet per character of the value yonder keeps.
static void password_show(FormWidget *fw)
{
    char bullets[sizeof(fw->text)];
    size_t n = 0;
    for (size_t i = 0; i < fw->secret_len && n + 1 < sizeof(bullets); i++)
        if (((unsigned char)fw->secret[i] & 0xc0) != 0x80)   // one per character
            bullets[n++] = '*';
    bullets[n] = '\0';
    os64_ui_textfield_set(&g.ui, &fw->u.field, bullets);
}

// The widgets show what the model holds. A text field's edits reach the
// model only when a form is sent (forms_flush), so until then the FIELD is
// the newer of the two: `text` rewrites the text kinds too, and is for
// when the model is the truth — the page arriving, a reset. A tick or a
// choice changes only ticks and lists, and rewrites only those.
static void forms_sync_from_model(bool text)
{
    const os64_page_t *model = page_model(&g.page);
    for (int32_t i = 0; i < g.nfw; i++) {
        FormWidget *fw = &g.fw[i];
        const os64_page_control_t *c = os64_page_control(model, fw->control);
        if (c == NULL)
            continue;
        if (!text && (fw->kind == FW_TEXT || fw->kind == FW_PASSWORD))
            continue;
        switch (fw->kind) {
        case FW_TEXT:
            os64_ui_textfield_set(&g.ui, &fw->u.field, c->value);
            break;
        case FW_PASSWORD: {
            size_t n = c->value_len < sizeof(fw->secret) ? c->value_len : sizeof(fw->secret) - 1;
            os64_memcpy(fw->secret, c->value, n);
            fw->secret_len = n;
            password_show(fw);
            break;
        }
        case FW_CHECK:
            os64_ui_checkbox_set(&g.ui, &fw->u.check, c->checked);
            break;
        case FW_LIST: {
            int selected = -1;
            for (int32_t k = 0; k < c->noptions && selected < 0; k++)
                if (c->options[k].selected)
                    selected = k;
            os64_ui_listbox_set(&g.ui, &fw->u.list, (size_t)c->noptions, selected);
            break;
        }
        default:
            break;
        }
    }
}

// Text fields tell nobody when they change, so what they hold is written
// to the model before anything reads it to send.
static void forms_flush(void)
{
    os64_page_t *model = page_model(&g.page);
    for (int32_t i = 0; i < g.nfw; i++) {
        FormWidget *fw = &g.fw[i];
        const os64_page_control_t *c = os64_page_control(model, fw->control);
        if (c == NULL || c->readonly || c->disabled)
            continue;
        const char *v = fw->kind == FW_PASSWORD ? fw->secret : fw->text;
        size_t n = fw->kind == FW_PASSWORD ? fw->secret_len : os64_strlen(fw->text);
        if ((fw->kind == FW_TEXT || fw->kind == FW_PASSWORD) &&
            (n != c->value_len || os64_memcmp(v, c->value, n) != 0))
            (void)os64_page_set_text(model, fw->control, v, n);
    }
}

// Sends a form: the button pressed (CONTROL) or Enter in a field (IMPLICIT).
// libpage decides what goes and where; libway judges it; an INVALID
// refusal puts the focus on the control that failed.
static void form_send(int32_t control, os64_page_activation_t how)
{
    forms_flush();
    os64_page_what_t what = {how, control, 0, 0};
    os64_page_request_t request;
    os64_page_verdict_t verdict = os64_page_activate(page_model(&g.page), what, &request);
    if (verdict == OS64_PAGE_NAVIGATE) {
        g.chain = 0;
        request_navigate(&request, NAV_GO, WAY_ASK_SEND);
        return;
    }
    if (verdict == OS64_PAGE_FRAGMENT) {
        scroll_to_node(request.anchor);
    } else {
        status_rest(os64_page_reason_name(request.reason));
        FormWidget *failed = request.reason == OS64_PAGE_REASON_INVALID
                                 ? form_widget(request.control) : NULL;
        if (failed != NULL && !failed->w->hidden)
            os64_ui_set_focus(&g.ui, failed->w);
    }
    os64_page_request_free(&request);
}

// The keys of a focused password field, taken before libui sees them: its
// field holds bullets, so yonder edits the value itself. True when taken.
static bool password_key(const os64_gui_event_t *ev)
{
    if (ev->type != OS64_GUI_EVENT_KEY_DOWN || g.ui.focus == NULL)
        return false;
    FormWidget *fw = NULL;
    for (int32_t i = 0; i < g.nfw && fw == NULL; i++)
        if (g.fw[i].kind == FW_PASSWORD && g.fw[i].w == g.ui.focus)
            fw = &g.fw[i];
    if (fw == NULL)
        return false;
    unsigned char a = (unsigned char)ev->key.ascii;
    if (a == '\t' || a == 0x1b)
        return false;                           // traversal and leaving are libui's
    if (a == '\r' || a == '\n') {
        form_send(fw->control, OS64_PAGE_ACTIVATE_IMPLICIT);
    } else if (a == '\b' || a == 0x7f) {
        while (fw->secret_len > 0 &&
               ((unsigned char)fw->secret[--fw->secret_len] & 0xc0) == 0x80)
            ;
        password_show(fw);
    } else if (a >= 0x20 && fw->secret_len + 4 < sizeof(fw->secret)) {
        // A key's byte is Latin-1; the value is UTF-8.
        fw->secret_len += os64_utf8_encode(a, fw->secret + fw->secret_len);
        password_show(fw);
    }
    return true;                                // arrows, selection, the rest: swallowed
}

// The page's controls, one widget each, after the window's own widgets.
static void forms_build(void)
{
    const os64_page_t *model = page_model(&g.page);
    int32_t n = g.page.tree != NULL ? flow_ncontrols(g.page.tree) : 0;
    g.ncontrols = model != NULL ? os64_page_ncontrols(model) : 0;
    if (n <= 0 || g.ncontrols <= 0)
        return;
    g.fw = os64_calloc((size_t)n, sizeof(*g.fw));
    g.by_control = os64_calloc((size_t)g.ncontrols, sizeof(*g.by_control));
    if (g.fw == NULL || g.by_control == NULL) {
        os64_free(g.fw);
        os64_free(g.by_control);
        g.fw = NULL;
        g.by_control = NULL;
        status_rest("Out of memory making this page's controls; they are drawn but cannot be used.");
        return;
    }
    for (int32_t k = 0; k < g.ncontrols; k++)
        g.by_control[k] = -1;
    for (int32_t i = 0; i < n; i++) {
        const flow_box_t *b = flow_control(g.page.tree, i);
        const os64_page_control_t *c = b != NULL ? os64_page_control(model, b->control) : NULL;
        if (c == NULL || g.by_control[b->control] >= 0)
            continue;
        FormWidget *fw = &g.fw[g.nfw];
        fw->control = b->control;
        if (c->element == OS64_PAGE_EL_SELECT) {
            fw->kind = FW_LIST;
            os64_ui_listbox(&fw->u.list, (size_t)c->noptions, option_label, list_changed, fw);
            fw->w = &fw->u.list.w;
        } else if (c->element == OS64_PAGE_EL_BUTTON || c->submits || c->resets ||
                   c->input == OS64_PAGE_INPUT_BUTTON || c->input == OS64_PAGE_INPUT_FILE) {
            fw->kind = c->input == OS64_PAGE_INPUT_FILE ? FW_FILE : FW_BUTTON;
            button_caption(c, fw->text, sizeof(fw->text));
            os64_ui_button(&fw->u.button, fw->text, button_clicked, fw);
            fw->w = &fw->u.button;
        } else if (c->input == OS64_PAGE_INPUT_CHECKBOX || c->input == OS64_PAGE_INPUT_RADIO) {
            fw->kind = FW_CHECK;
            os64_ui_checkbox(&fw->u.check, "", c->checked, check_changed, fw);
            fw->w = &fw->u.check.w;
        } else if (c->input == OS64_PAGE_INPUT_IMAGE) {
            continue;                   // a picture: a click on it sends the form
        } else {
            fw->kind = c->input == OS64_PAGE_INPUT_PASSWORD ? FW_PASSWORD : FW_TEXT;
            os64_ui_textfield(&fw->u.field, fw->text, sizeof(fw->text), field_submitted,
                              field_cancelled, fw);
            fw->w = &fw->u.field.w;
        }
        fw->w->hidden = true;
        g.by_control[b->control] = g.nfw++;
        os64_ui_add_child(&g.root, fw->w);
        os64_ui_set_enabled(&g.ui, fw->w,
                            !c->disabled && !c->readonly && fw->kind != FW_FILE);
    }
    forms_place();                      // bounds first: a field's scroll is judged by them
    forms_sync_from_model(true);
}

// Every control's widget at its box on the glass — or hidden, when its box
// is not wholly inside the page view, because libui does not clip a child
// to its parent and a widget half out of the view would paint over the
// toolbar; and hidden when the pointer cannot reach the control at its
// centre (flow_box_covered; POSITION.md, ruling 9): a field under a fixed
// header, a hidden dialog's field, one `visibility: hidden`. Whatever of
// its frame the page draws, the painter draws.
static void forms_place(void)
{
    if (g.page.tree == NULL)
        return;
    const os64_gui_rect_t v = g.view.bounds;
    int32_t n = flow_ncontrols(g.page.tree);
    for (int32_t i = 0; i < n; i++) {
        const flow_box_t *b = flow_control(g.page.tree, i);
        FormWidget *fw = b != NULL ? form_widget(b->control) : NULL;
        if (fw == NULL)
            continue;
        // The page's edges add in 64 bits (a box near INT32_MAX would wrap
        // back inside the view), and whatever is kept is clamped to int32.
        os64_gui_rect_t at = flow_box_doc_rect(b, scroll_now());
        int64_t x = (int64_t)v.x + at.x - g.sx, y = (int64_t)v.y + at.y - g.sy;
        bool shown = x >= v.x && y >= v.y && x + at.w <= (int64_t)v.x + v.w &&
                     y + at.h <= (int64_t)v.y + v.h && at.w > 0 && at.h > 0 &&
                     !flow_box_covered(g.page.tree, b, scroll_now());
        os64_gui_rect_t r = {clamp32(x), clamp32(y), at.w, at.h};
        if (!shown) {
            if (!fw->w->hidden)
                os64_ui_set_hidden(&g.ui, fw->w, true);
            fw->w->bounds = r;          // a field scrolls its text by its width
            continue;
        }
        bool moved = r.x != fw->w->bounds.x || r.y != fw->w->bounds.y ||
                     r.w != fw->w->bounds.w || r.h != fw->w->bounds.h;
        if (moved && !fw->w->hidden)
            os64_ui_mark_dirty(&g.ui, fw->w);
        fw->w->bounds = r;
        if (fw->w->hidden)
            os64_ui_set_hidden(&g.ui, fw->w, false);
        else if (moved)
            os64_ui_mark_dirty(&g.ui, fw->w);
    }
}

// The page's controls leave with it: interaction first (a hover or a press
// grab must not outlive the widget it names, and nor must the focus — which
// goes to the page view when a page widget held it, and stays where it is
// when the toolbar did), then each widget's text runs the way libui's own
// teardown releases them, then the list.
static void forms_drop(void)
{
    if (g.fw == NULL)
        return;
    bool page_focus = false;
    for (int32_t i = 0; i < g.nfw; i++)
        page_focus |= g.fw[i].w == g.ui.focus;
    os64_ui_cancel_gestures(&g.ui);
    if (page_focus)
        os64_ui_set_focus(&g.ui, &g.view);
    for (int32_t i = 0; i < g.nfw; i++) {
        os64_ui_widget_t *w = g.fw[i].w;
        if (w->cls != NULL && w->cls->destroy != NULL)
            w->cls->destroy(w);
        os64_ui_run_release(w->run);
        os64_ui_run_release(w->run_staged);
        w->run = w->run_staged = NULL;
    }
    g.status.next_sibling = NULL;       // the permanent widgets end at the status line
    os64_free(g.fw);
    os64_free(g.by_control);
    g.fw = NULL;
    g.by_control = NULL;
    g.nfw = g.ncontrols = 0;
    os64_ui_mark_dirty(&g.ui, &g.root);
}

// ── The doorbell ────────────────────────────────────────────────────────

// A job the pool finished. Only the current navigation's is looked at; any
// other finished before its cancel was noticed, and is let go unread.
static void reaped(os64_work_id_t id, void *job, void *product)
{
    if (*(const uint32_t *)job == YONDER_JOB_PICTURE) {
        picture_arrived(job, product);
        yonder_picture_release(job, product);
        pictures_feed(&g.page);
        return;
    }
    if (*(const uint32_t *)job == YONDER_JOB_SHEET) {
        sheet_arrived(job, product);
        yonder_sheet_release(job, product);
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
        yonder_trip_t *trip = job;
        if (fresh.way.posted && trip->has_request) {
            fresh.sent = trip->request;     // moved: the job's release frees nothing
            trip->has_request = false;
            os64_memset(&trip->request, 0, sizeof(trip->request));
        }
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
    if (mask & BELL_SETTINGS)
        yonder_settings_rung();
    if (mask & BELL_TICK) {
        if (g.coming.active && yonder_now_ms() >= g.coming.due)
            coming_show();
        pictures_tick();
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
// and the view's origin, and cut to the part of the view being painted —
// and, for what a verb draws whole, to the clip the painter hands it (a
// paint hook is handed no clip; libui's primitives clip only to the canvas).

// The groups open while a page is painted (paint.h): for each, where it is
// on the glass and a copy of what was there before it painted anything.
// Laying the group over that copy at its opacity once it has painted is
// exactly CSS's group compositing — the group G over the backdrop B at
// alpha a, G's own coverage c: a*c*G + (1 - a*c)*B, and a*(cG + (1-c)B) +
// (1-a)*B is the same — with no transparent canvas to paint G onto. A group
// past the deepest kept, or whose copy finds no memory, is painted opaque.
#define GLASS_GROUPS 32

typedef struct {
    struct {
        os64_gui_rect_t at;     // window coordinates; empty for nothing to lay
        uint32_t *under;
    } open[GLASS_GROUPS];
    int32_t depth;              // may pass GLASS_GROUPS; those are not kept
} GlassGroups;

typedef struct {
    os64_gui_surface_t *surf;
    int32_t dx, dy;             // page -> window
    os64_gui_rect_t clip;       // what is being painted, in window coordinates
    GlassGroups *groups;
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

// A colour's top byte is how transparent it is (flow.h): laid over what is
// there by its alpha.
static void glass_fill(void *ctx, os64_gui_rect_t r, uint32_t colour)
{
    const Glass *gl = ctx;
    r = on_glass(gl, r);
    if (r.w > 0 && r.h > 0)
        os64_draw_fill_rect_alpha(gl->surf, r, 0xff000000u | colour, flow_alpha(colour));
}

// A shadow's pixels, each row made ARGB from the colour and its alphas and
// blended where it meets what is being painted.
static void glass_mask(void *ctx, os64_gui_rect_t r, const uint8_t *alpha, uint32_t colour)
{
    const Glass *gl = ctx;
    os64_gui_rect_t at = on_glass(gl, r);
    if (at.w <= 0 || at.h <= 0)
        return;
    uint32_t *row = os64_malloc((size_t)at.w * sizeof(*row));
    if (row == NULL)
        return;
    int32_t sx = at.x - (r.x + gl->dx), sy = at.y - (r.y + gl->dy);
    for (int32_t y = 0; y < at.h; y++) {
        const uint8_t *a = alpha + (size_t)(sy + y) * (size_t)r.w + (size_t)sx;
        for (int32_t x = 0; x < at.w; x++)
            row[x] = (uint32_t)a[x] << 24 | (colour & 0xffffffu);
        os64_draw_blend(gl->surf, at.x, at.y + y, row, (uint32_t)at.w, 1, (uint32_t)at.w);
    }
    os64_free(row);
}

// A gradient's pixels, blended where they meet what is being painted.
static void glass_pixels(void *ctx, os64_gui_rect_t r, const uint32_t *argb)
{
    const Glass *gl = ctx;
    os64_gui_rect_t at = on_glass(gl, r);
    if (at.w <= 0 || at.h <= 0)
        return;
    int32_t sx = at.x - (r.x + gl->dx), sy = at.y - (r.y + gl->dy);
    os64_draw_blend(gl->surf, at.x, at.y, argb + (size_t)sy * (size_t)r.w + (size_t)sx,
                    (uint32_t)at.w, (uint32_t)at.h, (uint32_t)r.w);
}

static void glass_group_open(void *ctx, os64_gui_rect_t bounds)
{
    const Glass *gl = ctx;
    GlassGroups *gs = gl->groups;
    int32_t d = gs->depth++;
    if (d >= GLASS_GROUPS)
        return;
    os64_gui_rect_t at = on_glass(gl, bounds);
    gs->open[d].at = at;
    gs->open[d].under = NULL;
    if (at.w <= 0 || at.h <= 0)
        return;
    uint32_t *under = os64_malloc((size_t)at.w * (size_t)at.h * sizeof(uint32_t));
    if (under == NULL) {
        gs->open[d].at.w = 0;
        return;
    }
    for (int32_t y = 0; y < at.h; y++)
        os64_memcpy(under + (size_t)y * (size_t)at.w,
                    gl->surf->pixels + (size_t)(at.y + y) * gl->surf->pitch_px + (size_t)at.x,
                    (size_t)at.w * sizeof(uint32_t));
    gs->open[d].under = under;
}

// What was under the group goes back over it at 255 - alpha, which leaves
// the group at alpha over it (os64_draw_blend's mix).
static void glass_group_close(void *ctx, uint8_t alpha)
{
    const Glass *gl = ctx;
    GlassGroups *gs = gl->groups;
    int32_t d = --gs->depth;
    if (d >= GLASS_GROUPS || gs->open[d].under == NULL)
        return;
    os64_gui_rect_t at = gs->open[d].at;
    uint32_t *under = gs->open[d].under;
    uint32_t keep = (uint32_t)(255 - alpha) << 24;
    for (size_t k = 0; k < (size_t)at.w * (size_t)at.h; k++)
        under[k] = keep | (under[k] & 0xffffffu);
    os64_draw_blend(gl->surf, at.x, at.y, under, (uint32_t)at.w, (uint32_t)at.h, (uint32_t)at.w);
    os64_free(under);
    gs->open[d].under = NULL;
}

// The verbs that draw something whole — a run, a picture — cut it to
// `clip`, the painter's view narrowed by any `overflow` that clips the box,
// met with what is being painted.
static void glass_text(void *ctx, const flow_box_t *b, int32_t x, int32_t baseline,
                       os64_gui_rect_t clip, uint32_t colour)
{
    const Glass *gl = ctx;
    os64_gui_rect_t cut = on_glass(gl, clip);
    if (cut.w <= 0 || cut.h <= 0)
        return;
    os64_text_draw_alpha(b->run, gl->surf, cut, x + gl->dx, baseline + gl->dy,
                         0xff000000u | colour, flow_alpha(colour));
}

// A picture behind a box: the box's sheets, or else libpage's list, say
// whether there is one, and one that has arrived is tiled across `area`
// over what is under it — sheet layer `layer`'s at the size, position and
// repeat it says, in the origin box the painter names, or (`layer` -1) an
// attribute's unscaled from (ox, oy).
static bool glass_backdrop(void *ctx, const flow_box_t *b, int32_t layer,
                           const os64_gui_rect_t *area, os64_gui_rect_t origin, int32_t ox,
                           int32_t oy, os64_gui_rect_t clip)
{
    const Glass *gl = ctx;
    const Page *p = &g.page;
    int32_t k = -1;
    flow_layer_t l = {0};
    if (area == NULL) {
        // Only asking: a sheet's picture, whether it has come or not, or
        // an attribute's.
        for (int32_t i = 0; i < flow_background_layers(b->style); i++)
            if (flow_background_layer(b->style, i).image != NULL)
                return true;
        return b->node != NULL && os64_page_background_for(page_model(p), b->node) >= 0;
    }
    if (layer >= 0) {
        l = flow_background_layer(b->style, layer);
        k = css_picture(p, &l);
    } else {
        k = attribute_picture(p, b);
    }
    if (k < 0 || p->pics[k].state != PIC_SHOWN)
        return true;
    uint32_t w, h;
    const uint32_t *px = picture_pixels(&p->pics[k], &w, &h);
    // A sheet's picture is sized and placed in the origin box the painter
    // names; an attribute's is tiled from (ox, oy) at its own size.
    yonder_tile_t tile = {{ox, oy, (int32_t)w, (int32_t)h}, true, true};
    if (layer >= 0 && !yonder_background_tile(&l, origin, w, h, &tile))
        return true;
    os64_gui_rect_t on = {area->x + gl->dx, area->y + gl->dy, area->w, area->h};
    os64_gui_rect_t cut = on_glass(gl, clip);
    tile.at.x += gl->dx;
    tile.at.y += gl->dy;
    if (cut.w > 0 && cut.h > 0)
        yonder_tile_picture(gl->surf->pixels, gl->surf->pitch_px, cut, on, tile.at, tile.repeat_x,
                            tile.repeat_y, px, w, h);
    return true;
}

// A picture that arrived is drawn into its box, scaled and blended
// (scale.c); one on its way, or that will not come, keeps its frame.
static void glass_image(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    const Glass *gl = ctx;
    os64_gui_rect_t cut = on_glass(gl, clip);
    if (cut.w <= 0 || cut.h <= 0)
        return;
    const Page *p = &g.page;
    int32_t i = p->pic_of != NULL ? os64_page_image_for(page_model(p), b->node) : -1;
    if (i >= 0 && p->pic_of[i] >= 0 && p->pics[p->pic_of[i]].state == PIC_SHOWN) {
        uint32_t w, h;
        const uint32_t *px = picture_pixels(&p->pics[p->pic_of[i]], &w, &h);
        os64_gui_rect_t box = {c.x + gl->dx, c.y + gl->dy, c.w, c.h};
        yonder_draw_picture(gl->surf->pixels, gl->surf->pitch_px, cut, box, px, w, h);
        return;
    }
    // The frame of one still coming, cut the same way.
    Glass narrow = *gl;
    narrow.clip = cut;
    os64_gui_rect_t r = on_glass(&narrow, c);
    if (r.w <= 0 || r.h <= 0)
        return;
    os64_draw_fill_rect(gl->surf, r, 0xffe8e8e8u);
    glass_fill(&narrow, (os64_gui_rect_t){c.x, c.y, c.w, 1}, 0xa0a0a0);
    glass_fill(&narrow, (os64_gui_rect_t){c.x, c.y + c.h - 1, c.w, 1}, 0xa0a0a0);
    glass_fill(&narrow, (os64_gui_rect_t){c.x, c.y, 1, c.h}, 0xa0a0a0);
    glass_fill(&narrow, (os64_gui_rect_t){c.x + c.w - 1, c.y, 1, c.h}, 0xa0a0a0);
}

static FormWidget *form_widget(int32_t control);

// A control whose widget is showing draws itself; one hidden (its box not
// wholly in the view), or with no widget, is drawn inert: a white well with
// a sunken edge.
static void glass_control(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    const FormWidget *fw = form_widget(b->control);
    if (fw != NULL && !fw->w->hidden)
        return;
    // The stand-in is drawn whole, so it is cut like a run or a picture: a
    // control scrolled out of its box, or past an `overflow: hidden` edge,
    // is not drawn there.
    Glass cut = *(const Glass *)ctx;
    cut.clip = on_glass(&cut, clip);
    if (cut.clip.w <= 0 || cut.clip.h <= 0)
        return;
    glass_fill(&cut, c, 0xffffff);
    glass_fill(&cut, (os64_gui_rect_t){c.x, c.y, c.w, 1}, 0x808080);
    glass_fill(&cut, (os64_gui_rect_t){c.x, c.y, 1, c.h}, 0x808080);
    glass_fill(&cut, (os64_gui_rect_t){c.x, c.y + c.h - 1, c.w, 1}, 0xd4d4d4);
    glass_fill(&cut, (os64_gui_rect_t){c.x + c.w - 1, c.y, 1, c.h}, 0xd4d4d4);
}

// ── A scrolled box's bars ───────────────────────────────────────────────
//
// A box a person can scroll shows where it is with a thin bar over the
// inside of its padding box's far edge: a bar that OVERLAYS the content,
// as a touch screen's and macOS's do, so showing it changes no layout. An
// `overflow: scroll` axis shows its track whatever it holds; an `auto` one
// shows a bar only when there is something to scroll to. Whatever is
// painted over the box is over its bars too: a bar is drawn only where
// the pointer would reach the box (flow_box_covered's rule, POSITION.md
// ruling 9), at the bar's middle.

#define BOX_BAR 6               // thickness, pixels
#define BOX_BAR_THUMB_MIN 16
#define BOX_BAR_TRACK 0xd8d8d8u
#define BOX_BAR_THUMB 0x8c8c8cu

// Whether a person may scroll a box on one axis: `auto` and `scroll` say
// so, while `hidden` is a script's and a fragment link's to scroll.
static bool person_scrolls(flow_overflow_t o)
{
    return o == FLOW_OVERFLOW_AUTO || o == FLOW_OVERFLOW_SCROLL;
}

// Whether the pointer at the document point (x, y) reaches `box` or
// something inside it.
static bool reaches_box(const flow_box_t *box, int32_t x, int32_t y)
{
    const flow_box_t *h = flow_hit(g.page.tree, x, y, scroll_now());
    while (h != NULL && h != box)
        h = h->parent;
    return h != NULL;
}

// One bar along an axis: `track` in document coordinates, the thumb where
// `at` of `range` puts it, cut to `clip` — what clips the box, in document
// coordinates, or the view when nothing does.
static void box_bar(Glass *gl, os64_gui_rect_t track, bool across, int32_t at, int32_t range,
                    bool show_track, os64_gui_rect_t clip)
{
    os64_gui_rect_t on;
    if (!os64_rect_intersect(track, clip, &on))
        return;
    Glass cut = *gl;
    os64_gui_rect_t shown = on_glass(gl, on);
    if (shown.w <= 0 || shown.h <= 0)
        return;
    cut.clip = shown;
    if (show_track)
        glass_fill(&cut, track, BOX_BAR_TRACK);
    if (range <= 0)
        return;
    int64_t len = across ? track.w : track.h;
    int64_t thumb = len * len / (len + range);
    if (thumb < BOX_BAR_THUMB_MIN)
        thumb = len < BOX_BAR_THUMB_MIN ? len : BOX_BAR_THUMB_MIN;
    int64_t from = (len - thumb) * at / range;
    os64_gui_rect_t r = track;
    if (across) {
        r.x = (int32_t)(track.x + from);
        r.w = (int32_t)thumb;
    } else {
        r.y = (int32_t)(track.y + from);
        r.h = (int32_t)thumb;
    }
    glass_fill(&cut, r, BOX_BAR_THUMB);
}

static void box_bars(Glass *gl, os64_gui_rect_t view)
{
    flow_tree_t *t = g.page.tree;
    for (int32_t i = 0; i < flow_nscrollers(t); i++) {
        const flow_box_t *b = flow_scroller(t, i);
        const flow_style_t *s = b->style;
        if (b->unpainted || s->visibility != FLOW_VISIBLE)
            continue;
        flow_point_t range = flow_scroll_range(t, i), at = flow_scroll_at(t, i);
        bool down = person_scrolls(s->overflow_y) &&
                    (range.y > 0 || s->overflow_y == FLOW_OVERFLOW_SCROLL);
        bool across = person_scrolls(s->overflow_x) &&
                      (range.x > 0 || s->overflow_x == FLOW_OVERFLOW_SCROLL);
        os64_gui_rect_t r = flow_box_doc_rect(b, scroll_now()), meet;
        if ((!down && !across) || !os64_rect_intersect(r, view, &meet))
            continue;
        os64_gui_rect_t pad = padding_of(b, r);
        if (pad.w < BOX_BAR * 2 || pad.h < BOX_BAR * 2)
            continue;
        os64_gui_rect_t clip = b->clipped ? flow_box_doc_clip(b, scroll_now()) : view;
        int32_t corner = down && across ? BOX_BAR : 0;
        os64_gui_rect_t v = {pad.x + pad.w - BOX_BAR, pad.y, BOX_BAR, pad.h - corner};
        os64_gui_rect_t h = {pad.x, pad.y + pad.h - BOX_BAR, pad.w - corner, BOX_BAR};
        if (down && reaches_box(b, v.x + v.w / 2, v.y + v.h / 2))
            box_bar(gl, v, false, at.y, range.y, s->overflow_y == FLOW_OVERFLOW_SCROLL, clip);
        if (across && reaches_box(b, h.x + h.w / 2, h.y + h.h / 2))
            box_bar(gl, h, true, at.x, range.x, s->overflow_x == FLOW_OVERFLOW_SCROLL, clip);
    }
}

// Only the part of the view that is dirty is painted: the kernel takes
// exactly that rectangle when libui publishes, so nothing outside it is
// seen, and a moving picture costs its own box rather than the page.
static void view_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    (void)t;
    os64_gui_rect_t part;
    if (!os64_rect_intersect(w->bounds, g.ui.dirty, &part))
        return;
    GlassGroups groups = {.depth = 0};
    Glass gl = {&ctx->surf, w->bounds.x - g.sx, w->bounds.y - g.sy, part, &groups};
    if (g.page.tree == NULL) {
        os64_draw_fill_rect(&ctx->surf, part, 0xff000000u | PAGE_PAPER);
        return;
    }
    yonder_verbs_t v = {&gl,           glass_fill,       glass_text,       glass_image,
                        glass_control, glass_backdrop,   glass_group_open, glass_group_close,
                        glass_mask,    glass_pixels};
    os64_gui_rect_t view = {part.x - gl.dx, part.y - gl.dy, part.w, part.h};
    yonder_paint(g.page.tree, view, scroll_now(), PAGE_PAPER, &v);
    box_bars(&gl, view);
}

// The keyboard's arrows arrive as VT100 bursts (ESC [ A ...). libui's
// decoder for them is private to libos64, so the view reads the few it
// needs itself.
typedef enum { KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
               KEY_PGUP, KEY_PGDN, KEY_SPACE, KEY_POSITIONING } Key;

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
    return a == ' ' ? KEY_SPACE : a == 'p' ? KEY_POSITIONING : KEY_NONE;
}

// `p` lays pages out with every box static — the page as it reads in
// document order — until it is pressed again: a MODE, kept across
// navigation (a site's cookie wall is on every page of it), and said on
// the status line of every page laid out while it is in force (POSITION.md,
// ruling 8).
static void toggle_positioning(void)
{
    s_env.static_only = !s_env.static_only;
    relayout(true);
    status_rest(s_env.static_only ? "Positioning off: the page in document order. p turns it on."
                                  : "Positioning on.");
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
    const flow_box_t *b = flow_hit(g.page.tree, x - v.x + g.sx, y - v.y + g.sy, scroll_now());
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

// An `input type=image` is a picture that sends its form: libflow gave it
// an atom, and a click on it presses it.
static void picture_button_at(int32_t x, int32_t y)
{
    os64_gui_rect_t v = g.view.bounds;
    if (g.page.tree == NULL || x < v.x || y < v.y || x >= v.x + v.w || y >= v.y + v.h)
        return;
    const flow_box_t *b = flow_hit(g.page.tree, x - v.x + g.sx, y - v.y + g.sy, scroll_now());
    const os64_page_control_t *c =
        b != NULL && b->control >= 0 ? os64_page_control(page_model(&g.page), b->control) : NULL;
    if (c != NULL && c->input == OS64_PAGE_INPUT_IMAGE && !c->disabled)
        form_send(b->control, OS64_PAGE_ACTIVATE_CONTROL);
}

// A wheel over the page scrolls the innermost box under the pointer that
// can still move that way, then the one outside it, as browsers chain a
// scroll (CSS Overscroll 1 § 2). False when no box moved, and the page is
// the wheel's.
static bool wheel_box(int32_t x, int32_t y, int32_t dx, int32_t dy)
{
    os64_gui_rect_t v = g.view.bounds;
    flow_tree_t *t = g.page.tree;
    if (t == NULL || x < v.x || y < v.y || x >= v.x + v.w || y >= v.y + v.h)
        return false;
    const flow_box_t *b = flow_hit(t, x - v.x + g.sx, y - v.y + g.sy, scroll_now());
    int32_t i = b == NULL ? -1 : b->scroller >= 0 ? b->scroller : flow_box_scroller(b);
    for (; i >= 0; i = flow_box_scroller(flow_scroller(t, i))) {
        const flow_style_t *s = flow_scroller(t, i)->style;
        flow_point_t was = flow_scroll_at(t, i);
        flow_point_t to = {was.x + (person_scrolls(s->overflow_x) ? dx : 0),
                           was.y + (person_scrolls(s->overflow_y) ? dy : 0)};
        flow_point_t now = flow_scroll_set(t, i, to);
        if (now.x != was.x || now.y != was.y) {
            page_keep_box_scroll(&g.page, flow_scroller(t, i)->node, now);
            os64_ui_mark_dirty(&g.ui, &g.view);
            forms_place();
            pictures_schedule();
            hover(g.pointer_x, g.pointer_y);
            return true;
        }
    }
    return false;
}

static bool view_event(os64_ui_widget_t *w, os64_ui_t *ui, const os64_gui_event_t *ev)
{
    (void)ui;
    int32_t page = w->bounds.h - line_step();
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_WHEEL: {
        // Three lines a notch, as libui's lists scroll; a tilt goes sideways.
        if (ev->mouse.dy == 0 && ev->mouse.dx == 0)
            return false;
        int32_t dx = ev->mouse.dx * 3 * line_step(), dy = ev->mouse.dy * 3 * line_step();
        if (!wheel_box(ev->mouse.x, ev->mouse.y, dx, dy))
            scroll_to(g.sx + dx, g.sy + dy);
        return true;
    }
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
        else if (ev->mouse.button == OS64_GUI_MOUSE_LEFT)
            picture_button_at(ev->mouse.x, ev->mouse.y);
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
        case KEY_POSITIONING: toggle_positioning(); return true;
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
    if (g.page.sent.url != NULL) {
        resend_form();
        return;
    }
    char url[OS64_FETCH_URL_MAX];
    os64_strcopy(url, sizeof(url), g.page.way.url);
    start_trip(url, NULL, NAV_RELOAD, NULL);
}

static void click_stop(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    // A page waiting for its sheets is shown with the ones that came.
    if (g.coming.active) {
        coming_show();
        return;
    }
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
    forms_place();
    pictures_schedule();
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

// The page's title — the first `title` element in tree order, wherever the
// parser left it — whitespace collapsed, for the window's name.
static void title_of(const os64_html_document_t *doc, char *out, size_t cap)
{
    out[0] = '\0';
    const os64_html_node_t *t = NULL;
    const os64_html_node_t *n = doc != NULL ? doc->document : NULL;
    while (n != NULL && t == NULL) {
        if (n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML &&
            n->tag == OS64_HTML_TAG_TITLE) {
            t = n;
            break;
        }
        if (n->first_child != NULL) {
            n = n->first_child;
            continue;
        }
        while (n != NULL && n->next == NULL)
            n = n->parent;
        n = n != NULL ? n->next : NULL;
    }
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
// "..." first. A character past ASCII becomes one '?', because the title
// bar is drawn in the kernel's face, not the page's.
static void fit_title(const char *page, char out[OS64_GUI_TITLE_MAX])
{
    static const char kTail[] = " - yonder";
    size_t room = OS64_GUI_TITLE_MAX - 1 - (sizeof(kTail) - 1);
    size_t n = os64_strlen(page), at = 0;
    bool cut = n > room;
    size_t keep = cut ? room - 3 : n;
    for (size_t i = 0; i < keep; i++) {
        unsigned char ch = (unsigned char)page[i];
        if (ch >= 0x80 && (ch & 0xC0) == 0x80)
            continue;           // the rest of a character already marked
        out[at++] = ch < 0x80 ? (char)ch : '?';
    }
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
    g.win = os64_gui_window_create(title, 60, 40, 860, 640, OS64_GUI_WINDOW_HAS_SETTINGS);
    if (g.win < 0) {
        os64_printf("yonder: no window (is the desktop running?)\n");
        return 1;
    }
    if (os64_draw_ctx_init(&g.ctx, g.win) != 0) {
        os64_gui_window_destroy(g.win);
        return 1;
    }
    g.hover_link = g.pressed_link = -1;
    s_env.replaced_size = replaced_size;
    g.way.name = "yonder";
    char saved[YONDER_AGENT_MAX];
    const char *agent = yonder_settings_saved_agent(saved, sizeof(saved)) ? yonder_agent_keep(saved)
                                                                           : NULL;
    g.way.agent = agent != NULL ? agent : YONDER_AGENT;
    g.way.accept = YONDER_ACCEPT;
    g.way.delayed_hint = " - it is in the address field; press Enter to go";
    g.way.jar = way_jar_new();          // NULL keeps no cookies: the pages still load
    g.cache = yonder_settings_cache_open(); // NULL keeps nothing: pictures come from the network

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
    g.settle_due = YONDER_NEVER;
    g.ticker = yonder_ticker_start(g.win, BELL_TICK);
    window_seen();                      // a window born covered learns it now, not at a nudge
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
            else if (ev.type == OS64_GUI_EVENT_WINDOW_COVERED ||
                     ev.type == OS64_GUI_EVENT_WINDOW_UNCOVERED)
                window_seen();
            if (ev.type == OS64_GUI_EVENT_SETTINGS)
                yonder_settings_open(g.win, BELL_SETTINGS, g.way.agent, agent_use, g.cache);
            else if (!bar_event(&ev) && !password_key(&ev))
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

    // The clock and the Settings window's relay first: they only ring, and
    // nothing after this should.
    yonder_settings_close();
    yonder_ticker_stop(g.ticker);
    // The workers next: destroy cancels them — a fetch and a question
    // alike hear the pool's own predicate — and joins them, and releases
    // what they held, before anything they could touch goes away.
    os64_work_pool_destroy(g.pool);
    if (g.asker == ASK_REQUEST)
        os64_page_request_free(&g.pending);
    yonder_mail_drop(g.nav.mail);
    forms_drop();
    page_clear(&g.page);
    page_clear(&g.coming.page);
    way_jar_free(g.way.jar);
    if (g.cache != NULL)
        way_cache_close(g.cache);
    // Nothing is left to read an agent: the workers are gone, and the pages.
    yonder_agents_release();
    os64_ui_font_release(&g.ui);
    os64_font_family_cache_destroy(s_faces.families);
    os64_text_destroy(s_faces.text);
    os64_gui_window_destroy(g.win);
    return 0;
}
