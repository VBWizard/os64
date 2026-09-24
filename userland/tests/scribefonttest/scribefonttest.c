// scribefonttest — THE REAL SCRIBE under a real face.
//
// Not a lookalike editor: this links scribe's own window, document model and
// event loop and runs them through scribe_main, adding hooks production
// scribe does not have. Everything that changes the face goes through the
// same adoption transaction F5 will use, with scribe's own layout planner
// staging the window.
//
//   scribefonttest [file]              you drive it
//   scribefonttest --selftest [file]   it drives itself, prints PASS/FAIL, exits
//   scribefonttest --long-demo path    writes a file with a 2.5 MB line, and a
//                                      line holding one 10 KB cluster, to open
//                                      in scribe, and exits
//   scribefonttest --log-demo path     writes a 9 MB log-shaped file, and exits
//   scribefonttest --time-open path    times an Open of it, a change to DejaVu
//                                      Sans with it open, and an Open under
//                                      that face; prints, exits
//
// Interactive keys (ALT, so they cannot collide with anything scribe binds):
//   Alt+1..4   builtin 8x16, DejaVu Sans, Source Sans 3, Source Code Pro
//   Alt+= / -  nominal size, in steps of 4
//   Alt+b      a face too big for the window: scribe's planner must refuse it
//              and leave the editor exactly as it was
//   Alt+w      a 64-byte window for long lines, so both its ends show, and
//              back to the default
//
// WHY THE FONTS COME FROM /tests: which fonts os64 ships, and how a person
// chooses one, is F5's question. Fixture bytes pre-empt none of it.

#include "os64/os64.h"
#include "os64/fmt.h"
#include "os64/str.h"
#include "os64/mem.h"
#include "os64/io.h"
#include "os64/slurp.h"
#include "os64/ui.h"
#include "os64/font_provider.h"
#include "os64/font_adopt.h"
#include "os64/text.h"
#include "../../apps/scribe/scribe.h"
#include "../../apps/scribe/scribe_buf.h"

#define FONT_MAX (4u * 1024u * 1024u)
#define DEFAULT_DOC "/tests/fonts/LICENSE-DejaVu.txt"
#define SAVE_PROBE "/home/scribefonttest.out"
#define SAVE_INPUT "/home/scribefonttest.in"

static const char *const kFaces[] = {
    "builtin", "DejaVuSans.ttf", "SourceSans3-Regular.otf", "SourceCodePro-Regular.otf",
};
static size_t   gFace = 0;
static uint32_t gSize = 16;
static bool     gSelftest, gTimeOpen;
static const char *gDocPath = DEFAULT_DOC;
static int      gFailures, gChecks;

// ── fonts ───────────────────────────────────────────────────────────────────

// All three roles from one file; the terminal role keeps the mono face,
// because the provider refuses a set whose terminal is proportional and this
// window has no terminal to show.
static os64_font_status_t adopt(os64_ui_t *ui, size_t face, uint32_t size)
{
    os64_text_context_t *text = os64_ui_font_context(ui);
    if (!text)
        return OS64_FONT_NO_MEMORY;

    uint8_t *bytes = NULL, *mono = NULL;
    size_t length = 0, mono_length = 0;
    char path[96];
    if (face != 0) {
        os64_snprintf(path, sizeof(path), "/tests/fonts/%s", kFaces[face]);
        if (os64_slurp(path, FONT_MAX, &bytes, &length) != OS64_SLURP_OK)
            return OS64_FONT_MISSING;
    }
    if (os64_slurp("/tests/fonts/DejaVuSansMono.ttf", FONT_MAX, &mono, &mono_length)
        != OS64_SLURP_OK) {
        os64_free(bytes);
        return OS64_FONT_MISSING;
    }

    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT] = {0};
    for (int role = 0; role < OS64_FONT_ROLE_COUNT; ++role) {
        specs[role].pixel_height = face == 0 ? 16 : size;
        if (face != 0)
            specs[role].primary = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, bytes, length};
    }
    specs[OS64_FONT_ROLE_TERMINAL].pixel_height = face == 0 ? 16 : size;
    specs[OS64_FONT_ROLE_TERMINAL].primary =
        (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, mono, mono_length};

    os64_font_set_t *candidate = NULL;
    os64_font_status_t status = os64_font_set_prepare(text, specs, &candidate);
    os64_free(bytes);
    os64_free(mono);
    if (status != OS64_FONT_OK)
        return status;

    os64_font_consumer_t consumer;
    os64_ui_font_consumer(ui, &consumer);
    size_t failed = 0;
    status = os64_font_adopt(candidate, &consumer, 1, &failed);
    os64_font_set_release(candidate);
    return status;
}

