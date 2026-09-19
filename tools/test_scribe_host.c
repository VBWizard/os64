/* Scribe on the host: the real editor, its real buffer and its real font
 * planner, on a canvas instead of a window.
 *
 * WHAT THIS DRIVES IS SCRIBE ITSELF. scribe.c is #included whole, because
 * its statics are what is under test, and it is built by scribe_build — the
 * function scribe_main calls once it has a window. Keys go through
 * os64_ui_dispatch to the focused textview, font changes through libui's
 * consumer and the real adoption coordinator, files through sbuf_load and
 * sbuf_save over a small in-memory filesystem. None of the editor is
 * reimplemented here; what is stubbed is the window system, the session
 * theme and the disk.
 *
 * The libui suite is #included too, for its allocator (the allocation
 * counter and the switches that refuse), its face loader and its CHECK.
 * Its main is renamed out of the way and never runs here. */

#include <stdarg.h>

#define main ui_test_suite_main
#include "test_ui_text_host.c"
#undef main

#include "scribe.c"
#include "os64/clip.h"

/* ── what the editor asks of the system ────────────────────────────────── */

/* The buffer grows its lines through realloc, and a refused growth is half
 * of what the editing checks are about, so it answers to the same switches
 * as the libui allocator. */
void *os64_realloc(void *p, size_t n)
{
    ++allocations;
    if (ui_test_deny_all) return NULL;
    if (deny_countdown == 0) { deny_countdown = -1; return NULL; }
    if (deny_countdown > 0) --deny_countdown;
    return realloc(p, n ? n : 1);
}

int64_t os64_memory(os64_memory_t *m) { (void)m; return -1; }   /* no ceiling */

int32_t os64_snprintf(char *s, size_t n, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    int r = vsnprintf(s, n, f, a);
    va_end(a);
    return r;
}

/* The theme a window starts with is the session's, read through files and
 * a lock this harness does not have; the suite's view theme stands in. */
void os64_ui_theme_defaults(os64_ui_theme_t *t)
{
    ui_test_view_theme(t);
    t->scroll_w = 12;
}
void os64_ui_theme_current(os64_ui_theme_t *t, uint64_t *installed)
{
    (void)t;
    *installed = 0;
}
bool os64_ui_theme_session(os64_ui_theme_t *t, uint64_t *installed, uint64_t hint)
{
    (void)t; (void)installed; (void)hint;
    return false;                          /* no session changes arrive here */
}

/* A disk of eight files. A write-open truncates; a read can be told to come
 * up short, which is how a disk that fails halfway through a file looks. */
typedef struct { char path[64]; unsigned char *bytes; size_t len; bool used; } sh_file_t;
typedef struct { sh_file_t *f; size_t pos; bool open; } sh_handle_t;
static sh_file_t sh_fs[8];
static sh_handle_t sh_fd[4];
static long sh_read_budget = -1;     /* bytes all reads may return; -1 = all */
static size_t sh_read_chunk;         /* most one read returns; 0 = no limit */

static sh_file_t *sh_find(const char *path, bool create)
{
    for (size_t i = 0; i < 8; ++i)
        if (sh_fs[i].used && strcmp(sh_fs[i].path, path) == 0)
            return &sh_fs[i];
    if (!create)
        return NULL;
    for (size_t i = 0; i < 8; ++i)
        if (!sh_fs[i].used) {
            snprintf(sh_fs[i].path, sizeof(sh_fs[i].path), "%s", path);
            sh_fs[i].used = true;
            sh_fs[i].len = 0;
            return &sh_fs[i];
        }
    return NULL;
}
static void sh_put(const char *path, const void *bytes, size_t len)
{
    sh_file_t *f = sh_find(path, true);
    free(f->bytes);
    f->bytes = malloc(len ? len : 1);
    memcpy(f->bytes, bytes, len);
    f->len = len;
}
static void sh_rm(const char *path)
{
    sh_file_t *f = sh_find(path, false);
    if (!f)
        return;
    free(f->bytes);
    *f = (sh_file_t){0};
}

