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
#include "scripts.h"
#include "diag.h"
#include "script_job.h"
#include "geometry.h"
#include "events.h"
#include "ticker.h"
#include "trip.h"

// The largest file yonder reads: libhtml refuses by size beyond its own
// budget anyway, and says so; this only keeps a stray path from reading a
// disk image into memory first.
#define YONDER_FILE_MAX (32u << 20)

#define PAGE_INK   0x000000u
#define PAGE_LINK  0x0000eeu
#define PAGE_PAPER 0xffffffu

// DARK PAGES (paint.h's yonder_dark_t): yonder.conf's `appearance = dark`.
// A page with a dark design of its own is told to use it
// (`prefers-color-scheme: dark`), and the painter darkens what is left:
// light backgrounds go dark, dark text goes light. The paper of a page
// that set none is white's dark counterpart, as the painter turns it.
static const yonder_dark_t kDark = {.paper = 0x101010u};
static bool s_dark;

// What a navigation asks for: a page first, then a picture of a kind yonder
// shows by itself (YONDER.md § A picture asked for by itself), so a server
// that chooses by Accept may send one.
#define YONDER_ACCEPT "text/html, application/xhtml+xml, text/*;q=0.8, image/png;q=0.5, image/jpeg;q=0.5, image/gif;q=0.5, image/bmp;q=0.5"

// The work pool. A navigation's job is a FETCH (trip.h; the parse is this
// thread's, DOM_D4.md), so what it declares is one connection's worth of
// libfetch and libtls — a few hundred KiB — with room to spare; a picture
// declares PICTURE_RESERVE and a sheet SHEET_RESERVE, each its worst case.
// The budget admits twelve sheets at once beside a page's fetch: a page
// with twenty-eight linked sheets, none cached, was spending five seconds
// fetching them four at a time (Chris, 2026-10-05, on a twelve-core
// machine that was asked to use its cores and its memory).
#define POOL_WORKERS 12
#define TRIP_RESERVE (4u << 20)
#define POOL_BUDGET  ((size_t)2u << 30)

// THE STREAM (DOM_D4.md): how much of an arriving page is fed to its parser
// per turn of the loop before the window goes back to its events. A byte
// budget, because the window's clock is the 10 ms tick and bytes are
// deterministic on the host; the as-built section records what one costs
// in the guest.
#define STREAM_SLICE_BYTES (64u * 1024u)

// What a page's pictures may cost to keep — a still one's pixels, a moving
// one's whole sequence; past it a picture draws as its frame.
#define PICTURES_KEPT_MAX ((size_t)256u << 20)

// A page's pictures in the pool at once: half its workers. The pool admits
// in the order it was given work, so a navigation submitted after a page's
// pictures would wait for every one of them; yonder keeps the rest itself
// and hands over the next as each comes back, which keeps workers free for
// the person and for the next page's sheets — once any pictures of a page
// left behind have seen they were cancelled.
#define PICTURES_AT_ONCE (POOL_WORKERS / 2)

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
#define BELL_SCRIPTS (1u << 4)    // a page has another script or timer to run
#define BELL_STREAM (1u << 5)     // an arriving page has more to parse than one slice

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

// fonts.conf as read at start, or its defaults: the families above, and the
// Web setting — the page's default font size, and the controls' face.
static os64_font_config_t s_fonts_conf;

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
    os64_font_config_error_t error;
    bool read = os64_font_config_read(&s_fonts_conf, &error) == OS64_FONT_CONFIG_OK;
    if (!read)
        os64_font_config_defaults(&s_fonts_conf);
    s_env.viewport_font_px = (int32_t)s_fonts_conf.roles[OS64_FONT_CONFIG_WEB].size;
    if (read && os64_font_config_family_prepare(s_faces.text, &s_fonts_conf, &s_faces.families,
                                                &error) == OS64_FONT_CONFIG_OK)
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
static os64_html_document_t *parse_file(const uint8_t *bytes, size_t len, bool scripting)
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
    opt.scripting = scripting;
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (p == NULL)
        return NULL;
    os64_html_parser_feed(p, bytes, len);
    return os64_html_parser_finish(p);
}

// Developer fixture diagnostics, selected before any page is made: a
// scripted page's heap verdict at retirement, and what each slice of an
// arriving page cost to parse.
static bool s_script_audit;

// THE PAGE FILES (YONDER_DIAGNOSTICS.md): the directory yonder.conf's
// `diagnostics` names, empty when it names none or the directory could not
// be made (said once, at the start); the sequence the next load is given;
// and whether a failed write has been said, which is said once a run.
static struct {
    char dir[OS64_PATH_MAX];
    uint32_t next;
    bool write_said;
} s_diag = {.next = 1};

// ── Pages ───────────────────────────────────────────────────────────────

// A picture of the page's, one per ADDRESS: forty spacer GIFs are one
// fetch. The table owns its address so it survives replacement of the
// model or cascade that named it. QUEUED is
// yonder's to hand over; WAITING is in the pool.
typedef enum { PIC_QUEUED, PIC_WAITING, PIC_SHOWN, PIC_FAILED, PIC_NOT_KEPT } PictureState;

typedef struct {
    const char *url;
    PictureState state;
    os64_image_t image;             // a still picture's pixels
    os64_image_sequence_t *moving;  // or a moving one's, its frame on show
    uint64_t due;                   // when its next frame is, while it plays
    bool playing;                   // false once it stops: its loops done, or a frame failed
    os64_work_id_t id;              // while WAITING
    // An image naming it gives no width and height of its own, so its
    // arrival may require relayout; rebuilt models can add such a use.
    bool moves;
} Picture;

// One of a page's style sheets (GARB.md, G3 and G4): a `style` element's,
// a linked one, or one an @import brought. Entries never move once made —
// the cascade's input points at their parses — and an @import's entry is
// made when its importer is ready. A table is filled as its page's tree
// reveals sheets, so its order is the order they were found in; the
// cascade takes them in the order the page's model lists their elements
// (page_lay_out), which is the document's, and leaves out one whose element
// the model no longer lists.
#define SHEETS_MAX 64           // a page's sheets, @imports included
#define SHEET_IMPORTS_MAX 16    // the @imports one sheet may make
#define SHEETS_WAIT_MS 3000     // how long a page waits for its sheets before it is shown

