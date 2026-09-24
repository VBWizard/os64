// scribe.c — the GUI text editor. SCRIBE.md is the design authority; this
// file is the app half of the bargain it describes: buffer, file plumbing,
// button row, and layout live HERE; everything reusable (textview,
// scrollbar, textfield) went into libui, which is the entire reason this
// program exists (the toolkit grows app-driven, and scribe is the app).
//
// Lineage seat (SCRIBE.md): quill holds ed's chair — the editor as a
// conversation. scribe holds Bravo's (Xerox PARC, 1974): the screen IS the
// document. Modeless, by ruling.
//
//   scribe <file>      edit a file (created on first save if absent)
//   scribe             start empty; Save As names it
//
// Keys: type to insert; arrows/Home/End/PgUp/PgDn move; Shift+motion
// selects; click places, drag selects. Ctrl+S save, Ctrl+F find (Enter =
// next hit), Ctrl+O open, Ctrl+Q quit. Esc leaves the entry field.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/ui.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "scribe_buf.h"
#include "scribe.h"

#define SCRIBE_PATH_MAX  256
#define SCRIBE_STATUS_MAX 96

// What the one entry field currently means. One field, one label naming its
// mode — a menu bar's worth of dialogs in two widgets (his ruling: button
// row; "if we hate it we'll change it").
// MODE_QUITSAVE is MODE_SAVEAS with somewhere to be afterwards (2026-08-23):
// the same one field, prefilled with the same current path, but a successful
// write ends the program instead of returning to it. A separate mode rather
// than a "quitting" flag beside MODE_SAVEAS, because the field's Enter has to
// mean something different and the label has to SAY so.
typedef enum { MODE_NONE, MODE_OPEN, MODE_SAVEAS, MODE_FIND, MODE_QUITSAVE } field_mode_t;

typedef struct
{
    os64_ui_t ui;
    os64_draw_ctx_t ctx;
    int64_t win;

    sbuf_t buf;
    os64_ui_textbuf_t textbuf;      // template copy, user -> &buf

    os64_ui_widget_t root;
    os64_ui_widget_t status;        // left: file, lines, dirty star, verdicts
    os64_ui_widget_t btn_open, btn_save, btn_saveas;
    os64_ui_widget_t mode_label;    // names what the field means right now
    os64_ui_textfield_t field;
    os64_ui_textview_t view;
    os64_ui_scrollbar_t scroll;     // vertical: lines
    os64_ui_scrollbar_t hscroll;    // horizontal: pixels of the document face
    int64_t max_width;              // the widest row the view has shown since
                                    // the document or the face last changed:
                                    // the h-bar's `total`, unless the caret's
                                    // long-line window is wider. It GROWS as
                                    // rows come on screen and as edits widen
                                    // them, and no line is laid out for it
                                    // that has not been on screen: measuring
                                    // a 100,000-line log whole took half a
                                    // minute in QEMU. DEBTS.md books the
                                    // background measurer that would make it
                                    // the whole document's.

    char path[SCRIBE_PATH_MAX];     // empty = unnamed buffer
    char status_text[SCRIBE_STATUS_MAX];
    char field_buf[SCRIBE_PATH_MAX];
    char find_text[SCRIBE_PATH_MAX];

    field_mode_t mode;
    bool running;

    // The help document (Ctrl+G): a READ-ONLY buffer swapped into the same
    // textview — the format seam's first second tenant, arrived early: a
    // vtable with NULL editing ops makes the editor a viewer, and the help
    // page is the proof the log viewer will stand on.
    bool help_active;
    size_t  saved_top, saved_cur_line, saved_cur_col;
    // The document's long-line window, which help would otherwise take with
    // it: moving about the help page settles the view's window onto the help
    // page, and a restored pixel scroll only points at the same text if the
    // row it was measured in comes back too.
    size_t  saved_win_line, saved_win_from, saved_win_budget;
    int64_t saved_left, saved_max_width;
    bool    saved_sel;
    size_t  saved_sel_line, saved_sel_col;
    // Set when the face changes while help is open: saved_left is then in
    // pixels of a retired face, and help_leave scrolls to the caret instead
    // of restoring it. saved_max_width is in those pixels too, and the
    // change sets it to zero: no row of the document has been shown in the
    // new face.
    bool    saved_face_stale;
} scribe_t;

static scribe_t g;

// ── the help document ───────────────────────────────────────────────────────
// Chris's commissioning order, quoted: "it has to have all of the normal
// stuff, *including credits to the author*." Sir, yes sir.