int64_t os64_stat(const char *path, os64_dirent_t *e)
{
    sh_file_t *f = sh_find(path, false);
    if (!f)
        return -1;
    memset(e, 0, sizeof(*e));
    e->size = f->len;
    return 0;
}
int64_t os64_open(const char *path, const char *mode)
{
    sh_file_t *f = sh_find(path, mode[0] == 'w');
    if (!f)
        return -1;
    for (int i = 0; i < 4; ++i)
        if (!sh_fd[i].open) {
            sh_fd[i] = (sh_handle_t){ f, 0, true };
            if (mode[0] == 'w')
                f->len = 0;
            return 10 + i;
        }
    return -1;
}
int64_t os64_read(int32_t fd, void *p, size_t n)
{
    sh_handle_t *h = &sh_fd[fd - 10];
    size_t left = h->f->len - h->pos;
    if (n > left)
        n = left;
    if (sh_read_chunk && n > sh_read_chunk)
        n = sh_read_chunk;
    if (sh_read_budget >= 0 && n > (size_t)sh_read_budget)
        n = (size_t)sh_read_budget;
    if (sh_read_budget >= 0)
        sh_read_budget -= (long)n;
    memcpy(p, h->f->bytes + h->pos, n);
    h->pos += n;
    return (int64_t)n;
}
int64_t os64_write(int32_t fd, const void *p, size_t n)
{
    sh_handle_t *h = &sh_fd[fd - 10];
    unsigned char *grown = realloc(h->f->bytes, h->f->len + n + 1);
    if (!grown)
        return -1;
    h->f->bytes = grown;
    memcpy(h->f->bytes + h->f->len, p, n);
    h->f->len += n;
    return (int64_t)n;
}
int64_t os64_close(int32_t fd) { sh_fd[fd - 10].open = false; return 0; }
int64_t os64_sync(int32_t fd) { (void)fd; return 0; }

/* ── the window ────────────────────────────────────────────────────────── */

#define SH_W 640
#define SH_H 440
static uint32_t sh_px[SH_W * SH_H];
static const char *sh_dir;

/* The real Scribe, wearing DejaVu Sans at `size` through the same adoption
 * a font change uses, with `doc` loaded through do_load. */
static void sh_scribe(uint32_t size, const char *doc, size_t len)
{
    memset(&g, 0, sizeof(g));
    g.ctx.surf.pixels = sh_px;
    g.ctx.surf.width = SH_W;
    g.ctx.surf.height = SH_H;
    g.ctx.surf.pitch_px = SH_W;
    CHECK(scribe_build() == NULL);
    CHECK(ui_test_adopt(&g.ui, sh_dir, size) == OS64_FONT_OK);
    sh_put("/doc", doc, len);
    do_load("/doc");
    os64_ui_render(&g.ui, NULL);
}

static void sh_close(void)
{
    CHECK(os64_ui_font_release(&g.ui) == OS64_FONT_OK);
    sbuf_free(&g.buf);
}

/* The document as a file: what sbuf_save would write, byte for byte. */
static size_t sh_bytes(unsigned char *out, size_t cap)
{
    char err[96];
    CHECK(sbuf_save(&g.buf, "/snap", err, sizeof(err)) == 0);
    sh_file_t *f = sh_find("/snap", false);
    size_t n = f && f->len < cap ? f->len : 0;
    if (f)
        memcpy(out, f->bytes, n);
    sh_rm("/snap");
    return n;
}
static bool sh_doc_is(const char *want, size_t len)
{
    unsigned char got[512];
    bool dirty = g.buf.dirty;
    size_t n = sh_bytes(got, sizeof(got));
    g.buf.dirty = dirty;                 /* looking is not saving */
    return n == len && memcmp(got, want, len) == 0;
}
#define SH_DOC_IS(lit) sh_doc_is(lit, sizeof(lit) - 1)

static void sh_key(char ascii, uint8_t mods)
{
    os64_gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = OS64_GUI_EVENT_KEY_DOWN;
    ev.key.ascii = ascii;
    ev.key.scancode = 0x0e;
    ev.key.modifiers = mods;
    os64_ui_dispatch(&g.ui, &ev);
}
static void sh_burst(const char *seq, uint8_t mods)
{
    sh_key(27, mods);
    for (const char *p = seq; *p; ++p)
        sh_key(*p, mods);
}
#define SH_BACKSPACE() sh_key('\b', 0)
#define SH_DELETE()    sh_burst("[3~", 0)
#define SH_RIGHT(m)    sh_burst("[C", (m))
#define SH_DOWN()      sh_burst("[B", 0)

static void sh_caret(size_t line, size_t col)
{
    g.view.cur_line = line;
    g.view.cur_col = col;
    g.view.sel = false;
}

/* Is the caret on a boundary of its own line? */
static bool sh_caret_legal(void)
{
    size_t len;
    const char *ln = g.textbuf.line(g.textbuf.user, g.view.cur_line, &len);
    return g.view.cur_col <= len &&
           os64_ui_text_snap(ln, len, g.view.cur_col, false) == g.view.cur_col;
}