typedef struct Sheet {
    garb_parsed_t parsed;
    struct Sheet *borrowed_from; // staged reuse; ownership moves after old layout is freed
    bool ready;                     // parsed: it is in the cascade
    bool waiting;                   // its job is out, as `id`
    // The first paint waits for it: its media holds on this glass now, or
    // it is an import of a sheet that does. One that does not is fetched
    // all the same, and laid in when it lands.
    bool holds;
    // A script waited its SHEETS_WAIT_MS for it and it did not come: it
    // holds nothing from then on, neither a later script nor the first
    // paint (stream_sheets_hold).
    bool lapsed;
    os64_work_id_t id;
    const os64_html_node_t *node;   // a sheet the page names: its element, held; NULL otherwise
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
typedef struct Page {
    way_page_t way;
    struct Page *reuse_sheets;   // source of ready linked parses during a rebuild
    bool scripting;
    // The page's script host (scripts.h), and the control state its
    // scripts and its models share: made by the stream at the first script
    // that runs, owned by the page from then on, freed after the models.
    yonder_scripts_t *scripts;
    os64_page_state_t *state;
    // THE PAGE'S RECORD (diag.h): begun with its navigation and moved with
    // it from the stream, written when the page arrives, and written again,
    // complete, when page_clear lets the page go. A struct copy of a Page
    // borrows it; only page_clear ends it.
    yonder_diag_t *diag;
    uint64_t model_version, rendered_version, state_version;
    os64_html_document_t *plain;
    os64_page_t *plain_model;
    flow_tree_t *tree;
    int32_t laid_width, laid_height;    // device pixels
    uint32_t laid_zoom;                 // thousandths (flow_env_t.zoom)
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
    int32_t nimage_map, nbackground_map;
    // The table's index by address (picture_for): open-addressed, at most
    // half full, grown with the table.
    int32_t *pic_slot;
    size_t pic_slot_cap;
    // Still to come (QUEUED or WAITING), and of those, in the pool; the
    // next picture to hand it.
    int32_t waiting, in_flight, next_pic;
    int32_t not_kept;
    size_t kept_bytes, picture_url_bytes;
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

static void measured_forget(const os64_html_document_t *doc);
static Sheet *sheet_new(Page *p, const os64_html_node_t *node, const char *media,
                        int32_t importer, int32_t import_index);

// A sheet table's parses, records and element holds (on `doc`), and the
// table.
static void sheet_table_free(const os64_html_document_t *doc, Sheet *sheets, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        if (sheets[i].node != NULL)
            os64_html_release(doc, sheets[i].node);
        if (sheets[i].borrowed_from == NULL)
            garb_free(&sheets[i].parsed);
        os64_free(sheets[i].url);
        os64_free(sheets[i].imports);
        os64_free(sheets[i].child);
    }
    os64_free(sheets);
}

static void sheets_free(Page *p)
{
    measured_forget(page_doc(p));
    sheet_table_free(page_doc(p), p->sheets, p->nsheets);
    p->sheets = NULL;
    p->nsheets = p->sheets_waiting = p->sheets_ready = 0;
}

static void diag_leave(Page *p);
static void page_clear(Page *p)
{
    measured_forget(page_doc(p));
    // The host borrows the record (its miss hook), so the record goes after
    // it, while the tree and the cascade it looks at are still here.
    yonder_scripts_free(p->scripts);
    p->scripts = NULL;
    diag_leave(p);
    for (int32_t i = 0; i < p->npics; i++) {
        os64_image_free(&p->pics[i].image);
        os64_image_sequence_free(p->pics[i].moving);
        os64_free((char *)p->pics[i].url);
    }
    os64_free(p->pics);
    os64_free(p->pic_slot);
    os64_free(p->pic_of);
    os64_free(p->bg_of);
    for (int32_t i = 0; i < p->nbox_scrolls; i++)
        os64_html_release(page_doc(p), p->box_scrolls[i].node);
    flow_free(p->tree);
    garb_cascade_free(p->cascade);
    sheets_free(p);
    os64_page_free(p->plain_model);
    os64_html_document_free(p->plain);
    // The models borrow the scripts' control state, so they go first, then
    // the state, then (in way_page_clear) the document under all of them.
    os64_page_free(p->way.model);
    p->way.model = NULL;
    os64_page_state_free(p->state);
    way_page_clear(&p->way);
    os64_page_request_free(&p->sent);
    os64_free(p->box_scrolls);
    bool audit = s_script_audit && p->scripting;
    os64_memset(p, 0, sizeof(*p));
    if (audit) {
        char line[128];
        os64_snprintf(line, sizeof(line), "yonder: scripted page retired; heap problems=%lu",
                      (unsigned long)os64_heap_verify());
        os64_debug_log(line);
    }
}

// An entry of the cascade's input for sheet `e` and, before it, for the
// ready sheets its @imports brought, each naming it as `parent`
// (garb/cascade.h). Its index, or -1 when it is not ready.
// The cascade writes into each sheet's arena as it reads a rule's block
// (garb_rules_of), so the page is not const here.
// The table's entry for the sheet `node` names, -1 when it has none.
static int32_t sheet_of(const Page *p, const os64_html_node_t *node)
{
    for (int32_t e = 0; e < p->nsheets; e++)
        if (p->sheets[e].node == node && p->sheets[e].importer < 0)
            return e;
    return -1;
}

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

// A view `w` x `h` device pixels as the page's sheets see it: in CSS
// pixels at `zoom` (thousandths), never less than one.
static garb_env_t css_view(int32_t w, int32_t h, uint32_t zoom)
{
    int64_t cw = ((int64_t)w * 1000 + zoom / 2) / zoom;
    int64_t ch = ((int64_t)h * 1000 + zoom / 2) / zoom;
    return (garb_env_t){.width = cw > 0 ? (int32_t)cw : 1, .height = ch > 0 ? (int32_t)ch : 1,
                        .dark = s_dark};
}

// `n` CSS pixels as device pixels at `zoom` (thousandths; 0 is 1000),
// rounded, never below 1 for a picture that has a size, held to INT32_MAX.
static uint32_t zoomed_px(uint32_t n, uint32_t zoom)
{
    uint64_t z = zoom != 0 ? zoom : 1000;
    uint64_t r = ((uint64_t)n * z + 500) / 1000;
    return (uint32_t)(r > INT32_MAX ? INT32_MAX : r < 1 && n > 0 ? 1 : r);
}

// A position in device pixels, laid out at zoom `from`, at zoom `to`
// (thousandths), held to an int32_t.
static int32_t scale_zoom(int32_t v, uint32_t to, uint32_t from)
{
    int64_t r = (int64_t)v * to / (from != 0 ? from : 1000);
    return (int32_t)(r > INT32_MAX ? INT32_MAX : r < INT32_MIN ? INT32_MIN : r);
}

static bool page_model_refresh(Page *p);
static void sheets_restage(Page *p);
static bool script_geometry(void *opaque, const os64_html_node_t *node,
                            os64_dom_geometry_t *out);
static bool lay_out_page(Page *p, int32_t width, int32_t height);
static bool lay_out_page_reclaim(Page *p, int32_t width, int32_t height, Page *reclaim);

// Window-owner count: a provider takes its delta to include layout retries.
static uint64_t geometry_layout_total;

// Publish a layout beside the old one. `reclaim` owns geometry whose faces
// an incomplete attempt may release before retrying; it may be this Page
// or the shown Page beside a staged DOM rebuild. An owed rebuild refuses
// before layout work on its old Page so stale meaning cannot get new geometry.
static bool page_lay_out(Page *p, int32_t width, int32_t height, uint32_t zoom, Page *reclaim)
{
    if (p->rendered_version != 0 && p->rendered_version != os64_html_version(page_doc(p)))
        return false;
    garb_cascade_t *cascade = p->cascade;
    int32_t entry[SHEETS_MAX] = {0};
    garb_env_t view = css_view(width, height, zoom);
    if (cascade == NULL || p->sheets_changed || (cascade != NULL && (garb_cascade_env(cascade).width != view.width ||
                                                  garb_cascade_env(cascade).height != view.height ||
                                                  garb_cascade_env(cascade).dark != view.dark))) {
        garb_sheet_in_t in[SHEETS_MAX];
        int32_t n = 0;
        const os64_page_t *model = page_model(p);
        for (int32_t i = 0; model != NULL && i < os64_page_nsheets(model); i++) {
            int32_t e = sheet_of(p, os64_page_sheet(model, i)->node);
            if (e >= 0)
                (void)sheet_emit(p, e, in, entry, &n);
        }
        // Inline style attributes also need a cascade when no sheet is linked.
        cascade = garb_cascade(in, n, page_doc(p), view);
        if (cascade == NULL)
            return false;
    }
    // Context is valid for this layout only and may name a stack Page;
    // libflow consults replaced_size inside flow_layout, which uses it here.
    s_env.ctx = p;
    s_env.cascade = cascade;
    s_env.scripting = p->scripting;
    // One height for `vh` and the initial containing block (flow_env_t),
    // in CSS pixels at `zoom`.
    s_env.viewport_height = view.height;
    s_env.zoom = zoom;
    geometry_layout_total++;
    flow_tree_t *fresh = flow_layout(page_doc(p), page_model(p), width, &s_env);
    // The tree on screen holds the faces its runs were shaped in until it
    // is freed — each one a size of a file, counted against the text
    // engine's budget and its face limit — so a layout at new sizes (a
    // zoom, most of all) can stop partway for want of the room the old tree
    // holds. Then both go, and the page is laid out once more in the whole
    // budget.
    if (fresh != NULL && flow_incomplete(fresh) && reclaim != NULL && reclaim->tree != NULL) {
        flow_free(fresh);
        flow_free(reclaim->tree);
        reclaim->tree = NULL;
        if (reclaim != p) {
            garb_cascade_free(reclaim->cascade);
            reclaim->cascade = NULL;
        }
        geometry_layout_total++;
        fresh = flow_layout(page_doc(p), page_model(p), width, &s_env);
    }
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
    // container now. Where it was is in device pixels, so a new zoom
    // scales it with everything else the page holds.
    for (int32_t i = 0; i < p->nbox_scrolls; i++) {
        if (p->laid_zoom != 0 && p->laid_zoom != zoom) {
            p->box_scrolls[i].at.x = scale_zoom(p->box_scrolls[i].at.x, zoom, p->laid_zoom);
            p->box_scrolls[i].at.y = scale_zoom(p->box_scrolls[i].at.y, zoom, p->laid_zoom);
        }
        int32_t s = flow_scroller_for(fresh, p->box_scrolls[i].node);
        if (s >= 0)
            flow_scroll_set(fresh, s, p->box_scrolls[i].at);
    }
    p->laid_width = width;
    p->laid_height = height;
    p->laid_zoom = zoom;
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
    os64_html_hold(page_doc(p), node);
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

// A control's widget (YONDER.md § Y4), allocated individually so its
// address and editor state survive a rebuilt model.
typedef enum { FW_TEXT, FW_PASSWORD, FW_CHECK, FW_BUTTON, FW_LIST, FW_FILE } FormKind;

typedef struct {
    FormKind kind;
    const os64_html_node_t *node;   // stable across model indices
    const os64_html_document_t *document; // owns the widget's node hold
    char synced[512], model_text[512];
    size_t synced_len, model_text_len;
    bool presented;
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

// A person's input waiting to be dispatched as a DOM event (inputs_run).
typedef enum {
    IN_MOUSEDOWN, IN_MOUSEUP, IN_CLICK, IN_OVER, IN_OUT, IN_MOVE, IN_BUTTON, IN_IMPLICIT,
    IN_CHECK, IN_LIST, IN_INPUT, IN_FOCUS, IN_BLUR, IN_CHANGE
} InputKind;

typedef struct {
    InputKind kind;
    const os64_html_node_t *node;       // held, in `doc`
    const os64_html_node_t *related;    // held: over/out's other node, a click's link
    const os64_html_document_t *doc;
    int32_t x, y;                       // window coordinates, for a mouse event
    bool was;                           // IN_CHECK: the box before the click
} Input;

#define INPUTS_MAX 32

static struct {
    int64_t win;
    os64_draw_ctx_t ctx;
    os64_ui_t ui;
    os64_ui_widget_t root, status, view, back, forward, reload, stop;
    // The badge at the right of the status bar: the shown page's MISSING
    // and FAILED counts, the file's own tokens, present only when nonzero.
    os64_ui_widget_t badge;
    char badge_text[64];
    uint32_t badge_missing, badge_failed;
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
    bool scripts_on;
    // Each script task's budget (Settings' script time limit, yonder.conf's
    // script_seconds), in milliseconds.
    uint64_t script_ms;
    // A resize or a zoom is laid out once the events that arrived with it
    // are drained: a drag sends a stream of them, a person zooming presses
    // several times, and a big page takes long enough to lay out that
    // doing it per event would freeze the window.
    bool relayout_due;
    uint8_t seq;                    // the view's VT100 key-burst state

    way_session_t way;
    // The browser's cache (CACHE.md), shared by every picture and sheet
    // job; NULL keeps nothing.
    way_cache_t *cache;
    os64_work_pool_t *pool;
    Page page;                      // the page on screen
    int32_t sx, sy;                 // where it is scrolled to, in page pixels
    // The zoom every page is laid out at, in thousandths: a window's, kept
    // across navigation, starting at the settings' default (yonder.conf),
    // which Ctrl+0 goes back to.
    uint32_t zoom, zoom_default;
    int32_t hover_link, pressed_link;
    int32_t chain;                  // refresh hops since a person last went anywhere

    // The one navigation's job in flight: the fetch, on a worker. Zero
    // once it is reaped, which may be before the page it carried has been
    // parsed — the parse is the stream's.
    struct {
        os64_work_id_t id;
    } nav;
    // The page stream_finish holds while its DOMContentLoaded runs: no
    // longer the stream's, not yet coming or shown. A fragment its scripts
    // ask for lands in `fragment`, which arrive scrolls to; a measurement
    // they make is of `page` (measured_layout).
    struct {
        Page *page;
        const yonder_scripts_t *scripts;
        const char *url;
        char *fragment;
        size_t cap;
        bool *has_fragment;
    } finishing;
    // THE PAGE ARRIVING (DOM_D4.md): parsed here, on the window's thread, a
    // slice per turn of the loop, from what the fetch posts down the
    // mailbox. Its mailbox is held here while it is current; a bell from
    // any other is not read. `kind`, `crumb` and `fragment` are what
    // arriving needs; `sent` is the window's own copy of a form being sent,
    // kept for the page that comes back (Reload sends it again).
    struct {
        bool active;
        yonder_mail_t *mail;
        NavKind kind;
        way_position_t crumb;       // BACK, FORWARD: where the person was
        bool has_fragment;
        char fragment[256];
        bool scripting;             // the parser mode, captured when the navigation started
        bool has_head;
        way_head_t head;
        os64_html_parser_t *parser; // NULL until the head (HTML), or until a text body's first bytes are in hand (stream_open)
        bool text_utf8;
        // A text body's first bytes, gathered whole before its encoding is
        // judged (way_text_sniffs): the wire may hand a byte order mark
        // over in pieces, and a parser once made has chosen.
        uint8_t sniff[3];
        size_t sniff_len;
        bool has_verdict;
        yonder_verdict_t verdict;
        os64_page_request_t sent;
        bool has_sent;
        // THE PAGE'S SCRIPTS WHILE IT ARRIVES (DOM_D7.md § The stream's
        // turn): the host and the control state it shares with the model
        // to come, both made at the first script that runs. `serial` names
        // the page to a script's fetch from the start of the navigation.
        // `stopped`: the parse waits for the blocking script.
        yonder_scripts_t *scripts;
        os64_page_state_t *state;
        uint64_t serial;
        // The load's record (diag.h), the page's once it is one; the body's
        // bytes as they were taken, for it.
        yonder_diag_t *diag;
        uint64_t bytes;
        // `ending`: the parser has been told the input ended; `ended`: it
        // said OK to that, and the deferred scripts may run. `ended_ms` is
        // when: the timers due by then run before DOMContentLoaded.
        bool stopped, ending, ended;
        uint64_t ended_ms;
        bool scriptless;                // its host could not be made: no script runs
        // THE STREAM'S PAGE (DOM_D7.md § D7d): the parse so far as a page
        // with a sheet table, filled as the parse reveals sheets
        // (stream_sheets) and moved to the page that arrives. Its
        // document, address and serial are the stream's (stream_page).
        // `sheets_due`: when the script the parse is stopped at stops
        // waiting for those still out, 0 while it is not waiting.
        Page page;
        uint64_t sheets_due;
        // A file's bytes, fed through the same turn as a fetch's, so a page
        // from disk arrives by the one road a page from the network takes.
        uint8_t *local;
        size_t local_len, local_at;
    } stream;
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
    // Widgets have stable addresses and node keys; rebuilding replaces the
    // pointer list while preserving surviving widgets and their caret.
    FormWidget **fw;
    int32_t nfw;
    // What a script asked of the page's widgets during its task (focus,
    // blur, a reset), performed after it: a task does not reach widgets.
    struct {
        const os64_html_node_t *node;   // held
        const os64_html_document_t *doc;
        os64_dom_activation_t what;
    } asks[8];
    int32_t nasks;
    // INPUT EVENTS (DOM_D7.md § Input events): what waits to be dispatched,
    // the element under the pointer and the one pressed on (each held in
    // `pointer_doc`), the control holding focus and its value when it got
    // it (for `change`), and the VT100 burst a key belongs to.
    Input inputs[INPUTS_MAX];
    int32_t ninputs;
    const os64_html_node_t *hover_node, *pressed_node;
    const os64_html_document_t *pointer_doc;
    const os64_ui_widget_t *focus_seen;
    const os64_html_node_t *focus_node;
    const os64_html_document_t *focus_doc;
    char focus_value[512];
    size_t focus_len;
    uint8_t key_seq;
} g;   // zeroed: the session's histories are large, and .data would carry them in the file

// THE MEASURED LAYOUT (DOM.md § Geometry): a page not on screen, laid out
// for its scripts' measurements at the view's size — the stream's parse as
// far as it has gone, the page stream_finish holds, the page waiting for
// its sheets. It is made from a copy of the page and never published: the
// page's own model, cascade and tree are left as they were (arrival lays
// the page out for itself), and nothing is clamped, placed or painted. It
// is kept for the reads after it while nothing it was made from has moved,
// and let go before its document, its control state or its sheets are.
// Each page is measured against its own sheet table, the stream's included
// (stream_sheets): the sheets found so far, as far as they have come.
static struct {
    const os64_html_document_t *doc;
    uint64_t version, serial;
    int32_t width, height, sheets_ready;
    uint32_t zoom;
    os64_page_t *model;
    garb_cascade_t *cascade;
    flow_tree_t *tree;
} s_measured;
static Page s_measuring;            // the copy a measured layout is made from

// Lets go of the measured layout if it is `doc`'s.
static void measured_forget(const os64_html_document_t *doc)
{
    if (doc == NULL || s_measured.doc != doc)
        return;
    flow_free(s_measured.tree);
    garb_cascade_free(s_measured.cascade);
    os64_page_free(s_measured.model);
    os64_memset(&s_measured, 0, sizeof(s_measured));
}

// `p`'s measured layout at `width` x `height` and the window's zoom, in
// s_measured. The model is the tree's version: the last measured one or the
// page's own while current, else one built beside it, sharing `state`
// (`url` is where a page with no model came from). There is one measured
// layout at a time: the one held, whichever page's, is let go first. False
// when it cannot be had, or the layout ran short: no stale answer is kept.
static bool measured_layout(const Page *p, os64_page_state_t *state, const char *url,
                            int32_t width, int32_t height)
{
    const os64_html_document_t *doc = page_doc(p);
    uint64_t version = os64_html_version(doc);
    bool mine = s_measured.doc == doc && s_measured.model != NULL;
    if (mine && s_measured.version == version && s_measured.width == width &&
        s_measured.height == height && s_measured.zoom == g.zoom && s_measured.serial == p->serial &&
        s_measured.sheets_ready == p->sheets_ready)
        return true;
    os64_page_t *model = NULL;
    if (mine && s_measured.version == version)
        model = os64_page_retain(s_measured.model) ? s_measured.model : NULL;
    else if (page_model(p) != NULL && p->model_version == version)
        model = os64_page_retain(page_model(p)) ? page_model(p) : NULL;
    else if (mine)
        model = os64_page_rebuild(s_measured.model);
    else if (page_model(p) != NULL)
        model = os64_page_rebuild(page_model(p));
    else
        model = os64_page_build(doc, url, NULL, state);
    measured_forget(s_measured.doc);
    if (model == NULL)
        return false;
    s_measuring = *p;
    s_measuring.way.model = model;
    s_measuring.plain_model = NULL;
    s_measuring.tree = NULL;
    s_measuring.cascade = NULL;
    s_measuring.sheets_changed = true;
    s_measuring.box_scrolls = NULL;
    s_measuring.nbox_scrolls = s_measuring.cap_box_scrolls = 0;
    s_measuring.rendered_version = 0;
    bool laid = page_lay_out(&s_measuring, width, height, g.zoom, NULL);
    if (laid && flow_incomplete(s_measuring.tree)) {
        flow_free(s_measuring.tree);
        garb_cascade_free(s_measuring.cascade);
        laid = false;
    }
    if (!laid) {
        os64_page_free(model);
        os64_memset(&s_measuring, 0, sizeof(s_measuring));
        return false;
    }
    s_measured.doc = doc;
    s_measured.version = version;
    s_measured.serial = p->serial;
    s_measured.width = width;
    s_measured.height = height;
    s_measured.sheets_ready = p->sheets_ready;
    s_measured.zoom = g.zoom;
    s_measured.model = model;
    s_measured.cascade = s_measuring.cascade;
    s_measured.tree = s_measuring.tree;
    os64_memset(&s_measuring, 0, sizeof(s_measuring));
    return true;
}

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
// Measured in the controls' face (controls_face_at), the one its widget
// paints in. The page chooses `size` (at most INT32_MAX rows, by
// size_rows), so the arithmetic is 64-bit and the answer clamped.
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
        if (os64_ui_text_measure(&g.ui, OS64_UI_FONT_APP, label, os64_strlen(label), &px) ==
                OS64_FONT_OK && px > widest)
            widest = px;
    }
    // The listbox's own arithmetic: a two-pixel frame, rows eight pixels
    // taller than the face's, labels six pixels in. The face is already at
    // the zoom and libflow scales a replaced box's own size by it, so the
    // answer goes back in CSS pixels, rounded up so no row is cut. At most
    // INT32_MAX rows of a face under a thousand pixels, so 64 bits hold it.
    int64_t zoom = s_env.zoom > 0 ? s_env.zoom : 1000;
    int64_t dh = rows * ((int64_t)os64_ui_font_row_height(&g.ui, OS64_UI_FONT_APP) + 8) + 4;
    int64_t dw = (int64_t)widest + 2 * 6 + 4;
    *h = clamp32((dh * 1000 + zoom - 1) / zoom);
    *w = clamp32((dw * 1000 + zoom - 1) / zoom);
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
    if (i < 0 || i >= p->nimage_map || p->pic_of[i] < 0 || p->pics[p->pic_of[i]].state != PIC_SHOWN)
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
    os64_snprintf(g.status_text, sizeof(g.status_text), "%s%s",
                  g.scripts_on ? "SCRIPTS ON | " : "", text);
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

// ── The page's record (diag.h, YONDER_DIAGNOSTICS.md) ───────────────────

static void layout(void);

// yonder.conf's `diagnostics` directory, made if it is missing and its
// parent is not. The sentence to say once when it names one that cannot be
// used (it is also logged), NULL otherwise. A run's sequence continues from
// the highest the directory already holds, so a kept census is never
// written over by the next run's first page.
static const char *diag_dir_open(void)
{
    static char said[OS64_PATH_MAX + 96];
    char dir[OS64_PATH_MAX];
    if (!yonder_settings_saved_diagnostics(dir, sizeof(dir)))
        return NULL;
    os64_dirent_t e;
    const char *why = NULL;
    if (os64_stat(dir, &e) != 0)
        why = os64_mkdir(dir) == 0 ? NULL : "cannot be made (does its parent exist?)";
    else if ((e.flags & OS64_DE_DIR) == 0)
        why = "is not a directory";
    if (why != NULL) {
        os64_snprintf(said, sizeof(said), "Diagnostics: %s %s; no page files this run.", dir, why);
        os64_debug_log(said);
        return said;
    }
    os64_strcopy(s_diag.dir, sizeof(s_diag.dir), dir);
    s_diag.next = yonder_diag_next_seq(dir);
    return NULL;
}

// A load's record, numbered in browse order. NULL on no memory: the load
// goes on unrecorded.
static yonder_diag_t *diag_begin(const char *url)
{
    return yonder_diag_new(url, s_diag.next++, os64_micros());
}

// The record as it stands, written when yonder.conf asks for files. A
// failure is said once a run, in the log and on the status line.
static void diag_write(const yonder_diag_t *d)
{
    if (d == NULL || s_diag.dir[0] == '\0' || yonder_diag_write(d, s_diag.dir) == 0 ||
        s_diag.write_said)
        return;
    s_diag.write_said = true;
    char line[OS64_PATH_MAX + 96];
    os64_snprintf(line, sizeof(line), "A page file could not be written in %s; the rest are tried.",
                  s_diag.dir);
    os64_debug_log(line);
    status_rest(line);
}

// The badge follows the shown page's counts; its width moves the status
// line's edge, so a change lays the bar out again.
static void badge_follow(void)
{
    uint32_t missing = yonder_diag_missing_lines(g.page.diag);
    uint32_t failed = yonder_diag_failed_lines(g.page.diag);
    if (missing == g.badge_missing && failed == g.badge_failed)
        return;
    g.badge_missing = missing;
    g.badge_failed = failed;
    size_t at = 0;
    g.badge_text[0] = '\0';
    if (missing != 0)
        at = (size_t)os64_snprintf(g.badge_text, sizeof(g.badge_text), YONDER_DIAG_MISSING " %lu",
                                   (unsigned long)missing);
    if (failed != 0)
        os64_snprintf(g.badge_text + at, sizeof(g.badge_text) - at, "%s" YONDER_DIAG_FAILED " %lu",
                      at != 0 ? "  " : "", (unsigned long)failed);
    layout();
    os64_ui_mark_dirty(&g.ui, &g.root);
}

// What went wrong, said on the status line and recorded where the page's
// failures are kept. `d` is the record of the page it happened to.
static void say_failed(yonder_diag_t *d, const char *what, const char *line)
{
    yonder_diag_failed(d, what, line);
    status_rest(line);
    badge_follow();
}

// A load that never became a page: its failure recorded, its record
// written and let go, and the failure said.
static void diag_abandon(yonder_diag_t *d, const char *line)
{
    yonder_diag_failed(d, "page", line);
    diag_write(d);
    yonder_diag_free(d);
    status_rest(line);
}

// The elements a page has that are drawn as nothing, and the custom
// elements nothing defines, counted whole: the tally is taken again at
// every write and holds the most any look saw.
#define DIAG_CUSTOM_MAX 64
static void diag_elements(yonder_diag_t *d, const os64_html_document_t *doc)
{
    // A frame's document is not loaded into its box (it is offered as a
    // link instead), and an embed makes no box: both are asked for and not
    // shown. An object shows its fallback, as a browser does for what it
    // cannot play, and is not counted.
    static const char *const kNothing[] = {"canvas", "video", "audio", "svg", "iframe", "frame", "embed"};
    enum { NOTHING = sizeof(kNothing) / sizeof(kNothing[0]) };
    uint32_t nothing[NOTHING] = {0};
    struct { const char *name; uint32_t n; } custom[DIAG_CUSTOM_MAX];
    int32_t ncustom = 0;
    const os64_html_node_t *n = doc != NULL ? doc->document : NULL;
    while (n != NULL) {
        if (n->kind == OS64_HTML_ELEMENT && n->name != NULL) {
            for (int k = 0; k < NOTHING; k++)
                nothing[k] += os64_streq(n->name, kNothing[k]);
            if (n->ns == OS64_HTML_NS_HTML && os64_strchr(n->name, '-') != NULL) {
                int32_t c = 0;
                while (c < ncustom && !os64_streq(custom[c].name, n->name))
                    c++;
                if (c < ncustom) {
                    custom[c].n++;
                } else if (ncustom < DIAG_CUSTOM_MAX) {
                    custom[ncustom].name = n->name;
                    custom[ncustom++].n = 1;
                }
            }
        }
        if (n->first_child != NULL) {
            n = n->first_child;
            continue;
        }
        while (n != NULL && n->next == NULL)
            n = n->parent;
        if (n != NULL)
            n = n->next;
    }
    for (int k = 0; k < NOTHING; k++)
        if (nothing[k] != 0)
            yonder_diag_missing_seen(d, "element", kNothing[k], nothing[k]);
    for (int32_t c = 0; c < ncustom; c++)
        yonder_diag_missing_seen(d, "element", custom[c].name, custom[c].n);
}

// What the page's cascade passed over (garb_cascade_skips), by its kind.
static void diag_cascade(yonder_diag_t *d, const garb_cascade_t *c)
{
    static const char *const kinds[] = {
        [GARB_SKIP_PROPERTY] = "css-property", [GARB_SKIP_AT_RULE] = "css-at-rule",
        [GARB_SKIP_FUNCTION] = "css-function", [GARB_SKIP_FONT] = "font",
        [GARB_SKIP_VALUE] = "css-value",
    };
    garb_skip_t skips[GARB_SKIPS_MAX];
    int32_t n = garb_cascade_skips(c, skips, GARB_SKIPS_MAX, NULL);
    for (int32_t i = 0; i < n && i < GARB_SKIPS_MAX; i++)
        yonder_diag_missing_seen(d, kinds[skips[i].kind], skips[i].name, skips[i].count);
}

// The page's facts as they stand, and what its tree and cascade show now.
static void diag_observe(Page *p, const char *when)
{
    yonder_diag_t *d = p->diag;
    diag_elements(d, page_doc(p));
    if (p->cascade != NULL)
        diag_cascade(d, p->cascade);
    char v[128];
    yonder_diag_fact(d, "written", when);
    if (p->tree != NULL && flow_incomplete(p->tree))
        yonder_diag_failed(d, "layout", "the layout stopped partway (INCOMPLETE on the status line)");
    int32_t shown = 0, failed = 0;
    for (int32_t i = 0; i < p->npics; i++) {
        shown += p->pics[i].state == PIC_SHOWN;
        failed += p->pics[i].state == PIC_FAILED;
    }
    os64_snprintf(v, sizeof(v), "%d, %d shown, %d could not be read, %d past the memory kept",
                  (int)p->npics, (int)shown, (int)failed, (int)p->not_kept);
    yonder_diag_fact(d, "pictures", v);
    os64_snprintf(v, sizeof(v), "%d, %d ready", (int)p->nsheets, (int)p->sheets_ready);
    yonder_diag_fact(d, "sheets", v);
}

// The page has arrived: what it is now is written, and the badge is its.
static void diag_arrived(Page *p, uint64_t laid_ms)
{
    if (p->diag == NULL)
        return;
    char v[96];
    yonder_diag_fact(p->diag, "final address", p->way.url);
    os64_snprintf(v, sizeof(v), "%ld ms", (long)((os64_micros() - yonder_diag_began(p->diag)) / 1000));
    yonder_diag_fact(p->diag, "arrived after", v);
    os64_snprintf(v, sizeof(v), "%d px in %lu ms", (int)p->laid_width, (unsigned long)laid_ms);
    yonder_diag_fact(p->diag, "laid out at", v);
    diag_observe(p, "when the page arrived");
    diag_write(p->diag);
}

// The page is let go: the departure write, which is the complete one, and
// the record with it.
static void diag_leave(Page *p)
{
    if (p->diag == NULL)
        return;
    diag_observe(p, "when the page was left");
    diag_write(p->diag);
    yonder_diag_free(p->diag);
    p->diag = NULL;
}

// The load in flight failed: recorded now, because letting the stream go
// writes its record.
static void stream_failed(const char *line)
{
    yonder_diag_failed(g.stream.diag, "page", line);
}

// Whether libpage's answer is a request this browser refused to make, as
// against a person's own form left invalid, a control the page turned off
// or a reset: the first is a failure the record keeps.
static bool reason_refuses(os64_page_reason_t reason)
{
    switch (reason) {
    case OS64_PAGE_REASON_BAD_ACTION:
    case OS64_PAGE_REASON_TOO_LONG:
    case OS64_PAGE_REASON_SCHEME:
    case OS64_PAGE_REASON_BODY_TOO_LONG:
    case OS64_PAGE_REASON_NO_MEMORY:
    case OS64_PAGE_REASON_STALE:
        return true;
    default:
        return false;
    }
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

// Where the person is, as libway keeps it for the history: the scroll, in
// device pixels, and the zoom it was measured at.
static way_position_t position_now(void)
{
    way_position_t at;
    os64_memset(&at, 0, sizeof(at));
    uint32_t zoom = g.page.laid_zoom;
    _Static_assert(sizeof(g.sx) + sizeof(g.sy) + sizeof(zoom) <= WAY_POSITION_MAX,
                   "a position fits libway's bytes");
    os64_memcpy(at.bytes, &g.sx, sizeof(g.sx));
    os64_memcpy(at.bytes + sizeof(g.sx), &g.sy, sizeof(g.sy));
    os64_memcpy(at.bytes + sizeof(g.sx) + sizeof(g.sy), &zoom, sizeof(zoom));
    return at;
}

// A remembered position is a HINT: the page that came back need not be the
// page that was left, so it is clamped to the page that did — and scaled to
// the zoom that page is laid out at, when the zoom changed while it was
// away (a position with no zoom is taken as it is).
static void position_restore(const way_position_t *at)
{
    int32_t x, y;
    uint32_t zoom;
    os64_memcpy(&x, at->bytes, sizeof(x));
    os64_memcpy(&y, at->bytes + sizeof(x), sizeof(y));
    os64_memcpy(&zoom, at->bytes + sizeof(x) + sizeof(y), sizeof(zoom));
    if (zoom != 0 && g.page.laid_zoom != 0 && zoom != g.page.laid_zoom) {
        x = scale_zoom(x, g.page.laid_zoom, zoom);
        y = scale_zoom(y, g.page.laid_zoom, zoom);
    }
    g.sx = x;
    g.sy = y;
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
    char zoomed[24] = "";
    if (g.zoom != 1000)
        os64_snprintf(zoomed, sizeof(zoomed), " zoomed %d%%", (int)((g.zoom + 5) / 10));
    os64_snprintf(line, sizeof(line), "%s%s%s - laid out at %d px%s in %lu ms%s%s%s%s",
                  g.page.way.url, g.page.way.note[0] ? " - " : "", g.page.way.note,
                  g.page.laid_width, zoomed, (unsigned long)ms, pictures, asking,
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

// Lays the page out and publishes scroll, controls and paint state. Answers
// whether the resulting layout is complete. `again` lays it
// out even at the size it has: a picture's size arrived. Without sheets
// or positioned boxes only the width moves anything; with sheets the
// height does too, which is what their media queries and vh units read,
// and with a positioned box, whose initial containing block is the view.
static bool relayout(bool again)
{
    int32_t width = g.view.bounds.w, height = g.view.bounds.h;
    bool height_matters = g.page.sheets_ready != 0 || flow_npositioned(g.page.tree) > 0;
    if (page_doc(&g.page) == NULL || width <= 0 || height <= 0)
        return false;
    if (!again && g.page.tree != NULL && width == g.page.laid_width &&
         g.zoom == g.page.laid_zoom && (!height_matters || height == g.page.laid_height))
        return !flow_incomplete(g.page.tree);
    int32_t offset = 0;
    const os64_html_node_t *anchor = anchor_of(&offset);
    // How far into the anchor the view starts, and how far across the page
    // it is, scaled with the page when the zoom changed — held aside, and
    // taken only with the layout they belong to: a layout that fails keeps
    // the old page at the old zoom, and its scroll with it.
    int32_t sx = g.sx;
    if (g.page.laid_zoom != 0 && g.page.laid_zoom != g.zoom) {
        offset = scale_zoom(offset, g.zoom, g.page.laid_zoom);
        sx = scale_zoom(g.sx, g.zoom, g.page.laid_zoom);
    }
    os64_ticks_t t0, t1;
    os64_ticks(&t0);
    bool laid = lay_out_page(&g.page, width, height);
    os64_ticks(&t1);
    // The attempt consumes the moves whether or not it fits: kept, they
    // would retry a layout that just failed at every event batch.
    g.pictures_moved = false;
    g.settle_due = YONDER_NEVER;       // the deadline goes with the moves it was for
    if (!laid) {
        say_failed(g.page.diag, "layout", "Out of memory laying the page out; this is the last layout that fit.");
        return false;
    }
    g.laid_at = t1;
    g.sx = sx;
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
    return !flow_incomplete(g.page.tree);
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
static bool request_copy(const os64_page_request_t *from, os64_page_request_t *to);

// After a page arrives: a refresh it declares, judged by libway. A GO is a
// new navigation that leaves no crumb; the chain it belongs to is counted
// from the last place a person chose to go.
static void script_alert(void *opaque, const char *text, size_t length);
static void click_back(os64_ui_widget_t *w, void *user);
static void click_forward(os64_ui_widget_t *w, void *user);
static void click_reload(os64_ui_widget_t *w, void *user);
static void follow_link_asked(int32_t link, way_ask_t ask);
static void follow_link(int32_t link);
static bool script_rebuild(void);
static void stream_drop(void);
static void stream_sheets(void);
static int64_t task_begin(yonder_scripts_t *host);
static void task_said(yonder_scripts_t *host, bool was_alive, const char *what,
                      const os64_js_outcome_t *out, int64_t began);
static void asks_perform(yonder_scripts_t *host, const os64_html_document_t *doc, bool shown);
static bool scripts_live(void);
static void input_queue(InputKind kind, const os64_html_node_t *node, const os64_html_node_t *related,
                        int32_t x, int32_t y, bool was);
static void inputs_drop(void);

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
    // An async script or a timer may have changed the tree while the page
    // waited for its sheets: the layout, the controls and the sheet table
    // are all made from the model, so all three follow the tree.
    uint64_t built = fresh->model_version;
    bool laid = page_model_refresh(fresh);
    if (laid && fresh->model_version != built)
        sheets_restage(fresh);
    laid = laid && lay_out_page(fresh, width, height);
    os64_ticks(&t1);
    if (!laid) {
        yonder_diag_failed(fresh->diag, "layout",
                           "Out of memory laying that page out; this is still the page you were on.");
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
    inputs_drop();
    yonder_scripts_free(g.page.scripts);
    g.page.scripts = NULL;
    forms_drop();
    page_clear(&g.page);
    g.page = *fresh;
    os64_memset(fresh, 0, sizeof(*fresh));
    // On screen, its scripts measure the page's own layout from now on.
    measured_forget(page_doc(&g.page));
    g.page_serial++;
    g.page.rendered_version = os64_html_version(page_doc(&g.page));
    g.page.state_version = os64_page_state_version(os64_page_shared_state(page_model(&g.page)));
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
    // `load`, at window, now the page is shown: after its sheets' wait, not
    // after its pictures (DOM_D7.md § Booked). The document reads as its
    // target, as HTML's legacy target override has it.
    if (g.page.scripts != NULL) {
        os64_dom_event_t loaded = {.type = "load"};
        os64_js_outcome_t out;
        int64_t began = task_begin(g.page.scripts);
        const os64_html_document_t *doc = page_doc(&g.page);
        yonder_scripts_dispatch(g.page.scripts, NULL, &loaded, NULL, &out);
        task_said(g.page.scripts, true, "load", &out, began);
        if (script_rebuild())
            asks_perform(g.page.scripts, doc, true);
        // What the page's scripts said while it arrived is said again: the
        // arrival wrote the status line over it.
        const char *note = yonder_scripts_note(g.page.scripts);
        if (note != NULL && page_doc(&g.page) == doc)
            status_rest(note);
    }
    // The arrival write: what the page was once it was shown and its `load`
    // had run. Its departure writes the file again, complete.
    diag_arrived(&g.page, g.laid_ms);
    g.badge_missing = g.badge_failed = UINT32_MAX;     // a new page's badge, whatever it shows
    badge_follow();
    refresh_if_declared();
    buttons_follow();
    os64_ui_mark_dirty(&g.ui, &g.view);
}

// A page arrived. Its linked sheets are sent for first — those the stream
// did not send for already — and the page on
// screen stays while they come — for up to SHEETS_WAIT_MS — so the new one
// is not drawn once bare and then again dressed; a sheet still coming after
// that is laid in when it arrives (GARB.md, G4).
static void arrive(Page *fresh, NavKind kind, const way_position_t *crumb, const char *fragment)
{
    coming_drop();
    // A streamed page has its serial from the start of its navigation: its
    // scripts' fetches already carry it.
    if (fresh->serial == 0)
        fresh->serial = ++g.pages_made;
    // DOMContentLoaded's listeners may have changed the tree since the
    // model was built; the sheet table is read from the model.
    if (!page_model_refresh(fresh)) {
        yonder_diag_failed(fresh->diag, "page",
                           "Out of memory reading that page; this is still the page you were on.");
        sheets_leave(fresh);
        page_clear(fresh);
        status_rest("Out of memory reading that page; this is still the page you were on.");
        return;
    }
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

// A file is read here, on this thread: it is local, and the read is the
// least of what a page costs. Its bytes then take the stream, as a fetch's
// do, so a page from disk runs its scripts in the order a page from the
// network does.
static bool stream_parser(const void *first, size_t n);

static void open_local(const char *path, NavKind kind, const way_position_t *crumb)
{
    uint8_t *bytes = NULL;
    size_t len = 0;
    char line[512];
    os64_snprintf(line, sizeof(line), "file://%s", path);
    yonder_diag_t *diag = diag_begin(line);
    switch (os64_slurp(path, YONDER_FILE_MAX, &bytes, &len)) {
    case OS64_SLURP_OK:
        break;
    case OS64_SLURP_NO_FILE:
        os64_snprintf(line, sizeof(line), "No such file: %s", path);
        diag_abandon(diag, line);
        return;
    case OS64_SLURP_TOO_BIG:
        os64_snprintf(line, sizeof(line), "Too big to open: %s", path);
        diag_abandon(diag, line);
        return;
    default:
        os64_snprintf(line, sizeof(line), "Could not read %s", path);
        diag_abandon(diag, line);
        return;
    }
    os64_memset(&g.stream, 0, sizeof(g.stream));
    g.stream.diag = diag;
    g.stream.local = bytes;
    g.stream.local_len = len;
    g.stream.kind = kind;
    if (crumb != NULL)
        g.stream.crumb = *crumb;
    g.stream.scripting = g.scripts_on;
    g.stream.serial = ++g.pages_made;
    g.stream.has_head = true;
    g.stream.head.body = WAY_BODY_HTML;
    os64_snprintf(g.stream.head.url, sizeof(g.stream.head.url), "file://%s", path);
    g.stream.has_verdict = true;
    g.stream.verdict.page = true;
    g.stream.verdict.fetch = OS64_FETCH_OK;
    g.stream.active = true;
    if (!stream_parser(NULL, 0)) {
        stream_failed("Out of memory reading the page.");
        stream_drop();
        status_rest("Out of memory reading the page.");
    }
    buttons_follow();
}

static void buttons_follow(void)
{
    bool loading = g.stream.active || g.coming.active;
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
    if (g.asker == ASK_WORKER && g.stream.mail != NULL)
        yonder_mail_answer(g.stream.mail, g.bar.number, false);
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
    if (asker == ASK_WORKER && g.stream.mail != NULL) {
        yonder_mail_answer(g.stream.mail, number, yes);
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

// The page arriving is not wanted after all, in DOM.md's teardown order:
// the parser is ABANDONED, handing over the document as far as it was
// built, because a runtime, wrappers and the control state may point into
// it; then the scripts (lists, registries, engine), then the state, then
// the document. The mailbox this window held is let go, and outlives it for
// as long as the job still holds its own.
static void stream_drop(void)
{
    // A load that never became a page writes its record here, with what it
    // got as far as; one that did took its record with it (stream_finish).
    diag_write(g.stream.diag);
    yonder_diag_free(g.stream.diag);
    g.stream.diag = NULL;
    if (g.asker == ASK_WORKER)
        bar_forget();
    os64_html_document_t *doc = g.stream.parser != NULL ? os64_html_parser_abandon(g.stream.parser) : NULL;
    // The sheets it sent for are not wanted; their holds are on the
    // document, which goes last.
    sheets_leave(&g.stream.page);
    sheets_free(&g.stream.page);
    measured_forget(doc);
    yonder_scripts_free(g.stream.scripts);
    os64_page_state_free(g.stream.state);
    os64_html_document_free(doc);
    os64_free(g.stream.local);
    yonder_mail_drop(g.stream.mail);
    if (g.stream.has_sent)
        os64_page_request_free(&g.stream.sent);
    os64_memset(&g.stream, 0, sizeof(g.stream));
}

// Ends the navigation in flight, if there is one, and lets go of a page
// that arrived and is waiting for its sheets: somewhere else was asked
// for. The pool's own cancellation reaches its fetch, any question it is
// waiting on, and a post waiting for room in the ring. The page on screen
// and its scripts stay live until another page replaces it (DOM.md § What
// the old page does meanwhile).
static void stop_trip(void)
{
    coming_drop();
    if (g.nav.id != 0 && g.pool != NULL)
        os64_work_cancel(g.pool, g.nav.id);
    g.nav.id = 0;
    if (g.stream.active)
        stream_drop();
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
    // The record is begun while `url` is the caller's: it may be the
    // request's own, which the job takes below.
    yonder_diag_t *diag = diag_begin(url);
    if (g.pool == NULL) {
        os64_page_request_free(request);
        diag_abandon(diag, "The background workers stopped; restart yonder to fetch pages.");
        return;
    }
    yonder_mail_t *mail = yonder_mail_new(++g.generation);
    yonder_trip_t *trip = mail != NULL ? os64_calloc(1, sizeof(*trip)) : NULL;
    if (trip == NULL) {
        yonder_mail_drop(mail);
        os64_page_request_free(request);
        diag_abandon(diag, "Out of memory starting that page.");
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
    os64_memset(&g.stream, 0, sizeof(g.stream));
    if (request != NULL) {
        g.stream.has_fragment = request->has_fragment;
        if (request->has_fragment)
            os64_strcopy(g.stream.fragment, sizeof(g.stream.fragment),
                         request->fragment != NULL ? request->fragment : "");
        // The job sends the request; the page that comes back keeps the
        // window's own copy, since it arrives on its own schedule now.
        if (!request_copy(request, &g.stream.sent)) {
            yonder_trip_release(trip, NULL);    // drops the job's reference
            yonder_mail_drop(mail);
            os64_page_request_free(request);
            diag_abandon(diag, "Out of memory starting that page.");
            return;
        }
        g.stream.has_sent = true;
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
        if (g.stream.has_sent)
            os64_page_request_free(&g.stream.sent);
        os64_memset(&g.stream, 0, sizeof(g.stream));
        diag_abandon(diag, "Too busy to start another page; try again in a moment.");
        return;
    }
    g.nav.id = id;
    g.stream.active = true;
    g.stream.serial = ++g.pages_made;
    g.stream.diag = diag;
    g.stream.mail = mail;
    g.stream.kind = kind;
    g.stream.scripting = g.scripts_on;
    if (crumb != NULL)
        g.stream.crumb = *crumb;
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
        yonder_diag_failed(g.page.diag, "link refused", g.way.status);
        badge_follow();
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
// The catalog owns each address. The index grows with the table, so pictures found
// after a layout join the same one.
static int32_t picture_for(Page *p, const char *url)
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
    // Script-driven source changes may revisit many URLs between rebuilds.
    // Keep the native catalog bounded as well as its decoded pixel storage.
    size_t bytes = os64_strlen(url) + 1;
    if (p->npics >= 4096 || bytes > 4u * 1024u * 1024u - p->picture_url_bytes)
        return -1;
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
    const char *kept = copy_text(url);
    if (kept == NULL)
        return -1;
    p->pic_slot[picture_slot(p, kept)] = p->npics;
    Picture *pic = &p->pics[p->npics];
    pic->url = kept;
    p->picture_url_bytes += bytes;
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

// Whether the first paint waits for a sheet: its media holds on this glass
// as it is now, at its size and zoom — an import's own media list as well
// as its importer's. An import's supports() is the cascade's to judge and
// is not asked here, so one that fails is waited for all the same; rare,
// and never wrong about what applies.
static bool sheet_holds(const Page *p, const Sheet *sh)
{
    if (sh->lapsed)
        return false;
    garb_env_t view = css_view(g.view.bounds.w, g.view.bounds.h, g.zoom);
    if (sh->importer >= 0) {
        const garb_import_t *im = &p->sheets[sh->importer].imports[sh->import_index];
        return p->sheets[sh->importer].holds && garb_media_matches(im->media, im->nmedia, view);
    }
    return sh->media == NULL || garb_media_text_matches(sh->media, view);
}

// A new entry, NULL when the page has as many as it may.
static Sheet *sheet_new(Page *p, const os64_html_node_t *node, const char *media,
                        int32_t importer, int32_t import_index)
{
    if (p->sheets == NULL)
        p->sheets = os64_calloc(SHEETS_MAX, sizeof(Sheet));
    if (p->sheets == NULL || p->nsheets == SHEETS_MAX)
        return NULL;
    Sheet *sh = &p->sheets[p->nsheets++];
    // Held, so an element a script removes and frees cannot lend its
    // address to a new one the table would take for it.
    if (node != NULL)
        os64_html_hold(page_doc(p), node);
    sh->node = node;
    sh->media = media;
    sh->importer = importer;
    sh->import_index = import_index;
    sh->holds = sheet_holds(p, sh);
    return sh;
}

static void coming_show(void);

// The glass changed size or zoom while a page waits for its sheets: which
// of them the first paint waits for is judged again against the glass it
// will be shown on, so a sheet that applies now is waited for and one that
// no longer does is not. An import follows its importer, which comes
// before it in the table. Nothing left to wait for shows the page.
static void sheets_rejudge(Page *p)
{
    p->sheets_waiting = 0;
    for (int32_t i = 0; i < p->nsheets; i++) {
        p->sheets[i].holds = sheet_holds(p, &p->sheets[i]);
        if (p->sheets[i].waiting && p->sheets[i].holds)
            p->sheets_waiting++;
    }
}

// The same judgement for the sheets a stopped script waits for, which the
// stream's next turn reads.
static void coming_rejudge(void)
{
    if (g.stream.active)
        sheets_rejudge(&g.stream.page);
    if (!g.coming.active)
        return;
    sheets_rejudge(&g.coming.page);
    if (g.coming.page.sheets_waiting == 0)
        coming_show();
}

// Sends a sheet's address to a worker. One that cannot be fetched stays
// out of the cascade, and the page is drawn without it.
static void sheet_ready(Page *p, int32_t e);

static void sheet_fetch(Page *p, Sheet *sh, const char *url)
{
    sh->url = copy_text(url);
    if (sh->url != NULL && p->reuse_sheets != NULL) {
        Page *old = p->reuse_sheets;
        for (int32_t i = 0; i < old->nsheets; i++) {
            Sheet *source = &old->sheets[i];
            if (!source->ready || source->url == NULL || !os64_streq(source->url, url))
                continue;
            bool lent = false;
            for (int32_t k = 0; k < p->nsheets; k++)
                lent |= p->sheets[k].borrowed_from == source;
            if (lent)
                continue;
            sh->parsed = source->parsed;
            sh->borrowed_from = source;
            sheet_ready(p, (int32_t)(sh - p->sheets));
            return;
        }
    }
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
    // A `style` element's against the page's base: the model's, or for the
    // stream's page, which has no model, the one a model would have. With
    // no base to resolve against, the sheet has no @imports.
    char tree_base[OS64_FETCH_URL_MAX];
    const char *base = sh->url != NULL ? sh->url
                     : page_model(p) != NULL ? os64_page_base(page_model(p))
                     : os64_page_base_in(page_doc(p), p->way.url, tree_base, sizeof(tree_base)) ? tree_base
                     : NULL;
    if (base == NULL)
        return;
    sh->imports = os64_calloc((size_t)n, sizeof(*sh->imports));
    sh->child = os64_calloc((size_t)n, sizeof(*sh->child));
    if (sh->imports == NULL || sh->child == NULL)
        return;
    sh->nimports = garb_sheet_imports(&sh->parsed, sh->imports, n);
    if (sh->nimports > n)
        sh->nimports = n;
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
        Sheet *child = looping ? NULL : sheet_new(p, NULL, NULL, e, k);
        if (child == NULL)
            continue;
        sh->child[k] = (int32_t)(child - p->sheets);
        sheet_fetch(p, child, url);
    }
}

// A sheet the page names, if the table does not have it yet: a `style`
// element's parsed here, a linked one sent for. A page whose table the
// stream began has most of them already (stream_sheets); a sheet is
// fetched once in a page's life. A sheet that will not parse — too big, or
// no memory — is left out, and the page is drawn without it. False once
// the table is full.
static bool sheet_add(void *page, const os64_page_sheet_t *one)
{
    Page *p = page;
    if ((one->linked && one->href.url == NULL) || sheet_of(p, one->node) >= 0)
        return true;
    Sheet *sh = sheet_new(p, one->node, one->media, -1, -1);
    if (sh == NULL)
        return false;               // the table is full
    if (one->linked) {
        sheet_fetch(p, sh, one->href.url);
    } else if (garb_parse_style_element(one->node, &sh->parsed) == GARB_OK) {
        sheet_ready(p, (int32_t)(sh - p->sheets));
    } else {
        garb_free(&sh->parsed);
    }
    return true;
}

// The rest of the page's sheets, from its own model.
static void sheets_start(Page *p)
{
    const os64_page_t *model = page_model(p);
    for (int32_t i = 0; model != NULL && i < os64_page_nsheets(model); i++)
        if (!sheet_add(p, os64_page_sheet(model, i)))
            break;
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
// which is shown once the last is in — to the page on screen, which is
// laid out again with it, or to the stream's page, whose stopped script
// the next turn looks at again; any other page's is let go.
static void sheet_arrived(const yonder_sheet_job_t *job, yonder_sheet_t *got)
{
    Page *p = g.coming.active && g.coming.page.serial == job->page ? &g.coming.page
            : g.page.serial == job->page ? &g.page
            : g.stream.active && g.stream.serial == job->page ? &g.stream.page : NULL;
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
    } else if (p == &g.page && p->sheets_changed) {
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
                (void)picture_for(p, url);
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
    int32_t *images = n > 0 ? os64_malloc((size_t)n * sizeof(*images)) : NULL;
    int32_t *backgrounds = nb > 0 ? os64_malloc((size_t)nb * sizeof(*backgrounds)) : NULL;
    if ((n > 0 && images == NULL) || (nb > 0 && backgrounds == NULL)) {
        os64_free(images);
        os64_free(backgrounds);
        // Old maps carry old model indices and cannot answer a new model.
        os64_free(p->pic_of);
        os64_free(p->bg_of);
        p->pic_of = p->bg_of = NULL;
        p->nimage_map = p->nbackground_map = 0;
        return;
    }
    os64_free(p->pic_of);
    os64_free(p->bg_of);
    p->pic_of = images;
    p->bg_of = backgrounds;
    p->nimage_map = n;
    p->nbackground_map = nb;
    // A picture moves the page when any element naming it leaves its box
    // to the picture; a background never does, being painted, not laid out.
    for (int32_t i = 0; i < n; i++) {
        const os64_page_image_t *img = os64_page_image(model, i);
        int32_t k = picture_for(p, img->src.url);
        p->pic_of[i] = k;
        if (k >= 0 && !picture_sized(p, img->node))
            p->pics[k].moves = true;
    }
    for (int32_t i = 0; i < nb; i++)
        p->bg_of[i] = picture_for(p, os64_page_background(model, i)->src.url);
    if (g.pool != NULL)
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
        // A format yonder does not decode is asked for and not had; a
        // picture that would not fetch, or was broken, is counted on the
        // record's `pictures:` line and no more.
        if (product != NULL && product->format != NULL) {
            yonder_diag_missing(g.page.diag, "image", product->format, 1);
            badge_follow();
        }
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
    return i >= 0 && i < p->nbackground_map && p->bg_of != NULL ? p->bg_of[i] : -1;
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
        if (at < 0 || at >= p->nimage_map || p->pic_of[at] != k ||
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
    for (int32_t i = 0; p->bg_of != NULL && i < os64_page_nbackgrounds(model) && i < p->nbackground_map; i++) {
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

// Hands the ticker the earliest of its deadlines: the next frame among the
// pictures on screen and the moment a slow page's layout goes stale
// (pictures_settle) — neither for a covered window — the moment a page
// waiting for its sheets is shown whatever is still coming, the moment a
// script the stream is stopped at stops waiting for its sheets, and the next
// timer of each page's scripts, covered or not.
static void pictures_schedule(void)
{
    uint64_t next = g.covered ? YONDER_NEVER : g.settle_due;
    if (g.coming.active && g.coming.due < next)
        next = g.coming.due;
    if (g.stream.active && g.stream.sheets_due != 0 && g.stream.sheets_due < next)
        next = g.stream.sheets_due;
    // A page's timers, covered or not: a page that counts down keeps
    // counting while another window is in front (DOM_D7.md § The clock).
    const yonder_scripts_t *hosts[] = {g.page.scripts, g.stream.scripts,
                                       g.coming.active ? g.coming.page.scripts : NULL};
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        uint64_t due = yonder_scripts_timer_next(hosts[i]);
        if (due < next)
            next = due;
    }
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

static FormKind control_kind(const os64_page_control_t *c);

static int32_t widget_control(const FormWidget *fw)
{
    int32_t at = os64_page_control_for(page_model(&g.page), fw->node);
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), at);
    return c != NULL && c->input != OS64_PAGE_INPUT_HIDDEN &&
           c->input != OS64_PAGE_INPUT_IMAGE && control_kind(c) == fw->kind ? at : -1;
}

static FormWidget *form_widget(int32_t control)
{
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), control);
    for (int32_t i = 0; c != NULL && i < g.nfw; i++)
        if (g.fw[i]->node == c->node)
            return g.fw[i];
    return NULL;
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
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), widget_control(fw));
    return c != NULL && (int32_t)i < c->noptions && c->options[i].label != NULL
               ? c->options[i].label : "";
}

static void form_send(int32_t control, os64_page_activation_t how);

static void field_submitted(os64_ui_textfield_t *tf, void *user)
{
    (void)tf;
    if (scripts_live()) {
        input_queue(IN_IMPLICIT, ((FormWidget *)user)->node, NULL, 0, 0, false);
        return;
    }
    form_send(widget_control((FormWidget *)user), OS64_PAGE_ACTIVATE_IMPLICIT);
}

// A person's edit changed a field: its `input` event (libui's on_change).
static void field_changed(os64_ui_textfield_t *tf, void *user)
{
    (void)tf;
    if (scripts_live())
        input_queue(IN_INPUT, ((FormWidget *)user)->node, NULL, 0, 0, false);
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
    bool was = false;
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), widget_control(fw));
    if (c != NULL)
        was = c->checked;
    if (os64_page_set_checked(page_model(&g.page), widget_control(fw), cb->checked) < 0)
        status_rest("the page keeps that one as it is");
    forms_sync_from_model(false);
    // The box changed before its click, as HTML's legacy-pre-activation
    // behaviour has it; the click may still put it back (inputs_run).
    if (scripts_live())
        input_queue(IN_CHECK, fw->node, NULL, g.pointer_x, g.pointer_y, was);
}

static void list_changed(os64_ui_listbox_t *list, void *user)
{
    const FormWidget *fw = user;
    if (list->selected >= 0)
        (void)os64_page_set_chosen(page_model(&g.page), widget_control(fw), (int32_t)list->selected, true);
    forms_sync_from_model(false);
    if (scripts_live())
        input_queue(IN_LIST, fw->node, NULL, 0, 0, false);
}

// A form reset, by a reset button or a script's reset(): libpage puts the
// values back, and this form's unflushed edits go even when its native
// values were already defaults. Other forms keep their editor state.
static void form_reset(int32_t form)
{
    int64_t result = os64_page_reset(page_model(&g.page), form);
    if (result < 0) {
        status_rest(os64_page_reason_name((os64_page_reason_t)-result));
        return;
    }
    for (int32_t i = 0; i < g.nfw; i++) {
        FormWidget *editor = g.fw[i];
        const os64_page_control_t *control =
            os64_page_control(page_model(&g.page), widget_control(editor));
        if (control != NULL && control->form == form &&
            (editor->kind == FW_TEXT || editor->kind == FW_PASSWORD))
            editor->presented = false;
    }
    forms_sync_from_model(true);
}

static void button_clicked(os64_ui_widget_t *w, void *user)
{
    (void)w;
    const FormWidget *fw = user;
    if (scripts_live()) {
        input_queue(IN_BUTTON, fw->node, NULL, g.pointer_x, g.pointer_y, false);
        return;
    }
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), widget_control(fw));
    if (c != NULL && c->resets) {
        form_reset(c->form);
        return;
    }
    form_send(widget_control(fw), OS64_PAGE_ACTIVATE_CONTROL);
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

// Project native values into the bounded editor. Passwords retain their span,
// including embedded NUL; a textfield presents a C string.
static size_t editor_value(const FormWidget *fw, const os64_page_control_t *c, char *out)
{
    size_t n = fw->kind == FW_PASSWORD ? c->value_len : os64_strlen(c->value);
    if (n >= sizeof(fw->model_text))
        n = sizeof(fw->model_text) - 1;
    os64_memcpy(out, c->value, n);
    out[n] = '\0';
    return n;
}

// Text buffers keep intermediate edits. Native value changes and an explicit
// successful reset replace them; unrelated scripts preserve caret/selection.
static void forms_sync_from_model(bool text)
{
    const os64_page_t *model = page_model(&g.page);
    for (int32_t i = 0; i < g.nfw; i++) {
        FormWidget *fw = g.fw[i];
        const os64_page_control_t *c = os64_page_control(model, widget_control(fw));
        if (c == NULL) {
            os64_ui_set_enabled(&g.ui, fw->w, false);
            continue;
        }
        os64_ui_set_enabled(&g.ui, fw->w,
                            !c->disabled && !c->readonly && fw->kind != FW_FILE);
        switch (fw->kind) {
        case FW_TEXT:
        case FW_PASSWORD: {
            if (!text)
                break;
            char value[sizeof(fw->model_text)];
            size_t n = editor_value(fw, c, value);
            if (!fw->presented || n != fw->model_text_len ||
                os64_memcmp(value, fw->model_text, n) != 0) {
                if (fw->kind == FW_TEXT)
                    os64_ui_textfield_set(&g.ui, &fw->u.field, value);
                else {
                    os64_memcpy(fw->secret, value, n + 1);
                    fw->secret_len = n;
                    password_show(fw);
                }
                os64_memcpy(fw->synced, value, n + 1);
                fw->synced_len = n;
            }
            os64_memcpy(fw->model_text, value, n + 1);
            fw->model_text_len = n;
            fw->presented = true;
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
            os64_ui_mark_dirty(&g.ui, fw->w);
            break;
        }
        case FW_BUTTON:
        case FW_FILE:
            button_caption(c, fw->text, sizeof(fw->text));
            os64_ui_mark_dirty(&g.ui, fw->w);
            break;
        }
    }
}

// Flush before script and activation so the binding observes a person's edit.
// A stale/refused edit keeps its buffer and reports the native refusal.
static void forms_flush(void)
{
    os64_page_t *model = page_model(&g.page);
    for (int32_t i = 0; i < g.nfw; i++) {
        FormWidget *fw = g.fw[i];
        const os64_page_control_t *c = os64_page_control(model, widget_control(fw));
        if (c == NULL || c->readonly || c->disabled ||
            (fw->kind != FW_TEXT && fw->kind != FW_PASSWORD))
            continue;
        const char *v = fw->kind == FW_PASSWORD ? fw->secret : fw->text;
        size_t n = fw->kind == FW_PASSWORD ? fw->secret_len : os64_strlen(fw->text);
        if (n == fw->synced_len && os64_memcmp(v, fw->synced, n) == 0)
            continue;
        int64_t result = os64_page_set_text(model, widget_control(fw), v, n);
        if (result < 0) {
            status_rest(os64_page_reason_name((os64_page_reason_t)-result));
            continue;
        }
        os64_memcpy(fw->synced, v, n);
        fw->synced[n] = '\0';
        fw->synced_len = n;
        c = os64_page_control(model, widget_control(fw));
        fw->model_text_len = editor_value(fw, c, fw->model_text);
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
        if (reason_refuses(request.reason))
            say_failed(g.page.diag, "form refused", os64_page_reason_name(request.reason));
        else
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
        if (g.fw[i]->kind == FW_PASSWORD && g.fw[i]->w == g.ui.focus)
            fw = g.fw[i];
    if (fw == NULL)
        return false;
    unsigned char a = (unsigned char)ev->key.ascii;
    if (a == '\t' || a == 0x1b)
        return false;                           // traversal and leaving are libui's
    if (a == '\r' || a == '\n') {
        field_submitted(&fw->u.field, fw);
    } else if (a == '\b' || a == 0x7f) {
        bool edited = fw->secret_len > 0;
        while (fw->secret_len > 0 &&
               ((unsigned char)fw->secret[--fw->secret_len] & 0xc0) == 0x80)
            ;
        password_show(fw);
        if (edited)
            field_changed(&fw->u.field, fw);
    } else if (a >= 0x20 && fw->secret_len + 4 < sizeof(fw->secret)) {
        // A key's byte is Latin-1; the value is UTF-8.
        fw->secret_len += os64_utf8_encode(a, fw->secret + fw->secret_len);
        password_show(fw);
        field_changed(&fw->u.field, fw);
    }
    return true;                                // arrows, selection, the rest: swallowed
}

static FormKind control_kind(const os64_page_control_t *c)
{
    if (c->element == OS64_PAGE_EL_SELECT)
        return FW_LIST;
    if (c->element == OS64_PAGE_EL_BUTTON || c->submits || c->resets ||
        c->input == OS64_PAGE_INPUT_BUTTON || c->input == OS64_PAGE_INPUT_FILE)
        return c->input == OS64_PAGE_INPUT_FILE ? FW_FILE : FW_BUTTON;
    if (c->input == OS64_PAGE_INPUT_CHECKBOX || c->input == OS64_PAGE_INPUT_RADIO)
        return FW_CHECK;
    return c->input == OS64_PAGE_INPUT_PASSWORD ? FW_PASSWORD : FW_TEXT;
}

static void widget_destroy(FormWidget *fw)
{
    if (fw->w == g.ui.focus)
        os64_ui_set_focus(&g.ui, &g.view);
    os64_ui_cancel_gestures(&g.ui);
    os64_ui_widget_t *w = fw->w;
    if (w->cls != NULL && w->cls->destroy != NULL)
        w->cls->destroy(w);
    os64_ui_run_release(w->run);
    os64_ui_run_release(w->run_staged);
    os64_html_release(fw->document, fw->node);
    os64_free(fw);
}

// Build a new list beside the old. Surviving widgets keep their addresses;
// allocation refusal leaves the old list available, resolved by native node.
static void forms_build(void)
{
    const os64_page_t *model = page_model(&g.page);
    int32_t boxes = model != NULL ? os64_page_ncontrols(model) : 0;
    FormWidget **fresh = boxes > 0 ? os64_calloc((size_t)boxes, sizeof(*fresh)) : NULL;
    if (boxes > 0 && fresh == NULL)
        goto refused;
    int32_t n = 0;
    for (int32_t i = 0; i < boxes; i++) {
        const os64_page_control_t *c = os64_page_control(model, i);
        int32_t at = i;
        if (c == NULL || c->input == OS64_PAGE_INPUT_IMAGE || c->input == OS64_PAGE_INPUT_HIDDEN)
            continue;
        bool duplicate = false;
        for (int32_t j = 0; j < n; j++)
            duplicate |= fresh[j]->node == c->node;
        if (duplicate)
            continue;
        FormKind kind = control_kind(c);
        FormWidget *fw = form_widget(at);
        if (fw != NULL && fw->kind == kind) {
            fresh[n++] = fw;
            continue;
        }
        const flow_box_t *box = flow_box_for(g.page.tree, c->node);
        if (box == NULL || box->control < 0)
            continue;
        fw = os64_calloc(1, sizeof(*fw));
        if (fw == NULL) {
            for (int32_t j = 0; j < n; j++) {
                bool kept = false;
                for (int32_t k = 0; k < g.nfw; k++)
                    kept |= fresh[j] == g.fw[k];
                if (!kept)
                    widget_destroy(fresh[j]);
            }
            os64_free(fresh);
            goto refused;
        }
        fw->node = c->node;
        fw->document = page_doc(&g.page);
        os64_html_hold(fw->document, fw->node);
        fw->kind = kind;
        switch (kind) {
        case FW_LIST:
            os64_ui_listbox(&fw->u.list, (size_t)c->noptions, option_label, list_changed, fw);
            fw->w = &fw->u.list.w;
            break;
        case FW_BUTTON:
        case FW_FILE:
            button_caption(c, fw->text, sizeof(fw->text));
            os64_ui_button(&fw->u.button, fw->text, button_clicked, fw);
            fw->w = &fw->u.button;
            break;
        case FW_CHECK:
            os64_ui_checkbox(&fw->u.check, "", c->checked, check_changed, fw);
            fw->w = &fw->u.check.w;
            break;
        case FW_TEXT:
        case FW_PASSWORD:
            os64_ui_textfield(&fw->u.field, fw->text, sizeof(fw->text), field_submitted,
                              field_cancelled, fw);
            fw->u.field.on_change = field_changed;
            fw->w = &fw->u.field.w;
            break;
        }
        fw->w->hidden = true;
        fresh[n++] = fw;
        os64_ui_widget_app_face(&g.ui, fw->w, true); // page face, like the layout
    }
    for (int32_t i = 0; i < g.nfw; i++) {
        bool kept = false;
        for (int32_t j = 0; j < n; j++)
            kept |= fresh[j] == g.fw[i];
        if (!kept)
            widget_destroy(g.fw[i]);
    }
    g.status.next_sibling = NULL;
    for (int32_t i = 0; i < n; i++) {
        fresh[i]->w->next_sibling = NULL;
        os64_ui_add_child(&g.root, fresh[i]->w);
    }
    os64_free(g.fw);
    g.fw = fresh;
    g.nfw = n;
    forms_place();
    forms_sync_from_model(true);
    return;
refused:
    say_failed(g.page.diag, "controls", "Out of memory making this page's controls; existing controls are kept.");
    forms_sync_from_model(true);
}

// Every control's widget at its box on the glass — or hidden, when its box
// is not wholly inside the page view, because libui does not clip a child
// to its parent and a widget half out of the view would paint over the
// toolbar; and hidden when the pointer cannot reach the control at its
// centre (flow_box_covered; POSITION.md, ruling 9): a field under a fixed
// header, a hidden dialog's field, one `visibility: hidden`. Whatever of
// its frame the page draws, the painter draws. Without a layout there is
// no box to place a control at, so the widgets stay hidden.
// A control the page made invisible (`opacity: 0`) is drawn when it is a
// FIELD — in a form, or named, so something is submitted — because then it
// is nearly always a custom checkbox's real input, whose styled stand-in
// cannot show what a click here did (POSITION.md, ruling 6). One in no form
// and with no name, or an empty one (which submits nothing either), is the
// page's own gadget, a click target laid over something drawn (MediaWiki's
// menu checkbox), and is left undrawn. Either way the pointer still finds
// it, as a browser's does.
static bool gadget(const flow_box_t *b, int32_t control)
{
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), control);
    return b->unpainted && c != NULL && c->form < 0 && (c->name == NULL || c->name[0] == '\0');
}

static void forms_place(void)
{
    const os64_gui_rect_t v = g.view.bounds;
    for (int32_t i = 0; i < g.nfw; i++) {
        FormWidget *fw = g.fw[i];
        const flow_box_t *b = flow_box_for(g.page.tree, fw->node);
        if (b == NULL || b->control < 0 || widget_control(fw) < 0) {
            if (!fw->w->hidden)
                os64_ui_set_hidden(&g.ui, fw->w, true);
            continue;
        }
        // The page's edges add in 64 bits (a box near INT32_MAX would wrap
        // back inside the view), and whatever is kept is clamped to int32.
        os64_gui_rect_t at = flow_box_doc_rect(b, scroll_now());
        int64_t x = (int64_t)v.x + at.x - g.sx, y = (int64_t)v.y + at.y - g.sy;
        bool shown = x >= v.x && y >= v.y && x + at.w <= (int64_t)v.x + v.w &&
                     y + at.h <= (int64_t)v.y + v.h && at.w > 0 && at.h > 0 &&
                     !flow_box_covered(g.page.tree, b, scroll_now()) && !gadget(b, widget_control(fw));
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
    for (int32_t i = 0; i < g.nfw; i++)
        widget_destroy(g.fw[i]);
    g.status.next_sibling = NULL;
    os64_free(g.fw);
    g.fw = NULL;
    g.nfw = 0;
    os64_ui_mark_dirty(&g.ui, &g.root);
}

static void script_alert(void *opaque, const char *text, size_t length)
{
    (void)opaque;
    char line[512];
    os64_snprintf(line, sizeof(line), "Page says: %.*s", (int)(length < 450 ? length : 450), text);
    status_rest(line);
}

// The model brought up to the tree's version, if scripts moved it: false
// when the rebuild does not fit, which leaves the old model in place.
// model_version is the version the model was BUILT at, never assumed.
static bool page_model_refresh(Page *p)
{
    uint64_t version = os64_html_version(page_doc(p));
    if (version == p->model_version)
        return true;
    os64_page_t *model = os64_page_rebuild(page_model(p));
    if (model == NULL)
        return false;
    os64_page_t *old = page_model(p);
    if (p->way.model != NULL)
        p->way.model = model;
    else
        p->plain_model = model;
    p->model_version = version;
    os64_page_free(old);
    return true;
}

// A page not yet laid out whose model was just refreshed takes its sheet
// table from that model, as script_rebuild's staged page does: parses
// already in are borrowed, waits on the old table are cancelled, and a new
// serial keeps an old table's job out of the new one. Sheets the scripts
// linked are fetched and join when they arrive; the first paint does not
// wait for them.
static void sheets_restage(Page *p)
{
    Page next = *p;
    next.reuse_sheets = p;
    next.sheets = NULL;
    next.nsheets = next.sheets_waiting = next.sheets_ready = 0;
    next.sheets_changed = true;
    next.serial = ++g.pages_made;
    sheets_start(&next);
    sheets_leave(p);
    // A borrowed parse moves to its borrower before the old table goes.
    for (int32_t i = 0; i < next.nsheets; i++)
        if (next.sheets[i].borrowed_from != NULL) {
            os64_memset(&next.sheets[i].borrowed_from->parsed, 0, sizeof(garb_parsed_t));
            next.sheets[i].borrowed_from = NULL;
        }
    sheets_free(p);
    p->sheets = next.sheets;
    p->nsheets = next.nsheets;
    p->sheets_waiting = next.sheets_waiting;
    p->sheets_ready = next.sheets_ready;
    p->sheets_changed = next.sheets_changed;
    p->serial = next.serial;
    p->reuse_sheets = NULL;
}

static bool script_rebuild(void)
{
    Page *p = &g.page;
    uint64_t version = os64_html_version(page_doc(p));
    if (version != p->model_version) {
        // P5 testing: task timing excludes this host rebuild. Audit its cost
        // separately to distinguish a slow update from queued mouse samples.
        int64_t began = s_script_audit ? os64_micros() : 0;
        // The displayed layout retains the old model, including its
        // control state, past the refresh's free of it.
        if (!page_model_refresh(p)) {
            say_failed(g.page.diag, "page", "Out of memory rebuilding the page; actions wait for a current model.");
            return false;
        }
        g.hover_link = g.pressed_link = -1;
        pictures_start(p);
        if (s_script_audit) {
            char line[128];
            os64_snprintf(line,sizeof(line),"yonder: model refresh in %ld us",
                (long)(os64_micros()-began));
            os64_debug_log(line);
        }
    }
    if (version != p->rendered_version) {
        int64_t began = s_script_audit ? os64_micros() : 0;
        bool zoom_changed = p->laid_zoom != 0 && p->laid_zoom != g.zoom;
        int32_t offset = 0, sx = g.sx;
        const os64_html_node_t *anchor = zoom_changed ? anchor_of(&offset) : NULL;
        if (zoom_changed) {
            offset = scale_zoom(offset, g.zoom, p->laid_zoom);
            sx = scale_zoom(sx, g.zoom, p->laid_zoom);
        }
        Page next = *p;
        next.rendered_version = version; // the staged layout's target DOM version
        next.reuse_sheets = p;
        next.tree = NULL;
        next.cascade = NULL;
        next.sheets = NULL;
        next.nsheets = next.sheets_waiting = next.sheets_ready = 0;
        next.sheets_changed = true;
        next.serial = ++g.pages_made;
        sheets_start(&next);
        // The shown Page holds the old faces; an incomplete staged layout
        // can reclaim its tree and cascade before retrying in that room.
        if (!lay_out_page_reclaim(&next, g.view.bounds.w > 0 ? g.view.bounds.w : 1,
                                 g.view.bounds.h > 0 ? g.view.bounds.h : 1, p)) {
            sheets_leave(&next);
            flow_free(next.tree);
            garb_cascade_free(next.cascade);
            sheets_free(&next);
            say_failed(p->diag, "layout", p->tree != NULL
                ? "Out of memory laying out the changed page; this is the last layout that fit."
                : "Out of memory laying out the changed page.");
            forms_build();
            os64_ui_mark_dirty(&g.ui, &g.view);
            return false;
        }
        sheets_leave(p);
        flow_free(p->tree);
        garb_cascade_free(p->cascade);
        // A borrowed parse has one staged borrower. Once its old cascade
        // is gone, transfer its arena before freeing the old sheet table.
        for (int32_t i = 0; i < next.nsheets; i++)
            if (next.sheets[i].borrowed_from != NULL) {
                os64_memset(&next.sheets[i].borrowed_from->parsed, 0, sizeof(garb_parsed_t));
                next.sheets[i].borrowed_from = NULL;
            }
        sheets_free(p);
        p->laid_width = next.laid_width;
        p->laid_height = next.laid_height;
        p->laid_zoom = next.laid_zoom;
        p->tree = next.tree;
        p->cascade = next.cascade;
        p->sheets = next.sheets;
        p->nsheets = next.nsheets;
        p->sheets_waiting = next.sheets_waiting;
        p->sheets_ready = next.sheets_ready;
        p->sheets_changed = next.sheets_changed;
        p->serial = next.serial;
        os64_memcpy(p->cascade_entry, next.cascade_entry, sizeof(p->cascade_entry));
        p->rendered_version = version;
        g.hover_link = g.pressed_link = -1;
        // A rebuild can finish a zoom whose relayout waited for this
        // DOM version. Publish its scroll position with its geometry.
        if (zoom_changed) {
            g.sx = sx;
            const flow_box_t *b = anchor != NULL ? flow_box_for(p->tree, anchor) : NULL;
            if (b != NULL)
                g.sy = b->rect.y - offset;
        }
        css_pictures(p);
        forms_build();
        clamp_scroll();
        sync_bars();
        forms_place();
        pictures_schedule();
        os64_ui_mark_dirty(&g.ui, &g.view);
        if (s_script_audit) {
            char line[128];
            os64_snprintf(line,sizeof(line),"yonder: rendering rebuild in %ld us",
                (long)(os64_micros()-began));
            os64_debug_log(line);
        }
    }
    os64_page_state_t *state = os64_page_shared_state(page_model(p));
    uint64_t revision = os64_page_state_version(state);
    if (revision != p->state_version) {
        forms_sync_from_model(true);
        p->state_version = revision;
    }
    return true;
}

// ── Scripts ─────────────────────────────────────────────────────────────
//
// A page's scripts live in its host (scripts.h): the stream's while the page
// arrives, the page's once it has. These are the host's services — a
// script's fetch on the pool, the clock, the asks a task makes — and the
// turn of the page on screen (DOM_D7.md § The shown page's turn).

static bool fetchable(const char *url)
{
    return (os64_strlen(url) > 7 && os64_memcmp(url, "http://", 7) == 0) ||
           (os64_strlen(url) > 8 && os64_memcmp(url, "https://", 8) == 0) ||
           (os64_strlen(url) > 7 && os64_memcmp(url, "file://", 7) == 0);
}

// The address of the page a host belongs to, for a fetch's Referer.
static const char *scripts_page_url(uint64_t serial)
{
    if (g.stream.scripts != NULL && yonder_scripts_serial(g.stream.scripts) == serial)
        return g.stream.head.url;
    if (g.coming.active && yonder_scripts_serial(g.coming.page.scripts) == serial)
        return g.coming.page.way.url;
    return g.page.way.url;
}

static uint64_t script_fetch(void *opaque, uint64_t serial, uint32_t token, const char *url,
                             const char *fallback)
{
    (void)opaque;
    yonder_script_job_t *job = fetchable(url) && g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
    if (job == NULL)
        return 0;
    job->kind = YONDER_JOB_SCRIPT;
    job->page = serial;
    job->token = token;
    job->agent = g.way.agent;
    os64_strcopy(job->url, sizeof(job->url), url);
    os64_strcopy(job->fallback, sizeof(job->fallback), fallback);
    job->hooks.jar = g.way.jar;
    job->hooks.cache = g.cache;
    os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), scripts_page_url(serial));
    os64_work_t work = {yonder_script_run, yonder_script_release, job, SCRIPT_RESERVE};
    os64_work_id_t id = os64_work_submit(g.pool, &work);
    if (id == 0)
        os64_free(job);
    return id;
}

static void script_cancel(void *opaque, uint64_t job)
{
    (void)opaque;
    if (g.pool != NULL)
        os64_work_cancel(g.pool, job);
}

static uint64_t script_now(void *opaque)
{
    (void)opaque;
    return yonder_now_ms();
}

// focus(), blur() and a reset nobody cancelled reach widgets, which a task
// must not: each is kept, held, and performed when the task is over.
static void script_activate(void *opaque, const os64_html_node_t *node, os64_dom_activation_t what)
{
    if (g.nasks == (int32_t)(sizeof(g.asks) / sizeof(g.asks[0])))
        return;
    os64_html_hold(opaque, node);
    g.asks[g.nasks].node = node;
    g.asks[g.nasks].doc = opaque;
    g.asks[g.nasks].what = what;
    g.nasks++;
}

// document.write, from the script the arriving page's parse is stopped at:
// the stream's parser takes it at its insertion point. A host whose
// document is not the one being parsed has no parser to write into.
static int64_t script_write(void *opaque, const char *utf8, size_t length)
{
    if (g.stream.parser == NULL || os64_html_parser_document(g.stream.parser) != opaque)
        return OS64_HTML_BAD_ARGUMENT;
    return os64_html_parser_write(g.stream.parser, utf8, length);
}

static const char *script_user_agent(void *opaque)
{
    (void)opaque;
    return g.way.agent != NULL ? g.way.agent : YONDER_AGENT;
}

// document.cookie: the browser's jar, for the page's own address
// (libway's script door, which keeps HttpOnly cookies from scripts).
static size_t script_cookies_get(void *opaque, char *out, size_t cap, bool *whole)
{
    return way_script_cookies(g.way.jar, yonder_scripts_url(opaque), out, cap, whole);
}

static void script_cookies_set(void *opaque, const char *text, size_t length)
{
    way_script_cookie(g.way.jar, yonder_scripts_url(opaque), text, length);
}

static yonder_scripts_t *scripts_host(os64_html_document_t *doc, os64_page_state_t *state,
                                      const char *url, uint64_t serial, yonder_diag_t *diag)
{
    yonder_scripts_options_t options = {url, g.script_ms, serial, script_alert, script_fetch,
                                        script_cancel, script_activate, script_write, script_now,
                                        script_geometry, doc, diag};
    yonder_scripts_t *host = yonder_scripts_new(doc, state, &options);
    yonder_scripts_set_user_agent(host, script_user_agent, NULL);
    const os64_dom_cookies_t cookies = {script_cookies_get, script_cookies_set, host};
    yonder_scripts_set_cookies(host, &cookies);
    return host;
}

// The host a fetch job's serial names: the page arriving, the page waiting
// for its sheets, or the page on screen. NULL: its page is gone.
static yonder_scripts_t *scripts_for(uint64_t serial)
{
    if (g.stream.scripts != NULL && yonder_scripts_serial(g.stream.scripts) == serial)
        return g.stream.scripts;
    if (g.coming.active && g.coming.page.scripts != NULL &&
        yonder_scripts_serial(g.coming.page.scripts) == serial)
        return g.coming.page.scripts;
    if (g.page.scripts != NULL && yonder_scripts_serial(g.page.scripts) == serial)
        return g.page.scripts;
    return NULL;
}

static void script_arrived(const yonder_script_job_t *job, const yonder_script_t *got)
{
    yonder_scripts_t *host = scripts_for(job->page);
    if (host != NULL)
        yonder_scripts_fetched(host, job->token, got != NULL && got->ok ? got->source : NULL,
                               got != NULL ? got->length : 0);
}

// A task is about to run: the layouts it forces are counted from zero
// (DOM.md § Geometry), and the audit's clock starts. Every task kind
// begins here and ends at task_said.
static int64_t task_begin(yonder_scripts_t *host)
{
    (void)yonder_scripts_geometry_stats(host, true);
    return s_script_audit ? os64_micros() : 0;
}

// What a task said, on the status line: the layouts it forced and what they
// cost, then an error by its script's name, and a runtime its task retired
// by the sentence that says the page goes on without script. The audit
// logs every task, what it cost, what it forced and what it wrote: a page
// that writes its whole body shows as one.
static void task_said(yonder_scripts_t *host, bool was_alive, const char *what,
                      const os64_js_outcome_t *out, int64_t began)
{
    os64_dom_geometry_stats_t forced = yonder_scripts_geometry_stats(host, false);
    if (s_script_audit) {
        char line[OS64_JS_SOURCE_NAME_CAP + 160];
        os64_snprintf(line, sizeof(line), "yonder: task %s %s: status %d in %ld us, wrote %lu bytes, "
                      "forced %lu layouts in %lu us",
                      what, out->source_name, (int)out->status, (long)(os64_micros() - began),
                      (unsigned long)yonder_scripts_written(host), (unsigned long)forced.layouts,
                      (unsigned long)forced.elapsed_us);
        os64_debug_log(line);
    }
    char line[512];
    size_t at = 0;
    if (forced.layouts != 0) {
        os64_snprintf(line, sizeof(line), "Script forced %lu layouts in %lu.%03lu ms",
                      (unsigned long)forced.layouts, (unsigned long)(forced.elapsed_us / 1000),
                      (unsigned long)(forced.elapsed_us % 1000));
        at = os64_strlen(line);
    }
    yonder_diag_t *diag = yonder_scripts_diag(host);
    const char *name = out->source_name[0] ? out->source_name : what;
    // The record names a `src` script by its whole address, which the
    // engine's source name may cut short; the status line keeps the name.
    const char *address = yonder_scripts_task_address(host);
    char key[OS64_DOM_URL_MAX + 16];
    os64_snprintf(key, sizeof(key), "script %s", address != NULL ? address : name);
    // A task with a script's name is a script's, and the record lists it;
    // an event or a timer with none is listed only by a failure.
    if (out->status == OS64_JS_OK) {
        if (out->source_name[0] != '\0')
            yonder_diag_fact(diag, key, "ran");
        badge_follow();     // a global it missed is recorded as it runs
        if (at != 0)
            status_rest(line);
        return;
    }
    if (at != 0) {
        os64_strcopy(line + at, sizeof(line) - at, "; ");
        at += 2;
    }
    // The record keeps what the line says of the script, without the
    // layouts beside it: the same failure is one line however many times
    // it happens, and the count says how many.
    char failure[OS64_JS_MESSAGE_CAP + 64];
    if (was_alive && !yonder_scripts_alive(host)) {
        if (out->status == OS64_JS_LIMIT && out->limit == OS64_JS_LIMIT_EXECUTION)
            os64_snprintf(failure, sizeof(failure), "execution stopped after %lu s "
                          "(this page runs without script)",
                          (unsigned long)(yonder_scripts_execution_ms(host) / 1000));
        else
            os64_snprintf(failure, sizeof(failure), "%s (this page runs without script)",
                          out->message[0] ? out->message : "the runtime failed");
    } else {
        os64_strcopy(failure, sizeof(failure), out->message[0] ? out->message : "error");
    }
    os64_snprintf(line + at, sizeof(line) - at, "Script %s: %s", name, failure);
    yonder_diag_failed(diag, key, failure);
    if (out->source_name[0] != '\0')
        yonder_diag_fact(diag, key, "failed");
    badge_follow();
    status_rest(line);
    yonder_scripts_set_note(host, line);
}

// A request for an address a script named, owning its own storage as
// libpage's do. False when memory ran out.
static bool request_for(const char *url, os64_page_request_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->control = -1;
    out->method = OS64_PAGE_METHOD_GET;
    if (!os64_url_scheme_of(url, out->scheme, sizeof(out->scheme)))
        return false;
    char *copy = copy_text(url);
    if (copy == NULL)
        return false;
    out->url = copy;
    out->leaves_machine = !os64_streq(out->scheme, "mailto") && !os64_streq(out->scheme, "data") &&
                          !os64_streq(out->scheme, "javascript");
    out->downgrade = os64_strlen(g.page.way.url) > 8 && os64_memcmp(g.page.way.url, "https://", 8) == 0 &&
                     os64_streq(out->scheme, "http");
    return true;
}

// The part of an address before its fragment, as a length.
static size_t before_fragment(const char *url)
{
    size_t n = 0;
    while (url[n] != '\0' && url[n] != '#')
        n++;
    return n;
}

// Where a page not on screen keeps a fragment its scripts ask for, and the
// address it came from: the stream's, the page stream_finish holds, or the
// coming page's. False for a host that is none of them.
static bool unshown_place(const yonder_scripts_t *host, const char **url, char **fragment,
                          size_t *cap, bool **has_fragment)
{
    if (host == NULL)
        return false;
    if (g.stream.active && host == g.stream.scripts) {
        *url = g.stream.head.url;
        *fragment = g.stream.fragment;
        *cap = sizeof(g.stream.fragment);
        *has_fragment = &g.stream.has_fragment;
        return true;
    }
    if (host == g.finishing.scripts) {
        *url = g.finishing.url;
        *fragment = g.finishing.fragment;
        *cap = g.finishing.cap;
        *has_fragment = g.finishing.has_fragment;
        return true;
    }
    if (g.coming.active && host == g.coming.page.scripts) {
        *url = g.coming.page.way.url;
        *fragment = g.coming.fragment;
        *cap = sizeof(g.coming.fragment);
        *has_fragment = &g.coming.has_fragment;
        return true;
    }
    return false;
}

// A navigation a script asked for, performed once its task is over, by the
// door a person's own request takes (DOM.md § The event loop): judged as a
// page's own refresh is (WAY_ASK_GO), so a script cannot take a person
// from https to http without the question bar. `shown` says whether the
// asking page is on screen; a page still arriving has no model, so a link
// or a form of its own waits for it (DOM.md § D7b, as built: Booked).
static void navigation_perform(const yonder_scripts_t *host, const os64_dom_navigation_t *ask,
                               bool shown)
{
    switch (ask->kind) {
    case OS64_DOM_NAVIGATE_URL:
    case OS64_DOM_NAVIGATE_REPLACE: {
        // Only the fragment changed: a place in the asking page, no
        // request. On screen it scrolls now; a page not yet shown keeps
        // the fragment, and arriving scrolls to it.
        size_t n = before_fragment(ask->url);
        const char *here = NULL;
        char *fragment = NULL;
        size_t cap = 0;
        bool *has_fragment = NULL;
        if (shown)
            here = g.page.way.url;
        else if (!unshown_place(host, &here, &fragment, &cap, &has_fragment))
            here = NULL;
        if (here != NULL && ask->url[n] == '#' && n == before_fragment(here) &&
            os64_memcmp(ask->url, here, n) == 0) {
            if (shown) {
                scroll_to_fragment(ask->url + n + 1);
            } else {
                os64_strcopy(fragment, cap, ask->url + n + 1);
                *has_fragment = true;
            }
            return;
        }
        os64_page_request_t request;
        if (!request_for(ask->url, &request)) {
            say_failed(g.page.diag, "navigation", "A script asked for an address this browser cannot go to.");
            return;
        }
        request_navigate(&request, ask->kind == OS64_DOM_NAVIGATE_REPLACE ? NAV_REFRESH : NAV_GO,
                         WAY_ASK_GO);
        return;
    }
    case OS64_DOM_NAVIGATE_RELOAD:
        if (shown)
            click_reload(NULL, NULL);
        return;
    case OS64_DOM_NAVIGATE_HISTORY:
        if (ask->delta < 0)
            click_back(NULL, NULL);
        else
            click_forward(NULL, NULL);
        return;
    case OS64_DOM_NAVIGATE_FOLLOW:
        if (shown) {
            int32_t link = os64_page_link_for(page_model(&g.page), ask->node);
            if (link >= 0)
                follow_link_asked(link, WAY_ASK_GO);
        }
        return;
    case OS64_DOM_NAVIGATE_SUBMIT:
        if (shown) {
            int32_t control = ask->submitter != NULL
                ? os64_page_control_for(page_model(&g.page), ask->submitter) : -1;
            if (control >= 0)
                form_send(control, OS64_PAGE_ACTIVATE_CONTROL);
            else
                form_send(os64_page_form_for(page_model(&g.page), ask->node), OS64_PAGE_ACTIVATE_FORM);
        }
        return;
    case OS64_DOM_NAVIGATE_NONE:
        return;
    }
}

// After a task: what it asked of the widgets, then the navigation it asked
// for. The ask's nodes are let go before it is performed, since going
// somewhere may free the very page they belong to.
static void asks_perform(yonder_scripts_t *host, const os64_html_document_t *doc, bool shown)
{
    for (int32_t i = 0; i < g.nasks; i++) {
        if (shown && g.asks[i].doc == page_doc(&g.page)) {
            int32_t control = os64_page_control_for(page_model(&g.page), g.asks[i].node);
            FormWidget *fw = control >= 0 ? form_widget(control) : NULL;
            if (g.asks[i].what == OS64_DOM_ACTIVATE_RESET) {
                int32_t form = os64_page_form_for(page_model(&g.page), g.asks[i].node);
                if (form >= 0)
                    form_reset(form);
            } else if (fw != NULL && !fw->w->hidden && g.asks[i].what == OS64_DOM_ACTIVATE_FOCUS) {
                os64_ui_set_focus(&g.ui, fw->w);
            } else if (fw != NULL && g.ui.focus == fw->w && g.asks[i].what == OS64_DOM_ACTIVATE_BLUR) {
                os64_ui_set_focus(&g.ui, &g.view);
            }
        }
        os64_html_release(g.asks[i].doc, g.asks[i].node);
    }
    g.nasks = 0;
    os64_dom_navigation_t ask;
    if (!yonder_scripts_take_navigation(host, &ask))
        return;
    os64_dom_navigation_t held = ask;
    if (!shown) {
        if (ask.node != NULL)
            os64_html_release(doc, ask.node);
        if (ask.submitter != NULL)
            os64_html_release(doc, ask.submitter);
        held.node = held.submitter = NULL;
        navigation_perform(host, &held, false);
        return;
    }
    navigation_perform(host, &held, true);
    // A shown page's model was read while the nodes were held; the page
    // may since have been replaced, and its document freed with them.
    if (page_doc(&g.page) == doc) {
        if (ask.node != NULL)
            os64_html_release(doc, ask.node);
        if (ask.submitter != NULL)
            os64_html_release(doc, ask.submitter);
    }
}

// One task of a page's: a ready script, else a due timer. True when one
// ran. `shown` decides whether its asks may reach the page on screen.
static bool scripts_task(yonder_scripts_t *host, const os64_html_document_t *doc, bool shown)
{
    if (host == NULL || !yonder_scripts_alive(host))
        return false;
    os64_js_outcome_t out;
    int64_t began = task_begin(host);
    const char *what = "script";
    bool ran = yonder_scripts_step(host, &out);
    if (!ran) {
        what = "timer";
        ran = yonder_scripts_timer_fire(host, yonder_now_ms(), &out);
    }
    if (!ran)
        return false;
    task_said(host, true, what, &out, began);
    asks_perform(host, doc, shown);
    return true;
}

// Whether a host has a task to run now: a script ready, or a timer due.
static bool scripts_owed(const yonder_scripts_t *host)
{
    return yonder_scripts_pending(host) || yonder_scripts_timer_next(host) <= yonder_now_ms();
}

// The measured layout of the page whose document is `doc`, when that page
// is not on screen: the one stream_finish holds, the one waiting for its
// sheets, or the stream's parse so far. False for none of them.
static Page *stream_page(void);

static bool measured_for(const os64_html_document_t *doc, int32_t width, int32_t height)
{
    if (g.finishing.page != NULL && page_doc(g.finishing.page) == doc)
        return measured_layout(g.finishing.page, g.finishing.page->state, g.finishing.page->way.url,
                               width, height);
    if (g.coming.active && page_doc(&g.coming.page) == doc)
        return measured_layout(&g.coming.page, g.coming.page.state, g.coming.page.way.url, width, height);
    if (g.stream.parser == NULL || os64_html_parser_document(g.stream.parser) != doc)
        return false;
    return measured_layout(stream_page(), g.stream.state, g.stream.head.url, width, height);
}

// A script host's current-layout provider (DOM.md § Geometry), whose opaque
// is its page's document. The page on screen has its pending rebuild and an
// owed layout done now and published; a page not on screen is measured
// beside itself (measured_layout). Either way the work is charged to the
// asking script, and the box is read from the layout that results.
static bool script_geometry(void *opaque, const os64_html_node_t *node,
                            os64_dom_geometry_t *out)
{
    const os64_html_document_t *doc = opaque;
    if (!os64_html_owns_node(doc, node)) return false;
    int64_t began = os64_micros();
    uint64_t before = geometry_layout_total;
    int32_t width = g.view.bounds.w > 0 ? g.view.bounds.w : 1;
    int32_t height = g.view.bounds.h > 0 ? g.view.bounds.h : 1;
    bool ready = began >= 0;
    if (doc != page_doc(&g.page)) {
        ready = ready && measured_for(doc, width, height);
        if (ready)
            ready = yonder_geometry_snapshot(doc, s_measured.tree, node, width, height, g.zoom,
                                             (flow_point_t){0, 0}, out);
    } else {
        ready = ready && script_rebuild();
        if (ready && (g.page.tree == NULL || flow_incomplete(g.page.tree) || g.page.laid_width != width ||
                      g.page.laid_height != height || g.page.laid_zoom != g.zoom ||
                      g.page.sheets_changed))
            ready = relayout(true);
        if (ready)
            ready = yonder_geometry_snapshot(doc, g.page.tree, node,
                        width, height, g.zoom, (flow_point_t){g.sx, g.sy}, out);
    }
    int64_t ended = os64_micros();
    out->layouts = geometry_layout_total - before;
    out->elapsed_us = out->layouts != 0 && began >= 0 && ended >= began ? (uint64_t)(ended - began) : 0;
    return ready && ended >= began;
}

// THE SHOWN PAGE'S TURN: at most one task, then the rendering step.
static bool script_turn(void)
{
    // A font-capacity retry may have released the old tree. Keep retrying
    // its owed DOM version before allowing another task to run.
    if (g.page.tree == NULL && (page_doc(&g.page) == NULL ||
        g.page.rendered_version == os64_html_version(page_doc(&g.page))))
        return false;
    // A failed native rebuild is retried before another script can run.
    if (!script_rebuild())
        return false;
    if (!g.scripts_on || g.page.scripts == NULL)
        return false;
    forms_flush();
    const os64_html_document_t *doc = page_doc(&g.page);
    os64_js_outcome_t out;
    int64_t began = task_begin(g.page.scripts);
    const char *what = "script";
    bool ran = yonder_scripts_step(g.page.scripts, &out);
    if (!ran) {
        what = "timer";
        ran = yonder_scripts_timer_fire(g.page.scripts, yonder_now_ms(), &out);
    }
    if (!ran)
        return false;
    task_said(g.page.scripts, true, what, &out, began);
    // The rendering step before the asks: a form a script sent is read
    // from a model as current as its tree.
    bool rendered = script_rebuild();
    if (rendered)
        asks_perform(g.page.scripts, doc, true);
    return rendered;
}

// ── Input events ────────────────────────────────────────────────────────
//
// A person's input becomes DOM events (DOM_D7.md § Input events), each one
// task, dispatched BEFORE the action it precedes, which is skipped when a
// listener cancelled it. An event that starts inside a widget's own callback
// — a button, a box, a list, a field's edit — is QUEUED with its node held
// and dispatched once libui's dispatch has returned (inputs_run): a listener
// may take away the very control whose callback is running, and the rebuild
// after it would free the widget under libui's feet. Its default action then
// runs by the door it always has, found again by node, since the rebuild may
// have renumbered the controls. Without page scripts nothing is queued and
// every action runs where it always did.

static bool scripts_live(void)
{
    return g.scripts_on && g.page.scripts != NULL && yonder_scripts_alive(g.page.scripts);
}

static void input_queue(InputKind kind, const os64_html_node_t *node, const os64_html_node_t *related,
                        int32_t x, int32_t y, bool was)
{
    const os64_html_document_t *doc = page_doc(&g.page);
    if (node == NULL || doc == NULL)
        return;
    // One pointer move waits at a time: a page that listens to mousemove is
    // told where the pointer is, not every sample on the way there.
    for (int32_t i = 0; kind == IN_MOVE && i < g.ninputs; i++)
        if (g.inputs[i].kind == IN_MOVE && g.inputs[i].node == node) {
            g.inputs[i].x = x;
            g.inputs[i].y = y;
            return;
        }
    if (g.ninputs == INPUTS_MAX)
        return;
    os64_html_hold(doc, node);
    if (related != NULL)
        os64_html_hold(doc, related);
    g.inputs[g.ninputs++] = (Input){kind, node, related, doc, x, y, was};
}

static void input_release(const Input *in)
{
    os64_html_release(in->doc, in->node);
    if (in->related != NULL)
        os64_html_release(in->doc, in->related);
}

// The page is being replaced: what waits for it, and what the pointer and
// the focus were resting on in it, are let go.
static void inputs_drop(void)
{
    for (int32_t i = 0; i < g.ninputs; i++)
        input_release(&g.inputs[i]);
    g.ninputs = 0;
    if (g.hover_node != NULL)
        os64_html_release(g.pointer_doc, g.hover_node);
    if (g.pressed_node != NULL)
        os64_html_release(g.pointer_doc, g.pressed_node);
    if (g.focus_node != NULL)
        os64_html_release(g.focus_doc, g.focus_node);
    g.hover_node = g.pressed_node = g.focus_node = NULL;
    g.focus_seen = NULL;
}

// The element an event at `node` is dispatched at: a hit on a word is a hit
// on the element holding it.
static const os64_html_node_t *element_of(const os64_html_node_t *node)
{
    while (node != NULL && node->kind != OS64_HTML_ELEMENT)
        node = node->parent;
    return node;
}

// The element under a window position, and the link it is in, if any.
static const os64_html_node_t *element_at(int32_t x, int32_t y, const os64_html_node_t **link)
{
    os64_gui_rect_t v = g.view.bounds;
    if (link != NULL)
        *link = NULL;
    if (g.page.tree == NULL || x < v.x || y < v.y || x >= v.x + v.w || y >= v.y + v.h)
        return NULL;
    const flow_box_t *b = flow_hit(g.page.tree, x - v.x + g.sx, y - v.y + g.sy, scroll_now());
    if (b == NULL)
        return NULL;
    flow_point_t offset = flow_box_doc_offset(b, scroll_now());
    int64_t px = (int64_t)x - v.x + g.sx - offset.x - b->content.x;
    int64_t py = (int64_t)y - v.y + g.sy - offset.y - b->content.y;
    if (px >= 0 && py >= 0 && px < b->content.w && py < b->content.h) {
        double scale = 1000.0 / (g.zoom != 0 ? g.zoom : 1000);
        const os64_html_node_t *area = yonder_image_map_hit(page_doc(&g.page), b->node, px*scale, py*scale);
        if (area != NULL) {
            if (link != NULL && os64_html_attr(area, "href") != NULL) *link = area;
            return area;
        }
    }
    if (link != NULL) {
        const os64_page_link_t *l = os64_page_link(flow_model(g.page.tree), b->link);
        *link = l != NULL ? l->node : NULL;
    }
    return element_of(b->node);
}

static os64_dom_event_t mouse_event(const char *type, bool cancelable, const Input *in)
{
    os64_dom_event_t ev = {.type = type, .bubbles = true, .cancelable = cancelable,
                           .kind = OS64_DOM_EVENT_MOUSE};
    uint32_t zoom = g.zoom != 0 ? g.zoom : 1000;
    ev.client_x = clamp32(((int64_t)in->x - g.view.bounds.x)*1000/zoom);
    ev.client_y = clamp32(((int64_t)in->y - g.view.bounds.y)*1000/zoom);
    ev.page_coordinates = true;
    ev.page_x = clamp32(((int64_t)in->x - g.view.bounds.x + g.sx)*1000/zoom);
    ev.page_y = clamp32(((int64_t)in->y - g.view.bounds.y + g.sy)*1000/zoom);
    ev.screen_x = in->x;
    ev.screen_y = in->y;
    return ev;
}

// One event at `node` of the page on screen, as one task: the page's edits
// flushed first so its script reads what was typed, then the event, then
// the rendering step and the asks the task made. True when a listener
// cancelled the default. Nothing at all when nothing could hear it.
static bool page_event(const os64_dom_event_t *ev, const os64_html_node_t *node)
{
    yonder_scripts_t *host = g.page.scripts;
    if (!scripts_live() || !yonder_scripts_listens(host, ev->type))
        return false;
    const os64_html_document_t *doc = page_doc(&g.page);
    if (node != NULL && !os64_html_owns_node(doc, node))
        return false;
    forms_flush();
    os64_js_outcome_t out;
    bool prevented = false;
    int64_t began = task_begin(host);
    yonder_scripts_dispatch(host, node, ev, &prevented, &out);
    task_said(host, true, ev->type, &out, began);
    if (script_rebuild())
        asks_perform(host, doc, true);
    return prevented;
}

static bool plain_event(const char *type, bool bubbles, bool cancelable, const os64_html_node_t *node)
{
    os64_dom_event_t ev = {.type = type, .bubbles = bubbles, .cancelable = cancelable};
    return page_event(&ev, node);
}

// The form node a control belongs to, for its submit and reset events.
static const os64_html_node_t *control_form(int32_t control)
{
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), control);
    const os64_page_form_t *f = c != NULL ? os64_page_form(page_model(&g.page), c->form) : NULL;
    return f != NULL ? f->node : NULL;
}

// A text control's value as its editor holds it, for `change`.
static bool editor_text(const os64_html_node_t *node, const char **text, size_t *len)
{
    int32_t control = os64_page_control_for(page_model(&g.page), node);
    FormWidget *fw = control >= 0 ? form_widget(control) : NULL;
    if (fw == NULL || (fw->kind != FW_TEXT && fw->kind != FW_PASSWORD))
        return false;
    *text = fw->kind == FW_PASSWORD ? fw->secret : fw->text;
    *len = fw->kind == FW_PASSWORD ? fw->secret_len : os64_strlen(fw->text);
    return true;
}

// HTML's `change` for a text control: its value now differs from the one
// it had when it got focus (or when `change` last fired).
static void change_if_edited(const os64_html_node_t *node)
{
    const char *text;
    size_t len;
    if (node != g.focus_node || !editor_text(node, &text, &len) ||
        (len == g.focus_len && os64_memcmp(text, g.focus_value, len) == 0))
        return;
    g.focus_len = len < sizeof(g.focus_value) ? len : sizeof(g.focus_value) - 1;
    os64_memcpy(g.focus_value, text, g.focus_len);
    plain_event("change", true, false, node);
}

// The focus moved between libui's widgets: `blur` and `change` for the
// control it left, `focus` for the one it reached (neither bubbles).
static void focus_follow(void)
{
    if (g.ui.focus == g.focus_seen)
        return;
    g.focus_seen = g.ui.focus;
    if (g.focus_node != NULL) {
        // Judged now, while the value it had at focus is still at hand.
        const char *text;
        size_t len;
        if (editor_text(g.focus_node, &text, &len) &&
            (len != g.focus_len || os64_memcmp(text, g.focus_value, len) != 0))
            input_queue(IN_CHANGE, g.focus_node, NULL, 0, 0, false);
        input_queue(IN_BLUR, g.focus_node, NULL, 0, 0, false);
        os64_html_release(g.focus_doc, g.focus_node);
        g.focus_node = NULL;
    }
    FormWidget *fw = NULL;
    for (int32_t i = 0; i < g.nfw && fw == NULL; i++)
        if (g.fw[i]->w == g.ui.focus)
            fw = g.fw[i];
    if (fw == NULL || !scripts_live())
        return;
    g.focus_doc = page_doc(&g.page);
    g.focus_node = fw->node;
    os64_html_hold(g.focus_doc, g.focus_node);
    const char *text;
    size_t len = 0;
    if (!editor_text(fw->node, &text, &len))
        text = "";
    g.focus_len = len < sizeof(g.focus_value) ? len : sizeof(g.focus_value) - 1;
    os64_memcpy(g.focus_value, text, g.focus_len);
    input_queue(IN_FOCUS, fw->node, NULL, 0, 0, false);
}

// What waits, dispatched in order, each with its default action after it.
// A page replaced meanwhile drops its own (inputs_drop); one that is still
// on screen but changed is answered by node.
static void inputs_run(void)
{
    if (scripts_live())
        focus_follow();
    for (int32_t i = 0; i < g.ninputs; i++) {
        Input in = g.inputs[i];
        if (in.doc != page_doc(&g.page)) {
            input_release(&in);
            continue;
        }
        int32_t control = os64_page_control_for(page_model(&g.page), in.node);
        switch (in.kind) {
        case IN_MOUSEDOWN: case IN_MOUSEUP: case IN_MOVE: case IN_OVER: case IN_OUT: {
            static const char *const names[] = {"mousedown", "mouseup", "", "mouseover", "mouseout", "mousemove"};
            os64_dom_event_t ev = mouse_event(names[in.kind], in.kind != IN_MOVE, &in);
            ev.related = in.kind == IN_OVER || in.kind == IN_OUT ? in.related : NULL;
            page_event(&ev, in.node);
            break;
        }
        case IN_CLICK: {
            os64_dom_event_t ev = mouse_event("click", true, &in);
            if (page_event(&ev, in.node))
                break;
            // The listeners may have rebuilt the page: the image is found
            // again, and is gone if they took it away.
            control = os64_page_control_for(page_model(&g.page), in.node);
            int32_t link = in.related != NULL ? os64_page_link_for(page_model(&g.page), in.related) : -1;
            const os64_page_control_t *c = os64_page_control(page_model(&g.page), control);
            if (link >= 0)
                follow_link(link);
            else if (c != NULL && c->input == OS64_PAGE_INPUT_IMAGE && !c->disabled)
                form_send(control, OS64_PAGE_ACTIVATE_CONTROL);
            break;
        }
        case IN_BUTTON: {
            os64_dom_event_t ev = mouse_event("click", true, &in);
            if (page_event(&ev, in.node))
                break;
            control = os64_page_control_for(page_model(&g.page), in.node);
            const os64_page_control_t *c = os64_page_control(page_model(&g.page), control);
            if (c == NULL)
                break;
            bool resets = c->resets, submits = c->submits;
            const char *type = resets ? "reset" : "submit";
            if ((!resets && !submits) || plain_event(type, true, true, control_form(control)))
                break;
            // The listeners may have rebuilt the page: the button is found
            // again, and is gone if they took it away.
            control = os64_page_control_for(page_model(&g.page), in.node);
            c = os64_page_control(page_model(&g.page), control);
            if (c != NULL && resets)
                form_reset(c->form);
            else if (c != NULL)
                form_send(control, OS64_PAGE_ACTIVATE_CONTROL);
            break;
        }
        case IN_IMPLICIT:
            change_if_edited(in.node);
            control = os64_page_control_for(page_model(&g.page), in.node);
            if (!plain_event("submit", true, true, control_form(control)))
                form_send(os64_page_control_for(page_model(&g.page), in.node), OS64_PAGE_ACTIVATE_IMPLICIT);
            break;
        case IN_CHECK: {
            os64_dom_event_t ev = mouse_event("click", true, &in);
            if (page_event(&ev, in.node)) {
                // A cancelled click puts the box back.
                control = os64_page_control_for(page_model(&g.page), in.node);
                (void)os64_page_set_checked(page_model(&g.page), control, in.was);
                forms_sync_from_model(false);
                break;
            }
            plain_event("input", true, false, in.node);
            plain_event("change", true, false, in.node);
            break;
        }
        case IN_LIST:
            plain_event("input", true, false, in.node);
            plain_event("change", true, false, in.node);
            break;
        case IN_INPUT:
            plain_event("input", true, false, in.node);
            break;
        case IN_FOCUS:
            plain_event("focus", false, false, in.node);
            break;
        case IN_BLUR:
            plain_event("blur", false, false, in.node);
            break;
        case IN_CHANGE:
            plain_event("change", true, false, in.node);
            break;
        }
        input_release(&in);
        if (scripts_live())
            focus_follow();
    }
    g.ninputs = 0;
}

// A key, before libui sees it: keydown, and keypress for a character, at
// the control holding focus (the page's body when the view has it). A
// cancelled one is not typed. Keys in a VT100 burst (the arrows, ESC [ A)
// are libui's own and are not dispatched (DOM.md § D7b, as built: Booked).
static bool key_event(const os64_gui_event_t *ev)
{
    if (!scripts_live() || (ev->type != OS64_GUI_EVENT_KEY_DOWN && ev->type != OS64_GUI_EVENT_KEY_UP))
        return false;
    unsigned char a = (unsigned char)ev->key.ascii;
    if (ev->type == OS64_GUI_EVENT_KEY_DOWN) {
        if (a == 0x1b) {
            g.key_seq = 1;
            return false;
        }
        if (g.key_seq != 0) {
            if (g.key_seq == 1 && (a == '[' || a == 'O'))
                g.key_seq = 2;
            else if (!(g.key_seq == 2 && ((a >= '0' && a <= '9') || a == ';')))
                g.key_seq = 0;
            return false;
        }
    }
    bool printable = a >= 0x20 && a != 0x7f;
    if (!printable && a != '\r' && a != '\n' && a != '\b' && a != 0x7f && a != '\t')
        return false;
    const os64_html_node_t *target = NULL;
    for (int32_t i = 0; i < g.nfw && target == NULL; i++)
        if (g.fw[i]->w == g.ui.focus)
            target = g.fw[i]->node;
    if (target == NULL)
        target = page_doc(&g.page)->body != NULL ? page_doc(&g.page)->body : page_doc(&g.page)->document;
    char name[8];
    if (printable)
        name[os64_utf8_encode(a, name)] = '\0';
    os64_dom_event_t key = {.bubbles = true, .cancelable = true, .kind = OS64_DOM_EVENT_KEY,
                            .key = printable ? name : a == '\t' ? "Tab" : a == '\b' || a == 0x7f
                                   ? "Backspace" : "Enter",
                            .key_code = a >= 'a' && a <= 'z' ? (uint32_t)(a - 'a' + 'A')
                                      : a == '\n' ? 13 : a == 0x7f ? 8 : a,
                            .shift = (ev->key.modifiers & OS64_GUI_MOD_SHIFT) != 0,
                            .ctrl = (ev->key.modifiers & OS64_GUI_MOD_CTRL) != 0,
                            .alt = (ev->key.modifiers & OS64_GUI_MOD_ALT) != 0};
    if (ev->type == OS64_GUI_EVENT_KEY_UP) {
        key.type = "keyup";
        page_event(&key, target);
        return false;
    }
    key.type = "keydown";
    if (page_event(&key, target))
        return true;
    if (!printable)
        return false;
    key.type = "keypress";
    key.char_code = a;
    return page_event(&key, target);
}

// A page waiting for its sheets keeps running its async scripts and its
// timers, one task a turn: it is the page arriving, not yet shown.
static bool coming_turn(void)
{
    if (!g.coming.active || g.coming.page.scripts == NULL)
        return false;
    yonder_scripts_t *host = g.coming.page.scripts;
    if (!scripts_owed(host))
        return false;
    uint64_t serial = g.coming.page.serial;
    (void)scripts_task(host, page_doc(&g.coming.page), false);
    return g.coming.active && g.coming.page.serial == serial && scripts_owed(g.coming.page.scripts);
}

// ── The doorbell ────────────────────────────────────────────────────────

// A job the pool finished. A picture's or a sheet's is read here; a
// navigation's fetch said everything it had to say down the mailbox before
// it returned, so its job is only let go, and one finished before its
// cancel was noticed is let go the same way.
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
    if (*(const uint32_t *)job == YONDER_JOB_SCRIPT) {
        script_arrived(job, product);
        yonder_script_release(job, product);
        return;
    }
    // A fetch is done. Everything it had to say went down the mailbox
    // before it returned, and the stream reads that at its own pace; here
    // the job is let go.
    if (id != 0 && id == g.nav.id)
        g.nav.id = 0;
    yonder_trip_release(job, product);
}

