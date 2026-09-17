// os64/ui.h — libui, the retained-lite widget toolkit (L2 of LIBDRAW.md).
//
// THE MODEL (LIBDRAW.md, ratified 2026-08-17, built 2026-08-19): a widget is
// bounds + a paint routine + an event handler + children. The app DESCRIBES
// a UI; libui draws it, routes events to it, and repaints what changed. The
// endgame sentence: instantiate a button, don't draw one.
//
// RETAINED-LITE, deliberately (Chris's ruling): widgets are long-lived
// structs the APP owns — stack, static, or its own heap; libui never
// allocates or frees widgets. What "lite" buys: no scene graph, no z-order
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
// review. Startup theme.conf follows the shared configuration ladder and
// grammar. The schema validates supported colors and metrics before replacing
// the defaults; font-cell metrics must match the available renderer.
//
// SCOPE HONESTY: this themes WIDGETS. Window chrome (titlebars, borders) is
// painted by the kernel compositor from its own constants — chrome theming
// arrives when decorations go client-side or a chrome-theme channel exists,
// and is deliberately not faked here.
//
// Widgets serve applications and the Appearance Workshop gallery. The
// gallery is a place to develop reusable controls and their interaction
// states before another application needs them (APPEARANCE.md).

#ifndef OS64_UI_H
#define OS64_UI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "os64/draw.h"
#include "os64/gui.h"
#include "os64/font_adopt.h"

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
    uint32_t button_highlight;
    uint32_t button_shadow;
    uint32_t button_face_hover;
    uint32_t hover_border;
    uint32_t focus_ring;
    uint32_t disabled_bg;
    uint32_t disabled_fg;

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

    // The menu family (grootmenu, 2026-08-25 — and any launcher that draws
    // a menu: the colours are here, not in the program, so a dock and the
    // root menu wear the same theme by construction).
    uint32_t menu_bg;
    uint32_t menu_fg;
    uint32_t menu_hi_bg;         // the highlighted row
    uint32_t menu_hi_fg;
    uint32_t menu_sep;           // the separator rule

    // Metrics (pixels).
    int32_t  pad;        // inner padding: panel edges, button text inset
    int32_t  gap;        // spacing between stacked children
    int32_t  button_h;   // stock button height
    int32_t  scroll_w;   // scrollbar width
    int32_t  button_bevel; // 0/1 = flat; 2..4 = relief including the outer border
    int32_t  checkbox_size; // square indicator, clipped to the control bounds
    int32_t  slider_track_h; // horizontal track thickness, clipped to control height

    // Font cell (the one embedded PSF1 face — a slot, so the day a second
    // face exists it arrives through the table like everything else).
    int32_t  font_w;
    int32_t  font_h;
    int32_t  control_radius; // paint-only corner radius, 0 = square
} os64_ui_theme_t;

// Defaults, startup theme.conf through the config ladder, then the session.
// Invalid startup configuration falls back to defaults; invalid session data
// retains the process's last usable appearance.
void os64_ui_theme_init(os64_ui_theme_t *t);
void os64_ui_theme_startup(os64_ui_theme_t *t);
// Overlay a usable startup file onto an application's supplied defaults.
// Missing or invalid files leave the supplied theme unchanged.
bool os64_ui_theme_read_startup(os64_ui_theme_t *t);
// Initialize an active context from supplied defaults, disk geometry, and the
// session. A pinned startup overlay preserves absent keys as caller defaults.
// Use this for app initialization; read_startup reads the next-boot selection.
void os64_ui_theme_current(os64_ui_theme_t *t, uint64_t *installed);

// Ordinary-thread APIs, not async-signal-safe. Theme and installed belong to
// the calling context; the cache is synchronized across the process. hint=0
// checks the store even without an event (initialization); other hints come
// from APPEARANCE events. Colors, relief and corners merge without relayout.
// Returns true when this context adopted a newer usable generation.
bool os64_ui_theme_session(os64_ui_theme_t *theme, uint64_t *installed, uint64_t hint);

#define OS64_UI_APPLY_IO        (-1)
#define OS64_UI_APPLY_CONFLICT  (-2)
#define OS64_UI_APPLY_INVALID   (-3)
#define OS64_UI_APPLY_EXHAUSTED (-5)
// Read the latest session, merge the specified components, compare/publish.
// Returns 0 on success, optionally reports the published generation. A
// conflict leaves the draft intact for an explicit retry. No files are saved.
int os64_ui_theme_apply(const os64_ui_theme_t *draft, uint32_t components,
                        uint64_t *published);