/* ── C2-R1: deletion is by cluster, through the real handler ──────────── */

static void scribe_deletes_whole_letters(void)
{
    current = "scribe deletion";

    /* Precomposed é: two bytes, one letter, from either side. */
    sh_scribe(16, "caf\xc3\xa9\n", 6);
    sh_caret(0, 5);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("caf\n") && g.view.cur_col == 3);
    sh_close();
    sh_scribe(16, "caf\xc3\xa9\n", 6);
    sh_caret(0, 3);
    SH_DELETE();
    CHECK(SH_DOC_IS("caf\n") && g.view.cur_col == 3);
    sh_close();

    /* Decomposed: e and a combining acute, three bytes, one letter. */
    sh_scribe(16, "cafe\xcc\x81\n", 7);
    sh_caret(0, 6);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("caf\n") && g.view.cur_col == 3);
    sh_close();
    sh_scribe(16, "cafe\xcc\x81\n", 7);
    sh_caret(0, 3);
    SH_DELETE();
    CHECK(SH_DOC_IS("caf\n") && g.view.cur_col == 3);
    sh_close();

    /* The caret before the letter and after it. */
    sh_scribe(16, "\xc3\xa9t\n", 4);
    sh_caret(0, 0);
    SH_DELETE();
    CHECK(SH_DOC_IS("t\n") && g.view.cur_col == 0);
    sh_close();
    sh_scribe(16, "\xc3\xa9t\n", 4);
    sh_caret(0, 2);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("t\n") && g.view.cur_col == 0);
    sh_close();

    /* The line joins at either end are still one newline each. */
    sh_scribe(16, "ab\n\xc3\xa9\n", 6);
    sh_caret(1, 0);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("ab\xc3\xa9\n"));
    CHECK(g.view.cur_line == 0 && g.view.cur_col == 2);
    SH_DELETE();                                   /* and é from the join */
    CHECK(SH_DOC_IS("ab\n"));
    sh_close();
    sh_scribe(16, "ab\n\xc3\xa9\n", 6);
    sh_caret(0, 2);
    SH_DELETE();
    CHECK(SH_DOC_IS("ab\xc3\xa9\n") && g.view.cur_col == 2);
    sh_close();

    /* A selection made by Shift+Right covers the whole letter and goes. */
    sh_scribe(16, "x\xc3\xa9y\n", 5);
    sh_caret(0, 1);
    SH_RIGHT(OS64_GUI_MOD_SHIFT);
    CHECK(g.view.sel && g.view.sel_col == 1 && g.view.cur_col == 3);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("xy\n") && g.view.cur_col == 1 && !g.view.sel);
    sh_close();

    /* An edit that makes a letter around the caret leaves the caret after
     * it: typing e before a combining mark, and a join that puts one after
     * an e. */
    sh_scribe(16, "\xcc\x81x\n", 4);
    sh_caret(0, 0);
    sh_key('e', 0);
    CHECK(SH_DOC_IS("e\xcc\x81x\n") && g.view.cur_col == 3);
    sh_close();
    sh_scribe(16, "e\n\xcc\x81x\n", 6);
    sh_caret(1, 0);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("e\xcc\x81x\n"));
    CHECK(g.view.cur_line == 0 && g.view.cur_col == 3);
    sh_close();

    /* A deletion can make one too: without the digit, e and the accent are
     * one letter, and the next Backspace takes all of it. */
    sh_scribe(16, "e1\xcc\x81\n", 5);
    sh_caret(0, 1);
    SH_DELETE();
    CHECK(SH_DOC_IS("e\xcc\x81\n") && g.view.cur_col == 3);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("\n") && g.view.cur_col == 0);
    sh_close();
    sh_scribe(16, "e1\xcc\x81\n", 5);
    sh_caret(0, 2);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("e\xcc\x81\n") && g.view.cur_col == 3);
    sh_close();
    current = "";
}

/* ── C2-R2: a refused layout is refused, not turned into bytes ───────── */