static const char *kHelpLines[] = {
    "scribe - the os64 text editor",
    "",
    "The screen is the document (Bravo, Xerox PARC, 1974).",
    "Click to place the caret. Type. What you see is the file.",
    "",
    "KEYS",
    "  arrows, Home, End, PgUp, PgDn   move (Shift extends the selection)",
    "  click / drag                    place the caret / select",
    "  Backspace, Delete               erase (a selection, if one is lit)",
    "  Enter                           split the line",
    "  Ctrl+C, Ctrl+X, Ctrl+V          copy, cut, paste",
    "  Ctrl+S                          save",
    "  Ctrl+F                          find (Enter = next hit, wraps)",
    "  Ctrl+O                          open another file",
    "  Ctrl+G                          this page (Esc or ^G returns)",
    "  Ctrl+Q, Alt+F4                  quit (asks first if there are",
    "                                  unsaved changes)",
    "",
    "QUITTING WITH CHANGES PENDING",
    "  The * beside the filename means the buffer differs from the disk.",
    "  Quitting then does not just leave: the entry field opens as",
    "  \"save before quit:\", prefilled with the current path, so Enter",
    "  alone saves where the file came from and closes.",
    "",
    "      Enter   save and quit (a failed save quits nothing)",
    "      Ctrl+Q  quit anyway, discarding the changes",
    "      Esc     never mind - back to editing",
    "",
    "  Alt+F4 asks the same question. That is the window system being",
    "  polite: the first press is a REQUEST an app may answer, and only",
    "  a second press within five seconds is an order.",
    "",
    "THE ENTRY FIELD",
    "  Open, Save As, find, and save-before-quit share one field below",
    "  the buttons; the label names what it currently means. Enter acts,",
    "  Esc cancels.",
    "  Ctrl+V pastes into it too - one line's worth, since it is one line.",
    "",
    "THE CLIPBOARD IS A FILE",
    "  Copy and cut write /sys/clipboard; paste reads it. It is the",
    "  SYSTEM's one clipboard, so husk shares it:",
    "",
    "      grep panic /home/os64.log > /sys/clipboard    (copy)",
    "      cat /sys/clipboard                            (paste)",
    "",
    "  Text copied here pastes into a pipeline, and vice versa. You can",
    "  copy from this help page too - a page you cannot edit is still a",
    "  page worth quoting.",
    "",
    "NOTES",
    "  No word wrap yet - long lines scroll horizontally (the bar below).",
    "  A * after the filename means unsaved changes.",
    "  A file larger than memory is refused, with both numbers.",
    "",
    "CREDITS",
    "  scribe was designed and written by Claude Fable 5, who grew the",
    "  toolkit it stands on - textview, scrollbar, textfield - so that",
    "  the NEXT app costs less than this one did.",
    "  Save-before-quit, and the Alt+F4 answer behind it, by Claude",
    "  Opus 5 - because Chris noticed that ^Q threw away work without",
    "  so much as asking, and that one Alt+F4 did nothing at all.",
    "  Built for Chris. Built for os64: our OS.",
    "",
    "  Lineage: ed begat quill; Bravo begat scribe.",
};
#define HELP_LINE_COUNT (sizeof(kHelpLines) / sizeof(kHelpLines[0]))

static size_t help_line_count(void *user)
{
    (void)user;
    return HELP_LINE_COUNT;
}

static const char *help_line(void *user, size_t idx, size_t *len)
{
    (void)user;
    if (idx >= HELP_LINE_COUNT) {
        *len = 0;
        return "";
    }
    *len = os64_strlen(kHelpLines[idx]);
    return kHelpLines[idx];
}

// Editing ops all NULL: the textview refuses edits and becomes a viewer —
// no special case anywhere, which is the entire point of the vtable.
static const os64_ui_textbuf_t kHelpBuf = {
    .user = NULL,
    .line_count = help_line_count,
    .line = help_line,
};

// ── status line ─────────────────────────────────────────────────────────────

static void status_show(const char *msg)
{
    os64_strcopy(g.status_text, sizeof(g.status_text), msg);
    os64_ui_mark_dirty(&g.ui, &g.status);
}

static void status_refresh(void)
{
    // ASCII dash on purpose: these strings render through the 8x16 PSF face,
    // where a UTF-8 em-dash is three glyphs of mojibake (the FILE's bytes may
    // be anything — that's honest; scribe's own chrome must not be).
    // The ^G hint lives here permanently — the startup hint vanished with
    // the first file load and took discoverability with it (his find).
    os64_snprintf(g.status_text, sizeof(g.status_text),
                  "%s%s - %lu lines  (^G help)",
                  g.path[0] ? g.path : "(unnamed)",
                  g.buf.dirty ? " *" : "",
                  (unsigned long)g.buf.count);
    os64_ui_mark_dirty(&g.ui, &g.status);
}

// ── layout ──────────────────────────────────────────────────────────────────
// Manual bounds, recomputed on resize — layout is a call the app makes.
// Row 1: status | Open | Save | Save As.  Row 2 (only in a field mode):
// mode label | entry field.  Body: textview + scrollbar.

// Every rectangle scribe's window is made of. Computed by one function and
// then either ASSIGNED (startup, a window resize) or STAGED (a font change,
// where nothing may move until the whole adoption has succeeded) — one
// arithmetic, so the window a font change stages is the window a resize
// would have drawn.
typedef struct
{
    os64_gui_rect_t status, btn_open, btn_save, btn_saveas;
    os64_gui_rect_t mode_label, field, view, scroll, hscroll;
    bool bar2;
} scribe_layout_t;

// A button is as wide as its caption says, plus the padding a button
// carries on each side. The old widths were 60/60/84 pixels, written for an
// 8x16 cell; "Save As" at a 24-pixel face is wider than that on its own.
static os64_font_status_t button_width(os64_ui_t *ui, const char *caption,
                                       int32_t *out)
{
    int32_t tw = 0;
    os64_font_status_t status =
        os64_ui_text_measure(ui, OS64_FONT_ROLE_UI, caption, os64_strlen(caption), &tw);
    if (status == OS64_FONT_OK)
        *out = tw + 4 * ui->theme.pad;
    return status;
}