// ── looking at the document without touching it ─────────────────────────────

// FNV-1a over every line and a separator, so a changed byte anywhere — or a
// line that moved — changes the answer.
static uint32_t doc_hash(void)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < scribe_line_count(); ++i) {
        size_t len = 0;
        const char *ln = scribe_line(i, &len);
        for (size_t b = 0; b < len; ++b)
            h = (h ^ (uint8_t)ln[b]) * 16777619u;
        h = (h ^ 0x0a) * 16777619u;
    }
    return h;
}

typedef struct {
    size_t cur_line, cur_col, sel_line, sel_col;
    bool sel, focused;
    uint32_t hash;
    bool dirty;
} snapshot_t;

static snapshot_t snap(os64_ui_t *ui)
{
    os64_ui_textview_t *tv = scribe_view();
    return (snapshot_t){ tv->cur_line, tv->cur_col, tv->sel_line, tv->sel_col,
                         tv->sel, ui->focus == &tv->w, doc_hash(), scribe_dirty() };
}

static bool same_snapshot(const snapshot_t *a, const snapshot_t *b)
{
    return a->cur_line == b->cur_line && a->cur_col == b->cur_col &&
           a->sel == b->sel && (!a->sel || (a->sel_line == b->sel_line &&
                                            a->sel_col == b->sel_col)) &&
           a->focused == b->focused && a->hash == b->hash && a->dirty == b->dirty;
}

// The widest row the view shows now: what scribe's extent must be just
// after it started over, at a load or a font change. -1 if it could not be
// measured, which no extent equals.
static int64_t shown_extent(os64_ui_t *ui)
{
    int64_t w = -1;
    os64_ui_textview_shown_width(ui, scribe_view(), &w);
    return w;
}

// ── driving the real editor through its real front door ─────────────────────

static void key(os64_ui_t *ui, char ascii, uint8_t scancode, uint8_t mods)
{
    os64_gui_event_t ev = {0};
    ev.type = OS64_GUI_EVENT_KEY_DOWN;
    ev.key.ascii = ascii;
    ev.key.scancode = scancode;
    ev.key.modifiers = mods;
    os64_ui_dispatch(ui, &ev);
}

// Arrows and End arrive as VT100 bursts, stamped with the extended key's
// scancode so the decoder knows the ESC is not the Esc key.
static void burst(os64_ui_t *ui, char final, uint8_t mods)
{
    key(ui, 0x1b, 0x4d, mods);
    key(ui, '[', 0x4d, mods);
    key(ui, final, 0x4d, mods);
}

static bool same_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

#define CHECK(what, cond) do { ++gChecks; if (!(cond)) { ++gFailures; \
    os64_printf("scribefonttest: FAIL %s (line %d)\n", what, __LINE__); } } while (0)

// ── a line of megabytes ─────────────────────────────────────────────────────

#define LONG_WORD "caf\xc3\xa9 words "          // 12 bytes, a two-byte letter in it
#define LONG_BYTES (218453u * 12u)              // 2.5 MB of whole words
#define LONG_INPUT "/home/scribefonttest.long"
#define LONG_OUTPUT "/home/scribefonttest.long.out"

// "head", the long line, "tail": the file a demo opens and the self-test
// edits. False if the disk refused it.
static bool write_long_file(const char *path)
{
    static char chunk[4096 * 3];               // a whole number of words
    for (size_t i = 0; i < sizeof(chunk); ++i)
        chunk[i] = LONG_WORD[i % 12];
    int64_t fd = os64_open(path, "w");
    if (fd < 0)
        return false;
    bool ok = os64_write((int32_t)fd, "head\n", 5) == 5;
    for (size_t done = 0; ok && done < LONG_BYTES; done += sizeof(chunk)) {
        size_t n = LONG_BYTES - done < sizeof(chunk) ? LONG_BYTES - done : sizeof(chunk);
        ok = os64_write((int32_t)fd, chunk, n) == (int64_t)n;
    }
    ok = ok && os64_write((int32_t)fd, "\ntail\n", 6) == 6;
    os64_close((int32_t)fd);
    return ok;
}

