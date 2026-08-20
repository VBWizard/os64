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
#include "scribe_buf.h"

#define SCRIBE_PATH_MAX  256
#define SCRIBE_STATUS_MAX 96

// What the one entry field currently means. One field, one label naming its
// mode — a menu bar's worth of dialogs in two widgets (his ruling: button
// row; "if we hate it we'll change it").
typedef enum { MODE_NONE, MODE_OPEN, MODE_SAVEAS, MODE_FIND } field_mode_t;

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
    os64_ui_scrollbar_t scroll;

    char path[SCRIBE_PATH_MAX];     // empty = unnamed buffer
    char status_text[SCRIBE_STATUS_MAX];
    char field_buf[SCRIBE_PATH_MAX];
    char find_text[SCRIBE_PATH_MAX];

    field_mode_t mode;
    bool running;
} scribe_t;

static scribe_t g;

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
    os64_snprintf(g.status_text, sizeof(g.status_text), "%s%s - %lu lines",
                  g.path[0] ? g.path : "(unnamed)",
                  g.buf.dirty ? " *" : "",
                  (unsigned long)g.buf.count);
    os64_ui_mark_dirty(&g.ui, &g.status);
}

// ── layout ──────────────────────────────────────────────────────────────────
// Manual bounds, recomputed on resize — layout is a call the app makes.
// Row 1: status | Open | Save | Save As.  Row 2 (only in a field mode):
// mode label | entry field.  Body: textview + scrollbar.

static void layout(os64_ui_t *ui)
{
    const os64_ui_theme_t *t = &ui->theme;
    int32_t W = g.root.bounds.w;
    int32_t H = g.root.bounds.h;
    int32_t pad = t->pad, bh = t->button_h;

    int32_t bw_open = 60, bw_save = 60, bw_saveas = 84;
    int32_t bx = W - pad - bw_saveas - bw_save - bw_open - 2 * t->gap;
    g.status.bounds     = (os64_gui_rect_t){ pad, pad, bx - 2 * pad, bh };
    g.btn_open.bounds   = (os64_gui_rect_t){ bx, pad, bw_open, bh };
    g.btn_save.bounds   = (os64_gui_rect_t){ bx + bw_open + t->gap, pad, bw_save, bh };
    g.btn_saveas.bounds = (os64_gui_rect_t){ bx + bw_open + bw_save + 2 * t->gap,
                                             pad, bw_saveas, bh };

    int32_t y = pad + bh + t->gap;
    bool bar2 = (g.mode != MODE_NONE);
    g.mode_label.hidden = g.field.w.hidden = !bar2;
    if (bar2) {
        int32_t lw = 9 * t->font_w;
        g.mode_label.bounds = (os64_gui_rect_t){ pad, y, lw, bh };
        g.field.w.bounds    = (os64_gui_rect_t){ pad + lw + t->gap, y,
                                                 W - 2 * pad - lw - t->gap, bh };
        y += bh + t->gap;
    }

    int32_t body_h = H - y - pad;
    if (body_h < t->font_h)
        body_h = t->font_h;
    g.view.w.bounds   = (os64_gui_rect_t){ pad, y,
                                           W - 2 * pad - t->scroll_w - 2, body_h };
    g.scroll.w.bounds = (os64_gui_rect_t){ W - pad - t->scroll_w, y,
                                           t->scroll_w, body_h };
}

static void on_resize(os64_ui_t *ui)
{
    layout(ui);
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
    os64_ui_scrollbar_set(&g.ui, &g.scroll, (int64_t)g.buf.count,
                          rows, (int64_t)g.view.top);
}