#define OS64_UI_COMPONENT_PALETTE 1u
#define OS64_UI_COMPONENT_TREATMENT 2u
bool os64_ui_theme_valid(const os64_ui_theme_t *t);
// Atomic decode. Apply payloads require all colors and button.bevel. A payload
// starting with "inherit = startup\n" preserves only explicitly present live
// keys. Both session forms refuse layout metrics; startup files may be partial.
// Legacy saved/Apply snapshots without control.radius mean square controls;
// a preservation overlay leaves absent treatment keys at caller defaults.
bool os64_ui_theme_parse(os64_ui_theme_t *t, const char *text, size_t length,
                        bool session);
bool os64_ui_theme_parse_saved(os64_ui_theme_t *t, const char *text, size_t length);
int64_t os64_ui_theme_encode_session(const os64_ui_theme_t *t, char *text, size_t cap);
// Full snapshots include geometry. Startup selection writes theme.conf through
// the shared ladder without publishing a live session change.
int64_t os64_ui_theme_encode(const os64_ui_theme_t *t, char *text, size_t cap);
int64_t os64_ui_theme_set_startup(const os64_ui_theme_t *t);

#define OS64_UI_THEME_NAME_MAX 40
#define OS64_UI_THEME_IO (-1)
#define OS64_UI_THEME_EXISTS (-2)
#define OS64_UI_THEME_INVALID (-3)
#define OS64_UI_THEME_LIMIT (-4)
typedef struct { char name[OS64_UI_THEME_NAME_MAX + 1]; } os64_ui_theme_entry_t;
bool os64_ui_theme_name_valid(const char *name);
// Personal collection: themes/ under the first configured settings directory.
// List returns a count or a negative status; load leaves its output on failure.
int os64_ui_theme_list(os64_ui_theme_entry_t *entries, size_t cap);
int os64_ui_theme_load(const char *name, os64_ui_theme_t *theme);
// Create uses no-replace publication. Explicit replacement requires atomic
// filesystem replacement; concurrent replacements are last-publication-wins.
int os64_ui_theme_save(const char *name, const os64_ui_theme_t *theme, bool replace);
void os64_ui_theme_merge(os64_ui_theme_t *dst, const os64_ui_theme_t *src,
                         uint32_t components);

// Initialize without reading configuration, for independent theme previews.
void os64_ui_theme_defaults(os64_ui_theme_t *t);

typedef enum {
    OS64_UI_PALETTE_MIDNIGHT,
    OS64_UI_PALETTE_PAPER,
    OS64_UI_PALETTE_ELECTRIC,
} os64_ui_palette_t;

// Replace colors while preserving metrics and button treatment. These are
// building blocks for preview compositions, not a desktop-wide activation.
void os64_ui_theme_palette(os64_ui_theme_t *t, os64_ui_palette_t palette);

// The color inspector uses the serialization schema rather than a second
// list of fields. Invalid indices read as opaque black; writes are refused.
size_t os64_ui_theme_color_count(void);
const char *os64_ui_theme_color_name(size_t index); // stable file key
const char *os64_ui_theme_color_label(size_t index); // inspector caption
uint32_t os64_ui_theme_color_get(const os64_ui_theme_t *t, size_t index);
bool os64_ui_theme_color_set(os64_ui_theme_t *t, size_t index, uint32_t color);
#define OS64_UI_PALETTE_ROLE_COUNT 9
const char *os64_ui_palette_role_name(size_t role);
uint32_t os64_ui_palette_role_get(const os64_ui_theme_t *t, size_t role);
// Update role members that still equal the previous role color. Distinct
// individual colors remain overrides. Snapshots store the resolved colors.
void os64_ui_palette_role_set(os64_ui_theme_t *t, size_t role, uint32_t color);
bool os64_ui_palette_color_follow(os64_ui_theme_t *t, size_t index);

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
    // Optional private-state reset when focus or pointer ownership is lost.
    void (*cancel)(os64_ui_widget_t *w);
    // Optional: re-derive this widget's font-dependent geometry. libui calls
    // it when the widget joins a tree and again whenever the window's face
    // changes, before any layout runs.
    //
    // Stage this widget's text against the candidate face, apply it, throw
    // it away. THIS IS WHERE A WIDGET IS ALLOWED TO FAIL: laying text out
    // allocates, a commit may not, and a paint has nowhere to report to —
    // so the runs the next paint will need are built here, while "no" still
    // means the window keeps the face it has. A widget whose text is one
    // caption gets this for free (see os64_ui_stage_caption); one that
    // draws a list of app-supplied strings stages the rows it can show.
    os64_font_status_t (*prepare)(os64_ui_widget_t *w, os64_ui_t *ui);
    void (*commit)(os64_ui_widget_t *w);
    void (*discard)(os64_ui_widget_t *w);

    // IT REPORTS WHAT THE WIDGET NEEDS; IT DOES NOT ALLOCATE THE RECTANGLE.
    // A label, a button and a checkbox are each one row of text plus
    // furniture, so the class is the only thing that can size them under a
    // face the theme cannot carry — but it writes `natural_h`, and the
    // layout decides what `bounds.h` becomes. It must not allocate and must
    // not fail: it runs at commit, where there is no way to report either.
    // Fallible work belongs to preparation.
    //
    // It is also where a widget caches an answer the theme cannot give:
    // os64_ui_listbox_rows is handed a THEME and needs a row pitch, so the
    // listbox stores its own.
    void (*metrics)(os64_ui_widget_t *w, os64_ui_t *ui);
} os64_ui_class_t;