// THE WINDOW, UNDER WHATEVER FACE THE CALLS BELOW ARE REPORTING. Inside a
// font transaction that is the candidate; everywhere else it is the face the
// window is wearing. It never touches a live widget, so the planner can ask
// it a question it might not like the answer to.
//
// What gives when a face gets bigger: the STATUS LINE, which is the one
// thing on the top row whose text can be cut short without losing a
// control. The buttons keep their measured widths and the body keeps at
// least one row. When even that will not fit, the answer is LIMIT — a
// window that hides its own Save button to make a font fit has not fit it.
static os64_font_status_t compute_layout(os64_ui_t *ui, scribe_layout_t *out)
{
    const os64_ui_theme_t *t = &ui->theme;
    os64_gui_rect_t area = os64_ui_widget_planned_bounds(&g.root);
    int32_t W = area.w, H = area.h;
    int32_t pad = t->pad, gap = t->gap;
    int32_t bh = os64_ui_control_min_height(ui);

    int32_t bw_open = 0, bw_save = 0, bw_saveas = 0;
    os64_font_status_t status;
    if ((status = button_width(ui, "Open", &bw_open)) != OS64_FONT_OK ||
        (status = button_width(ui, "Save", &bw_save)) != OS64_FONT_OK ||
        (status = button_width(ui, "Save As", &bw_saveas)) != OS64_FONT_OK)
        return status;

    int32_t bx = W - pad - bw_saveas - bw_save - bw_open - 2 * gap;
    if (bx - 2 * pad < 0)
        return OS64_FONT_LIMIT;         // the buttons alone overrun the window
    out->status     = (os64_gui_rect_t){ pad, pad, bx - 2 * pad, bh };
    out->btn_open   = (os64_gui_rect_t){ bx, pad, bw_open, bh };
    out->btn_save   = (os64_gui_rect_t){ bx + bw_open + gap, pad, bw_save, bh };
    out->btn_saveas = (os64_gui_rect_t){ bx + bw_open + bw_save + 2 * gap,
                                         pad, bw_saveas, bh };

    int32_t y = pad + bh + gap;
    out->bar2 = (g.mode != MODE_NONE);
    if (out->bar2) {
        // MEASURE the label instead of assuming it (2026-08-23): a flat
        // allowance fits "open:" and guillotines "save before quit:". A
        // width derived from the text cannot go stale when the next mode is
        // named. Capped at a third of the window so a long label can never
        // starve the field it is labelling.
        const char *label = g.mode_label.text ? g.mode_label.text : "";
        int32_t lw = 0;
        status = os64_ui_text_measure(ui, OS64_FONT_ROLE_UI, label,
                                      os64_strlen(label), &lw);
        if (status != OS64_FONT_OK)
            return status;
        lw += pad;
        if (lw > W / 3)
            lw = W / 3;
        out->mode_label = (os64_gui_rect_t){ pad, y, lw, bh };
        out->field      = (os64_gui_rect_t){ pad + lw + gap, y,
                                             W - 2 * pad - lw - gap, bh };
        y += bh + gap;
    }

    // The body has to show at least one line of the DOCUMENT face.
    int32_t row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT);
    int32_t body_h = H - y - pad - t->scroll_w - 2;
    if (body_h < row + 4)
        return OS64_FONT_LIMIT;
    int32_t body_w = W - 2 * pad - t->scroll_w - 2;
    out->view   = (os64_gui_rect_t){ pad, y, body_w, body_h };
    out->scroll = (os64_gui_rect_t){ W - pad - t->scroll_w, y, t->scroll_w, body_h };
    // The horizontal bar under the text, stopping short of the vertical
    // bar's column — the classic empty corner, left empty on purpose.
    out->hscroll = (os64_gui_rect_t){ pad, y + body_h + 2, body_w, t->scroll_w };
    return OS64_FONT_OK;
}

static void place(os64_ui_widget_t *w, os64_gui_rect_t r, bool staged)
{
    if (staged)
        os64_ui_widget_stage_bounds(w, r);
    else
        w->bounds = r;
}

static void apply_layout(const scribe_layout_t *l, bool staged)
{
    place(&g.status, l->status, staged);
    place(&g.btn_open, l->btn_open, staged);
    place(&g.btn_save, l->btn_save, staged);
    place(&g.btn_saveas, l->btn_saveas, staged);
    if (l->bar2) {
        place(&g.mode_label, l->mode_label, staged);
        place(&g.field.w, l->field, staged);
    }
    place(&g.view.w, l->view, staged);
    place(&g.scroll.w, l->scroll, staged);
    place(&g.hscroll.w, l->hscroll, staged);
}

// The live layout. A measurement refused here leaves every widget where it
// was: the window is already wearing a face whose captions measured fine
// when it was adopted, so a refusal now is transient, and the next resize
// asks again rather than laying the window out from numbers it does not have.
static void layout(os64_ui_t *ui)
{
    scribe_layout_t l;
    if (compute_layout(ui, &l) != OS64_FONT_OK)
        return;
    g.mode_label.hidden = g.field.w.hidden = !l.bar2;
    apply_layout(&l, false);
}

static void on_resize(os64_ui_t *ui)
{
    layout(ui);
}

