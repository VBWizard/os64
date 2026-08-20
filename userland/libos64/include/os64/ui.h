// os64/ui.h — libui, the retained-lite widget toolkit (L2 of LIBDRAW.md).
//
// THE MODEL (LIBDRAW.md, ratified 2026-08-17, built 2026-08-19): a widget is
// bounds + a paint routine + an event handler + children. The app DESCRIBES
// a UI; libui draws it, routes events to it, and repaints what changed. The
// endgame sentence: instantiate a button, don't draw one.
//
// RETAINED-LITE, deliberately (Chris's ruling): widgets are long-lived
// structs the APP owns — stack, static, or its own heap; libui never
// allocates and never frees. What "lite" buys: no scene graph, no z-order
// inside a window (children paint in list order), no incremental min-repaint
// algebra — the dirty model is one union rect, the same choice the
// compositor made at this population size. A foundation, not a dead end:
// every one of those can grow inside this shape without an API break.
//
// THE LOGIC/PAINT SPLIT (the Gnome 2 theme-engine seam, ruled sacred): a
// widget's BEHAVIOR (press tracking, click firing, focus) lives in its
// class's event handler; its LOOK lives in the class's paint function, which
// takes the THEME TABLE as an argument and reads every color, metric, and
// font cell size from it. Swap the paint pointers and you have swapped the
// engine; edit the table and you have re-skinned the stock one. Nothing
// paints from a constant.
//
// THE THEME TABLE (customizability is a design value): ONE struct holds
// every color and metric libui uses — zero scattered constants, enforced by
// review. Defaults are the house look; `/home/theme.conf` overrides any of
// them (key = value, '#' comments — logd.conf's syntax exactly, because
// os64's second config file should not invent a second grammar; and /home
// because the persistence doctrine says the user's look survives rebuilds).
//
// SCOPE HONESTY: this themes WIDGETS. Window chrome (titlebars, borders) is
// painted by the kernel compositor from its own constants — chrome theming
// arrives when decorations go client-side or a chrome-theme channel exists,
// and is deliberately not faked here.
//
// Widgets grow APP-DRIVEN (the doc's rule: the first app that wants a
// textbox is what brings the textbox into being). The starter set below —
// panel, label, button — is what the first fixture demanded. Nothing is
// built speculatively.

#ifndef OS64_UI_H
#define OS64_UI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "os64/draw.h"
#include "os64/gui.h"

// ── The theme table ─────────────────────────────────────────────────────────
// Every color and metric libui reads, in one place. theme.conf keys are the
// field names with '_' spelled '.', e.g. `button.face = 4a90d9`. Colors are
// RGB hex (6 digits, no prefix — `2a62b8`); metrics are decimal pixels.

typedef struct os64_ui_theme
{
    // Colors (XRGB8888; the loader forces the X byte to 0xff — opaque, the
    // only kind of pixel the compositor has until the alpha row lands).
    uint32_t panel_bg;
    uint32_t panel_border;
    uint32_t label_fg;
    uint32_t button_face;
    uint32_t button_face_pressed;
    uint32_t button_border;
    uint32_t button_fg;

    // The text family (textview + textfield, born with scribe 2026-08-20).
    uint32_t text_bg;            // the paper
    uint32_t text_fg;            // the ink
    uint32_t text_sel_bg;        // selection highlight
    uint32_t text_sel_fg;
    uint32_t text_caret;         // the insertion caret
    uint32_t field_bg;
    uint32_t field_fg;
    uint32_t field_border;
    uint32_t field_border_focus; // the border that says "your keys land here"
    uint32_t scroll_track;
    uint32_t scroll_thumb;

    // Metrics (pixels).
    int32_t  pad;        // inner padding: panel edges, button text inset
    int32_t  gap;        // spacing between stacked children
    int32_t  button_h;   // stock button height
    int32_t  scroll_w;   // scrollbar width

    // Font cell (the one embedded PSF1 face — a slot, so the day a second
    // face exists it arrives through the table like everything else).
    int32_t  font_w;
    int32_t  font_h;
} os64_ui_theme_t;