static void scribe_keys_under_refusal(void)
{
    current = "scribe refusal";

    /* Every allocation refused: the edits still take whole letters, because
     * finding the letter needs no layout, and the model needs no memory to
     * shrink a line. */
    sh_scribe(16, "caf\xc3\xa9\nzz\n", 9);
    sh_caret(0, 3);
    ui_test_deny_all = true;
    SH_RIGHT(0);
    CHECK(g.view.cur_col == 5);
    SH_BACKSPACE();
    ui_test_deny_all = false;
    CHECK(SH_DOC_IS("caf\nzz\n") && g.view.cur_col == 3);
    sh_close();

    /* Down to a line that cannot be laid out moves nothing at all. */
    sh_scribe(16, "caf\xc3\xa9\n\xc3\xa9\xc3\xa9\xc3\xa9\n", 13);
    sh_caret(0, 5);
    g.view.sel = true;
    g.view.sel_line = 0;
    g.view.sel_col = 1;
    size_t top = g.view.top;
    g.view.w.run = (os64_ui_run_release(g.view.w.run), NULL);   /* cold */
    ui_test_deny_all = true;
    SH_DOWN();
    ui_test_deny_all = false;
    CHECK(g.view.cur_line == 0 && g.view.cur_col == 5);
    CHECK(g.view.sel && g.view.sel_line == 0 && g.view.sel_col == 1);
    CHECK(g.view.top == top);
    CHECK(SH_DOC_IS("caf\xc3\xa9\n\xc3\xa9\xc3\xa9\xc3\xa9\n"));
    sh_close();

    /* EVERY allocation in a Down, refused one at a time: wherever the
     * refusal lands, the caret ends on a boundary of the line it is on, and
     * either it moved to the line below or it did not move. */
    int moved = 0, stayed = 0;
    for (long k = 0; k < 400; ++k) {
        sh_scribe(16, "caf\xc3\xa9\n\xc3\xa9\xc3\xa9\xc3\xa9\n", 13);
        sh_caret(0, 3);
        g.view.w.run = (os64_ui_run_release(g.view.w.run), NULL);
        deny_countdown = k;
        SH_DOWN();
        bool fired = deny_countdown == -1;
        deny_countdown = -1;
        CHECK(sh_caret_legal());
        CHECK(g.view.cur_line == 1 || (g.view.cur_line == 0 && g.view.cur_col == 3));
        if (g.view.cur_line == 1) ++moved; else ++stayed;
        sh_close();
        if (!fired)
            break;
    }
    CHECK(moved > 0 && stayed > 0);            /* both outcomes were reached */

    /* The model refusing: an insert that needs a bigger line, and a split
     * that needs a new one. The caret moves only past bytes that exist. */
    sh_scribe(16, "0123456789abcdef\n", 17);         /* exactly a line's 16 */
    sh_caret(0, 8);          /* mid-line: at the end, a stray step is clamped */
    ui_test_deny_all = true;
    sh_key('x', 0);
    ui_test_deny_all = false;
    CHECK(SH_DOC_IS("0123456789abcdef\n") && g.view.cur_col == 8);
    CHECK(!g.buf.dirty);
    sh_close();
    sh_scribe(16, "abcdef\n", 7);
    sh_caret(0, 3);
    ui_test_deny_all = true;
    sh_key('\r', 0);                                  /* Enter */
    ui_test_deny_all = false;
    CHECK(SH_DOC_IS("abcdef\n"));
    CHECK(g.view.cur_line == 0 && g.view.cur_col == 3);
    sh_close();
    current = "";
}

/* ── C2-R3: the extent is staged, and commit allocates nothing ───────── */

static unsigned long sh_barrier_allocs;
static os64_font_status_t sh_barrier(void *user, void *plan)
{
    (void)user; (void)plan;
    sh_barrier_allocs = allocations;
    return OS64_FONT_OK;
}
static os64_font_status_t sh_refuse(void *user, void *plan)
{
    (void)user; (void)plan;
    return OS64_FONT_LIMIT;
}

static int64_t sh_fresh_extent(const os64_ui_textbuf_t *doc)
{
    int64_t widest = 0;
    size_t count = doc->line_count(doc->user);
    for (size_t i = 0; i < count; ++i) {
        size_t len;
        const char *ln = doc->line(doc->user, i, &len);
        int64_t v = -1;
        CHECK(os64_ui_textview_line_width(&g.ui, ln, len, &v) == OS64_FONT_OK);
        if (v > widest)
            widest = v;
    }
    return widest;
}

static os64_font_status_t sh_adopt(uint32_t size,
                                   os64_font_status_t (*barrier)(void *, void *))
{
    os64_font_set_t *set = outline_set(os64_ui_font_context(&g.ui), sh_dir,
                                       "DejaVuSans.ttf", size);
    CHECK(set != NULL);
    if (!set)
        return OS64_FONT_NO_MEMORY;
    os64_font_consumer_t c;
    os64_ui_font_consumer(&g.ui, &c);
    c.barrier = barrier;
    os64_font_status_t status = os64_font_adopt(set, &c, 1, NULL);
    os64_font_set_release(set);
    return status;
}

