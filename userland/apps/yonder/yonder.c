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

#include <stdarg.h>
#include <stddef.h>

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
#include "garb/cascade.h"
#include "way/way.h"
#include "image/icon_theme.h"
#include "image/image.h"
#include "os64/os64.h"
#include "os64/clip.h"
#include "os64/date.h"
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
#include "os64/ui_dispatcher.h"
#include "os64/ui_menu.h"
#include "os64/ui_file_dialog.h"
#include "os64/ui_icon.h"
#include "os64/work.h"
#include "agent.h"
#include "bar.h"
#include "mail.h"
#include "paint.h"
#include "picture.h"
#include "svg.h"
#include "webfont.h"
#include "sheet.h"
#include "scale.h"
#include "warp.h"
#include "settings.h"
#include "scripts.h"
#include "diag.h"
#include "script_job.h"
#include "fetch_job.h"
#include "modules.h"
#include "geometry.h"
#include "events.h"
#include "ticker.h"
#include "trip.h"
#include "download.h"
#include "download_job.h"

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

// THE PERSON'S MOTION SETTINGS (yonder.conf): `motion = reduce` has pages
// told `prefers-reduced-motion: reduce`, and `animations = off` hands no
// timeline to a layout, so no animation runs and the page's elements have
// the styles their rules give them.
static bool s_reduce_motion;
static bool s_animations = true;

// PAGE STORAGE (storage.h): the window's store over the profile folder
// (yonder.conf's `profile`), which every page's localStorage and
// sessionStorage are kept by; NULL keeps each page's for its own life. The
// problem last said on the status line, so one is said once. And the
// display the window is on, read at the start, which `screen` reports.
static yonder_storage_t *s_storage;
static char s_storage_said[OS64_PATH_MAX + 96];
static os64_dom_screen_t s_screen;

// THE COOKIE FILE (cookies.h): the jar kept in the profile folder, by
// yonder.conf's `cookies`; NULL keeps the jar for the window's life alone.
// The problem last said on the status line, so one is said once.
static yonder_cookies_t *s_cookies;
static char s_cookies_said[OS64_PATH_MAX + 96];
// yonder.conf's `cookies` as the window opened, which Settings shows when
// there is no file's jar to ask.
static yonder_cookies_mode_t s_cookies_asked;

// The pages a navigation asks for when the agent's preset names no list of
// its own (agent.c: a costume, or an agent typed). libway adds, on a GET,
// the pictures yonder shows by themselves (YONDER.md § A picture asked for
// by itself), libimage's list, at half the weight.
#define YONDER_ACCEPT "text/html, application/xhtml+xml, text/*;q=0.8"

// The work pool. A navigation's job is a FETCH (trip.h; the parse is this
// thread's, DOM_D4.md), so what it declares is one connection's worth of
// libfetch and libtls — a few hundred KiB — with room to spare; a picture
// declares PICTURE_RESERVE and a sheet SHEET_RESERVE, each its worst case.
// The budget counts those declarations, not memory held: what yonder uses
// is what its jobs allocate. Twelve workers each running the largest job
// (a picture, 512 MiB) declare 6 GiB, so 8 GiB admits any twelve at once
// with room for answers not yet reaped, and a page waits on its workers,
// never on worst cases that do not happen.
#define POOL_WORKERS 12
#define TRIP_RESERVE (4u << 20)
#define POOL_BUDGET  ((size_t)8u << 30)

// THE STREAM (DOM_D4.md): how much of an arriving page is fed to its parser
// per turn of the loop before the window goes back to its events. A byte
// budget, because the window's clock is the 10 ms tick and bytes are
// deterministic on the host; the as-built section records what one costs
// in the guest.
#define STREAM_SLICE_BYTES (64u * 1024u)

// What a page's pictures may cost to keep — a still one's pixels, a moving
// one's whole sequence, or an SVG's document/styles; past it a picture draws as its frame.
// A picture is kept at its own size, not the size it is drawn (DEBTS.md),
// so a page of 4K wallpapers costs 33 MiB a picture.
#define PICTURES_KEPT_MAX ((size_t)1u << 30)

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

// THE ANIMATIONS' FRAMES (PILE3.md § The animation model): a frame is a
// layout for an animation and the paint after it. A page whose last frame
// — before it has had one, its last layout — took more than MOTION_COST_MS
// lays out for no animation. Otherwise the next frame waits until twice
// the last one's cost has passed since it ended — at most a third of the
// window's time spent on motion — and comes no sooner than MOTION_FRAME_MS
// after the last began. Leans, to be moved by what the P5 measures.
#define MOTION_COST_MS 200
#define MOTION_FRAME_MS 16

// The window's doorbell bits: the pool's completions, and a navigation's
// mailbox (progress and questions).
#define BELL_WORK (1u << 0)
#define BELL_MAIL (1u << 1)
#define BELL_TICK (1u << 2)       // a moving picture's next frame is due
#define BELL_DOWNLOAD (1u << 3)   // a download's bytes arrived: its meter moves
#define BELL_SCRIPTS (1u << 4)    // a page has another script or timer to run
#define BELL_STREAM (1u << 5)     // an arriving page has more to parse than one slice

// A GIF frame that asks for 10 ms or less is shown for this long, as every
// browser shows the ones that ask for "as fast as you can".
#define FRAME_FLOOR_MS 100
// A frame shown this much after it was due is counted late (the paint
// counts): well past the clock's own step, which is a tick's lateness.
#define FRAME_LATE_MS 30

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

// The shown page's web fonts: their files and faces, and the cascade a
// layout in progress resolves them with (§ The page's web fonts, below).
#define WEBFONT_FILES_MAX 512
#define WEBFONT_HANDLES_MAX 64
#define WEBFONTS_AT_ONCE 4
// The most the HELD files may keep, together: past it, a file refused
// room is failed rather than held.
#define WEBFONT_HELD_MAX ((size_t)64 << 20)
// The page faces one list may gather; page_fonts keeps what the system's
// family and the generic leave of the eight.
#define WEBFONT_LIST_MAX OS64_TEXT_FALLBACK_MAX
// The names of one style's list looked up among the system's families.
#define WEBFONT_NAMES_MAX 32
// The sizes refused for room one layout records, and the sizes given
// their one more layout at one zoom and width: as many as the handle table
// keeps, since more sizes than that cannot all be open at once.
#define WEBFONT_REFUSED_MAX WEBFONT_HANDLES_MAX

_Static_assert(sizeof(garb_unicode_range_t) == sizeof(os64_text_range_t) &&
               offsetof(garb_unicode_range_t, first) == offsetof(os64_text_range_t, first) &&
               offsetof(garb_unicode_range_t, last) == offsetof(os64_text_range_t, last),
               "a face's range is one shape in libgarb and libos64");

// HELD: the file came and is sound, and opening it was refused for room
// (the tree on screen holds its faces in the same budget): its bytes are
// kept, and it is opened once more after the next layout of the shown
// page, which has let the old tree's room go. Refused again, or past
// WEBFONT_HELD_MAX of held bytes, or when that layout fails, the file
// fails.
typedef enum { WF_QUEUED, WF_WAITING, WF_HELD, WF_READY, WF_FAILED } WebFontState;

typedef struct {
    char *url;                      // owned
    WebFontState state;
    os64_text_font_t *base;         // READY: the file, at 16 px, every scalar
    os64_work_id_t id;              // WAITING
    uint8_t *sfnt;                  // HELD: the bytes, owned
    size_t sfnt_len;
    // What came, for the page's record: the container and its bytes, and
    // the layouts begun when it was opened (geometry_layout_total), so a
    // face is not said to be passed over by a layout that came before it.
    yonder_font_kind_t container;
    size_t got;
    uint64_t ready_layout;
    // The layouts begun when it failed: a face's next source is asked for
    // by the next layout, so one after a failure that late is not yet.
    uint64_t failed_layout;
} WebFontFile;

// A family a layout asked for, and what served it: the page's faces; the
// page's faces, none of them in the list (none open yet, none with a
// place, or the page not yet shown); a system family by that name; or
// nothing. `in` is the font the run's list began with, where the text
// context starts looking for each glyph. For the page's record
// (diag_fonts). Best first: a layout's answer for a name is the best its
// runs found.
typedef enum { WF_SERVED_PAGE, WF_SERVED_PAGE_UNLISTED, WF_SERVED_SYSTEM, WF_SERVED_NONE } WebFontServed;
typedef struct {
    char *name;                     // owned: the whole name, as first written
    WebFontServed served;
    char in[OS64_FONT_NAME_CAP];
    uint32_t missing;               // the layout's lookups that found nothing
} WebFontFamily;
// As many families as the record's tables hold lines.
#define WEBFONT_FAMILIES_MAX YONDER_DIAG_RECORDS_MAX
typedef struct {
    WebFontFamily *f;               // owned, grown as names come
    int32_t n, cap;
    // A new name was past the table (`full`), or had no memory to keep
    // (`short_of_memory`): the record says some are not on it, and which
    // (a name not kept is met again at every lookup, so how many is not
    // known).
    bool full, short_of_memory;
} WebFontFamilies;

// How a face fared in a layout, for the page's record: in a list at a size (the text context then draws each
// glyph from the first font in the list that has it, so a listed face may
// draw none), a size of its file refused for room or refused to open, or
// a source of it not kept: the page's table of files had no room for it.
enum { WF_USE_LISTED = 1, WF_USE_ROOM = 2, WF_USE_REFUSED = 4, WF_USE_UNKEPT = 8 };

// One name of a style's list the page's faces answer, as page_fonts met
// it: how many of its faces joined the list.
typedef struct {
    const char *name;
    uint32_t len;
    size_t joined;
} WebFontAsked;

// One face of a file as a layout draws it: at a size, with the ranges of
// the rule that names it (hashed: the rule's own list is the cascade's,
// and a new cascade is a new list of the same ranges).
typedef struct {
    int32_t file;
    uint32_t px;
    uint64_t ranges;
    os64_text_font_t *font;
    uint64_t used;
} WebFontHandle;

static struct {
    WebFontFile *files;
    int32_t nfiles, cap;
    // QUEUED or WAITING, and of those, with the pool; the next to hand it.
    int32_t waiting, in_flight, next;
    WebFontHandle handles[WEBFONT_HANDLES_MAX];
    int32_t nhandles;
    uint64_t clock;
    // A face arrived since the page was last laid out (pictures_settle).
    bool moved;
    // The bytes of the HELD files, held to WEBFONT_HELD_MAX.
    size_t held_bytes;
    // The sizes of open files refused for room in the last layout
    // (webfonts_size_key), and the sizes already given their one more
    // layout, forgotten once a layout refuses none or the page is laid
    // out at another zoom or width, the zoom and width they were for.
    uint64_t refused[WEBFONT_REFUSED_MAX], retried[WEBFONT_REFUSED_MAX];
    int32_t nrefused, nretried;
    uint32_t retried_zoom;
    int32_t retried_width;
    // A document's characters, a bit per code point — the document
    // `chars_doc` at its version `chars_version`, as the layout
    // `chars_layout` began — and for each 64-bit word of them, how many
    // words up to it hold one, so a range meets them in a few reads
    // whatever its width (webfonts_any). A record's write may ask for
    // another page's (diag_fonts), so all three key them.
    const os64_html_document_t *chars_doc;
    uint64_t chars_version, chars_layout;
    uint64_t chars[0x110000 / 64];
    uint16_t chars_upto[0x110000 / 64];
    // The answer page_fonts lends until its next call.
    os64_text_font_t *list[OS64_TEXT_FALLBACK_MAX];
    // What served each family the tree on screen asked for, for the
    // record; and the answers of the layout `laying_layout` of the shown
    // document, made the record's when its tree is shown (page_show) — a
    // layout that fails leaves the record saying what is on screen.
    WebFontFamilies families, laying;
    uint64_t laying_layout;
    // Each face of the cascade as the layout of the tree on screen found it
    // (WF_USE_*), and that layout (`use_layout`, 0 before the page's first
    // layout on screen), for the page's record; and as the layout
    // `laying_layout` is finding it (face_laying), made the record's with
    // its families.
    uint64_t use_layout;
    uint8_t face_use[GARB_FONT_FACES_MAX], face_laying[GARB_FONT_FACES_MAX];
    // A source not kept in the layout just done, for the badge and the
    // record after it (webfonts_feed).
    bool unkept;
} s_web;

// The families a page arriving asked for while it was laid out beside the
// one on screen, for its record (`seq`, its record's), made the shown
// page's when it is: so the arrival write names them. A page with faces
// draws in none of them until it is shown, so a name they answer is
// theirs with none in the list, which is what the arrival shows; the
// layout on screen then says what they serve.
static struct {
    bool staged;
    uint32_t seq;
    WebFontFamilies families;
} s_arriving;

// The cascade the layout in progress is made with, and for each of its
// inputs the sheet entry it is (page_lay_out): a face's url() resolves
// against that sheet. NULL outside a layout.
static struct {
    const garb_cascade_t *cascade;
    const int32_t *entry;
} s_laying;

static void webfonts_feed(bool laid);
static void webfonts_unhold(void);
static void webfonts_leave(void);
static void webfonts_families_free(WebFontFamilies *t);
static void webfonts_families_missing(const WebFontFamilies *t, yonder_diag_t *diag);
static void webfonts_orphaned(void);

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

// The resolver (the page's web fonts, below): a style's family list,
// answered with the page's faces of it that are open, then the system's.
static os64_font_status_t page_fonts(void *ctx, const flow_family_list_t *families, bool bold,
                                     bool italic, uint32_t px, os64_text_font_t *const **list,
                                     size_t *count, os64_font_face_info_t *primary);

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

// `--exit-when-loaded`: a measuring run, started from a shell (an ssh
// session, a script) that wants the page's record and then yonder gone.
// yonder leaves once the shown page has been settled — streamed, its
// pictures and fonts in, its record not owed, no script waiting — and no
// job has come back for EXIT_QUIET_MS; or, for a page that never goes
// quiet (one that polls), after EXIT_GIVE_UP_MS whatever it is doing.
// Leaving is the ordinary way out, so the page's departure record is
// written as it always is.
#define EXIT_QUIET_MS 3000
#define EXIT_GIVE_UP_MS 120000
static bool s_exit_when_loaded;
static uint64_t s_exit_started, s_exit_quiet_from;

// THE PAGE FILES (YONDER_DIAGNOSTICS.md): the directory yonder.conf's
// `diagnostics` names, empty when it names none or the directory could not
// be made (said once, at the start); the sequence the next load's claim
// begins at (diag_begin); and whether a failed write, and a load whose
// record could claim no name, have been said, each said once a run.
// `keep`: yonder.conf's `keep_pages`, which keeps each page beside its file
// (diag.h), read with the directory; a page that could not be kept is said
// once a run too.
static struct {
    char dir[OS64_PATH_MAX];
    uint32_t next;
    bool write_said, claim_said;
    bool keep, keep_said;
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
    yonder_picture_kind_t kind;
    yonder_svg_document_t *svg;
    uint32_t width, height;         // SVG default object size, in CSS pixels
    os64_image_t image;             // a still raster picture's pixels
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

// One of the history's events a page owes its scripts (MODERN_WEB.md
// § Slice 1b), with what it carries captured when it was owed (os64_dom_
// history_event_t): a hashchange's two addresses, or a popstate's entry,
// named by the binding, and that entry's state. All owned.
#define PAGE_EVENTS_MAX 16
typedef struct {
    bool hashchange;
    char *old_url, *new_url, *state;
    uint64_t entry;
} PageEvent;

// A page as the window holds it: libway's page (the tree, what it means,
// the reader's flips) and its layout at the size it was laid out at. A
// text/plain page has no tree of its own, so it is given one.
typedef struct Page {
    way_page_t way;
    struct Page *reuse_sheets;   // source of ready linked parses during a rebuild
    bool scripting;
    // Whether the page has the Modern Web arc's surface: its script host's
    // globals and the properties its cascade reads (garb_env_t.modern_off),
    // as the switch stood when its navigation began.
    bool modern;
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
    // The page's animations as its tree shows them (flow/flow.h): made
    // empty when it is first laid out to be shown (page_timeline), and
    // replaced each time a tree its layout built is (page_show). The tree
    // points into it, so it goes after the tree and before the document
    // whose elements it holds. A struct copy of a Page borrows it; only
    // page_show and page_clear free it.
    flow_timeline_t *timeline;
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
    // Pictures refused before the table (picture_unasked): how many, and
    // each one's identity, so a rebuild or a new layout that meets the
    // same one again records nothing more.
    int32_t unasked, unasked_cap;
    uint64_t *unasked_seen;
    // The pictures and fonts settled while one had moved the page or
    // another was still out, or a font failed: the record is written once
    // the page is laid out again with nothing out (diag_pictures_in,
    // pictures_settle).
    bool record_owed;
    size_t kept_bytes, picture_url_bytes;
    // Synchronous SVG jobs, destinations and retained raster variants share
    // this subdivision. Document/cascade memory is accounted separately.
    size_t svg_bytes, svg_peak;
    yonder_svg_cache_t *svg_cache; // Created by window painting after page installation.
    // The form whose reply this is, when the reply was to a POST: what
    // Reload sends again. Its url is NULL otherwise.
    os64_page_request_t sent;
    BoxScroll *box_scrolls;
    int32_t nbox_scrolls, cap_box_scrolls;
    // THE PAGE'S PLACE IN THE SESSION (MODERN_WEB.md § Slice 1b; the
    // session's entries, below). `document` names its document to the
    // history's crumbs (way_crumb_t.document) for the page's life, where
    // `serial` moves with every rebuild: a crumb naming it is an entry of
    // this document, reached with no fetch; 0 for a page that never came
    // through the stream. `entry` names the entry the page is at
    // (way_crumb_t.entry): a traversal's target keeps its name, a reload is
    // the same entry, a push makes a new one. `arrives_at` is the session
    // entry a fetched traversal is bringing back, by its name, whatever the
    // page's scripts push before it is shown, and still is for a page that
    // replaces such a page before it was shown (start_trip); 0 for any
    // other arrival. `entry_state` is the current entry's state as its
    // script left it (JSON, NULL for none). The session (g.way) holds the
    // entries of the page on screen; a page not yet shown keeps the ones
    // its scripts make, and those of a page not yet shown that it replaced,
    // in two stacks of its own, `back` and `ahead`, each nearest last,
    // which arriving moves in.
    // `events` are the history's events the page owes its scripts, in
    // order (page_event_run). A scroll its script asked for before it was
    // shown is made when it is (`scroll_asked`, in CSS pixels). `loaded`
    // is HTML's "completely loaded": false until the page is shown and its
    // `load` task, the navigation that task asked for included, is over
    // (arrive_now), and a navigation by address or a form its script asks
    // for before then replaces its entry (page_navigation_replaces). A
    // struct copy of a Page borrows what these point at; page_clear frees
    // it. `asked` is how the navigation that made the document was asked
    // (trip_send's metadata, before any hop), which every entry of the
    // document keeps (way_crumb_t.asked) for Reload and Back to replay;
    // zeroed for a file read from disk.
    uint64_t document, entry, arrives_at;
    way_metadata_t asked;
    char *entry_state;
    way_crumb_t *back, *ahead;
    int32_t nback, nahead;
    PageEvent events[PAGE_EVENTS_MAX];
    int32_t nevents;
    bool scroll_asked, loaded;
    double ask_x, ask_y;
} Page;

static void webfonts_families_shown(const Page *p);
static void webfonts_layout_begins(const Page *p);
static void pictures_schedule(void);
static uint64_t frame_delay(const Picture *pic);

// A shown picture's natural size and raster pixels. SVG returns no pixels:
// it is rasterized at the object/tile dimensions by the window painter.
static const uint32_t *picture_pixels(const Picture *pic, uint32_t *w, uint32_t *h)
{
    if (pic->kind == YONDER_PICTURE_SVG) {
        if (yonder_svg_document_size(pic->svg, s_dark, s_reduce_motion, NULL, NULL, w, h) != SVG_OK) {
            *w = pic->width;
            *h = pic->height;
        }
        return NULL;
    }
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
static void page_event_free(PageEvent *e)
{
    os64_free(e->old_url);
    os64_free(e->new_url);
    os64_free(e->state);
    e->old_url = e->new_url = e->state = NULL;
}

// A page's own entries and their states, and the events it owes.
static void page_session_free(Page *p)
{
    for (int32_t i = 0; i < p->nback; i++)
        os64_free(p->back[i].state);
    for (int32_t i = 0; i < p->nahead; i++)
        os64_free(p->ahead[i].state);
    for (int32_t i = 0; i < p->nevents; i++)
        page_event_free(&p->events[i]);
    os64_free(p->back);
    os64_free(p->ahead);
    os64_free(p->entry_state);
    p->back = p->ahead = NULL;
    p->entry_state = NULL;
    p->nback = p->nahead = p->nevents = 0;
}

static void page_clear(Page *p)
{
    measured_forget(page_doc(p));
    page_session_free(p);
    // The host borrows the record (its miss hook), so the record goes after
    // it, while the tree and the cascade it looks at are still here.
    yonder_scripts_free(p->scripts);
    p->scripts = NULL;
    diag_leave(p);
    yonder_svg_cache_free(p->svg_cache);
    p->svg_cache = NULL;
    for (int32_t i = 0; i < p->npics; i++) {
        yonder_svg_document_free(p->pics[i].svg);
        os64_image_free(&p->pics[i].image);
        os64_image_sequence_free(p->pics[i].moving);
        os64_free((char *)p->pics[i].url);
    }
    os64_free(p->pics);
    os64_free(p->pic_slot);
    os64_free(p->unasked_seen);
    os64_free(p->pic_of);
    os64_free(p->bg_of);
    for (int32_t i = 0; i < p->nbox_scrolls; i++)
        os64_html_release(page_doc(p), p->box_scrolls[i].node);
    flow_free(p->tree);
    flow_timeline_free(p->timeline);
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

// The table's entry for the sheet `node` names, -1 when it has none.
static int32_t sheet_of(const Page *p, const os64_html_node_t *node)
{
    for (int32_t e = 0; e < p->nsheets; e++)
        if (p->sheets[e].node == node && p->sheets[e].importer < 0)
            return e;
    return -1;
}

// An entry of the cascade's input for sheet `e` and, before it, for the
// ready sheets its @imports brought, each naming it as `parent`
// (garb/cascade.h). Its index, or -1 when it is not ready. The cascade
// only reads the sheets, so the page is const here.
static int32_t sheet_emit(const Page *p, int32_t e, garb_sheet_in_t *in, int32_t *entry, int32_t *n)
{
    const Sheet *sh = &p->sheets[e];
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
// pixels at `zoom` (thousandths), never less than one; and the screen the
// window is on (s_screen, or libdom's default when none was read, as
// `screen` reports it), in CSS pixels at `zoom` as `screen.width` is, for
// the device-* media features, which a window's size does not move; and
// the zoom as `resolution`, as devicePixelRatio reports it.
static garb_env_t css_view(int32_t w, int32_t h, uint32_t zoom)
{
    int64_t cw = ((int64_t)w * 1000 + zoom / 2) / zoom;
    int64_t ch = ((int64_t)h * 1000 + zoom / 2) / zoom;
    os64_dom_screen_t screen = s_screen.width > 0 && s_screen.height > 0 ? s_screen : os64_dom_default_options().screen;
    return (garb_env_t){.width = cw > 0 ? (int32_t)cw : 1, .height = ch > 0 ? (int32_t)ch : 1,
                        .dark = s_dark, .reduced_motion = s_reduce_motion,
                        .device_width = (double)((int64_t)screen.width * 1000 / zoom),
                        .device_height = (double)((int64_t)screen.height * 1000 / zoom),
                        .resolution = zoom / 1000.0};
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
static void page_timeline(Page *p);
static bool lay_out_page_reclaim(Page *p, int32_t width, int32_t height, Page *reclaim);

// Window-owner count: a provider takes its delta to include layout retries.
static uint64_t geometry_layout_total;

// A script changed the page since it was last laid out to be shown: its
// next layout is script_rebuild's staged one, and no other may be made of
// the old Page.
static bool page_rebuild_owed(const Page *p)
{
    return p->rendered_version != 0 && p->rendered_version != os64_html_version(page_doc(p));
}

// The tree a Page holds from now on, and with it the animations its layout
// built (flow_timeline_commit): the old tree goes first, since it points
// into the timeline they replace.
static void page_show(Page *p, flow_tree_t *tree)
{
    flow_free(p->tree);
    p->tree = tree;
    flow_timeline_commit(&p->timeline, tree);
    webfonts_families_shown(p);
}

// Publish a layout beside the old one. `reclaim` owns geometry whose faces
// an incomplete attempt may release before retrying; it may be this Page
// or the shown Page beside a staged DOM rebuild. An owed rebuild refuses
// before layout work on its old Page so stale meaning cannot get new geometry.
// A staged Page borrows the shown one's timeline, so its tree keeps the
// animations it built until script_rebuild publishes it.
static bool page_lay_out(Page *p, int32_t width, int32_t height, uint32_t zoom, Page *reclaim);

// Why a layout for the page's animations alone marked the whole view
// (s_cost.refused): libflow's reasons (flow_parts_why_t), then the ones
// decided before libflow is asked.
enum {
    REFUSED_LOADS = FLOW_PARTS_COUNT,   // a picture's size or a face that came in was laid out with it
    REFUSED_CASCADE,                    // the page was laid out from another cascade
    REFUSED_SIZE,                       // at another width, height or zoom
    REFUSED_VIEW,                       // the parts were marked, but the view moved with the layout
    REFUSED_COUNT
};

// A layout made for the page's animations alone asks for its parts:
// lay_out_shown names the page laid out (`page`, NULL for none asked), the
// view in page coordinates and its scroll, and page_lay_out_run says
// whether it marked them (`marked`), in which case the whole view is not
// marked, and if not, why (`why`, of REFUSED_COUNT) and which element and
// property refused (`what`, said while the old tree is alive, and only when
// a record can be written; empty otherwise). One call's: set before the
// layout and read after it.
static struct {
    const Page *page;
    os64_gui_rect_t view;
    flow_point_t scroll;
    bool marked;
    int32_t why;
    char what[96];
} s_parts;

static void mark_part(os64_gui_rect_t r);

// What refused a layout's parts, as the record says it (`out`, empty for
// none named): the element as a selector would name it — its tag, id and
// classes, the element holding it for a text — and the property a layout
// places, when that was the reason.
static void parts_what(char *out, size_t cap, const flow_parts_said_t *said)
{
    const os64_html_node_t *n = said->node;
    const char *text = "";
    if (n != NULL && n->kind != OS64_HTML_ELEMENT) {
        text = "text in ";
        n = n->parent;
    }
    size_t at = 0;
    out[0] = '\0';
    if (n != NULL && n->kind == OS64_HTML_ELEMENT) {
        const os64_html_attr_t *id = os64_html_attr(n, "id"), *cls = os64_html_attr(n, "class");
        at = (size_t)os64_snprintf(out, cap, "%s%s", text, n->name != NULL ? n->name : "?");
        if (at < cap && id != NULL && id->value != NULL && id->value[0] != '\0')
            at += (size_t)os64_snprintf(out + at, cap - at, "#%s", id->value);
        for (const char *c = cls != NULL ? cls->value : NULL; c != NULL && *c != '\0' && at + 1 < cap;) {
            while (*c == ' ' || *c == '\t' || *c == '\n')
                c++;
            if (*c == '\0')
                break;
            out[at++] = '.';
            while (*c != '\0' && *c != ' ' && *c != '\t' && *c != '\n' && at + 1 < cap)
                out[at++] = *c++;
            out[at] = '\0';
        }
    }
    if (said->why == FLOW_PARTS_PLACED && said->prop >= 0 && at < cap)
        os64_snprintf(out + at, cap - at, "%s%s", at > 0 ? ", " : "", garb_prop_name((garb_prop_t)said->prop));
}

// A part a layout for the animations repaints (flow_layout_parts), cut to
// the view `ctx` holds, in page coordinates, as a tick's are (tick_mark),
// without its question of the canvas, which flow_layout_parts answers by
// handing no parts.
static void layout_mark(void *ctx, const flow_box_t *box, os64_gui_rect_t r)
{
    const os64_gui_rect_t *view = ctx;
    os64_gui_rect_t meet;
    (void)box;
    if (os64_rect_intersect(r, *view, &meet))
        mark_part(meet);
}

static bool page_lay_out_run(Page *p, int32_t width, int32_t height, uint32_t zoom, Page *reclaim)
{
    if (page_rebuild_owed(p))
        return false;
    garb_cascade_t *cascade = p->cascade;
    int32_t entry[SHEETS_MAX] = {0};
    garb_env_t view = css_view(width, height, zoom);
    view.modern_off = !p->modern;
    view.scripting = p->scripting;
    if (cascade == NULL || p->sheets_changed || (cascade != NULL && (garb_cascade_env(cascade).width != view.width ||
                                                  garb_cascade_env(cascade).height != view.height ||
                                                  garb_cascade_env(cascade).dark != view.dark ||
                                                  garb_cascade_env(cascade).reduced_motion !=
                                                      view.reduced_motion ||
                                                  garb_cascade_env(cascade).modern_off != view.modern_off ||
                                                  garb_cascade_env(cascade).scripting != view.scripting ||
                                                  garb_cascade_env(cascade).device_width != view.device_width ||
                                                  garb_cascade_env(cascade).device_height != view.device_height ||
                                                  garb_cascade_env(cascade).resolution != view.resolution))) {
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
        // A condition of the page, said once however many of its cascades
        // meet it; a copy of the page laid out beside it (a rebuild staged,
        // a measurement) borrows its record, so it is said there too. The
        // stream's parse so far has no record of its own yet (it is the
        // stream's until the page arrives), so a measurement of it says
        // nothing: the page's own cascade at arrival says it if it is short.
        if (garb_cascade_incomplete(cascade))
            yonder_diag_failed_seen(p->diag, "style", "the cascade came up short (out of room, or nested too deep): "
                                    "part of the page's styles were not applied");
    }
    // Context is valid for this layout only and may name a stack Page;
    // libflow consults replaced_size inside flow_layout, which uses it here.
    s_env.ctx = p;
    s_env.svg = p->modern;
    s_env.cascade = cascade;
    s_laying.cascade = cascade;
    s_laying.entry = cascade == p->cascade ? p->cascade_entry : entry;
    s_env.scripting = p->scripting;
    // One height for `vh` and the initial containing block (flow_env_t),
    // in CSS pixels at `zoom`.
    s_env.viewport_height = view.height;
    s_env.zoom = zoom;
    s_env.timeline = s_animations ? p->timeline : NULL;
    s_env.ticks = s_env.timeline != NULL;
    s_env.now_ms = yonder_now_ms();
    geometry_layout_total++;
    webfonts_layout_begins(p);
    flow_tree_t *fresh = flow_layout(page_doc(p), page_model(p), width, &s_env);
    // The tree on screen holds the faces its runs were shaped in until it
    // is freed — each one a size of a file, counted against the text
    // engine's budget and its face limit — so a layout at new sizes (a
    // zoom, most of all) can stop partway for want of the room the old tree
    // holds. Then both go, and the page is laid out once more in the whole
    // budget — the discarded tree's animations with it, since it was never
    // shown: the retry reads the timeline the page shows, as the first
    // attempt did.
    if (fresh != NULL && flow_incomplete(fresh) && reclaim != NULL && reclaim->tree != NULL) {
        flow_free(fresh);
        flow_free(reclaim->tree);
        reclaim->tree = NULL;
        if (reclaim != p) {
            garb_cascade_free(reclaim->cascade);
            reclaim->cascade = NULL;
        }
        geometry_layout_total++;
        webfonts_layout_begins(p);
        fresh = flow_layout(page_doc(p), page_model(p), width, &s_env);
    }
    s_env.cascade = NULL;
    s_laying.cascade = NULL;
    s_laying.entry = NULL;
    s_env.timeline = NULL;
    s_env.ticks = false;
    if (fresh == NULL) {
        if (cascade != p->cascade)
            garb_cascade_free(cascade);
        return false;
    }
    // A box keeps where it was scrolled to, as near as the new layout lets
    // it: the range may have shrunk, and the element may make no scroll
    // container now. Where it was is in device pixels, so a new zoom
    // scales it with everything else the page holds. Before the tree is
    // shown, so that the parts below compare it scrolled as the old one is.
    for (int32_t i = 0; i < p->nbox_scrolls; i++) {
        if (p->laid_zoom != 0 && p->laid_zoom != zoom) {
            p->box_scrolls[i].at.x = scale_zoom(p->box_scrolls[i].at.x, zoom, p->laid_zoom);
            p->box_scrolls[i].at.y = scale_zoom(p->box_scrolls[i].at.y, zoom, p->laid_zoom);
        }
        int32_t s = flow_scroller_for(fresh, p->box_scrolls[i].node);
        if (s >= 0)
            flow_scroll_set(fresh, s, p->box_scrolls[i].at);
    }
    // A layout for the animations alone repaints what it changed: the old
    // tree is read here, before page_show frees it and commits the timeline
    // it points into. Only when lay_out_shown asked — it does not when a
    // picture's size or a face that came in waits to be laid out too — and
    // the new tree reads the very cascade the old one was laid out from, at
    // its sizes: a change of sheets or of the cascade's environment makes
    // another cascade, and a script's change is laid out by script_rebuild
    // before any other (page_rebuild_owed). Anything else marks the whole
    // view (YONDER_PERF.md § perf-4), and says why (s_parts.why), naming
    // what refused while the old tree it may point into is alive.
    if (p == s_parts.page && p->tree != NULL) {
        flow_parts_said_t said = {FLOW_PARTS_MARKED, NULL, -1};
        if (cascade != p->cascade)
            s_parts.why = REFUSED_CASCADE;
        else if (width != p->laid_width || height != p->laid_height || zoom != p->laid_zoom)
            s_parts.why = REFUSED_SIZE;
        else {
            s_parts.marked = flow_layout_parts(p->tree, fresh, s_parts.scroll, layout_mark, &s_parts.view, &said);
            s_parts.why = said.why;
        }
        // Named only when a record can be written (yonder.conf's
        // diagnostics): the name is a string made at every refusal.
        if (s_diag.dir[0] != '\0')
            parts_what(s_parts.what, sizeof(s_parts.what), &said);
    }
    if (reclaim != NULL && reclaim != p) {
        flow_free(p->tree);
        p->tree = fresh;
    } else {
        page_show(p, fresh);
    }
    if (cascade != p->cascade) {
        garb_cascade_free(p->cascade);
        p->cascade = cascade;
        os64_memcpy(p->cascade_entry, entry, sizeof(entry));
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
typedef enum { FW_TEXT, FW_PASSWORD, FW_CHECK, FW_BUTTON, FW_LIST, FW_DROPDOWN, FW_FILE } FormKind;

// A text control's selection in UTF-16 code units of its value, as
// libpage's record keeps it, and for the record how many times it has been
// written (os64_page_node_selection_writes): a set of the numbers it held
// is still a set.
typedef struct {
    uint32_t start, end;
    os64_page_direction_t direction;
    uint32_t writes;
} FieldSelection;

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
        os64_ui_dropdown_t drop;
    } u;
    os64_ui_widget_t *w;
    // The widget's own class, kept while a gadget's draws nothing
    // (forms_place); NULL until the widget is first placed.
    const os64_ui_class_t *own_class;
    char text[512];                 // a field's buffer, a button's caption
    char secret[512];               // a password's value: the field shows bullets
    size_t secret_len;
    // A text field's selection and libpage's record of it as each stood
    // when they were last in step, in UTF-16 code units of the value
    // (field_selection_push and _pull): whichever moved since then is the
    // one that moved. Until one does, or the person presses on the field or
    // focuses it (field_selection_chosen), they are left as they are, the
    // field's caret where the widget put it and the record at 0, where HTML
    // begins it: `record_seen` starts zeroed, which is that beginning, and
    // `field_seen` is noted when the field is first seen (`seen`).
    FieldSelection field_seen, record_seen;
    bool seen;
    // A dropdown's options as last synced, their nodes, labels and disabled
    // states hashed (options_signature): a change closes its open list, whose rows are a
    // copy of what they were.
    uint64_t options_sig;
} FormWidget;

// A person's input waiting to be dispatched as a DOM event (inputs_run).
typedef enum {
    IN_MOUSEDOWN, IN_MOUSEUP, IN_CLICK, IN_OVER, IN_OUT, IN_MOVE, IN_BUTTON, IN_IMPLICIT,
    IN_CHECK, IN_LIST, IN_INPUT, IN_FOCUS, IN_BLUR, IN_CHANGE
} InputKind;

typedef struct {
    InputKind kind;
    const os64_html_node_t *node;       // held, in `doc`
    const os64_html_node_t *related;    // held: over/out's other node, a click's link, a focus change's other node
    const os64_html_document_t *doc;
    int32_t x, y;                       // window coordinates, for a mouse event
    uint8_t was;                        // IN_CHECK: the box before the click (WAS_*)
    bool held;                          // the primary button was down as it was queued
} Input;

// What a box was before its click, which a cancelled click puts back.
#define WAS_CHECKED 1u
#define WAS_INDETERMINATE 2u

#define INPUTS_MAX 32

// The window's menu verbs, and its models: what the right-click menu offers
// depends on what is under the pointer (§ The right-click menu); a
// download's row offers its own (§ Downloads).
enum {
    ACT_OPEN_LINK = 1, ACT_SAVE_LINK, ACT_COPY_LINK, ACT_OPEN_PICTURE, ACT_SAVE_PICTURE, ACT_COPY_PICTURE,
    ACT_DOWNLOAD_CANCEL, ACT_DOWNLOAD_SHOW,
};
enum { CONTEXT_LINK, CONTEXT_PICTURE, CONTEXT_BOTH, CONTEXT_DOWNLOAD, CONTEXT_MENUS };

static struct {
    int64_t win;
    os64_draw_ctx_t ctx;
    os64_ui_t ui;
    // Its turns deliver the events of every window yonder owns: its own,
    // read a batch at a time (window_poll), and the Settings dialog's.
    os64_ui_dispatcher_t dispatcher;
    os64_ui_window_backend_t native;
    YonderEventBatch batch;
    // The window's popup controller, which a dropdown's list opens in
    // (DROPDOWN.md); NULL if it could not be made, and then no list opens.
    os64_ui_menu_session_t *menus;
    // The right-click menu's verbs, one model for each kind of thing it
    // opens over, and the addresses it acts on, copied as it opened (§ The
    // right-click menu). Empty for the kind not under the pointer.
    os64_ui_actions_t *actions;
    os64_ui_action_view_t *action_view;
    os64_ui_menu_model_t *context_menus[CONTEXT_MENUS];
    char context_link[OS64_FETCH_URL_MAX], context_picture[OS64_FETCH_URL_MAX];
    // Programs this window spawned and has not collected (children_turn).
    uint32_t children;
    os64_ui_widget_t root, status, view, back, forward, reload, stop;
    // The downloads button, at the address row's right end (§ Downloads).
    os64_ui_widget_t downloads;
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
    // The request a person is being asked about before it is sent, and
    // the address of the page that asked for it, its Referer when it goes
    // ("" for none): kept as it was when the page asked, which may be
    // gone or moved by the time the answer comes.
    os64_page_request_t pending;
    NavKind pending_kind;
    way_initiator_t pending_by;     // who asked for it (way/origin.h)
    // What the browser's Reload of a form's reply replays (g.replaying),
    // when `pending_replays`.
    way_metadata_t pending_replay;
    bool pending_replays;
    const char *pending_refused;
    char pending_referrer[OS64_FETCH_URL_MAX];

    char field_buf[1024];
    char status_text[512];
    // What the status line says when no link is under the pointer.
    char status_rest[512];
    int32_t pointer_x, pointer_y;   // where the pointer last was, for hover
    bool running;
    bool scripts_on;
    // The Modern Web arc's switch (yonder.conf's `modern`, Settings): what
    // the next navigation captures, so a page keeps the answer it began with.
    bool modern;
    // Each script task's budget (Settings' script time limit, yonder.conf's
    // script_seconds), in milliseconds.
    uint64_t script_ms;
    // A resize or a zoom is laid out once the events that arrived with it
    // are drained: a drag sends a stream of them, a person zooming presses
    // several times, and a big page takes long enough to lay out that
    // doing it per event would freeze the window.
    bool relayout_due;
    // The window's frame as the compositor last said it was, for a
    // script's outerWidth and outerHeight; 0 until it has said.
    int32_t frame_w, frame_h;
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
    // they make is of `page` (measured_layout); a request they make is
    // `page`'s, found by its serial (scripts_for). `kind` is how it came;
    // `abandoned` says its scripts went back or forward past it
    // (unshown_go), so it is never shown.
    struct {
        Page *page;
        const yonder_scripts_t *scripts;
        const char *url;
        char *fragment;
        size_t cap;
        bool *has_fragment;
        NavKind kind;
        bool abandoned;
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
        bool modern;                // the modern switch, captured then too
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
        // Stopped, and the script the parse stopped at has run: the parse
        // goes on once the inline scripts it connected have (stream_turn).
        bool ran;
        uint64_t ended_ms;
        bool scriptless;                // its host could not be made: no script runs
        // Its navigation timing (performance.timing), from the moment it
        // started: its scripts' host is handed it when it is made, and
        // hears the milestones after that (stream_mark).
        os64_dom_timing_t timing;
        // The address of the page it was asked for from, "" for none
        // (trip_send's: a link followed, a form sent, a script's
        // navigation): what its Referer was made from, and so its
        // document.referrer (stream_request_facts).
        char referrer[OS64_FETCH_URL_MAX];
        // A Reload's: the reloaded page's own document.referrer, which
        // the page that replaces it keeps, as a browser's does, though
        // the Reload's request names nothing ("" for none).
        char kept_referrer[OS64_DOM_URL_MAX];
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
    uint64_t documents_made;        // the last Page.document handed out
    uint64_t entries_made;          // the last Page.entry handed out
    // A TRAVERSAL BEING FETCHED (session_go): the session stays as it was
    // asked until the entry arrives (arrive_now). HTML runs a traversal and
    // the state updates and fragment navigations that come after it on one
    // session history traversal queue, in order, and finalizes each
    // same-document change only while its document is still the active one
    // (§ Finalize a same-document navigation), so what the page on screen
    // changes while the other entry is fetched never joins the session: its
    // document is gone when that runs. The page's own address and state do
    // change, as HTML's do at once. `held` is the entry it was at when the
    // first such change came, which is what the session keeps; `held_push`
    // says one of them was a push, for a traversal that is stopped instead
    // (stop_trip), where the change does join, as one entry.
    bool traversing, has_held, held_push;
    way_crumb_t held;
    // The page not yet shown whose navigation by address is being started
    // (navigation_perform), which takes its place in the session
    // (start_trip); NULL otherwise.
    Page *replacing;
    // The metadata the browser's Reload or Back is replaying (the entry's
    // own, way_crumb_t.asked), which the trip being started sends in place
    // of a fresh answer (trip_send); NULL otherwise.
    const way_metadata_t *replaying;
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
    // What the painter keeps between paints (paint.h): emptied when another
    // page is shown. NULL until the first paint, and when there was no
    // memory for it: the page is then painted working everything out.
    yonder_paint_cache_t *paint_cache;
    // The painter's working buffers, kept between paints (scratch.h): its
    // idle ones freed when another page is shown. NULL until the first
    // paint, and when there was no memory for it: each buffer then comes
    // from the heap and goes back to it.
    yonder_scratch_t *paint_scratch;
    bool covered;                   // nobody can see the window: nothing moves
    // When a slow page's deferred layout is owed (pictures_settle), in
    // yonder_now_ms's milliseconds; YONDER_NEVER for none.
    uint64_t settle_due;
    // The last layout: how long it took, for the status line and for
    // pictures_settle, and when it finished.
    uint64_t laid_ms;
    os64_ticks_t laid_at;
    // The shown page's last frame for an animation (motion_tick, then
    // motion_painted): when its layout began and when it ended, in
    // yonder_now_ms's milliseconds, and what it cost in microseconds,
    // `motion_began_us` on os64_micros' clock. `motion_laid` is false until
    // the page has had one; `motion_painting` while its paint is to come.
    // `motion_pause`: what the status row says the cost rule stopped, before
    // whatever it says (status_set) — empty while it stops nothing
    // (motion_paused_why, motion_pause_said).
    bool motion_laid, motion_painting;
    char motion_pause[96];
    uint64_t motion_began, motion_ended, motion_us;
    int64_t motion_began_us;
    // The same of its last tick (tick_page, then motion_painted): what the
    // animations that tick between layouts cost, held to the cost rule apart
    // from the layouts (PILE3.md § Animations in place). `tick_us` is
    // UINT64_MAX after a tick that found no room and no layout to stand in.
    bool ticked, tick_painting;
    uint64_t tick_began, tick_ended, tick_us;
    int64_t tick_began_us;
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

// What marked the whole view for a paint (mark_whole), counted by the
// paint (s_cost.whole_by). A paint of the whole view that none of yonder's
// own whole marks asked for was made whole by the parts it marked
// (mark_part) when they reach across the view, which is parts that
// together covered it (merged, or a full dirty list made one rectangle),
// and by libui's own marks otherwise: a hover or a focus moved onto the
// view, the window laid out. Two moving things cover the view alone and
// are whole marks of their own: the canvas's background (a frame of its
// picture, a tick of its style), and one part as big as the view (mark_part
// says so: a starfield on a body whose html has a background of its own).
typedef enum {
    WHOLE_PARTS, WHOLE_REFUSED, WHOLE_LAYOUT, WHOLE_SCRIPT, WHOLE_SCROLL, WHOLE_PICTURE, WHOLE_FACE, WHOLE_WINDOW,
    WHOLE_BACKDROP, WHOLE_BIG_PART, WHOLE_TOOLKIT, WHOLE_COUNT
} WholeBy;

// The first thing to mark the whole view since the last paint (glass_paint),
// WHOLE_PARTS while nothing has, and what the parts marked since then reach
// in the window, together.
static WholeBy s_whole_by;
static os64_gui_rect_t s_parts_reach;

// The whole view marked for repainting, and by what (s_whole_by): `root`
// marks the window's every widget, the view among them.
static void mark_whole(WholeBy by, bool root)
{
    if (s_whole_by == WHOLE_PARTS)
        s_whole_by = by;
    os64_ui_mark_dirty(&g.ui, root ? &g.root : &g.view);
}

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
    // No animation runs in it: a page's animations start when it is laid
    // out to be shown (page_timeline).
    s_measuring.timeline = NULL;
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

// A one-line select (HTML § 4.10.7): no `multiple`, and a `size` of 1 or
// none. It is a dropdown (DROPDOWN.md); any other select is a listbox.
static bool select_one_line(const os64_page_control_t *c, const os64_html_node_t *node)
{
    const os64_html_attr_t *size = os64_html_attr(node, "size");
    return !c->multiple && (size == NULL || size->value == NULL || size_rows(size->value) <= 1);
}

// A select's box: as wide as its longest option, and for a listbox the
// rows its list shows, for a dropdown one row and room for its arrow.
// Measured in the controls' face (controls_face_at), the one its widget
// paints in. The page chooses `size` (at most INT32_MAX rows, by
// size_rows), so the arithmetic is 64-bit and the answer clamped.
static void select_size(const os64_page_control_t *c, const os64_html_node_t *node,
                        int32_t *w, int32_t *h)
{
    const os64_html_attr_t *size = os64_html_attr(node, "size");
    bool one_line = select_one_line(c, node);
    int64_t rows = size != NULL && size->value != NULL ? size_rows(size->value) : 0;
    if (one_line)
        rows = 1;
    else if (rows <= 1)
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
    // taller than the face's, labels six pixels in; a dropdown's box is one
    // such row, twenty pixels wider for its arrow. The face is already at
    // the zoom and libflow scales a replaced box's own size by it, so the
    // answer goes back in CSS pixels, rounded up so no row is cut. At most
    // INT32_MAX rows of a face under a thousand pixels, so 64 bits hold it.
    int64_t zoom = s_env.zoom > 0 ? s_env.zoom : 1000;
    int64_t dh = rows * ((int64_t)os64_ui_font_row_height(&g.ui, OS64_UI_FONT_APP) + 8) + 4;
    int64_t dw = (int64_t)widest + 2 * 6 + 4 + (one_line ? 20 : 0);
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
    if (p->modern && yonder_svg_size(node, w, h))
        return true;
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

// What the status row says before anything while scripts run.
#define SCRIPTS_ON_SAID "SCRIPTS ON | "
// The room the modes said before every message take at most.
#define STATUS_MODES_ROOM (sizeof(SCRIPTS_ON_SAID) - 1 + sizeof(g.motion_pause) - 1)

// The status row: the modes in force, then `text`. A mode is said before
// every message, so no message — an alert, a link under the pointer, an
// error — can hide one.
static void status_set(const char *text)
{
    os64_snprintf(g.status_text, sizeof(g.status_text), "%s%s%s",
                  g.scripts_on ? SCRIPTS_ON_SAID : "", g.motion_pause, text);
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
// parent is not, and whether pages are kept in it (`keep_pages`). The
// sentence to say once when it names one that cannot be used (it is also
// logged), NULL otherwise. A run's sequence continues from the highest the
// directory already holds, so a kept census is never written over by the
// next run's first page.
static const char *diag_dir_open(void)
{
    static char said[OS64_PATH_MAX + 96];
    char dir[OS64_PATH_MAX];
    bool keep = yonder_settings_saved_keep_pages();
    if (!yonder_settings_saved_diagnostics(dir, sizeof(dir))) {
        if (keep)
            os64_debug_log("yonder: yonder.conf's keep_pages keeps pages beside their page files, "
                           "and there is no diagnostics directory; nothing is kept");
        return NULL;
    }
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
    s_diag.keep = keep;
    return NULL;
}

static void diag_kept(int64_t answer);

// A load's record, numbered in browse order, its name claimed in the
// directory before anything is written under it (yonder_diag_claim): a
// number another yonder took meanwhile is passed over. A record that can
// claim no name is kept for the badge and never written (diag_write).
// NULL on no memory: the load goes on unrecorded.
// THE SETTINGS A RECORD SAYS ITS PAGE WAS READ UNDER, by key, as in force
// now: what the claim writes, and what a write compares the record with.
// The value, which a number is written into `out` (`cap` bytes) for.
static const char *const kDiagSettings[] = {"agent", "scripts", "modern", "zoom", "appearance", "motion", "animate",
                                            "script seconds"};
static const char *diag_setting(const char *key, char *out, size_t cap)
{
    if (os64_streq(key, "agent"))
        return g.way.agent != NULL ? g.way.agent : YONDER_AGENT;
    if (os64_streq(key, "scripts"))
        return g.scripts_on ? "on" : "off";
    if (os64_streq(key, "modern"))
        return g.modern ? "on" : "off";
    if (os64_streq(key, "appearance"))
        return s_dark ? "dark" : "light";
    if (os64_streq(key, "motion"))
        return s_reduce_motion ? "reduce" : "full";
    // `animate`, not `animations`: that key says how the page's own
    // animations are faring.
    if (os64_streq(key, "animate"))
        return s_animations ? "on" : "off";
    os64_snprintf(out, cap, "%lu", (unsigned long)(os64_streq(key, "zoom") ? g.zoom : g.script_ms / 1000));
    return out;
}
// The view a record says its page was laid out in, `<width>x<height>`, and
// whether it is the view `w` by `h`.
static bool diag_same_view(const char *laid, int32_t w, int32_t h)
{
    char now[48];
    int n = os64_snprintf(now, sizeof(now), "%dx%d px", (int)w, (int)h);
    return laid == NULL || (n > 0 && os64_strlen(laid) >= (size_t)n && os64_memcmp(laid, now, (size_t)n) == 0);
}

// THE JAR'S `document.cookie` STRING AT THE HEAD (REPLAY.md § 3), on the
// record: libway's script door for the page's FINAL address and the
// transport it arrived over, as the page's script host asks it
// (scripts_host takes the same two), so HttpOnly cookies stay out. Taken
// once the head is in hand and before the parser is fed a byte
// (stream_turn, open_local), because that is the last moment the jar's
// state is the page's own and no other fetch's: every hop's Set-Cookie
// is heard as the fetch reads the head, before the head is posted (a
// trailer's is dropped), and after it the jar moves with whatever else is
// fetched or runs (a script's own `src`, a sheet a script waits for, the
// page still on screen, an expiry judged at each read), and a replay
// reproduces none of those changes. Asked again with twice the room, up to 1 MiB;
// cookies that do not fit even then are not written cut short, and the
// record says it lost a line.
static void diag_cookies(yonder_diag_t *d, const char *url, bool encrypted)
{
    if (d == NULL)
        return;
    bool whole = false;
    size_t cap = 512;
    char *cookies = NULL;
    while (!whole && cap <= ((size_t)1 << 20)) {
        os64_free(cookies);
        cookies = os64_malloc(cap);
        if (cookies == NULL)
            break;
        (void)way_script_cookies(g.way.jar, url, encrypted, cookies, cap, &whole);
        cap *= 2;
    }
    if (cookies != NULL && whole)
        yonder_diag_head_fact(d, "cookies", cookies);
    else
        yonder_diag_short(d);
    os64_free(cookies);
}

// A load that left cookies out for want of room (way/jar.h): one FAILED
// line a host on the page's record, its count the most one request to that
// host left out. A site that is sent fewer cookies than it set may see a
// stranger, which is why it is a FAILED line and not a plain one.
static void diag_cookies_short(yonder_diag_t *d, const way_cookies_short_t *s)
{
    if (d == NULL || s->left <= 0)
        return;
    char message[OS64_URL_HOST_MAX + 32];
    os64_snprintf(message, sizeof(message), "left out of a request to %s", s->host);
    yonder_diag_failed_most(d, "cookies", message, (uint32_t)s->left);
}

static yonder_diag_t *diag_begin(const char *url)
{
    yonder_diag_t *d = yonder_diag_new(url, s_diag.next, os64_micros());
    // THE INPUTS A REPLAY READS THE PAGE WITH, as in force for this load
    // (REPLAY.md § 3): the agent first; the clock and its zone (the agent,
    // the cookies, the clock and the zone are what make a record version
    // 2); the view's size; and the rest of kDiagSettings. The cookies are
    // not the claim's: they are the final address's, taken at the head
    // (diag_cookies). A replay sets each from the record and assumes none.
    // The arrival writes the laid-out size again; a record whose page was
    // laid out in another view than the claim's (diag_arrived), or whose
    // settings are not the ones the claim wrote when it is written
    // (diag_write), is marked unjudged.
    if (d != NULL) {
        char v[48];
        yonder_diag_head_fact(d, kDiagSettings[0], diag_setting(kDiagSettings[0], v, sizeof(v)));
        os64_time_t t;
        if (os64_time(&t) == 0 && t.ticks_per_second != 0) {
            os64_snprintf(v, sizeof(v), "%lld", (long long)t.epoch * 1000 +
                          (long long)((uint64_t)t.ticks_into_second * 1000 / t.ticks_per_second));
            yonder_diag_head_fact(d, "clock", v);
            const char *zone = os64_getenv("TZ");
            if (zone != NULL && zone[0] != '\0')
                yonder_diag_head_fact(d, "tz", zone);
            else {
                os64_snprintf(v, sizeof(v), "%dm", (int)t.tz_offset_minutes);
                yonder_diag_head_fact(d, "tz", v);
            }
        }
        os64_snprintf(v, sizeof(v), "%dx%d px", (int)g.view.bounds.w, (int)g.view.bounds.h);
        yonder_diag_head_fact(d, "laid out at", v);
        for (size_t i = 1; i < sizeof(kDiagSettings) / sizeof(kDiagSettings[0]); i++)
            yonder_diag_head_fact(d, kDiagSettings[i], diag_setting(kDiagSettings[i], v, sizeof(v)));
    }
    if (d != NULL && s_diag.dir[0] != '\0') {
        int64_t claimed = yonder_diag_claim(d, s_diag.dir, s_diag.keep);
        if (claimed != YONDER_DIAG_UNCLAIMED)
            diag_kept(claimed);
    }
    s_diag.next = (d != NULL ? yonder_diag_seq(d) : s_diag.next) + 1;
    return d;
}

// The record as it stands, written under the name its load claimed when
// yonder.conf asks for files; true when the file was written. A record
// that could claim no name is never written, and a failure is not either:
// each is said once a run, in the log and on the status line, when the
// record would have been written, after its load's own words.
static bool diag_write(yonder_diag_t *d)
{
    if (d == NULL || s_diag.dir[0] == '\0')
        return false;
    // A record is of one window: a setting that is not what the claim
    // wrote makes it a record no replay can read the page again by. (The
    // view is compared once, at arrival, against the claim's: after it,
    // the view also moves when yonder's own bars come and go, which a
    // replay of the page reproduces.)
    char now[48];
    for (size_t i = 0; i < sizeof(kDiagSettings) / sizeof(kDiagSettings[0]); i++) {
        const char *then = yonder_diag_fact_value(d, kDiagSettings[i]);
        if (then != NULL && !os64_streq(then, diag_setting(kDiagSettings[i], now, sizeof(now))))
            yonder_diag_unjudged(d, "settings changed during the load");
    }
    const yonder_diag_claim_t *c = yonder_diag_claimed(d);
    if (c == NULL) {
        if (!s_diag.claim_said) {
            s_diag.claim_said = true;
            char line[OS64_PATH_MAX + 128];
            os64_snprintf(line, sizeof(line), "No page file for this page: none of %u names from %lu on could be "
                          "made in %s; the rest are tried.", (unsigned)YONDER_DIAG_CLAIM_TRIES,
                          (unsigned long)yonder_diag_seq(d), s_diag.dir);
            os64_debug_log(line);
            status_rest(line);
        }
        return false;
    }
    if (yonder_diag_write(c) == 0)
        return true;
    if (s_diag.write_said)
        return false;
    s_diag.write_said = true;
    char line[OS64_PATH_MAX + 96];
    os64_snprintf(line, sizeof(line), "A page file could not be written in %s; the rest are tried.",
                  s_diag.dir);
    os64_debug_log(line);
    status_rest(line);
    return false;
}

// What a keeper answered (diag.h): a page that could not be kept is said
// once a run, in the log and on the status line, and the next page is
// tried. The scripts and sheets kept of it before the failure stay, and so
// do its bytes if they were already whole; bytes still in their temporary
// file go with it. Its record says it is not whole.
static void diag_kept(int64_t answer)
{
    if (answer == 0 || s_diag.keep_said)
        return;
    s_diag.keep_said = true;
    char line[OS64_PATH_MAX + 96];
    os64_snprintf(line, sizeof(line), "A page could not be kept in %s; the rest are tried.", s_diag.dir);
    os64_debug_log(line);
    status_rest(line);
}

// A script or sheet as the window received it, or that it did not come
// (`bytes` NULL), kept beside the record of the page that asked for it.
static void diag_keep(yonder_diag_t *d, yonder_diag_kept_t kind, uint32_t job, const char *url, const char *final,
                      const char *label, const void *bytes, size_t n)
{
    if (s_diag.keep)
        diag_kept(yonder_diag_keep_resource(yonder_diag_claimed(d), kind, job, url, final, label, bytes, n));
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
    mark_whole(WHOLE_WINDOW, true);
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

// The elements a page has whose content is not drawn (nothing, or a
// stand-in in its place), and the custom elements nothing defines, counted
// whole: the tally is taken again at every write and holds the most any
// look saw. A custom element its definition constructed is drawn as any
// element is; one whose constructor failed is the page's failure, by
// name and with what its constructor threw first, so the next wall a page
// meets (shadow DOM's attachShadow) reads as a reason and not as a count.
// A canvas where scripting is on is also the page's failure, one
// FAILED line however many it has (libflow lays out its fallback where
// scripting is off). Enabled inline SVG reports individual render refusals
// at the paint boundary instead of declaring every SVG missing. A frame
// is not a failure here: its stand-in takes a click to
// its document.
#define DIAG_CUSTOM_MAX 64
static void diag_elements(yonder_diag_t *d, const os64_html_document_t *doc, const yonder_scripts_t *scripts,
                          bool scripting, bool svg)
{
    // A frame's document is not loaded into its box (it is offered as a
    // link instead), and an embed makes no box: both are asked for and not
    // shown. An object shows its fallback, as a browser does for what it
    // cannot play, and is not counted.
    static const char *const kNothing[] = {"canvas", "video", "audio", "svg", "iframe", "frame", "embed"};
    enum { NOTHING = sizeof(kNothing) / sizeof(kNothing[0]) };
    uint32_t nothing[NOTHING] = {0}, canvases = 0;
    struct { const char *name; uint32_t n; bool failed; } custom[DIAG_CUSTOM_MAX];
    int32_t ncustom = 0;
    const os64_html_node_t *n = doc != NULL ? doc->document : NULL;
    while (n != NULL) {
        if (n->kind == OS64_HTML_ELEMENT && n->name != NULL) {
            for (int k = 0; k < NOTHING; k++)
                nothing[k] +=
                    os64_streq(n->name, kNothing[k]) && (!svg || !os64_streq(kNothing[k], "svg"));
            canvases += n->ns == OS64_HTML_NS_HTML && n->tag == OS64_HTML_TAG_CANVAS;
            if (n->ns == OS64_HTML_NS_HTML && n->ce != OS64_HTML_CE_CUSTOM &&
                os64_html_custom_name(n->name)) {
                int32_t c = 0;
                while (c < ncustom && !(custom[c].failed == (n->ce == OS64_HTML_CE_FAILED) &&
                                        os64_streq(custom[c].name, n->name)))
                    c++;
                if (c < ncustom) {
                    custom[c].n++;
                } else if (ncustom < DIAG_CUSTOM_MAX) {
                    custom[ncustom].name = n->name;
                    custom[ncustom].failed = n->ce == OS64_HTML_CE_FAILED;
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
    if (scripting && canvases != 0)
        yonder_diag_failed_seen(d, "unsupported", "canvas");
    for (int32_t c = 0; c < ncustom; c++) {
        char why[256], line[sizeof(why) + 80];
        if (!custom[c].failed)
            yonder_diag_missing_seen(d, "element", custom[c].name, custom[c].n);
        else if (yonder_scripts_custom_failure(scripts, custom[c].name, why, sizeof(why))) {
            os64_snprintf(line, sizeof(line), "%s: %s", custom[c].name, why);
            yonder_diag_failed_most(d, "element", line, custom[c].n);
        } else
            yonder_diag_failed_most(d, "element", custom[c].name, custom[c].n);
    }
}

// What the page's cascade passed over (garb_cascade_skips), by its kind.
static void diag_cascade(yonder_diag_t *d, const garb_cascade_t *c)
{
    static const char *const kinds[] = {
        [GARB_SKIP_PROPERTY] = "css-property", [GARB_SKIP_AT_RULE] = "css-at-rule",
        [GARB_SKIP_FUNCTION] = "css-function", [GARB_SKIP_VALUE] = "css-value",
    };
    garb_skip_t skips[GARB_SKIPS_MAX];
    int32_t n = garb_cascade_skips(c, skips, GARB_SKIPS_MAX, NULL);
    for (int32_t i = 0; i < n && i < GARB_SKIPS_MAX; i++)
        yonder_diag_missing_seen(d, kinds[skips[i].kind], skips[i].name, skips[i].count);
}

// ── What paints cost (YONDER_PERF.md § Measure first) ───────────────────

// The stages of a paint, and the work beside paints. RENDER is libui's
// whole walk — the root panel and the chrome as well as the view — and
// VIEW the page's part of it; the rest are inside VIEW but for PUBLISH,
// handing the damage to the compositor. LAID is inside SHADOWS and
// GRADIENTS: the part of them spent laying their pixels on the glass (the
// mask and pixels verbs), the rest being working them out or copying them
// out of the paint cache. TICKS is a tick's work before its
// paint, SCHEDULE handing the ticker its next deadline (pictures_schedule),
// counted a call — a tick's own schedule is in its time as well as here —
// TASKS every script task the loop runs, the shown page's or an arriving
// one's — a script, a timer or an event — from task_begin to task_said,
// the run itself (task_cost), counted a task, and LAYOUTS every layout made
// (page_lay_out: the shown page's, a rebuild's, a measurement's), counted a
// layout — a layout a task forces while it runs is in its time as well as
// here; the rebuild after it is a layout's alone.
typedef enum {
    PS_RENDER, PS_VIEW, PS_BACKGROUNDS, PS_PICTURES, PS_TEXT, PS_GROUPS, PS_SHADOWS, PS_GRADIENTS,
    PS_WARPS, PS_LAID, PS_PUBLISH, PS_TICKS, PS_SCHEDULE, PS_TASKS, PS_LAYOUTS, PS_COUNT
} PaintStage;

static const char *const kPaintStage[PS_COUNT] = {
    "render", "view", "backgrounds", "pictures", "text", "groups", "shadows", "gradients",
    "warps", "laid", "publish", "ticks", "schedule", "tasks", "layouts",
};

// The deadlines a schedule hands the ticker, by where the next came from:
// one already past when it is handed over is rung for at once, or at the
// clock's next tick when the loop meets it itself (pictures_schedule), so a
// source counted often here is rings that may do no work.
typedef enum {
    PAST_SETTLE, PAST_LAYOUTS, PAST_TICKS, PAST_ARRIVAL, PAST_STREAM, PAST_TIMERS, PAST_PICTURES, PAST_DOWNLOADS,
    PAST_COUNT
} PastDeadline;

static const char *const kPastDeadline[PAST_COUNT] = {
    "settle", "layouts", "ticks", "arrival", "stream", "timers", "pictures", "downloads",
};

static const char *const kWholeBy[WHOLE_COUNT] = {
    "parts that together covered the view", "layouts for animations that refused their parts", "other layouts",
    "scripts' changes", "scrolls", "pictures that came in", "faces that came in",
    "the window, its bars, a page shown and settings", "the page's background moving behind the whole view",
    "a moving part as big as the view", "the toolkit's own marks",
};

// Why a layout for the animations refused its parts (s_parts.why), as the
// record says it.
static const char *const kRefused[REFUSED_COUNT] = {
    [FLOW_PARTS_MARKED] = "none",
    [FLOW_PARTS_INCOMPLETE] = "a layout that stopped short",
    [FLOW_PARTS_ANIMATED] = "other elements animated",
    [FLOW_PARTS_LAYERS] = "the boxes in other layers",
    [FLOW_PARTS_SCROLLED] = "a box scrolled elsewhere",
    [FLOW_PARTS_MOVED] = "a box moved",
    [FLOW_PARTS_PLACED] = "a value a layout places",
    [FLOW_PARTS_NO_BOX] = "an element with no box of its own",
    [FLOW_PARTS_CANVAS] = "a background the canvas may take",
    [FLOW_PARTS_DEEP] = "stacked boxes nested too deep",
    [FLOW_PARTS_MEMORY] = "no memory",
    [REFUSED_LOADS] = "a picture's size or a face laid out with it",
    [REFUSED_CASCADE] = "another cascade",
    [REFUSED_SIZE] = "another width, height or zoom",
    [REFUSED_VIEW] = "the view moved with it",
};

// Counted with the processor's time-stamp counter, not a system call: a
// paint draws thousands of runs. That trusts the cores' counters to agree,
// which nothing here can check — the thread may move between cores between
// two readings, and os64_micros makes the same assumption (DEBTS § micros).
// An interval whose second reading is behind its first counts as nothing,
// and is counted in `backward`, which the reading names: a machine whose
// counters disagree shows it there. A stage may open inside itself (a group
// in a group), so it is timed from its outermost opening. `now` is the paint's
// (or the tick's, or the call's) until it is folded into `total` and
// `most`; the counter's rate is taken from os64_micros across the counts'
// life when they are written (diag_paint). `waited` is the main loop's
// time asleep for its next event, so the rest of the span is the time it
// was busy.
static struct {
    uint64_t now[PS_COUNT], total[PS_COUNT], most[PS_COUNT], began[PS_COUNT];
    uint32_t depth[PS_COUNT];
    uint64_t paints, whole, pixels, ticks, schedules, waited, scripts, timers, events, layouts;
    // libui's dirty list: the rectangles the paints published, the most in
    // one, and g.ui.damage_collapses when counting began (a full list made
    // one rectangle is a whole view by parts).
    uint64_t rects, rects_most, collapses_from;
    uint64_t layouts_motion;                // of `layouts`, those for the animations alone (LaidFor)
    uint64_t layouts_parts;                 // of those, the ones that marked only what they changed
    // The paints of the whole view, by what marked it (s_whole_by), and the
    // layouts for the animations that marked it, by why (s_parts.why), with
    // the last one's reason and what refused.
    uint64_t whole_by[WHOLE_COUNT], refused[REFUSED_COUNT];
    int32_t refused_last;
    char refused_what[96];
    uint64_t backward;                      // intervals whose counter stepped back
    // The longest one wait of the main loop (cycles), and how late a moving
    // picture's frame was shown against when it was due (ms): the most, and
    // the frames shown more than FRAME_LATE_MS late.
    uint64_t slept_most, late_most, late_frames;
    // The ticker's bells the loop took — rings made before the loop takes
    // one arrive as one (the window's doorbell is a mask) — and the
    // deadlines handed the ticker already past.
    uint64_t bells, past[PAST_COUNT];
    uint64_t from_cycles;
    int64_t from_us;
} s_cost;
// The readings the shown page's record keeps (`d`): numbered from 1 for
// each page shown.
static uint32_t s_cost_readings;

// The counter, read where the program says: the memory clobber keeps the
// compiler from moving the measured work across the read, and an lfence on
// each side keeps the processor from running it early or late (lfence
// waits for what came before it on Intel, and on AMD where it is
// dispatch-serializing — Zen 2 and later; an older AMD part may still let
// a read drift by a few instructions). Not rdtscp: a processor without it
// would kill the program for asking.
static inline uint64_t cycles(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

// The counter since `from`: nothing, and counted, when it reads behind it.
static uint64_t cycles_since(uint64_t from)
{
    uint64_t now = cycles();
    if (now < from) {
        s_cost.backward++;
        return 0;
    }
    return now - from;
}

static void cost_begin(PaintStage s)
{
    if (s_cost.depth[s]++ == 0)
        s_cost.began[s] = cycles();
}

static void cost_end(PaintStage s)
{
    if (s_cost.depth[s] > 0 && --s_cost.depth[s] == 0)
        s_cost.now[s] += cycles_since(s_cost.began[s]);
}

// The stages `from`..`to` of one paint or tick into the counts.
static void cost_fold(PaintStage from, PaintStage to)
{
    for (int32_t s = (int32_t)from; s <= (int32_t)to; s++) {
        s_cost.total[s] += s_cost.now[s];
        if (s_cost.now[s] > s_cost.most[s])
            s_cost.most[s] = s_cost.now[s];
        s_cost.now[s] = 0;
    }
}

// Counting starts again: a page shown, or its record written on asking.
static void cost_reset(void)
{
    os64_memset(&s_cost, 0, sizeof(s_cost));
    s_cost.collapses_from = g.ui.damage_collapses;
    s_cost.from_cycles = cycles();
    s_cost.from_us = os64_micros();
    yonder_ticker_counts_restart(g.ticker);
    yonder_paint_cache_counts_restart(g.paint_cache);
}

// A layout, timed (s_cost): page_lay_out_run is the layout itself.
static bool page_lay_out(Page *p, int32_t width, int32_t height, uint32_t zoom, Page *reclaim)
{
    cost_begin(PS_LAYOUTS);
    bool laid = page_lay_out_run(p, width, height, zoom, reclaim);
    cost_end(PS_LAYOUTS);
    s_cost.layouts++;
    cost_fold(PS_LAYOUTS, PS_LAYOUTS);
    return laid;
}

static void cost_painter(yonder_paint_stage_t stage, bool begin)
{
    PaintStage s = stage == YONDER_PAINT_SHADOW ? PS_SHADOWS : PS_GRADIENTS;
    if (begin)
        cost_begin(s);
    else
        cost_end(s);
}

// The page's facts as they stand, and what its tree and cascade show now.
static void diag_fonts(Page *p);
static void diag_paint(Page *p);

static void diag_observe(Page *p, const char *when)
{
    yonder_diag_t *d = p->diag;
    diag_elements(d, page_doc(p), p->scripts, p->scripting, p->modern);
    if (p->cascade != NULL)
        diag_cascade(d, p->cascade);
    char v[128];
    yonder_diag_fact(d, "written", when);
    if (p->svg_cache) {
        yonder_svg_cache_stats_t stats = yonder_svg_cache_stats(p->svg_cache);
        os64_snprintf(v, sizeof(v), "%lu hits, %lu misses, %lu rasterizations, %lu bypasses",
                      (unsigned long)stats.raster.hits, (unsigned long)stats.raster.misses,
                      (unsigned long)stats.rasterizations, (unsigned long)stats.bypasses);
        yonder_diag_fact(d, "svg raster cache", v);
        os64_snprintf(v, sizeof(v), "%lu entries, %lu retained bytes, %lu evictions; %lu peak SVG bytes",
                      (unsigned long)stats.raster.entries, (unsigned long)stats.owned_bytes,
                      (unsigned long)stats.raster.evictions, (unsigned long)p->svg_peak);
        yonder_diag_fact(d, "svg raster memory", v);
    }
    if (p->tree != NULL && flow_incomplete(p->tree))
        yonder_diag_failed_seen(d, "layout", "the layout stopped partway (INCOMPLETE on the status line)");
    // `still coming`: asked for and not yet in, which is what a page left
    // before its pictures arrived leaves; `not asked for`: refused before
    // the table (picture_unasked).
    int32_t shown = 0, failed = 0, coming = 0;
    for (int32_t i = 0; i < p->npics; i++) {
        shown += p->pics[i].state == PIC_SHOWN;
        failed += p->pics[i].state == PIC_FAILED;
        coming += p->pics[i].state == PIC_WAITING || p->pics[i].state == PIC_QUEUED;
    }
    os64_snprintf(v, sizeof(v),
                  "%d, %d shown, %d could not be read, %d past the memory kept, %d still coming, "
                  "%d not asked for",
                  (int)p->npics, (int)shown, (int)failed, (int)p->not_kept, (int)coming,
                  (int)p->unasked);
    yonder_diag_fact(d, "pictures", v);
    os64_snprintf(v, sizeof(v), "%d, %d ready", (int)p->nsheets, (int)p->sheets_ready);
    yonder_diag_fact(d, "sheets", v);
    diag_fonts(p);
    if (p == &g.page)
        diag_paint(p);
}

// Where an arrival's time went, from performance.timing's milestones,
// each after the navigation began: the document's first byte and its last,
// its parse done, DOMContentLoaded (after the deferred scripts and the
// module graphs have run), and load's end. "-" for one not reached.
static void diag_navigation(Page *p)
{
    static const struct {
        os64_dom_milestone_t which;
        const char *name;
    } kMarks[] = {{OS64_DOM_TIMING_RESPONSE_START, "first byte"}, {OS64_DOM_TIMING_RESPONSE_END, "last byte"},
                  {OS64_DOM_TIMING_DOM_INTERACTIVE, "parsed"},
                  {OS64_DOM_TIMING_DOM_CONTENT_LOADED_START, "DOMContentLoaded"},
                  {OS64_DOM_TIMING_LOAD_END, "load"}};
    const os64_dom_timing_t *t = yonder_scripts_timing(p->scripts);
    if (t == NULL)
        return;
    char v[256];
    size_t at = 0;
    for (size_t i = 0; i < sizeof(kMarks) / sizeof(kMarks[0]) && at < sizeof(v); i++) {
        uint64_t us = t->at[kMarks[i].which];
        int n = us == OS64_DOM_TIMING_NOT_YET
                    ? os64_snprintf(v + at, sizeof(v) - at, "%s%s -", i ? ", " : "", kMarks[i].name)
                    : os64_snprintf(v + at, sizeof(v) - at, "%s%s %lu ms", i ? ", " : "", kMarks[i].name,
                                    (unsigned long)(us / 1000));
        if (n < 0)
            return;
        at += (size_t)n;
    }
    yonder_diag_fact(p->diag, "navigation", v);
}

// The page has arrived: what it is now is written, and the badge is its.
static void diag_arrived(Page *p, uint64_t laid_ms)
{
    if (p->diag == NULL)
        return;
    char v[96];
    // Laid out in another view than the one the claim wrote: the window
    // was resized under the load.
    if (!diag_same_view(yonder_diag_fact_value(p->diag, "laid out at"), p->laid_width, p->laid_height))
        yonder_diag_unjudged(p->diag, "settings changed during the load");
    yonder_diag_head_fact(p->diag, "final address", p->way.url);
    os64_snprintf(v, sizeof(v), "%ld ms", (long)((os64_micros() - yonder_diag_began(p->diag)) / 1000));
    yonder_diag_fact(p->diag, "arrived after", v);
    diag_navigation(p);
    os64_snprintf(v, sizeof(v), "%dx%d px in %lu ms", (int)p->laid_width, (int)p->laid_height,
                  (unsigned long)laid_ms);
    yonder_diag_head_fact(p->diag, "laid out at", v);
    diag_observe(p, "when the page arrived");
    diag_write(p->diag);
}

// The page's last picture and font are in, drawn or not: the file is
// written again, so what is read while the page is still up names every
// picture and font that failed, and not only the ones that had failed when
// it arrived.
static void diag_pictures_in(Page *p)
{
    if (p->diag == NULL)
        return;
    // A picture or face that moved the shown page is laid out first, and
    // a picture or font still out (a face's next source, wanted by that
    // layout) comes in first: the record is owed until then
    // (pictures_settle), so it says what they came to. Every way the
    // pictures and fonts settle comes through here.
    if (p->waiting > 0 || (p == &g.page && (g.pictures_moved || s_web.moved || s_web.waiting > 0))) {
        p->record_owed = true;
        return;
    }
    p->record_owed = false;
    diag_observe(p, "when its pictures and fonts were in");
    diag_write(p->diag);
    // What the observation found since arrival is the badge's too.
    if (p == &g.page)
        badge_follow();
}

// The page is let go: the departure write, which is the complete one —
// for a page let go before it was shown, its fonts section is its faces
// and the families its layout staged for it (diag_fonts) — and the record
// with it, and those staged families.
static void diag_leave(Page *p)
{
    if (p->diag == NULL)
        return;
    diag_observe(p, "when the page was left");
    diag_write(p->diag);
    if (s_arriving.staged && s_arriving.seq == yonder_diag_seq(p->diag)) {
        webfonts_families_free(&s_arriving.families);
        os64_memset(&s_arriving, 0, sizeof(s_arriving));
    }
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

// A page's scroll coordinate in device pixels, rounded and then held to
// int32_t: a script may ask for any finite double (and a sum of two may
// be infinite), and a value rounded past the bound must not be cast.
static int32_t device_px(double v)
{
    if (!(v == v))
        return 0;
    v = v < 0 ? v - 0.5 : v + 0.5;
    return v >= (double)INT32_MAX ? INT32_MAX : v <= (double)INT32_MIN ? INT32_MIN : (int32_t)v;
}

static void scroll_to(int32_t x, int32_t y)
{
    g.sx = x;
    g.sy = y;
    clamp_scroll();
    sync_bars();
    mark_whole(WHOLE_SCROLL, false);
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

static bool motion_pause_said(void);

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
    // The notices first, and the address and the server's note in what the
    // status row leaves them (STATUS_MODES_ROOM before it all): an address
    // may be thousands of bytes, and a notice cut off is a mode unsaid.
    char tail[sizeof(line)];
    os64_snprintf(tail, sizeof(tail), " - laid out at %d px%s in %lu ms%s%s%s%s",
                  g.page.laid_width, zoomed, (unsigned long)ms, pictures, asking,
                  s_env.static_only ? " - POSITIONING OFF (p)" : "",
                  flow_incomplete(g.page.tree) ? " - INCOMPLETE: the layout stopped partway" : "");
    size_t used = os64_strlen(tail) + STATUS_MODES_ROOM;
    size_t head = used < sizeof(line) ? sizeof(line) - used : 1;
    os64_snprintf(line, head, "%s%s%s", g.page.way.url, g.page.way.note[0] ? " - " : "",
                  g.page.way.note);
    os64_strcopy(line + os64_strlen(line), sizeof(line) - os64_strlen(line), tail);
    status_rest(line);
    // A layout may change what the cost rule stops.
    (void)motion_pause_said();
}

// The agent yonder starts with: yonder.conf's saved default, a preset's
// string from an earlier yonder read as that preset's current one
// (yonder_agent_current), else yonder's own.
static const char *starting_agent(void)
{
    char saved[YONDER_AGENT_MAX];
    const char *agent = yonder_settings_saved_agent(saved, sizeof(saved))
                            ? yonder_agent_keep(yonder_agent_current(saved)) : NULL;
    return agent != NULL ? agent : YONDER_AGENT;
}

// An agent applied from the Settings window: every fetch from now on sends
// it and its preset's lines, and the page's standing line says so.
static void agent_use(const char *agent)
{
    g.way.agent = agent;
    g.way.identity = yonder_agent_identity(agent);
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
// What a layout is for: the page (said on the standing line), or a frame
// for an animation — a layout's (motion_tick) or a tick's that was not
// taken (tick_page) — which is quiet, so the standing line goes on saying
// what the page's own layout cost, and is that half's frame.
typedef enum { LAID_FOR_PAGE, LAID_FOR_MOTION, LAID_FOR_TICK } LaidFor;

static bool lay_out_shown(bool again, LaidFor laid_for)
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
    uint64_t began_ms = yonder_now_ms();
    int64_t began_us = os64_micros();
    os64_ticks(&t0);
    page_timeline(&g.page);
    s_cost.layouts_motion += laid_for != LAID_FOR_PAGE;
    int32_t sx_was = g.sx, sy_was = g.sy;
    // The parts bound what the animations changed and nothing else, so a
    // layout that also takes in a picture's size or a face that came in —
    // owed to the settle and consumed below — marks the whole view.
    s_parts.page = laid_for != LAID_FOR_PAGE && !g.pictures_moved && !s_web.moved ? &g.page : NULL;
    s_parts.view = (os64_gui_rect_t){g.sx, g.sy, g.view.bounds.w, g.view.bounds.h};
    s_parts.scroll = scroll_now();
    s_parts.marked = false;
    // Until the layout asks: parts not asked for were refused for what
    // came in with the layout (a layout for the page is never counted),
    // and parts asked for with no old tree to compare stopped short.
    s_parts.why = s_parts.page == NULL ? REFUSED_LOADS : FLOW_PARTS_INCOMPLETE;
    s_parts.what[0] = '\0';
    bool laid = lay_out_page(&g.page, width, height);
    bool parts = laid && s_parts.marked;
    s_parts.page = NULL;
    s_cost.layouts_parts += parts;
    os64_ticks(&t1);
    // A frame's layout; its paint is added when it is done (motion_painted).
    // One that fails is its half's last: its cost reads as past every limit
    // (motion_next, ticks_run).
    if (laid_for == LAID_FOR_MOTION) {
        g.motion_laid = true;
        g.motion_painting = laid;
        g.motion_began = began_ms;
        g.motion_began_us = began_us;
        g.motion_ended = yonder_now_ms();
        g.motion_us = laid ? (uint64_t)(os64_micros() - began_us) : UINT64_MAX;
    } else if (laid_for == LAID_FOR_TICK) {
        g.ticked = true;
        g.tick_painting = laid;
        g.tick_began = began_ms;
        g.tick_began_us = began_us;
        g.tick_ended = yonder_now_ms();
        g.tick_us = laid ? (uint64_t)(os64_micros() - began_us) : UINT64_MAX;
    }
    // The attempt consumes the moves whether or not it fits: kept, they
    // would retry a layout that just failed at every event batch.
    g.pictures_moved = false;
    s_web.moved = false;
    g.settle_due = YONDER_NEVER;       // the deadline goes with the moves it was for
    if (!laid) {
        // What the failed layout wanted is still asked for, and nothing
        // is held for it.
        webfonts_unhold();
        webfonts_feed(false);
        say_failed(g.page.diag, "layout", "Out of memory laying the page out; this is the last layout that fit.");
        return false;
    }
    g.laid_at = t1;
    g.sx = sx;
    css_pictures(&g.page);
    webfonts_feed(true);
    if (anchor != NULL) {
        const flow_box_t *b = flow_box_for(g.page.tree, anchor);
        if (b != NULL)
            g.sy = b->rect.y - offset;
    }
    clamp_scroll();
    sync_bars();
    forms_place();
    pictures_schedule();
    if (laid_for == LAID_FOR_PAGE)
        say_laid_out(ms_between(&t0, &t1));
    // Its parts are marked, unless the view moved after all. A layout for
    // the animations that marks the whole view is counted by why, and the
    // last one kept with what refused, when that was named.
    if (!parts || g.sx != sx_was || g.sy != sy_was) {
        if (laid_for != LAID_FOR_PAGE) {
            int32_t why = parts ? REFUSED_VIEW : s_parts.why;
            s_cost.refused[why]++;
            s_cost.refused_last = why;
            os64_strcopy(s_cost.refused_what, sizeof(s_cost.refused_what), parts ? "" : s_parts.what);
        }
        mark_whole(laid_for == LAID_FOR_PAGE ? WHOLE_LAYOUT : WHOLE_REFUSED, false);
    }
    return !flow_incomplete(g.page.tree);
}

static bool relayout(bool again)
{
    return lay_out_shown(again, LAID_FOR_PAGE);
}

static void request_navigate(os64_page_request_t *request, NavKind kind, way_ask_t ask, const char *from,
                             way_initiator_t by, yonder_diag_t *record);
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
static void reload_as(way_initiator_t by);
static void follow_link_asked(int32_t link, way_ask_t ask, way_initiator_t by);
static void follow_link(int32_t link);
static void download_ask(const char *url, const char *referrer, const char *disposition);
static void download_arrived(os64_work_id_t id, yonder_download_job_t *job);
static void downloads_follow(void);
static void downloads_orphaned(void);
static void downloads_heard(void);
static uint64_t downloads_due(void);
static bool script_rebuild(void);
static void stream_drop(void);
static void stream_sheets(void);
static int64_t task_begin(yonder_scripts_t *host);
static void task_said(yonder_scripts_t *host, bool was_alive, const char *what,
                      const os64_js_outcome_t *out, int64_t began);
static void asks_perform(yonder_scripts_t *host, const os64_html_document_t *doc, bool shown);
static uint64_t script_request(void *opaque, uint64_t serial, uint32_t token, const os64_dom_request_t *q);
static bool script_run(void *opaque, uint64_t serial, const os64_dom_request_t *q, bool (*stop)(void *stop_ctx),
                       void *stop_ctx, os64_dom_response_t *response);
static void script_release(void *opaque, os64_dom_response_t *response);
static void request_arrived(const yonder_fetch_job_t *job, yonder_fetched_t *got);
static bool scripts_live(void);
static void input_queue(InputKind kind, const os64_html_node_t *node, const os64_html_node_t *related,
                        int32_t x, int32_t y, uint8_t was);
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
        request_navigate(&request, NAV_REFRESH, WAY_ASK_GO, g.page.way.url, WAY_BY_PAGE, g.page.diag);
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

// THE PAGE ON SCREEN DEPARTS: a navigation away from it has become a page,
// whose parse begins now (stream_parser), or the window is closing. This is
// where a browser unloads the document, and so does yonder: its scripts'
// runtime is retired (yonder_scripts_depart), so no task of its runs again,
// and what it changed in its storage is committed now, so the next page's
// scripts read it: localStorage in the file, sessionStorage in the
// window's image. The page stays on screen, scriptless, until the new one
// is laid out, and stays so if that one never is (Stop, or a layout out of
// memory). The requests its scripts have out go with the runtime:
// draining it cancels each one.
static void page_departs(void)
{
    yonder_scripts_depart(g.page.scripts);
}

// A milestone of a page's navigation timing (stream_mark's, for a page
// past its stream, whose host is its own now), reached now.
static void page_mark(yonder_scripts_t *host, os64_dom_milestone_t which)
{
    int64_t now = os64_micros();
    yonder_scripts_mark(host, which, now > 0 ? (uint64_t)now : 0);
}

// A page is shown: laid out BESIDE the one on screen and swapped in only
// once the layout exists, so a page this machine cannot lay out costs a
// sentence and not the page the person was reading.
static way_crumb_t page_entry_left(Page *p);
static void traverse_end(bool stopped);

// Whether the session holds the entry named `entry` behind the page on
// screen (`back`) or ahead of it.
static bool session_holds(bool back, uint64_t entry)
{
    const way_crumb_t *stack = back ? g.way.history : g.way.forward;
    int32_t n = back ? g.way.depth : g.way.ahead;
    for (int32_t i = 0; entry != 0 && i < n; i++)
        if (stack[i].entry == entry)
            return true;
    return false;
}

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
    page_timeline(fresh);
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
    // The entry left goes into the session whole, its document named and
    // its state with it — the entry it was at when a traversal began
    // fetching, if the page changed since (g.held). A traversal's entry is
    // found by the name it was fetched for (Page.arrives_at), wherever the
    // list holds it now and whatever the arriving page pushed before it was
    // shown, and the entries between go to the other side as they were,
    // each with its own scroll and state; the page's own entries join after
    // it (below), as pushes from that entry. The arrived crumb's state is
    // the page's own already (stream_entry), so the crumb's copy goes. An
    // entry the list no longer holds (let go past its cap) arrives as a new
    // way.
    bool had = g.page.tree != NULL;
    if (had && (kind == NAV_GO || kind == NAV_BACK || kind == NAV_FORWARD)) {
        way_crumb_t left = page_entry_left(&g.page), arrived;
        if (g.has_held) {
            way_position_t at = left.position;
            os64_free(left.state);
            left = g.held;
            left.position = at;
            os64_memset(&g.held, 0, sizeof(g.held));
            g.has_held = false;
        }
        if (kind != NAV_GO && session_holds(kind == NAV_BACK, fresh->arrives_at)) {
            for (;;) {
                if (kind == NAV_BACK)
                    way_went_back_entry(&g.way, &left, &arrived);
                else
                    way_went_forward_entry(&g.way, &left, &arrived);
                if (arrived.entry == fresh->arrives_at)
                    break;
                left = arrived;
            }
            os64_free(arrived.state);
        } else {
            way_remember_entry(&g.way, &left);
        }
    }
    traverse_end(false);
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
    // After the departure write (page_clear), which names the page's fonts.
    webfonts_leave();
    g.page = *fresh;
    os64_memset(fresh, 0, sizeof(*fresh));
    // The entries its scripts made before it was shown join the session:
    // those behind it each a new way, as a pushState is, and those ahead
    // of it (a Back before it was shown made them) where Forward goes. A
    // page with entries of its own pushed at least once, which let go of
    // what was ahead of its entry, whether or not it went back after.
    if (g.page.nback > 0 || g.page.nahead > 0)
        way_forget_ahead(&g.way);
    for (int32_t i = 0; i < g.page.nback; i++)
        way_remember_entry(&g.way, &g.page.back[i]);
    for (int32_t i = 0; i < g.page.nahead; i++)
        way_forward_entry(&g.way, &g.page.ahead[i]);
    os64_free(g.page.back);
    os64_free(g.page.ahead);
    g.page.back = g.page.ahead = NULL;
    g.page.nback = g.page.nahead = 0;
    // What the last page's paints kept is not this one's.
    yonder_paint_cache_empty(g.paint_cache);
    yonder_scratch_drop(g.paint_scratch);
    // Its paints are counted from here (diag_paint), its readings anew.
    cost_reset();
    s_cost_readings = 0;
    // The families it asked for as it arrived (page_fonts) are the shown
    // page's now; another page's are let go.
    if (s_arriving.staged && g.page.diag != NULL && s_arriving.seq == yonder_diag_seq(g.page.diag)) {
        s_web.families = s_arriving.families;
        webfonts_families_missing(&s_web.families, g.page.diag);
    } else {
        webfonts_families_free(&s_arriving.families);
    }
    os64_memset(&s_arriving, 0, sizeof(s_arriving));
    // On screen, its scripts measure the page's own layout from now on.
    measured_forget(page_doc(&g.page));
    g.page_serial++;
    g.page.rendered_version = os64_html_version(page_doc(&g.page));
    g.page.state_version = os64_page_state_version(os64_page_shared_state(page_model(&g.page)));
    g.pictures_moved = false;
    // The page was laid out before it was shown, when no face of its is
    // wanted (page_fonts): one with faces is laid out again on screen, at
    // the next batch, to learn which it uses — and its record is owed the
    // write after that layout, which says what it found, whether or not
    // it asks for a file whose coming would write it.
    {
        const garb_font_face_t *faces;
        s_web.moved = g.page.cascade != NULL && garb_cascade_font_faces(g.page.cascade, &faces, NULL) > 0;
        g.page.record_owed = s_web.moved && g.page.diag != NULL;
    }
    g.settle_due = YONDER_NEVER;       // the deadline goes with the moves it was for
    g.laid_at = t1;
    g.motion_laid = false;             // a new page's own layout stands for its cost
    g.ticked = false;
    g.motion_pause[0] = '\0';
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
    // A scroll its script asked for while it arrived is made now, in the
    // zoom the page is laid out at.
    if (g.page.scroll_asked) {
        double z = (g.page.laid_zoom != 0 ? g.page.laid_zoom : g.zoom) / 1000.0;
        g.sx = device_px(g.page.ask_x * z);
        g.sy = device_px(g.page.ask_y * z);
        g.page.scroll_asked = false;
        clamp_scroll();
    }
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
        // The document is complete as its load event begins.
        page_mark(g.page.scripts, OS64_DOM_TIMING_DOM_COMPLETE);
        page_mark(g.page.scripts, OS64_DOM_TIMING_LOAD_START);
        yonder_scripts_dispatch(g.page.scripts, NULL, &loaded, NULL, &out);
        page_mark(g.page.scripts, OS64_DOM_TIMING_LOAD_END);
        task_said(g.page.scripts, true, "load", &out, began);
        if (script_rebuild())
            asks_perform(g.page.scripts, doc, true);
        // What the page's scripts said while it arrived is said again: the
        // arrival wrote the status line over it.
        const char *note = yonder_scripts_note(g.page.scripts);
        if (note != NULL && page_doc(&g.page) == doc)
            status_rest(note);
    }
    g.page.loaded = true;
    // The arrival write: what the page was once it was shown and its `load`
    // had run. Its departure writes the file again, complete.
    diag_arrived(&g.page, g.laid_ms);
    g.badge_missing = g.badge_failed = UINT32_MAX;     // a new page's badge, whatever it shows
    badge_follow();
    refresh_if_declared();
    buttons_follow();
    mark_whole(WHOLE_WINDOW, false);
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

// The modern switch as the navigation starting now finds it, which is the
// page's for its life. A record taken with it off says so, so a page that
// lacks the arc's surface is never read as one that lost it.
static void stream_modern_capture(void)
{
    g.stream.modern = g.modern;
    if (!g.modern)
        yonder_diag_fact(g.stream.diag, "modern", "off");
}

// THE PAGE'S NAVIGATION TIMING (performance.timing, the navigation entry):
// its origin is the moment the navigation began, on os64_micros' clock and
// the wall clock's, and the fetch starts then.
static void timing_start(os64_dom_timing_t *t, NavKind kind)
{
    int64_t now = os64_micros();
    t->origin_us = now > 0 ? (uint64_t)now : 0;
    os64_time_t wall;
    t->origin_epoch_us = os64_time(&wall) == 0 && wall.ticks_per_second != 0
        ? wall.epoch * 1000000 + (int64_t)((uint64_t)wall.ticks_into_second * 1000000 / wall.ticks_per_second)
        : 0;
    for (int i = 0; i < OS64_DOM_TIMING_COUNT; i++)
        t->at[i] = OS64_DOM_TIMING_NOT_YET;
    t->at[OS64_DOM_TIMING_FETCH_START] = 0;
    t->type = kind == NAV_RELOAD ? OS64_DOM_NAVIGATE_RELOADED
            : kind == NAV_BACK || kind == NAV_FORWARD ? OS64_DOM_NAVIGATE_TRAVERSED
            : OS64_DOM_NAVIGATE;
}

// A milestone of the navigation `t` times, now: kept, and answered on
// os64_micros' clock.
static uint64_t timing_mark(os64_dom_timing_t *t, os64_dom_milestone_t which)
{
    int64_t now = os64_micros();
    uint64_t at = now > 0 ? (uint64_t)now : 0;
    t->at[which] = at > t->origin_us ? at - t->origin_us : 0;
    return at;
}

// A milestone of the page arriving, now: kept, and told to its scripts'
// host when it has one.
static void stream_mark(os64_dom_milestone_t which)
{
    yonder_scripts_mark(g.stream.scripts, which, timing_mark(&g.stream.timing, which));
}

static void open_local(const char *path, NavKind kind, const way_position_t *crumb)
{
    uint8_t *bytes = NULL;
    size_t len = 0;
    char line[512];
    os64_snprintf(line, sizeof(line), "file://%s", path);
    yonder_diag_t *diag = diag_begin(line);
    // A file's response is its read: the navigation begins before it, the
    // response starts as the read does and ends when it has. The stream is
    // not this page's until the read has worked, so the times wait here.
    os64_dom_timing_t timing;
    timing_start(&timing, kind);
    timing_mark(&timing, OS64_DOM_TIMING_RESPONSE_START);
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
    timing_mark(&timing, OS64_DOM_TIMING_RESPONSE_END);
    os64_memset(&g.stream, 0, sizeof(g.stream));
    g.stream.timing = timing;
    g.stream.diag = diag;
    g.stream.local = bytes;
    g.stream.local_len = len;
    g.stream.kind = kind;
    if (crumb != NULL)
        g.stream.crumb = *crumb;
    g.stream.scripting = g.scripts_on;
    stream_modern_capture();
    g.stream.serial = ++g.pages_made;
    g.stream.page.document = ++g.documents_made;
    g.stream.page.entry = ++g.entries_made;
    g.stream.has_head = true;
    g.stream.head.body = WAY_BODY_HTML;
    os64_snprintf(g.stream.head.url, sizeof(g.stream.head.url), "file://%s", path);
    // A file's head is whole here, and nothing is parsed yet.
    diag_cookies(g.stream.diag, g.stream.head.url, g.stream.head.encrypted);
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
    mark_whole(WHOLE_WINDOW, true);
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

static void start_trip(const char *url, os64_page_request_t *request, const char *referrer, NavKind kind,
                       way_initiator_t by, const way_position_t *crumb);

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
            g.replaying = g.pending_replays ? &g.pending_replay : NULL;
            start_trip(g.pending.url, &g.pending, g.pending_referrer, g.pending_kind, g.pending_by, NULL);
            g.replaying = NULL;
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
    if (g.asker == ASK_WORKER)
        bar_forget();
    os64_html_document_t *doc = g.stream.parser != NULL ? os64_html_parser_abandon(g.stream.parser) : NULL;
    // The sheets it sent for are not wanted; their holds are on the
    // document, which goes last.
    sheets_leave(&g.stream.page);
    sheets_free(&g.stream.page);
    page_session_free(&g.stream.page);
    measured_forget(doc);
    yonder_scripts_free(g.stream.scripts);
    // A load that never became a page writes its record here, with what it
    // got as far as; one that did took its record with it (stream_finish).
    // The host borrows the record (its storage's last commit writes
    // there), so the record goes after it.
    diag_write(g.stream.diag);
    yonder_diag_free(g.stream.diag);
    g.stream.diag = NULL;
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
// stays; its scripts stay live only if the navigation never reached a
// parse, since page_departs unloads it there, and a page unloaded so keeps
// no scripts even when nothing replaces it (DOM.md § What the old page
// does meanwhile). A traversal it ends is ended as stopped
// (traverse_end).
static void stop_trip(void)
{
    if (g.traversing)
        traverse_end(true);
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

// A page not yet shown's place in the session, which the navigation that
// replaces its entry takes (g.replacing): how it is coming, the entry a
// traversal arrives at, and the entries its scripts made, which stay where
// they are around the entry. `kind` is how the new page comes: as the
// replaced page did, but a reload's successor comes as a refresh, a new
// document in the entry on screen. `from` is the page it was taken from.
typedef struct {
    bool taken;
    Page *from;
    NavKind kind;
    uint64_t arrives_at;
    way_crumb_t *back, *ahead;
    int32_t nback, nahead;
} Place;

static NavKind unshown_kind(const Page *p);
static void page_stack_clear(way_crumb_t *stack, int32_t *n);

static void place_take(Place *place)
{
    os64_memset(place, 0, sizeof(*place));
    Page *p = g.replacing;
    g.replacing = NULL;
    if (p == NULL)
        return;
    NavKind kind = unshown_kind(p);
    place->taken = true;
    place->from = p;
    place->kind = kind == NAV_RELOAD ? NAV_REFRESH : kind;
    place->arrives_at = p->arrives_at;
    place->back = p->back;
    place->ahead = p->ahead;
    place->nback = p->nback;
    place->nahead = p->nahead;
    p->back = p->ahead = NULL;
    p->nback = p->nahead = 0;
}

static void trip_send(const char *url, os64_page_request_t *request, const char *referrer, NavKind kind,
                      way_initiator_t by, const way_position_t *crumb);

// Sends `url` (with `request`'s body, if it is a form: MOVED into the job)
// to a worker, `referrer` the address of the page that asked for it (NULL
// for none: an address typed, a traversal, a reload). `by` is who started
// it, and the page that did is `referrer` (its Fetch Metadata: a page
// whose address is not kept reads as cross-site, the answer that claims
// least). The navigation it replaces is stopped first: one at a time.
// One started by a page not yet shown (g.replacing) takes that page's place
// (Place) instead of coming from the page on screen, and a traversal the
// page was coming by goes on as the new page's: the session moves to that
// entry when the new page arrives (arrive_now), and the page on screen's
// changes meanwhile stay held (g.held). Its scroll is the top, as a new
// document's.
static void start_trip(const char *url, os64_page_request_t *request, const char *referrer, NavKind kind,
                       way_initiator_t by, const way_position_t *crumb)
{
    Place place;
    place_take(&place);
    bool traversal = place.taken && g.traversing && (place.kind == NAV_BACK || place.kind == NAV_FORWARD);
    if (traversal)
        g.traversing = false;           // stop_trip must not end it
    trip_send(url, request, referrer, place.taken ? place.kind : kind, by, crumb);
    if (!place.taken)
        return;
    if (!g.stream.active) {
        // Nothing was started. A page held at its DOMContentLoaded
        // (g.finishing) outlives the stop and still arrives: its place, and
        // the traversal it was coming by, are its own again. Any other went
        // with the stop, and its place goes too, a traversal as one stopped.
        if (place.from == g.finishing.page) {
            place.from->back = place.back;
            place.from->ahead = place.ahead;
            place.from->nback = place.nback;
            place.from->nahead = place.nahead;
            g.traversing = traversal;
            return;
        }
        page_stack_clear(place.back, &place.nback);
        page_stack_clear(place.ahead, &place.nahead);
        os64_free(place.back);
        os64_free(place.ahead);
        if (traversal)
            traverse_end(true);
        return;
    }
    g.stream.page.arrives_at = place.arrives_at;
    g.stream.page.back = place.back;
    g.stream.page.ahead = place.ahead;
    g.stream.page.nback = place.nback;
    g.stream.page.nahead = place.nahead;
    g.traversing = traversal;
}

static void trip_send(const char *url, os64_page_request_t *request, const char *referrer, NavKind kind,
                      way_initiator_t by, const way_position_t *crumb)
{
    // A navigation carries the cookies another window kept since this one
    // last looked: a sign-in there is this window's at its next page.
    yonder_cookies_look(s_cookies);
    // What a page asked for names that page in its Referer; a Reload, even
    // one sending a form again, names nothing. Read now: `referrer` may be
    // the asking page's own address, which the stop below can let go. The
    // origin it starts from too, for the same reason.
    char named[OS64_FETCH_URL_MAX];
    os64_strcopy(named, sizeof(named), request != NULL && kind != NAV_RELOAD && referrer != NULL ? referrer : "");
    way_origin_t from;
    way_origin_of(referrer, 0, &from);
    // How it is asked: the entry's own, when the browser's Reload or Back
    // replays it (g.replaying), else who asks now, from where.
    way_metadata_t asked;
    if (g.replaying != NULL)
        asked = *g.replaying;
    else
        way_metadata_navigation(&asked, by, &from);
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
    trip->identity = g.way.identity;
    trip->metadata = asked;
    trip->window = g.win;
    trip->mail_bell = BELL_MAIL;
    os64_strcopy(trip->url, sizeof(trip->url), url);
    os64_strcopy(trip->referrer, sizeof(trip->referrer), named);
    os64_memset(&g.stream, 0, sizeof(g.stream));
    os64_strcopy(g.stream.referrer, sizeof(g.stream.referrer), named);
    // A Reload is of the page on screen (reload_as: the button, a form sent
    // again, and location.reload(), which only the page on screen's asks reach):
    // its document.referrer goes with it, though its Referer did not.
    if (kind == NAV_RELOAD)
        os64_strcopy(g.stream.kept_referrer, sizeof(g.stream.kept_referrer),
                     yonder_scripts_document_referrer(g.page.scripts));
    timing_start(&g.stream.timing, kind);
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
    g.stream.page.document = ++g.documents_made;
    g.stream.page.entry = ++g.entries_made;
    // What its entries keep for a later Reload or Back to replay: the
    // asking, not this one fetch's being a reload.
    g.stream.page.asked = asked;
    g.stream.page.asked.reload = false;
    g.stream.diag = diag;
    g.stream.mail = mail;
    g.stream.kind = kind;
    g.stream.scripting = g.scripts_on;
    stream_modern_capture();
    if (crumb != NULL)
        g.stream.crumb = *crumb;
    char line[512];
    os64_snprintf(line, sizeof(line), "fetching %s", url);
    status_rest(line);
    buttons_follow();
}

// A request a page makes, judged by libway: refused with its sentence,
// asked about in the bar, or sent. `request` is consumed either way. The
// page that asked is the one the request names and tells: `from`, its
// address, is its Referer, and `record` takes a refusal's line — that
// page's own wherever it is in its arrival, whichever page is on screen.
// `by` is whether a person on that page asked or the page itself did.
static void request_navigate(os64_page_request_t *request, NavKind kind, way_ask_t ask, const char *from,
                             way_initiator_t by, yonder_diag_t *record)
{
    way_judgement_t j = way_judge(&g.way, request, ask);
    if (j.kind == WAY_REFUSE) {
        os64_page_request_free(request);
        yonder_diag_failed(record, "link refused", g.way.status);
        badge_follow();
        say_way();
    } else if (j.kind == WAY_QUESTION) {
        bar_ask(ASK_REQUEST, ++g.asked, j.question);
        g.pending = *request;
        os64_memset(request, 0, sizeof(*request));
        g.pending_kind = kind;
        g.pending_by = by;
        g.pending_replays = false;
        g.pending_refused = j.refused;
        os64_strcopy(g.pending_referrer, sizeof(g.pending_referrer), from != NULL ? from : "");
    } else {
        start_trip(request->url, request, from, kind, by, NULL);
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
// it says that, and a yes to it is a yes to sending. `by` is who asked for
// the reload, and `replay` what the browser's own Reload replays, NULL for
// a page's (reload_as).
static void resend_form(way_initiator_t by, const way_metadata_t *replay)
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
    // The page it is sent from: a Reload names nothing in its Referer
    // (trip_send), and a page's own reload starts from this origin.
    os64_strcopy(g.pending_referrer, sizeof(g.pending_referrer), g.page.way.url);
    g.pending_kind = NAV_RELOAD;
    g.pending_by = by;
    g.pending_replays = replay != NULL;
    if (replay != NULL)
        g.pending_replay = *replay;
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
    start_trip(url, request, NULL, kind, WAY_BY_BROWSER, crumb);
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
    return b != NULL && flow_replaced_fixed(b);
}

// Who asks for a page's picture, sheet, script or font, and what for: the
// agent's preset lines, read with the agent, and the request's metadata
// from the page at `page_url`. Mode no-cors for every one: yonder runs no
// CORS check on any of them, a font included, so none says it does.
static void hooks_ask(way_hooks_t *hooks, const char *page_url, way_dest_t dest)
{
    hooks->identity = g.way.identity;
    way_origin_t page;
    way_origin_of(page_url, 0, &page);
    way_metadata_request(&hooks->metadata, &page, dest, WAY_MODE_NO_CORS);
}

// Hands the pool the page's next pictures, in table order, while fewer than
// PICTURES_AT_ONCE are in it. One it cannot take keeps its frame, and is
// named on the record; when that settles the page's last picture, the
// record is written then, as a worker's last arrival writes it (reaped).
static void picture_failed(Page *p, const char *url, const yonder_picture_t *product,
                           const char *asked);

static void pictures_feed(Page *p)
{
    bool refused = false;
    while (p->in_flight < PICTURES_AT_ONCE && p->next_pic < p->npics) {
        int32_t index = p->next_pic++;
        Picture *pic = &p->pics[index];
        if (pic->state != PIC_QUEUED)
            continue;
        pic->state = PIC_FAILED;
        p->waiting--;
        yonder_picture_job_t *job = g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
        if (job == NULL) {
            picture_failed(p, pic->url, NULL,
                           g.pool == NULL ? "no worker to fetch it" : "no memory to ask for it");
            refused = true;
            continue;
        }
        job->kind = YONDER_JOB_PICTURE;
        job->index = index;
        job->generation = g.page_serial;
        job->agent = g.way.agent;
        job->svg_enabled = p->modern;
        job->dark = s_dark;
        job->reduced_motion = s_reduce_motion;
        os64_strcopy(job->url, sizeof(job->url), pic->url);
        job->hooks.jar = g.way.jar;
        job->hooks.cache = g.cache;
        os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), p->way.url);
        hooks_ask(&job->hooks, p->way.url, WAY_DEST_IMAGE);
        os64_work_t work = {yonder_picture_run, yonder_picture_release, job, PICTURE_RESERVE};
        pic->id = os64_work_submit(g.pool, &work);
        if (pic->id == 0) {
            os64_free(job);
            picture_failed(p, pic->url, NULL, "the workers would not take it");
            refused = true;
            continue;
        }
        pic->state = PIC_WAITING;
        p->waiting++;
        p->in_flight++;
    }
    if (refused && p->waiting == 0)
        diag_pictures_in(p);
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

// Whether two addresses share an origin (libway's origin.h; an address
// with no tuple origin shares none).
static bool same_origin(const char *a, const char *b)
{
    way_origin_t oa, ob;
    way_origin_of(a, 0, &oa);
    way_origin_of(b, 0, &ob);
    return way_origin_same(&oa, &ob);
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
    view.scripting = p->scripting;
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
    hooks_ask(&job->hooks, p->way.url, WAY_DEST_STYLE);
    job->keep = s_diag.keep;
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
    diag_cookies_short(p == &g.stream.page ? g.stream.diag : p->diag, &job->hooks.cookies_short);
    Sheet *sh = &p->sheets[job->index];
    if (!sh->waiting)
        return;
    // Kept as the page received it, a sheet that failed as a failure,
    // beside the record of the page it is for: the stream's page has none
    // of its own, its record is the stream's.
    bool came = got != NULL && got->ok;
    diag_keep(p == &g.stream.page ? g.stream.diag : p->diag, YONDER_DIAG_KEPT_SHEET, (uint32_t)job->index, job->url,
              came ? got->url : job->url, came ? got->label : "",
              came ? (got->kept != NULL ? (const void *)got->kept : "") : NULL, came ? got->kept_len : 0);
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

static bool picture_unasked(Page *p, const char *url, const char *written,
                            os64_page_reason_t refused);
static const char *written_attr(const os64_html_node_t *node, const char *attr);

// The pictures one box's layers put behind it, each sent for once.
// NOT inlined into the walk below: the address buffer is 16KB, and the
// walk recurses once per level of box nesting on a 1MB stack — with the
// buffer in the walk's own frame, a page sixty boxes deep walked the stack
// off its end (the P5, 2026-10-30). Here it lives in a frame that is popped
// before the walk descends.
static __attribute__((noinline)) bool css_pictures_of(Page *p, const flow_box_t *b)
{
    bool fresh = false;             // a refusal the record had not met
    for (int32_t i = 0; i < flow_background_layers(b->style); i++) {
        flow_layer_t l = flow_background_layer(b->style, i);
        char url[OS64_FETCH_URL_MAX];
        if (l.image == NULL)
            continue;
        if (!css_url(p, &l, url, sizeof(url))) {
            // Unresolved: the address as the sheet wrote it.
            size_t len = l.image_len < sizeof(url) - 1 ? l.image_len : sizeof(url) - 1;
            os64_memcpy(url, l.image, len);
            url[len] = '\0';
            fresh |= picture_unasked(p, NULL, url, OS64_PAGE_REASON_OK);
        } else if (picture_for(p, url) < 0) {
            fresh |= picture_unasked(p, url, NULL, OS64_PAGE_REASON_OK);
        }
    }
    return fresh;
}

// After a layout: every picture the laid-out boxes' sheets put behind them
// is sent for, once — a resize whose media query brings a new one fetches
// it then. The frame here must stay small (css_pictures_of says why).
static bool css_pictures_under(Page *p, const flow_box_t *b)
{
    bool fresh = false;
    for (; b != NULL; b = b->next) {
        if (b->node != NULL)
            fresh |= css_pictures_of(p, b);
        fresh |= css_pictures_under(p, b->first);
    }
    return fresh;
}

// Without workers too: pictures_feed names each one it cannot hand over.
static void css_pictures(Page *p)
{
    if (p->tree != NULL && p->sheets_ready > 0) {
        bool fresh = css_pictures_under(p, flow_root(p->tree));
        pictures_feed(p);
        if (fresh && p->waiting == 0)
            diag_pictures_in(p);
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
        // None of the page's pictures is gathered: one line says so.
        yonder_diag_failed_seen(p->diag, "picture",
                                "the page's pictures: no memory to gather them");
        if (p == &g.page)
            badge_follow();
        if (p->waiting == 0)
            diag_pictures_in(p);
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
    bool fresh = false;             // a refusal the record had not met
    for (int32_t i = 0; i < n; i++) {
        const os64_page_image_t *img = os64_page_image(model, i);
        int32_t k = picture_for(p, img->src.url);
        p->pic_of[i] = k;
        if (k < 0)
            fresh |= picture_unasked(p, img->src.url, written_attr(img->node, "src"), img->src.refused);
        if (k >= 0 && !picture_sized(p, img->node))
            p->pics[k].moves = true;
    }
    for (int32_t i = 0; i < nb; i++) {
        const os64_page_background_t *bg = os64_page_background(model, i);
        p->bg_of[i] = picture_for(p, bg->src.url);
        if (p->bg_of[i] < 0)
            fresh |= picture_unasked(p, bg->src.url, written_attr(bg->node, "background"),
                                     bg->src.refused);
    }
    pictures_feed(p);
    // A refusal met with nothing out to write the record when it comes in
    // is written now, as one met with the pictures in would be.
    if (fresh && p->waiting == 0)
        diag_pictures_in(p);
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

// Whether `key` names a picture refused before the table for the first
// time on this page: a set of their hashes, kept so the record is a set
// of pictures and not of the times they were met. At YONDER_UNASKED_MAX
// identities the rest are taken as seen: the count stops there.
#define YONDER_UNASKED_MAX 4096
static bool unasked_first(Page *p, const char *key)
{
    uint64_t h = 1469598103934665603ull;       // FNV-1a, as url_hash
    for (const char *c = key; *c != '\0'; c++)
        h = (h ^ (uint8_t)*c) * 1099511628211ull;
    for (int32_t i = 0; i < p->unasked; i++)
        if (p->unasked_seen[i] == h)
            return false;
    if (p->unasked == p->unasked_cap) {
        int32_t cap = p->unasked_cap > 0 ? p->unasked_cap * 2 : 16;
        uint64_t *grown = cap <= YONDER_UNASKED_MAX
                              ? os64_realloc(p->unasked_seen, (size_t)cap * sizeof(*grown)) : NULL;
        if (grown == NULL)
            return false;
        p->unasked_seen = grown;
        p->unasked_cap = cap;
    }
    p->unasked_seen[p->unasked++] = h;
    return true;
}

// A picture the page names that never reaches the table, on the record
// once (unasked_first): one in a scheme nothing here fetches (a `data:`
// picture, which the resolver does not even take) is MISSING
// picture-scheme, counted, since its address is the picture itself; an
// address that was refused (`url` NULL) is FAILED picture, `written` as
// the page wrote it, with the refusal when there is one; one past what a
// page may name, or with no memory to hold it, is FAILED picture by its
// address. True when it was new, and the record changed.
static bool picture_unasked(Page *p, const char *url, const char *written,
                            os64_page_reason_t refused)
{
    const char *addr = url != NULL ? url : written != NULL ? written : "";
    if (p->diag == NULL || !unasked_first(p, addr))
        return false;
    // The scheme as written, by the URL grammar (os64_url_scheme_of): a
    // `data:` address is no URL the parser takes, and is the case most
    // worth counting.
    char scheme[OS64_URL_SCHEME_MAX];
    bool named = os64_url_scheme_of(addr, scheme, sizeof(scheme));
    bool fetched = named && (os64_streq(scheme, "http") || os64_streq(scheme, "https") ||
                             os64_streq(scheme, "file"));
    char line[OS64_FETCH_URL_MAX + 96];
    if (named && !fetched) {
        yonder_diag_missing(p->diag, "picture-scheme", scheme, 1);
    } else if (url == NULL) {
        if (refused != OS64_PAGE_REASON_OK)
            os64_snprintf(line, sizeof(line), "%.256s: the address was refused (%s)", addr,
                          os64_page_reason_name(refused));
        else
            os64_snprintf(line, sizeof(line), "%.256s: the address was refused", addr);
        yonder_diag_failed(p->diag, "picture", line);
    } else {
        os64_snprintf(line, sizeof(line), "%s: past the pictures a page may name, or no memory", url);
        yonder_diag_failed(p->diag, "picture", line);
    }
    if (p == &g.page)
        badge_follow();
    return true;
}

// A refused attribute's text as the page wrote it, for the record.
static const char *written_attr(const os64_html_node_t *node, const char *attr)
{
    const os64_html_attr_t *a = node != NULL ? os64_html_attr(node, attr) : NULL;
    return a != NULL ? a->value : NULL;
}

// The workers stopped (g.pool is gone) with pictures still out: each one
// that was with them is named as failed, and each still queued behind
// them goes through pictures_feed, which names it too, since none of them
// will arrive now; then the record is written.
static void pictures_orphaned(Page *p)
{
    bool any = false;
    for (int32_t i = 0; i < p->npics; i++) {
        if (p->pics[i].state != PIC_WAITING)
            continue;
        p->pics[i].state = PIC_FAILED;
        p->waiting--;
        picture_failed(p, p->pics[i].url, NULL, "the workers stopped");
        any = true;
    }
    p->in_flight = 0;
    pictures_feed(p);
    if (any)
        diag_pictures_in(p);
}

// Whether a picture's arrival can move the page (Picture.moves).
static bool picture_moves_page(const Page *p, int32_t pic)
{
    return p->pics[pic].moves;
}

// A picture that will not be shown, on the page's record by its address:
// an unsupported format or variant is asked for and not had, MISSING image
// <format>, with the address on a plain line; one that could not be asked
// for (`asked`, the reason) is FAILED picture; one whose fetch decided it
// (it would not fetch, came with an error the server answered, or would
// not decode) is FAILED picture-fetch, a kind of its own because a replay,
// which fetches no picture, cannot reproduce it (MISSING image is one too).
static void picture_failed(Page *p, const char *url, const yonder_picture_t *product,
                           const char *asked)
{
    char why[64];
    if (product != NULL && product->format != NULL) {
        yonder_diag_missing(p->diag, "image", product->format, 1);
        char key[OS64_FETCH_URL_MAX + 16];
        os64_snprintf(key, sizeof(key), "picture %s", url);
        if (product->why[0])
            os64_snprintf(why, sizeof(why), "%s, %s", product->format, product->why);
        else
            os64_snprintf(why, sizeof(why), "%s, which yonder does not decode", product->format);
        yonder_diag_fact(p->diag, key, why);
    } else {
        os64_strcopy(why, sizeof(why), asked != NULL ? asked : product == NULL ? "no memory to fetch it" :
                                       product->why[0] != '\0' ? product->why : "no picture came");
        char line[OS64_FETCH_URL_MAX + 80];
        os64_snprintf(line, sizeof(line), "%s: %s", url, why);
        yonder_diag_failed(p->diag, asked != NULL ? "picture" : "picture-fetch", line);
    }
    badge_follow();
}

static void picture_arrived(yonder_picture_job_t *job, yonder_picture_t *product)
{
    if (job->generation != g.page_serial || job->index < 0 || job->index >= g.page.npics)
        return;                     // a page no longer on screen
    diag_cookies_short(g.page.diag, &job->hooks.cookies_short);
    Picture *pic = &g.page.pics[job->index];
    if (pic->state != PIC_WAITING)
        return;
    g.page.waiting--;
    g.page.in_flight--;
    if (product == NULL || product->status != OS64_IMAGE_OK) {
        pic->state = PIC_FAILED;
        picture_failed(&g.page, pic->url, product, NULL);
        return;
    }
    size_t bytes = product->cost;
    if (bytes > PICTURES_KEPT_MAX - g.page.kept_bytes) {
        // Not a failure: what the page's pictures may cost is spent. Named
        // all the same, so every picture not shown is on the record.
        pic->state = PIC_NOT_KEPT;
        g.page.not_kept++;
        char key[OS64_FETCH_URL_MAX + 16];
        os64_snprintf(key, sizeof(key), "picture %s", pic->url);
        yonder_diag_fact(g.page.diag, key, "past the memory kept for the page's pictures");
        return;
    }
    pic->kind = product->kind;
    pic->svg = product->svg;
    product->svg = NULL;
    pic->width = product->width;
    pic->height = product->height;
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
    mark_whole(WHOLE_PICTURE, false);
}

// ── The page's web fonts (PILE3.md § Web fonts) ─────────────────────────
//
// The faces the shown page's sheets declare (garb_cascade_font_faces) are
// matched by the resolver, page_fonts, as a layout asks for a style's
// families: style, then weight (Fonts 4 § 5.2). A face that is open joins
// the list — opened at the size asked with its own unicode-range, so the
// layout passes on what it does not cover — and one that is not is
// WANTED: its first source yonder can read is interned here, to be fetched
// once the layout is done (webfonts_feed), because a resolver does no I/O
// (flow_env_t.fonts). A face with a range is wanted, and drawn, only when the
// document's characters meet it. Text draws at once in the fallback and is
// laid out again when a face arrives, batched as pictures are
// (pictures_settle): every font-display is swap here.
//
// The files are the shown page's, kept across its layouts and let go
// when it is left (webfonts_leave). A file is fetched and made a plain
// SFNT on a worker (webfont.c); this thread opens it once, at 16 px with
// every scalar — the text context's copy of the bytes, `base` — and every
// face drawn from it is another size of that copy (os64_text_font_open_like).

// Whether `a` (`alen` bytes) is `b`, ASCII letters folded on both sides.
static bool fold_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        char y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];
        if (x != y)
            return false;
    }
    return true;
}

// The elements whose text is never drawn, and so meets no face's range:
// the head, the title, and what holds sheets, scripts and templates.
static bool webfonts_undrawn(const os64_html_node_t *n)
{
    static const char *const kUndrawn[] = {"head", "style", "script", "template", "title", NULL};
    if (n->kind != OS64_HTML_ELEMENT || n->name == NULL || n->ns != OS64_HTML_NS_HTML)
        return false;
    for (int k = 0; kUndrawn[k] != NULL; k++)
        if (os64_streq(n->name, kUndrawn[k]))
            return true;
    return false;
}

// The document's characters (s_web.chars), found once a layout: its text
// nodes, but for those webfonts_undrawn holds. Generated content is not
// laid out by libflow, so it has none to add (GARB.md's G5).
static void webfonts_chars(const Page *p)
{
    const os64_html_document_t *doc = page_doc(p);
    uint64_t version = doc != NULL ? os64_html_version(doc) : 0;
    if (s_web.chars_layout == geometry_layout_total && s_web.chars_doc == doc && s_web.chars_version == version)
        return;
    s_web.chars_layout = geometry_layout_total;
    s_web.chars_doc = doc;
    s_web.chars_version = version;
    os64_memset(s_web.chars, 0, sizeof(s_web.chars));
    for (const os64_html_node_t *n = doc != NULL ? doc->document : NULL; n != NULL;) {
        if (n->kind == OS64_HTML_TEXT) {
            for (size_t at = 0; at < n->text_len;) {
                uint32_t cp;
                at += os64_utf8_decode(n->text + at, n->text_len - at, &cp);
                if (cp <= 0x10FFFF)
                    s_web.chars[cp >> 6] |= 1ull << (cp & 63);
            }
        }
        if (n->first_child != NULL && !webfonts_undrawn(n)) {
            n = n->first_child;
            continue;
        }
        while (n != NULL && n->next == NULL)
            n = n->parent;
        n = n != NULL ? n->next : NULL;
    }
    uint16_t upto = 0;
    for (uint32_t w = 0; w < 0x110000 / 64; w++) {
        upto += s_web.chars[w] != 0;
        s_web.chars_upto[w] = upto;
    }
}

// Whether the document has a character from `first` to `last`: the
// first and last words masked to the range, the words between counted.
static bool webfonts_any(uint32_t first, uint32_t last)
{
    uint32_t wf = first >> 6, wl = last >> 6;
    uint64_t head = ~0ull << (first & 63), tail = ~0ull >> (63 - (last & 63));
    if (wf == wl)
        return (s_web.chars[wf] & head & tail) != 0;
    return (s_web.chars[wf] & head) != 0 || (s_web.chars[wl] & tail) != 0 ||
           s_web.chars_upto[wl - 1] != s_web.chars_upto[wf];
}

// Whether a face's ranges meet the document's characters: one without
// ranges always does. libgarb keeps a range within U+10FFFF and forwards.
static bool webfonts_meet(const garb_font_face_t *f)
{
    if (f->nranges == 0)
        return true;
    for (int32_t i = 0; i < f->nranges; i++)
        if (webfonts_any(f->ranges[i].first, f->ranges[i].last))
            return true;
    return false;
}

// Whether a source's format() hint names a container yonder reads; one
// with no hint is fetched and known by its signature (webfont.c).
static bool webfonts_readable(const garb_face_src_t *s)
{
    static const char *const kRead[] = {"truetype", "opentype", "woff", NULL};
    if (s->format == NULL)
        return true;
    for (int k = 0; kRead[k] != NULL; k++)
        if (fold_eq(s->format, s->format_len, kRead[k], os64_strlen(kRead[k])))
            return true;
    return false;
}

// A url() source of a face resolved: against the sheet the rule was
// written in, after its redirects, or the page's base for a `style`
// element; held to the schemes yonder fetches.
static bool webfonts_url(const Page *p, const int32_t *entry, const garb_font_face_t *f,
                         const garb_face_src_t *s, char *out, size_t cap)
{
    char written[OS64_FETCH_URL_MAX];
    if (s->len >= sizeof(written))
        return false;
    os64_memcpy(written, s->text, s->len);
    written[s->len] = '\0';
    const char *base = os64_page_base(page_model(p));
    if (f->sheet >= 0) {
        if (f->sheet >= SHEETS_MAX || entry == NULL)
            return false;
        const Sheet *sh = &p->sheets[entry[f->sheet]];
        if (sh->url != NULL)
            base = sh->url;
    }
    if (!os64_page_url_absolute(base, written, out, cap))
        return false;
    return (os64_strlen(out) > 7 && os64_memcmp(out, "http://", 7) == 0) ||
           (os64_strlen(out) > 8 && os64_memcmp(out, "https://", 8) == 0) ||
           (os64_strlen(out) > 7 && os64_memcmp(out, "file://", 7) == 0);
}

static int32_t webfonts_find(const char *url)
{
    for (int32_t i = 0; i < s_web.nfiles; i++)
        if (os64_strcmp(s_web.files[i].url, url) == 0)
            return i;
    return -1;
}

// A file wanted for the first time: queued to be fetched. -1 past the
// files a page may name, or on no memory.
static int32_t webfonts_add(const char *url, const char **why)
{
    *why = "no memory to keep it";
    if (s_web.nfiles >= WEBFONT_FILES_MAX) {
        *why = "past the font files a page keeps";
        return -1;
    }
    if (s_web.nfiles == s_web.cap) {
        int32_t cap = s_web.cap > 0 ? s_web.cap * 2 : 16;
        WebFontFile *grown = os64_realloc(s_web.files, (size_t)cap * sizeof(*grown));
        if (grown == NULL)
            return -1;
        s_web.files = grown;
        s_web.cap = cap;
    }
    size_t len = os64_strlen(url) + 1;
    char *copy = os64_malloc(len);
    if (copy == NULL)
        return -1;
    os64_memcpy(copy, url, len);
    s_web.files[s_web.nfiles] = (WebFontFile){.url = copy, .state = WF_QUEUED};
    s_web.waiting++;
    return s_web.nfiles++;
}

// The file a face (`face` of the cascade's) is drawn from: its first
// source yonder can read whose file has not failed, interned when it is
// wanted for the first time — a source that cannot be interned (the table
// full, no memory) is passed over like a failed one, for a later source
// already in hand, and is a FAILED font on the record; -1 when there is
// none.
// local() names a system font by its full name, and yonder's have none a
// page writes: it is passed over to the next source.
static int32_t webfonts_file(const Page *p, const garb_font_face_t *f, int32_t face)
{
    for (int32_t i = 0; i < f->nsrc; i++) {
        const garb_face_src_t *s = &f->src[i];
        char url[OS64_FETCH_URL_MAX];
        if (s->kind != GARB_FACE_URL || !webfonts_readable(s) ||
            !webfonts_url(p, s_laying.entry, f, s, url, sizeof(url)))
            continue;
        int32_t file = webfonts_find(url);
        const char *why = NULL;
        if (file < 0)
            file = webfonts_add(url, &why);
        if (file < 0) {
            // Once on the record, however many layouts meet it — a full
            // table stays full; the badge follows after the layout
            // (webfonts_feed).
            char line[OS64_FETCH_URL_MAX + 80];
            os64_snprintf(line, sizeof(line), "%s: %s", url, why);
            yonder_diag_failed_seen(g.page.diag, "font", line);
            s_web.face_laying[face] |= WF_USE_UNKEPT;
            s_web.unkept = true;
        }
        if (file < 0 || s_web.files[file].state == WF_FAILED)
            continue;
        return file;
    }
    return -1;
}

static uint64_t webfonts_ranges_key(const garb_font_face_t *f)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)f->nranges;
    for (int32_t i = 0; i < f->nranges; i++) {
        h = (h ^ f->ranges[i].first) * 1099511628211ull;
        h = (h ^ f->ranges[i].last) * 1099511628211ull;
    }
    return h;
}

// One size of one file with one rule's ranges, as a refusal records it.
static uint64_t webfonts_size_key(int32_t file, uint32_t px, uint64_t ranges)
{
    return (ranges ^ ((uint64_t)(uint32_t)file << 32 | px)) * 1099511628211ull;
}

// A face of a READY file at `px` with the rule's ranges, kept for the
// layouts after this one; the least lately used goes, before the open,
// when the table is full (a run that holds it keeps it). NULL when it
// will not open — and
// one refused for room (`*room`) is recorded, to ask for a layout after
// this one (webfonts_feed).
static os64_text_font_t *webfonts_handle(int32_t file, const garb_font_face_t *f, uint32_t px, bool *room)
{
    *room = false;
    uint64_t key = webfonts_ranges_key(f);
    for (int32_t i = 0; i < s_web.nhandles; i++) {
        WebFontHandle *h = &s_web.handles[i];
        if (h->file == file && h->px == px && h->ranges == key) {
            h->used = ++s_web.clock;
            return h->font;
        }
    }
    // The table full, the least lately used goes first, so its room is
    // there for the open: a run that draws in it holds it.
    if (s_web.nhandles == WEBFONT_HANDLES_MAX) {
        int32_t lru = 0;
        for (int32_t i = 1; i < s_web.nhandles; i++)
            if (s_web.handles[i].used < s_web.handles[lru].used)
                lru = i;
        os64_text_font_release(s_web.handles[lru].font);
        s_web.handles[lru] = s_web.handles[--s_web.nhandles];
    }
    os64_font_face_options_t o = {px, OS64_FONT_HINT_NORMAL};
    os64_text_font_t *font = NULL;
    os64_font_status_t st = os64_text_font_open_like(s_web.files[file].base, &o, (const os64_text_range_t *)f->ranges,
                                                      (size_t)f->nranges, &font);
    if (st != OS64_FONT_OK) {
        // Room the tree on screen may hold: tried again after this layout.
        uint64_t size = webfonts_size_key(file, px, key);
        bool known = false;
        for (int32_t i = 0; i < s_web.nrefused; i++)
            known |= s_web.refused[i] == size;
        *room = st == OS64_FONT_LIMIT || st == OS64_FONT_NO_MEMORY;
        if (*room && !known && s_web.nrefused < WEBFONT_REFUSED_MAX)
            s_web.refused[s_web.nrefused++] = size;
        return NULL;
    }
    s_web.handles[s_web.nhandles++] = (WebFontHandle){file, px, key, font, ++s_web.clock};
    return font;
}

// How near a face's weight `w` is to `want` by Fonts 4 § 5.2's order: a
// tier, then a distance in it. From 400 to 500 the weights up to 500 come
// first, then those below, then those above 500; under 400, those below
// first, then above; over 500, those above first, then below.
static void webfonts_weight_rank(float w, float want, int *tier, float *dist)
{
    if (w == want) {
        *tier = 0, *dist = 0;
    } else if (want >= 400 && want <= 500) {
        if (w > want && w <= 500)
            *tier = 1, *dist = w - want;
        else if (w < want)
            *tier = 2, *dist = want - w;
        else
            *tier = 3, *dist = w - 500;
    } else if (want < 400) {
        if (w < want)
            *tier = 1, *dist = want - w;
        else
            *tier = 2, *dist = w - want;
    } else if (w > want) {
        *tier = 1, *dist = w - want;
    } else {
        *tier = 2, *dist = want - w;
    }
}

static float webfonts_nearest(const garb_font_face_t *f, float want)
{
    return want < f->weight_min ? f->weight_min : want > f->weight_max ? f->weight_max : want;
}

// Which of a family's faces Fonts 4 § 5.2 chooses: a style and a weight
// (webfonts_match), every face of both being the choice — a composite.
typedef struct {
    garb_face_style_t style;
    float weight;
} WebFontPick;

// One family's faces chosen as Fonts 4 § 5.2 chooses them: its style,
// italic then oblique then normal for an italic ask and normal then
// oblique then italic otherwise; then its weight, the nearest by § 5.2's
// order. Stretch is not read. Whether the family has a face at all; the
// choice into `*pick`, its faces told by webfonts_chosen.
static bool webfonts_match(const garb_font_face_t *faces, int32_t n, const flow_family_name_t *name,
                           bool italic, bool bold, WebFontPick *pick)
{
    const garb_face_style_t kItalic[] = {GARB_FACE_ITALIC, GARB_FACE_OBLIQUE, GARB_FACE_NORMAL};
    const garb_face_style_t kNormal[] = {GARB_FACE_NORMAL, GARB_FACE_OBLIQUE, GARB_FACE_ITALIC};
    const garb_face_style_t *order = italic ? kItalic : kNormal;
    int style = -1;
    for (int32_t i = 0; i < n; i++) {
        if (!fold_eq(faces[i].family, faces[i].family_len, name->name, name->len))
            continue;
        for (int k = 0; k < 3; k++)
            if (faces[i].style == order[k] && (style < 0 || k < style))
                style = k;
    }
    if (style < 0)
        return false;
    float want = bold ? 700 : 400, best = 0;
    int best_tier = -1;
    float best_dist = 0;
    for (int32_t i = 0; i < n; i++) {
        if (faces[i].style != order[style] ||
            !fold_eq(faces[i].family, faces[i].family_len, name->name, name->len))
            continue;
        float w = webfonts_nearest(&faces[i], want);
        int tier;
        float dist;
        webfonts_weight_rank(w, want, &tier, &dist);
        if (best_tier < 0 || tier < best_tier || (tier == best_tier && dist < best_dist))
            best_tier = tier, best_dist = dist, best = w;
    }
    *pick = (WebFontPick){order[style], best};
    return true;
}

// Whether `f` is of the family `name` and the choice webfonts_match made.
static bool webfonts_chosen(const garb_font_face_t *f, const flow_family_name_t *name, bool bold,
                            const WebFontPick *pick)
{
    return f->style == pick->style && webfonts_nearest(f, bold ? 700 : 400) == pick->weight &&
           fold_eq(f->family, f->family_len, name->name, name->len);
}

// What served a family a layout asked for, once a name for its answers
// (s_web.laying, or s_arriving's): the best of the layout's answers, in
// WebFontServed's order — a name some run listed a face of is the page's
// faces, whatever its other runs found.
// A lookup that found nothing counts toward the name's MISSING font line,
// written when the layout's answers are the record's
// (webfonts_families_missing). A name the table cannot keep — past its
// 512, or with no memory — only flags the table: nothing a layout finds
// reaches the record before its tree is shown, and the fonts line then
// says those names have no line.
static void webfonts_served(WebFontFamilies *t, const char *name, size_t len, WebFontServed served,
                            const char *in)
{
    WebFontFamily *f = NULL;
    for (int32_t i = 0; i < t->n && f == NULL; i++)
        if (fold_eq(t->f[i].name, os64_strlen(t->f[i].name), name, len))
            f = &t->f[i];
    if (f == NULL) {
        if (t->n == t->cap && t->n < WEBFONT_FAMILIES_MAX) {
            int32_t cap = t->cap != 0 ? t->cap * 2 : 16;
            WebFontFamily *grown = os64_realloc(t->f, (size_t)cap * sizeof(*grown));
            if (grown != NULL) {
                t->f = grown;
                t->cap = cap;
            }
        }
        char *copy = os64_malloc(len + 1);
        if (copy != NULL) {
            os64_memcpy(copy, name, len);
            copy[len] = '\0';
        }
        if (copy == NULL || t->n == t->cap) {
            if (t->n == WEBFONT_FAMILIES_MAX)
                t->full = true;
            else
                t->short_of_memory = true;
            os64_free(copy);
            return;
        }
        f = &t->f[t->n++];
        *f = (WebFontFamily){.name = copy, .served = served};
        os64_strcopy(f->in, sizeof(f->in), in);
    } else if (served < f->served) {
        f->served = served;
        os64_strcopy(f->in, sizeof(f->in), in);
    }
    if (served == WF_SERVED_NONE && f->missing < UINT32_MAX)
        f->missing++;
}

static void webfonts_families_free(WebFontFamilies *t)
{
    for (int32_t i = 0; i < t->n; i++)
        os64_free(t->f[i].name);
    os64_free(t->f);
    os64_memset(t, 0, sizeof(*t));
}

// Each name the layout found nothing for is MISSING font on the record,
// counted as the most lookups one layout made for it.
static void webfonts_families_missing(const WebFontFamilies *t, yonder_diag_t *diag)
{
    for (int32_t i = 0; diag != NULL && i < t->n; i++)
        if (t->f[i].missing > 0)
            yonder_diag_missing_seen(diag, "font", t->f[i].name, t->f[i].missing);
}

// A layout begins: a layout of the shown document records its answers,
// and how its faces fared, afresh (s_web.laying, face_laying), and a page
// arriving its answers (s_arriving) — whether or not its text asks for a
// font at all.
static void webfonts_layout_begins(const Page *p)
{
    if (page_doc(p) == page_doc(&g.page)) {
        webfonts_families_free(&s_web.laying);
        s_web.laying_layout = geometry_layout_total;
        os64_memset(s_web.face_laying, 0, sizeof(s_web.face_laying));
    } else if (p->diag != NULL) {
        webfonts_families_free(&s_arriving.families);
        s_arriving.staged = true;
        s_arriving.seq = yonder_diag_seq(p->diag);
    }
}

// A tree is shown: on the shown page, what the layout that made it found
// is the record's — its families, how its faces fared, and its MISSING
// fonts, which the badge then counts. A layout that fails leaves the
// record saying what is on screen.
static void webfonts_families_shown(const Page *p)
{
    if (p != &g.page || s_web.laying_layout != geometry_layout_total)
        return;
    webfonts_families_free(&s_web.families);
    s_web.families = s_web.laying;
    os64_memset(&s_web.laying, 0, sizeof(s_web.laying));
    os64_memcpy(s_web.face_use, s_web.face_laying, sizeof(s_web.face_use));
    s_web.use_layout = s_web.laying_layout;
    webfonts_families_missing(&s_web.families, g.page.diag);
    badge_follow();
}

// The list is borrowed until the next call, which is libflow's contract
// for this callback too (flow_env_t.fonts). The names in their written
// order: the shown page's faces of a name that are open (webfonts_match,
// the later of a composite first), and the system's family for the first
// name no face serves that it knows (os64_font_family_open), else the
// generic, which comes last. The resolver opens no file: a face is
// another size of a file the text context already holds, as the system's
// sizes are the provider's. What served each name is on the record
// (webfonts_served), and a name with neither is MISSING font there: the
// shown page's, or a page arriving (s_arriving). The metrics are the first font's, as
// an empty run would report them: a line at least as tall as its ascent
// and descent.
static os64_font_status_t page_fonts(void *ctx, const flow_family_list_t *families, bool bold,
                                     bool italic, uint32_t px, os64_text_font_t *const **list,
                                     size_t *count, os64_font_face_info_t *primary)
{
    Page *p = ctx;
    // The shown page, or a copy of it a script's rebuild or a measurement
    // lays out: its document is the one on screen. A page arriving has
    // its own until it is shown (arrive_now).
    bool shown = p != NULL && page_doc(p) == page_doc(&g.page) && s_laying.cascade != NULL;
    const garb_font_face_t *faces = NULL;
    int32_t own = s_laying.cascade != NULL ? garb_cascade_font_faces(s_laying.cascade, &faces, NULL) : 0;
    int32_t nfaces = shown ? own : 0;
    // Where the answers are kept for the record: the shown page's
    // layout's, or a page arriving's.
    WebFontFamilies *record = NULL;
    if (shown)
        record = &s_web.laying;
    else if (p != NULL && p->diag != NULL && s_arriving.staged && s_arriving.seq == yonder_diag_seq(p->diag))
        record = &s_arriving.families;
    if (nfaces > 0)
        webfonts_chars(p);
    // The names no page face serves, and the names they do with how many
    // of each joined: as many as the list holds.
    os64_font_family_name_t small[WEBFONT_NAMES_MAX];
    os64_font_family_name_t *rest = small;
    WebFontAsked small_served[WEBFONT_NAMES_MAX];
    WebFontAsked *served = small_served;
    if (nfaces > 0 && families->count > WEBFONT_NAMES_MAX) {
        rest = os64_malloc((size_t)families->count * sizeof(*rest));
        served = os64_malloc((size_t)families->count * sizeof(*served));
        if (rest == NULL || served == NULL) {
            os64_free(rest);
            os64_free(served);
            // The layout may go on, and be shown, without this run's
            // answers: the record says families went.
            if (record != NULL)
                record->short_of_memory = true;
            return OS64_FONT_NO_MEMORY;
        }
    }
    uint32_t nrest = 0, nserved = 0;
    for (uint32_t k = 0; k < families->count && nfaces > 0; k++) {
        WebFontPick pick;
        if (!webfonts_match(faces, nfaces, &families->names[k], italic, bold, &pick))
            rest[nrest++] = (os64_font_family_name_t){families->names[k].name, families->names[k].len};
        else
            served[nserved++] = (WebFontAsked){families->names[k].name, families->names[k].len, 0};
    }
    os64_font_family_list_t asked = {
        .names = nfaces > 0 ? rest : (const os64_font_family_name_t *)families->names,
        .count = nfaces > 0 ? nrest : families->count,
        .generic = (os64_font_family_t)families->generic,
    };
    os64_font_role_view_t view;
    os64_font_status_t st = os64_font_family_open(s_faces.families, &asked, bold, italic, px, &view);
    // The name the system's family answers: the first the provider knows,
    // which is the one it chose; the count when it chose the generic.
    uint32_t system_at = families->count;
    for (uint32_t r = 0; r < asked.count && system_at == families->count; r++)
        if (os64_font_family_known(s_faces.families, &asked.names[r]))
            for (uint32_t k = 0; k < families->count; k++)
                if (families->names[k].name == asked.names[r].name)
                    system_at = k;
    if (st != OS64_FONT_OK) {
        // As above, for want of memory or of the text engine's room.
        if (record != NULL && (st == OS64_FONT_NO_MEMORY || st == OS64_FONT_LIMIT))
            record->short_of_memory = true;
        if (rest != small)
            os64_free(rest);
        if (served != small_served)
            os64_free(served);
        return st;
    }
    size_t keep = view.font_count < OS64_TEXT_FALLBACK_MAX ? OS64_TEXT_FALLBACK_MAX - view.font_count : 0;
    // The page's faces, in order, while the list has a place: a file
    // coming, or a size of one refused for room, holds a place, once, so
    // the faces after it are not fetched while it may yet draw; a face
    // whose file failed, or whose size will not open for another reason,
    // gives its place to the next. A face whose ranges miss the document's
    // characters, or with more than a text font takes, is passed over —
    // even when another rule already fetched its file. A composite is
    // walked from its latest rule through every one, so an earlier face
    // serves when the later ones cannot.
    os64_text_font_t *mine[WEBFONT_LIST_MAX];
    uint32_t mine_name[WEBFONT_LIST_MAX];
    int32_t pending[WEBFONT_LIST_MAX];
    size_t nmine = 0, npending = 0, places = 0;
    for (uint32_t k = 0; k < families->count && nfaces > 0 && places < keep; k++) {
        WebFontPick pick;
        if (!webfonts_match(faces, nfaces, &families->names[k], italic, bold, &pick))
            continue;
        for (int32_t i = nfaces - 1; i >= 0 && places < keep; i--) {
            const garb_font_face_t *f = &faces[i];
            if (!webfonts_chosen(f, &families->names[k], bold, &pick) || f->nranges > (int32_t)OS64_TEXT_RANGES_MAX ||
                !webfonts_meet(f))
                continue;
            int32_t file = webfonts_file(p, f, i);
            if (file < 0)
                continue;
            bool ready = s_web.files[file].state == WF_READY, room = false;
            os64_text_font_t *font = ready ? webfonts_handle(file, f, px, &room) : NULL;
            if (ready)
                s_web.face_laying[i] |= font != NULL ? WF_USE_LISTED : room ? WF_USE_ROOM : WF_USE_REFUSED;
            if (ready && font == NULL && !room)
                continue;
            bool dup = false;
            if (font == NULL)
                for (size_t j = 0; j < npending; j++)
                    dup |= pending[j] == file;
            else
                for (size_t j = 0; j < nmine; j++)
                    dup |= mine[j] == font;
            if (font == NULL && !dup) {
                pending[npending++] = file;
                places++;
            } else if (font != NULL && !dup) {
                mine_name[nmine] = k;
                mine[nmine++] = font;
                places++;
            }
            for (uint32_t j = 0; font != NULL && j < nserved; j++)
                served[j].joined += served[j].name == families->names[k].name;
        }
    }
    // The names' written order: the page's faces of the names before the
    // system family the provider chose, that family's face, the page's
    // faces of the names after it, and last the provider's fallbacks and
    // its builtin — or, with no system family by name, every page face
    // and then the generic with its fallbacks.
    bool named = system_at < families->count;
    size_t n = 0;
    for (size_t i = 0; i < nmine; i++)
        if (mine_name[i] < system_at)
            s_web.list[n++] = mine[i];
    bool page_first = n > 0;
    if (named)
        s_web.list[n++] = view.fonts[0];
    for (size_t i = 0; i < nmine; i++)
        if (mine_name[i] > system_at)
            s_web.list[n++] = mine[i];
    for (size_t i = named ? 1 : 0; i < view.font_count; i++)
        s_web.list[n++] = view.fonts[i];
    *list = s_web.list;
    *count = n;
    if (page_first)
        (void)os64_text_font_info(s_web.list[0], primary);
    else
        *primary = view.primary;
    if (primary->line_height < primary->ascent + primary->descent)
        primary->line_height = primary->ascent + primary->descent;
    // What served each name, on the record, with the font the run's list
    // begins with: a page face the list puts before the provider's, too.
    for (uint32_t k = 0; record != NULL && k < asked.count; k++) {
        const os64_font_family_name_t *nm = &asked.names[k];
        bool system = os64_font_family_known(s_faces.families, nm);
        // A page arriving draws in none of its faces yet: a name they
        // answer is theirs, none in the list.
        WebFontPick pick;
        bool faced = !shown && own > 0 &&
                     webfonts_match(faces, own, &(flow_family_name_t){nm->name, (uint32_t)nm->len}, italic, bold, &pick);
        webfonts_served(record, nm->name, nm->len,
                        faced ? WF_SERVED_PAGE_UNLISTED : system ? WF_SERVED_SYSTEM : WF_SERVED_NONE, primary->family);
    }
    for (uint32_t k = 0; shown && k < nserved; k++)
        webfonts_served(&s_web.laying, served[k].name, served[k].len,
                        served[k].joined > 0 ? WF_SERVED_PAGE : WF_SERVED_PAGE_UNLISTED, primary->family);
    if (rest != small)
        os64_free(rest);
    if (served != small_served)
        os64_free(served);
    return OS64_FONT_OK;
}

// The page is to be laid out again (pictures_settle) — and now, so a move
// made after a settle has run (by the feed, after its layout) is not left
// waiting for an event: the ticker is armed for this moment.
static void webfonts_move(void)
{
    s_web.moved = true;
    g.settle_due = yonder_now_ms();
    pictures_schedule();
}

// A file opened in the page's text context, at 16 px with every scalar.
static os64_font_status_t webfonts_open(WebFontFile *w, const uint8_t *sfnt, size_t len)
{
    os64_font_face_options_t o = {16, OS64_FONT_HINT_NORMAL};
    return os64_text_font_open(s_faces.text, sfnt, len, &o, &w->base);
}

static const char *webfonts_refusal(os64_font_status_t st)
{
    return st == OS64_FONT_UNSUPPORTED ? "a font yonder does not draw (a variable font or a collection)"
                                       : "would not open";
}

// A file is failed: named in the page's record, and the badge told. One
// whose fetch decided it (`fetched`: the job came back, or its bytes would
// not open) is FAILED font-fetch, a kind of its own because a replay,
// which fetches no font, cannot reproduce it; one never fetched is FAILED
// font.
static void webfonts_fail(WebFontFile *w, const char *why, bool fetched)
{
    w->state = WF_FAILED;
    w->failed_layout = geometry_layout_total;
    char line[OS64_FETCH_URL_MAX + 80];
    os64_snprintf(line, sizeof(line), "%s: %s", w->url, why);
    yonder_diag_failed(g.page.diag, fetched ? "font-fetch" : "font", line);
    badge_follow();
    // The record is written again once nothing is out and the page is laid
    // out (diag_pictures_in), whichever way the failure came.
    if (g.page.diag != NULL)
        g.page.record_owed = true;
}

// The files wanted and not yet asked for handed to the pool, at most
// WEBFONTS_AT_ONCE out at a time; a file that cannot be asked for is
// failed, and the page laid out again for its next source. After a layout
// of the shown page (`laid`), which let the old tree's room go, the files
// held for room are opened once more — refused again, they fail — and a
// size refused for room gets its one more layout. A source the layout
// could not keep (webfonts_file) is the badge's, and owes the record.
static void webfonts_feed(bool laid)
{
    if (s_web.unkept) {
        s_web.unkept = false;
        badge_follow();
        if (g.page.diag != NULL)
            g.page.record_owed = true;
    }
    for (int32_t i = 0; laid && i < s_web.nfiles; i++) {
        WebFontFile *w = &s_web.files[i];
        if (w->state != WF_HELD)
            continue;
        os64_font_status_t st = webfonts_open(w, w->sfnt, w->sfnt_len);
        os64_free(w->sfnt);
        w->sfnt = NULL;
        s_web.held_bytes -= w->sfnt_len;
        if (st == OS64_FONT_OK) {
            w->state = WF_READY;
            w->ready_layout = geometry_layout_total;
        } else if (st == OS64_FONT_LIMIT || st == OS64_FONT_NO_MEMORY)
            webfonts_fail(w, "no room to open it", true);
        else
            webfonts_fail(w, webfonts_refusal(st), true);
        webfonts_move();
    }
    // A size refused for room gets one more layout: the tree that held the
    // room is gone after it. Refused again, the fallback stands — for that
    // size, until a layout refuses none or the page is laid out at another
    // zoom or width, when its sizes are new ones. A new tree at the same
    // zoom and width (a height alone changed, a script's rebuild) forgets
    // nothing, though it may have let the room go: BROWSER_DEBTS.md.
    if (laid) {
        if (g.page.laid_zoom != s_web.retried_zoom || g.page.laid_width != s_web.retried_width) {
            s_web.nretried = 0;
            s_web.retried_zoom = g.page.laid_zoom;
            s_web.retried_width = g.page.laid_width;
        }
        bool again = false;
        for (int32_t i = 0; i < s_web.nrefused; i++) {
            bool tried = false;
            for (int32_t j = 0; j < s_web.nretried; j++)
                tried |= s_web.retried[j] == s_web.refused[i];
            if (!tried && s_web.nretried < WEBFONT_REFUSED_MAX) {
                s_web.retried[s_web.nretried++] = s_web.refused[i];
                again = true;
            }
        }
        if (s_web.nrefused == 0)
            s_web.nretried = 0;
        s_web.nrefused = 0;
        if (again)
            webfonts_move();
    }
    while (s_web.next < s_web.nfiles && s_web.in_flight < WEBFONTS_AT_ONCE) {
        WebFontFile *w = &s_web.files[s_web.next];
        if (w->state != WF_QUEUED) {
            s_web.next++;
            continue;
        }
        const char *why = NULL;
        yonder_webfont_job_t *job = g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
        if (g.pool == NULL) {
            why = "no worker to fetch it";
        } else if (job == NULL) {
            why = "no memory to ask for it";
        } else {
            *job = (yonder_webfont_job_t){.kind = YONDER_JOB_FONT, .index = s_web.next,
                                          .generation = g.page_serial, .agent = g.way.agent};
            os64_strcopy(job->url, sizeof(job->url), w->url);
            job->hooks.jar = g.way.jar;
            job->hooks.cache = g.cache;
            os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), g.page.way.url);
            hooks_ask(&job->hooks, g.page.way.url, WAY_DEST_FONT);
            os64_work_t work = {yonder_webfont_run, yonder_webfont_release, job, WEBFONT_RESERVE};
            w->id = os64_work_submit(g.pool, &work);
            if (w->id == 0) {
                os64_free(job);
                why = "the workers would not take it";
            }
        }
        if (why != NULL) {
            s_web.waiting--;
            webfonts_fail(w, why, false);
            webfonts_move();
        } else {
            w->state = WF_WAITING;
            s_web.in_flight++;
        }
        s_web.next++;
    }
}

// A layout of the shown page failed, so the room the held files wait for
// was not let go: each fails, its bytes freed, rather than waiting for a
// layout that may not come. The page is not moved for them — the failed
// layout's moves were consumed so it is not retried at every batch — so
// each face's next source is asked for by whichever layout comes next,
// and until then the record says it is not asked for yet.
static void webfonts_unhold(void)
{
    for (int32_t i = 0; i < s_web.nfiles; i++) {
        WebFontFile *w = &s_web.files[i];
        if (w->state != WF_HELD)
            continue;
        os64_free(w->sfnt);
        w->sfnt = NULL;
        s_web.held_bytes -= w->sfnt_len;
        webfonts_fail(w, "no room to open it", true);
    }
}

// A file came back: opened in the page's text context, held for room
// (WF_HELD), or named as failed — and the next layout draws the face from
// its next source.
static void webfont_arrived(const yonder_webfont_job_t *job, yonder_webfont_t *product)
{
    if (job->generation != g.page_serial || job->index < 0 || job->index >= s_web.nfiles)
        return;                     // a page no longer on screen
    diag_cookies_short(g.page.diag, &job->hooks.cookies_short);
    WebFontFile *w = &s_web.files[job->index];
    if (w->state != WF_WAITING)
        return;
    s_web.waiting--;
    s_web.in_flight--;
    if (product != NULL && product->fetched) {
        w->container = product->container;
        w->got = product->got;
    }
    const char *why = product == NULL ? "no memory to fetch it" : product->why[0] ? product->why : NULL;
    if (why == NULL) {
        os64_font_status_t st = webfonts_open(w, product->sfnt, product->len);
        bool room = st == OS64_FONT_LIMIT || st == OS64_FONT_NO_MEMORY;
        if (room && product->len <= WEBFONT_HELD_MAX - s_web.held_bytes) {
            // Room, not the file: held, and opened once more after the
            // next layout, which this asks for (webfonts_feed).
            w->state = WF_HELD;
            w->sfnt = product->sfnt;
            w->sfnt_len = product->len;
            s_web.held_bytes += product->len;
            product->sfnt = NULL;
            webfonts_move();
            return;
        }
        if (room)
            why = "no room to open it";
        else if (st != OS64_FONT_OK)
            why = webfonts_refusal(st);
    }
    if (why != NULL) {
        webfonts_fail(w, why, true);
    } else {
        w->state = WF_READY;
        w->ready_layout = geometry_layout_total;
    }
    // Either way the page is laid out again: a face to draw, or the next
    // source to want.
    webfonts_move();
    mark_whole(WHOLE_FACE, false);
}

// The workers stopped with files out: each one with them is named as
// failed, and the page laid out again for each face's next source; each
// still queued goes through webfonts_feed, which names it too; then the
// record is written.
static void webfonts_orphaned(void)
{
    bool any = false;
    for (int32_t i = 0; i < s_web.nfiles; i++) {
        WebFontFile *w = &s_web.files[i];
        if (w->state != WF_WAITING)
            continue;
        s_web.waiting--;
        webfonts_fail(w, "the workers stopped", false);
        webfonts_move();
        any = true;
    }
    s_web.in_flight = 0;
    webfonts_feed(false);
    if (any && g.page.waiting == 0)
        diag_pictures_in(&g.page);
}

// The page is being left: its files in the pool are cancelled, and every
// face and file let go — a run on the old tree that holds one keeps it
// until the tree goes.
static void webfonts_leave(void)
{
    for (int32_t i = 0; i < s_web.nfiles; i++) {
        if (s_web.files[i].state == WF_WAITING && g.pool != NULL)
            os64_work_cancel(g.pool, s_web.files[i].id);
        os64_text_font_release(s_web.files[i].base);
        os64_free(s_web.files[i].sfnt);
        os64_free(s_web.files[i].url);
    }
    for (int32_t i = 0; i < s_web.nhandles; i++)
        os64_text_font_release(s_web.handles[i].font);
    webfonts_families_free(&s_web.families);
    webfonts_families_free(&s_web.laying);
    os64_free(s_web.files);
    os64_memset(&s_web, 0, sizeof(s_web));
}

// ── The page's fonts on its record (BROWSER_DEBTS, S9a4) ───────────────

static const char *webfonts_container(yonder_font_kind_t k)
{
    return k == YONDER_FONT_SFNT ? "SFNT" : k == YONDER_FONT_WOFF ? "WOFF" : k == YONDER_FONT_WOFF2 ? "WOFF2"
                                                                                                   : "not a font";
}

// The ranges a face line spells out before it counts the rest.
#define DIAG_FACE_RANGES 64

// A record line built whole, however long it comes out: what a page wrote
// (a family, a source's address, a format) may be any length. With no
// memory for the rest, `short_of_memory`, and the line says so rather than
// ending short (diag_face).
typedef struct {
    char *s;
    size_t n, cap;
    bool short_of_memory;
} DiagText;

static void dt_put(DiagText *t, const char *fmt, ...) OS64_PRINTF(2, 3);
static void dt_put(DiagText *t, const char *fmt, ...)
{
    if (t->short_of_memory)
        return;
    va_list args, again;
    va_start(args, fmt);
    va_copy(again, args);
    char probe[1];
    int32_t need = os64_vsnprintf(probe, sizeof(probe), fmt, args);
    va_end(args);
    if (need > 0 && t->n + (size_t)need + 1 > t->cap) {
        size_t cap = t->cap != 0 ? t->cap : 256;
        while (cap < t->n + (size_t)need + 1)
            cap *= 2;
        char *grown = os64_realloc(t->s, cap);
        if (grown == NULL) {
            t->short_of_memory = true;
            va_end(again);
            return;
        }
        t->s = grown;
        t->cap = cap;
    }
    if (need > 0) {
        os64_vsnprintf(t->s + t->n, t->cap - t->n, fmt, again);
        t->n += (size_t)need;
    }
    va_end(again);
}

// A weight as the rule wrote it: whole, or to two places.
static void diag_weight(char *out, size_t cap, float w)
{
    uint32_t hundredths = (uint32_t)(w * 100.0f + 0.5f);
    if (hundredths % 100 == 0)
        os64_snprintf(out, cap, "%u", (unsigned)(hundredths / 100));
    else if (hundredths % 10 == 0)
        os64_snprintf(out, cap, "%u.%u", (unsigned)(hundredths / 100), (unsigned)(hundredths % 100 / 10));
    else
        os64_snprintf(out, cap, "%u.%02u", (unsigned)(hundredths / 100), (unsigned)(hundredths % 100));
}

// A face's source whose file is open, as the layout of the tree on screen
// found the face (s_web.face_use): listed, or not and why — the file may
// be another face's that shares it. A file opened since that layout has
// not been in a list since it came.
static void diag_ready(DiagText *t, const char *url, const WebFontFile *w, int32_t face)
{
    bool since = w->ready_layout < s_web.use_layout;
    uint8_t use = since ? s_web.face_use[face] : 0;
    bool listed = use & WF_USE_LISTED, room = use & WF_USE_ROOM, refused = use & WF_USE_REFUSED;
    dt_put(t, "%s %s (%s, %lu bytes)", url, listed ? "listed" : "came", webfonts_container(w->container),
           (unsigned long)w->got);
    if (!since)
        dt_put(t, ", not listed since it came");
    else if (room || refused)
        dt_put(t, "%s%s%s%s", listed ? ", but " : ", not listed: ", room ? "a size refused for room" : "",
               room && refused ? ", " : "", refused ? "a size that would not open" : "");
    else if (!listed)
        dt_put(t, ", not listed: no text asked for it, or its lists had no place");
}

// One face on the record: its family, weights, style and ranges, and
// each of its sources in order up to the one it is drawn from — all of
// them when it is drawn from none — open
// (diag_ready), coming, held for room to open, failed (the reason is its
// FAILED font line, a source the table could not keep included), a
// format() or local() passed over — or why it was not asked for: not yet,
// before the page is laid out on screen in its faces (and always on a page
// let go before it was shown) or after a source that failed since that
// layout, or no text asked.
// A face with more ranges than a text font takes, or whose ranges miss
// the page's text, says only that: it is passed over whatever its file.
// The line is whole, however long, but for the ranges past the first
// DIAG_FACE_RANGES, which it counts.
static void diag_face(Page *p, int32_t i, const garb_font_face_t *f, bool shown)
{
    DiagText t = {0};
    char lo[16], hi[16];
    diag_weight(lo, sizeof(lo), f->weight_min);
    diag_weight(hi, sizeof(hi), f->weight_max);
    dt_put(&t, "%.*s %s", (int)f->family_len, f->family, lo);
    if (f->weight_max != f->weight_min)
        dt_put(&t, "-%s", hi);
    dt_put(&t, " %s", f->style == GARB_FACE_ITALIC ? "italic" : f->style == GARB_FACE_OBLIQUE ? "oblique" : "normal");
    // The first ranges and how many more: a rule may write thousands.
    for (int32_t r = 0; r < f->nranges && r < DIAG_FACE_RANGES; r++)
        dt_put(&t, "%sU+%X-%X", r ? "," : " ", (unsigned)f->ranges[r].first, (unsigned)f->ranges[r].last);
    if (f->nranges > DIAG_FACE_RANGES)
        dt_put(&t, " and %d more", (int)(f->nranges - DIAG_FACE_RANGES));
    dt_put(&t, ": ");
    if (f->nranges > (int32_t)OS64_TEXT_RANGES_MAX) {
        dt_put(&t, "passed over: %d ranges, past the %u a font takes", (int)f->nranges, (unsigned)OS64_TEXT_RANGES_MAX);
    } else if (!webfonts_meet(f)) {
        dt_put(&t, "not needed: its range is not in the page's text");
    } else {
        bool failed_late = false;
        for (int32_t k = 0; k < f->nsrc; k++) {
            const garb_face_src_t *s = &f->src[k];
            bool last = false;
            if (k > 0)
                dt_put(&t, "; ");
            if (s->kind == GARB_FACE_LOCAL) {
                dt_put(&t, "local(%.*s) passed over", (int)s->len, s->text);
            } else if (!webfonts_readable(s)) {
                dt_put(&t, "format(%.*s) not read: %.*s", (int)s->format_len, s->format, (int)s->len, s->text);
            } else {
                char url[OS64_FETCH_URL_MAX];
                if (!webfonts_url(p, p->cascade_entry, f, s, url, sizeof(url))) {
                    dt_put(&t, "%.*s, an address yonder does not fetch", (int)s->len, s->text);
                } else if (!shown) {
                    // The files are the shown page's: this page asked for none.
                    dt_put(&t, "%s not asked for yet: a page is laid out in its faces once it is shown", url);
                } else {
                    int32_t file = webfonts_find(url);
                    const WebFontFile *w = file >= 0 ? &s_web.files[file] : NULL;
                    // A layout that met this face tried each source in
                    // turn, so one it could not keep is a failed one.
                    bool unkept = w == NULL && (s_web.face_use[i] & WF_USE_UNKEPT);
                    // The face is drawn from the first source whose file is
                    // kept and has not failed, so the sources after it say
                    // nothing; a face no layout asked for is drawn from
                    // none, and every source is said.
                    last = w != NULL && w->state != WF_FAILED;
                    if (unkept || (w != NULL && w->state == WF_FAILED))
                        dt_put(&t, "%s failed", url);
                    else if (w == NULL && s_web.use_layout == 0)
                        dt_put(&t, "%s not asked for yet: a page is laid out in its faces once it is shown", url);
                    else if (w == NULL && failed_late)
                        dt_put(&t, "%s not asked for yet: the source before it failed since the page was laid out", url);
                    else if (w == NULL)
                        dt_put(&t, "%s never asked for: no text asked for it, or its lists had no place", url);
                    else if (w->state == WF_READY)
                        diag_ready(&t, url, w, i);
                    else if (w->state == WF_HELD)
                        dt_put(&t, "%s came (%s, %lu bytes), held for room to open", url,
                               webfonts_container(w->container), (unsigned long)w->got);
                    else
                        dt_put(&t, "%s coming", url);
                    // Once a source has failed since the layout on screen,
                    // that layout stopped at or before it: every source
                    // after it, past older failures too, is for the next
                    // layout to ask for.
                    if (w != NULL && w->state == WF_FAILED && w->failed_layout >= s_web.use_layout)
                        failed_late = true;
                }
            }
            if (last)
                break;
        }
    }
    char key[32];
    os64_snprintf(key, sizeof(key), "font face %d", (int)i + 1);
    yonder_diag_fact(p->diag, key,
                     t.short_of_memory || t.s == NULL ? "no memory to write this face's line" : t.s);
    os64_free(t.s);
}

// The main loop's work since the page was shown, or since its record was
// last written on asking (`d`, view_key): how many paints and how much of
// the view they painted, the ticks, schedules, layouts and script tasks,
// the ticker's bells and the deadlines handed it — every count, painted or
// not (`t`) — and, when the span has a rate, how busy the loop was and
// each stage in milliseconds a paint (a tick, a call, a task, a layout for
// those) — on average and at most — the counter's rate taken from
// os64_micros over the same span (`u`). "window" is the render outside the
// view (the root panel, the chrome); "the view's rest" is the view outside
// its timed stages. False when there is no rate: `t` says why, `u` is
// empty.
static bool paint_lines(DiagText *t, DiagText *u)
{
    uint64_t span_us = (uint64_t)(os64_micros() - s_cost.from_us);
    // The whole span, read once, as every interval is (cycles_since): one
    // whose counter stepped back has no rate, and the line says so.
    uint64_t back = s_cost.backward;
    uint64_t span = cycles_since(s_cost.from_cycles);
    bool stepped_back = s_cost.backward != back;
    uint64_t rate = span_us > 0 ? span / span_us : 0;   // per microsecond
    if (s_cost.paints == 0)
        dt_put(t, "nothing painted in %lu.%lu s", (unsigned long)(span_us / 1000000),
               (unsigned long)(span_us / 100000 % 10));
    else
        dt_put(t, "%lu paints in %lu.%lu s (%lu whole views), %lu.%lu Mpx of the view", (unsigned long)s_cost.paints,
               (unsigned long)(span_us / 1000000), (unsigned long)(span_us / 100000 % 10),
               (unsigned long)s_cost.whole, (unsigned long)(s_cost.pixels / 1000000),
               (unsigned long)(s_cost.pixels / 100000 % 10));
    dt_put(t, "; %lu ticks, %lu schedules", (unsigned long)s_cost.ticks, (unsigned long)s_cost.schedules);
    if (rate == 0)
        dt_put(t, stepped_back ? "; the counter stepped back across the span (the cores' counters disagree): no times"
                               : "; too short a span to time");
    else
        dt_put(t, "; the main loop busy %lu%%",
               (unsigned long)(span > s_cost.waited ? (span - s_cost.waited) * 100 / span : 0));
    if (s_cost.backward != 0)
        dt_put(t, "; the counter stepped back %lu times (the cores' counters disagree: these times are suspect)",
               (unsigned long)s_cost.backward);
    dt_put(t, "; %lu layouts, %lu of them for animations, %lu of those repainting only what they changed",
           (unsigned long)s_cost.layouts, (unsigned long)s_cost.layouts_motion,
           (unsigned long)s_cost.layouts_parts);
    dt_put(t, "; frames late at most %lu ms, %lu more than %d ms", (unsigned long)s_cost.late_most,
           (unsigned long)s_cost.late_frames, FRAME_LATE_MS);
    if (rate != 0)
        dt_put(t, "; the longest wait %lu ms", (unsigned long)(s_cost.slept_most / rate / 1000));
    // What the painter kept (YONDER_PERF.md § perf-2).
    yonder_paint_cache_counts_t kept = yonder_paint_cache_counts(g.paint_cache);
    if (g.paint_cache != NULL)
        dt_put(t, "; the paint cache: %lu blocks found, %lu made, %lu let go for room; pictures worked out "
               "without it %lu times for room, %lu for memory; %lu KiB held in %lu blocks, of %lu KiB",
               (unsigned long)kept.hits, (unsigned long)kept.made, (unsigned long)kept.evicted,
               (unsigned long)kept.no_room, (unsigned long)kept.no_memory,
               (unsigned long)(kept.bytes / 1024), (unsigned long)kept.entries, (unsigned long)(kept.budget / 1024));
    else
        dt_put(t, "; no paint cache");
    dt_put(t, "; %lu script tasks: %lu scripts, %lu timers, %lu events",
           (unsigned long)(s_cost.scripts + s_cost.timers + s_cost.events), (unsigned long)s_cost.scripts,
           (unsigned long)s_cost.timers, (unsigned long)s_cost.events);

    // The bells the loop took, then the ticker's own counts: its rings —
    // more than the bells when rings were made before the loop took one —
    // and none once it has stopped, on the way out, or when it never
    // started.
    dt_put(t, "; %lu bells taken", (unsigned long)s_cost.bells);
    yonder_ticker_counts_t ticker = yonder_ticker_counts(g.ticker);
    if (g.ticker != NULL)
        dt_put(t, "; the ticker rang %lu times, woke %lu times, rang at most %lu ms late, slept at most %lu ms "
               "past its time", (unsigned long)ticker.rings, (unsigned long)ticker.wakes,
               (unsigned long)ticker.rang_late_most, (unsigned long)ticker.overslept_most);
    else
        dt_put(t, "; the ticker not running");
    dt_put(t, "; deadlines already past when handed to it:");
    for (int32_t i = 0; i < PAST_COUNT; i++)
        dt_put(t, "%s %s %lu", i ? "," : "", kPastDeadline[i], (unsigned long)s_cost.past[i]);
    if (rate == 0)
        return false;
    // Tenths of a millisecond: cycles / (rate per microsecond x 100).
    uint64_t per = rate * 100;
    uint64_t inner = 0;
    for (int32_t s = PS_BACKGROUNDS; s <= PS_WARPS; s++)
        inner += s_cost.total[s];
    uint64_t window = s_cost.total[PS_RENDER] > s_cost.total[PS_VIEW] ? s_cost.total[PS_RENDER] - s_cost.total[PS_VIEW] : 0;
    uint64_t rest = s_cost.total[PS_VIEW] > inner ? s_cost.total[PS_VIEW] - inner : 0;
    dt_put(u, "ms a paint (most):");
    for (int32_t s = 0; s < PS_COUNT; s++) {
        uint64_t n = s == PS_TICKS ? s_cost.ticks : s == PS_SCHEDULE ? s_cost.schedules
                   : s == PS_TASKS ? s_cost.scripts + s_cost.timers + s_cost.events
                   : s == PS_LAYOUTS ? s_cost.layouts
                   : s_cost.paints;
        uint64_t avg = n > 0 ? s_cost.total[s] / n / per : 0, most = s_cost.most[s] / per;
        dt_put(u, "%s %s %lu.%lu (%lu.%lu)", s ? "," : "",
               s == PS_TICKS ? "ticks (a tick)" : s == PS_SCHEDULE ? "schedule (a call)"
               : s == PS_TASKS ? "tasks (a task)" : s == PS_LAYOUTS ? "layouts (a layout)" : kPaintStage[s],
               (unsigned long)(avg / 10), (unsigned long)(avg % 10), (unsigned long)(most / 10),
               (unsigned long)(most % 10));
    }
    uint64_t paints = s_cost.paints > 0 ? s_cost.paints : 1;
    uint64_t w = window / paints / per, r = rest / paints / per;
    dt_put(u, "; window %lu.%lu, the view's rest %lu.%lu", (unsigned long)(w / 10), (unsigned long)(w % 10),
           (unsigned long)(r / 10), (unsigned long)(r % 10));
    return true;
}

static const char *dt_text(const DiagText *t)
{
    return t->short_of_memory || t->s == NULL ? "no memory to write this line" : t->s;
}

// The counts so far, as `paint` and `paint stages` (paint_lines).
// The paints of the whole view by what marked them (WholeBy), then the
// layouts for the animations that marked it by why (s_parts.why), and the
// last of those with what refused: a page painting whole views says which
// rule to take up. False, with nothing written, when no paint was whole.
static bool whole_lines(DiagText *w)
{
    if (s_cost.whole == 0)
        return false;
    dt_put(w, "%lu of %lu paints:", (unsigned long)s_cost.whole, (unsigned long)s_cost.paints);
    const char *sep = "";
    for (int32_t i = 0; i < WHOLE_COUNT; i++)
        if (s_cost.whole_by[i] != 0) {
            dt_put(w, "%s %lu %s", sep, (unsigned long)s_cost.whole_by[i], kWholeBy[i]);
            sep = ",";
        }
    dt_put(w, "; the dirty list: %lu rectangles, %lu at most in a paint, %lu full lists made one",
           (unsigned long)s_cost.rects, (unsigned long)s_cost.rects_most,
           (unsigned long)(g.ui.damage_collapses - s_cost.collapses_from));
    uint64_t refused = 0;
    for (int32_t i = 0; i < REFUSED_COUNT; i++)
        refused += s_cost.refused[i];
    if (refused == 0)
        return true;
    dt_put(w, "; layouts for animations that marked the whole view, by why:");
    sep = "";
    for (int32_t i = 0; i < REFUSED_COUNT; i++)
        if (s_cost.refused[i] != 0) {
            dt_put(w, "%s %lu %s", sep, (unsigned long)s_cost.refused[i], kRefused[i]);
            sep = ",";
        }
    dt_put(w, "; the last: %s%s%s", kRefused[s_cost.refused_last], s_cost.refused_what[0] != '\0' ? ", " : "",
           s_cost.refused_what);
    return true;
}

static void diag_paint(Page *p)
{
    DiagText t = {0}, u = {0}, w = {0};
    bool timed = paint_lines(&t, &u);
    yonder_diag_fact(p->diag, "paint", dt_text(&t));
    if (timed)
        yonder_diag_fact(p->diag, "paint stages", dt_text(&u));
    else
        yonder_diag_forget(p->diag, "paint stages");
    if (whole_lines(&w))
        yonder_diag_fact(p->diag, "whole views", dt_text(&w));
    else
        yonder_diag_forget(p->diag, "whole views");
    os64_free(t.s);
    os64_free(u.s);
    os64_free(w.s);
}

// The counts kept as reading `n` of the page, with where the page was
// scrolled, before they start again (diag_asked): each `d` adds one.
static void diag_paint_reading(Page *p, uint32_t n)
{
    DiagText t = {0}, u = {0}, v = {0}, w = {0};
    bool timed = paint_lines(&t, &u);
    bool whole = whole_lines(&w);
    dt_put(&v, "at y %ld: %s%s%s%s%s", (long)g.sy, dt_text(&t), timed ? " | " : "", timed ? dt_text(&u) : "",
           whole ? " | whole views: " : "", whole ? dt_text(&w) : "");
    char key[32];
    os64_snprintf(key, sizeof(key), "paint reading %u", (unsigned)n);
    yonder_diag_fact(p->diag, key, dt_text(&v));
    os64_free(t.s);
    os64_free(u.s);
    os64_free(v.s);
    os64_free(w.s);
}

// A page's fonts on its record, at each write: how many faces and files
// and how they stand, each face (diag_face), and each family the tree on
// screen asked for with what served it (s_web.families). A page let go
// before it was shown has fetched nothing: its faces are its rules, and
// its families the ones its layout staged (s_arriving), with their
// MISSING lines. A page with none says nothing.
static void diag_fonts(Page *p)
{
    if (p->diag == NULL)
        return;
    bool shown = p == &g.page;
    const WebFontFamilies none = {0};
    const WebFontFamilies *fams = &s_web.families;
    if (!shown) {
        bool staged = s_arriving.staged && s_arriving.seq == yonder_diag_seq(p->diag);
        fams = staged ? &s_arriving.families : &none;
        webfonts_families_missing(fams, p->diag);
    }
    const garb_font_face_t *faces = NULL;
    uint32_t lost = 0;
    int32_t n = p->cascade != NULL ? garb_cascade_font_faces(p->cascade, &faces, &lost) : 0;
    // The section is written whole each time, in its order: a script's
    // rebuild may have left fewer faces, or none.
    yonder_diag_forget(p->diag, "fonts");
    yonder_diag_forget(p->diag, "font ");
    if (n == 0 && lost == 0 && fams->n == 0 && !fams->full && !fams->short_of_memory)
        return;
    int32_t open = 0, failed = 0, coming = 0, nfiles = shown ? s_web.nfiles : 0;
    for (int32_t i = 0; i < nfiles; i++) {
        open += s_web.files[i].state == WF_READY;
        failed += s_web.files[i].state == WF_FAILED;
        coming += s_web.files[i].state == WF_QUEUED || s_web.files[i].state == WF_WAITING ||
                  s_web.files[i].state == WF_HELD;
    }
    char v[320];
    char full[112] = "";
    if (fams->full)
        os64_snprintf(full, sizeof(full), "; more families than the %d the record keeps, the rest on no line here",
                      (int)WEBFONT_FAMILIES_MAX);
    char more[176];
    os64_snprintf(more, sizeof(more), "%s%s", full,
                  fams->short_of_memory ? "; families not kept: no memory for them, nor a line here" : "");
    os64_snprintf(v, sizeof(v), "%d faces (%u past the most a page keeps), %d files: %d open, %d failed, %d coming%s",
                  (int)n, (unsigned)lost, (int)nfiles, (int)open, (int)failed, (int)coming, more);
    yonder_diag_fact(p->diag, "fonts", v);
    // The page's characters as they are now, for the faces' ranges: the
    // tree on screen may be one laid out before the page was shown, which
    // looked for no face's range.
    if (n > 0)
        webfonts_chars(p);
    for (int32_t i = 0; i < n; i++)
        diag_face(p, i, &faces[i], shown);
    for (int32_t i = 0; i < fams->n; i++) {
        const WebFontFamily *fam = &fams->f[i];
        // The whole name in the key: two long ones that begin alike are two.
        size_t klen = os64_strlen("font family ") + os64_strlen(fam->name) + 1;
        char *key = os64_malloc(klen), value[OS64_FONT_NAME_CAP + 64];
        if (key == NULL) {
            yonder_diag_short(p->diag);
            continue;
        }
        os64_snprintf(key, klen, "font family %s", fam->name);
        if (fam->served == WF_SERVED_PAGE)
            os64_strcopy(value, sizeof(value), "the page's faces");
        else if (fam->served == WF_SERVED_PAGE_UNLISTED)
            os64_snprintf(value, sizeof(value), "the page's faces, none in the list, which begins with %s", fam->in);
        else if (fam->served == WF_SERVED_SYSTEM)
            os64_strcopy(value, sizeof(value), "a system family");
        else
            os64_snprintf(value, sizeof(value), "no face; the list begins with %s", fam->in);
        yonder_diag_fact(p->diag, key, value);
        os64_free(key);
    }
}

// After a drained batch: lay the page out again if pictures or faces moved it —
// once per batch when laying it out is cheap, so a slow page of forty
// pictures is not laid out forty times; otherwise when its last picture
// and face are in, or once the last layout is stale. A slow page that is waiting hands
// the ticker the moment it goes stale (pictures_schedule), so it settles
// on time with nobody at the mouse — or, with no ticker, at the first
// batch after that moment.
static void pictures_settle(void)
{
    uint64_t due = YONDER_NEVER;
    if (g.pictures_moved || s_web.moved) {
        bool now_ok = true;
        if (g.page.waiting + s_web.waiting > 0 && g.laid_ms > PICTURES_CHEAP_LAYOUT_MS) {
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
    // A face the layout's own feed moved (webfonts_move) is owed the next
    // batch, not dropped with the deadline this pass started from.
    if (s_web.moved && due == YONDER_NEVER)
        due = g.settle_due;
    if (g.page.record_owed && !g.pictures_moved && !s_web.moved)
        diag_pictures_in(&g.page);
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
// marks a widget's bounds, so a stand-in with the part's is how. A part
// that covers the view is a whole view of its own (WHOLE_BIG_PART), not
// parts that reached across it.
static void mark_part(os64_gui_rect_t r)
{
    os64_ui_widget_t part = {0};
    part.bounds = (os64_gui_rect_t){g.view.bounds.x + r.x - g.sx, g.view.bounds.y + r.y - g.sy,
                                    r.w, r.h};
    const os64_gui_rect_t v = g.view.bounds;
    if (part.bounds.x <= v.x && part.bounds.y <= v.y &&
        (int64_t)part.bounds.x + part.bounds.w >= (int64_t)v.x + v.w &&
        (int64_t)part.bounds.y + part.bounds.h >= (int64_t)v.y + v.h) {
        mark_whole(WHOLE_BIG_PART, false);
        return;
    }
    s_parts_reach = os64_rect_union(s_parts_reach, part.bounds);
    os64_ui_mark_dirty(&g.ui, &part);
}

typedef struct {
    int32_t k;
    bool mark;
    os64_gui_rect_t view;       // page coordinates
    bool seen;
    const flow_box_t *owner;    // the canvas's: asked apart from the walk
} SheetPictureSeen;

// Whether a box draws a background of its own: a text run, a line and a
// marker carry their element's style, background and all, but the painter
// draws none behind them (paint_box). Counting them would keep a picture
// playing for text that overflows a box scrolled away, and mark text, or a
// marker hung outside the box, for a picture that is not behind it.
static bool box_draws_background(const flow_box_t *b)
{
    return b->kind != FLOW_BOX_TEXT && b->kind != FLOW_BOX_LINE && b->kind != FLOW_BOX_MARKER;
}

// One box of the view's walk: does a sheet put picture `k` behind it?
static void sheet_picture_seen(void *ctx, const flow_box_t *b)
{
    SheetPictureSeen *look = ctx;
    if (b == look->owner || (look->seen && !look->mark) || !box_draws_background(b) ||
        !css_shows(&g.page, b->style, look->k))
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

// Whether a box's `background` attribute is what its background shows: a
// sheet's picture or gradient replaces it, as glass_backdrop draws it.
static bool attribute_shows(const flow_box_t *b)
{
    return !flow_background_has_image(b->style);
}

// Whether picture `k` is behind a box as glass_backdrop draws it: in a
// sheet's layer, or the attribute's when it shows (attribute_shows).
static bool box_shows(const Page *p, const flow_box_t *b, int32_t k)
{
    if (!attribute_shows(b))
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
        mark_whole(WHOLE_BACKDROP, false);
    }
    // Behind any other box: a `background` attribute's picture where it
    // shows (attribute_shows), and then a sheet's behind any box the view
    // shows (the walk visits only those).
    for (int32_t i = 0; p->bg_of != NULL && i < os64_page_nbackgrounds(model) && i < p->nbackground_map; i++) {
        if (p->bg_of[i] != k)
            continue;
        const flow_box_t *b = flow_box_for(p->tree, os64_page_background(model, i)->node);
        if (b == NULL || b == owner || !attribute_shows(b) ||
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

typedef struct {
    bool *seen;
    const bool *mark;
    int32_t n;
    os64_gui_rect_t view;       // page coordinates
    const flow_box_t *owner;    // the canvas's: asked apart from the walk
} SheetPicturesSeen;

// One box of the view's walk: which pictures does a sheet put behind it?
static void sheet_pictures_seen(void *ctx, const flow_box_t *b)
{
    SheetPicturesSeen *look = ctx;
    os64_gui_rect_t meet;
    if (b == look->owner || !box_draws_background(b) ||
        !os64_rect_intersect(flow_box_doc_rect(b, scroll_now()), look->view, &meet))
        return;
    for (int32_t i = 0; i < flow_background_layers(b->style); i++) {
        flow_layer_t l = flow_background_layer(b->style, i);
        int32_t k = l.image != NULL ? css_picture(&g.page, &l) : -1;
        if (k < 0 || k >= look->n)
            continue;
        look->seen[k] = true;
        if (look->mark != NULL && look->mark[k])
            mark_part(meet);
    }
}

// What picture_on_screen answers for one picture, for every picture of the
// shown page in one pass over it: which meet the view (`seen`, a flag a
// picture), and for each `mark` names (NULL: none), its boxes' parts of
// the glass marked for repainting. One pass, where asking each picture
// apart walks the page once a picture — at every tick and every schedule.
static void pictures_seen(bool *seen, const bool *mark)
{
    const Page *p = &g.page;
    const os64_page_t *model = page_model(p);
    os64_memset(seen, 0, (size_t)p->npics * sizeof(*seen));
    if (p->tree == NULL || model == NULL)
        return;
    os64_gui_rect_t view = {g.sx, g.sy, g.view.bounds.w, g.view.bounds.h}, meet;
    for (int32_t i = 0; p->pic_of != NULL && i < flow_nimages(p->tree); i++) {
        const flow_box_t *b = flow_image(p->tree, i);
        int32_t at = os64_page_image_for(model, b->node);
        int32_t k = at >= 0 && at < p->nimage_map ? p->pic_of[at] : -1;
        if (k < 0 || k >= p->npics || !os64_rect_intersect(flow_box_doc_rect(b, scroll_now()), view, &meet))
            continue;
        seen[k] = true;
        if (mark != NULL && mark[k])
            mark_part(meet);
    }
    // The canvas's pictures: behind the whole view, its owner found by the
    // painter's own rule (picture_on_screen), each as box_shows answers.
    const yonder_verbs_t asking = {.backdrop = glass_backdrop};
    const flow_box_t *owner = yonder_canvas_owner(p->tree, &asking);
    if (owner != NULL && !attribute_shows(owner)) {
        for (int32_t i = 0; i < flow_background_layers(owner->style); i++) {
            flow_layer_t l = flow_background_layer(owner->style, i);
            int32_t k = l.image != NULL ? css_picture(p, &l) : -1;
            if (k < 0 || k >= p->npics)
                continue;
            seen[k] = true;
            if (mark != NULL && mark[k])
                mark_whole(WHOLE_BACKDROP, false);
        }
    } else if (owner != NULL) {
        // The attribute shows: the branch above takes a sheet's picture.
        int32_t k = attribute_picture(p, owner);
        if (k >= 0 && k < p->npics) {
            seen[k] = true;
            if (mark != NULL && mark[k])
                mark_whole(WHOLE_BACKDROP, false);
        }
    }
    for (int32_t i = 0; p->bg_of != NULL && i < os64_page_nbackgrounds(model) && i < p->nbackground_map; i++) {
        int32_t k = p->bg_of[i];
        if (k < 0 || k >= p->npics)
            continue;
        const flow_box_t *b = flow_box_for(p->tree, os64_page_background(model, i)->node);
        if (b == NULL || b == owner || !attribute_shows(b) ||
            !os64_rect_intersect(flow_box_doc_rect(b, scroll_now()), view, &meet))
            continue;
        seen[k] = true;
        if (mark != NULL && mark[k])
            mark_part(meet);
    }
    SheetPicturesSeen look = {seen, mark, p->npics, view, owner};
    flow_visit(p->tree, view, scroll_now(), sheet_pictures_seen, &look);
}

// A flag a picture of the shown page, for pictures_seen: `small` when the
// page has few, else the heap — NULL with no memory, and the caller asks
// picture by picture instead.
#define PICTURE_FLAGS_SMALL 64
static bool *picture_flags(bool *small)
{
    if (g.page.npics <= PICTURE_FLAGS_SMALL)
        return small;
    return os64_malloc((size_t)g.page.npics * sizeof(bool));
}

static void picture_flags_free(bool *flags, bool *small)
{
    if (flags != small)
        os64_free(flags);
}

// What the shown page's frames are held to (MOTION_COST_MS), in
// microseconds: its last frame's cost, or before it has had one, its last
// layout's.
static uint64_t motion_cost_us(void)
{
    return g.motion_laid ? g.motion_us : g.laid_ms * 1000;
}

// Whether the cost rule is what keeps some of the shown page's animations
// still, and if so, in `out`, what cost too much, and in `*some`, whether
// others still move. Each half is held apart (PILE3.md § Animations in
// place, What a tick may cost): the layouts stop when their last frame —
// before the page has had one, its own layout — took more than
// MOTION_COST_MS and a change is left for a layout to show
// (flow_motion_pending_in); the ticks stop when theirs did and one is left
// for a tick. The other half still moves when it has a change to show and
// nothing stops it. It is the page's state at its tree's time, not the
// view's: scrolling changes nothing about it. A frame that failed is said
// where it failed (say_failed), and a page with nothing to show has
// nothing paused. A frame whose paint is still to come is not judged until
// it is whole (motion_painted): its cost so far is its layout's or its
// tick's alone. Its half was running when the frame began, and is taken to
// run until then; the other half is judged as it stands, so a pause said
// stays said while the other half paints.
static bool motion_paused_why(char *out, size_t cap, bool *some)
{
    out[0] = '\0';
    *some = false;
    if (!s_animations)
        return false;
    uint64_t lay_us = motion_cost_us(), tick_us = g.ticked ? g.tick_us : 0;
    uint64_t limit = (uint64_t)MOTION_COST_MS * 1000;
    bool lay_owed = flow_motion_pending_in(g.page.tree, false);
    bool tick_owed = flow_motion_pending_in(g.page.tree, true);
    bool lay_over = !g.motion_painting && lay_us > limit;
    bool tick_over = !g.tick_painting && tick_us > limit;
    bool lay_stopped = lay_us != UINT64_MAX && lay_over && lay_owed;
    bool tick_stopped = tick_us != UINT64_MAX && tick_over && tick_owed;
    if (!lay_stopped && !tick_stopped)
        return false;
    *some = lay_stopped ? tick_owed && !tick_over : lay_owed && !lay_over;
    uint64_t cost_us = lay_stopped ? lay_us : tick_us;
    os64_snprintf(out, cap, "%s took %lu ms, over %d",
                  !lay_stopped ? "a tick's frame" : g.motion_laid ? "a layout's frame" : "the layout",
                  (unsigned long)((cost_us + 999) / 1000), MOTION_COST_MS);
    return true;
}

// Whether a frame for an animation — a layout's or a tick's — waits for its
// paint (motion_painted): it is then the only frame, of either half. A
// next one is due only once it is painted, when its cost is whole, and so
// no event batch makes frames nobody sees, each costing what only the last
// is charged for.
static bool frame_unpainted(void)
{
    return g.motion_painting || g.tick_painting;
}

// When the shown page is next laid out for an animation: when libflow says
// what the view shows next changes, held to the cost rules
// (MOTION_COST_MS); never while animations are off or the window is
// covered, nor while a frame waits for its paint (frame_unpainted), nor
// while a script's change waits for its rebuild — a layout then is
// script_rebuild's, page_lay_out refuses any other, and a frame refused
// would read as one past every limit.
static uint64_t motion_next(void)
{
    if (!s_animations || g.covered || g.page.tree == NULL || page_rebuild_owed(&g.page) ||
        frame_unpainted())
        return YONDER_NEVER;
    uint64_t cost_us = motion_cost_us();
    if (cost_us > (uint64_t)MOTION_COST_MS * 1000)
        return YONDER_NEVER;
    os64_gui_rect_t view = {g.sx, g.sy, g.view.bounds.w, g.view.bounds.h};
    uint64_t due = flow_motion_due(g.page.tree, view, scroll_now());
    if (due == FLOW_NEVER)
        return YONDER_NEVER;
    if (g.motion_laid) {
        uint64_t rested = g.motion_ended + 2 * ((cost_us + 999) / 1000);
        uint64_t frame = g.motion_began + MOTION_FRAME_MS;
        if (due < rested)
            due = rested;
        if (due < frame)
            due = frame;
    }
    return due;
}

// Whether the shown page's ticks may run (PILE3.md § Animations in place):
// never while animations are off or the window is covered, nor while a
// script's change waits for its rebuild, nor once a tick's frame cost more
// than MOTION_COST_MS — or found no room with no layout to stand in.
static bool ticks_run(void)
{
    return s_animations && !g.covered && g.page.tree != NULL && !page_rebuild_owed(&g.page) &&
           !(g.ticked && g.tick_us > (uint64_t)MOTION_COST_MS * 1000);
}

// When the shown page's animations next tick: when libflow says what the
// view shows of them next changes (flow_motion_tick_due), a frame after
// the last tick began and, after it ended, as long again as it and its
// painting took — so ticking spends at most half the time — and never
// while a frame waits for its paint (frame_unpainted).
static uint64_t tick_next(void)
{
    if (!ticks_run() || frame_unpainted())
        return YONDER_NEVER;
    os64_gui_rect_t view = {g.sx, g.sy, g.view.bounds.w, g.view.bounds.h};
    uint64_t due = flow_motion_tick_due(g.page.tree, view, scroll_now());
    if (due == FLOW_NEVER)
        return YONDER_NEVER;
    if (g.ticked) {
        uint64_t rested = g.tick_ended + (g.tick_us + 999) / 1000;
        uint64_t frame = g.tick_began + MOTION_FRAME_MS;
        if (due < rested)
            due = rested;
        if (due < frame)
            due = frame;
    }
    return due;
}

// Hands the ticker the earliest of its deadlines: the next frame among the
// pictures on screen, the moment a slow page's layout goes stale
// (pictures_settle), the next layout for an animation (motion_next) and
// its next tick (tick_next) — none of them for a covered window — the
// moment a page waiting for its
// sheets is shown whatever is still coming, the moment a script the stream
// is stopped at stops waiting for its sheets, the next timer and the
// next storage commit of each page's scripts, covered or not, and the
// cookie file's next write.
// The earliest deadline offered so far (deadline_offer): when, where the
// first offered at that moment came from (for the counts), and whether any
// offered at that moment is the bell's to meet.
typedef struct {
    uint64_t next;
    PastDeadline from;
    bool bell;
} NextDeadline;

// A deadline offered to `d`. The loop meets a script's timer (script_turn,
// and the stream's and the arriving page's turns), a storage commit
// (storage_turn), a cookie file's write (cookies_turn, offered as a timer
// is), the settle (pictures_settle) and the stream's wait for its
// sheets (stream_turn) itself, after the events; the bell meets the rest (pictures_tick,
// motion_tick, coming_show, downloads_heard). Two at one moment are common — the clock
// steps by the tick — and the moment is the bell's if either is.
static void deadline_offer(NextDeadline *d, uint64_t due, PastDeadline from);

// The shown page has nothing more coming of its own accord: it has
// streamed and arrived, its pictures and fonts are in and its record is
// not owed, and no script of its is waiting to run or still being fetched
// (a `src` script, or a module script's graph: one slower than the quiet
// would otherwise be cut off). What a running script fetches for itself
// is not waited for, since a page that polls would never settle; its
// answer comes back as a job, which restarts the quiet.
static bool exit_settled(void)
{
    return g.page.tree != NULL && !g.stream.active && !g.coming.active && g.page.waiting == 0 &&
           !g.page.record_owed && s_web.waiting == 0 && !yonder_scripts_pending(g.page.scripts) &&
           !yonder_scripts_fetching(g.page.scripts);
}

// When an `--exit-when-loaded` run next looks at whether to leave.
static uint64_t exit_due(void)
{
    if (!s_exit_when_loaded)
        return YONDER_NEVER;
    uint64_t quiet = s_exit_quiet_from + EXIT_QUIET_MS, give_up = s_exit_started + EXIT_GIVE_UP_MS;
    return quiet < give_up ? quiet : give_up;
}

// An `--exit-when-loaded` run leaves once its page is settled and quiet,
// or once it has waited as long as it will; it says which on its way out.
// A page not settled now starts the quiet again.
static void exit_follow(void)
{
    if (!s_exit_when_loaded || !g.running)
        return;
    uint64_t now = yonder_now_ms();
    if (!exit_settled())
        s_exit_quiet_from = now;
    bool quiet = exit_settled() && now >= s_exit_quiet_from + EXIT_QUIET_MS;
    if (!quiet && now < s_exit_started + EXIT_GIVE_UP_MS)
        return;
    os64_printf("yonder: %s, leaving\n",
                quiet ? "the page is loaded and quiet" : "the page never went quiet in two minutes");
    g.running = false;
}

static void deadline_offer(NextDeadline *d, uint64_t due, PastDeadline from)
{
    bool bell = from != PAST_TIMERS && from != PAST_SETTLE && from != PAST_STREAM;
    if (due < d->next) {
        d->next = due;
        d->from = from;
        d->bell = bell;
    } else if (due == d->next && due != YONDER_NEVER && bell) {
        d->bell = true;
    }
}

static void pictures_schedule(void)
{
    cost_begin(PS_SCHEDULE);
    NextDeadline d = {YONDER_NEVER, PAST_SETTLE, false};
    if (!g.covered)
        deadline_offer(&d, g.settle_due, PAST_SETTLE);
    deadline_offer(&d, motion_next(), PAST_LAYOUTS);
    deadline_offer(&d, tick_next(), PAST_TICKS);
    if (g.coming.active)
        deadline_offer(&d, g.coming.due, PAST_ARRIVAL);
    if (g.stream.active && g.stream.sheets_due != 0)
        deadline_offer(&d, g.stream.sheets_due, PAST_STREAM);
    // A page's timers, covered or not: a page that counts down keeps
    // counting while another window is in front (DOM_D7.md § The clock).
    const yonder_scripts_t *hosts[] = {g.page.scripts, g.stream.scripts,
                                       g.coming.active ? g.coming.page.scripts : NULL};
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        deadline_offer(&d, yonder_scripts_timer_next(hosts[i]), PAST_TIMERS);
        // Its storage's next commit, which the loop meets (storage_turn).
        deadline_offer(&d, yonder_scripts_storage_due(hosts[i]), PAST_TIMERS);
    }
    // The cookie file's next write, which the loop makes (cookies_turn).
    deadline_offer(&d, yonder_cookies_due(s_cookies), PAST_TIMERS);
    // An `--exit-when-loaded` run's next look at whether to leave, which the
    // loop makes (exit_follow).
    deadline_offer(&d, exit_due(), PAST_TIMERS);
    // The shown page's next idle period, when only an idle callback that
    // asked for another waits for it (script_turn runs it).
    uint64_t idle = yonder_scripts_idle_next(g.page.scripts);
    if (idle != 0)
        deadline_offer(&d, idle, PAST_TIMERS);
    // The pictures on screen, found in one pass when any could be next or
    // at the next moment.
    bool asks = false;
    for (int32_t k = 0; !g.covered && k < g.page.npics && !asks; k++)
        asks = g.page.pics[k].playing && g.page.pics[k].due <= d.next;
    bool small[PICTURE_FLAGS_SMALL];
    bool *seen = asks ? picture_flags(small) : NULL;
    if (seen != NULL)
        pictures_seen(seen, NULL);
    for (int32_t k = 0; asks && k < g.page.npics; k++) {
        const Picture *pic = &g.page.pics[k];
        if (pic->playing && pic->due <= d.next && (seen != NULL ? seen[k] : picture_on_screen(k, false)))
            deadline_offer(&d, pic->due, PAST_PICTURES);
    }
    if (seen != NULL)
        picture_flags_free(seen, small);
    deadline_offer(&d, downloads_due(), PAST_DOWNLOADS);
    uint64_t next = d.next, now = yonder_now_ms();
    if (next != YONDER_NEVER && next <= now) {
        s_cost.past[d.from]++;
        // A past moment that is the loop's alone is the loop's to meet at its
        // turn, not the ticker's to ring for: the bell is dispatched among
        // the events, which cannot meet it, so it is handed on again and
        // rings again, the batch ringing itself until its bound. One still
        // owed when the loop next waits is rung for at the clock's next
        // tick. A moment the bell meets is rung for now.
        if (!d.bell)
            next = now + 1;
    }
    yonder_ticker_set(g.ticker, next);
    cost_end(PS_SCHEDULE);
    s_cost.schedules++;
    cost_fold(PS_SCHEDULE, PS_SCHEDULE);
}

// The ticker rang: every picture that is due and on screen shows its next
// frame, and where it shows is marked for repainting — its boxes, or the
// whole view for the picture behind the page (picture_on_screen). Timed as
// a tick (s_cost).
static void pictures_tick(void)
{
    cost_begin(PS_TICKS);
    uint64_t now = yonder_now_ms();
    // One pass finds the pictures on screen and one marks those that moved
    // (pictures_seen); with no memory for their flags, each is asked apart.
    bool small_seen[PICTURE_FLAGS_SMALL], small_moved[PICTURE_FLAGS_SMALL];
    bool *seen = !g.covered ? picture_flags(small_seen) : NULL;
    bool *moved = seen != NULL ? picture_flags(small_moved) : NULL;
    if (moved != NULL) {
        pictures_seen(seen, NULL);
        os64_memset(moved, 0, (size_t)g.page.npics * sizeof(*moved));
    }
    bool any = false;
    for (int32_t k = 0; !g.covered && k < g.page.npics; k++) {
        Picture *pic = &g.page.pics[k];
        if (!pic->playing || pic->due > now || !(moved != NULL ? seen[k] : picture_on_screen(k, false)))
            continue;
        // END holds the last frame and a failure the last good one: either
        // way the picture stays as it is, and stops.
        if (os64_image_sequence_next(pic->moving) != OS64_IMAGE_OK) {
            pic->playing = false;
            continue;
        }
        if (now - pic->due > s_cost.late_most)
            s_cost.late_most = now - pic->due;
        s_cost.late_frames += now - pic->due > FRAME_LATE_MS;
        pic->due = now + frame_delay(pic);
        if (moved != NULL)
            moved[k] = any = true;
        else
            (void)picture_on_screen(k, true);
    }
    if (any)
        pictures_seen(seen, moved);
    if (moved != NULL)
        picture_flags_free(moved, small_moved);
    if (seen != NULL)
        picture_flags_free(seen, small_seen);
    pictures_schedule();
    cost_end(PS_TICKS);
    s_cost.ticks++;
    cost_fold(PS_TICKS, PS_TICKS);
}

// A part a tick repaints (flow_motion_tick), cut to the view `ctx` holds,
// in page coordinates, first: a mark outside the view paints what nobody
// sees, and one that merges with another stretches it. The box whose
// background the canvas takes paints the whole view wherever its box is,
// so its mark is the whole view, said as the backdrop's (WHOLE_BACKDROP);
// which box that is is the painter's to say from the styles a tick
// changes — the root's background may be what moves — so it is asked at
// each mark, of the styles before the tick and after it.
static void tick_mark(void *ctx, const flow_box_t *box, os64_gui_rect_t r)
{
    const os64_gui_rect_t *view = ctx;
    const yonder_verbs_t asking = {.backdrop = glass_backdrop};
    os64_gui_rect_t meet;
    if (box == yonder_canvas_owner(g.page.tree, &asking))
        mark_whole(WHOLE_BACKDROP, false);
    else if (os64_rect_intersect(r, *view, &meet))
        mark_part(meet);
}

// The shown page's ticking animations moved to `now` (flow_motion_tick),
// what they repainted marked, and the controls placed again when a frame
// moved. A `frame` is the ticks' own, timed for the cost rule until its
// paint ends (motion_painted). A tick that is no frame — before a whole
// view is painted for something else — marks nothing: the whole view is.
// One not taken, either way — its lists found no room — is a layout
// instead, the ticks' frame with its paint (LAID_FOR_TICK); with the
// layouts stopped by what they cost, the ticks stop too, said where they
// failed (say_failed).
static void tick_page_run(uint64_t now, bool frame);

static void tick_page(uint64_t now, bool frame)
{
    cost_begin(PS_TICKS);
    tick_page_run(now, frame);
    cost_end(PS_TICKS);
    s_cost.ticks++;
    cost_fold(PS_TICKS, PS_TICKS);
}

static void tick_page_run(uint64_t now, bool frame)
{
    os64_gui_rect_t v = {g.sx, g.sy, g.view.bounds.w, g.view.bounds.h};
    int64_t began_us = os64_micros();
    bool moved = false;
    if (!flow_motion_tick(g.page.tree, now, scroll_now(), frame ? tick_mark : NULL, &v, &moved)) {
        if (motion_cost_us() > (uint64_t)MOTION_COST_MS * 1000) {
            g.ticked = true;
            g.tick_began = now;
            g.tick_us = UINT64_MAX;
            say_failed(g.page.diag, "animations",
                       "No room to move the page's animations between layouts; they stop here.");
            return;
        }
        (void)lay_out_shown(true, LAID_FOR_TICK);
        return;
    }
    if (moved)
        forms_place();
    if (frame) {
        g.ticked = true;
        g.tick_painting = true;
        g.tick_began = now;
        g.tick_began_us = began_us;
    }
}

// Before the whole view is painted — a scroll, the page's or a box's, may
// bring an animation into view whose last tick is long past, since nothing
// out of view was due — the ticking animations the view shows brought to
// now when any is behind (flow_motion_tick_due, unpaced), so the paint
// shows each at the time it is painted. Not a frame: its paint is the
// scroll's. A paint of parts does not tick, even parts far apart whose
// box spans the view: what it repaints of an animation is on the glass
// beside it already. The window loop asks before each paint.
static void ticks_before_paint(void)
{
    const os64_gui_rect_t v = g.view.bounds;
    if (!os64_ui_dirty_covers(&g.ui, v) || !ticks_run() || frame_unpainted())
        return;
    uint64_t now = yonder_now_ms();
    os64_gui_rect_t view = {g.sx, g.sy, v.w, v.h};
    if (flow_motion_tick_due(g.page.tree, view, scroll_now()) <= now)
        tick_page(now, false);
}

// The ticker rang: when an animation's layout is due, the page is laid out
// again at this moment, quietly (lay_out_shown); when only its tick is,
// the ticking animations move (tick_page). A ring for another deadline
// does neither.
static void motion_tick(void)
{
    uint64_t now = yonder_now_ms();
    if (motion_next() <= now)
        (void)lay_out_shown(true, LAID_FOR_MOTION);
    else if (tick_next() <= now)
        tick_page(now, true);
}

// The window painted: a frame for an animation costs its layout, or its
// tick, and the paint that shows it, so the frame ends now and costs all of
// it. A frame that cost too much is its half's last (motion_pause_said says
// so).
static void motion_painted(void)
{
    if (g.motion_painting) {
        g.motion_painting = false;
        g.motion_ended = yonder_now_ms();
        g.motion_us = (uint64_t)(os64_micros() - g.motion_began_us);
    }
    if (g.tick_painting) {
        g.tick_painting = false;
        g.tick_ended = yonder_now_ms();
        g.tick_us = (uint64_t)(os64_micros() - g.tick_began_us);
    }
}

// The status row made to say what the cost rule does to the shown page
// now (motion_paused_why), before whatever it says, and the page's record
// with it, when that changed since it last said it: true when the row
// changed. The window loop asks before each paint, and after a frame's
// paint ends, which may be the page's last — whatever changed it, a
// frame, a layout, a script's rebuild, the settings, the page — and a
// page that stays paused stays said, whatever message the row shows.
static bool motion_pause_said(void)
{
    char why[64], now[sizeof(g.motion_pause)] = "";
    bool some = false;
    if (motion_paused_why(why, sizeof(why), &some))
        os64_snprintf(now, sizeof(now), "%sANIMATIONS PAUSED: %s | ", some ? "SOME " : "", why);
    if (os64_streq(now, g.motion_pause))
        return false;
    if (g.page.diag != NULL) {
        char fact[96];
        os64_snprintf(fact, sizeof(fact), "%spaused: %s", some ? "some " : "", why);
        // Why the pause is over: animations turned off, none with a
        // change left to show, or a layout before the page's first frame
        // that cost less.
        yonder_diag_fact(g.page.diag, "animations", now[0] != '\0' ? fact
                                                  : !s_animations ? "off"
                                                  : !flow_motion_pending(g.page.tree) ? "none"
                                                  : "running");
    }
    os64_strcopy(g.motion_pause, sizeof(g.motion_pause), now);
    status_set(g.status_rest);
    return true;
}

// A paint of the rectangles damage[0..count) counted (s_cost): the view's
// pixels each covers, and when one covers the whole view, what marked it —
// `by`, else the parts when what they reach (`reach`) spans the view (a
// full list that became one rectangle, or one part as big as the view),
// else libui's own marks.
static void paint_counted(const os64_gui_rect_t *damage, uint32_t count, WholeBy by,
                          os64_gui_rect_t reach)
{
    bool whole = false;
    for (uint32_t i = 0; i < count; i++) {
        os64_gui_rect_t meet;
        if (!os64_rect_intersect(damage[i], g.view.bounds, &meet))
            continue;
        s_cost.pixels += (uint64_t)meet.w * (uint64_t)meet.h;
        whole = whole || (meet.w == g.view.bounds.w && meet.h == g.view.bounds.h);
    }
    if (whole) {
        os64_gui_rect_t parts;
        if (by == WHOLE_PARTS && (!os64_rect_intersect(reach, g.view.bounds, &parts) ||
                                  parts.w != g.view.bounds.w || parts.h != g.view.bounds.h))
            by = WHOLE_TOOLKIT;
        s_cost.whole++;
        s_cost.whole_by[by]++;
    }
    s_cost.paints++;
    s_cost.rects += count;
    if (count > s_cost.rects_most)
        s_cost.rects_most = count;
}

// libui's paint, timed (s_cost): its render, then each dirty rectangle
// handed to the compositor, as os64_ui_paint does both, then counted
// (paint_counted). What marked the view is this paint's, so it starts
// again here.
static void glass_paint(void)
{
    os64_gui_rect_t damage[OS64_UI_DAMAGE_MAX];
    WholeBy by = s_whole_by;
    os64_gui_rect_t reach = s_parts_reach;
    s_whole_by = WHOLE_PARTS;
    s_parts_reach = (os64_gui_rect_t){0, 0, 0, 0};
    cost_begin(PS_RENDER);
    uint32_t count = os64_ui_render_list(&g.ui, damage);
    cost_end(PS_RENDER);
    if (count == 0) {
        os64_memset(s_cost.now, 0, sizeof(s_cost.now));
        return;
    }
    cost_begin(PS_PUBLISH);
    for (uint32_t i = 0; i < count; i++)
        os64_draw_publish(g.ui.ctx, &damage[i]);
    cost_end(PS_PUBLISH);
    paint_counted(damage, count, by, reach);
    cost_fold(PS_RENDER, PS_PUBLISH);
}

// What the window loop does before each paint: a whole view's paint shows
// the ticking animations at its time (ticks_before_paint), and the status
// row says what the cost rule does then (motion_pause_said).
static void before_paint(void)
{
    ticks_before_paint();
    (void)motion_pause_said();
}

// The window's frame, as the compositor says it is now: what a script's
// outerWidth and outerHeight read (view_read).
static void frame_follow(void)
{
    os64_gui_window_state_t st;
    if (os64_gui_window_get_state(g.win, &st) == 0) {
        g.frame_w = st.width > INT32_MAX ? INT32_MAX : (int32_t)st.width;
        g.frame_h = st.height > INT32_MAX ? INT32_MAX : (int32_t)st.height;
    }
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

// What a dropdown's options are, for telling when they changed beneath an
// open list: each option's node, label and whether it may be chosen
// (FNV-1a), never 0.
static uint64_t options_signature(const os64_page_control_t *c)
{
    uint64_t h = 1469598103934665603ULL;
    for (int32_t k = 0; k < c->noptions; k++) {
        uintptr_t node = (uintptr_t)c->options[k].node;
        for (size_t b = 0; b < sizeof(node); b++)
            h = (h ^ (uint8_t)(node >> (8 * b))) * 1099511628211ULL;
        for (const char *s = c->options[k].label != NULL ? c->options[k].label : ""; *s; s++)
            h = (h ^ (uint8_t)*s) * 1099511628211ULL;
        h = (h ^ (c->options[k].disabled ? 0xFE : 0xFF)) * 1099511628211ULL;
    }
    return h != 0 ? h : 1;
}

// Whether a dropdown's list may choose option i: not when it, or its
// optgroup, is disabled.
static bool option_enabled(size_t i, void *user)
{
    const FormWidget *fw = user;
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), widget_control(fw));
    return c != NULL && (int32_t)i < c->noptions && !c->options[i].disabled;
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
    uint8_t was = 0;
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), widget_control(fw));
    if (c != NULL)
        was = (c->checked ? WAS_CHECKED : 0) | (c->indeterminate ? WAS_INDETERMINATE : 0);
    if (os64_page_set_checked(page_model(&g.page), widget_control(fw), cb->checked) < 0)
        status_rest("the page keeps that one as it is");
    forms_sync_from_model(false);
    // The box changed before its click, as HTML's legacy-pre-activation
    // behaviour has it; the click may still put it back (inputs_run).
    if (scripts_live())
        input_queue(IN_CHECK, fw->node, NULL, g.pointer_x, g.pointer_y, was);
}

// A listbox's row chosen, or a dropdown's value changed: the model's choice,
// then the page's `input` and `change` (IN_LIST). A dropdown calls it only
// for a new value, never for a commit of the value it had. A choice the
// model refuses (an option disabled since the list was drawn) changes
// nothing, and the widget is synced back to what the model holds.
static void option_chosen(const FormWidget *fw, int selected)
{
    bool chosen = selected >= 0 &&
        os64_page_set_chosen(page_model(&g.page), widget_control(fw), (int32_t)selected, true) >= 0;
    forms_sync_from_model(false);
    if (chosen && scripts_live())
        input_queue(IN_LIST, fw->node, NULL, 0, 0, false);
}

static void list_changed(os64_ui_listbox_t *list, void *user)
{
    option_chosen(user, list->selected);
}

static void drop_changed(os64_ui_dropdown_t *drop, void *user)
{
    option_chosen(user, drop->selected);
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
                // New text put the caret at its end, wherever the record is:
                // the next pull puts it back (a script that rewrites a value
                // and restores the selection it read leaves the record where
                // it was). A field shown for the first time keeps its caret.
                if (fw->presented)
                    fw->record_seen.start = UINT32_MAX;
            }
            os64_memcpy(fw->model_text, value, n + 1);
            fw->model_text_len = n;
            fw->presented = true;
            break;
        }
        case FW_CHECK:
            os64_ui_checkbox_set(&g.ui, &fw->u.check, c->checked);
            // HTML shows a checkbox's indeterminate flag and no radio's.
            os64_ui_checkbox_set_indeterminate(&g.ui, &fw->u.check,
                                               c->indeterminate && c->input == OS64_PAGE_INPUT_CHECKBOX);
            break;
        case FW_LIST:
        case FW_DROPDOWN: {
            int selected = -1;
            for (int32_t k = 0; k < c->noptions && selected < 0; k++)
                if (c->options[k].selected)
                    selected = k;
            if (fw->kind == FW_DROPDOWN) {
                uint64_t sig = options_signature(c);
                if (sig != fw->options_sig)
                    os64_ui_dropdown_rows_changed(&g.ui, &fw->u.drop);
                fw->options_sig = sig;
                os64_ui_dropdown_set(&g.ui, &fw->u.drop, (size_t)c->noptions, selected);
            } else
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

// ── A text field's selection ────────────────────────────────────────────
//
// A page's scripts read and set a text control's selection through
// libpage's record of it (os64_page_node_selection), in UTF-16 code units of
// its value; a person moves the field's caret, in bytes of the field's
// text. The two are kept in step at the points where scripts can see them:
// before a script runs, a field whose caret a person moved since they were
// last in step is written into the record (forms_flush); after a script's
// task, a record that moved since then moves the caret (script_rebuild).
// Neither is moved until one of them moves: a field nobody has touched
// keeps the caret the widget gave it, and its record reads 0.

// The field's text, and how many of its bytes are one unit of offset: a
// text field holds the value's bytes; a password field shows one bullet a
// character of the value it hides, so an offset in it counts characters.
static const char *field_value(const FormWidget *fw, size_t *len)
{
    *len = fw->kind == FW_PASSWORD ? fw->secret_len : os64_strlen(fw->text);
    return fw->kind == FW_PASSWORD ? fw->secret : fw->text;
}

// The UTF-16 code units of the value before `offset` in the field.
static uint32_t field_units_at(const FormWidget *fw, size_t offset)
{
    size_t len;
    const char *value = field_value(fw, &len);
    uint32_t units = 0;
    size_t chars = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if ((c & 0xC0) == 0x80)
            continue;
        if (fw->kind == FW_PASSWORD ? chars == offset : i >= offset)
            break;
        units += c >= 0xF0 ? 2 : 1;
        chars++;
    }
    return units;
}

// The field's offset at `units` UTF-16 code units of the value; past the
// value, its end.
static size_t field_offset_of(const FormWidget *fw, uint32_t units)
{
    size_t len;
    const char *value = field_value(fw, &len);
    uint32_t seen = 0;
    size_t chars = 0, i = 0;
    for (; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if ((c & 0xC0) == 0x80)
            continue;
        if (seen >= units)
            break;
        seen += c >= 0xF0 ? 2 : 1;
        chars++;
    }
    return fw->kind == FW_PASSWORD ? chars : i;
}

// The field's own selection, in the record's terms: its ends in code
// units, and its direction (the caret after the anchor is forward).
static FieldSelection field_selection(const FormWidget *fw)
{
    const os64_ui_textfield_t *tf = &fw->u.field;
    uint32_t caret = field_units_at(fw, tf->cursor);
    uint32_t anchor = tf->selected ? field_units_at(fw, tf->anchor) : caret;
    return (FieldSelection){anchor < caret ? anchor : caret, anchor < caret ? caret : anchor,
                            anchor == caret ? OS64_PAGE_DIRECTION_NONE
                            : anchor < caret ? OS64_PAGE_DIRECTION_FORWARD : OS64_PAGE_DIRECTION_BACKWARD,
                            0};
}

// The record as it stands; false for a control that has none (a number
// input) or one that cannot be read now.
static bool record_selection(const FormWidget *fw, FieldSelection *out)
{
    os64_page_state_t *state = os64_page_shared_state(page_model(&g.page));
    out->writes = os64_page_node_selection_writes(state, fw->node);
    return os64_page_node_selection(state, fw->node, &out->start, &out->end, &out->direction) == 0;
}

static bool same_selection(FieldSelection a, FieldSelection b)
{
    return a.start == b.start && a.end == b.end && a.direction == b.direction && a.writes == b.writes;
}

// Both sides noted as they now stand: they are in step.
static void field_selection_note(FormWidget *fw)
{
    fw->field_seen = field_selection(fw);
    fw->seen = record_selection(fw, &fw->record_seen);
}

// A person's selection into the record: when the field's selection moved
// since the two were last in step, or when `written` (the person's text was
// just written, and a value that differs puts the record at its end).
static void field_selection_push(FormWidget *fw, bool written)
{
    if (!fw->seen) {
        field_selection_note(fw);
        if (!written)
            return;
    }
    FieldSelection now = field_selection(fw);
    if (!written && same_selection(now, fw->field_seen))
        return;
    os64_page_state_t *state = os64_page_shared_state(page_model(&g.page));
    (void)os64_page_node_set_selection(state, fw->node, now.start, now.end, now.direction);
    field_selection_note(fw);
}

// The person chose this field's caret where it stands, whether or not it
// moved (a press on it, or the focus brought to it): the next flush
// publishes it. A field nobody has touched shows its caret at the value's
// end while the record is at 0, and a click at that end moves nothing.
static void field_selection_chosen(os64_ui_widget_t *w)
{
    for (int32_t i = 0; i < g.nfw; i++) {
        FormWidget *fw = g.fw[i];
        if (fw->w != w || (fw->kind != FW_TEXT && fw->kind != FW_PASSWORD))
            continue;
        if (!fw->seen)
            field_selection_note(fw);
        fw->field_seen.start = UINT32_MAX;
        return;
    }
}

// The record into the field: when it moved since the two were last in step
// (a script set it, or a value set put it at the end), the caret moves with
// it. A field not yet seen compares the record with where every record
// begins, so a record a script moved before this widget was made (a
// password field become a text one is a new widget) still moves its caret.
static void field_selection_pull(FormWidget *fw)
{
    FieldSelection record;
    if (!record_selection(fw, &record)) {
        field_selection_note(fw);
        return;
    }
    if (same_selection(record, fw->record_seen))
        return;
    size_t from = field_offset_of(fw, record.start), to = field_offset_of(fw, record.end);
    if (record.direction == OS64_PAGE_DIRECTION_BACKWARD)
        os64_ui_textfield_select_range(&g.ui, &fw->u.field, to, from);
    else
        os64_ui_textfield_select_range(&g.ui, &fw->u.field, from, to);
    field_selection_note(fw);
}

// Every text field's record into its caret, after a script's task.
static void forms_pull_selections(void)
{
    for (int32_t i = 0; i < g.nfw; i++)
        if (g.fw[i]->kind == FW_TEXT || g.fw[i]->kind == FW_PASSWORD)
            field_selection_pull(g.fw[i]);
}

// Flush before script and activation so the binding observes a person's edit
// and where the person left the caret. A stale/refused edit keeps its buffer
// and reports the native refusal.
static void forms_flush(void)
{
    os64_page_t *model = page_model(&g.page);
    for (int32_t i = 0; i < g.nfw; i++) {
        FormWidget *fw = g.fw[i];
        const os64_page_control_t *c = os64_page_control(model, widget_control(fw));
        if (c == NULL || (fw->kind != FW_TEXT && fw->kind != FW_PASSWORD))
            continue;
        const char *v = fw->kind == FW_PASSWORD ? fw->secret : fw->text;
        size_t n = fw->kind == FW_PASSWORD ? fw->secret_len : os64_strlen(fw->text);
        // A read-only or disabled field's text is not the person's to
        // change, but its selection still is.
        bool written = false;
        if (!c->readonly && !c->disabled && (n != fw->synced_len || os64_memcmp(v, fw->synced, n) != 0)) {
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
            written = true;
        }
        field_selection_push(fw, written);
    }
}

// Sends a form: the button pressed (CONTROL) or Enter in a field (IMPLICIT).
// libpage decides what goes and where; libway judges it; an INVALID
// refusal puts the focus on the control that failed and its validation
// message on the status line. `kind` is how the
// reply comes: a new way, or a refresh when a script sends the form before
// its page has completely loaded (navigation_perform). `by` is whether a
// person sent it or the page's script did. `validated`: the page's scripts
// validated it before its submit event, and the model does not again.
static void form_send_as(int32_t control, os64_page_activation_t how, NavKind kind, way_initiator_t by,
                         bool validated)
{
    forms_flush();
    os64_page_what_t what = {how, control, 0, 0};
    os64_page_request_t request;
    os64_page_verdict_t verdict = validated ? os64_page_activate_validated(page_model(&g.page), what, &request)
                                            : os64_page_activate(page_model(&g.page), what, &request);
    if (verdict == OS64_PAGE_NAVIGATE) {
        g.chain = 0;
        request_navigate(&request, kind, WAY_ASK_SEND, g.page.way.url, by, g.page.diag);
        return;
    }
    if (verdict == OS64_PAGE_FRAGMENT) {
        scroll_to_node(request.anchor);
    } else {
        // A button with no form, or one whose type submits nothing, does
        // nothing by HTML's own rule: on a page whose scripts run, what it
        // does is theirs, and saying it refused would be wrong. Without
        // scripts the sentence is why nothing happened.
        bool scripts_own_it = scripts_live() && (request.reason == OS64_PAGE_REASON_NO_FORM ||
                                                 request.reason == OS64_PAGE_REASON_NO_SUBMISSION);
        const os64_page_control_t *invalid = request.reason == OS64_PAGE_REASON_INVALID
            ? os64_page_control(page_model(&g.page), request.control) : NULL;
        char sentence[256];
        uint32_t flags = 0;
        if (reason_refuses(request.reason))
            say_failed(g.page.diag, "form refused", os64_page_reason_name(request.reason));
        else if (invalid != NULL &&
                 os64_page_node_validity(os64_page_shared_state(page_model(&g.page)), invalid->node, NULL, NULL,
                                         &flags) >= 0 &&
                 os64_page_node_validation_message(os64_page_shared_state(page_model(&g.page)), invalid->node,
                                                   flags, sentence, sizeof(sentence)) >= 0 &&
                 sentence[0] != '\0')
            // The control's own sentence, as the page's report says it.
            status_rest(sentence);
        else if (!scripts_own_it)
            status_rest(os64_page_reason_name(request.reason));
        FormWidget *failed = request.reason == OS64_PAGE_REASON_INVALID
                                 ? form_widget(request.control) : NULL;
        if (failed != NULL && !failed->w->hidden)
            os64_ui_set_focus(&g.ui, failed->w);
    }
    os64_page_request_free(&request);
}

// A form a person sent: a button pressed, Enter in a field.
static void form_send(int32_t control, os64_page_activation_t how)
{
    form_send_as(control, how, NAV_GO, WAY_BY_PERSON, false);
}

// The byte offset in a password's value of its `chars`th character, the
// offset its bullets count in.
static size_t password_byte(const FormWidget *fw, size_t chars)
{
    size_t at = 0;
    for (size_t seen = 0; at < fw->secret_len; at++) {
        if (((unsigned char)fw->secret[at] & 0xc0) == 0x80)
            continue;
        if (seen++ == chars)
            break;
    }
    return at;
}

// The keys of a focused password field, taken before libui sees them: its
// field holds bullets, so yonder edits the value itself, at the bullets'
// caret and selection. True when taken.
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
    // Traversal and leaving are libui's, and so is a key that is no
    // character (an arrow, Home, End), which moves the bullets' caret and
    // changes no text.
    if (a == '\t' || a == 0x1b || a == 0)
        return false;
    if (a == '\r' || a == '\n') {
        field_submitted(&fw->u.field, fw);
        return true;
    }
    bool erase = a == '\b' || a == 0x7f;
    if (!erase && a < 0x20)
        return true;                            // a chord: cut or paste would edit bullets, not the value
    // The edit is the value's, at the caret: a bullet is a character, so
    // the field's offsets count characters.
    const os64_ui_textfield_t *tf = &fw->u.field;
    size_t lo = tf->cursor, hi = tf->cursor;
    if (tf->selected) {
        lo = tf->anchor < tf->cursor ? tf->anchor : tf->cursor;
        hi = tf->anchor < tf->cursor ? tf->cursor : tf->anchor;
    }
    if (erase && lo == hi && lo > 0)
        lo--;                                   // Backspace without a selection: the character before
    if (erase && lo == hi)
        return true;
    char put[4];
    size_t n = erase ? 0 : os64_utf8_encode(a, put);   // a key's byte is Latin-1; the value is UTF-8
    size_t from = password_byte(fw, lo), to = password_byte(fw, hi);
    if (fw->secret_len - (to - from) + n + 1 > sizeof(fw->secret))
        return true;
    os64_memmove(fw->secret + from + n, fw->secret + to, fw->secret_len - to);
    os64_memcpy(fw->secret + from, put, n);
    fw->secret_len = fw->secret_len - (to - from) + n;
    fw->secret[fw->secret_len] = '\0';
    password_show(fw);
    os64_ui_textfield_select_range(&g.ui, &fw->u.field, lo + (n != 0), lo + (n != 0));
    field_changed(&fw->u.field, fw);
    return true;
}

static FormKind control_kind(const os64_page_control_t *c)
{
    if (c->element == OS64_PAGE_EL_SELECT)
        return select_one_line(c, c->node) ? FW_DROPDOWN : FW_LIST;
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
        case FW_DROPDOWN:
            os64_ui_dropdown(&fw->u.drop, (size_t)c->noptions, option_label, drop_changed, fw);
            fw->u.drop.enabled = option_enabled;
            fw->w = &fw->u.drop.w;
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

// Every control's widget at its box on the glass, clipped to the page view
// (libui's `clipped`), so a control half scrolled out of the view is half a
// control and takes no click on the toolbar it would otherwise cross; and
// hidden when none of it is in the view, or when the pointer cannot reach
// the control at its centre (flow_box_covered; POSITION.md, ruling 9 and
// its amendment): a field under a fixed header, a hidden dialog's field,
// one `visibility: hidden`. Whatever of its frame the page draws, the
// painter draws. Without a layout there is no box to place a control at,
// so the widgets stay hidden.
// A control the page made invisible (`opacity: 0`) is drawn when it is a
// FIELD — in a form, or named, so something is submitted — because then it
// is nearly always a custom checkbox's real input, whose styled stand-in
// cannot show what a click here did (POSITION.md, ruling 6). One in no form
// and with no name, or an empty one (which submits nothing either), is the
// page's own gadget, a click target laid over something drawn (MediaWiki's
// menu checkbox), and is left undrawn. Undrawn, not gone: its widget stays
// where the control is and takes its clicks, its focus and its keys as any
// control's does, painting nothing (unpainted_class), so a click ticks it
// and Space ticks it again, with every event a script listens for.
static bool gadget(const flow_box_t *b, int32_t control)
{
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), control);
    return b->unpainted && c != NULL && c->form < 0 && (c->name == NULL || c->name[0] == '\0');
}

// A widget class that is `cls` in all but painting: libui skips a class
// with no paint and still hit-tests, focuses and dispatches to it. One copy
// per class, made once; NULL when the few slots are spent, and the gadget
// is then hidden instead.
static const os64_ui_class_t *unpainted_class(const os64_ui_class_t *cls)
{
    static os64_ui_class_t copies[8];
    static const os64_ui_class_t *of[8];
    static int32_t n;
    for (int32_t i = 0; i < n; i++)
        if (of[i] == cls || &copies[i] == cls)
            return &copies[i];
    if (n == (int32_t)(sizeof(copies) / sizeof(copies[0])))
        return NULL;
    copies[n] = *cls;
    copies[n].paint = NULL;
    of[n] = cls;
    return &copies[n++];
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
        if (fw->own_class == NULL)
            fw->own_class = fw->w->cls;
        const os64_ui_class_t *cls = fw->own_class;
        if (gadget(b, widget_control(fw)))
            cls = unpainted_class(fw->own_class);
        bool shown = x < (int64_t)v.x + v.w && y < (int64_t)v.y + v.h && x + at.w > v.x && y + at.h > v.y &&
                     at.w > 0 && at.h > 0 && !flow_box_covered(g.page.tree, b, scroll_now()) && cls != NULL;
        fw->w->clipped = true;
        fw->w->clip = v;
        if (cls != NULL && cls != fw->w->cls) {
            fw->w->cls = cls;
            os64_ui_mark_dirty(&g.ui, fw->w);
        }
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
    mark_whole(WHOLE_WINDOW, true);
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
        // The staged copy reads the page's timeline, and its animations go
        // on from it once it is published.
        page_timeline(p);
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
            webfonts_feed(false);       // the files its layout wanted are still asked for
            say_failed(p->diag, "layout", p->tree != NULL
                ? "Out of memory laying out the changed page; this is the last layout that fit."
                : "Out of memory laying out the changed page.");
            forms_build();
            mark_whole(WHOLE_SCRIPT, false);
            return false;
        }
        sheets_leave(p);
        page_show(p, next.tree);
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
        webfonts_feed(true);
        forms_build();
        clamp_scroll();
        sync_bars();
        forms_place();
        pictures_schedule();
        mark_whole(WHOLE_SCRIPT, false);
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
    // A selection a script set changes no revision: it is asked of every
    // text field, after its text.
    forms_pull_selections();
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

// The host a serial names, in whichever role its page is: arriving, held
// by stream_finish for its DOMContentLoaded, waiting for its sheets, or on
// screen. NULL: its page is gone. A script's request is its own page's
// wherever that page is: what the window gives it of a page — its origin,
// a `src` script's Referer, the record it is written in — comes from this
// host, and a serial that finds none is refused, which the page sees as a
// network error, never taken as the page on screen's.
static yonder_scripts_t *scripts_for(uint64_t serial)
{
    if (g.stream.scripts != NULL && yonder_scripts_serial(g.stream.scripts) == serial)
        return g.stream.scripts;
    if (g.finishing.page != NULL && g.finishing.page->scripts != NULL &&
        yonder_scripts_serial(g.finishing.page->scripts) == serial)
        return g.finishing.page->scripts;
    if (g.coming.active && g.coming.page.scripts != NULL &&
        yonder_scripts_serial(g.coming.page.scripts) == serial)
        return g.coming.page.scripts;
    if (g.page.scripts != NULL && yonder_scripts_serial(g.page.scripts) == serial)
        return g.page.scripts;
    return NULL;
}

// A `src` script's fetch, its Referer the address its page's scripts run
// at (none when that address was too long to keep whole:
// yonder_scripts_referrer). 0, not begun, for a page that is gone, an
// address that is not http, https or file, no pool, no memory for the job,
// or a pool that will not take it.
static uint64_t script_fetch(void *opaque, uint64_t serial, uint32_t token, const char *url,
                             const char *fallback)
{
    (void)opaque;
    yonder_scripts_t *host = scripts_for(serial);
    yonder_script_job_t *job =
        host != NULL && fetchable(url) && g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
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
    const char *referrer = yonder_scripts_referrer(host);
    os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), referrer != NULL ? referrer : "");
    hooks_ask(&job->hooks, yonder_scripts_url(host), WAY_DEST_SCRIPT);
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

// devicePixelRatio: the window's zoom, as it stands when a script asks.
static uint32_t script_zoom(void *opaque)
{
    (void)opaque;
    return g.zoom;
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
    return way_script_cookies(g.way.jar, yonder_scripts_url(opaque), yonder_scripts_encrypted(opaque),
                              out, cap, whole);
}

static void script_cookies_set(void *opaque, const char *text, size_t length)
{
    way_script_cookie(g.way.jar, yonder_scripts_url(opaque), yonder_scripts_encrypted(opaque), text,
                      length);
}

static void view_read(void *opaque, os64_dom_view_t *out);
static bool view_scroll(void *opaque, double x, double y, os64_dom_geometry_stats_t *cost);
static bool script_computed(void *opaque, const os64_html_node_t *element, os64_dom_computed_t *out);
static bool history_update(void *opaque, const char *url, const char *state, size_t length, bool push);
static bool history_fragment(void *opaque, const char *url, bool replace);
static uint32_t history_length(void *opaque);

// document.activeElement's and hasFocus()'s answer for the page whose
// document is `opaque`: the element focused in it (the window keeps focus
// in the page on screen's document alone), and whether it is the page on
// screen. Whether the window itself has the person's focus is not
// followed (DEBTS.md), so the page on screen always says it has.
static bool script_focus(void *opaque, const os64_html_node_t **focused)
{
    *focused = g.focus_node != NULL && g.focus_doc == opaque ? g.focus_node : NULL;
    return opaque == page_doc(&g.page);
}

// What the arriving page's document says of its own request: its referrer,
// the Referer that request carried (way_referrer from the page it was asked
// for from to the address it arrived from, "" for none) or, for a Reload,
// the reloaded page's own; and its Last-Modified (0 for none or a date
// that does not read).
static void stream_request_facts(char *referrer, size_t cap, int64_t *modified)
{
    os64_url_t to;
    referrer[0] = '\0';
    if (g.stream.kept_referrer[0] != '\0')
        os64_strcopy(referrer, cap, g.stream.kept_referrer);
    else if (g.stream.referrer[0] == '\0' || os64_url_parse(g.stream.head.url, &to) != OS64_URL_OK ||
             !way_referrer(g.stream.referrer, &to, referrer, cap))
        referrer[0] = '\0';
    if (!way_cookie_date(g.stream.head.last_modified, os64_strlen(g.stream.head.last_modified), modified))
        *modified = 0;
}

static void script_keep_bytecode(void *opaque, const yonder_bytecode_t *where, uint8_t *blob, size_t length);

static yonder_scripts_t *scripts_host(os64_html_document_t *doc, os64_page_state_t *state,
                                      const char *url, bool encrypted, uint64_t serial,
                                      yonder_diag_t *diag, bool modern)
{
    char referrer[OS64_URL_REF_MAX];
    int64_t modified;
    stream_request_facts(referrer, sizeof(referrer), &modified);
    yonder_scripts_options_t options = {
        .url = url, .execution_ms = g.script_ms, .serial = serial, .alert = script_alert,
        .fetch = script_fetch, .cancel = script_cancel, .keep_bytecode = script_keep_bytecode,
        .activate = script_activate, .write = script_write, .now_ms = script_now, .geometry = script_geometry, .opaque = doc,
        .view = {view_read, view_scroll, doc}, .computed = script_computed,
        .history = {history_update, history_fragment, history_length, doc},
        .diag = diag, .encrypted = encrypted, .modern = modern,
        .storage = s_storage, .timing = &g.stream.timing, .screen = s_screen, .zoom = script_zoom,
        .request = script_request, .run = script_run, .release = script_release,
        .referrer = referrer, .last_modified = modified, .focus = script_focus};
    yonder_scripts_t *host = yonder_scripts_new(doc, state, &options);
    yonder_scripts_set_user_agent(host, script_user_agent, NULL);
    const os64_dom_cookies_t cookies = {script_cookies_get, script_cookies_set, host};
    yonder_scripts_set_cookies(host, &cookies);
    return host;
}

static void script_arrived(const yonder_script_job_t *job, yonder_script_t *got)
{
    yonder_scripts_t *host = scripts_for(job->page);
    // Kept as the host is handed it, decoded, a fetch that failed as a
    // failure: a replay hands it over the same.
    bool came = got != NULL && got->ok;
    if (host != NULL)
        diag_cookies_short(yonder_scripts_diag(host), &job->hooks.cookies_short);
    if (host != NULL)
        diag_keep(yonder_scripts_diag(host), YONDER_DIAG_KEPT_SCRIPT, job->token, job->url,
                  came ? got->url : job->url, "", came ? got->source : NULL, came ? got->length : 0);
    // Its compiled form, if the cache holds its body, is the host's now.
    yonder_bytecode_t *bytecode = came ? got->bytecode : NULL;
    if (came)
        got->bytecode = NULL;
    if (host != NULL)
        yonder_scripts_fetched(host, job->token, came ? got->source : NULL, got != NULL ? got->length : 0,
                               came ? got->url : NULL, bytecode);
    else
        yonder_bytecode_free(bytecode);
}

// The compiled forms on their way to the disk, in bytes: a page compiling
// a megabyte of script a second must not queue more than this behind the
// workers, and one that would is not kept, which costs the next visit a
// compile and nothing else.
#define BYTECODE_QUEUED_MAX ((size_t)64 << 20)
static size_t s_bytecode_queued;

// A script or a module compiled from a body the cache holds (scripts.h
// keep_bytecode): its blob is written beside the entry on a job of its own.
static void script_keep_bytecode(void *opaque, const yonder_bytecode_t *where, uint8_t *blob, size_t length)
{
    (void)opaque;
    yonder_bytecode_job_t *job = g.pool != NULL && g.cache != NULL && length <= SCRIPT_BYTECODE_MAX &&
                                         s_bytecode_queued + length <= BYTECODE_QUEUED_MAX
                                     ? os64_calloc(1, sizeof(*job))
                                     : NULL;
    if (job == NULL) {
        os64_free(blob);
        return;
    }
    job->kind = YONDER_JOB_BYTECODE;
    job->cache = g.cache;
    job->blob = blob;
    job->length = length;
    os64_strcopy(job->partition, sizeof(job->partition), where->partition);
    job->credentials = where->credentials;
    os64_strcopy(job->url, sizeof(job->url), where->url);
    os64_strcopy(job->tag, sizeof(job->tag), where->tag);
    os64_work_t work = {yonder_bytecode_keep_run, yonder_bytecode_keep_release, job, BYTECODE_KEEP_RESERVE};
    if (os64_work_submit(g.pool, &work) == 0) {
        yonder_bytecode_keep_release(job, NULL);
        return;
    }
    s_bytecode_queued += length;
}

// When the task running began (task_begin), for its cost (task_cost). A
// begun task that finds nothing to run never reaches task_said, and the
// next task_begin starts it again.
static uint64_t s_task_began;

// A task is about to run: the layouts it forces are counted from zero
// (DOM.md § Geometry), and the audit's clock starts, and the paint counts'.
// Every task kind begins here and ends at task_said.
static int64_t task_begin(yonder_scripts_t *host)
{
    (void)yonder_scripts_geometry_stats(host, true);
    s_task_began = cycles();
    return s_script_audit ? os64_micros() : 0;
}

// A task ran (task_said): its run into the paint counts, by its kind — a
// script's source, a timer, or an event by the event's name.
static void task_cost(const char *what)
{
    s_cost.now[PS_TASKS] += cycles_since(s_task_began);
    cost_fold(PS_TASKS, PS_TASKS);
    if (os64_streq(what, "script"))
        s_cost.scripts++;
    else if (os64_streq(what, "timer"))
        s_cost.timers++;
    else
        s_cost.events++;
}

// What a task said, on the status line: the layouts it forced and what they
// cost, then an error by its script's name, and a runtime its task retired
// by the sentence that says the page goes on without script. The audit
// logs every task, what it cost, what it forced and what it wrote: a page
// that writes its whole body shows as one.
static void task_said(yonder_scripts_t *host, bool was_alive, const char *what,
                      const os64_js_outcome_t *out, int64_t began)
{
    task_cost(what);
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
    // How long the engine took over it, compiling included, when the task
    // evaluated a script: `ran in 1520 us`, and `ran in 80 us from its
    // bytecode` when the compile was its kept bytecode's read instead
    // (BYTECODE_CACHE.md), which is the before and after.
    int64_t us = yonder_scripts_task_us(host);
    const char *from = yonder_scripts_task_from_bytecode(host) ? " from its bytecode" : "";
    char ran[64] = "ran", died[64] = "failed";
    if (us >= 0) {
        os64_snprintf(ran, sizeof(ran), "ran in %ld us%s", (long)us, from);
        os64_snprintf(died, sizeof(died), "failed in %ld us%s", (long)us, from);
    }
    // A task with a script's name is a script's, and the record lists it;
    // an event or a timer with none is listed only by a failure.
    if (out->status == OS64_JS_OK) {
        if (out->source_name[0] != '\0')
            yonder_diag_fact(diag, key, ran);
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
    // layouts beside it, and with where it failed (yonder_scripts_failure):
    // the same failure at the same place is one line however many times it
    // happens, and the count says how many.
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
    char placed[OS64_JS_MESSAGE_CAP + OS64_DOM_URL_MAX + 128];
    yonder_scripts_failure(host, address, failure, out->stack_trace, placed, sizeof(placed));
    yonder_diag_failed(diag, key, placed);
    if (out->source_name[0] != '\0')
        yonder_diag_fact(diag, key, died);
    badge_follow();
    status_rest(line);
    yonder_scripts_set_note(host, line);
}

// ── A script's requests (MODERN_WEB.md § Slice 2) ──────────────────────
//
// fetch() and XMLHttpRequest hand their request to the window: a fetch job
// (fetch_job.h) on the pool, or, for a synchronous XMLHttpRequest, the same
// job run here on the window thread. Its answer is the page's task when it
// is reaped. The page departing cancels what is out (page_departs).

// The browser's CORS preflight cache (way/origin.h), every page's; NULL
// sends a preflight every time one is needed.
static way_preflight_t *s_preflight;

// A job for a page's request, its bytes its own, from the page its serial
// names (scripts_for). NULL on no memory, for an address longer than a
// data: URL may be (fetch_job.h), and for a serial no page has, each of
// which the page sees as a network error.
static yonder_fetch_job_t *request_job(uint64_t serial, uint32_t token, const os64_dom_request_t *q)
{
    size_t url_len = os64_strlen(q->url);
    yonder_scripts_t *host = scripts_for(serial);
    if (url_len > YONDER_FETCH_DATA_URL_MAX || host == NULL)
        return NULL;
    // The page's origin, from the address its scripts run at; an opaque
    // one is this page's own, named by its serial.
    way_origin_t origin;
    way_origin_of(yonder_scripts_url(host), serial != 0 ? serial : 1, &origin);
    yonder_fetch_job_t *job = os64_calloc(1, sizeof(*job));
    if (job == NULL)
        return NULL;
    job->kind = YONDER_JOB_FETCH;
    job->page = serial;
    job->token = token;
    job->agent = g.way.agent;
    job->identity = g.way.identity;
    job->url = os64_malloc(url_len + 1);
    job->headers = os64_malloc(q->headers_length + 1);
    job->body = q->body != NULL ? os64_malloc(q->body_length + 1) : NULL;
    if (job->url == NULL || job->headers == NULL || (q->body != NULL && job->body == NULL)) {
        yonder_fetch_release(job, NULL);
        return NULL;
    }
    os64_memcpy(job->url, q->url, url_len + 1);
    if (q->headers_length != 0)
        os64_memcpy(job->headers, q->headers, q->headers_length);
    job->headers[q->headers_length] = '\0';
    job->headers_len = q->headers_length;
    if (q->body != NULL && q->body_length != 0)
        os64_memcpy(job->body, q->body, q->body_length);
    job->body_len = q->body != NULL ? q->body_length : 0;
    job->request.origin = origin;
    job->request.mode = q->mode;
    job->request.credentials = q->credentials;
    job->request.redirect = q->redirect;
    job->request.policy = q->policy;
    job->request.preflight_flag = q->preflight;
    os64_strcopy(job->request.method, sizeof(job->request.method), q->method);
    // A referrer too long to keep whole goes as none: a cut address would
    // name a page that is not this one. The page's own address is one when
    // the host kept only its start (yonder_scripts_referrer), which is the
    // address the binding names for "about:client".
    const char *referrer = q->referrer != NULL ? q->referrer : "";
    if (yonder_scripts_referrer(host) == NULL && os64_streq(referrer, yonder_scripts_url(host)))
        referrer = "";
    os64_strcopy(job->referrer, sizeof(job->referrer),
                 os64_strlen(referrer) < sizeof(job->referrer) ? referrer : "");
    job->jar = g.way.jar;
    job->preflight = s_preflight;
    // The browser's cache, as the request's cache mode says to use it
    // (SCRIPT_CACHE.md § 5); a mode past the list is a request's default.
    job->cache = g.cache;
    job->cache_mode = q->cache <= WAY_CACHE_ONLY_IF_CACHED ? (way_cache_mode_t)q->cache : WAY_CACHE_DEFAULT;
    job->module = (token & YONDER_MODULE_TOKEN) != 0;
    return job;
}

static uint64_t script_request(void *opaque, uint64_t serial, uint32_t token, const os64_dom_request_t *q)
{
    (void)opaque;
    yonder_fetch_job_t *job = g.pool != NULL ? request_job(serial, token, q) : NULL;
    if (job == NULL)
        return 0;
    os64_work_t work = {yonder_fetch_run, yonder_fetch_release, job,
                        job->module ? YONDER_FETCH_MODULE_RESERVE : YONDER_FETCH_RESERVE};
    job->submitted_us = os64_micros();
    os64_work_id_t id = os64_work_submit(g.pool, &work);
    if (id == 0)
        yonder_fetch_release(job, NULL);
    return id;
}

// The job's answer as the binding reads it, borrowed from `got`. A network
// error's reason stays here, for the page's record (request_record): the
// binding is handed none, and the page learns only that it failed.
static void request_response(const yonder_fetched_t *got, os64_dom_response_t *out)
{
    if (got == NULL) {
        *out = (os64_dom_response_t){.ok = false};
        return;
    }
    *out = (os64_dom_response_t){
        .ok = got->ok, .type = got->type, .status = got->status,
        .status_text = got->status_text, .url = got->url != NULL ? got->url : "", .redirected = got->redirected,
        .headers = got->headers, .headers_length = got->headers_len, .has_body = got->has_body,
        .body = got->body, .body_length = got->body_len,
    };
}

// The page's record of a request: a line per address with what came back,
// how many redirects it took (each was a dial), and what it cost — the
// body's bytes and the fetch's time — and a failure said as one. A
// synchronous one says so: it stopped the page while it ran.
static void request_record(yonder_scripts_t *host, const yonder_fetch_job_t *job, const yonder_fetched_t *got,
                           bool sync)
{
    yonder_diag_t *diag = yonder_scripts_diag(host);
    if (diag == NULL)
        return;
    if (got != NULL)
        diag_cookies_short(diag, &got->cookies_short);
    if (got == NULL || !got->ok) {
        char line[OS64_FETCH_URL_MAX + 320];
        os64_snprintf(line, sizeof(line), "%s %s: %s%s", job->request.method, job->url,
                      got != NULL ? got->why : "out of memory", sync ? " (synchronous)" : "");
        yonder_diag_failed(diag, "request", line);
        return;
    }
    char key[OS64_FETCH_URL_MAX + 48];
    char value[160];
    char took[96] = "";
    if (got->elapsed_us >= 0)
        os64_snprintf(took, sizeof(took), " in %ld ms", (long)(got->elapsed_us / 1000));
    // A wait for a worker, or for the window to hear the answer, a
    // millisecond or more: time the request spent not being fetched.
    int64_t heard = got->finished_us >= 0 ? os64_micros() - got->finished_us : -1;
    size_t at = os64_strlen(took);
    if (got->queued_us >= 1000 && at < sizeof(took))
        at += (size_t)os64_snprintf(took + at, sizeof(took) - at, ", queued %ld ms", (long)(got->queued_us / 1000));
    if (heard >= 1000 && at < sizeof(took))
        os64_snprintf(took + at, sizeof(took) - at, ", heard %ld ms later", (long)(heard / 1000));
    os64_snprintf(key, sizeof(key), "request %s %s", job->request.method, job->url);
    // An answer the cache gave says so (SCRIPT_CACHE.md § 9): kept, served
    // without asking; confirmed, served after a 304.
    const char *from = got->served == WAY_SERVED_KEPT        ? ", kept"
                       : got->served == WAY_SERVED_CONFIRMED ? ", confirmed"
                                                             : "";
    os64_snprintf(value, sizeof(value), "%d %s, %u hops, %lu bytes%s%s%s", (int)got->status,
                  way_type_name(got->type), (unsigned)got->hops, (unsigned long)got->body_len, took, from,
                  sync ? ", synchronous" : "");
    yonder_diag_fact(diag, key, value);
}

// The document a host's page is, for the asks its task made.
static const os64_html_document_t *scripts_doc(const yonder_scripts_t *host)
{
    if (host == g.page.scripts)
        return page_doc(&g.page);
    if (g.coming.active && host == g.coming.page.scripts)
        return page_doc(&g.coming.page);
    if (g.finishing.page != NULL && host == g.finishing.page->scripts)
        return page_doc(g.finishing.page);
    if (host == g.stream.scripts && g.stream.parser != NULL)
        return os64_html_parser_document(g.stream.parser);
    return NULL;
}

// A request's job was reaped: its answer is the page's task, as a timer's
// is, and what the task asked of the widgets is performed after it. A
// module's compiled form is the page's module map's now.
static void request_arrived(const yonder_fetch_job_t *job, yonder_fetched_t *got)
{
    yonder_scripts_t *host = scripts_for(job->page);
    if (host == NULL)
        return;
    request_record(host, job, got, false);
    os64_dom_response_t response;
    request_response(got, &response);
    yonder_bytecode_t *bytecode = got != NULL ? got->bytecode : NULL;
    if (got != NULL)
        got->bytecode = NULL;
    os64_js_outcome_t out;
    int64_t began = task_begin(host);
    if (!yonder_scripts_answered(host, job->token, &response, bytecode, &out))
        return;
    task_said(host, true, "request", &out, began);
    const os64_html_document_t *doc = scripts_doc(host);
    if (host != g.page.scripts)
        asks_perform(host, doc, false);
    else if (script_rebuild())
        asks_perform(host, doc, true);
}

// A synchronous XMLHttpRequest: the job run here, on the window thread,
// to its end or until the script task that made it is out of time (the
// binding's `stop`), whichever is first: the window waits for a sync
// request exactly as long as it waits for any script. Its job and answer
// are kept until the binding lets the answer go (script_release): the
// binding runs one at a time.
static yonder_fetch_job_t *s_sync_job;
static yonder_fetched_t *s_sync_got;

static bool script_run(void *opaque, uint64_t serial, const os64_dom_request_t *q, bool (*stop)(void *stop_ctx),
                       void *stop_ctx, os64_dom_response_t *response)
{
    (void)opaque;
    yonder_fetch_job_t *job = request_job(serial, 0, q);
    if (job == NULL)
        return false;
    void *product = NULL;
    // `stop` is the binding's: the script task's own deadline (and libjs's
    // cancel), asked by the job and by libfetch before each wait.
    (void)yonder_fetch_run(job, stop, stop_ctx, &product);
    yonder_scripts_t *host = scripts_for(serial);
    if (host != NULL)
        request_record(host, job, product, true);
    s_sync_job = job;
    s_sync_got = product;
    request_response(product, response);
    return true;
}

static void script_release(void *opaque, os64_dom_response_t *response)
{
    (void)opaque;
    (void)response;
    yonder_fetch_release(s_sync_job, s_sync_got);
    s_sync_job = NULL;
    s_sync_got = NULL;
}

// A request for an address a script named, owning its own storage as
// libpage's do, from the page at `from`: leaving an https:// page for an
// http:// address is a downgrade, which libway asks about (way_judge).
// False when memory ran out.
static bool request_for(const char *url, const char *from, os64_page_request_t *out)
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
    out->downgrade = os64_strlen(from) > 8 && os64_memcmp(from, "https://", 8) == 0 &&
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
static Page *page_of_host(const yonder_scripts_t *host);
static bool page_fragment(Page *p, const char *url, bool replace, bool tell);
static void fragment_scroll(const os64_html_node_t *target, const char *name);
static bool page_traverse(Page *p, int32_t delta);
static void session_go(int32_t delta, way_initiator_t by, const char *from);

// How many entries a history delta moves, in a type that holds every one:
// a page's history.go() takes any WebIDL `long`, and INT32_MIN has no
// int32_t magnitude, so a delta is never negated as itself.
static int64_t steps_of(int32_t delta)
{
    return delta < 0 ? -(int64_t)delta : (int64_t)delta;
}

// How a page not yet shown is coming, from wherever its arrival holds it:
// the stream, its DOMContentLoaded, or the wait for its sheets.
static NavKind unshown_kind(const Page *p)
{
    return p == &g.stream.page ? g.stream.kind : p == g.finishing.page ? g.finishing.kind : g.coming.kind;
}

// Where the entry a page not yet shown arrives at stands, counted from the
// page on screen (0). A new way's page is the entry after the one on screen
// and the newest, its commit having let go of what was ahead (1). A reload
// or a refresh is the entry on screen again (0). A traversal's page is the
// session's entry it named (Page.arrives_at), which stays where it is; one
// the session let go past its cap arrives as a new way's (arrive_now).
// True when it arrives as a new way's: nothing is ahead of it.
static bool unshown_at(const Page *p, int64_t *at)
{
    NavKind kind = unshown_kind(p);
    if (kind == NAV_BACK || kind == NAV_FORWARD) {
        for (int32_t i = 0; i < g.way.depth; i++)
            if (g.way.history[i].entry == p->arrives_at) {
                *at = (int64_t)i - g.way.depth;
                return false;
            }
        for (int32_t j = 0; j < g.way.ahead; j++)
            if (g.way.forward[j].entry == p->arrives_at) {
                *at = (int64_t)g.way.ahead - j;
                return false;
            }
    } else if (kind != NAV_GO) {
        *at = 0;
        return false;
    }
    *at = 1;
    return true;
}

// Whether a navigation a page's script asks for replaces the page's own
// entry: until the page has completely loaded (Page.loaded), HTML navigates
// with historyHandling "replace", by address (§ 7.2.4 The Location
// interface, "Location-object navigate", step 3, short of transient
// activation) and by a form (§ 4.10.22.3 Form submission algorithm, step
// 25). No activation is weighed: a page is shown and its `load` runs in one
// turn (arrive_now), so a person's input has no task in that window. A
// fragment navigation is not held to it (DEBTS.md § Classic browser
// scripting).
static bool page_navigation_replaces(const Page *p)
{
    return p != NULL && !p->loaded;
}

// Back or Forward from a page not yet shown, past its own entries (`delta`
// as asked; page_traverse has answered within them). ONE RULE WHEREVER THE
// PAGE IS in its arrival — parsing, at its DOMContentLoaded, waiting for its
// sheets — because by HTML its navigation committed when its response came
// and its document was made (§ Finalize a cross-document navigation, which
// puts its entry in the session before any of its scripts run): it is an
// entry already, and going back or forward from it lands where doing so
// from that entry would (§ Traverse the history by a delta: the step that
// many entries away, and nothing when there is none). So, when there is
// such an entry, the page is never shown, a new way's own entries join the
// session as its commit would have put them (a traversal's or a reload's go
// with the page: DEBTS.md § Classic browser scripting), and the window goes
// to the entry asked for from the page on screen: no fetch when that is the
// page on screen. Once it is shown (its `load` and every later task) the
// page on screen IS it, and session_go answers.
static void unshown_go(Page *p, int32_t delta)
{
    if (g.page.tree == NULL || delta == 0)
        return;                     // no session around it to go into
    NavKind kind = unshown_kind(p);
    bool back = delta < 0;
    // A page with entries of its own pushed at least once, which let go of
    // what was ahead of its entry (arrive_now): forward past its own is
    // nothing, however it came.
    if (!back && (p->nback > 0 || p->nahead > 0))
        return;
    // How far past the entry next to its own the delta reaches: 0 is that
    // entry; `at` is where its own entry stands (unshown_at).
    int64_t past = back ? (int64_t)delta + 1 + p->nback : (int64_t)delta - 1 - p->nahead;
    int64_t at;
    if (unshown_at(p, &at) && !back)
        return;
    int64_t next = at + (back ? -1 : 1);
    int64_t target = next + past;
    if (target < -(int64_t)g.way.depth || target > (int64_t)g.way.ahead)
        return;                     // no entry there: nothing happens, and the page arrives
    // A new way's entries go ahead of the page on screen, nearest last
    // (way_forward_entry), as its own commit would have put them, and that
    // commit let go of what was ahead first.
    if (kind == NAV_GO) {
        way_forget_ahead(&g.way);
        for (int32_t i = 0; i < p->nahead; i++)
            way_forward_entry(&g.way, &p->ahead[i]);
        way_crumb_t current = page_entry_left(p);
        way_forward_entry(&g.way, &current);
        for (int32_t i = p->nback - 1; i >= 0; i--)
            way_forward_entry(&g.way, &p->back[i]);
        page_stack_clear(p->back, &p->nback);
        page_stack_clear(p->ahead, &p->nahead);
    }
    if (p == g.finishing.page)
        g.finishing.abandoned = true;
    // The traversal is the page's own, from its address, read before the
    // stop below can let the page go.
    char from[OS64_FETCH_URL_MAX];
    os64_strcopy(from, sizeof(from), p->way.url);
    // Stopping a traversal can add the page on screen's held change to the
    // session (traverse_end) after `target` was measured (DEBTS.md § Classic
    // browser scripting).
    stop_trip();
    if (target != 0)
        session_go((int32_t)target, WAY_BY_PAGE, from);
}

// A navigation a page's script asked for. Every one is the page's own
// (WAY_BY_PAGE): no person's activation, even for a click() or submit() a
// listener ran inside the person's own click, which HTML would give it
// (transient activation, which yonder does not track: DIVERGENCES).
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
        // A fragment navigation (MODERN_WEB.md § Slice 1b): an entry, and
        // for the page on screen the scroll; one not yet shown scrolls when
        // it arrives. The binding asked for it here only when it did not
        // make it itself (dom.h § HISTORY): its page's events are owed all
        // the same, and are heard when the arc's surface is on. With no
        // memory for the entry it is the scroll alone.
        if (here != NULL && ask->url[n] == '#' && n == before_fragment(here) &&
            os64_memcmp(ask->url, here, n) == 0) {
            bool replace = ask->kind == OS64_DOM_NAVIGATE_REPLACE || os64_strcmp(ask->url, here) == 0;
            Page *p = shown ? &g.page : page_of_host(host);
            if (p != NULL)
                (void)page_fragment(p, ask->url, replace, true);
            if (shown) {
                fragment_scroll(NULL, ask->url + n + 1);
            } else {
                os64_strcopy(fragment, cap, ask->url + n + 1);
                *has_fragment = true;
            }
            return;
        }
        // The navigation is the asking page's, wherever that page is: its
        // address is what a downgrade is judged from and the request's
        // Referer (none when it was too long to keep whole:
        // yonder_scripts_referrer), and its record takes a refusal's line.
        const char *from = yonder_scripts_referrer(host);
        yonder_diag_t *record = yonder_scripts_diag(host);
        os64_page_request_t request;
        if (!request_for(ask->url, yonder_scripts_url(host), &request)) {
            say_failed(record, "navigation", "A script asked for an address this browser cannot go to.");
            return;
        }
        // A page's navigation is from its own entry, which replace()
        // replaces, and so does an ordinary navigation until the page has
        // completely loaded (page_navigation_replaces). For the page on
        // screen, whose `load` is running, that entry is the one on screen.
        // A page not yet shown is the session's entry already (unshown_go
        // says why), not the page on screen's: the new page takes its place
        // (start_trip). One that waits on the question bar is still judged
        // and named as the asking page's (its downgrade, and its Referer on
        // a yes), but its place in the session is the page on screen's: a
        // page not yet shown's place is not taken (DEBTS.md).
        Page *p = shown ? &g.page : page_of_host(host);
        bool replaces = page_navigation_replaces(p);
        g.replacing = replaces && p != &g.page ? p : NULL;
        request_navigate(&request,
                         ask->kind == OS64_DOM_NAVIGATE_REPLACE || (replaces && p == &g.page) ? NAV_REFRESH
                                                                                                : NAV_GO,
                         WAY_ASK_GO, from, WAY_BY_PAGE, record);
        g.replacing = NULL;
        return;
    }
    case OS64_DOM_NAVIGATE_RELOAD:
        if (shown)
            reload_as(WAY_BY_PAGE);
        return;
    case OS64_DOM_NAVIGATE_HISTORY: {
        // A page not yet shown goes through the entries its scripts made;
        // Back or Forward past them is unshown_go's. Either is the page's
        // own traversal.
        Page *p = shown ? NULL : page_of_host(host);
        if (p == NULL)
            session_go(ask->delta, WAY_BY_PAGE, g.page.way.url);
        else if (!page_traverse(p, ask->delta))
            unshown_go(p, ask->delta);
        return;
    }
    case OS64_DOM_NAVIGATE_FOLLOW:
        if (shown) {
            int32_t link = os64_page_link_for(page_model(&g.page), ask->node);
            if (link >= 0)
                follow_link_asked(link, WAY_ASK_GO, WAY_BY_PAGE);
        }
        return;
    case OS64_DOM_NAVIGATE_SUBMIT:
        if (shown) {
            // A form sent before its page has completely loaded replaces
            // the page's entry, as an address does.
            NavKind kind = page_navigation_replaces(&g.page) ? NAV_REFRESH : NAV_GO;
            int32_t control = ask->submitter != NULL
                ? os64_page_control_for(page_model(&g.page), ask->submitter) : -1;
            // With the Modern Web switch on, the binding validated a
            // submitter's submission before its submit event (libdom's
            // window.c), and the model must not judge it again.
            if (control >= 0)
                form_send_as(control, OS64_PAGE_ACTIVATE_CONTROL, kind, WAY_BY_PAGE, g.page.modern);
            else
                form_send_as(os64_page_form_for(page_model(&g.page), ask->node), OS64_PAGE_ACTIVATE_FORM, kind,
                             WAY_BY_PAGE, false);
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
            } else if (g.asks[i].what == OS64_DOM_ACTIVATE_REPORT) {
                // A control's constraint problem, reported as a refused
                // submission is: the control focused, its sentence said.
                char said[256];
                if (fw != NULL && !fw->w->hidden)
                    os64_ui_set_focus(&g.ui, fw->w);
                if (yonder_scripts_report_message(host, g.asks[i].node, said, sizeof(said)) && said[0] != '\0')
                    status_rest(said);
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
static bool page_event_run(Page *p, bool shown);

// A page's queued task (MODERN_WEB.md § P2b): its oldest posted message or
// its next due timer, whichever was queued first — a message when it was
// posted, a timer when it fell due — the timer first when both were queued
// at once, so a page whose every message posts another still has its
// timers run. `what` names the one tried. True when one ran.
static bool queued_task(yonder_scripts_t *host, const char **what, os64_js_outcome_t *out)
{
    uint64_t now = yonder_now_ms(), timer = yonder_scripts_timer_next(host);
    uint64_t message = yonder_scripts_message_next(host);
    if (message <= now && (timer > now || message < timer)) {
        *what = "message";
        if (yonder_scripts_message_fire(host, out))
            return true;
    }
    *what = "timer";
    return yonder_scripts_timer_fire(host, now, out);
}

static bool scripts_task(yonder_scripts_t *host, const os64_html_document_t *doc, bool shown)
{
    if (host == NULL || !yonder_scripts_alive(host))
        return false;
    // The history's events a page is owed come first, in order: each was
    // owed by a task or a traversal that is over.
    Page *p = page_of_host(host);
    if (p != NULL && page_event_run(p, shown))
        return true;
    os64_js_outcome_t out;
    int64_t began = task_begin(host);
    const char *what = "script";
    bool ran = yonder_scripts_step(host, &out);
    if (!ran)
        ran = queued_task(host, &what, &out);
    if (!ran)
        return false;
    task_said(host, true, what, &out, began);
    asks_perform(host, doc, shown);
    return true;
}

// Whether a host has a task to run now: a history event owed its page, a
// script ready, a message posted, or a timer due. An idle callback is no
// task owed: the shown page's turn runs one only when nothing else is
// (script_turn).
static bool scripts_owed(const yonder_scripts_t *host)
{
    const Page *p = page_of_host(host);
    uint64_t now = yonder_now_ms();
    return (p != NULL && p->nevents > 0 && yonder_scripts_alive(host)) || yonder_scripts_pending(host) ||
           yonder_scripts_message_next(host) <= now || yonder_scripts_timer_next(host) <= now;
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

// ── The session's entries (MODERN_WEB.md § Slice 1b) ───────────────────
//
// HTML's session history, the window's half. The entries of the page on
// screen are crumbs in g.way, each naming its document (Page.document) and
// carrying its state, and moving between two of one document's is no
// fetch. A page not yet shown keeps the entries its scripts make itself,
// and arriving moves them in (arrive_now). Three ways an entry changes,
// three event rules (MODERN_WEB_BRIEFS.md § Slice 1b): a STATE UPDATE
// (pushState, replaceState) moves the address and fires nothing; a
// FRAGMENT NAVIGATION adds an entry (replaces the current one when the
// address is the same), scrolls, and fires popstate — inside the script
// when the page's binding made it, else owed — then owes hashchange when
// the fragment moved; a TRAVERSAL to an entry of this document fetches
// nothing, owes the same two, and puts the scroll back where it was unless
// the page keeps its own. One to an entry of another document fetches it
// and moves the session when it arrives (session_go, g.traversing).

// The page whose document `doc` is: on screen, held by stream_finish,
// waiting for its sheets, or arriving. NULL for none.
static Page *page_of_doc(const os64_html_document_t *doc)
{
    if (doc == NULL)
        return NULL;
    if (page_doc(&g.page) == doc)
        return &g.page;
    if (g.finishing.page != NULL && page_doc(g.finishing.page) == doc)
        return g.finishing.page;
    if (g.coming.active && page_doc(&g.coming.page) == doc)
        return &g.coming.page;
    if (g.stream.parser != NULL && os64_html_parser_document(g.stream.parser) == doc)
        return stream_page();
    return NULL;
}

// The arriving page's scripts are the stream's until stream_finish gives
// them to the page.
static yonder_scripts_t *page_scripts(const Page *p)
{
    return p == &g.stream.page ? g.stream.scripts : p->scripts;
}

static Page *page_of_host(const yonder_scripts_t *host)
{
    if (host == NULL)
        return NULL;
    if (host == g.page.scripts)
        return &g.page;
    if (g.finishing.page != NULL && host == g.finishing.scripts)
        return g.finishing.page;
    if (g.coming.active && host == g.coming.page.scripts)
        return &g.coming.page;
    if (g.stream.active && host == g.stream.scripts)
        return stream_page();
    return NULL;
}

// Where a page keeps its address: the head's while it streams, which
// stream_page and the page's arrival read it from; the page's own after.
static char *page_address(Page *p, size_t *cap)
{
    if (p == &g.stream.page) {
        *cap = sizeof(g.stream.head.url);
        return g.stream.head.url;
    }
    *cap = sizeof(p->way.url);
    return p->way.url;
}

// Where a page not yet shown keeps the fragment its arrival scrolls to.
static bool page_fragment_slot(Page *p, char **fragment, size_t *cap, bool **has)
{
    if (p == &g.stream.page) {
        *fragment = g.stream.fragment;
        *cap = sizeof(g.stream.fragment);
        *has = &g.stream.has_fragment;
    } else if (p == g.finishing.page) {
        *fragment = g.finishing.fragment;
        *cap = g.finishing.cap;
        *has = g.finishing.has_fragment;
    } else if (g.coming.active && p == &g.coming.page) {
        *fragment = g.coming.fragment;
        *cap = sizeof(g.coming.fragment);
        *has = &g.coming.has_fragment;
    } else {
        return false;
    }
    return true;
}

// Whether two addresses' fragments differ, a missing one included.
static bool fragments_differ(const char *a, const char *b)
{
    const char *x = a + before_fragment(a), *y = b + before_fragment(b);
    return os64_strcmp(x, y) != 0;
}

// The event a page is owed for the move it just made: a hashchange from
// `old_url` to where it is now, or a popstate for the entry it is now at
// and that entry's state, captured now (a later move must not change what
// this one's event says).
static void page_event_owe(Page *p, bool hashchange, const char *old_url)
{
    // A page with no scripts has nothing that could hear it, and one owed
    // more than it can hold has its newest let go: a script that traverses
    // in a loop is owed its events in order, not all of them. An event
    // with no memory for what it carries is let go the same way.
    if (page_scripts(p) == NULL || p->nevents == PAGE_EVENTS_MAX)
        return;
    size_t cap;
    PageEvent e = {.hashchange = hashchange};
    bool kept;
    if (hashchange) {
        e.old_url = copy_text(old_url);
        e.new_url = copy_text(page_address(p, &cap));
        kept = e.old_url != NULL && e.new_url != NULL;
    } else {
        e.entry = yonder_scripts_history_entry(page_scripts(p));
        e.state = p->entry_state != NULL ? copy_text(p->entry_state) : NULL;
        kept = p->entry_state == NULL || e.state != NULL;
    }
    if (!kept) {
        page_event_free(&e);
        return;
    }
    p->events[p->nevents++] = e;
}

// The links resolve against the address a page is at now (`<base href>`
// aside): after a state update or a traversal, its model is built again
// at the new one, sharing its control state. One that cannot be built
// leaves the old, whose links resolve against where the page came from.
static void page_rebase(Page *p)
{
    os64_page_t *old = p->way.model;
    if (old == NULL)
        return;
    os64_page_t *model = os64_page_build(page_doc(p), p->way.url, NULL, os64_page_shared_state(old));
    if (model == NULL || os64_page_incomplete(model)) {
        os64_page_free(model);
        return;
    }
    p->way.model = model;
    os64_page_free(old);
    if (p == &g.page)
        g.hover_link = g.pressed_link = -1;
}

// What follows a page's address wherever it moved: its scripts' base, the
// address field for the page on screen, the model's links.
static void page_address_moved(Page *p)
{
    size_t cap;
    const char *address = page_address(p, &cap);
    yonder_scripts_set_url(page_scripts(p), address);
    if (p == &g.page)
        os64_ui_textfield_set(&g.ui, &g.field, address);
    if (p != &g.stream.page)
        page_rebase(p);
}

// Room on a page's own stack of `n` crumbs for `more` pushes (the session's
// cap counted: a push there lets the oldest go), so the pushes cannot
// fail. False on no memory, with the stack as it was.
static bool page_stack_room(way_crumb_t **stack, int32_t n, int32_t more)
{
    int32_t want = n + more < WAY_HISTORY_MAX ? n + more : WAY_HISTORY_MAX;
    if (want <= n)
        return true;
    way_crumb_t *grown = os64_realloc(*stack, (size_t)want * sizeof(**stack));
    if (grown == NULL)
        return false;
    *stack = grown;
    return true;
}

// `crumb` (its state taken) onto a page's own stack that page_stack_room
// made room on, the oldest let go past the session's cap, as the session
// lets it go.
static void page_stack_push(way_crumb_t *stack, int32_t *n, way_crumb_t *crumb)
{
    if (*n == WAY_HISTORY_MAX) {
        os64_free(stack[0].state);
        os64_memmove(stack, stack + 1, (size_t)(*n - 1) * sizeof(*stack));
        (*n)--;
    }
    stack[(*n)++] = *crumb;
    crumb->state = NULL;
}

static void page_stack_clear(way_crumb_t *stack, int32_t *n)
{
    for (int32_t i = 0; i < *n; i++) {
        os64_free(stack[i].state);
        stack[i].state = NULL;
    }
    *n = 0;
}

// The entry a page is at, as a crumb, its state moved into it.
static way_crumb_t page_entry_left(Page *p)
{
    way_crumb_t left;
    os64_memset(&left, 0, sizeof(left));
    size_t cap;
    os64_strcopy(left.url, sizeof(left.url), page_address(p, &cap));
    if (p == &g.page)
        left.position = position_now();
    left.document = p->document;
    left.entry = p->entry;
    left.asked = p->asked;
    left.state = p->entry_state;
    p->entry_state = NULL;
    return left;
}

// The entry the page on screen is at while a traversal is fetched, kept
// once, before the first change the session will not take (g.held). False
// on no memory, with nothing kept.
static bool page_hold(void)
{
    if (g.has_held)
        return true;
    char *state = g.page.entry_state != NULL ? copy_text(g.page.entry_state) : NULL;
    if (g.page.entry_state != NULL && state == NULL)
        return false;
    os64_memset(&g.held, 0, sizeof(g.held));
    os64_strcopy(g.held.url, sizeof(g.held.url), g.page.way.url);
    g.held.document = g.page.document;
    g.held.entry = g.page.entry;
    g.held.asked = g.page.asked;
    g.held.state = state;
    g.has_held = true;
    return true;
}

// The traversal is over or stopped: what it held is let go. `stopped`
// when it never arrived, where a push the page made meanwhile joins the
// session after all, as the one entry it had then (g.held).
static void traverse_end(bool stopped)
{
    if (g.has_held && stopped && g.held_push)
        way_remember_entry(&g.way, &g.held);
    os64_free(g.held.state);
    os64_memset(&g.held, 0, sizeof(g.held));
    g.traversing = g.has_held = g.held_push = false;
}

// Whether a traversal is still on its way: its fetch, its DOMContentLoaded
// or its sheets. One that failed on the way (no memory to read or lay it
// out) is ended here, as stopped.
static bool traverse_pending(void)
{
    if (!g.traversing)
        return false;
    if (g.stream.active || g.coming.active || g.finishing.page != NULL)
        return true;
    traverse_end(true);
    return false;
}

// A STATE UPDATE: the page's address `url` and its state a copy of
// `state` (NULL for none), the entry that was current kept behind it when
// `push`; told to the page's binding when `tell` (the binding did not make
// the move itself), after everything that could fail on the window's side
// and before any of it changes, so the two agree. Nothing is owed. False
// on no memory, with nothing changed on either side.
static bool page_update(Page *p, const char *url, const char *state, size_t length, bool push, bool tell)
{
    size_t cap;
    char *address = page_address(p, &cap);
    if (os64_strlen(url) >= cap)
        return false;
    char *kept = NULL;
    if (state != NULL) {
        kept = os64_malloc(length + 1);
        if (kept == NULL)
            return false;
        os64_memcpy(kept, state, length);
        kept[length] = '\0';
    }
    // The page on screen while a traversal is fetched changes its own
    // address and state and not the session (g.traversing).
    bool held = p == &g.page && traverse_pending();
    if ((push && p != &g.page && !page_stack_room(&p->back, p->nback, 1)) || (held && !page_hold()) ||
        (tell && !yonder_scripts_history_moved(page_scripts(p), url, state, length))) {
        os64_free(kept);
        return false;
    }
    if (held) {
        g.held_push = g.held_push || push;
    } else if (push) {
        way_crumb_t left = page_entry_left(p);
        if (p == &g.page) {
            way_remember_entry(&g.way, &left);
        } else {
            page_stack_push(p->back, &p->nback, &left);
            page_stack_clear(p->ahead, &p->nahead);
        }
    }
    if (push)
        p->entry = ++g.entries_made;
    os64_free(p->entry_state);
    p->entry_state = kept;
    os64_strcopy(address, cap, url);
    page_address_moved(p);
    return true;
}

// A FRAGMENT NAVIGATION of a page to `url`: an entry added (the current
// one replaced when `replace`) with no state, and hashchange from where it
// was owed when the fragment moved. `tell` when the page's binding has not
// made the move itself, which then owes popstate too; a move the binding
// made fires its popstate there (os64_dom_history_t.fragment). The scroll
// is the caller's. False on no memory, with nothing changed (page_update).
static bool page_fragment(Page *p, const char *url, bool replace, bool tell)
{
    size_t cap;
    char old[OS64_FETCH_URL_MAX];
    os64_strcopy(old, sizeof(old), page_address(p, &cap));
    if (!page_update(p, url, NULL, 0, !replace, tell))
        return false;
    if (tell)
        page_event_owe(p, false, NULL);
    if (fragments_differ(old, url))
        page_event_owe(p, true, old);
    return true;
}

// A fragment navigation's scroll on the page on screen: to `target`, or
// by the fragment's name; a name nothing in the page claims scrolls
// nowhere, but `top`, which is the page's top (HTML § Scroll to the
// fragment).
static void fragment_scroll(const os64_html_node_t *target, const char *name)
{
    if (target != NULL) {
        scroll_to_node(target);
        return;
    }
    const os64_html_node_t *node = NULL;
    if (os64_page_resolve_fragment(page_model(&g.page), name, &node) == OS64_PAGE_REASON_OK)
        scroll_to_node(node);
    else if (name[0] == '\0' || os64_streq_nocase(name, "top"))
        scroll_to(g.sx, 0);
}

// A TRAVERSAL of the page on screen `delta` entries back (below 0) or
// forward, when every entry it crosses is this document's: no fetch; the
// address and the state the entry's; popstate owed, and hashchange when
// the fragment moved; the scroll put back where it was unless the page
// keeps its own. False, with nothing changed, when they are not all its;
// true, with nothing changed but the status line, when the page's binding
// had no memory to move.
static bool session_traverse(int32_t delta)
{
    bool back = delta < 0;
    const way_crumb_t *stack = back ? g.way.history : g.way.forward;
    int32_t count = back ? g.way.depth : g.way.ahead;
    if (g.page.document == 0 || delta == 0 || steps_of(delta) > count)
        return false;
    int32_t n = (int32_t)steps_of(delta);
    for (int32_t k = 1; k <= n; k++)
        if (stack[count - k].document != g.page.document)
            return false;
    // The binding hears the entry first, as the one step that can fail:
    // a window that moved without it would show one address while the
    // page's scripts read another.
    const way_crumb_t *target = &stack[count - n];
    if (!yonder_scripts_history_moved(g.page.scripts, target->url, target->state,
                                      target->state != NULL ? os64_strlen(target->state) : 0)) {
        status_rest("Out of memory moving this page to that entry; it is where it was.");
        return true;
    }
    char old[OS64_FETCH_URL_MAX];
    os64_strcopy(old, sizeof(old), g.page.way.url);
    // Only the entry being left takes where the person is now; each one
    // passed over goes to the other side as it was, its own scroll and
    // state with it.
    way_crumb_t left = page_entry_left(&g.page), arrived;
    for (int32_t k = 0; k < n; k++) {
        if (back)
            way_went_back_entry(&g.way, &left, &arrived);
        else
            way_went_forward_entry(&g.way, &left, &arrived);
        if (k + 1 < n)
            left = arrived;
    }
    os64_strcopy(g.page.way.url, sizeof(g.page.way.url), arrived.url);
    g.page.entry = arrived.entry;
    g.page.entry_state = arrived.state;
    way_position_t at = arrived.position;
    page_address_moved(&g.page);
    page_event_owe(&g.page, false, NULL);
    if (fragments_differ(old, g.page.way.url))
        page_event_owe(&g.page, true, old);
    if (!yonder_scripts_scroll_manual(g.page.scripts)) {
        position_restore(&at);
        scroll_to(g.sx, g.sy);
    }
    g.chain = 0;
    buttons_follow();
    return true;
}

// The same for a page not yet shown, within the entries its scripts made:
// false when they do not reach `delta` away, and true, with nothing
// changed, when there was no memory to move.
static bool page_traverse(Page *p, int32_t delta)
{
    bool back = delta < 0;
    way_crumb_t **from = back ? &p->back : &p->ahead, **to = back ? &p->ahead : &p->back;
    int32_t *nfrom = back ? &p->nback : &p->nahead, *nto = back ? &p->nahead : &p->nback;
    if (delta == 0 || steps_of(delta) > *nfrom)
        return false;
    int32_t n = (int32_t)steps_of(delta);
    // Everything that can fail before anything moves, the binding last
    // (session_traverse says why).
    const way_crumb_t *target = &(*from)[*nfrom - n];
    if (!page_stack_room(to, *nto, n) ||
        !yonder_scripts_history_moved(page_scripts(p), target->url, target->state,
                                      target->state != NULL ? os64_strlen(target->state) : 0))
        return true;
    size_t cap;
    char *address = page_address(p, &cap);
    char old[OS64_FETCH_URL_MAX];
    os64_strcopy(old, sizeof(old), address);
    for (int32_t k = 0; k < n; k++) {
        way_crumb_t left = page_entry_left(p);
        page_stack_push(*to, nto, &left);
        way_crumb_t arrived = (*from)[--*nfrom];
        os64_strcopy(address, cap, arrived.url);
        p->entry = arrived.entry;
        p->entry_state = arrived.state;
    }
    page_address_moved(p);
    page_event_owe(p, false, NULL);
    if (fragments_differ(old, address))
        page_event_owe(p, true, old);
    // Its arrival scrolls to the fragment the entry it is at names.
    char *fragment;
    bool *has;
    if (page_fragment_slot(p, &fragment, &cap, &has)) {
        size_t at = before_fragment(address);
        *has = address[at] == '#';
        if (*has)
            os64_strcopy(fragment, cap, address + at + 1);
    }
    return true;
}

// The page arriving is the entry `entry`, which a traversal or a reload
// brings back: its document reads that entry's state, a copy of `state`
// (HTML § Restore the history object state), from before its first script
// (stream_scripts_entry). No memory for the copy leaves it none.
static void stream_entry(uint64_t entry, const char *state, bool traversal)
{
    if (!g.stream.active)
        return;
    g.stream.page.entry = entry;
    g.stream.page.arrives_at = traversal ? entry : 0;
    os64_free(g.stream.page.entry_state);
    g.stream.page.entry_state = state != NULL ? copy_text(state) : NULL;
}

// A host just made for the arriving page reads its entry's state before
// any of its scripts run. No memory to tell it leaves the page's state
// none, so the window and the page agree.
static void stream_scripts_entry(void)
{
    const char *state = g.stream.page.entry_state;
    if (g.stream.scripts == NULL || state == NULL)
        return;
    if (!yonder_scripts_history_moved(g.stream.scripts, g.stream.head.url, state, os64_strlen(state))) {
        os64_free(g.stream.page.entry_state);
        g.stream.page.entry_state = NULL;
    }
}

// history.go(delta), Back and Forward for the page on screen: a traversal
// within its document when every entry it crosses is its own; else the
// entry `delta` away fetched, which the session moves to, by its name,
// when it arrives (arrive_now; g.traversing until then). Nothing when
// there is no such entry, which the status line says. `by` is who asked:
// the browser's Back and Forward, or the page at `from` (its history.go).
static void session_go(int32_t delta, way_initiator_t by, const char *from)
{
    // A traversal asked while another is fetched is measured from the
    // entry on screen: the one in flight is stopped first (the HTML queue
    // would run the second after the first; this window runs one).
    if (traverse_pending())
        stop_trip();
    if (delta == 0 || session_traverse(delta))
        return;
    bool back = delta < 0;
    way_crumb_t crumb;
    if (!(back ? way_last(&g.way, &crumb) : way_next(&g.way, &crumb))) {
        say_way();
        return;
    }
    if (steps_of(delta) > (back ? g.way.depth : g.way.ahead)) {
        status_rest(back ? "This window has not been that far back." : "This window has not been that far forward.");
        return;
    }
    int32_t n = (int32_t)steps_of(delta);
    crumb = back ? g.way.history[g.way.depth - n] : g.way.forward[g.way.ahead - n];
    // The state stays the session's crumb's: the arriving page takes a
    // copy (stream_entry), and the session is untouched until it arrives.
    char *state = crumb.state != NULL ? copy_text(crumb.state) : NULL;
    crumb.state = NULL;
    g.chain = 0;
    // The browser's Back and Forward replay how the entry was first asked
    // for, as Chrome 156 and Firefox 158 do (Back to a typed entry sends
    // Sec-Fetch-Site none and ?1 again: capture-27-chrome-win-get-root and
    // capture-44-firefox-linux-get-root); an entry that kept nothing goes
    // as the browser's own. A page's history.go is the page's, from
    // `from`. A traversal's Referer names nothing (trip_send), whatever
    // `from` is.
    g.replaying = by == WAY_BY_BROWSER && crumb.asked.dest == WAY_DEST_DOCUMENT ? &crumb.asked : NULL;
    start_trip(crumb.url, NULL, from, back ? NAV_BACK : NAV_FORWARD, by, &crumb.position);
    g.replaying = NULL;
    if (g.stream.active) {
        stream_entry(crumb.entry, state, true);
        g.traversing = true;
    }
    os64_free(state);
}

// How many entries a page's session holds, for history.length: the
// session's and the page on screen's; for a page not yet shown, its own
// and, when a page is shown, the session as its arrival leaves it
// (arrive_now): the entries before the one it arrives at (unshown_at), its
// own, and those after that entry, of which there are none for a new way's
// page or for a page that pushed, its push having let go of them.
static uint32_t page_entries(const Page *p)
{
    uint32_t session = (uint32_t)g.way.depth + 1 + (uint32_t)g.way.ahead;
    if (p == &g.page)
        return session;
    uint32_t own = (uint32_t)p->nback + 1 + (uint32_t)p->nahead;
    if (g.page.tree == NULL)
        return own;
    int64_t at;
    bool fresh = unshown_at(p, &at);
    int64_t after = fresh || p->nback > 0 || p->nahead > 0 ? 0 : (int64_t)g.way.ahead - at;
    return (uint32_t)((int64_t)g.way.depth + at + own + after);
}

// The first event a page owes, as one task: true when one ran.
static bool page_event_run(Page *p, bool shown)
{
    yonder_scripts_t *host = page_scripts(p);
    while (p->nevents > 0) {
        PageEvent e = p->events[0];
        os64_memmove(p->events, p->events + 1, (size_t)(p->nevents - 1) * sizeof(p->events[0]));
        p->nevents--;
        const os64_html_document_t *doc = page_doc(p);
        os64_js_outcome_t out;
        int64_t began = task_begin(host);
        os64_dom_history_event_t what = {e.hashchange, e.old_url, e.new_url, e.state,
                                         e.state != NULL ? os64_strlen(e.state) : 0, e.entry};
        bool ran = yonder_scripts_history_event(host, &what, &out);
        page_event_free(&e);
        if (!ran)
            continue;
        task_said(host, true, e.hashchange ? "hashchange" : "popstate", &out, began);
        if (!shown || script_rebuild())
            asks_perform(host, doc, shown);
        return true;
    }
    return false;
}

// ── The binding's providers for the session and the view ────────────────

static bool history_update(void *opaque, const char *url, const char *state, size_t length, bool push)
{
    Page *p = page_of_doc(opaque);
    return p != NULL && page_update(p, url, state, length, push, false);
}

static bool history_fragment(void *opaque, const char *url, bool replace)
{
    Page *p = page_of_doc(opaque);
    if (p == NULL || !page_fragment(p, url, replace, false))
        return false;
    const char *name = url + before_fragment(url);
    name += *name == '#';
    char *fragment;
    size_t cap;
    bool *has;
    if (p == &g.page)
        fragment_scroll(NULL, name);
    else if (page_fragment_slot(p, &fragment, &cap, &has)) {
        os64_strcopy(fragment, cap, name);
        *has = true;
    }
    return true;
}

static uint32_t history_length(void *opaque)
{
    const Page *p = page_of_doc(opaque);
    return p != NULL ? page_entries(p) : 1;
}

// Device pixels at `zoom` (thousandths) as CSS pixels.
static double css_px(int32_t device, uint32_t zoom)
{
    return (double)device * 1000.0 / (zoom != 0 ? zoom : 1000);
}

static void view_read(void *opaque, os64_dom_view_t *out)
{
    const Page *p = page_of_doc(opaque);
    int32_t w = g.view.bounds.w > 0 ? g.view.bounds.w : 1, h = g.view.bounds.h > 0 ? g.view.bounds.h : 1;
    garb_env_t css = css_view(w, h, g.zoom);
    os64_memset(out, 0, sizeof(*out));
    out->width = (int32_t)css.width;
    out->height = (int32_t)css.height;
    os64_gui_rect_t root = os64_ui_widget_planned_bounds(&g.root);
    out->outer_width = g.frame_w > 0 ? g.frame_w : root.w;
    out->outer_height = g.frame_h > 0 ? g.frame_h : root.h;
    out->dark = s_dark;
    out->reduced_motion = s_reduce_motion;
    if (p == &g.page) {
        uint32_t zoom = g.page.laid_zoom != 0 ? g.page.laid_zoom : g.zoom;
        out->scroll_x = css_px(g.sx, zoom);
        out->scroll_y = css_px(g.sy, zoom);
    } else if (p != NULL && p->scroll_asked) {
        out->scroll_x = p->ask_x;
        out->scroll_y = p->ask_y;
    }
}

// The page whose document is `doc` laid out as it stands, at the view's
// size, for a script: the page on screen's own layout made current (its
// pending rebuild, an owed relayout), or a page not on screen measured
// beside itself (measured_layout). The tree, and in `env` what it was laid
// out with; NULL when no current layout could be made. What it cost goes
// in `layouts` and `elapsed_us`, as geometry's does.
static const flow_tree_t *layout_current(const os64_html_document_t *doc, flow_env_t *env,
                                         uint64_t *layouts, uint64_t *elapsed_us)
{
    int64_t began = os64_micros();
    uint64_t before = geometry_layout_total;
    int32_t width = g.view.bounds.w > 0 ? g.view.bounds.w : 1;
    int32_t height = g.view.bounds.h > 0 ? g.view.bounds.h : 1;
    const Page *p = page_of_doc(doc);
    const flow_tree_t *tree = NULL;
    *env = s_env;
    env->timeline = NULL;
    env->ticks = false;
    if (p == &g.page) {
        bool ready = script_rebuild();
        if (ready && (g.page.tree == NULL || flow_incomplete(g.page.tree) || g.page.laid_width != width ||
                      g.page.laid_height != height || g.page.laid_zoom != g.zoom || g.page.sheets_changed))
            ready = relayout(true);
        if (ready && g.page.tree != NULL) {
            tree = g.page.tree;
            env->cascade = g.page.cascade;
            env->zoom = g.page.laid_zoom;
            env->viewport_height = (int32_t)css_view(1, g.page.laid_height, g.page.laid_zoom).height;
            env->scripting = g.page.scripting;
        }
    } else if (p != NULL && measured_for(doc, width, height)) {
        tree = s_measured.tree;
        env->cascade = s_measured.cascade;
        env->zoom = s_measured.zoom;
        env->viewport_height = (int32_t)css_view(1, s_measured.height, s_measured.zoom).height;
        env->scripting = p->scripting;
    }
    int64_t ended = os64_micros();
    *layouts = geometry_layout_total - before;
    *elapsed_us = *layouts != 0 && began >= 0 && ended >= began ? (uint64_t)(ended - began) : 0;
    return tree;
}

static bool view_scroll(void *opaque, double x, double y, os64_dom_geometry_stats_t *cost)
{
    Page *p = page_of_doc(opaque);
    if (p == NULL)
        return false;
    // Before it is shown a page's scroll is only asked for: it is made
    // when the page is, and reads back as asked meanwhile.
    if (p != &g.page) {
        p->scroll_asked = true;
        p->ask_x = x > 0 ? x : 0;
        p->ask_y = y > 0 ? y : 0;
        return true;
    }
    flow_env_t env;
    if (layout_current(opaque, &env, &cost->layouts, &cost->elapsed_us) == NULL)
        return false;
    double z = (g.page.laid_zoom != 0 ? g.page.laid_zoom : g.zoom) / 1000.0;
    scroll_to(device_px(x * z), device_px(y * z));
    return true;
}

// A background url() a sheet wrote, resolved as the page on screen's
// painter fetches it (css_url).
static bool computed_url(void *ctx, const char *written, uint32_t len, int32_t sheet, char *out, size_t cap)
{
    flow_layer_t layer;
    os64_memset(&layer, 0, sizeof(layer));
    layer.image = written;
    layer.image_len = len;
    layer.sheet = sheet;
    return css_url(ctx, &layer, out, cap);
}

// getComputedStyle's provider: the element's resolved style read from the
// page's current layout (libflow's flow_computed), every property of
// libgarb's table put. An element with no style puts nothing.
static bool script_computed(void *opaque, const os64_html_node_t *element, os64_dom_computed_t *out)
{
    const os64_html_document_t *doc = opaque;
    if (!os64_html_owns_node(doc, element))
        return false;
    flow_env_t env;
    uint64_t layouts = 0, elapsed_us = 0;
    const flow_tree_t *tree = layout_current(doc, &env, &layouts, &elapsed_us);
    os64_dom_computed_cost(out, layouts, elapsed_us);
    if (tree == NULL)
        return false;
    const Page *p = page_of_doc(doc);
    // Only the page on screen's sheet table is its cascade's: a measured
    // page's url() is written as its sheet wrote it.
    flow_computed_options_t options = {p == &g.page ? computed_url : NULL, (void *)p};
    flow_computed_t *c = flow_computed_new(tree, &env, element, &options);
    if (c == NULL)
        return false;
    bool kept = true;
    char small[512];
    for (int32_t i = 0; kept && flow_computed_styled(c) && i < GARB_NPROPS; i++) {
        size_t n = flow_computed_text(c, i, small, sizeof(small));
        if (n < sizeof(small)) {
            kept = os64_dom_computed_put(out, i, small, n);
            continue;
        }
        char *big = os64_malloc(n + 1);
        kept = big != NULL;
        if (kept) {
            flow_computed_text(c, i, big, n + 1);
            kept = os64_dom_computed_put(out, i, big, n);
        }
        os64_free(big);
    }
    flow_computed_free(c);
    return kept;
}

// THE SHOWN PAGE'S TURN: at most one task, then the rendering step.
// Whether the turn just ran one of the view's events: more may be owed.
static bool s_view_ran;

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
    // The history's events the page is owed come before its other tasks.
    if (page_event_run(&g.page, true))
        return true;
    const os64_html_document_t *doc = page_doc(&g.page);
    os64_js_outcome_t out;
    int64_t began = task_begin(g.page.scripts);
    // The rendering step's events (MODERN_WEB.md § Slice 1b): the view's
    // resize and scroll, and the media lists' changes, one a turn, after
    // the page's tasks, and the loop comes back for the next while one ran
    // (s_view_ran). While the tasks keep coming (a message that posts
    // another) they still run once a rendering opportunity
    // (yonder_scripts_view_due), first in the turn, as HTML's event loop
    // updates the rendering between tasks.
    s_view_ran = false;
    const char *what = "view";
    bool ran = yonder_scripts_view_due(g.page.scripts) &&
               (s_view_ran = yonder_scripts_view_step(g.page.scripts, &out));
    if (!ran) {
        what = "script";
        ran = yonder_scripts_step(g.page.scripts, &out);
    }
    if (!ran)
        ran = queued_task(g.page.scripts, &what, &out);
    if (!ran) {
        what = "view";
        ran = s_view_ran = yonder_scripts_view_step(g.page.scripts, &out);
    }
    // And when the turn found nothing else, an idle callback (MODERN_WEB.md
    // § P2b): the loop is idle. The loop comes back for the next while one
    // is due (main's bell), and the ticker wakes it for one that waits
    // (pictures_schedule). Tasks that keep coming leave none idle, which
    // HTML allows: an idle period starts only when the loop is idle.
    if (!ran) {
        what = "idle";
        ran = yonder_scripts_idle_run(g.page.scripts, &out);
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

// PAGE STORAGE'S TURN: each page's storage commits what is due when no task
// has (a task's own end commits what is due then), and a store that could
// not be kept is said on the status line, once.
static void storage_turn(void)
{
    yonder_scripts_t *hosts[] = {g.page.scripts, g.stream.scripts, g.coming.active ? g.coming.page.scripts : NULL};
    uint64_t now = yonder_now_ms();
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
        if (yonder_scripts_storage_due(hosts[i]) <= now)
            yonder_scripts_storage_settle(hosts[i]);
    const char *problem = yonder_storage_problem(s_storage);
    if (problem != NULL && !os64_streq(problem, s_storage_said)) {
        os64_strcopy(s_storage_said, sizeof(s_storage_said), problem);
        status_rest(problem);
    }
}

// THE COOKIE FILE'S TURN: the write the jar's changes owe it, when it is
// due (cookies.h), and a file that could not be kept said on the status
// line, once.
static void cookies_turn(void)
{
    yonder_cookies_turn(s_cookies, yonder_now_ms());
    const char *problem = yonder_cookies_problem(s_cookies);
    if (problem != NULL && !os64_streq(problem, s_cookies_said)) {
        os64_strcopy(s_cookies_said, sizeof(s_cookies_said), problem);
        status_rest(problem);
    }
}

// Settings' cookie switches. With no file (no memory for it), `off` still
// refuses cookies, and the jar lasts as long as the window either way.
static void cookies_use(yonder_cookies_mode_t mode)
{
    if (s_cookies != NULL)
        yonder_cookies_set_mode(s_cookies, mode);
    else
        way_jar_refuse(g.way.jar, mode == YONDER_COOKIES_OFF);
    s_cookies_said[0] = '\0';
}

// Why the cookie file is not kept, for Settings as it opens: NULL while it is.
static const char *cookies_problem(void)
{
    return yonder_cookies_problem(s_cookies);
}

// Settings' "Clear cookies": the jar emptied, and the file.
static void cookies_clear(char *line, size_t cap)
{
    if (s_cookies == NULL) {
        int32_t had = way_jar_count(g.way.jar);
        way_jar_clear(g.way.jar);
        os64_snprintf(line, cap, "Cleared this window's cookies: %d gone.", (int)had);
        return;
    }
    int64_t gone = yonder_cookies_clear(s_cookies);
    s_cookies_said[0] = '\0';
    if (gone < 0)
        os64_snprintf(line, cap, "Cleared this window's cookies; %s could not be emptied.",
                      yonder_cookies_path(s_cookies));
    else if (yonder_cookies_path(s_cookies)[0] == '\0')
        os64_snprintf(line, cap, "Cleared this window's cookies: %ld gone (none are kept in a file).", (long)gone);
    else
        os64_snprintf(line, cap, "Cleared cookies: %ld gone, and the file is empty.", (long)gone);
}

// Settings' "Clear page storage": the store emptied, and every page open
// now forgets what it had, so its next touch reads the empty store.
static void storage_clear(char *line, size_t cap)
{
    if (s_storage == NULL) {
        os64_strcopy(line, cap, "Page storage is not kept in this window; there is nothing to clear.");
        return;
    }
    int64_t removed = yonder_storage_clear(s_storage);
    yonder_scripts_t *hosts[] = {g.page.scripts, g.stream.scripts, g.coming.active ? g.coming.page.scripts : NULL};
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
        yonder_scripts_storage_forget(hosts[i]);
    s_storage_said[0] = '\0';
    if (removed < 0)
        os64_strcopy(line, cap, "Cleared this window's page storage; some stored files could not be removed.");
    else
        os64_snprintf(line, cap, "Cleared page storage: %ld stored site%s gone.", (long)removed,
                      removed == 1 ? "" : "s");
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
                        int32_t x, int32_t y, uint8_t was)
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
    // A press is held from its mousedown until its mouseup; the mouseup and
    // its click are queued before the press is let go.
    bool held = g.pressed_node != NULL && kind != IN_MOUSEUP && kind != IN_CLICK;
    g.inputs[g.ninputs++] = (Input){kind, node, related, doc, x, y, was, held};
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

// The element under a window position, and the link it is in, if any; and,
// in `boxed`, the element whose box was hit, which differs from the answer
// over an image map: the answer is the region's `area`, the box the
// picture's.
static const os64_html_node_t *element_hit(int32_t x, int32_t y, const os64_html_node_t **link,
                                           const os64_html_node_t **boxed)
{
    os64_gui_rect_t v = g.view.bounds;
    if (link != NULL)
        *link = NULL;
    if (boxed != NULL)
        *boxed = NULL;
    if (g.page.tree == NULL || x < v.x || y < v.y || x >= v.x + v.w || y >= v.y + v.h)
        return NULL;
    const flow_box_t *b = flow_hit(g.page.tree, x - v.x + g.sx, y - v.y + g.sy, scroll_now());
    if (b == NULL)
        return NULL;
    if (boxed != NULL)
        *boxed = element_of(b->node);
    // Where in the picture: the point taken into the box, through any
    // transform it is drawn through, and then into the content box the
    // painter draws the picture in. That is yonder_box_edge's, not the
    // box's `content`, which libflow leaves empty on an inline picture's
    // atom.
    flow_point_t at;
    bool in = flow_box_point(b, scroll_now(), x - v.x + g.sx, y - v.y + g.sy, &at);
    os64_gui_rect_t content = yonder_box_edge(b, b->rect, FLOW_EDGE_CONTENT);
    int64_t px = (int64_t)at.x - content.x, py = (int64_t)at.y - content.y;
    if (in && px >= 0 && py >= 0 && px < content.w && py < content.h) {
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

static const os64_html_node_t *element_at(int32_t x, int32_t y, const os64_html_node_t **link)
{
    return element_hit(x, y, link, NULL);
}

// A mouse event of the primary button, the one yonder hands a page: a
// press, a release and a click count one click (UIEvent's detail), and the
// button is in `buttons` while it is held.
static os64_dom_event_t mouse_event(const char *type, bool cancelable, const Input *in)
{
    os64_dom_event_t ev = {.type = type, .bubbles = true, .cancelable = cancelable,
                           .kind = OS64_DOM_EVENT_MOUSE};
    bool press = in->kind == IN_MOUSEDOWN || in->kind == IN_MOUSEUP || in->kind == IN_CLICK ||
                 in->kind == IN_BUTTON || in->kind == IN_CHECK;
    ev.detail = press ? 1 : 0;
    ev.buttons = in->held ? 1 : 0;
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

// An event of `kind` (a FocusEvent, an InputEvent, a SubmitEvent) at `node`,
// `related` its other node: a focus change's, or a submission's submitter.
static bool kind_event(const char *type, bool bubbles, bool cancelable, os64_dom_event_kind_t kind,
                       const os64_html_node_t *node, const os64_html_node_t *related)
{
    os64_dom_event_t ev = {.type = type, .bubbles = bubbles, .cancelable = cancelable, .kind = kind,
                           .related = related};
    return page_event(&ev, node);
}

// The form node a control belongs to, for its submit and reset events.
static const os64_html_node_t *control_form(int32_t control)
{
    const os64_page_control_t *c = os64_page_control(page_model(&g.page), control);
    const os64_page_form_t *f = c != NULL ? os64_page_form(page_model(&g.page), c->form) : NULL;
    return f != NULL ? f->node : NULL;
}

// A person's submission from `control`: submit at its form, a SubmitEvent
// whose submitter is the one libpage's submission takes (none when the form
// submits itself). True when a listener cancelled it.
static bool submit_event(int32_t control, os64_page_activation_t how)
{
    const os64_page_t *page = page_model(&g.page);
    os64_page_what_t what = {how, control, 0, 0};
    int32_t submitter = -1;
    const os64_page_control_t *by =
        os64_page_submitter(page, what, &submitter) ? os64_page_control(page, submitter) : NULL;
    return kind_event("submit", true, true, OS64_DOM_EVENT_SUBMIT, control_form(control),
                      by != NULL ? by->node : NULL);
}

// A person's submission from `control` (a button pressed, Enter in a
// field), validated by the page's scripts before its submit event (HTML §
// 4.10.21.3): false when it is invalid, and then the first problem whose
// invalid event nobody cancelled is reported.
// Only a submission that will happen is validated, by the submitter libpage
// would take. `*validated` says the scripts' task did it and finished, so
// the model must not judge the submission again after its submit event;
// without scripts, or with a task that failed, the model's validation in
// form_send is the whole of it.
static bool page_validates(int32_t control, os64_page_activation_t how, bool *validated)
{
    yonder_scripts_t *host = g.page.scripts;
    *validated = false;
    if (!scripts_live())
        return true;
    forms_flush();
    const os64_page_t *page = page_model(&g.page);
    os64_page_what_t what = {how, control, 0, 0};
    int32_t submitter = -1;
    const os64_html_node_t *form = control_form(control);
    if (form == NULL || !os64_page_submitter(page, what, &submitter))
        return true;
    const os64_page_control_t *by = os64_page_control(page, submitter);
    const os64_html_document_t *doc = page_doc(&g.page);
    os64_js_outcome_t out;
    bool valid = true;
    int64_t began = task_begin(host);
    if (yonder_scripts_form_validate(host, form, by != NULL ? by->node : NULL, &valid, &out)) {
        *validated = out.status == OS64_JS_OK;
        task_said(host, true, "invalid", &out, began);
        if (script_rebuild())
            asks_perform(host, doc, true);
    }
    return valid;
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
// control it left, `focus` for the one it reached (neither bubbles), each
// naming the other control as its relatedTarget.
static void focus_follow(void)
{
    if (g.ui.focus == g.focus_seen)
        return;
    g.focus_seen = g.ui.focus;
    FormWidget *fw = NULL;
    for (int32_t i = 0; i < g.nfw && fw == NULL; i++)
        if (g.fw[i]->w == g.ui.focus)
            fw = g.fw[i];
    const os64_html_node_t *reached = fw != NULL && scripts_live() ? fw->node : NULL;
    const os64_html_node_t *left = g.focus_node;
    const os64_html_document_t *left_doc = g.focus_doc;
    if (left != NULL) {
        // Judged now, while the value it had at focus is still at hand.
        const char *text;
        size_t len;
        if (editor_text(left, &text, &len) &&
            (len != g.focus_len || os64_memcmp(text, g.focus_value, len) != 0))
            input_queue(IN_CHANGE, left, NULL, 0, 0, false);
        input_queue(IN_BLUR, left, reached, 0, 0, false);
        g.focus_node = NULL;
    }
    if (reached != NULL) {
        g.focus_doc = page_doc(&g.page);
        g.focus_node = reached;
        os64_html_hold(g.focus_doc, g.focus_node);
        const char *text;
        size_t len = 0;
        if (!editor_text(reached, &text, &len))
            text = "";
        g.focus_len = len < sizeof(g.focus_value) ? len : sizeof(g.focus_value) - 1;
        os64_memcpy(g.focus_value, text, g.focus_len);
        input_queue(IN_FOCUS, reached, left, 0, 0, false);
    }
    // Let go only now: the focus event held it as its relatedTarget.
    if (left != NULL)
        os64_html_release(left_doc, left);
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
            if (!resets && !submits)
                break;
            bool validated = false;
            if (submits) {
                if (!page_validates(control, OS64_PAGE_ACTIVATE_CONTROL, &validated))
                    break;
                // The invalid events' listeners may have rebuilt the page.
                control = os64_page_control_for(page_model(&g.page), in.node);
                if (control < 0)
                    break;
            }
            if (resets ? plain_event("reset", true, true, control_form(control))
                       : submit_event(control, OS64_PAGE_ACTIVATE_CONTROL))
                break;
            // The listeners may have rebuilt the page: the button is found
            // again, and is gone if they took it away.
            control = os64_page_control_for(page_model(&g.page), in.node);
            c = os64_page_control(page_model(&g.page), control);
            if (c != NULL && resets)
                form_reset(c->form);
            else if (c != NULL)
                form_send_as(control, OS64_PAGE_ACTIVATE_CONTROL, NAV_GO, WAY_BY_PERSON, validated);
            break;
        }
        case IN_IMPLICIT: {
            change_if_edited(in.node);
            control = os64_page_control_for(page_model(&g.page), in.node);
            bool validated = false;
            if (!page_validates(control, OS64_PAGE_ACTIVATE_IMPLICIT, &validated))
                break;
            control = os64_page_control_for(page_model(&g.page), in.node);
            if (!submit_event(control, OS64_PAGE_ACTIVATE_IMPLICIT))
                form_send_as(os64_page_control_for(page_model(&g.page), in.node), OS64_PAGE_ACTIVATE_IMPLICIT, NAV_GO,
                             WAY_BY_PERSON, validated);
            break;
        }
        case IN_CHECK: {
            os64_dom_event_t ev = mouse_event("click", true, &in);
            if (page_event(&ev, in.node)) {
                // A cancelled click puts the box back, and its
                // indeterminate flag, which the tick cleared.
                control = os64_page_control_for(page_model(&g.page), in.node);
                if (os64_page_set_checked(page_model(&g.page), control, (in.was & WAS_CHECKED) != 0) >= 0)
                    (void)os64_page_node_set_indeterminate(os64_page_shared_state(page_model(&g.page)), in.node,
                                                           (in.was & WAS_INDETERMINATE) != 0);
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
            kind_event("input", true, false, OS64_DOM_EVENT_INPUT, in.node, NULL);
            break;
        case IN_FOCUS:
            kind_event("focus", false, false, OS64_DOM_EVENT_FOCUS, in.node, in.related);
            break;
        case IN_BLUR:
            kind_event("blur", false, false, OS64_DOM_EVENT_FOCUS, in.node, in.related);
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

static void input_lost(void)
{
    g.seq = g.key_seq = 0;
    g.pressed_link = -1;
    if (g.pressed_node != NULL)
        os64_html_release(g.pointer_doc, g.pressed_node);
    g.pressed_node = NULL;
    g.bar.pressed = BAR_ELSEWHERE;
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
    // A keypress's codes are its character's, keyCode as well as charCode
    // (Blink's answer, and so which's: 97 for 'a', where keydown says 65).
    key.type = "keypress";
    key.char_code = a;
    key.key_code = a;
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

// A job the pool finished, read here by its kind and handed to what asked
// for it. A navigation's fetch said everything it had to say down
// the mailbox before it returned, so its job is only let go, and one
// finished before its cancel was noticed is let go the same way.
static void reaped(os64_work_id_t id, void *job, void *product)
{
    // Work came back: an `--exit-when-loaded` run's quiet starts again.
    s_exit_quiet_from = yonder_now_ms();
    if (*(const uint32_t *)job == YONDER_JOB_PICTURE) {
        int32_t waiting = g.page.waiting;
        picture_arrived(job, product);
        yonder_picture_release(job, product);
        pictures_feed(&g.page);
        if (waiting > 0 && g.page.waiting == 0 && s_web.waiting == 0)
            diag_pictures_in(&g.page);
        return;
    }
    if (*(const uint32_t *)job == YONDER_JOB_FONT) {
        int32_t waiting = s_web.waiting;
        webfont_arrived(job, product);
        yonder_webfont_release(job, product);
        webfonts_feed(false);
        if (waiting > 0 && s_web.waiting == 0 && g.page.waiting == 0)
            diag_pictures_in(&g.page);
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
    if (*(const uint32_t *)job == YONDER_JOB_FETCH) {
        request_arrived(job, product);
        yonder_fetch_release(job, product);
        return;
    }
    if (*(const uint32_t *)job == YONDER_JOB_DOWNLOAD) {
        download_arrived(id, job);
        yonder_download_release(job, product);
        return;
    }
    // A compiled form written, or not: either way nothing waits on it.
    if (*(const uint32_t *)job == YONDER_JOB_BYTECODE) {
        s_bytecode_queued -= ((const yonder_bytecode_job_t *)job)->length;
        yonder_bytecode_keep_release(job, product);
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
// A picture's page is yonder's own (image_page), in UTF-8. Every
// navigation's page is first parsed here, so this is where the page on
// screen departs (page_departs).
static bool stream_parser(const void *first, size_t n)
{
    page_departs();
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
    stream_mark(OS64_DOM_TIMING_DOM_LOADING);
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
    // The parser has been handed every byte it will have: a kept page's
    // bytes are whole.
    if (s_diag.keep)
        diag_kept(yonder_diag_keep_bytes_end(yonder_diag_claimed(g.stream.diag)));
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
    fresh.modern = g.stream.modern;
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
            ? scripts_host(doc, g.stream.state, g.stream.head.url, g.stream.head.encrypted, g.stream.serial,
                           g.stream.diag, g.stream.modern) : NULL;
        if (g.stream.scripts == NULL) {
            os64_page_state_free(g.stream.state);
            g.stream.state = NULL;
        }
        stream_scripts_entry();
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
            yonder_diag_head_fact(fresh.diag, "TLS", g.stream.head.tls_version == 0x0304 ? "1.3" :
                g.stream.head.tls_fallback ? "1.2 (fallback)" : "1.2");
        }
        // The server's answer, for every page from the network (0 when its
        // protocol has none), which a replay requires rather than assumes.
        // One that is an error is shown, as a browser shows it, and is a
        // failed load from the person's chair: a 429's page is not a clean
        // page.
        if (g.stream.local == NULL) {
            char status[16 + HTTP_REASON_MAX];
            os64_snprintf(status, sizeof(status), g.stream.head.reason[0] ? "%d %s" : "%d",
                          (int)g.stream.head.status, g.stream.head.reason);
            yonder_diag_head_fact(fresh.diag, "status", status);
            if (g.stream.head.status >= 400) {
                os64_snprintf(status, sizeof(status), "HTTP %d %s", (int)g.stream.head.status,
                              g.stream.head.reason);
                yonder_diag_failed(fresh.diag, "page", status);
            }
        }
        // What a replay of a kept page needs to rebuild the head it came
        // with (tools/test_yonder_replay_host.c): where it came from after
        // its redirects and what libway judged its body to be; for a page
        // from the network, its type and charset as the server named them
        // (empty when it named none: a replay requires every line, and
        // assumes none) and whether it arrived encrypted; and a page read with scripts
        // off says so. They, the status and TLS above, and the width it is
        // laid out at have room a page's other plain lines cannot take
        // (yonder_diag_head_fact).
        static const char *const kBodies[] = {[WAY_BODY_HTML] = "html", [WAY_BODY_TEXT] = "text",
                                              [WAY_BODY_IMAGE] = "image"};
        yonder_diag_head_fact(fresh.diag, "final address", g.stream.head.url);
        yonder_diag_head_fact(fresh.diag, "body", kBodies[g.stream.head.body]);
        if (g.stream.local == NULL) {
            yonder_diag_head_fact(fresh.diag, "content-type", g.stream.head.content_type);
            yonder_diag_head_fact(fresh.diag, "charset", g.stream.head.charset);
            yonder_diag_head_fact(fresh.diag, "encrypted", g.stream.head.encrypted ? "yes" : "no");
        }
        if (!fresh.scripting)
            yonder_diag_head_fact(fresh.diag, "scripts", "off");
    }
    // The sheets the stream sent for move with it, still out or landed,
    // and keep their jobs, which name this serial: no sheet is fetched
    // twice in one page's life (arrive adds the rest).
    Page *found = &g.stream.page;
    // Its place in the session moves with it: its document's name, the
    // current entry's state, the entries its scripts made, the events it
    // owes, a scroll it asked for.
    fresh.document = found->document;
    fresh.entry = found->entry;
    fresh.asked = found->asked;
    fresh.arrives_at = found->arrives_at;
    fresh.entry_state = found->entry_state;
    fresh.back = found->back;
    fresh.ahead = found->ahead;
    fresh.nback = found->nback;
    fresh.nahead = found->nahead;
    os64_memcpy(fresh.events, found->events, sizeof(fresh.events));
    fresh.nevents = found->nevents;
    fresh.scroll_asked = found->scroll_asked;
    fresh.ask_x = found->ask_x;
    fresh.ask_y = found->ask_y;
    found->entry_state = NULL;
    found->back = found->ahead = NULL;
    found->nback = found->nahead = found->nevents = 0;
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
        g.finishing.kind = kind;
        page_mark(fresh.scripts, OS64_DOM_TIMING_DOM_CONTENT_LOADED_START);
        yonder_scripts_dispatch(fresh.scripts, doc->document, &loaded, NULL, &out);
        page_mark(fresh.scripts, OS64_DOM_TIMING_DOM_CONTENT_LOADED_END);
        task_said(fresh.scripts, true, "DOMContentLoaded", &out, began);
        asks_perform(fresh.scripts, doc, false);
        bool abandoned = g.finishing.abandoned;
        os64_memset(&g.finishing, 0, sizeof(g.finishing));
        if (g.stream.active || abandoned) {
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
// file's, one chunk at a time. Every byte taken goes to the parser, so this
// is where a kept page's bytes are kept (keep_pages, diag.h): as the parser
// is fed them, after the transfer's decoding and before the characters'.
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
    if (s_diag.keep && n != 0)
        diag_kept(yonder_diag_keep_bytes(yonder_diag_claimed(g.stream.diag), chunk, n));
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
    // A module runs, and an import map registers, with the Modern Web
    // surface (its host's modules.c); without it, each is what a scripted
    // page asked for and did not get. With scripts off nothing runs, and
    // that is a setting.
    os64_dom_script_kind_t kind = os64_dom_script_kind(script);
    if (g.stream.scripting && kind == OS64_DOM_SCRIPT_MODULE && !g.stream.modern)
        yonder_diag_missing(g.stream.diag, "script", "module", 1);
    if (g.stream.scripting && kind == OS64_DOM_SCRIPT_IMPORTMAP && !g.stream.modern)
        yonder_diag_missing(g.stream.diag, "script", "importmap", 1);
    if (!g.stream.scripting || g.stream.scriptless || kind == OS64_DOM_SCRIPT_NONE ||
        (kind != OS64_DOM_SCRIPT_CLASSIC && !g.stream.modern))
        return YONDER_STOP_RESUME;
    if (g.stream.scripts == NULL) {
        os64_html_document_t *doc = os64_html_parser_document(g.stream.parser);
        g.stream.state = os64_page_state_create(doc, 0);
        g.stream.scripts = g.stream.state != NULL
            ? scripts_host(doc, g.stream.state, g.stream.head.url, g.stream.head.encrypted, g.stream.serial,
                           g.stream.diag, g.stream.modern) : NULL;
        if (g.stream.scripts == NULL) {
            os64_page_state_free(g.stream.state);
            g.stream.state = NULL;
            g.stream.scriptless = true;
            yonder_diag_failed(g.stream.diag, "scripts", "Out of memory for the page's scripts; it is read without them.");
            status_rest("Out of memory for the page's scripts; it is read without them.");
            return YONDER_STOP_RESUME;
        }
        stream_scripts_entry();
    }
    return yonder_scripts_parser_stop(g.stream.scripts, script);
}

// The custom element reactions a parser call left owed (an element it
// inserted, upgraded), run as their own task now: after every parser call,
// and so before the script a parse stopped at, which then finds the
// elements above it upgraded (CUSTOM_ELEMENTS.md, decision 6). What they
// ask for (a navigation) waits for the turn's next task to perform.
static void stream_reactions(void)
{
    yonder_scripts_t *host = g.stream.scripts;
    if (!yonder_scripts_reactions_owed(host))
        return;
    os64_js_outcome_t out;
    int64_t began = task_begin(host);
    if (yonder_scripts_reactions_run(host, &out))
        task_said(host, true, "custom element reactions", &out, began);
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
            stream_reactions();
            return r;
        }
        r = os64_html_parser_resume(g.stream.parser);
    }
    stream_reactions();
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
    stream_mark(OS64_DOM_TIMING_DOM_INTERACTIVE);
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
    p->modern = g.stream.modern;
    return p;
}

// THE SHEETS THE PARSE HAS REVEALED, sent for as it finds them (DOM_D7.md
// § D7d): after every slice, at a stop that blocks and at the end, every
// `link` and `style` element the tree holds that the stream's page has no
// entry for, by libpage's own rule and order (os64_page_sheets_in, a walk
// of the tree, which costs a fraction of a model). A `style` element is
// parsed now; a `link` is fetched under the stream's serial. A walk that
// had no memory leaves them to the next look. The same looks find the
// module preloads, once the page has a script host to hold their modules.
static void stream_sheets(void)
{
    if (g.stream.parser == NULL || g.stream.head.body != WAY_BODY_HTML)
        return;
    Page *p = stream_page();
    int64_t began = s_script_audit ? os64_micros() : 0;
    int32_t had = p->nsheets;
    int32_t listed = os64_page_sheets_in(page_doc(p), g.stream.head.url, stream_sheet_add, p);
    yonder_scripts_preload(g.stream.scripts);
    if (s_script_audit) {
        char line[128];
        os64_snprintf(line, sizeof(line),
                      "yonder: stream sheets and preloads: %d sheets listed, %d new, %d out, in %ld us",
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
        // Before any of the page's scripts can ask: their host takes the
        // timing when it is made, at the parse.
        g.stream.timing.redirects = g.stream.head.redirects;
        stream_mark(OS64_DOM_TIMING_RESPONSE_START);
        // The head is final and nothing is parsed yet: the jar's cookies
        // for the page, as they stand at its head (diag_cookies).
        diag_cookies(g.stream.diag, g.stream.head.url, g.stream.head.encrypted);
        if (g.stream.head.body != WAY_BODY_TEXT && !stream_parser(NULL, 0)) {
            stream_failed("Out of memory reading that page.");
            stop_trip();
            status_rest("Out of memory reading that page.");
            return false;
        }
    }
    if (mail != NULL && !g.stream.has_verdict && yonder_mail_take_verdict(mail, &g.stream.verdict)) {
        g.stream.has_verdict = true;
        diag_cookies_short(g.stream.diag, &g.stream.verdict.cookies_short);
        stream_mark(OS64_DOM_TIMING_RESPONSE_END);
    }
    yonder_scripts_t *host = g.stream.scripts;
    uint64_t serial = g.stream.serial;
    if (yonder_scripts_connected_pending(host))
        return stream_connected();
    // The history's events a task left the page owing (a history.back()
    // while it is parsed) are tasks queued before the parse's next slice.
    if (g.stream.page.nevents > 0 && yonder_scripts_alive(host))
        return stream_task();
    if (g.stream.stopped) {
        // The script the parse waits for: run it when its source is in
        // hand (a dead runtime runs nothing), then carry on. The resume is
        // the parser carrying on, not a task; the next script it meets
        // waits for the next turn. While its `src` is out, a ready script
        // (async, or one a script connected) or a due timer runs: HTML's
        // loop runs other tasks while a parser-blocking script is fetched.
        if (!g.stream.ran) {
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
            g.stream.ran = true;
            // What it connected without a `src` runs first, on the turns
            // after this one (stream_connected, above), as a browser runs
            // it inside the verb: an import map a script connects is
            // registered before the markup after that script is parsed.
            if (yonder_scripts_connected_pending(host))
                return true;
        }
        g.stream.ran = false;
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
        // not a page. libway's sentence says which. One that was not a page,
        // and was asked for by a GET, is offered to save (§ Downloads)
        // instead of said: the page on screen stays, and the Save dialog
        // asks where it goes. It is fetched again for the download.
        char line[WAY_SENTENCE_MAX];
        os64_strcopy(line, sizeof(line), g.stream.verdict.reason);
        stream_failed(trim(line));
        way_offer_t offer = g.stream.verdict.offer;
        char referrer[OS64_FETCH_URL_MAX];
        os64_strcopy(referrer, sizeof(referrer), g.stream.referrer);
        stream_drop();
        buttons_follow();
        if (offer.url[0] != '\0') {
            if (g.page.tree != NULL)
                say_laid_out(g.laid_ms);    // the page's own line again, not the fetch's
            else
                status_rest("");
            download_ask(offer.url, referrer, offer.disposition);
        } else {
            status_rest(trim(line));
        }
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
    if ((mask & BELL_DOWNLOAD) || ((mask & BELL_TICK) && downloads_due() <= yonder_now_ms()))
        downloads_heard();
    if (mask & BELL_TICK) {
        s_cost.bells++;
        if (g.coming.active && yonder_now_ms() >= g.coming.due)
            coming_show();
        pictures_tick();
        motion_tick();
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
            pictures_orphaned(&g.page);
            webfonts_orphaned();
            downloads_orphaned();
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
// blended where it meets what is being painted. `row` holds `r.w` pixels,
// the caller's (pane_mask), so every target draws or none does.
static void glass_mask(const Glass *gl, os64_gui_rect_t r, const uint8_t *alpha, uint32_t colour,
                       uint32_t *row)
{
    os64_gui_rect_t at = on_glass(gl, r);
    if (at.w <= 0 || at.h <= 0)
        return;
    int32_t sx = at.x - (r.x + gl->dx), sy = at.y - (r.y + gl->dy);
    for (int32_t y = 0; y < at.h; y++) {
        const uint8_t *a = alpha + (size_t)(sy + y) * (size_t)r.w + (size_t)sx;
        for (int32_t x = 0; x < at.w; x++)
            row[x] = (uint32_t)a[x] << 24 | (colour & 0xffffffu);
        os64_draw_blend(gl->surf, at.x, at.y + y, row, (uint32_t)at.w, 1, (uint32_t)at.w);
    }
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

// The glass's working buffers come from the painter's scratch too.
static void *glass_take(size_t bytes)
{
    return yonder_scratch_take(g.paint_scratch, bytes);
}

static void glass_give(void *p)
{
    yonder_scratch_give(g.paint_scratch, p);
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
    uint32_t *under = glass_take((size_t)at.w * (size_t)at.h * sizeof(uint32_t));
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
    glass_give(under);
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

#define YONDER_SVG_MEMORY_MAX ((size_t)64 * 1024 * 1024)
static os64_geometry_status_t svg_allocate(void *ctx, size_t bytes, void **out)
{
    Page *p = ctx;
    *out = NULL;
    if (bytes > YONDER_SVG_MEMORY_MAX - p->svg_bytes)
        yonder_svg_cache_trim(p->svg_cache);
    if (bytes > YONDER_SVG_MEMORY_MAX - p->svg_bytes)
        return OS64_GEOMETRY_LIMIT;
    *out = os64_malloc(bytes);
    if (!*out) {
        yonder_svg_cache_trim(p->svg_cache);
        *out = os64_malloc(bytes);
    }
    if (!*out)
        return OS64_GEOMETRY_NO_MEMORY;
    p->svg_bytes += bytes;
    if (p->svg_bytes > p->svg_peak)
        p->svg_peak = p->svg_bytes;
    return OS64_GEOMETRY_OK;
}
static void svg_free(void *ctx, void *bytes, size_t size)
{
    Page *p = ctx;
    os64_free(bytes);
    p->svg_bytes -= size;
}

// Eight MiB of variants stays inside the shared 64 MiB geometry ledger.
static yonder_svg_cache_t *svg_window_cache(void)
{
    if (!g.page.svg_cache) {
        os64_geometry_memory_t memory = {&g.page, svg_allocate, svg_free};
        g.page.svg_cache = yonder_svg_cache_create(&memory, 8u << 20, 32, 256u << 10);
    }
    return g.page.svg_cache;
}

// SVG CSS work is serialized on the window, separate from the worker pool
// and retained-picture account. Document cascades cap state/read arenas and
// retained attribute parses; geometry/output share the 64 MiB ledger above.
// The 512 MiB working allowance includes those caps plus parser/match scratch.
#define YONDER_SVG_WINDOW_RESERVE ((size_t)512u << 20)
_Static_assert(YONDER_SVG_CASCADE_BYTES + YONDER_SVG_READ_BYTES +
               (size_t)YONDER_SVG_ATTRIBUTES * YONDER_SVG_ATTRIBUTE_BYTES +
               YONDER_SVG_ATTRIBUTE_BYTES + YONDER_SVG_MEMORY_MAX < YONDER_SVG_WINDOW_RESERVE,
               "SVG window allowance must leave room for CSS scratch");
// A repaint is not another omission. Keep the largest count seen in one
// successful root/image render; this is not a page-wide subtree census.
static void svg_partial_diagnostic(const yonder_svg_pixels_t *pixels, bool image)
{
    if (pixels->omitted_subtrees)
        yonder_diag_missing_seen(g.page.diag, image ? "svg image" : "svg",
            "unsupported drawing subtrees omitted (maximum per render)", pixels->omitted_subtrees);
}
static bool svg_picture_pixels(const Picture *pic, os64_gui_rect_t at, yonder_svg_pixels_t *out)
{
    *out = (yonder_svg_pixels_t){0};
    if (!g.page.modern || at.w <= 0 || at.h <= 0)
        return false;
    uint32_t zoom = g.page.laid_zoom ? g.page.laid_zoom : 1000;
    yonder_svg_image_view_t view = {
        .width = (double)at.w * 1000 / zoom, .height = (double)at.h * 1000 / zoom,
        .pixel_width = (uint32_t)at.w, .pixel_height = (uint32_t)at.h,
        .dark = s_dark, .reduced_motion = s_reduce_motion, .cache = svg_window_cache()};
    os64_geometry_memory_t memory = {&g.page, svg_allocate, svg_free};
    svg_status_t status = yonder_svg_document_render(pic->svg, &view, &memory,
                                                     os64_freetype_geometry_v1(), out);
    if (status != SVG_OK)
        yonder_diag_failed_seen(g.page.diag, "svg image",
            status == SVG_UNSUPPORTED ? "SVG image uses an unsupported feature" :
            status == SVG_MALFORMED ? "SVG image contains malformed geometry" :
                                     "SVG image rendering failed or exceeded its limits");
    else
        svg_partial_diagnostic(out, true);
    return status == SVG_OK;
}
static void svg_picture_pixels_free(yonder_svg_pixels_t *out)
{
    os64_geometry_memory_t memory = {&g.page, svg_allocate, svg_free};
    yonder_svg_pixels_free(&memory, out);
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
    yonder_svg_pixels_t vector_pixels = {0};
    if (p->pics[k].kind == YONDER_PICTURE_SVG) {
        if (!svg_picture_pixels(&p->pics[k], tile.at, &vector_pixels))
            return true;
        px = vector_pixels.pixels;
        w = (uint32_t)vector_pixels.rect.w;
        h = (uint32_t)vector_pixels.rect.h;
    }
    os64_gui_rect_t on = {area->x + gl->dx, area->y + gl->dy, area->w, area->h};
    os64_gui_rect_t cut = on_glass(gl, clip);
    tile.at.x += gl->dx;
    tile.at.y += gl->dy;
    if (cut.w > 0 && cut.h > 0)
        yonder_tile_picture(gl->surf->pixels, gl->surf->pitch_px, cut, on, tile.at, tile.repeat_x,
                            tile.repeat_y, !b->style->pixelated, px, w, h);
    svg_picture_pixels_free(&vector_pixels);
    return true;
}

// A picture that arrived is drawn where object-fit and object-position put
// it in its box, scaled and blended (scale.c), and cut to the box; one on
// its way, or that will not come, keeps its frame.
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
        // Placed by object-fit and object-position at its size at the
        // page's zoom, and cut to its content box (CSS Images 3 § 4.5).
        os64_gui_rect_t at = yonder_object_rect(b->style, c, zoomed_px(w, p->laid_zoom),
                                                zoomed_px(h, p->laid_zoom));
        os64_gui_rect_t box = {at.x + gl->dx, at.y + gl->dy, at.w, at.h};
        os64_gui_rect_t content = {c.x + gl->dx, c.y + gl->dy, c.w, c.h};
        if (!os64_rect_intersect(cut, content, &cut))
            return;
        yonder_svg_pixels_t vector_pixels = {0};
        if (p->pics[p->pic_of[i]].kind == YONDER_PICTURE_SVG) {
            if (!svg_picture_pixels(&p->pics[p->pic_of[i]], at, &vector_pixels))
                goto frame;
            px = vector_pixels.pixels;
            w = (uint32_t)vector_pixels.rect.w;
            h = (uint32_t)vector_pixels.rect.h;
        }
        yonder_draw_picture(gl->surf->pixels, gl->surf->pitch_px, cut, box, !b->style->pixelated,
                            px, w, h);
        svg_picture_pixels_free(&vector_pixels);
        return;
    }
frame:
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

// A control whose widget is showing draws itself; one hidden (out of the
// view, or covered), or with no widget, is drawn inert: a white well with a
// sunken edge.
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

// A stand-in (paint.h's stand_in), and along its top what the box would
// hold, cut to the box: a frame's document ("Frame: https://...") on a
// pale panel, or that a canvas cannot be drawn ("Canvas: not supported")
// inside a 1px outline with NO panel, since a canvas is transparent until
// drawn and full-viewport ones (confetti, snow, a cursor's trail) lie over
// the page, which must show through. The label's run is made within one
// paint and let go at its end, so no tree's replacement can leave one
// behind: a frame's for each frame drawn, the canvas one, being the same
// text, once a paint (GlassPane.canvas_label), and none for a box whose top
// strip is out of view. It is made once for every target, so all draw it
// or none does; NULL, the panel or the outline alone.
#define STAND_IN_LABEL_PX 13
#define STAND_IN_LABEL_MAX 512
typedef struct {
    os64_text_run_t *run;
    int32_t pad, ascent;        // pixels
} StandInLabel;

// The labels stand_in_label has laid out, for the bound cases.
static uint64_t s_stand_in_labels;

// What a stand-in's label says, into `text`.
static void stand_in_text(const flow_box_t *b, char *text, size_t cap)
{
    const os64_page_link_t *l = os64_page_link(flow_model(g.page.tree), b->link);
    if (b->node != NULL && b->node->tag == OS64_HTML_TAG_CANVAS)
        os64_strcopy(text, cap, "Canvas: not supported");
    else if (l != NULL && l->href.url != NULL)
        os64_snprintf(text, cap, "Frame: %s", l->href.url);
    else
        os64_strcopy(text, cap, "Frame (it names no document)");
}

static StandInLabel stand_in_label(const flow_box_t *b)
{
    s_stand_in_labels++;
    StandInLabel fl = {NULL, 0, 0};
    char text[STAND_IN_LABEL_MAX];
    stand_in_text(b, text, sizeof(text));
    const flow_family_list_t sans = {.generic = FLOW_GENERIC_SANS};
    os64_text_font_t *const *fonts;
    size_t nfonts;
    os64_font_face_info_t face;
    uint32_t px = (uint32_t)((int64_t)STAND_IN_LABEL_PX * (g.zoom != 0 ? g.zoom : 1000) / 1000);
    if (s_env.text == NULL || page_fonts(NULL, &sans, false, false, px, &fonts, &nfonts, &face) != OS64_FONT_OK)
        return fl;
    os64_text_layout_t opt = {.encoding = OS64_TEXT_UTF8_WESTERN_V1, .fonts = fonts,
                              .font_count = nfonts, .tab_interval = 64 * 8};
    if (os64_text_layout(s_env.text, (const uint8_t *)text, os64_strlen(text), &opt, &fl.run) !=
        OS64_FONT_OK)
        fl.run = NULL;
    fl.pad = (int32_t)(px / 2);
    fl.ascent = (int32_t)((face.ascent + 63) / 64);
    return fl;
}

static void glass_stand_in(const Glass *gl, os64_gui_rect_t c, os64_gui_rect_t clip, uint32_t ink,
                           uint32_t paper, bool panel, const StandInLabel *fl)
{
    Glass cut = *gl;
    os64_gui_rect_t room;
    if (!os64_rect_intersect(c, clip, &room))
        return;
    cut.clip = on_glass(&cut, room);
    if (cut.clip.w <= 0 || cut.clip.h <= 0)
        return;
    if (panel) {
        glass_fill(&cut, c, paper);
    } else {
        glass_fill(&cut, (os64_gui_rect_t){c.x, c.y, c.w, 1}, ink);
        glass_fill(&cut, (os64_gui_rect_t){c.x, c.y + c.h - 1, c.w, 1}, ink);
        glass_fill(&cut, (os64_gui_rect_t){c.x, c.y, 1, c.h}, ink);
        glass_fill(&cut, (os64_gui_rect_t){c.x + c.w - 1, c.y, 1, c.h}, ink);
    }
    if (fl->run == NULL)
        return;
    int32_t baseline = c.y + fl->pad + fl->ascent;
    os64_text_draw_alpha(fl->run, cut.surf, cut.clip, c.x + fl->pad + cut.dx, baseline + cut.dy,
                         0xff000000u | ink, flow_alpha(ink));
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

// The verbs a page is painted through: each drawn into every target being
// painted — the window, or, inside a group laid through its matrix, that
// group's two SHEETS, the same verb on black and on white, which warp.c
// lays onto the targets outside at the close (PILE3.md § Transforms, the
// painter). Each target keeps its own opacity groups. A group laid so is
// at most SHEET_SIDE a side and SHEET_PIXELS in all, at most
// GLASS_THROUGHS nest, and it lies within half of what a 32-bit
// coordinate can say; one past those, or that finds no memory, is
// refused, and the painter draws it upright.
#define GLASS_THROUGHS 16
#define SHEET_SIDE 4096
#define SHEET_PIXELS 2097152

typedef struct {
    Glass sheet[2];
    GlassGroups groups[2];
    os64_gui_surface_t surf[2];
    Glass *was[2];
    int32_t was_n;
    double m[6];                // the sheets' page coordinates -> those painted before
    os64_gui_rect_t clip;       // the coordinates painted before
    os64_gui_rect_t bounds;     // the sheets' page coordinates
} GlassThrough;

// A TEXT CLIP (PILE3.md § The text clip): a mask the clip's text is drawn
// into, white on black, then a group of two sheets as a laid group has,
// laid at the close over the targets outside at each pixel's coverage
// (yonder_warp_masked). One is open at a time — nothing inside a clip
// opens another — and it is bounded as a laid group's sheets are.
typedef struct {
    Glass sheet[3];             // the mask, then the group on black and on white
    GlassGroups groups[3];
    os64_gui_surface_t surf[3];
    Glass *was[2];
    int32_t was_n;
    os64_gui_rect_t bounds;     // page coordinates
} GlassClip;

typedef struct {
    Glass *now[2];
    int32_t n;
    GlassThrough *open[GLASS_THROUGHS];
    int32_t depth;
    GlassClip *clip;
    // The canvas stand-in's label, the same text for every canvas, laid
    // out at the first this paint draws and let go when the paint ends.
    StandInLabel canvas_label;
    bool canvas_label_made;
} GlassPane;

static void pane_fill(void *ctx, os64_gui_rect_t r, uint32_t colour)
{
    GlassPane *gp = ctx;
    for (int32_t k = 0; k < gp->n; k++)
        glass_fill(gp->now[k], r, colour);
}

static void pane_text(void *ctx, const flow_box_t *b, int32_t x, int32_t baseline,
                      os64_gui_rect_t clip, uint32_t colour)
{
    GlassPane *gp = ctx;
    for (int32_t k = 0; k < gp->n; k++)
        glass_text(gp->now[k], b, x, baseline, clip, colour);
}

static void pane_image(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    GlassPane *gp = ctx;
    if (flow_svg_root(b->node)) {
        os64_geometry_memory_t memory = {&g.page, svg_allocate, svg_free};
        yonder_svg_pixels_t pixels;
        svg_status_t status =
            yonder_svg_render(page_doc(&g.page), g.page.cascade, b, c, clip, g.page.laid_zoom,
                              s_env.ink ^ 0xff000000u, &memory, os64_freetype_geometry_v1(),
                              svg_window_cache(), &pixels);
        if (status == SVG_OK) {
            svg_partial_diagnostic(&pixels, false);
            if (pixels.pixels)
                for (int32_t k = 0; k < gp->n; ++k)
                    glass_pixels(gp->now[k], pixels.rect, pixels.pixels);
            yonder_svg_pixels_free(&memory, &pixels);
        } else {
            yonder_diag_failed_seen(
                g.page.diag, "svg",
                status == SVG_UNSUPPORTED ? "inline SVG uses an unsupported feature"
                : status == SVG_MALFORMED ? "inline SVG has malformed viewport or transform data"
                                          : "inline SVG rendering failed or exceeded its limits");
        }
        return;
    }
    for (int32_t k = 0; k < gp->n; k++)
        glass_image(gp->now[k], b, c, clip);
}

static void pane_control(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    GlassPane *gp = ctx;
    for (int32_t k = 0; k < gp->n; k++)
        glass_control(gp->now[k], b, c, clip);
}

// Asked with no area, it draws nothing, so one target answers.
static bool pane_backdrop(void *ctx, const flow_box_t *b, int32_t layer,
                          const os64_gui_rect_t *area, os64_gui_rect_t origin, int32_t ox,
                          int32_t oy, os64_gui_rect_t clip)
{
    GlassPane *gp = ctx;
    bool any = false;
    for (int32_t k = 0; k < (area == NULL ? 1 : gp->n); k++)
        any = glass_backdrop(gp->now[k], b, layer, area, origin, ox, oy, clip) || any;
    return any;
}

// On a laid group's two sheets an opacity group opens on both or on
// neither: one kept and one painted opaque would leave the sheets saying
// different things, and the warp's B and T would be wrong.
static void pane_group_open(void *ctx, os64_gui_rect_t bounds)
{
    GlassPane *gp = ctx;
    for (int32_t k = 0; k < gp->n; k++)
        glass_group_open(gp->now[k], bounds);
    if (gp->n != 2)
        return;
    GlassGroups *a = gp->now[0]->groups, *b = gp->now[1]->groups;
    int32_t d = a->depth - 1;
    if (d < 0 || d >= GLASS_GROUPS || (a->open[d].under == NULL) == (b->open[d].under == NULL))
        return;
    for (GlassGroups *g = a; g != NULL; g = g == a ? b : NULL) {
        glass_give(g->open[d].under);
        g->open[d].under = NULL;
    }
}

static void pane_group_close(void *ctx, uint8_t alpha)
{
    GlassPane *gp = ctx;
    for (int32_t k = 0; k < gp->n; k++)
        glass_group_close(gp->now[k], alpha);
}

// What a verb allocates is allocated once, here, for every target: two
// sheets that disagreed — one drawn, one not, for want of memory — would
// give the warp the wrong B and T.
static void pane_mask(void *ctx, os64_gui_rect_t r, const uint8_t *alpha, uint32_t colour)
{
    GlassPane *gp = ctx;
    if (r.w <= 0 || r.h <= 0)
        return;
    uint32_t *row = os64_malloc((size_t)r.w * sizeof(*row));
    if (row == NULL)
        return;
    for (int32_t k = 0; k < gp->n; k++)
        glass_mask(gp->now[k], r, alpha, colour, row);
    os64_free(row);
}

static void pane_pixels(void *ctx, os64_gui_rect_t r, const uint32_t *argb)
{
    GlassPane *gp = ctx;
    for (int32_t k = 0; k < gp->n; k++)
        glass_pixels(gp->now[k], r, argb);
}

static void pane_stand_in(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip,
                          uint32_t ink, uint32_t paper)
{
    GlassPane *gp = ctx;
    // A box with nothing in view (an empty canvas, one scrolled off) is not
    // drawn: a page can have as many of those as it likes.
    os64_gui_rect_t room;
    if (!os64_rect_intersect(c, clip, &room) || room.w <= 0 || room.h <= 0)
        return;
    bool canvas = b->node != NULL && b->node->tag == OS64_HTML_TAG_CANVAS;
    // The label is drawn in the box's top strip, two of its pixel sizes
    // tall: a box whose strip is out of view needs none laid out.
    int32_t strip = 2 * (int32_t)((int64_t)STAND_IN_LABEL_PX * (g.zoom != 0 ? g.zoom : 1000) / 1000);
    os64_gui_rect_t top = {c.x, c.y, c.w, strip < c.h ? strip : c.h}, seen;
    StandInLabel own = {NULL, 0, 0};
    const StandInLabel *fl = &own;
    if (os64_rect_intersect(top, clip, &seen) && seen.w > 0 && seen.h > 0) {
        if (!canvas) {
            own = stand_in_label(b);
        } else {
            if (!gp->canvas_label_made) {
                gp->canvas_label = stand_in_label(b);
                gp->canvas_label_made = true;
            }
            fl = &gp->canvas_label;
        }
    }
    for (int32_t k = 0; k < gp->n; k++)
        glass_stand_in(gp->now[k], c, clip, ink, paper, !canvas, fl);
    if (own.run != NULL)
        os64_text_run_release(own.run);
}

static void through_free(GlassThrough *t)
{
    for (int k = 0; k < 2; k++)
        glass_give(t->surf[k].pixels);
    os64_free(t);
}

// The two sheets of a group laid through its matrix, `bounds` of its own
// coordinates, black and white, made the targets until its close.
static bool pane_through_open(void *ctx, os64_gui_rect_t bounds, const double m[6],
                              os64_gui_rect_t clip)
{
    GlassPane *gp = ctx;
    // A sheet's offset is its origin negated, and every verb adds it to a
    // coordinate in 32 bits: a sheet lying past half of what a coordinate
    // can say is refused, so neither can overflow, and drawn upright.
    const int64_t half = INT32_MAX / 2;
    if (gp->depth >= GLASS_THROUGHS || bounds.w <= 0 || bounds.h <= 0 || bounds.w > SHEET_SIDE ||
        bounds.h > SHEET_SIDE || (int64_t)bounds.w * bounds.h > SHEET_PIXELS ||
        bounds.x < -half || bounds.y < -half || (int64_t)bounds.x + bounds.w > half ||
        (int64_t)bounds.y + bounds.h > half)
        return false;
    GlassThrough *t = os64_malloc(sizeof(*t));
    if (t == NULL)
        return false;
    os64_memset(t, 0, sizeof(*t));
    size_t n = (size_t)bounds.w * (size_t)bounds.h;
    for (int k = 0; k < 2; k++) {
        t->surf[k].pixels = glass_take(n * sizeof(uint32_t));
        if (t->surf[k].pixels == NULL) {
            through_free(t);
            return false;
        }
        t->surf[k].width = t->surf[k].pitch_px = (uint32_t)bounds.w;
        t->surf[k].height = (uint32_t)bounds.h;
        uint32_t paper = k == 0 ? 0xff000000u : 0xffffffffu;
        for (size_t i = 0; i < n; i++)
            t->surf[k].pixels[i] = paper;
        t->sheet[k] = (Glass){&t->surf[k], -bounds.x, -bounds.y,
                              (os64_gui_rect_t){0, 0, bounds.w, bounds.h}, &t->groups[k]};
    }
    os64_memcpy(t->m, m, sizeof(t->m));
    t->clip = clip;
    t->bounds = bounds;
    t->was_n = gp->n;
    for (int32_t k = 0; k < gp->n; k++)
        t->was[k] = gp->now[k];
    gp->open[gp->depth++] = t;
    gp->now[0] = &t->sheet[0];
    gp->now[1] = &t->sheet[1];
    gp->n = 2;
    return true;
}

// The sheets laid onto each target outside through the matrix — a sheet
// pixel (i, j) is the group's (i + bounds.x, j + bounds.y), the matrix
// takes that to the page coordinates painted before, and the target's
// offset onto its pixels — inside the clip and what the target paints.
static void pane_through_close(void *ctx, uint8_t alpha)
{
    GlassPane *gp = ctx;
    if (gp->depth <= 0)
        return;
    GlassThrough *t = gp->open[--gp->depth];
    gp->n = t->was_n;
    for (int32_t k = 0; k < t->was_n; k++)
        gp->now[k] = t->was[k];
    yonder_sheets_t s = {t->surf[0].pixels, t->surf[1].pixels, t->bounds.w, t->bounds.h};
    for (int32_t k = 0; k < gp->n; k++) {
        const Glass *on = gp->now[k];
        double lay[6];
        yonder_warp_rebase(t->m, t->bounds.x, t->bounds.y, on->dx, on->dy, lay);
        os64_gui_rect_t cut = on_glass(on, t->clip);
        double back[6];
        if (cut.w > 0 && cut.h > 0 && flow_affine_invert(lay, back))
            (void)yonder_warp(on->surf->pixels, on->surf->pitch_px, cut, &s, lay, back, alpha);
    }
    through_free(t);
}

static void clip_free(GlassClip *c)
{
    for (int k = 0; k < 3; k++)
        glass_give(c->surf[k].pixels);
    os64_free(c);
}

// The mask and the group's two sheets, `bounds` of the coordinates being
// painted, black, black and white; the mask the target until clip_fill.
static bool pane_clip_open(void *ctx, os64_gui_rect_t bounds)
{
    GlassPane *gp = ctx;
    // Bounded as a laid group's sheets, for the same reasons
    // (pane_through_open).
    const int64_t half = INT32_MAX / 2;
    if (gp->clip != NULL || bounds.w <= 0 || bounds.h <= 0 || bounds.w > SHEET_SIDE ||
        bounds.h > SHEET_SIDE || (int64_t)bounds.w * bounds.h > SHEET_PIXELS ||
        bounds.x < -half || bounds.y < -half || (int64_t)bounds.x + bounds.w > half ||
        (int64_t)bounds.y + bounds.h > half)
        return false;
    GlassClip *c = os64_malloc(sizeof(*c));
    if (c == NULL)
        return false;
    os64_memset(c, 0, sizeof(*c));
    size_t n = (size_t)bounds.w * (size_t)bounds.h;
    for (int k = 0; k < 3; k++) {
        c->surf[k].pixels = glass_take(n * sizeof(uint32_t));
        if (c->surf[k].pixels == NULL) {
            clip_free(c);
            return false;
        }
        c->surf[k].width = c->surf[k].pitch_px = (uint32_t)bounds.w;
        c->surf[k].height = (uint32_t)bounds.h;
        uint32_t paper = k < 2 ? 0xff000000u : 0xffffffffu;
        for (size_t i = 0; i < n; i++)
            c->surf[k].pixels[i] = paper;
        c->sheet[k] = (Glass){&c->surf[k], -bounds.x, -bounds.y,
                              (os64_gui_rect_t){0, 0, bounds.w, bounds.h}, &c->groups[k]};
    }
    c->bounds = bounds;
    c->was_n = gp->n;
    for (int32_t k = 0; k < gp->n; k++)
        c->was[k] = gp->now[k];
    gp->clip = c;
    gp->now[0] = &c->sheet[0];
    gp->n = 1;
    return true;
}

static void pane_clip_fill(void *ctx)
{
    GlassPane *gp = ctx;
    if (gp->clip == NULL)
        return;
    gp->now[0] = &gp->clip->sheet[1];
    gp->now[1] = &gp->clip->sheet[2];
    gp->n = 2;
}

// The group laid onto each target outside at the mask's coverage — a sheet
// pixel (i, j) is the page's (i + bounds.x, j + bounds.y), which the
// target's offset takes onto its pixels — inside what the target paints.
static void pane_clip_close(void *ctx)
{
    GlassPane *gp = ctx;
    GlassClip *c = gp->clip;
    if (c == NULL)
        return;
    gp->clip = NULL;
    gp->n = c->was_n;
    for (int32_t k = 0; k < c->was_n; k++)
        gp->now[k] = c->was[k];
    yonder_sheets_t s = {c->surf[1].pixels, c->surf[2].pixels, c->bounds.w, c->bounds.h};
    for (int32_t k = 0; k < gp->n; k++) {
        const Glass *on = gp->now[k];
        os64_gui_rect_t cut = on_glass(on, c->bounds);
        if (cut.w > 0 && cut.h > 0)
            (void)yonder_warp_masked(on->surf->pixels, on->surf->pitch_px, cut, &s,
                                     c->surf[0].pixels, c->bounds.x + on->dx,
                                     c->bounds.y + on->dy);
    }
    clip_free(c);
}

// Only the part of the view that is dirty is painted: the kernel takes
// exactly that rectangle when libui publishes, so nothing outside it is
// seen, and a moving picture costs its own box rather than the page.
// The view's verbs, each timed as its stage (s_cost).
static void timed_text(void *ctx, const flow_box_t *b, int32_t x, int32_t baseline, os64_gui_rect_t clip,
                       uint32_t colour)
{
    cost_begin(PS_TEXT);
    pane_text(ctx, b, x, baseline, clip, colour);
    cost_end(PS_TEXT);
}

static void timed_image(void *ctx, const flow_box_t *b, os64_gui_rect_t c, os64_gui_rect_t clip)
{
    cost_begin(PS_PICTURES);
    pane_image(ctx, b, c, clip);
    cost_end(PS_PICTURES);
}

static bool timed_backdrop(void *ctx, const flow_box_t *b, int32_t layer, const os64_gui_rect_t *area,
                           os64_gui_rect_t origin, int32_t ox, int32_t oy, os64_gui_rect_t clip)
{
    cost_begin(PS_BACKGROUNDS);
    bool any = pane_backdrop(ctx, b, layer, area, origin, ox, oy, clip);
    cost_end(PS_BACKGROUNDS);
    return any;
}

static void timed_group_open(void *ctx, os64_gui_rect_t bounds)
{
    cost_begin(PS_GROUPS);
    pane_group_open(ctx, bounds);
    cost_end(PS_GROUPS);
}

static void timed_group_close(void *ctx, uint8_t alpha)
{
    cost_begin(PS_GROUPS);
    pane_group_close(ctx, alpha);
    cost_end(PS_GROUPS);
}

static bool timed_through_open(void *ctx, os64_gui_rect_t bounds, const double m[6], os64_gui_rect_t clip)
{
    cost_begin(PS_WARPS);
    bool open = pane_through_open(ctx, bounds, m, clip);
    cost_end(PS_WARPS);
    return open;
}

static void timed_through_close(void *ctx, uint8_t alpha)
{
    cost_begin(PS_WARPS);
    pane_through_close(ctx, alpha);
    cost_end(PS_WARPS);
}

static void timed_mask(void *ctx, os64_gui_rect_t rect, const uint8_t *alpha, uint32_t colour)
{
    cost_begin(PS_LAID);
    pane_mask(ctx, rect, alpha, colour);
    cost_end(PS_LAID);
}

static void timed_pixels(void *ctx, os64_gui_rect_t rect, const uint32_t *argb)
{
    cost_begin(PS_LAID);
    pane_pixels(ctx, rect, argb);
    cost_end(PS_LAID);
}

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
    GlassPane pane = {{&gl, NULL}, 1, {NULL}, 0, NULL, {NULL, 0, 0}, false};
    yonder_verbs_t v = {&pane,        pane_fill,     timed_text,       timed_image,
                        pane_control, timed_backdrop, timed_group_open, timed_group_close,
                        timed_mask,   timed_pixels,  pane_stand_in,   timed_through_open,
                        timed_through_close, pane_clip_open, pane_clip_fill, pane_clip_close};
    os64_gui_rect_t view = {part.x - gl.dx, part.y - gl.dy, part.w, part.h};
    cost_begin(PS_VIEW);
    yonder_paint_stage = cost_painter;
    if (g.paint_cache == NULL) {
        uint32_t sw = 0, sh = 0;
        os64_memory_t mem = {0};
        if (os64_gui_screen_info(&sw, &sh) != 0)
            sw = sh = 0;
        if (os64_memory(&mem) != 0)
            mem.total = 0;
        g.paint_cache = yonder_paint_cache_new(yonder_paint_cache_budget(sw, sh, mem.total));
    }
    if (g.paint_scratch == NULL)
        g.paint_scratch = yonder_scratch_new();
    yonder_paint(g.page.tree, view, scroll_now(), PAGE_PAPER, s_dark ? &kDark : NULL, g.paint_cache,
                 g.paint_scratch, &v);
    if (pane.canvas_label.run != NULL)
        os64_text_run_release(pane.canvas_label.run);
    yonder_paint_stage = NULL;
    box_bars(&gl, view);
    yonder_scratch_paint_end(g.paint_scratch);
    cost_end(PS_VIEW);
}

// The keyboard's arrows arrive as VT100 bursts (ESC [ A ...). libui's
// decoder for them is private to libos64, so the view reads the few it
// needs itself.
typedef enum { KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
               KEY_PGUP, KEY_PGDN, KEY_SPACE, KEY_POSITIONING, KEY_RECORD } Key;

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
    return a == ' ' ? KEY_SPACE : a == 'p' ? KEY_POSITIONING : a == 'd' ? KEY_RECORD : KEY_NONE;
}

// `d` writes the shown page's record now ("when asked"), keeping the paint
// counts since the last `d` as a numbered reading with where the page was
// scrolled (diag_paint_reading), and starts them again — so each part of a
// page can be measured on its own: scroll there, `d`, wait, `d`, and the
// next part. With no page files to write, or no record (one is made for
// every page, short of memory), it says which. When the file is not written
// the reading stays in the record and the counts go on, so the next `d`
// writes both, its own reading counted from the same start.
static void diag_asked(void)
{
    if (s_diag.dir[0] == '\0') {
        status_rest("No page files: set diagnostics in yonder.conf to write them.");
        return;
    }
    if (g.page.diag == NULL) {
        status_rest("No record for this page: there was no memory to keep one.");
        return;
    }
    diag_observe(&g.page, "when asked");
    diag_paint_reading(&g.page, ++s_cost_readings);
    if (!diag_write(g.page.diag)) {
        char said[OS64_PATH_MAX + 96];
        os64_snprintf(said, sizeof(said), "Paint reading %u kept, but the page file could not be written in %s; "
                      "the counts go on.", (unsigned)s_cost_readings, s_diag.dir);
        status_rest(said);
        return;
    }
    cost_reset();
    char said[96];
    os64_snprintf(said, sizeof(said), "Paint reading %u written; the counts start again.", (unsigned)s_cost_readings);
    status_rest(said);
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
    status_rest(s_env.static_only
                    ? "Positioning off: the page in document order, floats too. p turns it on."
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
    if (page_rebuild_owed(p))
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

// A page about to be laid out to be shown gets its timeline, if it has
// none; its animations start at that layout. With no memory for one the
// page is laid out without it, and they start at a later layout that has
// one.
static void page_timeline(Page *p)
{
    if (p->timeline == NULL && page_doc(p) != NULL)
        p->timeline = flow_timeline_new(page_doc(p));
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
    mark_whole(WHOLE_WINDOW, true);
}

// The animations' switches (Settings). Reduce motion changes what
// `prefers-reduced-motion` answers, so the cascade is judged again, as for
// dark pages. Run animations off lays the page out with no timeline: its
// elements take the styles their rules give them, and nothing is due. The
// page keeps its timeline, so turned on again its animations are as far
// along as their start times say.
static void motion_use(bool run, bool reduce)
{
    if (run == s_animations && reduce == s_reduce_motion)
        return;
    s_animations = run;
    s_reduce_motion = reduce;
    coming_rejudge();
    (void)relayout(true);
    mark_whole(WHOLE_WINDOW, true);
}

// The modern switch (Settings): the next page made takes it, and the page on
// screen keeps the surface it was made with, which a script runtime cannot
// shed and a page half switched would show.
static void modern_use(bool modern)
{
    g.modern = modern;
}

// Ctrl+L takes the keys to the address field with all of it selected, so
// what is typed next replaces it — every browser's way to a new address,
// wherever focus is in the window. A Ctrl'd letter arrives as its control
// code ('L' & 0x1F). True when the event was one.
static bool address_event(const os64_gui_event_t *ev)
{
    if (ev->type != OS64_GUI_EVENT_KEY_DOWN || !(ev->key.modifiers & OS64_GUI_MOD_CTRL) ||
        (ev->key.modifiers & OS64_GUI_MOD_ALT) || ev->key.ascii != ('L' & 0x1F))
        return false;
    os64_ui_set_focus(&g.ui, &g.field.w);
    os64_ui_textfield_select_all(&g.ui, &g.field);
    return true;
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
// move, a `javascript:` address is the page's own script, anywhere else is
// judged by libway with `ask`. A person pressing it is never asked about
// (WAY_ASK_NEVER); a script's click() is judged as the page's own choice
// (WAY_ASK_GO), and `by` says which of the two it was.
static void follow_link_asked(int32_t link, way_ask_t ask, way_initiator_t by)
{
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_LINK, link, 0, 0};
    os64_page_request_t request;
    os64_page_verdict_t verdict = os64_page_activate(page_model(&g.page), what, &request);
    // A place in this page is a fragment navigation (MODERN_WEB.md § Slice
    // 1b): an entry, the scroll, and the page's popstate, then hashchange
    // when the fragment moved. A
    // name nothing in the page claims is one too, scrolling nowhere: a
    // router that hears hashchange is what such a link is for. With no
    // memory for the entry it is the scroll alone, the address unmoved.
    bool unclaimed = verdict == OS64_PAGE_NOTHING && request.reason == OS64_PAGE_REASON_NO_ANCHOR &&
                     request.has_fragment && request.fragment != NULL;
    if (verdict == OS64_PAGE_FRAGMENT || unclaimed) {
        char url[OS64_FETCH_URL_MAX];
        size_t keep = before_fragment(g.page.way.url);
        const char *name = request.fragment != NULL ? request.fragment : "";
        if (keep + 1 + os64_strlen(name) < sizeof(url)) {
            os64_snprintf(url, sizeof(url), "%.*s#%s", (int)keep, g.page.way.url, name);
            (void)page_fragment(&g.page, url, os64_strcmp(url, g.page.way.url) == 0, true);
        }
        if (verdict == OS64_PAGE_FRAGMENT)
            scroll_to_node(request.anchor);
        else
            fragment_scroll(NULL, name);
        os64_page_request_free(&request);
    } else if (verdict == OS64_PAGE_NAVIGATE && os64_streq(request.scheme, "javascript")) {
        // A `javascript:` link is the page's own code in an address: it
        // runs in the page, by the door an inline script runs through, and
        // nothing is fetched, so libway is not asked about it.
        if (!scripts_live())
            status_rest(g.scripts_on && g.page.scripts != NULL
                            ? "That link runs a script, and this page's scripts have stopped."
                            : "That link runs a script, and scripts are off.");
        else if (!yonder_scripts_address(g.page.scripts, request.url,
                                         request.has_fragment ? request.fragment : NULL))
            say_failed(g.page.diag, "script javascript:", "the link's script could not be queued");
        os64_page_request_free(&request);
    } else if (verdict == OS64_PAGE_NAVIGATE) {
        g.chain = 0;
        request_navigate(&request, NAV_GO, ask, g.page.way.url, by, g.page.diag);
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
    follow_link_asked(link, WAY_ASK_NEVER, WAY_BY_PERSON);
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
            mark_whole(WHOLE_SCROLL, false);
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
        case KEY_RECORD: diag_asked(); return true;
        default: return g.seq != 0;
        }
    default:
        return false;
    }
}

// ── Downloads ───────────────────────────────────────────────────────────
//
// YONDER_HONEY_DO.md § 6. A download is asked for (a Save verb, or a click
// on something that is no page), named in the Save dialog, and fetched by
// a job on the window's pool into a staging file of its own, renamed whole
// at the end (download_job.h). It is the WINDOW's: it goes on across pages,
// and closing yonder cancels it, its run removing what it staged. The downloads
// button at the address row's right end is the meter, and a click on it
// lists them; nothing about a download goes on the status line but a
// refusal to start one.
//
// THE SAVE DIALOG IS MODAL TO THIS WINDOW: libui's dialog is a window of
// its own with no owner lock, so while it is up this window takes no
// press, key, wheel or Settings (window_event), and any of them brings the
// dialog forward instead. The page goes on painting and the downloads on
// running.

#define DOWNLOADS_MAX 32        // rows kept; the oldest finished one makes room
#define DOWNLOADS_AT_ONCE 3     // running at once: the rest wait, and a page keeps its workers

typedef enum { DL_WAITING, DL_RUNNING, DL_DONE, DL_FAILED, DL_CANCELLED } DownloadState;

typedef struct {
    DownloadState state;
    uint32_t serial;            // which download, for a verb picked from its row
    char *url, *referrer;       // os64_malloc'd
    char path[OS64_PATH_MAX];
    bool replace;               // the dialog's consent to replace a file of that name
    os64_work_id_t id;
    // RUNNING: the pool's job, read for the meter, until it is reaped or
    // cancelled (a cancelled job is the pool's to free, so it goes NULL
    // before the cancel).
    yonder_download_job_t *job;
    uint64_t got, total;        // the last the job said; a finished one's size
    // Its recent pace, noted as the bytes arrive, for the list's speed and
    // time left; when it began running and when it finished, for its average.
    yonder_download_pace_t pace;
    uint64_t began, ended;
    char why[160];
} Download;

static Download s_downloads[DOWNLOADS_MAX];     // oldest first
static int32_t s_ndownloads;
static uint32_t s_download_serial;
static uint32_t s_download_chosen;              // the row the row menu acts on
static bool s_download_failed_unseen;           // a failure the list has not shown yet
// The button's class, with the meter painted over its face (head_paint),
// and the stock button paint a face without a picture falls back to. Both
// are taken from the widget os64_ui_button made, never by the library's
// name for the class: an executable that names a library's data gets a copy
// relocation, which the kernel's linker does not perform.
static os64_ui_class_t s_downloads_class;
static void (*s_button_paint)(os64_ui_widget_t *, os64_draw_ctx_t *, const os64_ui_theme_t *);
// The list while it is open: said again when a download changes state, as
// the bytes arrive at most every DOWNLOADS_RELIST_MS, and every
// DOWNLOADS_STALL_MS while one runs, so a stall shows as one rather than as
// the last speed seen. Its address is the list's owner for the menu session.
#define DOWNLOADS_RELIST_MS 500
#define DOWNLOADS_STALL_MS 1000
static bool s_downloads_listed;
static uint64_t s_downloads_listed_at;
// A download just finished: the meter's lane blinks green, on and off
// every DOWNLOAD_FLASH_STEP_MS for DOWNLOAD_FLASH_MS from s_download_flash_from.
// s_download_flash_seen is when it was last stepped: the next step is owed
// from there, not from whenever the clock is asked.
#define DOWNLOAD_FLASH_STEP_MS 250
#define DOWNLOAD_FLASH_MS 1500
static bool s_download_flashing;
static uint64_t s_download_flash_from, s_download_flash_seen;

// The question the Save dialog is asked, one at a time: `url` NULL when
// nothing is asked. The dialog opens between turns (downloads_turn).
static struct {
    char *url, *referrer;       // os64_malloc'd
    char dir[OS64_PATH_MAX];
    char name[YONDER_DOWNLOAD_NAME_MAX];
} s_ask;
static os64_ui_file_dialog_t s_save;
static bool s_save_cleanup;     // a failed open left a window to close

static bool downloadable(const char *url)
{
    return (os64_strlen(url) > 7 && os64_memcmp(url, "http://", 7) == 0) ||
           (os64_strlen(url) > 8 && os64_memcmp(url, "https://", 8) == 0);
}

static bool download_taken(void *ctx, const char *name)
{
    char path[OS64_PATH_MAX];
    os64_dirent_t entry;
    int n = os64_snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, name);
    return n < 0 || (size_t)n >= sizeof(path) || os64_stat(path, &entry) == 0;
}

// Asks where a download of `url` goes, asked from the page at `referrer`
// ("" for none), named from `disposition` (a Content-Disposition line, NULL
// for none) or the address. A question already up is brought forward
// instead: one dialog at a time.
static void download_ask(const char *url, const char *referrer, const char *disposition)
{
    if (s_ask.url != NULL) {
        if (s_save.window > 0)
            (void)os64_gui_window_focus(s_save.window);
        return;
    }
    if (!downloadable(url)) {
        status_rest("Only an http or https address can be saved.");
        return;
    }
    const char *said = yonder_settings_saved_downloads(s_ask.dir, sizeof(s_ask.dir));
    if (!yonder_profile_make_dirs(s_ask.dir)) {
        status_rest("The downloads folder cannot be made; nothing was saved.");
        return;
    }
    if (said != NULL)
        status_rest(said);
    char proposed[YONDER_DOWNLOAD_NAME_MAX];
    yonder_download_name(disposition, url, proposed, sizeof(proposed));
    // A hundred taken: the name as it is, and the dialog asks before
    // replacing it.
    if (!yonder_download_free_name(proposed, download_taken, s_ask.dir, s_ask.name, sizeof(s_ask.name)))
        os64_strcopy(s_ask.name, sizeof(s_ask.name), proposed);
    s_ask.url = copy_text(url);
    s_ask.referrer = copy_text(referrer != NULL ? referrer : "");
    if (s_ask.url == NULL || s_ask.referrer == NULL) {
        os64_free(s_ask.url);
        os64_free(s_ask.referrer);
        s_ask.url = s_ask.referrer = NULL;
        status_rest("Out of memory; nothing was saved.");
    }
}

static int32_t downloads_running(void)
{
    int32_t n = 0;
    for (int32_t i = 0; i < s_ndownloads; i++)
        n += s_downloads[i].state == DL_RUNNING;
    return n;
}

static void downloads_relist(bool now);

static void downloads_follow(void)
{
    os64_ui_set_enabled(&g.ui, &g.downloads, s_ndownloads > 0);
    os64_ui_mark_dirty(&g.ui, &g.downloads);
    downloads_relist(true);
}

static void download_failed(Download *d, const char *why)
{
    d->state = DL_FAILED;
    os64_strcopy(d->why, sizeof(d->why), why);
    s_download_failed_unseen = true;
}

// Hands the pool the waiting downloads, oldest first, while fewer than
// DOWNLOADS_AT_ONCE run.
static void downloads_feed(void)
{
    int32_t running = downloads_running();
    for (int32_t i = 0; i < s_ndownloads && running < DOWNLOADS_AT_ONCE; i++) {
        Download *d = &s_downloads[i];
        if (d->state != DL_WAITING)
            continue;
        yonder_download_job_t *job = g.pool != NULL ? os64_calloc(1, sizeof(*job)) : NULL;
        if (job == NULL) {
            download_failed(d, g.pool == NULL ? "no worker to fetch it" : "out of memory");
            continue;
        }
        job->kind = YONDER_JOB_DOWNLOAD;
        job->agent = g.way.agent;
        job->window = g.win;
        job->bell = BELL_DOWNLOAD;
        job->replace = d->replace;
        if (os64_strcopy(job->url, sizeof(job->url), d->url) >= sizeof(job->url) ||
            os64_strcopy(job->path, sizeof(job->path), d->path) >= sizeof(job->path)) {
            os64_free(job);
            download_failed(d, "its address or its name is too long");
            continue;
        }
        job->hooks.jar = g.way.jar;
        job->hooks.identity = g.way.identity;
        os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), d->referrer);
        way_origin_t from;
        way_origin_of(d->referrer, 0, &from);
        way_metadata_navigation(&job->hooks.metadata, d->referrer[0] != '\0' ? WAY_BY_PERSON : WAY_BY_BROWSER,
                                &from);
        os64_work_t work = {yonder_download_run, yonder_download_release, job, YONDER_DOWNLOAD_RESERVE};
        d->id = os64_work_submit(g.pool, &work);
        if (d->id == 0) {
            os64_free(job);
            download_failed(d, "the workers would not take it");
            continue;
        }
        d->job = job;
        d->state = DL_RUNNING;
        d->began = yonder_now_ms();
        yonder_download_pace_note(&d->pace, d->began, 0);
        running++;
    }
    downloads_follow();
}

static bool download_finished(const Download *d)
{
    return d->state == DL_DONE || d->state == DL_FAILED || d->state == DL_CANCELLED;
}

static void download_forget(int32_t i)
{
    os64_free(s_downloads[i].url);
    os64_free(s_downloads[i].referrer);
    os64_memmove(&s_downloads[i], &s_downloads[i + 1], (size_t)(s_ndownloads - i - 1) * sizeof(s_downloads[0]));
    s_ndownloads--;
}

// A download the dialog said yes to: `url` and `referrer` are taken.
static void download_start(char *url, char *referrer, const char *path, bool replace)
{
    if (s_ndownloads == DOWNLOADS_MAX) {
        for (int32_t i = 0; i < s_ndownloads; i++)
            if (download_finished(&s_downloads[i])) {
                download_forget(i);
                break;
            }
    }
    if (s_ndownloads == DOWNLOADS_MAX) {
        os64_free(url);
        os64_free(referrer);
        status_rest("Too many downloads at once; this one was not started.");
        return;
    }
    Download *d = &s_downloads[s_ndownloads++];
    os64_memset(d, 0, sizeof(*d));
    d->state = DL_WAITING;
    d->serial = ++s_download_serial;
    d->url = url;
    d->referrer = referrer;
    d->replace = replace;
    if (os64_strcopy(d->path, sizeof(d->path), path) >= sizeof(d->path))
        download_failed(d, "its name is too long");
    downloads_feed();
}

// Between turns: the dialog opened when it can be (menus finish retiring
// their windows first), and its answer taken once it has one.
static void downloads_turn(void)
{
    os64_ui_file_dialog_t *dialog = &s_save;
    if (s_ask.url == NULL && !s_save_cleanup)
        return;
    if (dialog->window <= 0 && !s_save_cleanup) {
        if (!os64_ui_dispatcher_can_create(&g.dispatcher) || (g.menus != NULL && os64_ui_menu_active(g.menus)))
            return;
        os64_ui_file_options_t options = {
            .mode = OS64_UI_FILE_SAVE,
            .title = "Save As - yonder",
            .directory = s_ask.dir,
            .filename = s_ask.name,
        };
        if (os64_ui_file_dialog_open(dialog, &g.dispatcher, g.win, &options) == OS64_UI_OK)
            return;
        // A failed open can keep a window when its cleanup is refused.
        s_save_cleanup = true;
        status_rest("The Save dialog would not open; nothing was saved.");
    }
    if (!s_save_cleanup && dialog->result == OS64_UI_FILE_PENDING)
        return;
    bool accepted = !s_save_cleanup && dialog->result == OS64_UI_FILE_ACCEPTED;
    bool replace = dialog->replace_existing;
    char path[OS64_DIRECTORY_PATH];
    os64_strcopy(path, sizeof(path), dialog->path);
    if (os64_ui_file_dialog_close(dialog) != OS64_UI_OK)
        return;                         // asked again next turn
    s_save_cleanup = false;
    // The keys come back here, where they were before the dialog, or the
    // next click on the window would go to giving it them.
    (void)os64_gui_window_focus(g.win);
    char *url = s_ask.url, *referrer = s_ask.referrer;
    s_ask.url = s_ask.referrer = NULL;
    if (accepted && url != NULL) {
        download_start(url, referrer, path, replace);
    } else {
        os64_free(url);
        os64_free(referrer);
    }
}

// A download's job came back from the pool: whole, or why not.
static void download_arrived(os64_work_id_t id, yonder_download_job_t *job)
{
    for (int32_t i = 0; i < s_ndownloads; i++) {
        Download *d = &s_downloads[i];
        if (d->state != DL_RUNNING || d->id != id || d->job != job)
            continue;
        d->job = NULL;
        d->got = job->got;
        d->total = job->total;
        d->ended = yonder_now_ms();
        if (job->whole) {
            d->state = DL_DONE;
            s_download_flashing = true;
            s_download_flash_from = s_download_flash_seen = d->ended;
        } else {
            download_failed(d, job->why[0] != '\0' ? job->why : "it stopped");
        }
        break;
    }
    downloads_feed();
}

static void download_cancel(Download *d)
{
    if (d->state == DL_RUNNING) {
        d->got = __atomic_load_n(&d->job->got, __ATOMIC_RELAXED);
        d->job = NULL;                  // the pool's to free from here
        if (g.pool != NULL)
            os64_work_cancel(g.pool, d->id);
    }
    if (d->state == DL_RUNNING || d->state == DL_WAITING)
        d->state = DL_CANCELLED;
    downloads_feed();
}

// The workers stopped (g.pool is gone), and destroying the pool released
// every job still out: each running download lets go of its job and is
// named as failed, and each still waiting goes through downloads_feed, which
// names it too, since none of them will arrive now.
static void downloads_orphaned(void)
{
    for (int32_t i = 0; i < s_ndownloads; i++) {
        Download *d = &s_downloads[i];
        if (d->state != DL_RUNNING)
            continue;
        d->job = NULL;
        download_failed(d, "the workers stopped");
    }
    downloads_feed();
}

// The bytes so far and how many there will be, over the running downloads
// whose servers said; `unknown` when one running did not say.
static void downloads_progress(uint64_t *got, uint64_t *total, bool *unknown)
{
    *got = *total = 0;
    *unknown = false;
    for (int32_t i = 0; i < s_ndownloads; i++) {
        Download *d = &s_downloads[i];
        if (d->state != DL_RUNNING || d->job == NULL)
            continue;
        d->got = __atomic_load_n(&d->job->got, __ATOMIC_RELAXED);
        d->total = __atomic_load_n(&d->job->total, __ATOMIC_RELAXED);
        *got += d->got;
        if (d->total == 0)
            *unknown = true;
        else
            *total += d->total;
    }
}

// Bytes arrived, or a moment came (downloads_due): each running download's
// pace noted, the meter moved or its flash stepped, and the open list said
// again.
static void downloads_heard(void)
{
    uint64_t got, total, now = yonder_now_ms();
    bool unknown;
    if (s_download_flashing) {
        s_download_flash_seen = now;
        s_download_flashing = now - s_download_flash_from < DOWNLOAD_FLASH_MS;
    }
    downloads_progress(&got, &total, &unknown);
    for (int32_t i = 0; i < s_ndownloads; i++)
        if (s_downloads[i].state == DL_RUNNING)
            yonder_download_pace_note(&s_downloads[i].pace, now, s_downloads[i].got);
    os64_ui_mark_dirty(&g.ui, &g.downloads);
    downloads_relist(false);
}

// When the meter's flash next steps, or the open list is next said again
// though no bytes arrive: YONDER_NEVER when neither is owed.
static uint64_t downloads_due(void)
{
    uint64_t due = s_downloads_listed && downloads_running() > 0 ? s_downloads_listed_at + DOWNLOADS_STALL_MS
                                                                  : YONDER_NEVER;
    if (s_download_flashing) {
        uint64_t steps = (s_download_flash_seen - s_download_flash_from) / DOWNLOAD_FLASH_STEP_MS + 1;
        uint64_t step = s_download_flash_from + steps * DOWNLOAD_FLASH_STEP_MS;
        if (step < due)
            due = step;
    }
    return due;
}

// The flash's lit half: the even steps of its run.
static bool download_flash_lit(void)
{
    uint64_t since = yonder_now_ms() - s_download_flash_from;
    return s_download_flashing && since < DOWNLOAD_FLASH_MS && (since / DOWNLOAD_FLASH_STEP_MS) % 2 == 0;
}

static void head_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t);

// The button, and its meter along its foot: green across, blinking, as a
// download finishes; the share that has arrived while every running
// download's size is known, a band that moves with the bytes while one's is
// not; red across after a failure the list has not shown yet.
static void downloads_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    head_paint(w, ctx, t);
    os64_gui_rect_t b = w->bounds;
    os64_gui_rect_t lane = {b.x + 3, b.y + b.h - 5, b.w - 6, 3};
    if (lane.w <= 0)
        return;
    if (download_flash_lit()) {
        os64_draw_fill_rect(&ctx->surf, lane, 0xff46b03cu);
        return;
    }
    uint64_t got, total;
    bool unknown;
    downloads_progress(&got, &total, &unknown);
    if (downloads_running() > 0) {
        os64_gui_rect_t fill = lane;
        if (unknown || total == 0) {
            // A third of the lane, stepping along a twelfth for each 64 KB.
            int32_t step = lane.w / 12 > 0 ? lane.w / 12 : 1;
            fill.w = lane.w / 3;
            fill.x = lane.x + (int32_t)((got / YONDER_DOWNLOAD_CHUNK) % 9) * step;
        } else {
            fill.w = (int32_t)((uint64_t)lane.w * (got < total ? got : total) / total);
        }
        os64_draw_fill_rect(&ctx->surf, fill, os64_ui_widget_enabled(w) ? t->button_fg : t->disabled_fg);
    } else if (s_download_failed_unseen) {
        os64_draw_fill_rect(&ctx->surf, lane, 0xffc83232u);
    }
}

// A row's name for the file: cut, at a character's start, to leave the
// row's news room inside the 127 bytes the list keeps of a row.
#define DOWNLOAD_ROW_NAME 40

static void download_row_name(const Download *d, char *out, size_t cap)
{
    const char *slash = os64_strrchr(d->path, '/');
    const char *name = slash != NULL ? slash + 1 : d->path;
    size_t n = os64_strlen(name);
    if (n <= DOWNLOAD_ROW_NAME || cap < DOWNLOAD_ROW_NAME + 4) {
        os64_strcopy(out, cap, name);
        return;
    }
    n = DOWNLOAD_ROW_NAME;
    while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80)
        n--;
    os64_memcpy(out, name, n);
    os64_memcpy(out + n, "\xe2\x80\xa6", 4);
}

// One row of the list at `now`: the file's name, and how far it got and how
// fast (the last few seconds' speed and the time that leaves while it runs,
// its average once it is done), or why not.
static void download_row(const Download *d, uint64_t now, char *out, size_t cap)
{
    char name[DOWNLOAD_ROW_NAME + 4], got[32], total[32], speed[32], left[32];
    download_row_name(d, name, sizeof(name));
    yonder_download_size(d->got, got, sizeof(got));
    yonder_download_size(d->total, total, sizeof(total));
    uint64_t rate = 0;
    switch (d->state) {
    case DL_WAITING: os64_snprintf(out, cap, "%s - waiting", name); break;
    case DL_RUNNING:
        if (!yonder_download_pace_rate(&d->pace, now, d->got, &rate))
            speed[0] = '\0';
        else if (rate == 0)
            os64_strcopy(speed, sizeof(speed), ", stalled");
        else {
            char size[32];
            yonder_download_size(rate, size, sizeof(size));
            os64_snprintf(speed, sizeof(speed), ", %s/s", size);
        }
        left[0] = '\0';
        if (d->total > d->got && rate != 0 && yonder_download_left(d->total - d->got, rate, left + 2, sizeof(left) - 2))
            os64_memcpy(left, ", ", 2);
        if (d->total != 0)
            os64_snprintf(out, cap, "%s - %s of %s%s%s", name, got, total, speed, left);
        else
            os64_snprintf(out, cap, "%s - %s%s", name, got, speed);
        break;
    case DL_DONE:
        if (d->ended - d->began >= 1000) {
            yonder_download_size(d->got * 1000 / (d->ended - d->began), speed, sizeof(speed));
            os64_snprintf(out, cap, "%s - %s, done (%s/s average)", name, got, speed);
        } else {
            os64_snprintf(out, cap, "%s - %s, done", name, got);
        }
        break;
    case DL_FAILED:
        if (d->got != 0)
            os64_snprintf(out, cap, "%s - failed at %s: %s", name, got, d->why);
        else
            os64_snprintf(out, cap, "%s - failed: %s", name, d->why);
        break;
    case DL_CANCELLED: os64_snprintf(out, cap, "%s - cancelled", name); break;
    }
}

// The list's rows, newest first, and "Clear finished" at its foot, into
// `labels` and `enabled` (DOWNLOADS_MAX + 1 of each): how many.
static size_t downloads_rows(const char **labels, bool *enabled)
{
    static char rows[DOWNLOADS_MAX + 1][224];
    uint64_t got, total, now = yonder_now_ms();
    bool unknown;
    downloads_progress(&got, &total, &unknown);
    bool finished = false;
    for (int32_t i = 0; i < s_ndownloads; i++) {
        const Download *d = &s_downloads[s_ndownloads - 1 - i];
        download_row(d, now, rows[i], sizeof(rows[i]));
        labels[i] = rows[i];
        enabled[i] = true;
        finished |= download_finished(d);
    }
    labels[s_ndownloads] = "Clear finished";
    enabled[s_ndownloads] = finished;
    return (size_t)s_ndownloads + 1;
}

// The rows went to a list that shows them: a failure among them is seen,
// and the button's red goes. Rows built for a list found shut were not.
static void downloads_seen(void)
{
    if (s_download_failed_unseen) {
        s_download_failed_unseen = false;
        os64_ui_mark_dirty(&g.ui, &g.downloads);
    }
}

// The open list said again (s_downloads_listed): now, or, unless `now`, once
// DOWNLOADS_RELIST_MS has passed since it last was. A list the person shut
// is found shut here, and not said again.
static void downloads_relist(bool now)
{
    uint64_t at = yonder_now_ms();
    if (!s_downloads_listed || (!now && at - s_downloads_listed_at < DOWNLOADS_RELIST_MS))
        return;
    const char *labels[DOWNLOADS_MAX + 1];
    bool enabled[DOWNLOADS_MAX + 1];
    size_t count = downloads_rows(labels, enabled);
    s_downloads_listed = g.menus != NULL &&
                         os64_ui_menu_list_relabel(g.menus, &s_downloads_listed, labels, enabled, count) == OS64_UI_OK;
    s_downloads_listed_at = at;
    if (s_downloads_listed)
        downloads_seen();
}

static void downloads_picked(void *user, size_t row);

// The list, said again while it is open (downloads_relist).
static void click_downloads(os64_ui_widget_t *w, void *user)
{
    (void)user;
    if (g.menus == NULL || s_ndownloads == 0)
        return;
    const char *labels[DOWNLOADS_MAX + 1];
    bool enabled[DOWNLOADS_MAX + 1];
    size_t count = downloads_rows(labels, enabled);
    // A list keeps the width it opened at, so it opens wide enough for the
    // longest a waiting or running row's news grows to: its speed and the
    // time left are not cut when they come. The room around a label is the
    // list's mark and margins, with some to spare.
    int32_t width = w->bounds.w;
    for (int32_t i = 0; i < s_ndownloads; i++) {
        const Download *d = &s_downloads[i];
        if (d->state != DL_WAITING && d->state != DL_RUNNING)
            continue;
        char name[DOWNLOAD_ROW_NAME + 4], widest[224];
        download_row_name(d, name, sizeof(name));
        os64_snprintf(widest, sizeof(widest), "%s - 999.9 MB of 999.9 MB, 999.9 MB/s, 99 h 59 min left", name);
        int32_t px = 0;
        if (os64_ui_text_measure(&g.ui, OS64_FONT_ROLE_UI, widest, os64_strlen(widest), &px) == OS64_FONT_OK &&
            px + 48 > width)
            width = px + 48;
    }
    os64_ui_menu_list_t list = {
        .labels = labels, .enabled = enabled, .count = count,
        .selected = count, .min_width = width, .widget = w,
        .pick = downloads_picked, .user = &s_downloads_listed,
    };
    // BUSY: it opens once a menu just shut has gone, and is said again
    // meanwhile like an open one.
    os64_ui_status_code_t rc = os64_ui_menu_open_list(g.menus, &list, w->bounds);
    s_downloads_listed = rc == OS64_UI_OK || rc == OS64_UI_BUSY;
    s_downloads_listed_at = yonder_now_ms();
    if (s_downloads_listed)
        downloads_seen();
    else
        status_rest("The downloads list would not open.");
}

static Download *download_chosen(void)
{
    for (int32_t i = 0; i < s_ndownloads; i++)
        if (s_downloads[i].serial == s_download_chosen)
            return &s_downloads[i];
    return NULL;
}

// A row: its verbs, in a menu of its own at the button; the foot: the
// finished rows go.
static void downloads_picked(void *user, size_t row)
{
    (void)user;
    if (row >= (size_t)s_ndownloads) {
        for (int32_t i = s_ndownloads - 1; i >= 0; i--)
            if (download_finished(&s_downloads[i]))
                download_forget(i);
        downloads_follow();
        return;
    }
    s_download_chosen = s_downloads[s_ndownloads - 1 - (int32_t)row].serial;
    if (g.action_view == NULL || g.context_menus[CONTEXT_DOWNLOAD] == NULL)
        return;
    (void)os64_ui_actions_refresh(g.action_view);
    os64_ui_status_code_t rc = os64_ui_menu_open(g.menus, g.context_menus[CONTEXT_DOWNLOAD], g.action_view,
                                                 g.downloads.bounds, OS64_UI_MENU_CONTEXT);
    if (rc != OS64_UI_OK && rc != OS64_UI_BUSY)
        status_rest("The menu would not open.");
}

// Roam on the folder a finished download went to.
static void download_show(const Download *d)
{
    char folder[OS64_PATH_MAX];
    os64_strcopy(folder, sizeof(folder), d->path);
    char *slash = os64_strrchr(folder, '/');
    if (slash != NULL)
        slash[slash == folder ? 1 : 0] = '\0';
    char *argv[] = {"/bin/roam", folder, NULL};
    int64_t pid = os64_spawn(argv[0], argv);
    if (pid > 0)
        g.children++;
    else
        status_rest("Roam would not start.");
}

// As yonder leaves, after the pool: its destroy cancelled the running ones.
static void downloads_drop(void)
{
    if (s_save.window > 0 || s_save_cleanup)
        (void)os64_ui_file_dialog_close(&s_save);
    os64_free(s_ask.url);
    os64_free(s_ask.referrer);
    s_ask.url = s_ask.referrer = NULL;
    while (s_ndownloads > 0)
        download_forget(s_ndownloads - 1);
}

// ── The right-click menu ────────────────────────────────────────────────
//
// One popup over the page, opened on the right button (or Shift+F10 while
// the page has focus, at the pointer) over a link, a picture, or a picture
// inside a link; over
// anything else nothing opens. The menu is the WINDOW's: no `contextmenu`
// event reaches the page, so a page cannot keep it shut. What its verbs act
// on is copied as it opens, the addresses and not the nodes, so a page that
// changes or departs under an open menu leaves them meaning what was
// clicked.

// The link's address resolved as the page's links are, with its fragment
// as the page wrote it: the model's fragment is DECODED, for matching an
// anchor, and spelling that back would copy a different address (`%2520`
// would come out `%20`). The attribute is the one the model's link came
// from: `href`, or a frame's `src`.
static bool context_link_address(const os64_html_node_t *node, char *out, size_t cap)
{
    const os64_page_t *model = page_model(&g.page);
    int32_t at = node != NULL && model != NULL ? os64_page_link_for(model, node) : -1;
    const os64_page_link_t *l = at >= 0 ? os64_page_link(model, at) : NULL;
    if (l == NULL || l->href.url == NULL)
        return false;
    const os64_html_attr_t *spelled = os64_html_attr(node, "href");
    if (spelled == NULL)
        spelled = os64_html_attr(node, "src");
    return spelled != NULL && spelled->value != NULL &&
           os64_page_resolve_in(page_doc(&g.page), os64_page_document_url(model), spelled->value, out, cap);
}

// The address the picture was fetched from, `srcset`'s choice included: the
// page's catalog entry the painter draws it from, so a picture not fetched
// (a data: address, one the catalog had no room for) offers no address.
static bool context_picture_address(const os64_html_node_t *node, char *out, size_t cap)
{
    const Page *p = &g.page;
    const os64_page_t *model = page_model(p);
    if (node == NULL || model == NULL || p->pic_of == NULL)
        return false;
    int32_t at = os64_page_image_for(model, node);
    if (at < 0 || at >= p->nimage_map)
        return false;
    int32_t k = p->pic_of[at];
    return k >= 0 && k < p->npics && os64_strcopy(out, cap, p->pics[k].url) < cap;
}

// A `javascript:` address is the page's own code, which a new window has no
// page to run in.
static bool context_openable(const char *address)
{
    static const char kScript[] = "javascript:";
    if (address[0] == '\0')
        return false;
    for (size_t i = 0; i + 1 < sizeof(kScript); i++) {
        char c = address[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != kScript[i])
            return true;
    }
    return false;
}

static void context_query(void *app, const os64_ui_action_context_t *ctx, uint32_t id,
                          os64_ui_action_state_t *out)
{
    (void)app;
    (void)ctx;
    out->checked = false;
    const Download *d = download_chosen();
    switch (id) {
    case ACT_OPEN_LINK: out->enabled = context_openable(g.context_link); break;
    case ACT_SAVE_LINK: out->enabled = downloadable(g.context_link); break;
    case ACT_COPY_LINK: out->enabled = g.context_link[0] != '\0'; break;
    case ACT_OPEN_PICTURE: out->enabled = context_openable(g.context_picture); break;
    case ACT_SAVE_PICTURE: out->enabled = downloadable(g.context_picture); break;
    case ACT_COPY_PICTURE: out->enabled = g.context_picture[0] != '\0'; break;
    case ACT_DOWNLOAD_CANCEL: out->enabled = d != NULL && (d->state == DL_WAITING || d->state == DL_RUNNING); break;
    case ACT_DOWNLOAD_SHOW: out->enabled = d != NULL && d->state == DL_DONE; break;
    default: out->enabled = false; break;
    }
}

// A new window is another yonder, given the address; it shares this one's
// profile files, its cookies among them as the jar last wrote them.
static void context_open(const char *address)
{
    char *argv[] = {"/bin/yonder", (char *)address, NULL};
    int64_t pid = os64_spawn(argv[0], argv);
    if (pid > 0)
        g.children++;
    else if (pid == OS64_SPAWN_TOO_LONG)
        status_rest("That address is too long to hand to a new window.");
    else if (pid <= 0)
        status_rest("A new window would not start.");
}

// A child that has finished is collected at the window's next turn, never
// waited for: the kernel keeps an uncollected child of a living parent, so
// a window opened and closed over and over would otherwise pile up until
// this one exits.
static void children_turn(void)
{
    while (g.children > 0 && os64_reap(NULL) > 0)
        g.children--;
}

static void context_copy(const char *address, const char *said)
{
    size_t n = os64_strlen(address);
    status_rest(os64_clip_copy(address, n) == (int64_t)n ? said : "The clipboard did not take it.");
}

static os64_ui_action_result_t context_invoke(void *app, const os64_ui_action_context_t *ctx, uint32_t id)
{
    (void)app;
    (void)ctx;
    os64_ui_action_state_t state;
    context_query(NULL, NULL, id, &state);
    if (!state.enabled)
        return (os64_ui_action_result_t){OS64_UI_REFUSED, 0};
    switch (id) {
    case ACT_OPEN_LINK: context_open(g.context_link); break;
    case ACT_SAVE_LINK: download_ask(g.context_link, g.page.way.url, NULL); break;
    case ACT_COPY_LINK: context_copy(g.context_link, "Link address copied."); break;
    case ACT_OPEN_PICTURE: context_open(g.context_picture); break;
    case ACT_SAVE_PICTURE: download_ask(g.context_picture, g.page.way.url, NULL); break;
    case ACT_COPY_PICTURE: context_copy(g.context_picture, "Picture address copied."); break;
    case ACT_DOWNLOAD_CANCEL: download_cancel(download_chosen()); break;
    case ACT_DOWNLOAD_SHOW: download_show(download_chosen()); break;
    default: return (os64_ui_action_result_t){OS64_UI_REFUSED, 0};
    }
    return (os64_ui_action_result_t){OS64_UI_OK, 0};
}

// The window outlives every menu it opens, so the context is always valid.
static bool context_retain(void *cookie, uint64_t generation) { (void)cookie; return generation == 1; }
static void context_release(void *cookie, uint64_t generation) { (void)cookie; (void)generation; }
static bool context_valid(void *cookie, uint64_t generation) { (void)cookie; return generation == 1; }

// The registry, its view and the models; false leaves whatever was made
// for context_menus_drop, and no menu opens.
static bool context_menus_make(void)
{
    static const os64_ui_action_spec_t specs[] = {
        {ACT_OPEN_LINK, OS64_UI_ACTION_COMMAND, 0, "Open Link in New Window",
         "Open the link in a window of its own", NULL, 'w', {0, 0}},
        {ACT_SAVE_LINK, OS64_UI_ACTION_COMMAND, 0, "Save Link As...",
         "Save what the link names to a file", NULL, 'k', {0, 0}},
        {ACT_COPY_LINK, OS64_UI_ACTION_COMMAND, 0, "Copy Link Address",
         "Copy the link's address to the clipboard", NULL, 'l', {0, 0}},
        {ACT_OPEN_PICTURE, OS64_UI_ACTION_COMMAND, 0, "Open Picture in New Window",
         "Show the picture in a window of its own", NULL, 'i', {0, 0}},
        {ACT_SAVE_PICTURE, OS64_UI_ACTION_COMMAND, 0, "Save Picture As...",
         "Save the picture to a file", NULL, 'v', {0, 0}},
        {ACT_COPY_PICTURE, OS64_UI_ACTION_COMMAND, 0, "Copy Picture Address",
         "Copy the picture's address to the clipboard", NULL, 'a', {0, 0}},
        {ACT_DOWNLOAD_CANCEL, OS64_UI_ACTION_COMMAND, 0, "Cancel Download",
         "Stop it and keep nothing", NULL, 'c', {0, 0}},
        {ACT_DOWNLOAD_SHOW, OS64_UI_ACTION_COMMAND, 0, "Show in Roam",
         "Open its folder in Roam", NULL, 'r', {0, 0}},
    };
    const os64_ui_action_callbacks_t callbacks = {context_query, context_invoke};
    if (os64_ui_actions_create(specs, sizeof(specs) / sizeof(*specs), &callbacks, NULL, &g.actions) != OS64_UI_OK)
        return false;
    os64_ui_action_context_t ctx = {NULL, 1, context_retain, context_release, context_valid};
    if (os64_ui_action_view_create(g.actions, &ctx, &g.action_view) != OS64_UI_OK)
        return false;
    const os64_ui_menu_entry_t link[] = {{.action = ACT_OPEN_LINK}, {.action = ACT_SAVE_LINK},
                                         {.action = ACT_COPY_LINK}};
    const os64_ui_menu_entry_t picture[] = {{.action = ACT_OPEN_PICTURE}, {.action = ACT_SAVE_PICTURE},
                                            {.action = ACT_COPY_PICTURE}};
    const os64_ui_menu_entry_t both[] = {{.action = ACT_OPEN_LINK}, {.action = ACT_SAVE_LINK},
                                         {.action = ACT_COPY_LINK}, {.kind = OS64_UI_MENU_SEPARATOR},
                                         {.action = ACT_OPEN_PICTURE}, {.action = ACT_SAVE_PICTURE},
                                         {.action = ACT_COPY_PICTURE}};
    const os64_ui_menu_entry_t download[] = {{.action = ACT_DOWNLOAD_CANCEL}, {.action = ACT_DOWNLOAD_SHOW}};
    return os64_ui_menu_model_create(g.actions, link, 3, &g.context_menus[CONTEXT_LINK]) == OS64_UI_OK &&
           os64_ui_menu_model_create(g.actions, picture, 3, &g.context_menus[CONTEXT_PICTURE]) == OS64_UI_OK &&
           os64_ui_menu_model_create(g.actions, both, 7, &g.context_menus[CONTEXT_BOTH]) == OS64_UI_OK &&
           os64_ui_menu_model_create(g.actions, download, 2, &g.context_menus[CONTEXT_DOWNLOAD]) == OS64_UI_OK;
}

// After the popup controller is disposed of, which borrows the view.
static void context_menus_drop(void)
{
    for (size_t i = 0; i < CONTEXT_MENUS; i++) {
        os64_ui_menu_model_release(g.context_menus[i]);
        g.context_menus[i] = NULL;
    }
    if (g.action_view != NULL)
        (void)os64_ui_action_view_destroy(g.action_view);
    g.action_view = NULL;
    if (g.actions != NULL)
        (void)os64_ui_actions_destroy(g.actions);
    g.actions = NULL;
}

static bool view_context(os64_ui_widget_t *w, const os64_gui_event_t *ev, void *user)
{
    (void)w;
    (void)user;
    if (g.menus == NULL || g.context_menus[CONTEXT_MENUS - 1] == NULL)   // the last made: all were
        return false;
    bool keyboard = ev->type == OS64_GUI_EVENT_KEY_DOWN;
    int32_t x = keyboard ? g.pointer_x : ev->mouse.x, y = keyboard ? g.pointer_y : ev->mouse.y;
    // The picture is the hit box's element, not the answer: over an image
    // map the answer is the region, and the picture under it is still one.
    const os64_html_node_t *link = NULL, *boxed = NULL;
    (void)element_hit(x, y, &link, &boxed);
    bool has_link = context_link_address(link, g.context_link, sizeof(g.context_link));
    bool has_picture = context_picture_address(boxed, g.context_picture, sizeof(g.context_picture));
    if (!has_link)
        g.context_link[0] = '\0';
    if (!has_picture)
        g.context_picture[0] = '\0';
    if (!has_link && !has_picture)
        return false;
    os64_ui_menu_model_t *model = g.context_menus[has_link && has_picture ? CONTEXT_BOTH
                                                  : has_link              ? CONTEXT_LINK
                                                                          : CONTEXT_PICTURE];
    (void)os64_ui_actions_refresh(g.action_view);
    os64_ui_status_code_t rc = os64_ui_menu_open(g.menus, model, g.action_view,
                                                 (os64_gui_rect_t){x, y, 0, 0}, OS64_UI_MENU_CONTEXT);
    if (rc != OS64_UI_OK && rc != OS64_UI_BUSY)
        status_rest("The menu would not open.");
    return true;
}

static const os64_ui_class_t kViewClass = {
    .name = "pageview",
    .paint = view_paint,
    .event = view_event,
    .paints_part = true,   // view_paint paints what g.ui.dirty covers of it
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

// Back and Forward: within the page's document when the entry is its own,
// else a fetch (session_go).
static void click_back(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    session_go(-1, WAY_BY_BROWSER, NULL);
}

static void click_forward(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    session_go(1, WAY_BY_BROWSER, NULL);
}

// The page on screen again, asked for by `by`: the browser's own Reload,
// which replays how the entry's navigation was asked and is a reload
// (Chrome 156 and Firefox 158 both send the entry's own Sec-Fetch-Site
// and -User again: capture-26-chrome-win-get-clicked and
// capture-43-firefox-linux-get-clicked), or the page's location.reload(),
// which starts from the page's own origin and is nobody's.
static void reload_as(way_initiator_t by)
{
    if (g.page.tree == NULL)
        return;
    g.chain = 0;
    way_metadata_t replay = g.page.asked;
    if (replay.dest != WAY_DEST_DOCUMENT)
        way_metadata_navigation(&replay, WAY_BY_BROWSER, NULL);    // kept nothing (a file)
    replay.reload = true;
    const way_metadata_t *replays = by == WAY_BY_BROWSER ? &replay : NULL;
    if (g.page.sent.url != NULL) {
        resend_form(by, replays);
        return;
    }
    char url[OS64_FETCH_URL_MAX];
    os64_strcopy(url, sizeof(url), g.page.way.url);
    // The address goes as the page it starts from; a Reload's Referer
    // names nothing all the same (trip_send).
    g.replaying = replays;
    start_trip(url, NULL, url, NAV_RELOAD, by, NULL);
    g.replaying = NULL;
    // A reload is the same entry again, its state with it.
    stream_entry(g.page.entry, g.page.entry_state, false);
}

static void click_reload(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    reload_as(WAY_BY_BROWSER);
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
    // The page on screen keeps its scripts while a navigation is only
    // asked for (they go when the next page's parse begins: page_departs),
    // and never across a change of the mode its document was parsed in.
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

// ── The head row's pictures ─────────────────────────────────────────────
//
// Back, Forward, Reload, Stop and Downloads wear the icon set's pictures
// beside their labels (icons.conf on the config ladder, the shipped set
// beneath it), at the size the row's height holds, so the row is no taller
// for them. A button whose picture is not there (no theme, a set without
// that icon, a lease refused) is the plain captioned button.

typedef struct {
    os64_ui_widget_t *w;
    const char *key;
    os64_ui_icon_lease_t icon;          // pixels NULL: none
} HeadFace;

static HeadFace s_head_faces[] = {
    {&g.back, "go-back", {0}},       {&g.forward, "go-forward", {0}}, {&g.reload, "view-refresh", {0}},
    {&g.stop, "window-close", {0}},  {&g.downloads, "download", {0}},
};
static os64_icon_theme_t *s_icons;
static uint32_t s_face_size;            // the size the pictures were taken at; 0 before
static os64_ui_class_t s_head_class;    // the button's, painted with its picture

// The pictures' size for a row `bh` high: 24 where it leaves the face a
// margin, else 16.
static uint32_t head_icon_size(int32_t bh)
{
    return bh >= 32 ? 24 : 16;
}

static HeadFace *head_face(const os64_ui_widget_t *w)
{
    for (size_t i = 0; i < sizeof(s_head_faces) / sizeof(s_head_faces[0]); i++)
        if (s_head_faces[i].w == w)
            return &s_head_faces[i];
    return NULL;
}

// The pictures taken at the size a row `bh` high holds, when it is not the
// size they have; one not to be had at it keeps the picture it has.
static void head_faces_take(int32_t bh)
{
    uint32_t size = head_icon_size(bh);
    if (s_icons == NULL || size == s_face_size)
        return;
    const os64_ui_icon_provider_t *provider = os64_icon_theme_provider(s_icons);
    for (size_t i = 0; i < sizeof(s_head_faces) / sizeof(s_head_faces[0]); i++) {
        os64_ui_icon_lease_t lease;
        if (os64_ui_icon_acquire(provider, s_head_faces[i].key, size, &lease) != OS64_UI_ICON_OK)
            continue;
        os64_ui_icon_release(provider, &s_head_faces[i].icon);
        s_head_faces[i].icon = lease;
        os64_ui_mark_dirty(&g.ui, s_head_faces[i].w);
    }
    s_face_size = size;
}

// As yonder leaves: the pictures go back before the theme that lent them.
static void head_faces_drop(void)
{
    if (s_icons == NULL)
        return;
    const os64_ui_icon_provider_t *provider = os64_icon_theme_provider(s_icons);
    for (size_t i = 0; i < sizeof(s_head_faces) / sizeof(s_head_faces[0]); i++)
        os64_ui_icon_release(provider, &s_head_faces[i].icon);
    if (os64_icon_theme_destroy(s_icons) == OS64_UI_OK)
        s_icons = NULL;
}

// The width a head button takes in a row `bh` high: its caption's, and its
// picture's beside it.
static int32_t head_width(os64_ui_widget_t *w, int32_t bh)
{
    const HeadFace *f = head_face(w);
    int32_t width = button_width(w->text);
    if (f != NULL && f->icon.pixels != NULL)
        width += (int32_t)head_icon_size(bh) + g.ui.theme.gap;
    return width;
}

// The button's face, its picture at the left and its caption centred in
// what is left; the stock paint when it has no picture. The caption is the
// run the button stages, measured and drawn as the stock paint does.
static void head_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    const HeadFace *f = head_face(w);
    if (f == NULL || f->icon.pixels == NULL) {
        s_button_paint(w, ctx, t);
        return;
    }
    os64_ui_button_face(w, ctx, t);
    os64_gui_rect_t b = w->bounds;
    os64_gui_rect_t slot = {b.x + 2 * t->pad, b.y, (int32_t)s_face_size, b.h};
    os64_ui_icon_draw(&ctx->surf, slot, &f->icon, !os64_ui_widget_enabled(w));
    os64_ui_t *ui = os64_ui_of(w);
    const char *text = w->text != NULL ? w->text : "";
    size_t len = os64_strlen(text);
    int32_t tw = 0;
    if (os64_ui_run_width(ui, &w->run, OS64_FONT_ROLE_UI, text, len, &tw) != OS64_FONT_OK)
        return;
    int32_t left = slot.x + slot.w + t->gap, right = b.x + b.w - 2 * t->pad;
    int32_t tx = left + (right - left - tw) / 2;
    int32_t ty = b.y + (b.h - os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI)) / 2;
    uint32_t face = w->pressed ? t->button_face_pressed : w->hovered ? t->button_face_hover : t->button_face;
    os64_ui_draw_run(ui, w->run, OS64_FONT_ROLE_UI, &ctx->surf, b, tx, ty, text, len, t->button_fg, face);
}

// Font adoption measures candidate metrics before replacing the live face.
// Stage the same geometry used for ordinary resizes so a refused adoption
// leaves the current window intact.
static void layout_place(os64_ui_widget_t *w, os64_gui_rect_t r, bool staged)
{
    if (staged)
        os64_ui_widget_stage_bounds(w, r);
    else
        w->bounds = r;
}

static os64_font_status_t layout_window(bool staged)
{
    const os64_ui_theme_t *t = &g.ui.theme;
    os64_gui_rect_t area = os64_ui_widget_planned_bounds(&g.root);
    int32_t pad = t->pad, gap = t->gap, bh = os64_ui_control_min_height(&g.ui), sw = t->scroll_w;
    int32_t x = pad;
    if (!staged)
        head_faces_take(bh);
    os64_ui_widget_t *tools[] = {&g.back, &g.forward, &g.reload, &g.stop};
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        int32_t bw = head_width(tools[i], bh);
        layout_place(tools[i], (os64_gui_rect_t){x, pad, bw, bh}, staged);
        x += bw + gap;
    }
    // The downloads button takes the row's right end; the field has the rest.
    int32_t dw = head_width(&g.downloads, bh);
    layout_place(&g.downloads, (os64_gui_rect_t){area.w - pad - dw, pad, dw, bh}, staged);
    layout_place(&g.field.w, (os64_gui_rect_t){x, pad, area.w - pad - dw - gap - x, bh}, staged);
    int32_t top = pad + bh + pad;
    int32_t bottom = area.h - pad - bh;
    int32_t bar_h = g.bar.up ? bh + gap : 0;
    int32_t vh = bottom - pad - sw - top - bar_h, vw = area.w - 2 * pad - sw;
    // A font that leaves no usable address field or page area cannot fit.
    if (staged && (area.w - pad - dw - gap - x < bh || vh < 1 || vw < 1))
        return OS64_FONT_LIMIT;
    if (vh < 1)
        vh = 1;
    if (vw < 1)
        vw = 1;
    layout_place(&g.view, (os64_gui_rect_t){pad, top, vw, vh}, staged);
    layout_place(&g.vbar.w, (os64_gui_rect_t){pad + vw, top, sw, vh}, staged);
    layout_place(&g.hbar.w, (os64_gui_rect_t){pad, top + vh, vw, sw}, staged);
    int32_t qy = top + vh + sw + gap;
    int32_t yes = button_width(g.qyes.text), no = button_width(g.qno.text);
    layout_place(&g.qpanel, (os64_gui_rect_t){pad, qy, area.w - 2 * pad, bh}, staged);
    layout_place(&g.qno, (os64_gui_rect_t){area.w - pad - no, qy, no, bh}, staged);
    layout_place(&g.qyes, (os64_gui_rect_t){area.w - pad - no - gap - yes, qy, yes, bh}, staged);
    layout_place(&g.qlabel, (os64_gui_rect_t){2 * pad, qy, area.w - pad - no - gap - yes - 3 * pad, bh}, staged);
    // The badge takes the bar's right end when it has anything to say, so
    // a long hover address never pushes it out of sight.
    int32_t badge_w = g.badge_text[0] != '\0' ? button_width(g.badge_text) : 0;
    if (badge_w > (area.w - 2 * pad) / 2)
        badge_w = (area.w - 2 * pad) / 2;
    layout_place(&g.badge, (os64_gui_rect_t){area.w - pad - badge_w, bottom, badge_w, bh}, staged);
    if (!staged)
        os64_ui_set_hidden(&g.ui, &g.badge, badge_w == 0);
    layout_place(&g.status, (os64_gui_rect_t){pad, bottom, area.w - 2 * pad - (badge_w != 0 ? badge_w + gap : 0), bh}, staged);
    return OS64_FONT_OK;
}

static void layout(void)
{
    (void)layout_window(false);
}

static bool s_interface_resize_due;

static os64_font_status_t interface_font_plan(os64_ui_t *ui, void *user, void **out)
{
    (void)ui;
    (void)user;
    *out = NULL;
    return layout_window(true);
}

static void interface_font_commit(os64_ui_t *ui, void *user, void *plan)
{
    (void)ui;
    (void)user;
    (void)plan;
    // Commit cannot allocate. Run the viewport resize work after adoption,
    // before dispatching another event against its changed coordinates.
    s_interface_resize_due = true;
}

static void interface_font_discard(os64_ui_t *ui, void *user, void *plan)
{
    (void)ui;
    (void)user;
    (void)plan;
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
    mark_whole(WHOLE_WINDOW, true);
}

static void interface_font_finish(void)
{
    if (!s_interface_resize_due)
        return;
    s_interface_resize_due = false;
    on_resize(&g.ui);
}

// Why the window loop ended, said in the log as yonder leaves (left_said):
// every way out is a clean exit with code 0, so without it the log cannot
// tell a close the person asked for from one nobody did.
static const char *s_leaving;

static void on_close(os64_ui_t *ui)
{
    (void)ui;
    s_leaving = "the window was asked to close (WINDOW_CLOSE)";
    g.running = false;
}

static const char *gui_err_name(int64_t rc)
{
    switch (rc) {
    case OS64_GUI_ERR_INVALID_HANDLE: return "INVALID_HANDLE: the window is gone";
    case OS64_GUI_ERR_NO_RESOURCES: return "NO_RESOURCES";
    case OS64_GUI_ERR_BAD_ARGS: return "BAD_ARGS";
    case OS64_GUI_ERR_INTERRUPTED: return "INTERRUPTED: a signal ended the wait";
    case OS64_GUI_ERR_NOT_OWNER: return "NOT_OWNER";
    case OS64_GUI_ERR_NOT_RUNNING: return "NOT_RUNNING: no GUI";
    case OS64_GUI_ERR_BUSY: return "BUSY: another thread waits on the window";
    case OS64_GUI_ERR_NOT_FOCUSED: return "NOT_FOCUSED";
    default: return "an answer no error names";
    }
}

// One line in the log naming the way the window loop ended: the close,
// libui's quit, or the event wait's answer (`woke`, anything but 1).
static void left_said(int64_t woke)
{
    char said[192];
    if (woke != 1)
        os64_snprintf(said, sizeof(said), "yonder: leaving: waiting for or reading events answered %ld (%s)",
                      (long)woke, gui_err_name(woke));
    else if (s_leaving != NULL)
        os64_snprintf(said, sizeof(said), "yonder: leaving: %s", s_leaving);
    else if (g.ui.quit)
        os64_snprintf(said, sizeof(said), "yonder: leaving: libui's quit");
    else
        os64_snprintf(said, sizeof(said), "yonder: leaving: the loop ended with nothing said why");
    os64_debug_log(said);
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
// bar is drawn in the kernel's face, not the page's. So does a control
// character: a page can spell one (`&#27;`), and the kernel logs the title
// it is given, so an ESC kept here would be an escape sequence written by
// the page into whatever terminal shows that log.
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
        out[at++] = ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
    }
    if (cut)
        for (int i = 0; i < 3; i++)
            out[at++] = '.';
    for (size_t i = 0; i < sizeof(kTail) - 1; i++)
        out[at++] = kTail[i];
    out[at] = '\0';
}

// The name the window wears, and the page and DOM version it was last
// worked out from.
static char s_title_worn[OS64_GUI_TITLE_MAX] = "yonder";
static uint64_t s_title_page, s_title_version;

// THE WINDOW'S NAME FOLLOWS THE PAGE ON SCREEN: "<title> - yonder", or
// "yonder" for a page with none. Called once per pass of the main loop,
// after the turns that run the page's tasks, so a script that sets the
// title many times between two passes costs one rename. The name is worked
// out again only when another page is on screen (g.page_serial) or its tree
// has moved (os64_html_version, which moves on every change a reader could
// see, a write to the <title> element's text included), and the GUI is
// asked only when the name differs from the one worn.
static void title_follow(void)
{
    const os64_html_document_t *doc = page_doc(&g.page);
    uint64_t version = doc != NULL ? os64_html_version(doc) : 0;
    if (g.page_serial == s_title_page && version == s_title_version)
        return;
    s_title_page = g.page_serial;
    s_title_version = version;
    char named[120], title[OS64_GUI_TITLE_MAX] = "yonder";
    title_of(doc, named, sizeof(named));
    if (named[0] != '\0')
        fit_title(named, title);
    if (os64_streq(title, s_title_worn))
        return;
    if (os64_gui_window_set_title(g.win, title, 0) == 0)
        os64_strcopy(s_title_worn, sizeof(s_title_worn), title);
}

static int64_t event_poll(void *opaque, os64_gui_event_t *event)
{
    (void)opaque;
    return os64_gui_event_poll(g.win,event);
}

// The dispatcher's poll: yonder's own window a batch at a time
// (yonder_event_batch_next, which bounds a turn's input and merges idle
// motion), every other window as it comes. The batch's end reads as an
// empty queue for the rest of the turn, except while the dispatcher
// quarantines a destroyed window's handle, when the cap starts a new batch
// and only a queue that is empty says so (yonder_event_batch_draining); a
// lookahead the batch holds is the next turn's first event.
static int64_t window_poll(void *user, int64_t handle, os64_gui_event_t *event)
{
    (void)user;
    if (handle != g.win)
        return g.native.poll(g.native.user, handle, event);
    bool quarantine = !os64_ui_dispatcher_can_create(&g.dispatcher);
    return yonder_event_batch_draining(&g.batch, event, event_poll, NULL, quarantine) ? 1 : 0;
}

// A person's event to libui: a press, or a focus it moves, chooses the
// focused field's caret (field_selection_chosen). A script's focus() is
// performed after its task (asks_perform), never inside this, and chooses
// nothing.
static void ui_dispatch(const os64_gui_event_t *ev)
{
    os64_ui_widget_t *had = g.ui.focus;
    os64_ui_dispatch(&g.ui, ev);
    if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN || g.ui.focus != had)
        field_selection_chosen(g.ui.focus);
}

// One of yonder's window's events. The pointer is read before libui
// because a widget is handed moves only while a button is held and the
// status line follows its current position; the question bar answers by
// its own rules.
static void window_event(void *user, const os64_gui_event_t *event)
{
    (void)user;
    if (!g.running || g.ui.quit)
        return;
    const os64_gui_event_t ev = *event;
    // The Save dialog is modal (§ Downloads): a press, a key, the wheel or
    // Settings brings it forward instead. A release still goes through, so
    // a press that began before the dialog finishes where it began.
    if (s_save.window > 0 &&
        (ev.type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN || ev.type == OS64_GUI_EVENT_KEY_DOWN ||
         ev.type == OS64_GUI_EVENT_MOUSE_WHEEL || ev.type == OS64_GUI_EVENT_SETTINGS)) {
        (void)os64_gui_window_focus(s_save.window);
        return;
    }
    if (ev.type == OS64_GUI_EVENT_INPUT_LOST)
        input_lost();
    if (ev.type == OS64_GUI_EVENT_MOUSE_MOVE)
        hover(ev.mouse.x, ev.mouse.y);
    else if (ev.type == OS64_GUI_EVENT_POINTER_STATE)
        hover(ev.pointer.inside ? ev.pointer.x : -1, ev.pointer.inside ? ev.pointer.y : -1);
    else if (ev.type == OS64_GUI_EVENT_WINDOW_COVERED ||
             ev.type == OS64_GUI_EVENT_WINDOW_UNCOVERED)
        window_seen();
    if (ev.type == OS64_GUI_EVENT_SETTINGS)
        yonder_settings_open(g.win, &g.dispatcher, g.way.agent, settings_use, g.cache,
                             g.scripts_on, (uint32_t)(g.script_ms / 1000), g.zoom_default,
                             zoom_use, s_dark, dark_use, s_animations, s_reduce_motion,
                             motion_use, g.modern, modern_use,
                             s_cookies != NULL ? yonder_cookies_mode(s_cookies) : s_cookies_asked,
                             cookies_use);
    else if (!bar_event(&ev) && !zoom_event(&ev) && !address_event(&ev) && !key_event(&ev) &&
             !password_key(&ev))
        ui_dispatch(&ev);
    interface_font_finish();
    // What the event became, as DOM events, now libui is done with it.
    inputs_run();
}

// yonder paints its window after the turn, with the page's work.
static void window_paint(void *user)
{
    (void)user;
}

int main(int argc, char **argv)
{
    const char *why = faces_open();
    if (why != NULL) {
        os64_printf("yonder: %s\n", why);
        return 1;
    }
    // The window is named after the page (title_follow). A FILE given on
    // the command line is read before the window exists, so the window is
    // born with that page's name rather than taking it a moment later.
    // The switches come before the address, in any order.
    int address_arg = 1;
    for (; address_arg < argc && argv[address_arg][0] == '-' && argv[address_arg][1] == '-'; address_arg++) {
        if (os64_streq(argv[address_arg], "--script-audit"))
            s_script_audit = true;
        else if (os64_streq(argv[address_arg], "--exit-when-loaded"))
            s_exit_when_loaded = true;
        else
            break;
    }
    s_exit_started = s_exit_quiet_from = yonder_now_ms();
    g.scripts_on = yonder_settings_saved_scripts();
    s_dark = yonder_settings_saved_dark();
    s_animations = yonder_settings_saved_animations();
    s_reduce_motion = yonder_settings_saved_reduce_motion();
    g.script_ms = (uint64_t)yonder_settings_saved_script_seconds() * 1000;
    const char *modern_refused;
    g.modern = yonder_settings_saved_modern(&modern_refused);
    const char *diag_refused = diag_dir_open();
    char profile[OS64_PATH_MAX];
    const char *profile_refused = yonder_settings_saved_profile(profile, sizeof(profile));
    s_storage = yonder_storage_new(profile);    // NULL keeps each page's storage for its own life
    s_preflight = way_preflight_new();          // NULL sends every preflight
    yonder_settings_on_clear_storage(storage_clear);
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
    os64_strcopy(s_title_worn, sizeof(s_title_worn), title);
    if (os64_draw_ctx_init(&g.ctx, g.win) != 0) {
        os64_gui_window_destroy(g.win);
        return 1;
    }
    // The display, for `screen`: what a window may take of it is not known
    // here, so it is all of it.
    {
        uint32_t sw = 0, sh = 0;
        if (os64_gui_screen_info(&sw, &sh) == 0 && sw > 0 && sh > 0 && sw <= INT32_MAX && sh <= INT32_MAX)
            s_screen = (os64_dom_screen_t){(int32_t)sw, (int32_t)sh, (int32_t)sw, (int32_t)sh, 24};
    }
    g.hover_link = g.pressed_link = -1;
    s_env.replaced_size = replaced_size;
    g.zoom = g.zoom_default = yonder_settings_saved_zoom();
    g.way.name = "yonder";
    g.way.agent = starting_agent();
    g.way.identity = yonder_agent_identity(g.way.agent);
    g.way.accept = YONDER_ACCEPT;
    g.way.delayed_hint = " - it is in the address field; press Enter to go";
    g.way.jar = way_jar_new();          // NULL keeps no cookies: the pages still load
    const char *cookies_refused;
    yonder_cookies_mode_t cookies = yonder_settings_saved_cookies(&cookies_refused);
    s_cookies_asked = cookies;
    s_cookies = yonder_cookies_new(g.way.jar, profile, cookies);
    if (s_cookies == NULL)
        way_jar_refuse(g.way.jar, cookies == YONDER_COOKIES_OFF);
    yonder_settings_on_clear_cookies(cookies_clear);
    yonder_settings_on_cookies_problem(cookies_problem);
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
    os64_ui_button(&g.downloads, "Downloads", click_downloads, NULL);
    s_downloads_class = *g.downloads.cls;
    s_button_paint = s_downloads_class.paint;
    s_head_class = s_downloads_class;
    s_head_class.name = "head-button";
    s_head_class.paint = head_paint;
    s_downloads_class.name = "downloads";
    s_downloads_class.paint = downloads_paint;
    g.downloads.cls = &s_downloads_class;
    g.back.cls = g.forward.cls = g.reload.cls = g.stop.cls = &s_head_class;
    (void)os64_icon_theme_create(&s_icons);     // NULL: the buttons are captions alone
    os64_ui_textfield(&g.field, g.field_buf, sizeof(g.field_buf), field_submit, field_cancel, NULL);
    os64_ui_label(&g.status, g.status_text);
    os64_ui_label(&g.badge, g.badge_text);
    g.view.cls = &kViewClass;
    g.view.focusable = true;
    g.view.on_context = view_context;
    os64_ui_scrollbar(&g.vbar, scrolled_v, NULL);
    os64_ui_scrollbar(&g.hbar, scrolled_h, NULL);
    g.hbar.horizontal = true;
    os64_ui_panel(&g.qpanel);
    os64_ui_label(&g.qlabel, g.qtext);
    os64_ui_button(&g.qyes, "Yes", NULL, NULL);
    os64_ui_button(&g.qno, "No", NULL, NULL);
    os64_ui_widget_t *kids[] = {&g.back, &g.forward, &g.reload, &g.stop, &g.field.w, &g.downloads, &g.view,
                                &g.vbar.w, &g.hbar.w, &g.qpanel, &g.qlabel, &g.qyes, &g.qno,
                                &g.badge, &g.status};
    for (size_t i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        os64_ui_add_child(&g.root, kids[i]);
    layout();
    os64_ui_set_root(&g.ui, &g.root);
    (void)os64_ui_font_planner(&g.ui, interface_font_plan, interface_font_commit,
                               interface_font_discard, NULL);
    (void)os64_ui_font_follow(&g.ui);
    {
        os64_font_set_t *was = NULL;
        uint32_t was_px = 0;
        if (controls_face_at(g.zoom, &was, &was_px))
            os64_font_set_release(was);
    }
    bar_show(false);
    layout();
    interface_font_finish();
    downloads_follow();

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
    // What yonder.conf asked for and could not have, and a cookie file that
    // could not be read: said last, so it is still there to read when the
    // window first shows, and in one line, so no refusal hides another.
    {
        const char *refused[] = {diag_refused, modern_refused, profile_refused, cookies_refused,
                                 yonder_cookies_problem(s_cookies)};
        char said[512] = "";
        for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
            if (refused[i] != NULL) {
                size_t at = os64_strlen(said);
                os64_snprintf(said + at, sizeof(said) - at, "%s%s", at != 0 ? " " : "", refused[i]);
            }
        if (said[0] != '\0')
            status_rest(said);
        // Said here once already: the loop's turn says only a new one.
        if (yonder_cookies_problem(s_cookies) != NULL)
            os64_strcopy(s_cookies_said, sizeof(s_cookies_said), yonder_cookies_problem(s_cookies));
    }
    os64_ui_set_focus(&g.ui, first != NULL ? &g.view : &g.field.w);
    sync_bars();

    // P5 testing: Million Dollar Homepage's popup stalled while motion kept
    // the queue busy. Each turn reads a bounded batch of yonder's window's
    // events, idle moves merged (window_poll), so it reaches painting
    // without waiting for an empty queue. The turn is the dispatcher's
    // (DROPDOWN.md § Step 1), which reads every window yonder owns; the
    // wait is yonder's, so its sleep is measured as it always was. After
    // painting, a bar not yet armed is armed only if the queues are EMPTY:
    // anything waiting may have been done before the bar could be seen, so
    // the next turn does not sleep, dispatches it to the disarmed bar, and
    // only a turn that finds nothing arms it.
    g.running = true;
    frame_follow();
    glass_paint();
    g.native = os64_ui_dispatcher_native();
    os64_ui_window_backend_t backend = g.native;
    backend.poll = window_poll;
    os64_ui_dispatcher_init(&g.dispatcher, &backend);
    if (os64_ui_dispatcher_add(&g.dispatcher, g.win, &g.ui, window_event, window_paint, NULL) != OS64_UI_OK) {
        s_leaving = "the dispatcher refused yonder's window";
        g.running = false;
    } else if (os64_ui_menu_session_create(&g.dispatcher, &g.ui, g.win, NULL, NULL, NULL, &g.menus) != OS64_UI_OK) {
        g.menus = NULL;
        status_rest("No popup controller; a one-line list will not open.");
    } else if (!context_menus_make()) {
        status_rest("No right-click menu; its verbs could not be made.");
    }
    int64_t woke = 1;               // the wait's or the turn's last answer, said as yonder leaves
    bool looking = false;           // a disarmed bar: is anything waiting?
    while (g.running && !g.ui.quit) {
        // No sleep when a lookahead the last batch holds is waiting already,
        // or (the dispatcher's own rule, can_create false) while the queues
        // drain after a window of yonder's was destroyed.
        if (!looking && !g.batch.held && os64_ui_dispatcher_can_create(&g.dispatcher)) {
            int64_t ready = 0;
            uint64_t asleep = cycles();
            woke = os64_gui_event_wait_any(&ready);
            uint64_t slept = cycles_since(asleep);
            s_cost.waited += slept;
            if (slept > s_cost.slept_most)
                s_cost.slept_most = slept;
            if (woke != 1)
                break;
        }
        g.batch.consumed = 0;
        g.batch.drained = false;
        int64_t events = os64_ui_dispatcher_turn(&g.dispatcher, false);
        if (events < 0) {
            woke = events;
            break;
        }
        if (looking && events == 0 && !g.batch.held) {
            looking = false;
            yonder_bar_settled(&g.bar);
            bar_show(true);
            glass_paint();
            continue;
        }
        if (g.relayout_due) {
            g.relayout_due = false;
            frame_follow();
            relayout(false);
        }
        if (!g.running || g.ui.quit)
            break;
        bool stream_more = stream_turn();
        bool coming_more = coming_turn();
        bool scripts_ready = script_turn();
        storage_turn();
        cookies_turn();
        children_turn();
        downloads_turn();
        pictures_settle();
        title_follow();
        before_paint();
        glass_paint();
        motion_painted();
        if (motion_pause_said())
            glass_paint();
        if (stream_more)
            (void)os64_gui_event_ring(g.win, BELL_STREAM);
        if (coming_more || (scripts_ready && (scripts_owed(g.page.scripts) || s_view_ran ||
                                              yonder_scripts_idle_next(g.page.scripts) <= yonder_now_ms())))
            (void)os64_gui_event_ring(g.win, BELL_SCRIPTS);
        exit_follow();
        // A task may have set or cleared a timer, and a frame for an
        // animation has its whole cost now: the ticker hears of both.
        pictures_schedule();
        looking = g.bar.up && !g.bar.armed;
    }

    left_said(woke);
    // The Settings dialog, the popup controller (an open list or menu goes
    // with it) and the clock first; the clock only rings, and nothing after
    // this should. The menus' verbs go only once the controller has, which
    // borrows them; one it would not let go keeps them too.
    yonder_settings_close();
    if (g.menus != NULL && os64_ui_menu_session_dispose(g.menus) == OS64_UI_OK)
        g.menus = NULL;
    if (g.menus == NULL)
        context_menus_drop();
    yonder_ticker_stop(g.ticker);
    g.ticker = NULL;                    // the page's record, written after, reads its counts
    // The workers next: destroy cancels them — a fetch and a question
    // alike hear the pool's own predicate — and joins them, and releases
    // what they held, before anything they could touch goes away.
    os64_work_pool_destroy(g.pool);
    g.pool = NULL;                      // and no teardown below cancels through it
    downloads_drop();
    head_faces_drop();
    if (g.asker == ASK_REQUEST)
        os64_page_request_free(&g.pending);
    page_departs();
    if (g.stream.active)
        stream_drop();
    inputs_drop();
    yonder_scripts_free(g.page.scripts);
    g.page.scripts = NULL;
    forms_drop();
    page_clear(&g.page);
    page_clear(&g.coming.page);
    way_history_clear(&g.way);
    // After the departure write (page_clear), which reads its counts.
    yonder_paint_cache_free(g.paint_cache);
    g.paint_cache = NULL;
    yonder_scratch_free(g.paint_scratch);
    g.paint_scratch = NULL;
    // What the jar owes its file, written as yonder leaves (in SESSION, the
    // file emptied): the workers that changed it are gone, and so are the
    // pages' scripts.
    yonder_cookies_flush(s_cookies);
    yonder_cookies_free(s_cookies);
    s_cookies = NULL;
    way_jar_free(g.way.jar);
    way_preflight_free(s_preflight);    // the workers that asked it are gone
    s_preflight = NULL;
    if (g.cache != NULL)
        way_cache_close(g.cache);
    // Every page's scripts' host is gone by here, and each made its
    // departure commit as it went.
    yonder_storage_free(s_storage);
    s_storage = NULL;
    // Nothing is left to read an agent: the workers are gone, and the pages.
    yonder_agents_release();
    os64_font_set_release(s_controls_set);  // yonder's own reference to the lent face
    os64_ui_font_release(&g.ui);
    webfonts_leave();
    webfonts_families_free(&s_arriving.families);
    os64_font_family_cache_destroy(s_faces.families);
    os64_text_destroy(s_faces.text);
    os64_gui_window_destroy(g.win);
    return 0;
}