// House defaults, then /home/theme.conf on top (absent file = defaults,
// silently; a PRESENT file with an unknown key or unparsable value is
// complained about by name on the console — the logd.conf discipline: a
// config the user wrote deserves a loud answer, and the log is one Alt+F1
// away).
void os64_ui_theme_init(os64_ui_theme_t *t);

// ── Widgets ─────────────────────────────────────────────────────────────────

typedef struct os64_ui os64_ui_t;
typedef struct os64_ui_widget os64_ui_widget_t;

// The class: one per widget KIND, shared by every instance. paint reads the
// theme and draws the widget INSIDE its bounds (libui clips by contract, not
// by cop — see os64_ui_paint's comment); event returns true if it consumed
// the event. Swapping a class's paint pointer is the theme-engine seam.
typedef struct os64_ui_class
{
    const char *name;
    void (*paint)(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                  const os64_ui_theme_t *t);
    bool (*event)(os64_ui_widget_t *w, os64_ui_t *ui,
                  const os64_gui_event_t *ev);
} os64_ui_class_t;

struct os64_ui_widget
{
    const os64_ui_class_t *cls;
    os64_gui_rect_t bounds;      // content-local, assigned by app or layout
    bool hidden;                 // skipped by paint AND hit-test
    bool pressed;                // button state (owned by button's event fn)
    bool focused;                // mirror of (ui->focus == this) — maintained
                                 // by os64_ui_set_focus so PAINT can know,
                                 // since paint deliberately never sees the ui

    const char *text;            // label/button caption (app-owned storage)

    void (*on_click)(os64_ui_widget_t *w, void *user);
    void *user;

    // Tree links (intrusive — the app owns the nodes, libui only threads
    // them). Children paint and hit-test in list order; last child is
    // "topmost" for a hit, matching paint order.
    os64_ui_widget_t *parent;
    os64_ui_widget_t *first_child;
    os64_ui_widget_t *next_sibling;
};

// ── The UI context (one per window) ─────────────────────────────────────────

struct os64_ui
{
    os64_draw_ctx_t *ctx;        // the window's draw context (app-owned)
    os64_ui_theme_t  theme;
    os64_ui_widget_t *root;
    os64_ui_widget_t *grab;      // widget owning the mouse (button held)
    os64_ui_widget_t *focus;     // key events go here (NULL = dropped)
    os64_gui_rect_t  dirty;      // union of everything needing repaint
    bool             any_dirty;

    // Called after the window changed size, once libui has refreshed the draw
    // context and stretched `root` to the new content area — the app's cue to
    // re-run whatever layout it used in the first place (libui cannot know:
    // layout is a call the app makes, not a policy libui holds). NULL is the
    // honest default for a fixed-layout window; everything still repaints,
    // the widgets just stay where the app put them.
    void (*on_resize)(os64_ui_t *ui);
};

// Bind a UI to a window's draw context and load the theme. root may be set
// afterwards (os64_ui_set_root marks everything dirty).
void os64_ui_init(os64_ui_t *ui, os64_draw_ctx_t *ctx);
void os64_ui_set_root(os64_ui_t *ui, os64_ui_widget_t *root);

// Attach a child at the END of parent's list (paints last = on top).
void os64_ui_add_child(os64_ui_widget_t *parent, os64_ui_widget_t *child);

// Mark a widget (its current bounds) as needing repaint.
void os64_ui_mark_dirty(os64_ui_t *ui, os64_ui_widget_t *w);

// Route one event. Pointer events hit-test to the deepest visible widget
// (with a press grab: DOWN grabs, UP releases and fires on_click if it ends
// over the widget it started on — the ancient button contract, so a drag-off
// cancels). Key events go to `focus`. Returns true if any widget consumed it.
bool os64_ui_dispatch(os64_ui_t *ui, const os64_gui_event_t *ev);

// Repaint whatever is dirty and publish exactly that rect. No-op when clean.
void os64_ui_paint(os64_ui_t *ui);

// The canonical L2 loop, packaged: event_wait → dispatch → paint, until the
// window dies (event_wait error) or `*running` (may be NULL) goes false —
// a widget callback clearing its app's flag is how a Quit button works.
void os64_ui_run(os64_ui_t *ui, int64_t win, volatile bool *running);