/* Everything a refused change must leave as it was. */
typedef struct {
    int32_t row_h;
    int64_t extent, bar_total, saved_extent;
    os64_gui_rect_t view, saveas;
    size_t line, col, sel_line, sel_col, top;
    bool sel;
    unsigned char bytes[256];
    size_t len;
} sh_state_t;

static sh_state_t sh_state(void)
{
    sh_state_t s;
    memset(&s, 0, sizeof(s));
    s.row_h = os64_ui_font_row_height(&g.ui, OS64_FONT_ROLE_DOCUMENT);
    s.extent = g.max_width;
    s.bar_total = g.hscroll.total;
    s.saved_extent = g.saved_max_width;
    s.view = g.view.w.bounds;
    s.saveas = g.btn_saveas.bounds;
    s.line = g.view.cur_line; s.col = g.view.cur_col;
    s.sel = g.view.sel; s.sel_line = g.view.sel_line; s.sel_col = g.view.sel_col;
    s.top = g.view.top;
    bool dirty = g.buf.dirty;
    s.len = sh_bytes(s.bytes, sizeof(s.bytes));
    g.buf.dirty = dirty;
    return s;
}
static bool sh_rect_eq(os64_gui_rect_t a, os64_gui_rect_t b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
static bool sh_same_state(const sh_state_t *a, const sh_state_t *b)
{
    return a->row_h == b->row_h && a->extent == b->extent &&
           a->bar_total == b->bar_total && a->saved_extent == b->saved_extent &&
           sh_rect_eq(a->view, b->view) && sh_rect_eq(a->saveas, b->saveas) &&
           a->line == b->line && a->col == b->col && a->sel == b->sel &&
           a->sel_line == b->sel_line && a->sel_col == b->sel_col &&
           a->top == b->top && a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}
/* The caret, the selection and the text: what a successful change keeps. */
static bool sh_same_document(const sh_state_t *a, const sh_state_t *b)
{
    return a->line == b->line && a->col == b->col && a->sel == b->sel &&
           a->sel_line == b->sel_line && a->sel_col == b->sel_col &&
           a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static const char kShWide[] =
    "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW\n"
    "a short line with caf\xc3\xa9 in it\n";

static void sh_mark_document(void)
{
    g.view.cur_line = 1;
    g.view.cur_col = 10;
    g.view.sel = true;
    g.view.sel_line = 1;
    g.view.sel_col = 2;
}

static void scribe_font_change_stages_its_extent(void)
{
    current = "scribe extent";
    sh_scribe(16, kShWide, sizeof(kShWide) - 1);
    sh_mark_document();
    sh_state_t before = sh_state();
    CHECK(before.extent == sh_fresh_extent(&g.textbuf));

    /* Bigger. Nothing is allocated once every prepare has returned — the
     * barrier is where preparing ends — and the extent the bar gets is the
     * new face's measurement of every line. */
    sh_barrier_allocs = 0;
    CHECK(sh_adopt(24, sh_barrier) == OS64_FONT_OK);
    CHECK(sh_barrier_allocs > 0 && allocations == sh_barrier_allocs);
    sh_state_t big = sh_state();
    CHECK(big.row_h > before.row_h);
    CHECK(big.extent > before.extent);
    CHECK(big.extent == sh_fresh_extent(&g.textbuf));
    CHECK(big.bar_total == big.extent);
    CHECK(sh_same_document(&before, &big));

    /* The first paint after it lays nothing out, anywhere in the window. */
    unsigned long painted = allocations;
    os64_ui_render(&g.ui, NULL);
    CHECK(allocations == painted);

    /* Smaller: the extent shrinks with the face instead of keeping a
     * width no line has any more. */
    CHECK(sh_adopt(12, NULL) == OS64_FONT_OK);
    CHECK(g.max_width < before.extent);
    CHECK(g.max_width == sh_fresh_extent(&g.textbuf));
    CHECK(g.hscroll.total == g.max_width);

    /* Refused after every prepare: nothing of it shows. */
    sh_state_t small = sh_state();
    CHECK(sh_adopt(40, sh_refuse) == OS64_FONT_LIMIT);
    sh_state_t after = sh_state();
    CHECK(sh_same_state(&small, &after));
    sh_close();

    /* EVERY allocation in a font change, refused one at a time. A change
     * that still succeeds has the new face's extent in full; one that is
     * refused leaves the face, the layout, the extent, the caret, the
     * selection and the bytes exactly as they were. */
    int ok = 0, refused = 0;
    for (long k = 0; k < 20000; ++k) {
        sh_scribe(16, kShWide, sizeof(kShWide) - 1);
        sh_mark_document();
        sh_state_t old = sh_state();
        os64_font_set_t *set = outline_set(os64_ui_font_context(&g.ui), sh_dir,
                                           "DejaVuSans.ttf", 24);
        os64_font_consumer_t c;
        os64_ui_font_consumer(&g.ui, &c);
        deny_countdown = k;
        os64_font_status_t status = os64_font_adopt(set, &c, 1, NULL);
        bool fired = deny_countdown == -1;
        deny_countdown = -1;
        os64_font_set_release(set);
        sh_state_t now = sh_state();
        if (status == OS64_FONT_OK) {
            ++ok;
            CHECK(now.extent == sh_fresh_extent(&g.textbuf));
            CHECK(now.bar_total == now.extent);
            CHECK(now.row_h > old.row_h);
            CHECK(sh_same_document(&old, &now));
        } else {
            ++refused;
            CHECK(sh_same_state(&old, &now));
        }
        sh_close();
        if (!fired)
            break;
    }
    CHECK(ok > 0 && refused > 0);
    current = "";
}

/* The document waiting behind the help page is measured by the same change
 * and committed with it; the page itself gets its own extent. */
static void scribe_font_change_under_help(void)
{
    current = "scribe help";
    sh_scribe(16, kShWide, sizeof(kShWide) - 1);
    sh_mark_document();
    sh_state_t doc = sh_state();
    help_toggle();
    CHECK(g.help_active);
    CHECK(g.max_width == sh_fresh_extent(&kHelpBuf));

    sh_barrier_allocs = 0;
    CHECK(sh_adopt(24, sh_barrier) == OS64_FONT_OK);
    CHECK(allocations == sh_barrier_allocs);
    CHECK(g.max_width == sh_fresh_extent(&kHelpBuf));
    CHECK(g.hscroll.total == g.max_width);
    CHECK(g.saved_max_width == sh_fresh_extent(&g.textbuf));
    CHECK(g.saved_max_width > doc.extent);

    /* Refused while help is up: neither extent moves. */
    sh_state_t up = sh_state();
    CHECK(sh_adopt(40, sh_refuse) == OS64_FONT_LIMIT);
    sh_state_t still = sh_state();
    CHECK(sh_same_state(&up, &still));

    help_toggle();
    CHECK(!g.help_active);
    sh_state_t back = sh_state();
    CHECK(sh_same_document(&doc, &back));
    CHECK(back.extent == sh_fresh_extent(&g.textbuf));
    CHECK(g.hscroll.total == back.extent);
    int32_t cx = -1;
    size_t len;
    const char *ln = g.textbuf.line(g.textbuf.user, g.view.cur_line, &len);
    void *run = NULL;
    CHECK(os64_ui_run_layout(&g.ui, OS64_FONT_ROLE_DOCUMENT, ln, len, &run) == OS64_FONT_OK);
    if (run) {
        CHECK(os64_ui_run_caret(run, g.view.cur_col, false, &cx) == OS64_FONT_OK);
        os64_ui_run_release(run);
    }
    CHECK(cx >= g.view.left_px &&
          cx - g.view.left_px <= os64_ui_textview_width(&g.view));   /* in view */
    sh_close();
    current = "";
}

/* ── C2-R5: a file saves as it loaded ──────────────────────────────────── */

typedef struct { const char *bytes; size_t len; } sh_case_t;
#define SH_CASE(lit) { lit, sizeof(lit) - 1 }

static void scribe_saves_what_it_loaded(void)
{
    current = "scribe save";
    static const sh_case_t cases[] = {
        SH_CASE("abc"), SH_CASE(""), SH_CASE("abc\n"), SH_CASE("\n"),
        SH_CASE("a\n\n"), SH_CASE("\n\n\n"), SH_CASE("a\r\nb\r\n"),
        SH_CASE("a\r\nb"), SH_CASE("\r"), SH_CASE("\xff\xfe\xc3(\n\xe2\x82"),
        SH_CASE("x\x01\x00y\x7f\n\x00"), SH_CASE("caf\xc3\xa9\ncafe\xcc\x81\n"),
        SH_CASE("\t tabs\t\n  trailing  "),
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        sbuf_t b;
        char err[96];
        CHECK(sbuf_init(&b));
        sh_put("/in", cases[i].bytes, cases[i].len);
        CHECK(sbuf_load(&b, "/in", err, sizeof(err)) == 0);
        CHECK(!b.dirty);
        CHECK(sbuf_save(&b, "/out", err, sizeof(err)) == 0);
        sh_file_t *out = sh_find("/out", false);
        CHECK(out && out->len == cases[i].len &&
              memcmp(out->bytes, cases[i].bytes, cases[i].len) == 0);
        sbuf_free(&b);
    }

    /* Each edit is the byte edit it looks like, at the end of a file too. */
    sh_scribe(16, "abc", 3);
    sh_caret(0, 3);
    sh_key('\r', 0);                         /* Enter adds the newline it lacked */
    CHECK(SH_DOC_IS("abc\n"));
    sh_close();
    sh_scribe(16, "abc\n", 4);
    sh_caret(0, 3);
    sh_key('\r', 0);
    CHECK(SH_DOC_IS("abc\n\n"));
    sh_close();
    sh_scribe(16, "", 0);
    sh_key('x', 0);
    CHECK(SH_DOC_IS("x"));
    sh_close();
    sh_scribe(16, "a\nb", 3);
    sh_caret(1, 0);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("ab"));
    sh_close();
    sh_scribe(16, "a\nb\n", 4);
    sh_caret(1, 0);
    SH_BACKSPACE();
    CHECK(SH_DOC_IS("ab\n"));
    sh_close();

    /* A file that does not exist yet is made the Unix way. */
    memset(&g, 0, sizeof(g));
    g.ctx.surf.pixels = sh_px;
    g.ctx.surf.width = SH_W;
    g.ctx.surf.height = SH_H;
    g.ctx.surf.pitch_px = SH_W;
    CHECK(scribe_build() == NULL);
    sh_rm("/new");
    do_load("/new");
    sh_key('h', 0);
    sh_key('i', 0);
    CHECK(SH_DOC_IS("hi\n"));
    sh_close();
    current = "";
}

/* A load that fails leaves the buffer the user was editing, unsaved
 * changes and all — never part of the file they asked for. */
static void scribe_failed_load_keeps_the_document(void)
{
    current = "scribe load";
    static const char next[] = "one\ntwo\nthree\n";
    int loaded = 0, failed = 0;
    for (long k = 0; k < 100; ++k) {
        sbuf_t b;
        char err[96];
        CHECK(sbuf_init(&b));
        sh_put("/in", "kept\n", 5);
        CHECK(sbuf_load(&b, "/in", err, sizeof(err)) == 0);
        os64_ui_textbuf_t tb = sbuf_textbuf_template;
        tb.user = &b;
        CHECK(tb.insert(tb.user, 0, 4, "!", 1));             /* unsaved */
        sh_put("/next", next, sizeof(next) - 1);
        deny_countdown = k;
        int rc = sbuf_load(&b, "/next", err, sizeof(err));
        bool fired = deny_countdown == -1;
        deny_countdown = -1;
        CHECK(sbuf_save(&b, "/out", err, sizeof(err)) == 0);
        sh_file_t *out = sh_find("/out", false);
        if (rc == 0) {
            ++loaded;
            CHECK(out->len == sizeof(next) - 1 && memcmp(out->bytes, next, out->len) == 0);
        } else {
            ++failed;
            CHECK(out->len == 6 && memcmp(out->bytes, "kept!\n", 6) == 0);
        }
        sbuf_free(&b);
        if (!fired)
            break;
    }
    CHECK(loaded > 0 && failed > 0);

    /* A read that comes up short is a failed load, not a shorter file. */
    sbuf_t b;
    char err[96];
    CHECK(sbuf_init(&b));
    sh_put("/in", "kept\n", 5);
    CHECK(sbuf_load(&b, "/in", err, sizeof(err)) == 0);
    sh_read_budget = 4;
    CHECK(sbuf_load(&b, "/next", err, sizeof(err)) == -1);
    sh_read_budget = -1;
    CHECK(sbuf_save(&b, "/out", err, sizeof(err)) == 0);
    sh_file_t *out = sh_find("/out", false);
    CHECK(out->len == 5 && memcmp(out->bytes, "kept\n", 5) == 0);
    sbuf_free(&b);
    current = "";
}

/* ── C2-R8: a paste into a full field is cut where a letter ends ────────── */

/* The field Scribe uses for paths and searches, pasting from the clipboard
 * FILE. `cap` bytes of storage, `text` in it, the caret at `cursor`. */
static size_t sh_paste(size_t cap, const char *text, size_t cursor,
                       const char *clip, size_t clip_len, char *out, size_t *out_cursor)
{
    os64_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui_test_view_theme(&ui.theme);
    char buf[32];
    os64_ui_textfield_t tf;
    os64_ui_textfield(&tf, buf, cap, NULL, NULL, NULL);
    tf.w.bounds = (os64_gui_rect_t){0, 0, 200, 32};
    os64_ui_set_root(&ui, &tf.w);
    os64_ui_textfield_set(&ui, &tf, text);
    tf.cursor = cursor;
    sh_put(OS64_CLIPBOARD_PATH, clip, clip_len);
    size_t added = os64_ui_textfield_paste(&ui, &tf);
    memcpy(out, buf, tf.len + 1);
    *out_cursor = tf.cursor;
    CHECK(tf.len == strlen(buf));
    CHECK(os64_ui_font_release(&ui) == OS64_FONT_OK);
    return added;
}

static void field_paste_cuts_at_letters(void)
{
    current = "field paste";
    char out[32];
    size_t cur;

    /* One byte free and a two-byte é on the clipboard: the é stays behind
     * whole, rather than arriving as its first byte. */
    CHECK(sh_paste(5, "abc", 3, "\xc3\xa9", 2, out, &cur) == 0);
    CHECK(strcmp(out, "abc") == 0 && cur == 3);
    /* The same for e + a combining acute: three bytes, one letter. */
    CHECK(sh_paste(5, "abc", 3, "e\xcc\x81", 3, out, &cur) == 0);
    CHECK(strcmp(out, "abc") == 0 && cur == 3);
    /* With room for exactly the é, it all goes in. */
    CHECK(sh_paste(6, "abc", 3, "\xc3\xa9", 2, out, &cur) == 2);
    CHECK(strcmp(out, "abc\xc3\xa9") == 0 && cur == 5);

    /* What fits, up to the last whole letter — precomposed and decomposed,
     * and with the field's own text after the caret kept intact. */
    CHECK(sh_paste(6, "abZ", 2, "x\xc3\xa9", 3, out, &cur) == 1);
    CHECK(strcmp(out, "abxZ") == 0 && cur == 3);
    CHECK(sh_paste(6, "abZ", 2, "xe\xcc\x81", 4, out, &cur) == 1);
    CHECK(strcmp(out, "abxZ") == 0 && cur == 3);

    /* The clipboard arriving a byte per read changes nothing: the letter
     * that crosses a read is judged whole, not by its first piece. */
    sh_read_chunk = 1;
    CHECK(sh_paste(7, "ab", 2, "xy\xc3\xa9z", 5, out, &cur) == 4);
    CHECK(strcmp(out, "abxy\xc3\xa9") == 0 && cur == 6);
    CHECK(sh_paste(6, "ab", 2, "xye\xcc\x81", 5, out, &cur) == 2);
    CHECK(strcmp(out, "abxy") == 0 && cur == 4);
    sh_read_chunk = 0;

    /* The clipboard's own bytes are kept as they came: a malformed byte is
     * a one-byte letter, pasted, not repaired or refused. */
    CHECK(sh_paste(5, "abc", 3, "\xff\xfe", 2, out, &cur) == 1);
    CHECK(memcmp(out, "abc\xff", 5) == 0 && cur == 4);

    /* And a field takes the first line. */
    CHECK(sh_paste(16, "", 0, "\xc3\xa9\nzz", 5, out, &cur) == 2);
    CHECK(strcmp(out, "\xc3\xa9") == 0 && cur == 2);
    sh_rm(OS64_CLIPBOARD_PATH);
    current = "";
}

int main(int argc, char **argv)
{
    sh_dir = argc > 1 ? argv[1] : "userland/libfreetype/fixtures";
    (void)ui_test_suite_main;

    scribe_deletes_whole_letters();
    scribe_keys_under_refusal();
    scribe_font_change_stages_its_extent();
    scribe_font_change_under_help();
    scribe_saves_what_it_loaded();
    scribe_failed_load_keeps_the_document();
    field_paste_cuts_at_letters();

    for (size_t i = 0; i < 8; ++i)
        free(sh_fs[i].bytes);
    printf("test_scribe_host: %lu checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