// Defined with the scrollbar wiring below.
static void sync_scrollbar(void);

// ── adopting a face ─────────────────────────────────────────────────────────
// Scribe's half of a font change. libui stages the widgets' text; this stages
// the WINDOW — every rectangle the new face implies — so the textview
// prepares runs for the lines it will be able to show rather than the ones it
// shows now. Nothing live moves until commit.
//
// The horizontal extent is not measured here. Every extent scribe holds is
// in the pixels of the face being retired, so commit starts the one on stage
// over, from the rows the new face shows — and those are exactly the runs
// libui staged beside this plan, so reading them back lays nothing out and
// allocates nothing, as a commit must not.

typedef struct
{
    scribe_layout_t layout;
} scribe_font_plan_t;

static os64_font_status_t plan_font(os64_ui_t *ui, void *user, void **out)
{
    (void)user;
    scribe_font_plan_t *p = os64_malloc(sizeof(*p));
    if (!p)
        return OS64_FONT_NO_MEMORY;
    os64_font_status_t status = compute_layout(ui, &p->layout);
    if (status != OS64_FONT_OK) {
        os64_free(p);
        return status;
    }
    apply_layout(&p->layout, true);
    *out = p;
    return OS64_FONT_OK;
}

// libui has already applied the staged rectangles and swapped in the runs;
// what is left is scribe's own derived state.
static void commit_font(os64_ui_t *ui, void *user, void *plan)
{
    (void)ui; (void)user;
    scribe_font_plan_t *p = plan;
    g.mode_label.hidden = g.field.w.hidden = !p->layout.bar2;
    g.max_width = 0;
    if (g.help_active) {
        // The document waiting behind help has shown no row in this face,
        // and its saved scroll is in the retired face's pixels: both are
        // found again when help closes.
        g.saved_max_width = 0;
        g.saved_face_stale = true;
    }
    os64_free(p);
    sync_scrollbar();
}

static void discard_font(os64_ui_t *ui, void *user, void *plan)
{
    (void)ui; (void)user;
    os64_free(plan);
}

// Relayout + full repaint — the field bar appearing/vanishing moves the
// body, so a widget-rect union isn't enough.
static void relayout_all(void)
{
    layout(&g.ui);
    g.ui.dirty = g.root.bounds;
    g.ui.any_dirty = true;
}

// ── scrollbar <-> view wiring ───────────────────────────────────────────────

static void sync_scrollbar(void)
{
    int32_t rows = os64_ui_textview_rows(&g.view, &g.ui.theme);
    int32_t width = os64_ui_textview_width(&g.view);
    // Count through the view's OWN vtable, not g.buf directly — the help
    // page swaps a different document in, and the bars must follow it.
    int64_t count = (int64_t)g.view.buf->line_count(g.view.buf->user);
    os64_ui_scrollbar_set(&g.ui, &g.scroll, count, rows, (int64_t)g.view.top);
    // The horizontal bar counts PIXELS now, because a proportional face
    // has no columns to count. Its units are the view's own. It reaches the
    // widest row shown so far: the rows on screen join the extent here, laid
    // out in the slots the paint draws from. A refusal leaves the extent as
    // it was. The caret's row counts too, on screen or not: a long line the
    // caret is on shows a window, and the bar reaches that window; the rest
    // of the line is unknown, and reached by moving the caret, which moves
    // the window. Nothing else in the document is laid out for the bar.
    int64_t shown = 0;
    if (os64_ui_textview_shown_width(&g.ui, &g.view, &shown) == OS64_FONT_OK &&
        shown > g.max_width)
        g.max_width = shown;
    int64_t total = g.max_width;
    if (os64_ui_textview_row_width(&g.ui, &g.view, g.view.cur_line, &shown) ==
            OS64_FONT_OK && shown > total)
        total = shown;
    os64_ui_scrollbar_set(&g.ui, &g.hscroll, total, width, g.view.left_px);
}

// The document's extent, measured afresh after something replaced it
// wholesale: it starts over from the rows the view shows, and grows as more
// of them are shown.
static void measure_document(void)
{
    int64_t shown = 0;
    g.max_width = 0;
    if (os64_ui_textview_shown_width(&g.ui, &g.view, &shown) == OS64_FONT_OK)
        g.max_width = shown;
}

static void view_changed(os64_ui_textview_t *tv, void *user)
{
    (void)tv; (void)user;
    // An edit widens rows that are on screen, or that the view is about to
    // move to — and sync_scrollbar measures the rows shown, here and again
    // when the view moves. The extent GROWS and never shrinks, so a line
    // that was the widest and then got shorter leaves the bar too long
    // until the next load or font change. The alternative is re-measuring
    // every line on every keystroke.
    status_refresh();       // dirty star + line count stay honest
    sync_scrollbar();
}

static void view_moved(os64_ui_textview_t *tv, void *user)
{
    (void)tv; (void)user;
    sync_scrollbar();
}

static void scrolled(os64_ui_scrollbar_t *sb, void *user)
{
    (void)user;
    os64_ui_textview_scroll_to(&g.ui, &g.view, (size_t)sb->pos);
}

static void hscrolled(os64_ui_scrollbar_t *sb, void *user)
{
    (void)user;
    os64_ui_textview_scroll_left(&g.ui, &g.view, sb->pos);
}

// ── the entry field's modes ─────────────────────────────────────────────────