// ── Layout (v1: the trivial one) ────────────────────────────────────────────
// Assign bounds to parent's children top-to-bottom inside parent's bounds,
// inset by pad, separated by gap; each child keeps the height already in its
// bounds.h (0 = theme->button_h). Fixed placement needs no helper — the app
// just writes bounds. Grids and springs are future apps' demands.
void os64_ui_stack_vertical(os64_ui_t *ui, os64_ui_widget_t *parent);

// ── Starter widgets (app-driven; see header comment) ────────────────────────
// Initializers, not allocators: the app hands in the struct.
void os64_ui_panel(os64_ui_widget_t *w);                       // themed slab + border
void os64_ui_label(os64_ui_widget_t *w, const char *text);     // one line of text
void os64_ui_button(os64_ui_widget_t *w, const char *text,
                    void (*on_click)(os64_ui_widget_t *, void *),
                    void *user);

// The stock classes, exported so a theme engine can wrap or replace their
// paint while keeping their logic (the split, made concrete).
extern const os64_ui_class_t os64_ui_panel_class;
extern const os64_ui_class_t os64_ui_label_class;
extern const os64_ui_class_t os64_ui_button_class;

// Move key focus. Maintains each widget's `focused` mirror and dirties both
// ends so carets appear and disappear honestly. NULL blurs. Widgets that
// WANT focus call this from their own BUTTON_DOWN handling; nothing focuses
// by accident.
void os64_ui_set_focus(os64_ui_t *ui, os64_ui_widget_t *w);

// ── Stateful widgets (scribe's demands, 2026-08-20) ─────────────────────────
// THE CONTAINER PATTERN: a widget kind that needs state beyond the base
// struct EMBEDS os64_ui_widget_t as its FIRST member and hands libui the
// address of that member — paint/event cast back to the container. The app
// still owns the whole struct (retained-lite's rule survives); libui still
// only ever threads the base. This is the shape every future stateful
// widget follows.

// ── ui_scrollbar — vertical, proportional ───────────────────────────────────
// Units are the APP's (scribe uses lines): `total` things exist, `visible`
// fit the companion view, `pos` is the first visible one. The thumb is the
// proportion made pixel; dragging it, or clicking the track above/below
// (page jumps), moves pos and fires on_scroll. total <= visible = full
// thumb, nothing to do — a scrollbar that vanishes would reflow its
// neighbour, and v1 does not reflow.
typedef struct os64_ui_scrollbar os64_ui_scrollbar_t;
struct os64_ui_scrollbar
{
    os64_ui_widget_t w;          // MUST be first (container pattern)
    int64_t total, visible, pos;
    void (*on_scroll)(os64_ui_scrollbar_t *sb, void *user);
    void *scroll_user;
    int32_t drag_grab;           // px into the thumb where the press landed; -1 idle
};

void os64_ui_scrollbar(os64_ui_scrollbar_t *sb,
                       void (*on_scroll)(os64_ui_scrollbar_t *, void *),
                       void *user);
// Update the three numbers and repaint. Clamps pos into [0, total-visible].
void os64_ui_scrollbar_set(os64_ui_t *ui, os64_ui_scrollbar_t *sb,
                           int64_t total, int64_t visible, int64_t pos);

// ── ui_textfield — one line of editable text ────────────────────────────────
// Born for Save As; really the FORM control every dialog after it needs.
// Storage is the APP's (buf/cap, NUL-kept). Click focuses and places the
// caret; printable keys insert; Backspace/Delete, Left/Right/Home/End move
// and erase; Enter fires on_submit, Esc fires on_cancel. Long content
// scrolls horizontally to keep the caret in view.
typedef struct os64_ui_textfield os64_ui_textfield_t;
struct os64_ui_textfield
{
    os64_ui_widget_t w;          // MUST be first
    char  *buf;                  // app-owned, NUL-terminated
    size_t cap;                  // bytes including the NUL
    size_t len, cursor;
    size_t first;                // first visible byte (horizontal scroll)
    void (*on_submit)(os64_ui_textfield_t *tf, void *user);
    void (*on_cancel)(os64_ui_textfield_t *tf, void *user);
    void *edit_user;
    uint8_t seq;                 // VT100 burst parser state (see ui_text.c)
};