struct os64_ui_widget
{
    const os64_ui_class_t *cls;
    os64_gui_rect_t bounds;      // content-local, assigned by app or layout
    bool hidden;                 // skipped by paint AND hit-test
    bool disabled;               // disables this subtree; use runtime setters below
    bool focusable;              // participates in keyboard traversal
    bool accepts_tab;            // literal Tab input; Ctrl+Tab still traverses
    bool hovered;                // maintained by dispatch
    bool pressed;                // button state (owned by button's event fn)
    bool focused;                // active keyboard focus; false while the window is blurred
    uint16_t activation_key;     // physical key identity while keyboard-pressed; 0 = idle

    const char *text;            // label/button caption (app-owned storage)

    void (*on_click)(os64_ui_widget_t *w, void *user);
    void *user;

    // WHAT THIS WIDGET WANTS, AND WHAT IT WAS GIVEN, are different things.
    // `natural_h` is the height this kind of widget needs under the current
    // face — a minimum, re-derived by its class and never allocated storage.
    // `bounds.h` is what the layout handed it. `auto_h` says which one a
    // layout should use, and it is an EXPLICIT policy rather than a guess:
    // a height of zero cannot be told apart from one a layout wrote last
    // time, and an application must not have to repair its own choice after
    // every attach and font change. Constructors set it for the controls
    // that are one row of text; os64_ui_widget_fixed_height clears it.
    int32_t natural_h;
    bool    auto_h;

    // The run this widget's text is drawn from, retained under the active
    // face, and the one prepared against a candidate during adoption. Both
    // are os64_text_run_t*, opaque here because a widget does not open them
    // — libui lays them out and swaps `staged` in at commit, which is what
    // lets a commit allocate nothing and the first paint after it be
    // coherent. Laying text out at paint time could fail, and a paint has
    // nowhere to report a failure to.
    void *run, *run_staged;

    // The UI this widget belongs to, threaded in when it joins a tree.
    // A paint routine is handed only (widget, canvas, theme) — the theme
    // cannot carry a font, so this is how a widget reaches the window's
    // binding to measure a string. NULL until it is attached, which reads
    // as "no binding" and draws through the bitmap painter.
    os64_ui_t *ui;

    // Tree links (intrusive — the app owns the nodes, libui only threads
    // them). Children paint and hit-test in list order; last child is
    // "topmost" for a hit, matching paint order.
    os64_ui_widget_t *parent;
    os64_ui_widget_t *first_child;
    os64_ui_widget_t *next_sibling;
};

// ── The UI context (one per widget tree) ────────────────────────────────────

struct os64_ui
{
    os64_draw_ctx_t *ctx;        // the window's draw context (app-owned)
    os64_ui_theme_t  theme;
    void            *font;       // this window's font binding, libui-owned
                                 // (opaque: the plan types behind it are not
                                 // an interface). NULL until a widget draws
                                 // text or the app asks for the context.

    // The application's layout planner, set by os64_ui_font_planner. It
    // lives HERE, in storage the application already owns, so registering
    // one cannot fail for want of memory — a planner that silently failed
    // to register would let a later adoption install a face and leave the
    // window's own layout untouched, which is the exact half-change the
    // transaction exists to prevent. Set it through the function, which
    // checks that the trio is whole.
    os64_font_status_t (*font_plan)(os64_ui_t *ui, void *user, void **out);
    void (*font_plan_commit)(os64_ui_t *ui, void *user, void *plan);
    void (*font_plan_discard)(os64_ui_t *ui, void *user, void *plan);
    void *font_plan_user;
    uint64_t appearance_generation; // installed by this context, not its siblings
    bool follow_session;           // false for independent draft previews
    os64_ui_widget_t *root;
    os64_ui_widget_t *grab;      // widget owning the mouse (button held)
    os64_ui_widget_t *focus;     // key events go here (NULL = dropped)
    os64_ui_widget_t *hover;     // current enabled control under the pointer
    uint8_t grab_button;         // button that owns the widget grab
    bool window_blurred;        // retain the logical focus target while its window is inactive
    os64_gui_rect_t  dirty;      // union of everything needing repaint
    bool             any_dirty;