// ── The stream ──────────────────────────────────────────────────────────
//
// The page arriving is parsed here (DOM_D4.md): the head makes the parser,
// each turn of the loop feeds it a slice of what the fetch has posted, and
// the verdict, once the ring is drained, finishes it and hands the page to
// arrive() exactly as a page from a worker used to be handed over.

// Text into the page yonder writes, escaped as an attribute's value and a
// title's text both need it.
static void feed_escaped(os64_html_parser_t *parser, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        const char *escaped = text[i] == '&' ? "&amp;" : text[i] == '"' ? "&quot;"
                            : text[i] == '<' ? "&lt;" : NULL;
        if (escaped != NULL)
            os64_html_parser_feed(parser, escaped, os64_strlen(escaped));
        else
            os64_html_parser_feed(parser, text + i, 1);
    }
}

// A picture asked for by itself is shown as a browser shows one: alone, in
// the middle of a dark page, the title its file's name (or its address,
// when the path names none). The page is written here and the picture
// fetched as any page's is: the reply's own bytes were not read.
static void image_page(os64_html_parser_t *parser, const char *url)
{
    const char *end = url;
    while (*end != '\0' && *end != '?' && *end != '#')
        end++;
    const char *name = end;
    while (name > url && name[-1] != '/')
        name--;
    static const char kHead[] = "<!doctype html><title>";
    static const char kBody[] = "</title><body style=\"margin:0;min-height:100vh;display:flex;"
                                "align-items:center;justify-content:center;background:#0e0e0e\">"
                                "<img src=\"";
    os64_html_parser_feed(parser, kHead, sizeof(kHead) - 1);
    if (name < end)
        feed_escaped(parser, name, (size_t)(end - name));
    else
        feed_escaped(parser, url, os64_strlen(url));
    os64_html_parser_feed(parser, kBody, sizeof(kBody) - 1);
    feed_escaped(parser, url, os64_strlen(url));
    os64_html_parser_feed(parser, "\">", 2);
}