static void leave_mode(void);   // defined with its twin below; help_toggle
                                // closes the field when the help page opens

// Leave the help page, restoring the document's viewport and selection
// exactly as they were — help is a detour, not a destination.
static void help_leave(void)
{
    if (!g.help_active)
        return;
    g.help_active = false;
    g.view.buf = &g.textbuf;
    g.view.top = g.saved_top;
    g.view.left_px = g.saved_left;
    g.view.cur_line = g.saved_cur_line;
    g.view.cur_col = g.saved_cur_col;
    g.view.sel = g.saved_sel;
    g.view.sel_line = g.saved_sel_line;
    g.view.sel_col = g.saved_sel_col;
    g.max_width = g.saved_max_width;
    g.view.win_line = g.saved_win_line;
    g.view.win_from = g.saved_win_from;
    if (g.saved_face_stale) {
        // The face changed while help was on stage, so the saved scroll is
        // in pixels of a face nobody is wearing — and the saved budget is
        // that face's, while the one that arrived proved its own. The caret
        // and the selection are BYTE positions and came back exactly; the
        // scroll is found again around them in the window just restored,
        // and the extent grows again from the rows shown.
        g.saved_face_stale = false;
        g.view.left_px = 0;
        os64_ui_textview_goto(&g.ui, &g.view, g.view.cur_line, g.view.cur_col, false);
        g.view.sel = g.saved_sel;
    } else {
        g.view.win_budget = g.saved_win_budget;
    }
    status_refresh();
    sync_scrollbar();
    os64_ui_mark_dirty(&g.ui, &g.view.w);
}

static void help_toggle(void)
{
    if (g.help_active) {
        help_leave();
        return;
    }
    // The entry field closes when help opens — a find executed against a
    // document the eyes can't see (the view holds the help page) would jump
    // the viewport somewhere invisible, and restoring on leave would then
    // silently discard the jump. One document on stage at a time.
    if (g.mode != MODE_NONE)
        leave_mode();
    g.saved_top = g.view.top;
    g.saved_left = g.view.left_px;
    g.saved_cur_line = g.view.cur_line;
    g.saved_cur_col = g.view.cur_col;
    g.saved_sel = g.view.sel;
    g.saved_sel_line = g.view.sel_line;
    g.saved_sel_col = g.view.sel_col;
    g.saved_max_width = g.max_width;
    g.saved_win_line = g.view.win_line;
    g.saved_win_from = g.view.win_from;
    g.saved_win_budget = g.view.win_budget;

    g.help_active = true;
    g.view.buf = &kHelpBuf;
    g.view.top = 0;
    g.view.left_px = 0;
    g.view.cur_line = g.view.cur_col = 0;
    g.view.sel = false;
    g.max_width = 0;            // the page's own, grown as its rows show
    status_show("help - Esc or ^G returns");
    sync_scrollbar();
    os64_ui_mark_dirty(&g.ui, &g.view.w);
}

static void enter_mode(field_mode_t m, const char *label, const char *prefill)
{
    help_leave();   // a file verb always means the DOCUMENT, never the help
    g.mode = m;
    g.mode_label.text = label;
    // The bar is laid out FIRST: that is what shows the field, and a hidden
    // widget cannot take focus; and it is what decides the field's width —
    // the label beside it is measured — which the prefill's scroll has to
    // be worked out against, or the caret at the end of a long path lands
    // past the field's edge.
    relayout_all();
    os64_ui_textfield_set(&g.ui, &g.field, prefill);
    os64_ui_set_focus(&g.ui, &g.field.w);
}

static void leave_mode(void)
{
    g.mode = MODE_NONE;
    os64_ui_set_focus(&g.ui, &g.view.w);
    relayout_all();
}

// ── file verbs ──────────────────────────────────────────────────────────────

static void do_load(const char *path)
{
    char err[SCRIBE_STATUS_MAX];
    int rc = sbuf_load(&g.buf, path, err, sizeof(err));
    if (rc < 0) {
        status_show(err);
        return;
    }
    os64_strcopy(g.path, sizeof(g.path), path);
    g.view.top = 0;
    g.view.left_px = 0;
    g.view.cur_line = g.view.cur_col = 0;
    g.view.sel = false;
    measure_document();
    leave_mode();
    if (rc == 1)
        status_show("new file - Save creates it");
    else
        status_refresh();
    sync_scrollbar();
    os64_ui_mark_dirty(&g.ui, &g.view.w);
}

// Returns whether the bytes actually reached the disk. The caller that quits
// on success (request_quit's prompt) NEEDS that answer: quitting after a
// failed write is the one bug an editor is never forgiven for, and a `void`
// here would have made it the easy thing to write. A failure deliberately
// leaves the field up with its error in the status line, so the next thing
// the user types is a different path.
static bool do_save(const char *path)
{
    char err[SCRIBE_STATUS_MAX];
    if (sbuf_save(&g.buf, path, err, sizeof(err)) < 0) {
        status_show(err);
        return false;
    }
    os64_strcopy(g.path, sizeof(g.path), path);
    leave_mode();
    status_refresh();
    return true;
}