void os64_ui_textfield(os64_ui_textfield_t *tf, char *buf, size_t cap,
                       void (*on_submit)(os64_ui_textfield_t *, void *),
                       void (*on_cancel)(os64_ui_textfield_t *, void *),
                       void *user);
// Replace the content (truncated to cap-1) and put the caret at its end.
void os64_ui_textfield_set(os64_ui_t *ui, os64_ui_textfield_t *tf,
                           const char *text);

// ── ui_textview — a viewport over a text buffer ─────────────────────────────
// THE PLAIN-TEXT RENDERER (SCRIBE.md's format seam): one model+view pair
// among possible several. The view owns the viewport, cursor, and
// selection; the MODEL is the app's, handed in behind this vtable — which
// is what lets a future log viewer bring a read-only buffer and get
// scrolling, selection, and search-jump for free.
typedef struct os64_ui_textbuf
{
    void *user;
    size_t (*line_count)(void *user);                 // always >= 1
    const char *(*line)(void *user, size_t idx, size_t *len);

    // Editing — ALL NULL for a read-only buffer (the view then refuses
    // edits and is a viewer). Column values are BYTE indexes into the line.
    bool (*insert)(void *user, size_t line, size_t col, const char *s, size_t n);
    bool (*erase)(void *user, size_t line, size_t col, size_t n);   // within one line
    bool (*split)(void *user, size_t line, size_t col);             // Enter
    bool (*join)(void *user, size_t line);            // line absorbs line+1
    // Remove whole lines [first, first+count). Exists so deleting a large
    // selection is one memmove of the line table, not count joins — the
    // difference between O(n) and O(n^2) on a select-all in a big log.
    bool (*erase_lines)(void *user, size_t first, size_t count);
} os64_ui_textbuf_t;

typedef struct os64_ui_textview os64_ui_textview_t;
struct os64_ui_textview
{
    os64_ui_widget_t w;          // MUST be first
    const os64_ui_textbuf_t *buf;

    size_t  top;                 // first visible line
    int64_t left;                // first visible VISUAL column (tabs expand)
    size_t  cur_line, cur_col;   // caret; cur_col is a BYTE index
    bool    sel;                 // selection live?
    size_t  sel_line, sel_col;   // the anchor (byte index)
    int64_t goal_vcol;           // remembered column for Up/Down runs

    void (*on_change)(os64_ui_textview_t *tv, void *user);  // buffer edited
    void (*on_view)(os64_ui_textview_t *tv, void *user);    // viewport moved
    void *view_user;
    uint8_t seq;                 // VT100 burst parser state
};

void os64_ui_textview(os64_ui_textview_t *tv, const os64_ui_textbuf_t *buf,
                      void (*on_change)(os64_ui_textview_t *, void *),
                      void (*on_view)(os64_ui_textview_t *, void *),
                      void *user);
// Rows/columns that fit the current bounds under this theme.
int32_t os64_ui_textview_rows(const os64_ui_textview_t *tv,
                              const os64_ui_theme_t *t);
int32_t os64_ui_textview_cols(const os64_ui_textview_t *tv,
                              const os64_ui_theme_t *t);
// Scroll so `top` is the first visible line (clamped); fires on_view.
void os64_ui_textview_scroll_to(os64_ui_t *ui, os64_ui_textview_t *tv,
                                size_t top);
// Place the caret (clamped), optionally keeping/starting a selection from
// the current anchor, and scroll it into view. The search-jump primitive.
void os64_ui_textview_goto(os64_ui_t *ui, os64_ui_textview_t *tv,
                           size_t line, size_t col, bool select);
// Select [sl,sc) .. [el,ec), caret at the end, scrolled into view.
void os64_ui_textview_select(os64_ui_t *ui, os64_ui_textview_t *tv,
                             size_t sl, size_t sc, size_t el, size_t ec);

extern const os64_ui_class_t os64_ui_scrollbar_class;
extern const os64_ui_class_t os64_ui_textfield_class;
extern const os64_ui_class_t os64_ui_textview_class;

#endif // OS64_UI_H