// The parser for the page whose head has arrived. HTML takes the head's
// charset; text is laid out by handing libhtml `<plaintext>` and then the
// bytes — the standard's own element for "everything after this is text",
// so libflow sees preformatted text and needs nothing new — in the
// encoding libway's rule chooses from the head and the body's first bytes.
// A picture's page is yonder's own (image_page), in UTF-8.
static bool stream_parser(const void *first, size_t n)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.scripting = g.stream.scripting;
    if (g.stream.head.body == WAY_BODY_HTML) {
        opt.charset = g.stream.head.charset[0] ? g.stream.head.charset : NULL;
    } else if (g.stream.head.body == WAY_BODY_IMAGE) {
        opt.charset = "utf-8";
    } else {
        g.stream.text_utf8 = way_text_utf8(&g.stream.head, first, n);
        opt.charset = g.stream.text_utf8 ? "utf-8" : "windows-1252";
    }
    g.stream.parser = os64_html_parser_new(&opt);
    if (g.stream.parser == NULL)
        return false;
    if (g.stream.head.body == WAY_BODY_TEXT) {
        static const char kOpen[] = "<!doctype html><plaintext>";
        os64_html_parser_feed(g.stream.parser, kOpen, sizeof(kOpen) - 1);
    } else if (g.stream.head.body == WAY_BODY_IMAGE) {
        image_page(g.stream.parser, g.stream.head.url);
    }
    return true;
}

