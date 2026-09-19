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
//
// Interactive keys (ALT, so they cannot collide with anything scribe binds):
//   Alt+1..4   builtin 8x16, DejaVu Sans, Source Sans 3, Source Code Pro
//   Alt+= / -  nominal size, in steps of 4
//   Alt+b      a face too big for the window: scribe's planner must refuse it
//              and leave the editor exactly as it was
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
#include "../../apps/scribe/scribe.h"
#include "../../apps/scribe/scribe_buf.h"

#define FONT_MAX (4u * 1024u * 1024u)
#define DEFAULT_DOC "/tests/fonts/LICENSE-DejaVu.txt"
#define SAVE_PROBE "/home/scribefonttest.out"

static const char *const kFaces[] = {
    "builtin", "DejaVuSans.ttf", "SourceSans3-Regular.otf", "SourceCodePro-Regular.otf",
};
static size_t   gFace = 0;
static uint32_t gSize = 16;
static bool     gSelftest;
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

// The widest line of whatever document is on stage, measured fresh.
static int64_t fresh_extent(os64_ui_t *ui)
{
    int64_t widest = 0;
    for (size_t i = 0; i < scribe_line_count(); ++i) {
        size_t len = 0;
        const char *ln = scribe_line(i, &len);
        int64_t w = 0;
        if (os64_ui_textview_line_width(ui, ln, len, &w) == OS64_FONT_OK && w > widest)
            widest = w;
    }
    return widest;
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
    CHECK("extent measured in the new face", scribe_extent() == fresh_extent(ui));

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
    CHECK("shrunk extent is the true one", narrow == fresh_extent(ui));

    // HELP, AND A FONT CHANGE WHILE IT IS OPEN. The document behind it keeps
    // its bytes and its caret; its scroll and extent were pixels of a face
    // that is gone by the time help closes, so they are measured again.
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
    CHECK("document extent measured in the face now on", scribe_extent() == fresh_extent(ui));

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
}

// ── the hooks ───────────────────────────────────────────────────────────────

static void ready(os64_ui_t *ui, void *user)
{
    (void)user;
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
    if (argc > 1 && os64_streq(argv[1], "--selftest")) {
        gSelftest = true;
        first = 2;
    }
    if (argc > first)
        gDocPath = argv[first];

    // scribe_main reads its file from argv[1], so hand it a clean argv.
    char *args[] = { argv[0], (char *)gDocPath, (char *)0 };
    scribe_hooks_t hooks = { ready, on_key, (void *)0 };
    int rc = scribe_main(2, args, &hooks);
    if (gSelftest)
        return gFailures ? 1 : rc;
    return rc;
}