static bool is_long_word_run(const uint8_t *b, size_t at, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (b[i] != (uint8_t)LONG_WORD[(at + i) % 12])
            return false;
    return true;
}

// Opened with scribe's own Open, edited deep inside through the editor's
// keys, split there, and saved with Save As: every byte the window never
// showed must come back as it was, around exactly the edits made.
static void long_line(os64_ui_t *ui)
{
    CHECK("write a 2.5 MB line", write_long_file(LONG_INPUT));
    scribe_open(LONG_INPUT);
    size_t len = 0;
    scribe_line(1, &len);
    CHECK("open it", scribe_line_count() == 3 && len == LONG_BYTES);

    size_t mid = LONG_BYTES / 2 - (LONG_BYTES / 2) % 12;   // a word's start
    os64_ui_textview_t *tv = scribe_view();
    os64_ui_textview_goto(ui, tv, 1, mid, false);
    key(ui, 'X', 0x2d, OS64_GUI_MOD_SHIFT);
    key(ui, 'Y', 0x15, OS64_GUI_MOD_SHIFT);
    os64_text_run_view_t rv;
    CHECK("the row lays out a window, not the line",
          tv->w.run && os64_text_run_view(tv->w.run, &rv) == OS64_FONT_OK &&
          rv.byte_count <= OS64_UI_TEXTVIEW_WINDOW);
    key(ui, '\r', 0x1c, 0);                    // split it there
    burst(ui, 'F', 0);                         // End
    key(ui, 'Z', 0x2c, OS64_GUI_MOD_SHIFT);
    CHECK("save it", scribe_save_as(LONG_OUTPUT));

    uint8_t *back = NULL;
    size_t blen = 0;
    bool ok = os64_slurp(LONG_OUTPUT, FONT_MAX, &back, &blen) == OS64_SLURP_OK &&
              blen == 5 + LONG_BYTES + 3 + 1 + 6;
    if (ok) {
        size_t at = 0;
        ok = same_bytes(back, (const uint8_t *)"head\n", 5);
        at = 5;
        ok = ok && is_long_word_run(back + at, 0, mid);
        at += mid;
        ok = ok && same_bytes(back + at, (const uint8_t *)"XY\n", 3);
        at += 3;
        ok = ok && is_long_word_run(back + at, mid, LONG_BYTES - mid);
        at += LONG_BYTES - mid;
        ok = ok && same_bytes(back + at, (const uint8_t *)"Z\ntail\n", 7);
    }
    CHECK("every omitted byte saved as it was", ok);
    os64_free(back);
    os64_unlink(LONG_INPUT);
    os64_unlink(LONG_OUTPUT);
}

// ── the self-test ───────────────────────────────────────────────────────────