    // Called after the window changed size, once libui has refreshed the draw
    // context and stretched `root` to the new content area — the app's cue to
    // re-run whatever layout it used in the first place (libui cannot know:
    // layout is a call the app makes, not a policy libui holds). NULL is the
    // honest default for a fixed-layout window; everything still repaints,
    // the widgets just stay where the app put them.
    void (*on_resize)(os64_ui_t *ui);

    // The user asked the window to close (Alt+F4 — OS64_GUI_EVENT_WINDOW_CLOSE,
    // 2026-08-23). If set, libui calls this and does nothing else: the app
    // decides (save? ask? ignore?). If NULL, libui sets `quit`, which ends
    // os64_ui_run; an app with its own loop should check `quit` too. An app
    // that answers neither is still closable — the user's next Alt+F4 within
    // five seconds is SIGTERM to the task, which is the window system's way
    // of saying it asked nicely once.
    void (*on_close)(os64_ui_t *ui);
    bool             quit;       // set by the default close handling; read by os64_ui_run
};

// ── the window's fonts (F4; FONT_PROVIDER.md is the contract) ───────────────
// A window measures and paints text through an immutable role set: UI for
// labels, buttons, list rows and fields; DOCUMENT for textviews. The set is
// F5's to resolve from configuration — until then a window binds the builtin
// 8x16 face on first use, which is the face libui always drew with.
//
// THE THEME'S font.w/font.h ARE NOT THIS. They stay 8/16 and describe the
// bitmap painter that os64_draw_text still uses; they are not selectors and
// they do not follow the bound face. Ask these calls for live metrics.

typedef struct
{
    int32_t row_h;     // row pitch: the primary's line box, not a nominal size
    int32_t baseline;  // from the row's top to the baseline
    int32_t cell_w;    // fixed cell width; zero for UI and DOCUMENT
} os64_ui_font_metrics_t;

// The window's text context, created on first use and owned by libui. F5 and
// the font fixtures prepare their candidate sets on THIS context. A set may
// only be bound to a window whose context it was prepared on — a foreign
// candidate is refused rather than measured through the wrong engine.
os64_text_context_t *os64_ui_font_context(os64_ui_t *ui);

// Lend this window an APPLICATION-OWNED context instead, so several windows
// can share one engine and one glyph cache while each holds its own active
// set. That is the usual shape for a program with more than one window: a
// context is not an active-font singleton, and independent previews come
// from separate SETS, not separate engines.
//
// libui borrows it and never destroys it; the application outlives every
// window using it and destroys it last. Refused once this window already
// has a context with anything in it, because moving a window between
// engines would strand the runs it is holding.
os64_font_status_t os64_ui_font_borrow_context(os64_ui_t *ui,
                                               os64_text_context_t *text);

// Install a set immediately, taking a reference. This is the STARTUP door —
// there is no old layout to preserve, so it cannot half-succeed. A live
// replacement goes through os64_font_adopt with the consumer below, which is
// what keeps the document's bytes, focus and selection across the change.
os64_font_status_t os64_ui_font_bind(os64_ui_t *ui, os64_font_set_t *set);

// The whole-window consumer: prepare stages runs and geometry, commit swaps
// them without allocating, abort throws the staging away. Hand it to
// os64_font_adopt; the plan behind it is libui's and has no public shape.
void os64_ui_font_consumer(os64_ui_t *ui, os64_font_consumer_t *out);