// ── quitting, which is a question when there is something to lose ───────────
//
// Ctrl+Q used to end the program on the spot, dirty buffer and all, on the
// argument that the status line's `*` had been the warning. It was not much
// of one (Chris, 2026-08-23), and Alt+F4 made it worse: the window system
// asks the app first and only kills on the SECOND press, so scribe — which
// answered neither — could be closed by any two presses and by no single one.
//
// Both verbs come here now, and both mean the same thing: with nothing
// pending, go; with changes, ASK. The question is the field the user already
// knows — the same one Open and Save As use — prefilled with the current path
// so Enter alone is "save where it came from".
//
//   Enter   save and quit (a FAILED save quits nothing; the field stays up)
//   ^Q      quit anyway, discarding — the escape hatch the old behavior was
//   Esc     never mind, back to editing
//
// "Press the quit key a second time to mean it" is deliberately the window
// system's own grammar one level down: Alt+F4 asks once and escalates on the
// repeat, and so does this. Two doors, one habit.
static void request_quit(void)
{
    // Already asking? Then this is the second press, and it means it.
    if (g.mode == MODE_QUITSAVE) {
        g.running = false;
        return;
    }
    if (!g.buf.dirty) {
        g.running = false;
        return;
    }
    // enter_mode leaves the help page for us — being asked about a document
    // you cannot see would be a poor question.
    enter_mode(MODE_QUITSAVE, "save before quit:", g.path);
    // Short enough to fit beside the buttons — the status widget's width ends
    // where Open begins, so a longer line simply gets cut off there. The full
    // rule (including Esc) lives on the help page, which is where a sentence
    // that long belongs.
    status_show("unsaved changes - Enter saves, ^Q discards");
}

// ── find ────────────────────────────────────────────────────────────────────
// Linear scan from just past the cursor, wrapping once — SCRIBE.md's
// promotion: "finding things in os64.log" IS this feature.

static bool find_in_line(const char *hay, size_t hlen, size_t from,
                         const char *needle, size_t nlen, size_t *at)
{
    if (nlen == 0 || nlen > hlen)
        return false;
    for (size_t i = from; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && hay[i + j] == needle[j])
            j++;
        if (j == nlen) {
            *at = i;
            return true;
        }
    }
    return false;
}

static void do_find(const char *needle)
{
    size_t nlen = os64_strlen(needle);
    if (nlen == 0)
        return;
    os64_strcopy(g.find_text, sizeof(g.find_text), needle);

    size_t total = g.buf.count;
    size_t line = g.view.cur_line;
    size_t col = g.view.cur_col + 1;   // past the cursor, so Enter advances
    for (size_t seen = 0; seen <= total; seen++, col = 0) {
        size_t li = (line + seen) % total;
        size_t len, at;
        const char *ln = g.textbuf.line(g.textbuf.user, li, &len);
        size_t from = (seen == 0) ? (col > len ? len : col) : 0;
        if (find_in_line(ln, len, from, needle, nlen, &at)) {
            os64_ui_textview_select(&g.ui, &g.view, li, at, li, at + nlen);
            char msg[SCRIBE_STATUS_MAX];
            os64_snprintf(msg, sizeof(msg), "found at line %lu",
                          (unsigned long)(li + 1));
            status_show(msg);
            return;
        }
    }
    status_show("not found");
}

// ── widget callbacks ────────────────────────────────────────────────────────

static void field_submit(os64_ui_textfield_t *tf, void *user)
{
    (void)user;
    switch (g.mode) {
    case MODE_OPEN:   if (tf->buf[0]) do_load(tf->buf); break;
    case MODE_SAVEAS: if (tf->buf[0]) (void)do_save(tf->buf); break;
    case MODE_FIND:   do_find(tf->buf); break;   // stays open: Enter = next
    case MODE_QUITSAVE:
        // Only a save that REACHED THE DISK earns the exit. Anything else
        // leaves the prompt standing with the reason in the status line.
        if (tf->buf[0] == '\0')
            status_show("name a file to save to, or ^Q to quit without saving");
        else if (do_save(tf->buf))
            g.running = false;
        break;
    default: break;
    }
}

static void field_cancel(os64_ui_textfield_t *tf, void *user)
{
    (void)tf; (void)user;
    // Cancelling the save-before-quit prompt cancels the QUIT as well, which
    // is what Esc means everywhere else in this program: never mind. The way
    // out without saving is ^Q, which request_quit spells out in the status
    // line while the prompt is up.
    leave_mode();
}

// Alt+F4. The window system delivers this as a REQUEST — the window is the
// app's, and an editor with unsaved work gets to answer (GRAPHICS.md's chord
// table). libui routes it here when on_close is set; without one it would set
// ui->quit instead, and scribe's hand-rolled loop was reading neither, which
// is why a single Alt+F4 used to do nothing at all and a second one arrived
// as SIGTERM. Same door as ^Q, deliberately: one habit, two keys.
static void on_close_request(os64_ui_t *ui)
{
    (void)ui;
    request_quit();
}

static void click_open(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    enter_mode(MODE_OPEN, "open:", "");
}

static void click_save(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    help_leave();   // Save means the document, wherever the eyes were
    if (g.path[0])
        do_save(g.path);
    else
        enter_mode(MODE_SAVEAS, "save as:", "");
}

static void click_saveas(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    enter_mode(MODE_SAVEAS, "save as:", g.path);
}