static bool stream_open(void);

// The parse is over: finish it, build what the page means — adopting the
// control state the page's scripts already changed — write its standing
// line, fire DOMContentLoaded, and arrive. `fetch` and `reason` are the
// wire's verdict, or OK and NULL when the window cut the fetch short itself.
static void stream_finish(os64_fetch_status_t fetch, const char *reason)
{
    NavKind kind = g.stream.kind;
    way_position_t crumb = g.stream.crumb;
    char fragment[sizeof(g.stream.fragment)];
    bool has_fragment = g.stream.has_fragment;
    os64_strcopy(fragment, sizeof(fragment), g.stream.fragment);
    // A body too short to have filled the sniff is judged on what there is.
    if (g.stream.parser == NULL && !stream_open()) {
        stream_failed("Out of memory reading that page.");
        stream_drop();
        buttons_follow();
        status_rest("Out of memory reading that page.");
        return;
    }
    Page fresh;
    os64_memset(&fresh, 0, sizeof(fresh));
    fresh.scripting = g.stream.scripting;
    os64_html_document_t *doc = os64_html_parser_finish(g.stream.parser);
    g.stream.parser = NULL;
    if (doc == NULL) {
        stream_failed("Out of memory reading that page.");
        stream_drop();
        buttons_follow();
        status_rest("Out of memory reading that page.");
        return;
    }
    // The scripts, their state and the document are the page's from here.
    // A scripted page that ran no script still gets its host: its handler
    // attributes, `<body onload>` first of all, are scripts too.
    if (g.stream.scripts == NULL && fresh.scripting && !g.stream.scriptless &&
        g.stream.head.body == WAY_BODY_HTML) {
        g.stream.state = os64_page_state_create(doc, 0);
        g.stream.scripts = g.stream.state != NULL
            ? scripts_host(doc, g.stream.state, g.stream.head.url, g.stream.serial, g.stream.diag) : NULL;
        if (g.stream.scripts == NULL) {
            os64_page_state_free(g.stream.state);
            g.stream.state = NULL;
        }
    }
    fresh.scripts = g.stream.scripts;
    fresh.state = g.stream.state;
    fresh.serial = g.stream.serial;
    // The record is the page's from here: stream_drop below must not end it.
    fresh.diag = g.stream.diag;
    g.stream.diag = NULL;
    {
        char bytes[32];
        os64_snprintf(bytes, sizeof(bytes), "%lu", (unsigned long)g.stream.bytes);
        yonder_diag_fact(fresh.diag, "bytes", bytes);
        if (g.stream.head.tls_version) {
            yonder_diag_fact(fresh.diag, "TLS", g.stream.head.tls_version == 0x0304 ? "1.3" :
                g.stream.head.tls_fallback ? "1.2 (fallback)" : "1.2");
        }
        // The server's answer, for every page from the network. One that is
        // an error is shown, as a browser shows it, and is a failed load
        // from the person's chair: a 429's page is not a clean page.
        if (g.stream.local == NULL && g.stream.head.status != 0) {
            char status[16 + HTTP_REASON_MAX];
            os64_snprintf(status, sizeof(status), "%d %s", (int)g.stream.head.status,
                          g.stream.head.reason);
            yonder_diag_fact(fresh.diag, "status", status);
            if (g.stream.head.status >= 400) {
                os64_snprintf(status, sizeof(status), "HTTP %d %s", (int)g.stream.head.status,
                              g.stream.head.reason);
                yonder_diag_failed(fresh.diag, "page", status);
            }
        }
    }
    // The sheets the stream sent for move with it, still out or landed,
    // and keep their jobs, which name this serial: no sheet is fetched
    // twice in one page's life (arrive adds the rest).
    Page *found = &g.stream.page;
    fresh.sheets = found->sheets;
    fresh.nsheets = found->nsheets;
    fresh.sheets_waiting = found->sheets_waiting;
    fresh.sheets_ready = found->sheets_ready;
    fresh.sheets_changed = found->sheets_changed;
    found->sheets = NULL;
    found->nsheets = found->sheets_waiting = found->sheets_ready = 0;
    g.stream.scripts = NULL;
    g.stream.state = NULL;
    // WHAT THE PAGE MEANS, from the tree and the address it came from —
    // `<base href>` included, which libpage reads. A page it cannot build
    // a model of is still a page worth reading: what it costs is the links
    // and boxes, and the status line says so. A text page's tree is the
    // window's own, beside libway's page, as page_doc() reads it.
    os64_page_t *model = os64_page_build(doc, g.stream.head.url, NULL, fresh.state);
    fresh.model_version = os64_html_version(doc);
    // A picture's page is a page like an HTML one (image_page): its picture
    // is fetched and its record kept as any page's are.
    if (g.stream.head.body == WAY_BODY_HTML || g.stream.head.body == WAY_BODY_IMAGE) {
        fresh.way.doc = doc;
        fresh.way.model = model;
        if (g.stream.local != NULL)
            os64_strcopy(fresh.way.url, sizeof(fresh.way.url), g.stream.head.url);
        else
            way_note(&fresh.way, &g.stream.head, false, fetch, reason);
    } else {
        fresh.plain = doc;
        fresh.plain_model = model;
        fresh.way.text_utf8 = g.stream.text_utf8;
        // The standing line reads the tree's refusal from the page it is
        // given, so the text's tree stands in for the moment it is written.
        way_page_t noted = fresh.way;
        noted.doc = doc;
        way_note(&noted, &g.stream.head, false, fetch, reason);
        os64_strcopy(fresh.way.url, sizeof(fresh.way.url), noted.url);
        os64_strcopy(fresh.way.note, sizeof(fresh.way.note), noted.note);
        fresh.way.posted = noted.posted;
    }
    if (fresh.way.posted && g.stream.has_sent) {
        fresh.sent = g.stream.sent;     // moved: stream_drop frees nothing
        g.stream.has_sent = false;
        os64_memset(&g.stream.sent, 0, sizeof(g.stream.sent));
    }
    bool text = g.stream.head.body == WAY_BODY_TEXT;
    stream_drop();
    buttons_follow();
    if (model == NULL && (text || fresh.state != NULL)) {
        // A page whose scripts share the state needs the model that adopts
        // it; without one it is a text that could not be read.
        yonder_diag_failed(fresh.diag, "page",
                           text ? "Out of memory reading that text." : "Out of memory reading that page.");
        sheets_leave(&fresh);
        page_clear(&fresh);
        status_rest(text ? "Out of memory reading that text." : "Out of memory reading that page.");
        return;
    }
    // DOMContentLoaded: the parse and the deferred scripts are done (DOM_D7.md
    // § The stream's turn, step 6). A navigation it asks for replaces the
    // page before it is ever shown.
    if (fresh.scripts != NULL) {
        os64_dom_event_t loaded = {.type = "DOMContentLoaded", .bubbles = true};
        os64_js_outcome_t out;
        int64_t began = task_begin(fresh.scripts);
        g.finishing.page = &fresh;
        g.finishing.scripts = fresh.scripts;
        g.finishing.url = fresh.way.url;
        g.finishing.fragment = fragment;
        g.finishing.cap = sizeof(fragment);
        g.finishing.has_fragment = &has_fragment;
        yonder_scripts_dispatch(fresh.scripts, doc->document, &loaded, NULL, &out);
        task_said(fresh.scripts, true, "DOMContentLoaded", &out, began);
        asks_perform(fresh.scripts, doc, false);
        os64_memset(&g.finishing, 0, sizeof(g.finishing));
        if (g.stream.active) {
            sheets_leave(&fresh);
            page_clear(&fresh);
            return;
        }
    }
    arrive(&fresh, kind, &crumb, has_fragment ? fragment : NULL);
}