static void selftest(os64_ui_t *ui)
{
    // STARTUP. The builtin face, the document loaded, nothing unsaved.
    CHECK("document loaded", scribe_line_count() > 3);
    CHECK("starts clean", !scribe_dirty());
    CHECK("starts on the builtin face",
          os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT) == 16);

    // AN UNSAVED EDIT, made through the editor rather than behind it.
    burst(ui, 'F', 0);                       // End
    key(ui, 'X', 0x2d, OS64_GUI_MOD_SHIFT);
    key(ui, 'Y', 0x15, OS64_GUI_MOD_SHIFT);
    key(ui, 'Z', 0x2c, OS64_GUI_MOD_SHIFT);
    CHECK("the edit landed", scribe_dirty());

    // A SELECTION, three clusters back from the caret.
    for (int i = 0; i < 3; ++i)
        burst(ui, 'D', OS64_GUI_MOD_SHIFT);
    CHECK("a selection exists", scribe_view()->sel);

    // SWITCH FACES WITH ALL OF THAT OUTSTANDING. The bytes, the caret, the
    // selection, the focus and the unsaved flag must all come through.
    snapshot_t before = snap(ui);
    CHECK("adopt DejaVu Sans 20", adopt(ui, 1, 20) == OS64_FONT_OK);
    snapshot_t after = snap(ui);
    CHECK("unsaved edit survives the switch", same_snapshot(&before, &after));
    CHECK("the new face is really on", os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT) != 16);
    CHECK("extent measured in the new face", scribe_extent() == shown_extent(ui));

    // AND AGAIN, to a different family: the selection is still the same bytes.
    CHECK("adopt Source Sans 3 18", adopt(ui, 2, 18) == OS64_FONT_OK);
    snapshot_t again = snap(ui);
    CHECK("selection survives a second switch", same_snapshot(&before, &again));

    // THE WIDEST LINE follows the face, including when it gets SMALLER — the
    // case scribe's grow-only edit path never handles on its own.
    CHECK("adopt DejaVu Sans 24", adopt(ui, 1, 24) == OS64_FONT_OK);
    int64_t wide = scribe_extent();
    CHECK("adopt DejaVu Sans 12", adopt(ui, 1, 12) == OS64_FONT_OK);
    int64_t narrow = scribe_extent();
    CHECK("a smaller face shrinks the extent", narrow < wide);
    CHECK("shrunk extent is the true one", narrow == shown_extent(ui));

    // HELP, AND A FONT CHANGE WHILE IT IS OPEN. The document behind it keeps
    // its bytes and its caret. Its extent and its scroll were pixels of a
    // face that is gone by the time help closes, so the extent grows again
    // from the rows it shows and the scroll is found again around the caret.
    snapshot_t doc = snap(ui);
    scribe_toggle_help();
    CHECK("help is on stage", scribe_help_active());
    int64_t help_small = scribe_extent();
    CHECK("font change while help is open", adopt(ui, 1, 20) == OS64_FONT_OK);
    CHECK("help re-measured under the new face", scribe_extent() != help_small);
    CHECK("help is still on stage", scribe_help_active());
    scribe_toggle_help();
    CHECK("document back on stage", !scribe_help_active());
    snapshot_t back = snap(ui);
    CHECK("document caret and bytes survive help + a font change",
          back.cur_line == doc.cur_line && back.cur_col == doc.cur_col &&
          back.hash == doc.hash && back.dirty == doc.dirty);
    CHECK("document extent measured in the face now on", scribe_extent() == shown_extent(ui));

    // REFUSAL. A face too big for this window: scribe's own planner turns it
    // down, and the editor is exactly as it was and still takes typing.
    int32_t row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT);
    int64_t extent = scribe_extent();
    snapshot_t kept = snap(ui);
    // LIMIT specifically: that is scribe's layout saying the window cannot
    // hold this face, rather than something upstream failing for a reason
    // that has nothing to do with the editor.
    os64_font_status_t big = adopt(ui, 1, 96);
    if (big != OS64_FONT_LIMIT)
        os64_printf("scribefonttest: 96px refusal status %d\n", (int)big);
    CHECK("a 96px face is refused by scribe's layout", big == OS64_FONT_LIMIT);
    snapshot_t refused = snap(ui);
    CHECK("refusal leaves the editor as it was", same_snapshot(&kept, &refused));
    CHECK("refusal leaves the face", os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT) == row);
    CHECK("refusal leaves the extent", scribe_extent() == extent);
    uint32_t h = doc_hash();
    key(ui, 'Q', 0x10, OS64_GUI_MOD_SHIFT);
    CHECK("the editor still takes typing", doc_hash() != h);

    // BYTE-IDENTICAL SAVE of an unchanged file, through scribe's own buffer.
    sbuf_t b;
    char err[128];
    CHECK("buffer for the save probe", sbuf_init(&b));
    CHECK("load the untouched document", sbuf_load(&b, gDocPath, err, sizeof(err)) >= 0);
    CHECK("save it unchanged", sbuf_save(&b, SAVE_PROBE, err, sizeof(err)) >= 0);
    sbuf_free(&b);
    uint8_t *original = NULL, *saved = NULL;
    size_t olen = 0, slen = 0;
    CHECK("read the original", os64_slurp(gDocPath, FONT_MAX, &original, &olen) == OS64_SLURP_OK);
    CHECK("read the save", os64_slurp(SAVE_PROBE, FONT_MAX, &saved, &slen) == OS64_SLURP_OK);
    CHECK("an unchanged save is byte-identical",
          original && saved && olen == slen && same_bytes(original, saved, olen));
    os64_free(original);
    os64_free(saved);
    os64_unlink(SAVE_PROBE);

    // HOW A FILE ENDS IS PART OF THE FILE, through the real disk: no final
    // newline, nothing at all, CRLF with and without a last one, malformed
    // UTF-8, control bytes and a NUL. Each is written, loaded, saved
    // untouched and read back.
    static const struct { const char *what, *bytes; size_t len; } ends[] = {
        { "no final newline",     "abc", 3 },
        { "an empty file",        "", 0 },
        { "a final newline",      "abc\n", 4 },
        { "only a newline",       "\n", 1 },
        { "CRLF lines",           "a\r\nb\r\n", 6 },
        { "CRLF, last unended",   "a\r\nb", 4 },
        { "malformed UTF-8",      "\xff\xfe\xc3(\n\xe2\x82", 7 },
        { "control bytes, a NUL", "x\x01\x00y\x7f\n\x00", 7 },
    };
    for (size_t i = 0; i < sizeof(ends) / sizeof(*ends); ++i) {
        char what[64];
        os64_snprintf(what, sizeof(what), "unchanged save: %s", ends[i].what);
        int64_t fd = os64_open(SAVE_INPUT, "w");
        bool wrote = fd >= 0 &&
            (ends[i].len == 0 ||
             os64_write((int32_t)fd, ends[i].bytes, ends[i].len) == (int64_t)ends[i].len);
        if (fd >= 0)
            os64_close((int32_t)fd);
        sbuf_t e;
        bool ok = wrote && sbuf_init(&e);
        if (ok) {
            ok = sbuf_load(&e, SAVE_INPUT, err, sizeof(err)) == 0 &&
                 sbuf_save(&e, SAVE_PROBE, err, sizeof(err)) == 0;
            sbuf_free(&e);
        }
        uint8_t *back = NULL;
        size_t blen = 0;
        ok = ok && os64_slurp(SAVE_PROBE, FONT_MAX, &back, &blen) == OS64_SLURP_OK &&
             blen == ends[i].len &&
             (blen == 0 || same_bytes(back, (const uint8_t *)ends[i].bytes, blen));
        CHECK(what, ok);
        os64_free(back);
        os64_unlink(SAVE_INPUT);
        os64_unlink(SAVE_PROBE);
    }

    long_line(ui);
}