// ── app-level shortcuts, intercepted BEFORE dispatch ────────────────────────
// The driver already turned Ctrl+letter into its control code (1963's
// design, working). Scribe owns document shortcuts; textfields handle their
// own selection and clipboard chords. Returns true for an app command.

static bool app_shortcut(const os64_gui_event_t *ev)
{
    if (ev->type != OS64_GUI_EVENT_KEY_DOWN)
        return false;
    // Esc leaves the help page from anywhere — before the widgets see it,
    // because the textview would spend it on clearing a selection instead.
    // os64_ui_key_is_esc, never a raw scancode compare: the Esc key speaks
    // two dialects (PS/2 0x01, HID 0x29) and a burst's ESC byte speaks
    // neither — the library predicate knows all of that so apps don't.
    if (g.help_active && os64_ui_key_is_esc(ev)) {
        help_leave();
        return true;
    }
    if (!(ev->key.modifiers & OS64_GUI_MOD_CTRL))
        return false;
    switch (ev->key.ascii) {
    // The clipboard trio. libui owns the mechanism (it knows the selection
    // and the buffer); scribe owns the textview's keys. Field modes leave
    // copy and cut to the focused field. The clipboard is /sys/clipboard, the
    // system's one snarf buffer, which is why what leaves here also arrives
    // in `cat /sys/clipboard` (CLIPBOARD.md).
    case 0x03: {  // Ctrl+C — copy
        if (g.mode != MODE_NONE)
            return false;           // the field owns the keyboard right now
        int64_t n = os64_ui_textview_copy(&g.view);
        char msg[SCRIBE_STATUS_MAX];
        if (n > 0) {
            os64_snprintf(msg, sizeof(msg), "copied %ld bytes  (^G help)",
                          (long)n);
            status_show(msg);
        } else if (n < 0) {
            // The kernel already said WHY on the console (a copy over the
            // clipboard's ceiling announces itself there); this line is so
            // the person watching the window knows nothing was copied.
            status_show("copy refused - clipboard unchanged");
        } else {
            status_show("nothing selected");
        }
        return true;
    }
    case 0x18: {  // Ctrl+X — cut (copy first; a failed copy cuts nothing)
        if (g.mode != MODE_NONE)
            return false;
        if (os64_ui_textview_cut(&g.ui, &g.view))
            status_show("cut  (^G help)");
        else
            status_show("nothing cut");
        return true;
    }
    case 0x16:  // Ctrl+V — paste
        if (g.mode != MODE_NONE) {
            // A path or a search term, pasted into the field it belongs in.
            os64_ui_textfield_paste(&g.ui, &g.field);
            return true;
        }
        // A multi-line paste widens rows the way typing does: on_change
        // measures those on screen, and the rest join the bar as they show.
        if (os64_ui_textview_paste(&g.ui, &g.view))
            os64_ui_mark_dirty(&g.ui, &g.view.w);
        return true;

    case 0x07:  // Ctrl+G — the guide (his commissioning order: a help
                // screen "with all the normal stuff, including credits")
        help_toggle();
        return true;
    case 0x13:  // Ctrl+S
        click_save(NULL, NULL);
        return true;
    case 0x06:  // Ctrl+F
        enter_mode(MODE_FIND, "find:", g.find_text);
        return true;
    case 0x0f:  // Ctrl+O
        enter_mode(MODE_OPEN, "open:", "");
        return true;
    case 0x11:  // Ctrl+Q — asks first when there is something to lose, and a
                // second press while it is asking discards (see request_quit).
                // Deliberately NOT guarded by `g.mode != MODE_NONE` like the
                // clipboard trio above: the whole point is that ^Q reaches
                // through its own prompt.
        request_quit();
        return true;
    default:
        return false;
    }
}

// ── main ────────────────────────────────────────────────────────────────────

// ── what a fixture may read ─────────────────────────────────────────────────

bool scribe_help_active(void) { return g.help_active; }
bool scribe_dirty(void) { return g.buf.dirty; }
size_t scribe_line_count(void) { return g.buf.count; }
const char *scribe_line(size_t index, size_t *len)
{
    return g.textbuf.line(g.textbuf.user, index, len);
}
os64_ui_textview_t *scribe_view(void) { return &g.view; }
int64_t scribe_extent(void) { return g.max_width; }
void scribe_toggle_help(void) { help_toggle(); }
void scribe_open(const char *path) { do_load(path); }
bool scribe_save_as(const char *path) { return do_save(path); }

static const char *scribe_build(void);   // after scribe_main, which calls it