// The app's own layout, planned with the CANDIDATE metrics before anything
// live moves. libui calls `plan` inside prepare: measure with
// os64_ui_text_measure and os64_ui_font_metrics, which report the candidate
// for the duration, and stage the result without touching a live bound.
// Anything but OS64_FONT_OK fails the whole adoption and leaves the old
// state as it was — return LIMIT when the layout will not fit the content
// area, NO_MEMORY when staging could not be allocated, so the caller can
// tell "too big" from "out of room" (they deserve different next moves).
// Measure from the calls above, NOT from widget bounds: those still
// describe the face the window is wearing, and only change at commit.
// `commit` applies what was planned and cannot fail; `discard` frees it.
// ALL THREE OR NONE: a plan that owns anything needs both the door that
// applies it and the door that throws it away, so a partial trio is
// BAD_ARGUMENT and all-NULL is how a window says it has no layout to stage.
// It cannot fail for want of memory — the trio is stored in the os64_ui_t
// itself — so the only refusal is a malformed one.
os64_font_status_t os64_ui_font_planner(os64_ui_t *ui,
                          os64_font_status_t (*plan)(os64_ui_t *ui, void *user, void **out),
                          void (*commit)(os64_ui_t *ui, void *user, void *plan),
                          void (*discard)(os64_ui_t *ui, void *user, void *plan),
                          void *user);

// Live metrics for a role. These never build a font engine: a widget derives
// its geometry the moment it joins a tree, and a tree is often laid out
// before anything is drawn. False therefore means "this window is not
// wearing a set" — it has drawn no text yet, or its engine refused — and the
// metrics describe the 8x16 bitmap cell those windows draw with. A caller
// that just wants numbers can ignore the return.
bool os64_ui_font_metrics(os64_ui_t *ui, os64_font_role_t role,
                          os64_ui_font_metrics_t *out);
int32_t os64_ui_font_row_height(os64_ui_t *ui, os64_font_role_t role);

// A string's advance in whole pixels, rounded outward so a width that
// decides whether text fits never comes back short of its own ink.
//
// IT RETURNS A STATUS BECAUSE MEASURING CAN FAIL. Laying a string out
// allocates, and a window wearing a real face has no honest answer when
// that allocation is refused — the 8x16 cell is not a smaller version of
// DejaVu, it is a different number (`WWWW` is 32 pixels against 96). A
// planner that swallowed the failure would stage a layout measured in a
// font nobody is wearing and hand the coordinator a successful plan. An
// unbound window is the one case with a real answer: it draws with the
// bitmap cell, so that is what it measures.
os64_font_status_t os64_ui_text_measure(os64_ui_t *ui, os64_font_role_t role,
                                        const char *s, size_t len, int32_t *out);

// Paint one line at (x, top_y) — top, not baseline, which is what every
// caller already has. Fills the run's box with `bg` first, then the glyphs,
// CLIPPED TO THE PRIMARY ROW: the row's pitch comes from the primary face
// and does not grow for a taller fallback or a missing-glyph marker, so
// what does not fit the row is cut rather than allowed to paint over the
// line above. The caller's clip still bounds it horizontally, overhang and
// all. Returns the pen's x after the run.
//
// `run_slot` is WHERE THIS TEXT'S RETAINED RUN LIVES — a widget's own `run`
// for a caption, a row's slot for a list, NULL for text nobody caches. The
// run there is reused when it still says what `s` says and re-laid-out into
// the same slot when it does not, so app-supplied text that changes without
// notice stays correct without a paint having to be told.
//
// A window wearing a face whose text cannot be laid out paints NOTHING
// rather than substituting a different font's glyphs, and records the
// failure for os64_ui_font_status.
int32_t os64_ui_draw_text(os64_ui_t *ui, void **run_slot,
                          os64_font_role_t role,
                          os64_gui_surface_t *dst, os64_gui_rect_t clip,
                          int32_t x, int32_t top_y, const char *s, size_t len,
                          uint32_t fg, uint32_t bg);

// The set this window holds (borrowed, NULL before first use), the status
// that explains an absent set, and the engine's live bytes — the last for
// fixtures that prove a released binding owes nothing.
os64_font_set_t *os64_ui_font_set(os64_ui_t *ui);
os64_font_status_t os64_ui_font_status(const os64_ui_t *ui);
size_t os64_ui_font_live_bytes(const os64_ui_t *ui);

// Re-derive every widget's cached geometry from the current binding. Called
// for you when the set changes; an app needs it only after building a tree
// under a set it bound earlier.
void os64_ui_font_restamp(os64_ui_t *ui);

// Drop the set, the retained runs and — if libui built it — the context.
// An app that simply exits need not call this; it exists so a fixture can
// close the loop on the engine's accounting.
//
// IT CAN REFUSE. A context stays BUSY while anything the caller still holds
// is alive: a candidate set it retained, a run it took. The binding is that
// context's ALLOCATOR, so freeing it would leave the survivors calling into
// freed memory the moment they are released. BUSY therefore leaves the
// window exactly as it was — release what you are holding and call again.
os64_font_status_t os64_ui_font_release(os64_ui_t *ui);