// The parser for a text body, made from what decides its encoding — the
// sniffed first bytes when the head left it to them (way_text_sniffs), as
// many as arrived — which then go to the parser, so the caller feeds only
// what follows them. False on no memory.
static bool stream_open(void)
{
    if (!stream_parser(g.stream.sniff, g.stream.sniff_len))
        return false;
    // Three bytes after <plaintext> cannot stop at a script or refuse.
    if (g.stream.sniff_len != 0)
        (void)os64_html_parser_feed(g.stream.parser, g.stream.sniff, g.stream.sniff_len);
    g.stream.sniff_len = 0;
    return true;
}

// The bytes the arriving page has to give: the mailbox's chunks, or the
// file's, one chunk at a time.
static size_t stream_take(uint8_t *chunk, size_t cap)
{
    size_t n;
    if (g.stream.local == NULL) {
        n = yonder_mail_take(g.stream.mail, chunk, cap);
    } else {
        n = g.stream.local_len - g.stream.local_at;
        if (n > cap)
            n = cap;
        os64_memcpy(chunk, g.stream.local + g.stream.local_at, n);
        g.stream.local_at += n;
    }
    g.stream.bytes += n;
    return n;
}

static bool stream_streaming(void)
{
    return g.stream.local != NULL ? g.stream.local_at < g.stream.local_len
                                  : yonder_mail_streaming(g.stream.mail);
}