static void view_changed(os64_ui_textview_t *tv, void *user)
{
    (void)tv; (void)user;
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

// ── the entry field's modes ─────────────────────────────────────────────────

static void enter_mode(field_mode_t m, const char *label, const char *prefill)
{
    g.mode = m;
    g.mode_label.text = label;
    os64_ui_textfield_set(&g.ui, &g.field, prefill);
    os64_ui_set_focus(&g.ui, &g.field.w);
    relayout_all();
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
    g.view.left = 0;
    g.view.cur_line = g.view.cur_col = 0;
    g.view.sel = false;
    leave_mode();
    if (rc == 1)
        status_show("new file - Save creates it");
    else
        status_refresh();
    sync_scrollbar();
    os64_ui_mark_dirty(&g.ui, &g.view.w);
}

static void do_save(const char *path)
{
    char err[SCRIBE_STATUS_MAX];
    if (sbuf_save(&g.buf, path, err, sizeof(err)) < 0) {
        status_show(err);
        return;
    }
    os64_strcopy(g.path, sizeof(g.path), path);
    leave_mode();
    status_refresh();
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
    case MODE_SAVEAS: if (tf->buf[0]) do_save(tf->buf); break;
    case MODE_FIND:   do_find(tf->buf); break;   // stays open: Enter = next
    default: break;
    }
}

static void field_cancel(os64_ui_textfield_t *tf, void *user)
{
    (void)tf; (void)user;
    leave_mode();
}

static void click_open(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    enter_mode(MODE_OPEN, "open:", "");
}

static void click_save(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
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
// design, working); the widgets ignore control bytes on purpose, so these
// are the app's to claim. Returns true when the event was a command.

static bool app_shortcut(const os64_gui_event_t *ev)
{
    if (ev->type != OS64_GUI_EVENT_KEY_DOWN ||
        !(ev->key.modifiers & OS64_GUI_MOD_CTRL))
        return false;
    switch (ev->key.ascii) {
    case 0x13:  // Ctrl+S
        click_save(NULL, NULL);
        return true;
    case 0x06:  // Ctrl+F
        enter_mode(MODE_FIND, "find:", g.find_text);
        return true;
    case 0x0f:  // Ctrl+O
        enter_mode(MODE_OPEN, "open:", "");
        return true;
    case 0x11:  // Ctrl+Q — quits regardless; the status line was the warning
        g.running = false;
        return true;
    default:
        return false;
    }
}

// ── main ────────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    const char *arg_path = (argc > 1) ? argv[1] : NULL;

    char title[SCRIBE_PATH_MAX + 16];
    os64_snprintf(title, sizeof(title), "scribe%s%s",
                  arg_path ? " - " : "", arg_path ? arg_path : "");

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

    if (!sbuf_init(&g.buf)) {
        os64_hprintf(OS64_STDERR, "scribe: out of memory\n");
        return 1;
    }
    g.textbuf = sbuf_textbuf_template;
    g.textbuf.user = &g.buf;

    os64_ui_init(&g.ui, &g.ctx);
    g.ui.on_resize = on_resize;

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

    os64_ui_add_child(&g.root, &g.status);
    os64_ui_add_child(&g.root, &g.btn_open);
    os64_ui_add_child(&g.root, &g.btn_save);
    os64_ui_add_child(&g.root, &g.btn_saveas);
    os64_ui_add_child(&g.root, &g.mode_label);
    os64_ui_add_child(&g.root, &g.field.w);
    os64_ui_add_child(&g.root, &g.view.w);
    os64_ui_add_child(&g.root, &g.scroll.w);

    layout(&g.ui);
    os64_ui_set_root(&g.ui, &g.root);
    os64_ui_set_focus(&g.ui, &g.view.w);

    if (arg_path)
        do_load(arg_path);
    else
        status_show("(unnamed) - ^S save  ^F find  ^O open  ^Q quit");
    sync_scrollbar();

    // The canonical loop, with the app's shortcut check ahead of dispatch —
    // scribe owns its loop instead of using os64_ui_run for exactly this.
    g.running = true;
    os64_ui_paint(&g.ui);
    while (g.running) {
        os64_gui_event_t ev;
        int64_t rc = os64_gui_event_wait(g.win, &ev);
        if (rc != 1)
            break;
        do {
            if (!app_shortcut(&ev))
                os64_ui_dispatch(&g.ui, &ev);
        } while (g.running && os64_gui_event_poll(g.win, &ev) == 1);
        os64_ui_paint(&g.ui);
    }

    os64_gui_window_destroy(g.win);
    return 0;
}