// ── how long a log takes ────────────────────────────────────────────────────
// Scribe's horizontal bar grows from the rows it shows, so an Open and a
// font change lay out a screen of rows, not the document. These are the
// numbers for a log-sized file — for the record, not checks, because they
// are the machine's as much as the code's.

#define LOG_LINES 100000u

// Log-shaped lines, 60 to 124 bytes each: about 9 MB.
static bool write_log_file(const char *path)
{
    int64_t fd = os64_open(path, "w");
    if (fd < 0)
        return false;
    static char buf[16384];
    size_t used = 0;
    bool ok = true;
    for (unsigned i = 0; ok && i < LOG_LINES; ++i) {
        char line[160];
        int n = os64_snprintf(line, sizeof(line),
                              "[%8u.%03u] core %u: scheduler: task %u ran %u ticks ",
                              i / 7, (i * 37) % 1000, i % 8, (i * 13) % 4096, i % 97);
        unsigned pad = (i * 29) % 64;
        for (unsigned p = 0; p < pad && n < (int)sizeof(line) - 1; ++p)
            line[n++] = (char)('a' + (i + p) % 26);
        line[n++] = '\n';
        if (used + (size_t)n > sizeof(buf)) {
            ok = os64_write((int32_t)fd, buf, used) == (int64_t)used;
            used = 0;
        }
        for (int b = 0; b < n; ++b)
            buf[used++] = line[b];
    }
    ok = ok && os64_write((int32_t)fd, buf, used) == (int64_t)used;
    os64_close((int32_t)fd);
    return ok;
}

static unsigned long ms_between(const os64_ticks_t *a, const os64_ticks_t *b)
{
    return (unsigned long)((b->ticks - a->ticks) * 1000u / (a->per_second ? a->per_second : 1));
}

// Timed, in order: opening the document under the builtin face, changing
// to DejaVu Sans with it open, and opening it again under that face.
static void time_open(os64_ui_t *ui)
{
    os64_ticks_t t0, t1, t2, t3;
    os64_ticks(&t0);
    scribe_open(gDocPath);
    os64_ticks(&t1);
    os64_font_status_t status = adopt(ui, 1, 16);
    os64_ticks(&t2);
    scribe_open(gDocPath);
    os64_ticks(&t3);
    os64_printf("scribefonttest: %u lines; builtin Open %lu ms; DejaVu Sans 16 %s in %lu ms,"
                " then Open in %lu ms\n",
                (unsigned)scribe_line_count(), ms_between(&t0, &t1),
                status == OS64_FONT_OK ? "adopted" : "REFUSED",
                ms_between(&t1, &t2), ms_between(&t2, &t3));
}