int scribe_main(int argc, char **argv, const scribe_hooks_t *hooks)
{
    const char *arg_path = (argc > 1) ? argv[1] : NULL;

    // The window title gets the file's NAME, not its path — partly Bravo
    // manners, mostly the ABI: a title longer than OS64_GUI_TITLE_MAX is
    // REFUSED at the boundary (never truncated — the house convention for
    // strings crossing ring 3), and "scribe - /fat/boot/limine/limine.conf"
    // was the day-one casualty: the first deep path typed at husk earned
    // "window create failed (-3)" before the file was ever opened
    // (2026-08-21, the HD-boot evening). The full path lives in the status
    // line, where it always did. snprintf clamps a still-too-long basename
    // into the field rather than bouncing the window — scribe's own title
    // is scribe's to shorten.
    const char *base = arg_path;
    if (base != NULL) {
        for (const char *p = arg_path; *p; p++)
            if (*p == '/')
                base = p + 1;
        if (*base == '\0')
            base = arg_path;   // trailing slash — show the path, let open fail loudly
    }
    char title[OS64_GUI_TITLE_MAX];
    os64_snprintf(title, sizeof(title), "scribe%s%s",
                  base ? " - " : "", base ? base : "");

    g.win = os64_gui_window_create(title, 140, 90, 640, 440, 0);
    if (g.win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("scribe: no GUI on this boot\n");
        return 0;
    }
    if (g.win <= 0) {
        os64_hprintf(OS64_STDERR, "scribe: window create failed (%ld)\n",
                     (long)g.win);
        return 1;
    }
    if (os64_draw_ctx_init(&g.ctx, g.win) != 0) {
        os64_hprintf(OS64_STDERR, "scribe: no surface\n");
        return 1;
    }

    const char *refused = scribe_build();
    if (refused) {
        os64_hprintf(OS64_STDERR, "scribe: %s\n", refused);
        return 1;
    }

    if (arg_path)
        do_load(arg_path);
    else
        status_show("(unnamed) - ^G help  ^S save  ^F find  ^O open  ^Q quit");
    sync_scrollbar();

    if (!hooks) (void)os64_ui_font_follow(&g.ui);
    if (hooks && hooks->ready)
        hooks->ready(&g.ui, hooks->user);

    // The canonical loop, with the app's shortcut check ahead of dispatch —
    // scribe owns its loop instead of using os64_ui_run for exactly this.
    g.running = true;
    os64_ui_paint(&g.ui);
    // `!g.ui.quit` as well as `g.running`: ui.h's contract says an app with
    // its own loop must check it, because libui sets it for any close request
    // an app has not claimed. scribe HAS claimed it (scribe_build sets
    // on_close), so the flag should never fire — which is exactly why it is
    // cheap to honor and expensive to omit. Not honoring it is the bug this
    // slice came from.
    while (g.running && !g.ui.quit) {
        os64_gui_event_t ev;
        int64_t rc = os64_gui_event_wait(g.win, &ev);
        if (rc != 1)
            break;
        do {
            if (hooks && hooks->key && ev.type == OS64_GUI_EVENT_KEY_DOWN &&
                hooks->key(&g.ui, &ev, hooks->user))
                continue;
            if (!app_shortcut(&ev))
                os64_ui_dispatch(&g.ui, &ev);
        } while (g.running && os64_gui_event_poll(g.win, &ev) == 1);
        os64_ui_paint(&g.ui);
    }

    os64_gui_window_destroy(g.win);
    return 0;
}

// The window's contents, on whatever surface g.ctx holds: the buffer, the
// widgets, their layout and the font planner. NULL when it is all standing,
// otherwise what refused. Kept apart from the window so that a host harness
// can build the real Scribe on a canvas of its own.
static const char *scribe_build(void)
{
    if (!sbuf_init(&g.buf))
        return "out of memory";
    g.textbuf = sbuf_textbuf_template;
    g.textbuf.user = &g.buf;

    os64_ui_init(&g.ui, &g.ctx);
    g.ui.on_resize = on_resize;
    g.ui.on_close  = on_close_request;   // Alt+F4 asks; scribe answers

    os64_ui_panel(&g.root);
    g.root.bounds = (os64_gui_rect_t){ 0, 0, (int32_t)g.ctx.surf.width,
                                       (int32_t)g.ctx.surf.height };
    os64_ui_label(&g.status, g.status_text);
    os64_ui_button(&g.btn_open,   "Open",    click_open,   NULL);
    os64_ui_button(&g.btn_save,   "Save",    click_save,   NULL);
    os64_ui_button(&g.btn_saveas, "Save As", click_saveas, NULL);
    os64_ui_label(&g.mode_label, "");
    os64_ui_textfield(&g.field, g.field_buf, sizeof(g.field_buf),
                      field_submit, field_cancel, NULL);
    os64_ui_textview(&g.view, &g.textbuf, view_changed, view_moved, NULL);
    os64_ui_scrollbar(&g.scroll, scrolled, NULL);
    os64_ui_scrollbar(&g.hscroll, hscrolled, NULL);
    g.hscroll.horizontal = true;

    os64_ui_add_child(&g.root, &g.status);
    os64_ui_add_child(&g.root, &g.btn_open);
    os64_ui_add_child(&g.root, &g.btn_save);
    os64_ui_add_child(&g.root, &g.btn_saveas);
    os64_ui_add_child(&g.root, &g.mode_label);
    os64_ui_add_child(&g.root, &g.field.w);
    os64_ui_add_child(&g.root, &g.view.w);
    os64_ui_add_child(&g.root, &g.scroll.w);
    os64_ui_add_child(&g.root, &g.hscroll.w);

    layout(&g.ui);
    os64_ui_set_root(&g.ui, &g.root);
    os64_ui_set_focus(&g.ui, &g.view.w);

    // Scribe's half of any font change: its window's layout, planned against
    // the candidate face before anything live moves. Registration stores the
    // trio in the os64_ui_t and cannot run out of memory; the one refusal is
    // a malformed trio, which these are not.
    if (os64_ui_font_planner(&g.ui, plan_font, commit_font, discard_font,
                             NULL) != OS64_FONT_OK)
        return "font planner refused";
    return NULL;
}