// The parser stopped at `script`: the page's host decides (scripts.h). Its
// host is made at the first script that runs, with the control state the
// model will adopt when the parse is over.
static yonder_stop_t stream_stop(os64_html_node_t *script)
{
    // A module is recognised and never run: what a scripted page asked for
    // and did not get. With scripts off nothing runs, and that is a setting.
    if (g.stream.scripting && os64_dom_script_kind(script) == OS64_DOM_SCRIPT_MODULE)
        yonder_diag_missing(g.stream.diag, "script", "module", 1);
    if (!g.stream.scripting || g.stream.scriptless ||
        os64_dom_script_kind(script) != OS64_DOM_SCRIPT_CLASSIC)
        return YONDER_STOP_RESUME;
    if (g.stream.scripts == NULL) {
        os64_html_document_t *doc = os64_html_parser_document(g.stream.parser);
        g.stream.state = os64_page_state_create(doc, 0);
        g.stream.scripts = g.stream.state != NULL
            ? scripts_host(doc, g.stream.state, g.stream.head.url, g.stream.serial, g.stream.diag) : NULL;
        if (g.stream.scripts == NULL) {
            os64_page_state_free(g.stream.state);
            g.stream.state = NULL;
            g.stream.scriptless = true;
            yonder_diag_failed(g.stream.diag, "scripts", "Out of memory for the page's scripts; it is read without them.");
            status_rest("Out of memory for the page's scripts; it is read without them.");
            return YONDER_STOP_RESUME;
        }
    }
    return yonder_scripts_parser_stop(g.stream.scripts, script);
}

// A parser call's answer, acted on: a stop at a script the page does not
// run is resumed at once, as D4 resumed every stop; one it runs leaves the
// stream STOPPED, for this turn or a later one to run.
static int64_t stream_parsed(int64_t r)
{
    while (r == OS64_HTML_SCRIPT) {
        if (stream_stop(os64_html_parser_script(g.stream.parser)) != YONDER_STOP_RESUME) {
            g.stream.stopped = true;
            stream_sheets();
            return r;
        }
        r = os64_html_parser_resume(g.stream.parser);
    }
    return r;
}

// The parser refused — the page is bigger or deeper than this browser
// parses — and what it built is a page you can read the beginning of. The
// rest of the body is not wanted: the fetch is cancelled, as way_load
// closes its fetch at the same point, and the page arrives with the
// parser's sentence. Its deferred scripts do not run: the parse did not end.
static void stream_cut_short(void)
{
    stream_failed("the page could not be read to its end; what was read is shown");
    if (g.nav.id != 0 && g.pool != NULL)
        os64_work_cancel(g.pool, g.nav.id);
    g.nav.id = 0;
    stream_finish(OS64_FETCH_OK, NULL);
}

// A script a verb connected with no `src`, as its own task: a browser runs
// it inside the verb, so it runs before the parse goes on. False when none
// ran, or when it navigated and dropped this stream.
static bool stream_connected(void)
{
    yonder_scripts_t *host = g.stream.scripts;
    uint64_t serial = g.stream.serial;
    os64_js_outcome_t out;
    int64_t began = task_begin(host);
    if (!yonder_scripts_step_connected(host, &out))
        return false;
    task_said(host, true, "script", &out, began);
    asks_perform(host, os64_html_parser_document(g.stream.parser), false);
    return g.stream.active && g.stream.serial == serial;
}

// The parse is over: the deferred scripts may run, and what is due by now
// runs before DOMContentLoaded, which HTML queues behind it.
static void stream_parse_over(void)
{
    g.stream.ended = true;
    g.stream.ended_ms = yonder_now_ms();
    stream_sheets();
    yonder_scripts_parse_ended(g.stream.scripts);
}

// A sheet the parse has revealed, unless it is the `style` whose text is
// still arriving: a slice can end inside one, and its text so far would be
// parsed as the whole sheet. A later look takes it whole; it is never open
// at a stop or at the end.
static bool stream_sheet_add(void *page, const os64_page_sheet_t *one)
{
    if (one->node == os64_html_parser_open_text(g.stream.parser))
        return true;
    return sheet_add(page, one);
}

// The stream's page, with the stream's document, address and serial.
static Page *stream_page(void)
{
    Page *p = &g.stream.page;
    p->way.doc = g.stream.parser != NULL ? os64_html_parser_document(g.stream.parser) : NULL;
    os64_strcopy(p->way.url, sizeof(p->way.url), g.stream.head.url);
    p->serial = g.stream.serial;
    p->scripting = g.stream.scripting;
    return p;
}

// THE SHEETS THE PARSE HAS REVEALED, sent for as it finds them (DOM_D7.md
// § D7d): after every slice, at a stop that blocks and at the end, every
// `link` and `style` element the tree holds that the stream's page has no
// entry for, by libpage's own rule and order (os64_page_sheets_in, a walk
// of the tree, which costs a fraction of a model). A `style` element is
// parsed now; a `link` is fetched under the stream's serial. A walk that
// had no memory leaves them to the next look.
static void stream_sheets(void)
{
    if (g.stream.parser == NULL || g.stream.head.body != WAY_BODY_HTML)
        return;
    Page *p = stream_page();
    int64_t began = s_script_audit ? os64_micros() : 0;
    int32_t had = p->nsheets;
    int32_t listed = os64_page_sheets_in(page_doc(p), g.stream.head.url, stream_sheet_add, p);
    if (s_script_audit) {
        char line[128];
        os64_snprintf(line, sizeof(line), "yonder: stream sheets: %d listed, %d new, %d out, in %ld us",
                      (int)listed, (int)(p->nsheets - had), (int)p->sheets_waiting,
                      (long)(os64_micros() - began));
        os64_debug_log(line);
    }
}

// Whether the script the parse is stopped at waits for the sheets still
// out (DOM_D7.md § D7d): a script may measure, and one that ran before
// them would read a layout they will change. Only a sheet whose media
// holds on this glass is waited for, as the first paint waits; one whose
// fetch failed holds nothing. The wait lasts SHEETS_WAIT_MS at most, on
// the ticker; the script then runs with what came, and the sheets still
// out have had their time: they lapse, and hold no later script and not
// the first paint. A sheet found after that starts a wait of its own.
static bool stream_sheets_hold(void)
{
    Page *p = &g.stream.page;
    if (p->sheets_waiting == 0)
        return false;
    uint64_t now = yonder_now_ms();
    if (g.stream.sheets_due == 0) {
        g.stream.sheets_due = now + SHEETS_WAIT_MS;
        pictures_schedule();
    }
    if (now < g.stream.sheets_due)
        return true;
    for (int32_t e = 0; e < p->nsheets; e++)
        if (p->sheets[e].waiting) {
            p->sheets[e].lapsed = true;
            p->sheets[e].holds = false;
        }
    p->sheets_waiting = 0;
    return false;
}

// One task of the arriving page's own: a ready script or a due timer. The
// task may navigate, which drops this stream; false then.
static bool stream_task(void)
{
    uint64_t serial = g.stream.serial;
    (void)scripts_task(g.stream.scripts, os64_html_parser_document(g.stream.parser), false);
    return g.stream.active && g.stream.serial == serial;
}

// ONE TURN OF THE ARRIVING PAGE (DOM_D7.md § The stream's turn): a script
// a verb connected with no `src` (a browser runs it inside the verb), else
// the script the parse is stopped at when its source is in hand, else one slice
// of parsing, else, when the parse is waiting (a `src` out, a deferred
// fetch out, or the mailbox empty), a ready script or a due timer. The
// parse comes first while it has input, as in a browser, where a
// parser-blocking script runs inside the parser's own task and a 0 ms timer
// it set waits for the parser to yield. At most one task a turn, so a page
// of a hundred small scripts still lets the window read its events a
// hundred times. True when the turn should come straight back; a wait for a
// fetch rings no bell, the pool's reap does.
static bool stream_turn(void)
{
    if (!g.stream.active)
        return false;
    yonder_mail_t *mail = g.stream.mail;
    if (mail != NULL && !g.stream.has_head && yonder_mail_take_head(mail, &g.stream.head)) {
        g.stream.has_head = true;
        if (g.stream.head.body != WAY_BODY_TEXT && !stream_parser(NULL, 0)) {
            stream_failed("Out of memory reading that page.");
            stop_trip();
            status_rest("Out of memory reading that page.");
            return false;
        }
    }
    if (mail != NULL && !g.stream.has_verdict && yonder_mail_take_verdict(mail, &g.stream.verdict))
        g.stream.has_verdict = true;
    yonder_scripts_t *host = g.stream.scripts;
    uint64_t serial = g.stream.serial;
    if (yonder_scripts_connected_pending(host))
        return stream_connected();
    if (g.stream.stopped) {
        // The script the parse waits for: run it when its source is in
        // hand (a dead runtime runs nothing), then carry on. The resume is
        // the parser carrying on, not a task; the next script it meets
        // waits for the next turn. While its `src` is out, a ready script
        // (async, or one a script connected) or a due timer runs: HTML's
        // loop runs other tasks while a parser-blocking script is fetched.
        if (yonder_scripts_alive(host) && !yonder_scripts_blocking_ready(host))
            return scripts_owed(host) ? stream_task() : false;
        // Its sheets the same way: their reap or the ticker carries the
        // turn on, and other tasks run meanwhile.
        if (yonder_scripts_alive(host) && stream_sheets_hold())
            return scripts_owed(host) ? stream_task() : false;
        g.stream.sheets_due = 0;
        os64_js_outcome_t out;
        int64_t began = task_begin(host);
        if (yonder_scripts_run_blocking(host, &out)) {
            task_said(host, true, "script", &out, began);
            asks_perform(host, os64_html_parser_document(g.stream.parser), false);
            if (!g.stream.active || g.stream.serial != serial)
                return false;
        }
        g.stream.stopped = false;
        int64_t r = stream_parsed(os64_html_parser_resume(g.stream.parser));
        if (r != OS64_HTML_OK && r != OS64_HTML_SCRIPT) {
            stream_cut_short();
            return false;
        }
        // A stop after the input ended was the end's own: an OK now is the
        // end of the parse.
        if (r == OS64_HTML_OK && g.stream.ending)
            stream_parse_over();
        return !g.stream.stopped || yonder_scripts_blocking_ready(host) ||
               yonder_scripts_connected_pending(g.stream.scripts);
    }
    size_t fed = 0;
    int64_t began = s_script_audit ? os64_micros() : 0;
    uint8_t chunk[YONDER_STREAM_CHUNK];
    while (g.stream.has_head && !g.stream.ending && fed < STREAM_SLICE_BYTES) {
        size_t n = stream_take(chunk, sizeof(chunk));
        if (n == 0)
            break;
        fed += n;
        size_t at = 0;
        if (g.stream.parser == NULL) {
            // A text body's encoding is read off its first three bytes when
            // the head left it to them: they are gathered whole first,
            // however the wire cut them, and go to the parser the moment it
            // exists. A body shorter than that is judged at the end.
            if (way_text_sniffs(&g.stream.head)) {
                while (g.stream.sniff_len < sizeof(g.stream.sniff) && at < n)
                    g.stream.sniff[g.stream.sniff_len++] = chunk[at++];
                if (g.stream.sniff_len < sizeof(g.stream.sniff))
                    continue;
            }
            if (!stream_open()) {
                stream_failed("Out of memory reading that page.");
                stop_trip();
                status_rest("Out of memory reading that page.");
                return false;
            }
        }
        int64_t r = stream_parsed(os64_html_parser_feed(g.stream.parser, chunk + at, n - at));
        if (r == OS64_HTML_SCRIPT)
            break;                      // the slice ends at a script the page runs
        if (r != OS64_HTML_OK) {
            stream_cut_short();
            return false;
        }
    }
    // The developer audit says what a slice cost: the number the byte
    // budget is held to (DOM_D4.md).
    if (s_script_audit && fed != 0) {
        char line[96];
        os64_snprintf(line, sizeof(line), "yonder: stream slice %lu bytes in %ld us",
                      (unsigned long)fed, (long)(os64_micros() - began));
        os64_debug_log(line);
    }
    // What the slice revealed is sent for now, the body still coming: a
    // stop looked already (stream_parsed).
    if (fed != 0 && !g.stream.stopped)
        stream_sheets();
    if (g.stream.stopped)
        return yonder_scripts_blocking_ready(g.stream.scripts) ||
               yonder_scripts_connected_pending(g.stream.scripts);
    // A slice that fed something was this turn's work; a task waits for a
    // turn that finds the parse with nothing to do.
    host = g.stream.scripts;            // the slice may have made it
    bool owed = host != NULL && scripts_owed(host);
    // A slice that ended at its budget may have left chunks in the ring
    // with no bell to come for them: the worker rang when it posted, and a
    // quiet socket rings nothing more. The verdict is waited for; the
    // chunks are not.
    if (!g.stream.has_verdict) {
        if (fed == 0)
            return owed ? stream_task() : false;
        return owed || fed >= STREAM_SLICE_BYTES;   // a task, or chunks, wait for the next turn
    }
    if (stream_streaming())
        return true;                    // chunks remain: next turn
    if (!g.stream.verdict.page) {
        // There never was a page: a head that did not come, or one that was
        // not a page. libway's sentence says which.
        char line[WAY_SENTENCE_MAX];
        os64_strcopy(line, sizeof(line), g.stream.verdict.reason);
        stream_failed(trim(line));
        stream_drop();
        buttons_follow();
        status_rest(trim(line));
        return false;
    }
    if (!g.stream.has_head)
        return true;                    // the head is behind the verdict in the mail: next turn
    if (!g.stream.ending) {
        // The input has ended. `end` may stop at a script still in the hold.
        // A text body too short to have filled the sniff is judged on what
        // there is.
        if (g.stream.parser == NULL && !stream_open()) {
            stream_failed("Out of memory reading that page.");
            stop_trip();
            status_rest("Out of memory reading that page.");
            return false;
        }
        g.stream.ending = true;
        int64_t r = stream_parsed(os64_html_parser_end(g.stream.parser));
        if (r == OS64_HTML_SCRIPT)
            return yonder_scripts_blocking_ready(g.stream.scripts) ||
                   yonder_scripts_connected_pending(g.stream.scripts);
        if (r != OS64_HTML_OK) {
            stream_cut_short();
            return false;
        }
        stream_parse_over();
        fed = 1;                        // ending the input was this turn's work
    }
    // The deferred scripts, one a turn, each when its source is in hand; a
    // turn that parsed comes back for its task.
    if (yonder_scripts_deferring(g.stream.scripts)) {
        if (scripts_owed(g.stream.scripts))
            return fed != 0 || stream_task();
        return false;                   // a deferred script's fetch is out
    }
    // A ready script, and a timer that was due when the parse ended, run
    // before DOMContentLoaded. A timer set later waits for the page to
    // arrive, so a chain of 0 ms timers cannot hold the page back.
    host = g.stream.scripts;
    if (host != NULL && (yonder_scripts_pending(host) ||
                         yonder_scripts_timer_next(host) <= g.stream.ended_ms))
        return fed != 0 || stream_task();
    yonder_verdict_t verdict = g.stream.verdict;
    stream_finish(verdict.fetch, verdict.reason);
    return false;
}