// Bind a UI to a window's draw context and load the theme. root may be set
// afterwards (os64_ui_set_root marks everything dirty).
void os64_ui_init(os64_ui_t *ui, os64_draw_ctx_t *ctx);
void os64_ui_set_root(os64_ui_t *ui, os64_ui_widget_t *root);

// Attach a child at the END of parent's list (paints last = on top). The
// child (and its own subtree) joins the parent's UI, so it can measure text
// before the first paint.
void os64_ui_add_child(os64_ui_widget_t *parent, os64_ui_widget_t *child);

// The UI a widget belongs to — what a paint routine or a metrics callback
// asks when it needs the window's fonts. NULL for an unattached widget.
static inline os64_ui_t *os64_ui_of(os64_ui_widget_t *w) { return w->ui; }

// Give a widget a height of the application's choosing, and say so: the
// layout stops sizing it from the face, and a font change leaves it alone.
void os64_ui_widget_fixed_height(os64_ui_widget_t *w, int32_t h);

// The one-caption case of the class trio above, for a widget whose text is
// `w->text`. A class with nothing else to stage can use these three
// directly as its prepare/commit/discard.
os64_font_status_t os64_ui_stage_caption(os64_ui_widget_t *w, os64_ui_t *ui);
void os64_ui_commit_caption(os64_ui_widget_t *w);
void os64_ui_discard_caption(os64_ui_widget_t *w);

// Lay `s` out for this role and hand back a retained run, or NULL with OK
// when the window wears no face and the caller should draw the bitmap cell.
// Release it with os64_ui_run_release. This is what a class stages with.
os64_font_status_t os64_ui_run_layout(os64_ui_t *ui, os64_font_role_t role,
                                      const char *s, size_t len, void **out);
void os64_ui_run_release(void *run);
// Does this retained run still say what these bytes say? A class checks
// before reusing one, because app-supplied text changes without notice.
bool os64_ui_run_matches(void *run, const char *s, size_t len);

// What a control has to be tall enough for: one row of the UI face plus the
// theme's padding above and below, never less than the theme's stock button
// height. A caption clipped by its own button is the fingerprint this
// answers — the theme's button.h was written for an 8x16 cell and a face is
// free to be taller than that.
int32_t os64_ui_control_min_height(os64_ui_t *ui);

// Mark a widget (its current bounds) as needing repaint.
void os64_ui_mark_dirty(os64_ui_t *ui, os64_ui_widget_t *w);

// Route one event. Pointer events hit-test to the deepest visible widget
// (with a press grab: DOWN grabs, UP releases and fires on_click if it ends
// over the widget it started on — the ancient button contract, so a drag-off
// cancels). Key events go to `focus`, with Tab traversal unless the focused
// control accepts literal tabs (Ctrl+Tab traverses there). Hover state updates
// on motion; ungrabbed moves are not sent to widget event handlers.
// Returns true if the toolkit or a widget consumed the event.
bool os64_ui_dispatch(os64_ui_t *ui, const os64_gui_event_t *ev);

// Repaint whatever is dirty and publish exactly that rect. No-op when clean.
void os64_ui_paint(os64_ui_t *ui);

// Paint dirty widgets into the canvas without publishing. Returns true when
// pending damage was processed, optionally reports that damage, and clears
// it. Several UI contexts sharing a canvas can render first and publish their
// combined damage once; damage may be NULL when the caller repaints a superset.
bool os64_ui_render(os64_ui_t *ui, os64_gui_rect_t *damage);

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

// ── Basic widgets ──────────────────────────────────────────────────────────
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
// ends so carets appear and disappear honestly. NULL blurs. A non-NULL target
// must be focusable, enabled, visible, and part of this UI's tree.
void os64_ui_set_focus(os64_ui_t *ui, os64_ui_widget_t *w);

// Step through focusable controls in tree order. With wrap=false, reaching
// the edge clears focus and returns false so an app can continue in another
// UI context. With wrap=true, traversal cycles within this tree.
bool os64_ui_focus_next(os64_ui_t *ui, bool reverse, bool wrap);
bool os64_ui_widget_enabled(const os64_ui_widget_t *w);
void os64_ui_set_enabled(os64_ui_t *ui, os64_ui_widget_t *w, bool enabled);
void os64_ui_set_hidden(os64_ui_t *ui, os64_ui_widget_t *w, bool hidden);
void os64_ui_clear_hover(os64_ui_t *ui);
// Cancel presses, drags, and hover while retaining the logical keyboard focus.
void os64_ui_cancel_gestures(os64_ui_t *ui);
// Cancel gestures and clear focus when replacing/hiding a tree or entering a modal.
void os64_ui_cancel_interaction(os64_ui_t *ui);