// ── the hooks ───────────────────────────────────────────────────────────────

static void ready(os64_ui_t *ui, void *user)
{
    (void)user;
    if (gTimeOpen) {
        time_open(ui);
        ui->quit = true;
        return;
    }
    if (!gSelftest)
        return;
    selftest(ui);
    os64_printf("scribefonttest: %s, %d checks, %d failed\n",
                gFailures ? "FAIL" : "PASS", gChecks, gFailures);
    ui->quit = true;       // scribe's loop honours it and returns
}

static void report(os64_ui_t *ui, os64_font_status_t status)
{
    os64_ui_font_metrics_t m;
    os64_ui_font_metrics(ui, OS64_FONT_ROLE_DOCUMENT, &m);
    os64_printf("scribefonttest: %s %upx -> %s (row %d)\n", kFaces[gFace], gSize,
                status == OS64_FONT_OK ? "adopted" : "REFUSED, editor unchanged",
                (int)m.row_h);
}

static bool on_key(os64_ui_t *ui, const os64_gui_event_t *ev, void *user)
{
    (void)user;
    if (!(ev->key.modifiers & OS64_GUI_MOD_ALT))
        return false;
    char a = ev->key.ascii;
    size_t face = gFace;
    uint32_t size = gSize;
    if (a >= '1' && a <= '4')
        face = (size_t)(a - '1');
    else if (a == '=' || a == '+')
        size = size < 72 ? size + 4 : size;
    else if (a == '-' || a == '_')
        size = size > 8 ? size - 4 : size;
    else if (a == 'b') {
        os64_font_status_t status = adopt(ui, gFace ? gFace : 1, 96);
        report(ui, status);
        return true;
    } else if (a == 'w') {
        // A window small enough to see both ends of on screen.
        os64_ui_textview_t *tv = scribe_view();
        tv->window_bytes = tv->window_bytes ? 0 : 64;
        os64_printf("scribefonttest: long-line window %u bytes\n",
                    tv->window_bytes ? (unsigned)tv->window_bytes
                                     : (unsigned)OS64_UI_TEXTVIEW_WINDOW);
        os64_ui_mark_dirty(ui, &tv->w);
        return true;
    } else
        return false;

    os64_font_status_t status = adopt(ui, face, size);
    if (status == OS64_FONT_OK) {
        gFace = face;
        gSize = size;
    }
    report(ui, status);
    return true;
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 2 && os64_streq(argv[1], "--long-demo")) {
        // The self-test's file, and after it a line whose e carries 5,000
        // combining acutes: one cluster longer than the default window.
        bool ok = write_long_file(argv[2]);
        int64_t fd = ok ? os64_open(argv[2], "a") : -1;
        ok = fd >= 0 && os64_write((int32_t)fd, "abc e", 5) == 5;
        for (int i = 0; ok && i < 5000; ++i)
            ok = os64_write((int32_t)fd, "\xcc\x81", 2) == 2;
        ok = ok && os64_write((int32_t)fd, " xyz\n", 5) == 5;
        if (fd >= 0)
            os64_close((int32_t)fd);
        os64_printf("scribefonttest: %s %s\n", ok ? "wrote" : "could not write", argv[2]);
        return ok ? 0 : 1;
    }
    if (argc > 2 && os64_streq(argv[1], "--log-demo")) {
        bool ok = write_log_file(argv[2]);
        os64_printf("scribefonttest: %s %s\n", ok ? "wrote" : "could not write", argv[2]);
        return ok ? 0 : 1;
    }
    if (argc > 1 && os64_streq(argv[1], "--selftest")) {
        gSelftest = true;
        first = 2;
    } else if (argc > 2 && os64_streq(argv[1], "--time-open")) {
        gTimeOpen = true;
        first = 2;
    }
    if (argc > first)
        gDocPath = argv[first];

    // scribe_main reads its file from argv[1], so hand it a clean argv.
    char *args[] = { argv[0], (char *)gDocPath, (char *)0 };
    scribe_hooks_t hooks = { ready, on_key, (void *)0 };
    // A timed run starts empty, so every open it reports is one it timed.
    int rc = scribe_main(gTimeOpen ? 1 : 2, args, &hooks);
    if (gSelftest)
        return gFailures ? 1 : rc;
    return rc;
}