static void on_doorbell(os64_ui_t *ui, const os64_gui_event_t *ev)
{
    (void)ui;
    uint32_t mask = ev->doorbell.mask;
    if ((mask & BELL_MAIL) && g.stream.mail != NULL &&
        yonder_mail_generation(g.stream.mail) == g.generation) {
        char line[WAY_SENTENCE_MAX];
        uint32_t number = 0;
        if (yonder_mail_take_progress(g.stream.mail, line, sizeof(line)))
            status_rest(line);
        if (yonder_mail_take_question(g.stream.mail, &number, line, sizeof(line)))
            bar_ask(ASK_WORKER, number, line);
        // The head, the body and the verdict are the stream's: the loop
        // feeds a slice after the events (stream_turn, in main).
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
            if (g.stream.active)
                stream_drop();
            g.nav.id = 0;
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
// attribute's natural size at the page zoom, from (ox, oy).
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
    // A picture's own size is in CSS pixels, so at the page's zoom it is
    // that many device pixels times the zoom, as every CSS length is: an
    // `auto` size and an attribute's tile are its size zoomed, and the
    // tiler scales the picture to it.
    uint32_t zw = zoomed_px(w, p->laid_zoom), zh = zoomed_px(h, p->laid_zoom);
    // A sheet's picture is sized and placed in the origin box the painter
    // names; an attribute's is tiled from (ox, oy) at its zoomed size.
    yonder_tile_t tile = {{ox, oy, (int32_t)zw, (int32_t)zh}, true, true};
    if (layer >= 0 && !yonder_background_tile(&l, origin, zw, zh, &tile))
        return true;
    os64_gui_rect_t on = {area->x + gl->dx, area->y + gl->dy, area->w, area->h};
    os64_gui_rect_t cut = on_glass(gl, clip);
    tile.at.x += gl->dx;
    tile.at.y += gl->dy;
    if (cut.w > 0 && cut.h > 0)
        yonder_tile_picture(gl->surf->pixels, gl->surf->pitch_px, cut, on, tile.at, tile.repeat_x,
                            tile.repeat_y, !b->style->pixelated, px, w, h);
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
    if (i >= 0 && i < p->nimage_map && p->pic_of[i] >= 0 && p->pics[p->pic_of[i]].state == PIC_SHOWN) {
        uint32_t w, h;
        const uint32_t *px = picture_pixels(&p->pics[p->pic_of[i]], &w, &h);
        os64_gui_rect_t box = {c.x + gl->dx, c.y + gl->dy, c.w, c.h};
        yonder_draw_picture(gl->surf->pixels, gl->surf->pitch_px, cut, box, !b->style->pixelated,
                            px, w, h);
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
    const FormWidget *fw = form_widget(os64_page_control_for(page_model(&g.page), b->node));
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

// A frame's stand-in: a pale panel, and along its top where the frame's
// document is ("Frame: https://..."), cut to the panel. The run is made for
// this paint and let go after it, so no tree's replacement can leave one
// behind; a page has few frames.
#define FRAME_LABEL_PX 13
#define FRAME_LABEL_MAX 512
static void glass_frame(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip,
                        uint32_t ink, uint32_t paper)
{
    Glass cut = *(const Glass *)ctx;
    os64_gui_rect_t room;
    if (!os64_rect_intersect(c, clip, &room))
        return;
    cut.clip = on_glass(&cut, room);
    if (cut.clip.w <= 0 || cut.clip.h <= 0)
        return;
    glass_fill(&cut, c, paper);
    const os64_page_link_t *l = os64_page_link(flow_model(g.page.tree), b->link);
    char text[FRAME_LABEL_MAX];
    if (l != NULL && l->href.url != NULL)
        os64_snprintf(text, sizeof(text), "Frame: %s", l->href.url);
    else
        os64_strcopy(text, sizeof(text), "Frame (it names no document)");
    const flow_family_list_t sans = {NULL, 0, FLOW_GENERIC_SANS};
    os64_text_font_t *const *fonts;
    size_t nfonts;
    os64_font_face_info_t face;
    uint32_t px = (uint32_t)((int64_t)FRAME_LABEL_PX * (g.zoom != 0 ? g.zoom : 1000) / 1000);
    if (s_env.text == NULL || page_fonts(NULL, &sans, false, false, px, &fonts, &nfonts, &face) != OS64_FONT_OK)
        return;
    os64_text_layout_t opt = {.encoding = OS64_TEXT_UTF8_WESTERN_V1, .fonts = fonts,
                              .font_count = nfonts, .tab_interval = 64 * 8};
    os64_text_run_t *run = NULL;
    if (os64_text_layout(s_env.text, (const uint8_t *)text, os64_strlen(text), &opt, &run) != OS64_FONT_OK)
        return;
    int32_t pad = (int32_t)(px / 2);
    int32_t baseline = c.y + pad + (int32_t)((face.ascent + 63) / 64);
    os64_text_draw_alpha(run, cut.surf, cut.clip, c.x + pad + cut.dx, baseline + cut.dy,
                         0xff000000u | ink, flow_alpha(ink));
    os64_text_run_release(run);
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
        os64_draw_fill_rect(&ctx->surf, part, 0xff000000u | (s_dark ? kDark.paper : PAGE_PAPER));
        return;
    }
    yonder_verbs_t v = {&gl,           glass_fill,       glass_text,       glass_image,
                        glass_control, glass_backdrop,   glass_group_open, glass_group_close,
                        glass_mask,    glass_pixels,     glass_frame};
    os64_gui_rect_t view = {part.x - gl.dx, part.y - gl.dy, part.w, part.h};
    yonder_paint(g.page.tree, view, scroll_now(), PAGE_PAPER, s_dark ? &kDark : NULL, &v);
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

// The zooms Ctrl+= and Ctrl+- step through, in thousandths: Chrome's, so a
// hand that knows one knows the other.
static const uint32_t kZoomSteps[] = {250,  330,  500,  670,  750,  800,  900,  1000, 1100,
                                      1250, 1500, 1750, 2000, 2500, 3000, 4000, 5000};

// The page's form controls draw in the Web face (fonts.conf's `web.face`)
// at the size browsers give a control — 13/16 of the page's default size,
// 13 px at 16 (Chrome's small-control) — at the zoom in force, lent to the
// window so only those widgets wear it. A face that will not open leaves
// them in the Interface font, and says so once. yonder keeps its own
// reference to the set it lent, so the one before can be lent back.
static os64_font_set_t *s_controls_set;
static uint32_t s_controls_px;
static bool s_controls_refused;

// Lends the controls the Web face for `zoom`. False when the face they wear
// does not change — that size's face is lent already, or it could not be
// opened and they are in the Interface font already; true, with the face it
// replaced and that face's size in *was and *was_px (yonder's reference, now
// the caller's), when it changed. A size whose face could not be lent is not
// one they wear, so the next layout asks again: a refusal for want of memory
// passes.
static bool controls_face_at(uint32_t zoom, os64_font_set_t **was, uint32_t *was_px)
{
    uint32_t px = (uint32_t)(((uint64_t)(uint32_t)s_env.viewport_font_px * 13 * zoom + 8000) /
                             16000);
    if (px == s_controls_px && s_controls_set != NULL)
        return false;
    os64_text_context_t *text = os64_ui_font_context(&g.ui);
    os64_font_set_t *set = NULL;
    os64_font_config_error_t error = {0};
    os64_font_config_status_t st =
        text != NULL ? os64_font_config_web_prepare(text, &s_fonts_conf, px, &set, &error)
                     : OS64_FONT_CONFIG_NO_MEMORY;
    // A lend allocates nothing (os64_ui_font_app); one that is refused all
    // the same leaves the controls in the Interface font.
    if (st == OS64_FONT_CONFIG_OK && os64_ui_font_app(&g.ui, set, OS64_FONT_ROLE_UI) != OS64_FONT_OK) {
        os64_font_set_release(set);
        set = NULL;
        st = OS64_FONT_CONFIG_LIMIT;
    }
    if (set == NULL) {
        if (!s_controls_refused)
            os64_printf("yonder: the Web face (%s) could not be used (%s); form controls are "
                        "drawn in the Interface font\n",
                        s_fonts_conf.roles[OS64_FONT_CONFIG_WEB].face[0],
                        os64_font_config_status_name(st));
        s_controls_refused = true;
        // Already in the Interface font: a lend would only throw away what
        // the controls have laid out in it.
        if (s_controls_set == NULL)
            return false;
        (void)os64_ui_font_app(&g.ui, NULL, OS64_FONT_ROLE_UI);
    }
    *was = s_controls_set;
    *was_px = s_controls_px;
    s_controls_set = set;
    s_controls_px = px;
    return true;
}

// Lays a page out at the zoom in force, its controls' face with it: the face
// for the zoom is lent first, because the layout measures the controls in it
// (select_size), and lent back as it was when the layout fails. Geometry
// kept on screen stays at its zoom, its controls in the face they wore. The lend
// allocates nothing, so the way back cannot fail; but a lend lets go of the
// controls' runs, so they are laid out again at their next paint, which
// under the same shortage can draw them empty (BROWSER_DEBTS.md).
static bool lay_out_page_reclaim(Page *p, int32_t width, int32_t height, Page *reclaim)
{
    // An owed DOM version goes through the staged rebuild, including
    // after a failed retry reclaimed its old geometry.
    if (p->rendered_version != 0 && p->rendered_version != os64_html_version(page_doc(p)))
        return false;
    os64_font_set_t *was = NULL;
    uint32_t was_px = 0;
    bool changed = controls_face_at(g.zoom, &was, &was_px);
    bool laid = page_lay_out(p, width, height, g.zoom, reclaim);
    if (changed && !laid) {
        (void)os64_ui_font_app(&g.ui, was, OS64_FONT_ROLE_UI);
        os64_font_set_release(s_controls_set);
        s_controls_set = was;
        s_controls_px = was_px;
    } else if (changed) {
        os64_font_set_release(was);
    }
    return laid;
}

static bool lay_out_page(Page *p, int32_t width, int32_t height)
{
    return lay_out_page_reclaim(p, width, height, p);
}

// The zoom in force from now on; the page follows once the events queued
// with this one are drained, so presses in a row lay it out once, and the
// controls' face with it (lay_out_page).
static void zoom_to(uint32_t zoom)
{
    if (zoom == g.zoom)
        return;
    g.zoom = zoom;
    g.relayout_due = true;
    coming_rejudge();
}

// The next step in (`in`) or out from the zoom in force; one between steps
// (the settings' default may be any whole percent) goes to the next step
// that way.
static uint32_t zoom_step(bool in)
{
    int32_t n = (int32_t)(sizeof(kZoomSteps) / sizeof(kZoomSteps[0]));
    for (int32_t i = 0; i < n; i++) {
        uint32_t s = kZoomSteps[in ? i : n - 1 - i];
        if (in ? s > g.zoom : s < g.zoom)
            return s;
    }
    return g.zoom;
}

// A zoom applied from the Settings window: the default Ctrl+0 goes back to,
// and the zoom in force from now on.
static void zoom_use(uint32_t zoom)
{
    g.zoom_default = zoom;
    zoom_to(zoom);
}

// Dark pages turned on or off (Settings): the page's cascade is judged
// again, since `prefers-color-scheme` has changed its answer, and the page
// is laid out and painted afresh.
static void dark_use(bool dark)
{
    if (dark == s_dark)
        return;
    s_dark = dark;
    coming_rejudge();
    (void)relayout(true);
    os64_ui_mark_dirty(&g.ui, &g.root);
}

// Ctrl with = or + zooms in, with - out, with 0 back to the settings'
// default; Ctrl and the wheel steps too, up to zoom in. Wherever focus is
// in the window, as a browser does. True when the event was one.
static bool zoom_event(const os64_gui_event_t *ev)
{
    if (ev->type == OS64_GUI_EVENT_KEY_DOWN && (ev->key.modifiers & OS64_GUI_MOD_CTRL)) {
        char a = ev->key.ascii;
        if (a == '=' || a == '+')
            zoom_to(zoom_step(true));
        else if (a == '-')
            zoom_to(zoom_step(false));
        else if (a == '0')
            zoom_to(g.zoom_default);
        else
            return false;
        return true;
    }
    if (ev->type == OS64_GUI_EVENT_MOUSE_WHEEL && (ev->mouse.modifiers & OS64_GUI_MOD_CTRL) &&
        ev->mouse.dy != 0) {
        zoom_to(zoom_step(ev->mouse.dy < 0));
        return true;
    }
    return false;
}

// One line of scrolling: the default font's line, as the view shows it at
// the zoom in force.
static int32_t line_step(void)
{
    return (int32_t)((int64_t)s_env.viewport_font_px * 5 * g.zoom / 4000);
}

// The link under a point of the view, or -1.
static int32_t link_at(int32_t x, int32_t y)
{
    const os64_html_node_t *node = NULL;
    element_at(x, y, &node);
    return node != NULL ? os64_page_link_for(page_model(&g.page), node) : -1;
}

// A link followed: libpage says what it is — a place in this page is a
// move, anywhere else is judged by libway with `ask`. A person pressing it
// is never asked about (WAY_ASK_NEVER); a script's click() is judged as
// the page's own choice (WAY_ASK_GO).
static void follow_link_asked(int32_t link, way_ask_t ask)
{
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_LINK, link, 0, 0};
    os64_page_request_t request;
    os64_page_verdict_t verdict = os64_page_activate(page_model(&g.page), what, &request);
    if (verdict == OS64_PAGE_FRAGMENT) {
        scroll_to_node(request.anchor);
        os64_page_request_free(&request);
    } else if (verdict == OS64_PAGE_NAVIGATE) {
        g.chain = 0;
        request_navigate(&request, NAV_GO, ask);
    } else {
        if (reason_refuses(request.reason))
            say_failed(g.page.diag, "link refused", os64_page_reason_name(request.reason));
        else
            status_rest(os64_page_reason_name(request.reason));
        os64_page_request_free(&request);
    }
}

static void follow_link(int32_t link)
{
    follow_link_asked(link, WAY_ASK_NEVER);
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
        b != NULL ? os64_page_control(page_model(&g.page),
                                     os64_page_control_for(page_model(&g.page), b->node)) : NULL;
    if (c != NULL && c->input == OS64_PAGE_INPUT_IMAGE && !c->disabled)
        form_send(os64_page_control_for(page_model(&g.page), b->node), OS64_PAGE_ACTIVATE_CONTROL);
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
        if (scripts_live() && ev->mouse.button == OS64_GUI_MOUSE_LEFT) {
            const os64_html_node_t *at = element_at(ev->mouse.x, ev->mouse.y, NULL);
            if (g.pressed_node != NULL)
                os64_html_release(g.pointer_doc, g.pressed_node);
            g.pressed_node = at;
            g.pointer_doc = page_doc(&g.page);
            if (at != NULL)
                os64_html_hold(g.pointer_doc, at);
            input_queue(IN_MOUSEDOWN, at, NULL, ev->mouse.x, ev->mouse.y, false);
        }
        return true;
    case OS64_GUI_EVENT_MOUSE_BUTTON_UP: {
        // With page scripts the press and the release are events, and a
        // click on the same element is one too; its default action, the
        // link or the picture button, follows the click (inputs_run).
        if (scripts_live() && ev->mouse.button == OS64_GUI_MOUSE_LEFT) {
            const os64_html_node_t *link_node = NULL;
            const os64_html_node_t *at = element_at(ev->mouse.x, ev->mouse.y, &link_node);
            input_queue(IN_MOUSEUP, at, NULL, ev->mouse.x, ev->mouse.y, false);
            if (at != NULL && at == g.pressed_node)
                input_queue(IN_CLICK, at, link_node, ev->mouse.x, ev->mouse.y, false);
            if (g.pressed_node != NULL)
                os64_html_release(g.pointer_doc, g.pressed_node);
            g.pressed_node = NULL;
            g.pressed_link = -1;
            return true;
        }
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
    // The element under the pointer, for mouseover and mouseout: hover
    // tracks the node now, not only the link (DOM_D7.md § Input events).
    if (scripts_live()) {
        const os64_html_node_t *over = element_at(x, y, NULL);
        if (over != g.hover_node) {
            if (g.hover_node != NULL) {
                input_queue(IN_OUT, g.hover_node, over, x, y, false);
                os64_html_release(g.pointer_doc, g.hover_node);
            }
            if (over != NULL) {
                input_queue(IN_OVER, over, g.hover_node, x, y, false);
                os64_html_hold(page_doc(&g.page), over);
            }
            g.hover_node = over;
            g.pointer_doc = page_doc(&g.page);
        }
        if (over != NULL && yonder_scripts_listens(g.page.scripts, "mousemove"))
            input_queue(IN_MOVE, over, NULL, x, y, false);
    }
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

// A mode change stops queued execution immediately. Reload re-parses noscript;
// a POST reply uses the existing confirmation instead of silently resending.
// A new script time limit reaches every page's host now, for its next
// task: the arriving page's, the one waiting for its sheets, and the one on
// screen.
static void settings_use(const char *agent, bool scripts, uint32_t script_seconds)
{
    agent_use(agent);
    g.script_ms = (uint64_t)script_seconds * 1000;
    yonder_scripts_set_execution_ms(g.page.scripts, g.script_ms);
    yonder_scripts_set_execution_ms(g.stream.scripts, g.script_ms);
    yonder_scripts_set_execution_ms(g.coming.page.scripts, g.script_ms);
    if (g.scripts_on == scripts)
        return;
    g.scripts_on = scripts;
    status_set(g.status_rest);
    // The page on screen keeps its scripts across a navigation, but not
    // across a change of the mode its document was parsed in.
    inputs_drop();
    yonder_scripts_free(g.page.scripts);
    g.page.scripts = NULL;
    stop_trip();
    if (g.page.tree != NULL)
        click_reload(NULL, NULL);
    else
        status_rest("Page-script setting applied; open a page to use it.");
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
    if (!g.stream.active)
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
    // The badge takes the bar's right end when it has anything to say, so
    // a long hover address never pushes it out of sight.
    int32_t badge_w = g.badge_text[0] != '\0' ? button_width(g.badge_text) : 0;
    if (badge_w > (area.w - 2 * pad) / 2)
        badge_w = (area.w - 2 * pad) / 2;
    g.badge.bounds = (os64_gui_rect_t){area.w - pad - badge_w, bottom, badge_w, bh};
    os64_ui_set_hidden(&g.ui, &g.badge, badge_w == 0);
    g.status.bounds = (os64_gui_rect_t){pad, bottom, area.w - 2 * pad - (badge_w != 0 ? badge_w + gap : 0), bh};
}

static void on_resize(os64_ui_t *ui)
{
    (void)ui;
    layout();
    g.relayout_due = true;
    coming_rejudge();
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

static int64_t event_poll(void *opaque, os64_gui_event_t *event)
{
    (void)opaque;
    return os64_gui_event_poll(g.win,event);
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
    s_script_audit = argc > 1 && os64_streq(argv[1], "--script-audit");
    g.scripts_on = yonder_settings_saved_scripts();
    s_dark = yonder_settings_saved_dark();
    g.script_ms = (uint64_t)yonder_settings_saved_script_seconds() * 1000;
    const char *diag_refused = diag_dir_open();
    int address_arg = s_script_audit ? 2 : 1;
    const char *first = argc > address_arg ? argv[address_arg] : NULL;
    char title[OS64_GUI_TITLE_MAX] = "yonder";
    if (first != NULL && first[0] == '/') {
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (os64_slurp(first, YONDER_FILE_MAX, &bytes, &len) == OS64_SLURP_OK) {
            os64_html_document_t *doc = parse_file(bytes, len, g.scripts_on);
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
    g.zoom = g.zoom_default = yonder_settings_saved_zoom();
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
    os64_ui_label(&g.badge, g.badge_text);
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
                                &g.badge, &g.status};
    for (size_t i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        os64_ui_add_child(&g.root, kids[i]);
    layout();
    os64_ui_set_root(&g.ui, &g.root);
    (void)os64_ui_font_follow(&g.ui);
    {
        os64_font_set_t *was = NULL;
        uint32_t was_px = 0;
        if (controls_face_at(g.zoom, &was, &was_px))
            os64_font_set_release(was);
    }
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
    // A file's stream starts with no event to wake the loop for it.
    if (g.stream.active)
        (void)os64_gui_event_ring(g.win, BELL_STREAM);
    else
        status_rest("Type an address, or a path to an HTML file, and press Enter.");
    // Said last, so it is still there to read when the window first shows.
    if (diag_refused != NULL)
        status_rest(diag_refused);
    os64_ui_set_focus(&g.ui, first != NULL ? &g.view : &g.field.w);
    sync_bars();

    // P5 testing: Million Dollar Homepage's popup stalled while motion kept
    // the queue busy. Bound input and coalesce idle moves before their scripts
    // run, so each batch reaches painting without waiting for an empty queue.
    // The pointer is read before libui because a widget is handed moves only
    // while a button is held and the status
    // line follows its current position; the question bar answers by its
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
        YonderEventBatch batch = {.held=true,.pending=ev};
        while (g.running && !g.ui.quit && yonder_event_batch_next(&batch,&ev,event_poll,NULL)) {
            if (ev.type == OS64_GUI_EVENT_MOUSE_MOVE)
                hover(ev.mouse.x, ev.mouse.y);
            else if (ev.type == OS64_GUI_EVENT_POINTER_STATE)
                hover(ev.pointer.inside ? ev.pointer.x : -1, ev.pointer.inside ? ev.pointer.y : -1);
            else if (ev.type == OS64_GUI_EVENT_WINDOW_COVERED ||
                     ev.type == OS64_GUI_EVENT_WINDOW_UNCOVERED)
                window_seen();
            if (ev.type == OS64_GUI_EVENT_SETTINGS)
                yonder_settings_open(g.win, BELL_SETTINGS, g.way.agent, settings_use, g.cache,
                                     g.scripts_on, (uint32_t)(g.script_ms / 1000), g.zoom_default,
                                     zoom_use, s_dark, dark_use);
            else if (!bar_event(&ev) && !zoom_event(&ev) && !key_event(&ev) && !password_key(&ev))
                os64_ui_dispatch(&g.ui, &ev);
            // What the event became, as DOM events, now libui is done with it.
            inputs_run();
        }
        if (batch.held) {
            ev = batch.pending;
            held = true;
        }
        if (g.relayout_due) {
            g.relayout_due = false;
            relayout(false);
        }
        if (!g.running || g.ui.quit)
            break;
        bool stream_more = stream_turn();
        bool coming_more = coming_turn();
        bool scripts_ready = script_turn();
        pictures_settle();
        os64_ui_paint(&g.ui);
        if (stream_more)
            (void)os64_gui_event_ring(g.win, BELL_STREAM);
        if (coming_more || (scripts_ready && scripts_owed(g.page.scripts)))
            (void)os64_gui_event_ring(g.win, BELL_SCRIPTS);
        // A task may have set or cleared a timer: the ticker hears of it.
        pictures_schedule();
        if (g.bar.up && !g.bar.armed) {
            // P5 batching may hold a lookahead event. Preserve it and leave
            // the bar disarmed until that older input has been dispatched.
            if (held || os64_gui_event_poll(g.win, &ev) == 1) {
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
    if (g.stream.active)
        stream_drop();
    inputs_drop();
    yonder_scripts_free(g.page.scripts);
    g.page.scripts = NULL;
    forms_drop();
    page_clear(&g.page);
    page_clear(&g.coming.page);
    way_jar_free(g.way.jar);
    if (g.cache != NULL)
        way_cache_close(g.cache);
    // Nothing is left to read an agent: the workers are gone, and the pages.
    yonder_agents_release();
    os64_font_set_release(s_controls_set);  // yonder's own reference to the lent face
    os64_ui_font_release(&g.ui);
    os64_font_family_cache_destroy(s_faces.families);
    os64_text_destroy(s_faces.text);
    os64_gui_window_destroy(g.win);
    return 0;
}