typedef struct os64_ui_checkbox {
    os64_ui_widget_t w;
    bool checked;
    void (*on_change)(struct os64_ui_checkbox *, void *);
    void *check_user;
} os64_ui_checkbox_t;
void os64_ui_checkbox(os64_ui_checkbox_t *cb, const char *text, bool checked,
                     void (*on_change)(os64_ui_checkbox_t *, void *), void *user);
// Programmatic setters update/repaint without firing interaction callbacks.
void os64_ui_checkbox_set(os64_ui_t *ui, os64_ui_checkbox_t *cb, bool checked);

typedef struct os64_ui_slider {
    os64_ui_widget_t w;
    int32_t min, max, step, value;
    int32_t drag_offset;
    int32_t drag_x;              // last pointer sample; a stationary press preserves value
    uint8_t seq;
    void (*on_change)(struct os64_ui_slider *, void *);
    void *slider_user;
} os64_ui_slider_t;
// Inclusive range; max<min becomes a fixed-value range and step<1 becomes 1.
// Values clamp to the range. Keyboard arrows step, Home/End choose endpoints.
void os64_ui_slider(os64_ui_slider_t *sl, int32_t min, int32_t max,
                   int32_t step, int32_t value,
                   void (*on_change)(os64_ui_slider_t *, void *), void *user);
void os64_ui_slider_set(os64_ui_t *ui, os64_ui_slider_t *sl, int32_t value);
extern const os64_ui_class_t os64_ui_checkbox_class;
extern const os64_ui_class_t os64_ui_slider_class;

// HSV color picker: saturation/value square and hue strip. Arrow keys move
// saturation/value; Shift+Left/Right changes hue. Programmatic set is silent.
typedef struct os64_ui_colorpicker {
    os64_ui_widget_t w;
    uint32_t color;
    int hue, saturation, value, drag_part;
    uint8_t seq;
    void (*on_change)(struct os64_ui_colorpicker *, void *);
    void *color_user;
} os64_ui_colorpicker_t;
void os64_ui_colorpicker(os64_ui_colorpicker_t *picker, uint32_t color,
    void (*on_change)(os64_ui_colorpicker_t *, void *), void *user);
void os64_ui_colorpicker_set(os64_ui_t *ui, os64_ui_colorpicker_t *picker, uint32_t color);
uint32_t os64_ui_color_from_hsv(int hue, int saturation, int value);

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
    bool horizontal;             // set after init for a bottom-edge bar; the
                                 // thumb, drag, and page-jumps all follow the
                                 // long axis (Chris's first-test verdict:
                                 // no-wrap without one was the only gap)
};

void os64_ui_scrollbar(os64_ui_scrollbar_t *sb,
                       void (*on_scroll)(os64_ui_scrollbar_t *, void *),
                       void *user);
// Update the three numbers and repaint. Clamps pos into [0, total-visible].
void os64_ui_scrollbar_set(os64_ui_t *ui, os64_ui_scrollbar_t *sb,
                           int64_t total, int64_t visible, int64_t pos);

// ── listbox — a single selection over application-owned labels ──────────────
typedef struct os64_ui_listbox os64_ui_listbox_t;
struct os64_ui_listbox {
    os64_ui_widget_t w;
    size_t count, top;
    int selected, pressed_index;
    uint8_t seq;
    const char *(*label)(size_t index, void *user);
    void (*on_change)(os64_ui_listbox_t *, void *user);
    void *list_user;
    // Optional color chip before each label; shares the row's hit target.
    uint32_t (*swatch)(size_t index, void *user);
    // Row pitch, re-derived from the UI role whenever the font changes.
    // Zero means "not stamped yet" and reads as the theme's bitmap cell,
    // which is the same answer the builtin set gives.
    int32_t row_h;

    // Retained runs for the rows this box can SHOW, staged against a
    // candidate during adoption and swapped in at commit — the same
    // contract a caption has, except the strings come from the
    // application's callback and there is one per visible row. Rows
    // outside the viewport are laid out when they scroll into it.
    void **row_runs, **row_runs_staged;
    size_t row_run_count, row_runs_staged_count;
};
void os64_ui_listbox(os64_ui_listbox_t *list, size_t count,
                     const char *(*label)(size_t, void *),
                     void (*on_change)(os64_ui_listbox_t *, void *), void *user);
// Programmatic updates do not call on_change. Selection is -1 for none.
void os64_ui_listbox_set(os64_ui_t *ui, os64_ui_listbox_t *list, size_t count, int selected);
int os64_ui_listbox_rows(const os64_ui_listbox_t *list, const os64_ui_theme_t *theme);
void os64_ui_listbox_scroll_to(os64_ui_t *ui, os64_ui_listbox_t *list, size_t top);

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
// Insert the system clipboard at the caret. A field is ONE line, so it takes
// the clipboard's first line and stops there — said out loud rather than
// discovered: pasting a two-line snarf into an Open box gets you line one,
// not a mangled path. Returns bytes inserted (0 = nothing to paste, or full).
// There is deliberately no field COPY: a textfield has no selection model,
// and inventing one to feed the clipboard is a different slice with its own
// consumer. (CLIPBOARD.md)
size_t os64_ui_textfield_paste(os64_ui_t *ui, os64_ui_textfield_t *tf);

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
// Scroll horizontally so `left` is the first visible VISUAL column
// (clamped at 0; the right edge is the app's knowledge — it tracks the
// widest line, the view doesn't). Fires on_view.
void os64_ui_textview_scroll_left(os64_ui_t *ui, os64_ui_textview_t *tv,
                                  int64_t left);
// A line's width in VISUAL columns (tabs expanded to their 8-stop) — the
// number a horizontal scrollbar's `total` is made of.
int64_t os64_ui_text_vcols(const char *s, size_t len);
// Is this key event the REAL Esc key (not the ESC byte that opens a VT100
// burst)? The burst's ESC is stamped with the extended key's code; the Esc
// key carries its own — which is a DIALECT: PS/2 make-code 0x01 or HID
// usage 0x29, depending on which keyboard driver fed the compositor. Apps
// answering Esc themselves (ahead of widget dispatch) must use this, not a
// raw scancode compare — a check that knows one dialect works in QEMU and
// dies on the P5 (2026-08-21, the chord-publish day's ring-3 echo).
bool os64_ui_key_is_esc(const os64_gui_event_t *ev);
// Place the caret (clamped), optionally keeping/starting a selection from
// the current anchor, and scroll it into view. The search-jump primitive.
void os64_ui_textview_goto(os64_ui_t *ui, os64_ui_textview_t *tv,
                           size_t line, size_t col, bool select);
// Select [sl,sc) .. [el,ec), caret at the end, scrolled into view.
void os64_ui_textview_select(os64_ui_t *ui, os64_ui_textview_t *tv,
                             size_t sl, size_t sc, size_t el, size_t ec);

// ── the system clipboard (CLIPBOARD.md) ─────────────────────────────────────
// THE MECHANISM LIVES HERE; THE BINDING IS THE APP'S. libui does not claim
// Ctrl+C — a terminal widget would want that key for SIGINT, and a library
// that decides such things for its host is a library you fight. scribe calls
// these from its own shortcut table, like it does Ctrl+S; the next consumer
// picks its own keys and gets the same three verbs.
//
// The clipboard these speak to is the SYSTEM's one snarf buffer — the same
// bytes `cat /sys/clipboard` prints, so text copied here pastes into a shell
// pipeline and vice versa. That is the entire point of it being a file.

// Write the selection to the clipboard. Returns bytes copied, 0 when nothing
// is selected, negative if the clipboard refused it (over its ceiling, say).
// Works on a READ-ONLY view: a viewer that can't be edited can still be
// quoted, which is most of what a help page or a log viewer is for.
int64_t os64_ui_textview_copy(const os64_ui_textview_t *tv);

// Copy the selection, then delete it. Refuses on a read-only view, and —
// deliberately — refuses if the COPY failed: text is never destroyed on
// behalf of a clipboard that did not take it.
bool os64_ui_textview_cut(os64_ui_t *ui, os64_ui_textview_t *tv);

// Insert the clipboard at the caret, replacing any selection (Bravo's
// manners, and everyone's since). Newlines split lines; a CR is dropped as
// the line-ending fossil it is, because a buffer holds LINES, not
// terminators. Streams straight from the file — a large paste costs no
// intermediate copy of itself. False = read-only view, or empty clipboard
// (which changes nothing, selection included).
bool os64_ui_textview_paste(os64_ui_t *ui, os64_ui_textview_t *tv);

extern const os64_ui_class_t os64_ui_scrollbar_class;
extern const os64_ui_class_t os64_ui_textfield_class;
extern const os64_ui_class_t os64_ui_textview_class;

#endif // OS64_UI_H
