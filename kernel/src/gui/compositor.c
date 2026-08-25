// compositor.c — the GUI compositor daemon and subsystem entry point.
//
// The pipeline (per frame): drain input → route events → recomposite damaged
// screen regions into the RAM backbuffer (scene layers bottom-up, cursor
// last) → flush only the damaged rects to the uncached hardware framebuffer.
// The backbuffer is the canonical screen image; the framebuffer only ever
// receives finished pixels, which is why nothing can flicker.
//
// Scene layers: the desktop surface (bottom), the windows in z-order, then
// the mouse cursor. The cursor needs no save-under trickery — recompositing
// the damaged region rebuilds whatever it covered.

#include "gui/compositor.h"
#include "gui/surface.h"
#include "gui/input.h"
#include "gui/window.h"
#include "gui/gui_client.h"
#include "gui/gui_internal.h"

#include "tty.h"             // kTTY/kTTYFocused — glass ownership IS VT focus (VT8 chapter)
#include "vt_select.h"       // the other side of the input fork: console mouse selection
#include "driver/system/keyboard.h"   // KEYBOARD_MOD_* — the Ctrl+Alt gestures

#include "CONFIG.h"
#include "kernel.h"
#include "printd.h"
#include "smp.h"
#include "smp_core.h"
#include "scheduler.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"
#include "video.h"

// kTicksSinceStart comes from kernel.h; kKernelTask is per-file extern by convention
extern task_t *kKernelTask;
extern struct Framebuffer kFrameBuffer;

bool kEnableGUI = false;

extern bool kTicklessScheduler;

// The compositor task, created by gui_start(). Kept for future use
// (diagnostics, wake-on-damage once an IRQ-safe wake primitive exists).
task_t *kGuiCompTask = NULL;

// ---------------------------------------------------------------------------
// Shared GUI state. Everything guarded by kGuiLock is touched by client
// threads (gui_window_* calls) and the compositor. Compositing into the RAM
// backbuffer happens UNDER the lock (sub-ms; keeps window lifetimes safe);
// only the flush to the slow uncached framebuffer runs after release.
// ---------------------------------------------------------------------------
spinlock_t kGuiLock = 0;   // shared with window.c / gui_client.c via gui_internal.h

// ── VT8 glass ownership (Phase B, 2026-08-19 — GRAPHICS.md's VT8 chapter) ───
// The GUI is VT8's seated shell, not the framebuffer's owner-by-force. One
// predicate gates every flush: the compositor paints the IRON only while VT8
// is the focused terminal AND the compositor is seated there. Without the GUI
// cmdline flag VT8 is an ordinary text terminal — dormant, husk-on-knock —
// and this predicate stays false for the machine's whole life (ruling 2).
//
// Compositing into the RAM backbuffer continues while a text VT holds the
// glass: it is cheap, it keeps window state current, and it is what makes
// switching back ONE full-screen damage add instead of a scene rebuild — the
// backbuffer is VT8's grid, and this is its repaint-from-state.
static volatile bool s_gui_seated = false;

bool gui_vt8_seated(void)
{
	return s_gui_seated;
}

bool gui_owns_glass(void)
{
	return s_gui_seated && kTTYFocused == &kTTY[7];
}

// (gui_vt8_focus_gained lives below kBackbuffer's declaration.)

// ---------------------------------------------------------------------------
// Damage accumulator, v2: a small rect LIST (2026-08-18).
//
// v1 kept ONE union rect, and the P5 shakedown priced that choice: a bouncing
// ball in one corner unioned with a cursor in the other produced a near-
// fullscreen rectangle, recomposited and pushed through the uncached
// framebuffer ~100 times a second. The screen was mostly unchanged; we paid
// retail for it anyway. Two small rects should cost two small rects.
//
// The design is deliberately the cheap one — a fixed array plus a merge
// heuristic, NOT region algebra. Regions (X11's, Cairo's) split rectangles on
// overlap to keep an exact non-overlapping cover, and they are the right
// answer when you have thousands; here the whole population is "one ball, one
// cursor, one console line, sometimes a dragged window", and an exact cover
// would cost more to maintain than the overdraw it saves.
//
// Two rules keep it honest:
//   1. MERGE WHEN MERGING IS CHEAP. Union two entries only if the union wastes
//      no more than DAMAGE_MERGE_SLACK pixels beyond their own areas — so
//      touching/overlapping rects collapse (and overlapping ones MUST, or the
//      overlap composites twice) while distant ones stay apart. Without this,
//      a list degenerates into a list of near-duplicates.
//   2. OVERFLOW FALLS BACK TO v1. When the list is full and nothing merges
//      cheaply, collapse everything into one union rect. Worst case is exactly
//      today's behavior — never a wrong screen, only a slower one.
//
// Entries may still overlap (rule 1 tolerates SLACK, and merging is
// pairwise-greedy with NO re-merge pass: a rect that bridges into entry i can
// grow it until it swallows entry j whole, and nothing goes back to notice).
// So the overlap is not strictly slack-bounded — the swallowed-entry case
// composites and UC-flushes a duplicate of j's whole area. It is WASTE, never
// wrongness: both passes composite identical bytes in identical z-order, so
// the glass cannot disagree with itself. The geometry that triggers it (a
// late rect bridging two established ones) is rare at this population size;
// if it ever shows in the flush counter, the fix is a containment sweep after
// a merge grows an entry, not a redesign. (Honesty upgrade from review,
// 2026-08-19 — the first version of this comment claimed the slack bound.)
// ---------------------------------------------------------------------------
// Sixteen since the rubber band (2026-08-19). An interactive resize adds the
// four edges of the OLD outline and the four of the NEW one every time the
// mouse moves — eight thin rects that deliberately do not merge with each
// other (their unions are the whole window's area, which is exactly what the
// slack rule is there to refuse). At the old ceiling of eight, adding the
// cursor's rect on top tipped every band frame into the union fallback and a
// full-screen composite, which is the precise cost the list was built to
// avoid. The population argument in the comment above is unchanged; the
// population simply grew a member.
#define DAMAGE_MAX_RECTS   16
// One 8x16 glyph cell (128 px) of tolerated waste per merge: generous enough
// that a scrolling console line's per-glyph damage collapses into one row
// rect, tight enough that opposite corners of a 1024x768 screen never do.
#define DAMAGE_MERGE_SLACK 128

static rect_t   kPendingDamage[DAMAGE_MAX_RECTS];
static uint32_t kPendingDamageCount;   // 0 means "no damage"

static inline int64_t rect_area(rect_t r)
{
	return rect_is_empty(r) ? 0 : (int64_t)r.w * (int64_t)r.h;
}

// Would unioning these two waste little enough to be worth one composite?
// Overlapping rects always qualify: their union is never bigger than the sum
// of their areas, so the test below passes by construction — which is what
// guarantees the list never composites the same pixel twice for free.
static inline bool damage_merge_is_cheap(rect_t a, rect_t b)
{
	return rect_area(rect_union(a, b)) <= rect_area(a) + rect_area(b) + DAMAGE_MERGE_SLACK;
}

// The canonical screen image (scene + cursor). Compositor-thread-only.
static surface_t kBackbuffer;

// tty_focus calls this instead of a grid repaint when focus lands on the
// seated VT8. It runs in the SWITCHER's context — usually the keyboard IRQ —
// and GUI code never runs in ISR context (invariant 4; gui_damage_add's own
// header says NOT from IRQ handlers). So this is ONE STORE to a flag, the
// same lock-free shape as gui_emergency_disable: the compositor's frame loop
// converts it into full-screen damage under its own lock. The keystroke's
// interrupt is what ends the compositor's hlt, so the repaint follows within
// a frame anyway.
static volatile bool s_glass_regained = false;

void gui_vt8_focus_gained(void)
{
	s_glass_regained = true;
}

// The desktop: bottom layer of the scene, painted once at startup. Windows
// (M5) stack on top of it, the cursor on top of everything.
static surface_t kDesktop;

// ---------------------------------------------------------------------------
// Frame pacing: hlt-wait, always.
//
// The compositor never sleeps through the scheduler and never busy-waits:
// it HALTS its core until something interrupt-shaped happens. Because
// kernel_init routes the input IRQs (1, 12) at this core, a mouse packet or
// keystroke ends the hlt directly — input-to-screen latency is one
// interrupt — while an idle desktop costs ~10 trivial wakeups/sec (the
// core's scheduler timer) and effectively zero CPU, guest or host.
//
// History, so nobody resurrects the alternatives: SIGSLEEP pacing had
// 100-500ms wake latency (wakes ride the BSP's signal pass), and the
// "hot spin to the next tick" workaround ate one full core — a full HOST
// core under VBox/QEMU — whenever the mouse moved.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The mouse cursor. Classic arrow, built from readable string art at startup:
// 'X' = black outline, 'o' = white fill, ' ' = transparent (mask 0).
// ---------------------------------------------------------------------------
#define CURSOR_W 12
#define CURSOR_H 18

static const char *const kCursorArt[CURSOR_H] = {
	"X           ",
	"XX          ",
	"XoX         ",
	"XooX        ",
	"XoooX       ",
	"XooooX      ",
	"XoooooX     ",
	"XooooooX    ",
	"XoooooooX   ",
	"XooooooooX  ",
	"XoooooooooX ",
	"XooooooXXXX ",
	"XoooXooX    ",
	"XooX XooX   ",
	"XoX  XooX   ",
	"XX    XooX  ",
	"X     XooX  ",
	"       XX   ",
};

static uint32_t s_cursor_pixels[CURSOR_W * CURSOR_H];
static uint8_t  s_cursor_mask[CURSOR_W * CURSOR_H];

// Cursor position (top-left of the art; the hotspot is the arrow tip at 0,0).
// Written only by the compositor thread while handling MOUSE_MOVE events.
static int32_t s_cursor_x, s_cursor_y;

static void cursor_build(void)
{
	for (int y = 0; y < CURSOR_H; y++) {
		for (int x = 0; x < CURSOR_W; x++) {
			char c = kCursorArt[y][x];
			s_cursor_pixels[y * CURSOR_W + x] =
				(c == 'o') ? GUI_COLOR_WHITE : GUI_COLOR_BLACK;
			s_cursor_mask[y * CURSOR_W + x] = (c != ' ');
		}
	}
}

static inline rect_t cursor_rect(void)
{
	return (rect_t){s_cursor_x, s_cursor_y, CURSOR_W, CURSOR_H};
}

void gui_damage_add_locked(rect_t screen_rect)
{
	if (rect_is_empty(screen_rect))
		return;

	// Merge into the first entry where merging is cheap. First-fit, not
	// best-fit: the list is at most DAMAGE_MAX_RECTS long and the common case
	// is one or two entries, so hunting for the optimal partner would cost
	// more than the overdraw it saves.
	for (uint32_t i = 0; i < kPendingDamageCount; i++) {
		if (damage_merge_is_cheap(kPendingDamage[i], screen_rect)) {
			kPendingDamage[i] = rect_union(kPendingDamage[i], screen_rect);
			return;
		}
	}

	if (kPendingDamageCount < DAMAGE_MAX_RECTS) {
		kPendingDamage[kPendingDamageCount++] = screen_rect;
		return;
	}

	// Full, and nothing merged cheaply — fall back to v1: one union rect for
	// the whole frame. Over-redraws, never under-redraws.
	rect_t all = screen_rect;
	for (uint32_t i = 0; i < kPendingDamageCount; i++)
		all = rect_union(all, kPendingDamage[i]);
	kPendingDamage[0] = all;
	kPendingDamageCount = 1;
}

void gui_damage_add(rect_t screen_rect)
{
	uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
	gui_damage_add_locked(screen_rect);
	spinlock_release_irqrestore(&kGuiLock, flags);
}

void gui_census(uint32_t *windows, uint64_t *surface_bytes)
{
	*windows = 0;
	*surface_bytes = 0;
	if (!kEnableGUI)
		return;   // no compositor, no z-list, nothing to lock

	uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
	wm_census_locked(windows, surface_bytes);
	spinlock_release_irqrestore(&kGuiLock, flags);
}

void gui_emergency_disable(void)
{
	// One store, no locks — the doctrine survives the sink it used to serve:
	// panic() calls this first so the compositor stops flushing and cannot
	// overpaint the dying words (tty_emergency_direct handles the grid side;
	// panic text then goes straight at the iron). The desktop gets scribbled
	// over — intentionally.
	s_gui_seated = false;
}

// ---------------------------------------------------------------------------
// Desktop scene: background + primitive sampler, so every drawing primitive
// stays exercised on real hardware. Painted into kDesktop once.
// ---------------------------------------------------------------------------
static void paint_desktop(void)
{
	int32_t w = (int32_t)kDesktop.width;
	int32_t h = (int32_t)kDesktop.height;

	surface_fill_rect(&kDesktop, (rect_t){0, 0, w, h}, GUI_COLOR_DESKTOP);

	// Deliberately overlapping and partially off-screen rects prove clipping.
	surface_fill_rect(&kDesktop, (rect_t){40, 60, 220, 140}, GUI_COLOR_BLUE);
	surface_fill_rect(&kDesktop, (rect_t){120, 130, 220, 140}, GUI_COLOR_GREEN);
	surface_fill_rect(&kDesktop, (rect_t){-60, h - 100, 200, 200}, GUI_COLOR_RED);    // clips left+bottom
	surface_fill_rect(&kDesktop, (rect_t){w - 120, -40, 200, 120}, GUI_COLOR_YELLOW); // clips right+top
	surface_draw_rect(&kDesktop, (rect_t){36, 56, 310, 220}, GUI_COLOR_WHITE);

	surface_draw_hline(&kDesktop, 40, h / 2, w - 80, GUI_COLOR_LIGHT_GRAY);
	surface_draw_vline(&kDesktop, w / 2, 40, h - 80, GUI_COLOR_LIGHT_GRAY);

	const char banner[] = "os64 GUI: surface core online";
	surface_draw_text(&kDesktop, (w - (int32_t)(sizeof(banner) - 1) * 8) / 2, 24,
	                  banner, sizeof(banner) - 1, GUI_COLOR_WHITE, GUI_COLOR_DESKTOP);

	gui_damage_add((rect_t){0, 0, w, h});
}

// ---------------------------------------------------------------------------
// Recomposite `damage` (screen coords) into the backbuffer: scene layers
// bottom-up (desktop, windows), cursor last. Caller holds kGuiLock; the
// caller flushes AFTER releasing it. This is the only writer of the
// backbuffer.
// ---------------------------------------------------------------------------
// The rubber band lives with the gesture state that drives it (below), but it
// is a SCENE LAYER and has to be drawn from here — hence the one forward
// declaration. It sits above the windows and below the cursor: an outline you
// could lose the pointer inside would be a poor tool.
static void band_composite(surface_t *backbuffer, rect_t damage);
// Same arrangement for the Alt+Tab strip, and the same reason. Like the band
// it is compositor-owned pixels rather than a window: no window_t, no owner,
// no event queue, not in the z-list, not hit-tested.
static void switcher_composite(surface_t *backbuffer, rect_t damage);

static rect_t composite_locked(rect_t damage)
{
	rect_t screen = {0, 0, (int32_t)kBackbuffer.width, (int32_t)kBackbuffer.height};
	if (!rect_intersect(damage, screen, &damage))
		return (rect_t){0, 0, 0, 0};

	// Layer 0: desktop (same coordinates both sides — straight copy), UNLESS
	// a window already covers this rect completely, in which case these
	// pixels are about to be painted over and nobody can see them. Since the
	// desktop shell is a fullscreen window, that is the common case now, and
	// skipping it is what keeps a drag from costing two screenful blits per
	// damage rect (wm_rect_is_covered carries the measurement).
	if (!wm_rect_is_covered(damage))
		surface_blit(&kBackbuffer, damage.x, damage.y, &kDesktop, damage);

	// Layer 1: windows, bottom-up by z-order.
	wm_composite(&kBackbuffer, damage);

	// Layer 2: the interactive-resize outline, when one is up. No-op
	// otherwise, and the check is one pointer compare per damage rect.
	band_composite(&kBackbuffer, damage);

	// Layer 3: the Alt+Tab strip. Above the band because it is the more modal
	// of the two — though in practice they cannot coexist, one needing a
	// Ctrl+Alt mouse drag and the other Alt held on the keyboard.
	switcher_composite(&kBackbuffer, damage);

	// Cursor last, and clipped to this rect like everything else. The old code
	// drew the FULL cursor art on the argument that stray pixels were harmless
	// (the cursor is topmost, so anywhere it paints is somewhere it really is).
	// That argument still holds — but "writes stay inside the damage rect" is
	// now a contract the whole multi-rect frame depends on, and a layer that
	// keeps its own private exemption is how the next person gets bitten.
	rect_t overlap;
	if (rect_intersect(cursor_rect(), damage, &overlap)) {
		surface_t view = surface_view(&kBackbuffer, damage);
		surface_blit_masked(&view, s_cursor_x - damage.x, s_cursor_y - damage.y,
		                    s_cursor_pixels, s_cursor_mask, CURSOR_W, CURSOR_H);
	}

	return damage;
}

// ---------------------------------------------------------------------------
// Event routing (kGuiLock held): hit-test clicks to windows, drive
// click-to-focus/raise and titlebar dragging, and translate whatever belongs
// to a window into content-local coordinates on its queue.
// ---------------------------------------------------------------------------
static window_t *s_drag_window = NULL;
static int32_t s_drag_dx, s_drag_dy;   // grab offset: cursor minus frame origin

// An Alt+F4 escalation decided by the router (under kGuiLock) and carried
// out by the frame loop after the lock drops. The owner's task id, 0 = none.
static uint64_t s_terminate_owner = 0;

// Double-click detection on titlebars (maximize toggle). By id, not pointer
// — the first click's window may be dead by the second click.
#define DOUBLE_CLICK_TICKS (TICKS_PER_SECOND / 2)
static uint32_t s_titlebar_click_window = 0;
static uint64_t s_titlebar_click_tick = 0;

// ── Alt+Tab: cycling focus from the keyboard (2026-08-23) ───────────────────
//
// The first QoL chord, and the one every other window-management feature
// leans on: minimize has nowhere to restore FROM without it (there is no
// taskbar and no launcher yet), so it goes first.
//
// The shape is the 1990 one (Windows 3.0 introduced it; every WM since has
// kept it because it is right): while Alt is HELD, each Tab steps one window
// further down the MOST-RECENTLY-FOCUSED order as it stood WHEN THE HOLD
// BEGAN, and a STRIP of window titles shows where the step has landed.
//
// THE FIRST VERSION HAD NO STRIP, and raised each window as it was stepped
// to — the raise WAS the feedback, on the argument that at this size nothing
// more was needed. It cost one rule that had to go: "stepping onto a
// minimized window restores it, and it stays restored if the walk moves on."
// Chris, the first afternoon minimize existed: "I minimized everything, and
// the only way back to the gterm I was on was to Alt+Tab through a bunch of
// things, which brought them all back up." Walking past a window is not a
// request to un-hide it, and with raise-as-feedback there was no way to say
// so — showing you where you were REQUIRED disturbing the scene.
//
// A visible list is the other way, and it is what every desktop grew for
// exactly this reason: Windows 3.1's (1992) was a box of icons. os64's is a
// box of TITLES, because os64 windows have no icons and a title is what they
// have. With the strip up, the scene can hold still — see
// switcher_composite. Release Alt and the hold
// ends; the next hold starts from the new order, which makes a quick Alt+Tab
// a toggle between the two most recent windows, exactly the case people
// reach for. (Recency, not z-order: the first version walked the stack, and
// pin-on-top broke it the same afternoon — a pinned window holds the top of
// the stack while focus goes elsewhere, so "second from the top" stopped
// meaning "the one I was just using". window_t.focusSerial is the fact.)
//
// Why a SNAPSHOT and not a depth counter against the live order: every raise
// rewrites it. With windows A B C D, three presses walking "index = press
// count" do reach B, C, D — but the fourth lands on index 0, which is now D
// again, and the cycle never returns to A. Walking a copy taken at hold
// start returns to A on the fourth press, by construction.
// The copy holds IDS, not pointers: a window can die mid-hold (its task
// exits), and wm_window_by_id answers NULL for the dead where a pointer
// would have answered with freed memory — the exact bug the resize slice
// found in the drag state.
//
// Shift+Alt+Tab walks the other way. Sixteen windows is the most a snapshot
// holds; past that the cycle simply covers the top sixteen, which is more
// windows than this desktop has ever shown at once.

// THE SNAPSHOT is taken once, at the first Tab, and holds everything the
// strip needs to draw itself: ids, titles, and which entries were minimized.
// Not pointers — a window can die mid-hold (its task exits), and an id lets
// wm_window_by_id answer NULL for the dead where a pointer would answer with
// freed memory (the exact bug the resize slice found in the drag state).
//
// Copying the TITLES too, rather than reading them back through the window
// on every repaint, is the same argument one level up: the draw path then
// dereferences nothing at all, and the picture the user is choosing from
// cannot reflow under their hand halfway through a walk. Nothing in the
// scene is allowed to change during a hold anyway, so a live re-read could
// only ever differ from the snapshot by being wrong.
static bool     s_alttab_active = false;
static uint32_t s_alttab_ring[ALTTAB_RING_MAX];
static char     s_alttab_titles[ALTTAB_RING_MAX][GUI_WINDOW_TITLE_MAX];
static uint8_t  s_alttab_title_len[ALTTAB_RING_MAX];
static bool     s_alttab_dim[ALTTAB_RING_MAX];   // minimized when the hold began
static size_t   s_alttab_count = 0;
static size_t   s_alttab_step  = 0;

// Set when Escape cancelled a hold, so the router can swallow that same
// Escape's release edge (see the cancel arm in route_event_locked).
static bool     s_alttab_eat_esc_up = false;

// The strip's pixels, computed once per hold by switcher_layout_locked.
static bool     s_alttab_shown = false;
static rect_t   s_alttab_strip = {0, 0, 0, 0};
static int32_t  s_alttab_row_w = 0;
static int32_t  s_alttab_chars = 0;   // characters that fit in one row

// Strip geometry. The boot PSF1 font is 8x16, which sets every number here.
#define SWITCHER_PAD      8    // left/right breathing room inside a row
#define SWITCHER_ROW_H    24   // pitch: a 16px glyph cell with 4px above and below
#define SWITCHER_ROW_GAP  1    // see switcher_composite — the separator IS this gap
#define SWITCHER_BORDER   4    // strip frame around the column of rows
#define SWITCHER_MARGIN   16   // smallest gap the strip keeps from a screen edge
#define SWITCHER_ROW_MIN_W (8 * 8 + 2 * SWITCHER_PAD)   // eight characters, floor

// Size and centre the strip for the ring currently snapshotted.
//
// THE LIST IS VERTICAL, one window per row (ruled 2026-08-23 by Chris, who
// asked what was in the boxes and thereby found the answer). The horizontal
// row of tiles every desktop shows works because those tiles hold ICONS:
// square, small, sixteen across a screen without complaint. os64 has no
// icons — a window has a TITLE, and titles are wide and variable-length,
// which is the one shape a horizontal strip handles worst. The arithmetic
// is brutal: sixteen windows sharing 1024 pixels leaves 61 per cell, which
// at 8 pixels a glyph is FIVE CHARACTERS — "gterm", "scrib", "bounc" — and
// it gets worse the more descriptive the titles become, which is exactly
// the direction they are heading. The same sixteen rows stacked cost 384
// pixels of height, fit inside 640x480 with room to spare, and show all 32
// characters at a comfortable width.
//
// The lineage agrees. The vertical list of window titles is the older and
// specifically Unix answer — twm's window menu (1987), then fvwm's
// WindowList (1993) — arrived at by people who likewise had no icons to
// show; even Microsoft's pre-icon Task List (Alt+Esc, Windows 3.0) was a
// vertical list box, and went horizontal only once there were icons for it.
//
// ROWS ARE UNIFORM WIDTH, not sized to their own titles: surface_draw_text
// paints an OPAQUE background per glyph, so a title allowed to run past its
// row would repaint the frame beside it. Clipping to a character count
// computed from the row width makes the spill impossible rather than merely
// unlikely.
static void switcher_layout_locked(void)
{
	int32_t screen_w = (int32_t)kBackbuffer.width;
	int32_t screen_h = (int32_t)kBackbuffer.height;

	int32_t longest = 0;
	for (size_t i = 0; i < s_alttab_count; i++)
		if ((int32_t)s_alttab_title_len[i] > longest)
			longest = (int32_t)s_alttab_title_len[i];

	int32_t row_w = 8 * longest + 2 * SWITCHER_PAD;
	if (row_w < SWITCHER_ROW_MIN_W)
		row_w = SWITCHER_ROW_MIN_W;

	// A full-width row on a very narrow framebuffer: clip the titles rather
	// than run off the glass. (32 characters plus padding is 272px, so this
	// only bites below a 320-wide screen — the clamp is the belt to that
	// pair of suspenders.)
	int32_t available_w = screen_w - 2 * SWITCHER_BORDER - 2 * SWITCHER_MARGIN;
	if (row_w > available_w)
		row_w = available_w;

	s_alttab_row_w = row_w;
	s_alttab_chars = (row_w - 2 * SWITCHER_PAD) / 8;
	if (s_alttab_chars < 0)
		s_alttab_chars = 0;

	// Height: rows at SWITCHER_ROW_H pitch, the last one ending flush with
	// the frame (hence the one subtracted gap — a trailing separator would
	// read as lopsided padding).
	s_alttab_strip.w = row_w + 2 * SWITCHER_BORDER;
	s_alttab_strip.h = SWITCHER_ROW_H * (int32_t)s_alttab_count
	                 - SWITCHER_ROW_GAP + 2 * SWITCHER_BORDER;
	s_alttab_strip.x = (screen_w - s_alttab_strip.w) / 2;
	s_alttab_strip.y = (screen_h - s_alttab_strip.h) / 2;
	if (s_alttab_strip.x < 0)
		s_alttab_strip.x = 0;
	if (s_alttab_strip.y < 0)
		s_alttab_strip.y = 0;
}

// Republish the whole strip. The rubber band damages its four thin edges
// because it is window-sized and moves on every mouse packet; the strip is
// small, moves once per keystroke, and repaints its highlight in the MIDDLE
// of itself — so the union is both simpler and cheaper than the bookkeeping
// that would avoid it.
static void switcher_damage_locked(void)
{
	if (s_alttab_shown)
		gui_damage_add_locked(s_alttab_strip);
}

// Draw the strip into the damaged region. Same contract as band_composite:
// everything in VIEW coordinates, so surface.c's clipping makes "writes stay
// inside the damage rect" structural rather than remembered.
static void switcher_composite(surface_t *backbuffer, rect_t damage)
{
	if (!s_alttab_shown)
		return;

	surface_t view = surface_view(backbuffer, damage);
	int32_t sx = s_alttab_strip.x - damage.x;
	int32_t sy = s_alttab_strip.y - damage.y;

	// Frame: a filled slab in the unfocused border colour with the focused
	// border colour outlining it — the same two greys the chrome uses to say
	// "this one is live", one level up from a window.
	surface_fill_rect(&view, (rect_t){sx, sy, s_alttab_strip.w, s_alttab_strip.h},
	                  WINDOW_BORDER_UNFOCUSED);
	surface_draw_rect(&view, (rect_t){sx, sy, s_alttab_strip.w, s_alttab_strip.h},
	                  WINDOW_BORDER_FOCUSED);

	// NO SEPARATOR LINES. Each row is a tile laid on the dark slab and one
	// pixel shorter than its pitch, so the frame itself shows through between
	// them: the gap IS the separator. It costs no colour, no extra draw and
	// no decision about how dark a divider should be, and it makes the
	// highlighted row read as a raised tile rather than a painted stripe —
	// which still works when two adjacent rows are the same grey.
	for (size_t i = 0; i < s_alttab_count; i++) {
		rect_t row = {sx + SWITCHER_BORDER,
		              sy + SWITCHER_BORDER + (int32_t)i * SWITCHER_ROW_H,
		              s_alttab_row_w, SWITCHER_ROW_H - SWITCHER_ROW_GAP};
		uint32_t bg = (i == s_alttab_step) ? WINDOW_TITLEBAR_FOCUSED
		                                   : WINDOW_TITLEBAR_UNFOCUSED;
		surface_fill_rect(&view, row, bg);

		// A MINIMIZED WINDOW IS IN THE RING AND DRAWN DIM. That is the whole
		// point of the slice: "bring back the one I hid" has to be a visible
		// choice rather than a guess, and dim-but-present says both halves —
		// it is here, and it is not on the glass.
		int32_t len = (int32_t)s_alttab_title_len[i];
		if (len > s_alttab_chars)
			len = s_alttab_chars;
		if (len > 0)
			surface_draw_text(&view, row.x + SWITCHER_PAD,
			                  row.y + (SWITCHER_ROW_H - SWITCHER_ROW_GAP - 16) / 2,
			                  s_alttab_titles[i], (size_t)len,
			                  // LIGHT grey, not GUI_COLOR_GRAY: the dim title
			                  // has to stay legible on BOTH row colours, and
			                  // its worst case is the one that matters most —
			                  // a minimized window that is also the highlighted
			                  // one, i.e. the window you are in the act of
			                  // choosing to bring back. 0x808080 on the
			                  // focused blue was muddy; this reads clearly and
			                  // is still obviously not white.
			                  s_alttab_dim[i] ? GUI_COLOR_LIGHT_GRAY : GUI_COLOR_WHITE,
			                  bg);
	}
}

// Take the snapshot the strip draws from: recency order, titles, dim flags.
static void alttab_snapshot_locked(void)
{
	s_alttab_count = wm_recency_ids(s_alttab_ring, ALTTAB_RING_MAX);
	for (size_t i = 0; i < s_alttab_count; i++) {
		window_t *w = wm_window_by_id(s_alttab_ring[i]);
		size_t n = 0;
		if (w != NULL) {
			while (n < GUI_WINDOW_TITLE_MAX && w->title[n] != '\0') {
				s_alttab_titles[i][n] = w->title[n];
				n++;
			}
			s_alttab_dim[i] = wm_is_hidden(w);
		} else {
			s_alttab_dim[i] = false;   // cannot happen: the ids came from the live list
		}
		s_alttab_title_len[i] = (uint8_t)n;
	}
}

static void alttab_step_locked(bool backwards)
{
	bool first = !s_alttab_active;
	if (first) {
		alttab_snapshot_locked();
		s_alttab_step = 0;
		s_alttab_active = true;
		if (s_alttab_count >= 2) {
			switcher_layout_locked();
			s_alttab_shown = true;
		}
	}
	if (s_alttab_count < 2)
		return;   // nothing to cycle between; the hold still starts, harmlessly

	// THE FIRST STEP FROM THE WALLPAPER LANDS ON THE MOST RECENT APP (Codex
	// #31 rd3). The ring never lists the desktop, so when the desktop holds
	// focus the focused window is absent from the snapshot and entry 0 is
	// not "where you already are" — it is the app you most recently used.
	// Advancing past it, as every other first step does, skipped that app
	// and offered the second one. When the focus is not in the ring, the
	// first step stays on 0.
	if (first) {
		window_t *focus = wm_focused();
		if (focus == NULL || s_alttab_ring[0] != focus->id)
			goto chosen;
	}
	s_alttab_step = backwards
		? (s_alttab_step + s_alttab_count - 1) % s_alttab_count
		: (s_alttab_step + 1) % s_alttab_count;

chosen:
	// A STEP MOVES THE HIGHLIGHT AND NOTHING ELSE. No raise, no restore, no
	// focus change, no recency stamp — the z-order the user had is the
	// z-order they keep until they let go, and exactly one window changes at
	// the end of the hold. (PASSING THROUGH IS NOT USING — Chris's nit, the
	// first hour Alt+Tab existed on the P5 — used to need a save/restore of
	// focusSerial around the raise. Now it needs nothing, because nothing
	// happens.)
	printd(DEBUG_GUI, "guicomp: alt-tab step %lu/%lu -> window %u\n",
		(uint64_t)s_alttab_step, (uint64_t)s_alttab_count,
		s_alttab_ring[s_alttab_step]);
	switcher_damage_locked();
}

// The hold is over. `commit` false = Escape was pressed: the strip goes and
// the scene is left exactly as it was found, which is the promise that makes
// walking the ring safe to do idly.
//
// Committing acts on the HIGHLIGHTED window, not on whatever holds focus:
// nothing moved focus during the hold, so the highlight is the only record of
// what the user chose. Restore it if it was minimized, raise it otherwise —
// both paths focus and stamp recency through wm_raise, so the window you
// released on becomes the most recent and the quick Alt+Tab that follows
// bounces back to where you came from.
static void alttab_end_locked(bool commit)
{
	s_alttab_active = false;
	if (s_alttab_shown) {
		s_alttab_shown = false;
		gui_damage_add_locked(s_alttab_strip);   // erase: repaint what was under it
	}

	if (commit && s_alttab_count > 0) {
		window_t *w = wm_window_by_id(s_alttab_ring[s_alttab_step]);
		if (w == NULL) {
			// Chosen window died mid-hold. Nothing to switch to, and nothing
			// to clean up — the id was the whole of our hold on it.
			printd(DEBUG_GUI, "guicomp: alt-tab chose window %u, which is gone\n",
				s_alttab_ring[s_alttab_step]);
		} else if (wm_is_hidden(w)) {
			wm_set_minimized(w, false);   // back on the glass, raised, focused
		} else {
			wm_raise(w);
		}
	}
	printd(DEBUG_GUI, "guicomp: alt-tab hold %s\n",
		commit ? "ended" : "cancelled");
}

// ── Interactive resize: the Ctrl+Alt gesture and its rubber band ────────────
//
// THE GESTURE (ruled 2026-08-19). Ctrl+Alt held: left-drag moves the window,
// right-drag resizes it — anywhere in the window, no handle to hit. This is
// fvwm's bargain, and before fvwm it was uwm's and mwm's: the pointer plus a
// modifier is a complete window-management vocabulary, so the chrome does not
// have to grow targets for it. os64 gets three things out of choosing it
// first: no chrome change at all (composite_one is untouched), no 1-pixel
// border to hit (the border stays a hairline because nobody has to grab it),
// and it still works on a window dragged half off the screen — the case
// corner grips famously cannot reach. Ctrl+Alt rather than plain Alt because
// Alt belongs to the terminal stack here (Alt+arrows, Alt+F1..F8 switch VTs
// and are consumed before any of this); GRAPHICS.md's VT8 ruling named
// Ctrl+Alt as the escape hatch for exactly this day.
//
// THE BAND. A resize shows a wireframe outline and a live size readout; the
// window itself does not change until the button comes up. That is twm's
// 1987 look, and the reason to start there is not nostalgia: the app receives
// exactly ONE resize event per gesture instead of one per mouse packet, so a
// program that repaints slowly cannot be dragged into a repaint storm it will
// never catch up with. Opaque (live) resize is the natural upgrade once
// somebody wants it — it is the same commit, just called from the MOVE arm
// instead of the UP arm.
//
// The readout reports CONTENT size, not frame size: the client area is what
// an app actually gets, and for a terminal it is the number that matters.
static window_t *s_band_window = NULL;   // the window being rubber-banded
static rect_t    s_band_rect;            // the outline, screen coords, as drawn
static rect_t    s_band_origin;          // its frame when the gesture began
static int32_t   s_band_anchor_x, s_band_anchor_y;   // where the press landed
// Which edges follow the pointer, chosen from the quadrant the press landed
// in: grab the left half and the LEFT edge moves (so the right edge stays
// pinned), grab the top half and the top edge moves. mwm's rule, and the one
// that makes "drag toward the corner you grabbed" mean what it looks like.
static bool      s_band_west, s_band_north;

#define BAND_THICKNESS   2
#define BAND_COLOR       GUI_COLOR_WHITE
// The top strip of the outline is damaged this tall rather than
// BAND_THICKNESS tall, because the size readout is drawn inside it.
#define BAND_LABEL_STRIP 24

// Minimal unsigned-to-decimal, because the band needs a string and the
// kernel's formatter writes to the log rather than to a buffer. Returns the
// length written; the caller sizes `out` (a 32-bit pixel count is 10 digits).
static size_t band_utoa(uint32_t v, char *out)
{
	char tmp[12];
	size_t n = 0;
	do {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v != 0);
	for (size_t i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

// Damage the four edges of an outline instead of the rectangle it encloses.
// The whole point of the band is that it costs four thin strips per frame
// rather than one window-sized recomposite; unioning them here would hand
// back exactly the cost we are avoiding.
static void band_damage_locked(rect_t b)
{
	if (rect_is_empty(b))
		return;
	gui_damage_add_locked((rect_t){b.x, b.y, b.w, BAND_LABEL_STRIP});
	gui_damage_add_locked((rect_t){b.x, b.y + b.h - BAND_THICKNESS, b.w, BAND_THICKNESS});
	gui_damage_add_locked((rect_t){b.x, b.y, BAND_THICKNESS, b.h});
	gui_damage_add_locked((rect_t){b.x + b.w - BAND_THICKNESS, b.y, BAND_THICKNESS, b.h});
}

// Draw the band into the damaged region. Same contract as composite_one:
// everything is done in VIEW coordinates so surface.c's clipping makes
// "writes stay inside the damage rect" structural rather than remembered.
static void band_composite(surface_t *backbuffer, rect_t damage)
{
	if (s_band_window == NULL)
		return;

	surface_t view = surface_view(backbuffer, damage);
	rect_t b = {s_band_rect.x - damage.x, s_band_rect.y - damage.y,
	            s_band_rect.w, s_band_rect.h};

	for (int32_t i = 0; i < BAND_THICKNESS; i++)
		surface_draw_rect(&view,
		                  (rect_t){b.x + i, b.y + i, b.w - 2 * i, b.h - 2 * i},
		                  BAND_COLOR);

	// "640x480" — the CONTENT the client will be handed, derived with the
	// same chrome inset wm_resize will apply to the frame we commit.
	char label[24];
	size_t len = 0;
	int32_t content_w = s_band_rect.w - 2 * wm_border_width(s_band_window->flags);
	int32_t content_h = s_band_rect.h - wm_chrome_top(s_band_window->flags) - wm_border_width(s_band_window->flags);
	if (content_w < 0) content_w = 0;
	if (content_h < 0) content_h = 0;
	len += band_utoa((uint32_t)content_w, label + len);
	label[len++] = 'x';
	len += band_utoa((uint32_t)content_h, label + len);
	surface_draw_text(&view, b.x + BAND_THICKNESS + 4, b.y + BAND_THICKNESS + 3,
	                  label, len, GUI_COLOR_WHITE, GUI_COLOR_BLACK);
}

// Recompute the outline from the current cursor position and republish the
// damage. Called on every MOUSE_MOVE while a band is up.
static void band_track_locked(int32_t x, int32_t y)
{
	int32_t dx = x - s_band_anchor_x;
	int32_t dy = y - s_band_anchor_y;

	rect_t f = s_band_origin;
	// A west/north edge moving means the ORIGIN moves and the extent shrinks
	// by the same amount — the opposite corner stays exactly where it is,
	// which is the entire visual promise of grabbing a corner.
	if (s_band_west) {
		f.x += dx;
		f.w -= dx;
	} else {
		f.w += dx;
	}
	if (s_band_north) {
		f.y += dy;
		f.h -= dy;
	} else {
		f.h += dy;
	}

	// Preview EXACTLY what the commit will do — same function, so the outline
	// cannot promise a size wm_resize would then clamp away.
	rect_t clamped = wm_clamp_frame(s_band_window, f);
	// Clamping shrinks from the far edge; when the near edge is the one
	// moving, re-pin the far edge so the window grows out of the corner the
	// user grabbed instead of sliding.
	if (s_band_west)
		clamped.x = s_band_origin.x + s_band_origin.w - clamped.w;
	if (s_band_north)
		clamped.y = s_band_origin.y + s_band_origin.h - clamped.h;

	if (clamped.x == s_band_rect.x && clamped.y == s_band_rect.y &&
	    clamped.w == s_band_rect.w && clamped.h == s_band_rect.h)
		return;   // sub-pixel wobble inside a clamp: nothing to redraw

	band_damage_locked(s_band_rect);   // erase where it was
	s_band_rect = clamped;
	band_damage_locked(s_band_rect);   // draw where it is
}

// THE IMPLICIT POINTER GRAB (2026-08-21). While a button is down on a
// client's content area, that client owns the pointer: every move and the
// release go to IT, wherever the cursor has wandered to.
//
// The WM's own gestures had this from birth (s_drag_window, s_band_window,
// and the comment above them says why: "a gesture in progress OWNS the
// pointer... losing this is how a drag ends up half-delivered to three
// different windows"). Clients did not, and hit-testing every event is not
// the same promise: a drag-select that leaves the window lost its tail, and
// the BUTTON_UP that ends the gesture was delivered to whatever happened to
// be under the cursor — a phantom release for a stranger, and no release at
// all for the app that was mid-drag. scribe wore that as a quirk for a day;
// gterm's select-is-copy would have worn it as "the copy sometimes doesn't
// happen", which is worse because it is silent.
//
// X11 named this in 1987 — the IMPLICIT PASSIVE GRAB: a button press grabs
// the pointer for the client until the last button comes up. Every window
// system since has one, because every window system without one grew this
// same bug.
static window_t *s_pointer_window = NULL;

void gui_grab_release(const struct window *w)
{
	// Called from wm_destroy under kGuiLock — the one moment a dying window
	// can un-name itself before its memory goes back. Compare only; never
	// dereference (the caller is mid-teardown).
	if (s_drag_window == (const window_t *)w)
		s_drag_window = NULL;
	if (s_pointer_window == (const window_t *)w)
		s_pointer_window = NULL;   // a window that dies mid-drag lets go
	if (s_band_window == (const window_t *)w) {
		band_damage_locked(s_band_rect);   // erase the orphaned outline
		s_band_window = NULL;
	}
}

// Deliver a mouse event to a window, in ITS coordinates. `require_inside`
// is the difference between routing and grabbing: a hit-tested event must
// land in the content area (chrome clicks are the window system's business,
// not the client's), while a GRABBED event is delivered wherever the pointer
// has got to — including coordinates outside the window, which is exactly
// what an app tracking a drag past its own edge needs to see. Returns
// whether the client got it.
static bool deliver_mouse_to_window(window_t *w, input_event_t ev,
                                    bool require_inside)
{
	rect_t content = wm_content_rect_on_screen(w);
	if (require_inside && !rect_contains_point(content, ev.mouse.x, ev.mouse.y))
		return false;
	ev.mouse.x -= content.x;
	ev.mouse.y -= content.y;
	wm_deliver_event(w, &ev);
	return true;
}

// Is the window-management chord held? BOTH modifiers, so a plain Ctrl-click
// or Alt-click still reaches the app underneath — the chord has to be
// deliberate, because it overrides whatever the window itself wanted the
// click to mean.
static inline bool wm_chord_held(const input_event_t *ev)
{
	const uint8_t both = KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT;
	return (ev->mouse.modifiers & both) == both;
}

static void route_event_locked(const input_event_t *ev)
{
	// THE INPUT FORK (2026-08-21). Mouse events belong to whoever holds the
	// glass: the window system when VT8 is up, the focused TEXT TERMINAL
	// otherwise — where they become gpm's old gesture, select-to-copy and
	// right-click-to-paste (vt_select.c). Keys are NOT forked here: the
	// keyboard driver routes those at its own end, tty by tty, and always
	// did. Only the pointer had nowhere to go.
	if (!gui_owns_glass()) {
		switch (ev->type) {
		case INPUT_EVENT_MOUSE_MOVE:
		case INPUT_EVENT_MOUSE_BUTTON_DOWN:
		case INPUT_EVENT_MOUSE_BUTTON_UP:
			vtsel_mouse_event(ev);   // state only; painting waits for the frame
			return;
		default:
			break;
		}
	}

	switch (ev->type) {
	case INPUT_EVENT_MOUSE_MOVE: {
		// Damage where the cursor WAS and where it lands; the recomposite
		// repaints the scene under the old position.
		rect_t moved = cursor_rect();
		s_cursor_x = ev->mouse.x;
		s_cursor_y = ev->mouse.y;
		moved = rect_union(moved, cursor_rect());
		gui_damage_add_locked(moved);

		// A gesture in progress OWNS the pointer: no hit-testing, no delivery
		// to whatever the cursor happens to sweep across. Losing this is how
		// a drag ends up half-delivered to three different windows.
		if (s_band_window)
			band_track_locked(ev->mouse.x, ev->mouse.y);
		else if (s_drag_window)
			wm_move(s_drag_window,
			        ev->mouse.x - s_drag_dx, ev->mouse.y - s_drag_dy);
		else if (s_pointer_window)
			deliver_mouse_to_window(s_pointer_window, *ev, false);
		else {
			window_t *under = wm_topmost_at(ev->mouse.x, ev->mouse.y);
			if (under)
				deliver_mouse_to_window(under, *ev, true);
		}
		break;
	}
	case INPUT_EVENT_MOUSE_BUTTON_DOWN: {
		if (s_band_window || s_drag_window)
			break;   // a second button during a gesture is not a new gesture

		window_t *w = wm_topmost_at(ev->mouse.x, ev->mouse.y);
		if (!w)
			break;   // desktop click: nothing to do (yet)
		// The modifiers are logged because a chord that does not fire looks
		// exactly like a chord that was never held — this line is the
		// difference between those two, and it costs one printd per click.
		printd(DEBUG_GUI, "guicomp: button %u down on window %u at (%d,%d), mods 0x%02x\n",
			ev->mouse.button, w->id, ev->mouse.x, ev->mouse.y, ev->mouse.modifiers);
		wm_raise(w);

		// The chord's two verbs (see the gesture comment above the band
		// state): left moves, right resizes, anywhere in the window.
		//
		// NOT ON THE DESKTOP. The wallpaper is a window, so without this
		// check Ctrl+Alt+drag on empty desktop MOVED it, and the right-button
		// chord shrank it to reveal the test pattern around it (Fable's
		// review, 2026-08-25). Move and resize have no wm_ setter of their
		// own to guard — wm_move/wm_resize are the WM's internal verbs and
		// the chord is their only outside caller — so the decline lives at
		// the gesture, the same way Alt+F4's does.
		if (wm_chord_held(ev) && (w->flags & GUI_WINDOW_DESKTOP)) {
			printd(DEBUG_GUI, "guicomp: chord move/resize declined — window %u is the desktop\n",
				w->id);
		} else if (wm_chord_held(ev)) {
			if (ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT) {
				s_drag_window = w;
				s_drag_dx = ev->mouse.x - w->frame.x;
				s_drag_dy = ev->mouse.y - w->frame.y;
			} else if (ev->mouse.button == INPUT_MOUSE_BUTTON_RIGHT) {
				s_band_window = w;
				s_band_origin = w->frame;
				s_band_rect   = w->frame;
				s_band_anchor_x = ev->mouse.x;
				s_band_anchor_y = ev->mouse.y;
				// The quadrant picks the edges: grabbing in the left half
				// moves the left edge, the top half moves the top edge.
				s_band_west  = (ev->mouse.x - w->frame.x) < w->frame.w / 2;
				s_band_north = (ev->mouse.y - w->frame.y) < w->frame.h / 2;
				band_damage_locked(s_band_rect);   // paint the initial outline
				printd(DEBUG_GUI, "guicomp: band resize on window %u from %dx%d (%s%s corner)\n",
					w->id, w->frame.w, w->frame.h,
					s_band_north ? "N" : "S", s_band_west ? "W" : "E");
			}
			break;
		}

		if (ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT &&
		    wm_point_in_titlebar(w, ev->mouse.x, ev->mouse.y)) {
			// DOUBLE-CLICK ON THE TITLEBAR MAXIMIZES (and restores) — the
			// 1990 habit, kept because it is the one thing everyone tries
			// first. Two presses on the SAME window's bar within the
			// double-click window make the second one a toggle instead of
			// a drag. Windows' default interval has been 500ms since 3.0.
			if (w->id == s_titlebar_click_window &&
			    ev->tick - s_titlebar_click_tick <= DOUBLE_CLICK_TICKS) {
				s_titlebar_click_window = 0;   // a third click starts over
				bool max = !(w->flags & GUI_WINDOW_MAXIMIZED);
				printd(DEBUG_GUI, "guicomp: titlebar double-click: window %u %s\n",
					w->id, max ? "maximized" : "restored");
				wm_set_maximized(w, max);
				break;
			}
			s_titlebar_click_window = w->id;
			s_titlebar_click_tick = ev->tick;
			// Grab for dragging; remember where in the frame we grabbed so
			// the window doesn't jump under the cursor.
			s_drag_window = w;
			s_drag_dx = ev->mouse.x - w->frame.x;
			s_drag_dy = ev->mouse.y - w->frame.y;
		} else if (deliver_mouse_to_window(w, *ev, true)) {
			// It landed in the client's own area, so the grab begins here —
			// and only here: a press on chrome belongs to the window system,
			// and grabbing for it would hand the client a release it never
			// asked for.
			s_pointer_window = w;
		}
		break;
	}
	case INPUT_EVENT_MOUSE_BUTTON_UP: {
		// The band commits on release — one wm_resize, therefore one resize
		// event for the app, no matter how far the pointer travelled. The
		// chord is deliberately NOT re-checked here: the gesture belongs to
		// the button that started it, so letting go of Ctrl+Alt mid-drag
		// finishes the resize instead of abandoning a window at whatever size
		// the outline happened to be. (Every WM that got this wrong taught
		// its users to release the mouse first, which is not a lesson worth
		// teaching.)
		if (s_band_window && ev->mouse.button == INPUT_MOUSE_BUTTON_RIGHT) {
			window_t *w = s_band_window;
			rect_t final = s_band_rect;
			band_damage_locked(s_band_rect);   // erase the outline
			s_band_window = NULL;
			w->flags &= ~GUI_WINDOW_MAXIMIZED;   // a hand-sized window is not "the maximized one"
			wm_resize(w, final);
			break;
		}
		if (s_drag_window && ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT) {
			s_drag_window = NULL;
			break;
		}
		if (s_pointer_window) {
			// The grab holder gets its release even if the cursor left the
			// window — that release is the END of its gesture, and an app
			// that never hears it is an app stuck mid-drag forever.
			deliver_mouse_to_window(s_pointer_window, *ev, false);
			if (ev->mouse.buttons == 0)
				s_pointer_window = NULL;   // last button up: the grab ends
			break;
		}
		window_t *under = wm_topmost_at(ev->mouse.x, ev->mouse.y);
		if (under)
			deliver_mouse_to_window(under, *ev, true);
		break;
	}
	case INPUT_EVENT_KEY_DOWN:
	case INPUT_EVENT_KEY_UP: {
		printd(DEBUG_GUI | DEBUG_DETAILED,
			"guicomp: key %s sc 0x%02x ascii 0x%02x mods 0x%02x\n",
			ev->type == INPUT_EVENT_KEY_DOWN ? "down" : "up  ",
			ev->key.scancode, (uint8_t)ev->key.ascii, ev->key.modifiers);

		// The window system's own chords are taken BEFORE delivery, so an
		// app never sees half a gesture. Alt+Tab is the whole of that set
		// today (Alt+F1..F8 and Alt+arrows never reach here — the keyboard
		// driver consumes them for the VT switch).
		//
		// TWO DIALECTS, ONE TEST (the chord-publish lesson of 2026-08-21,
		// re-learned here the same day Alt+Tab shipped: it worked in QEMU's
		// PS/2 and did nothing on the P5's USB keyboard). The scancode field
		// is PS/2 set-1 on one path and a HID usage on the other — Tab is
		// 0x0F there and 0x2B here — so the key is recognized by its ASCII,
		// which both paths translate identically. And the hold ends not on
		// "the Alt key's release event" (PS/2 never delivers one for Right
		// Alt, HID delivered none for either until today) but on the first
		// key event of any kind that arrives WITHOUT Alt in its modifiers —
		// a fact both drivers publish on every event they send.
		if (s_alttab_active && !(ev->key.modifiers & KEYBOARD_MOD_ALT))
			alttab_end_locked(true);
		// Escape abandons the hold: the strip goes away and the scene is left
		// exactly as it was found. Swallowed, since the app never saw the
		// press that opened the strip either — half a gesture is worse than
		// none. (Only while a hold is running; Escape is the app's otherwise.)
		//
		// THE SCANCODE TEST IS LOAD-BEARING, not belt-and-braces: every arrow
		// press arrives as a three-event VT100 burst whose first event also
		// carries ascii 0x1B, so `ascii == 0x1B` alone would cancel the hold
		// on the first third of the Up key immediately below. See
		// keyboard_is_escape_key for the whole trap.
		//
		// The cancel happens on the key-DOWN (the strip should go the
		// instant you say so), which leaves its key-UP arriving after the
		// hold is already over — and an app handed the release half of a
		// press it never saw is exactly the "half a gesture" this router
		// swallows Tab's release to avoid. One flag closes it, rather than
		// eating Alt+Esc unconditionally and quietly taking a chord away
		// from every app that might want it.
		if (ev->key.ascii == 0x1b &&
		    keyboard_is_escape_key(ev->key.scancode, ev->key.modifiers) &&
		    (s_alttab_active || s_alttab_eat_esc_up)) {
			if (ev->type == INPUT_EVENT_KEY_DOWN) {
				if (s_alttab_active) {
					alttab_end_locked(false);
					s_alttab_eat_esc_up = true;
				}
			} else {
				s_alttab_eat_esc_up = false;
			}
			break;
		}
		// Up/Down walk the list too, because on a VERTICAL list they are what
		// the keys look like they should do. They are available precisely
		// because the driver spends Alt+Left/Right on the virtual-terminal
		// cycle and leaves the vertical pair alone — horizontal arrows walk
		// terminals, vertical arrows walk windows.
		//
		// Arrows do not START a hold (Tab does): an Alt+Up that reached no
		// switcher would be silently stolen from whatever app wanted it.
		if (s_alttab_active) {
			int arrow = keyboard_arrow_updown(ev->key.scancode, ev->key.modifiers);
			if (arrow != 0) {
				// One press is three events (ESC, '[', 'A'/'B'). Step on the
				// last of them so a press is a step, and swallow all three so
				// no app is ever handed half an escape sequence.
				if (ev->type == INPUT_EVENT_KEY_DOWN &&
				    (ev->key.ascii == 'A' || ev->key.ascii == 'B'))
					alttab_step_locked(arrow < 0);
				break;
			}
		}
		if (ev->key.ascii == '\t' && (ev->key.modifiers & KEYBOARD_MOD_ALT)) {
			if (ev->type == INPUT_EVENT_KEY_DOWN)
				alttab_step_locked((ev->key.modifiers & KEYBOARD_MOD_SHIFT) != 0);
			break;   // the Tab's release is swallowed too: the app never saw the press
		}

		// Alt+F4: close (1990 again, and the reason the VT switch now wants
		// Ctrl under the GUI). The first press is a REQUEST to the owner —
		// INPUT_EVENT_WINDOW_CLOSE on its queue, because the window is the
		// app's and an editor with unsaved work gets to answer. The second
		// press within GUI_CLOSE_ESCALATE_TICKS, on a window that is still
		// here, is "I mean it": SIGTERM to the owning task, carried out by
		// the frame loop after the lock drops. Explicit rather than timed —
		// a stuck program is killed by the user's hand, never by a clock.
		// F-keys have no ASCII, so this is the one chord that reads the
		// scancode — through keyboard_fkey_number, which knows both
		// dialects. Both edges swallowed.
		if ((ev->key.modifiers & KEYBOARD_MOD_ALT) && !(ev->key.modifiers & KEYBOARD_MOD_CTRL) &&
		    keyboard_fkey_number(ev->key.scancode, ev->key.modifiers) == 4) {
			window_t *focus = wm_focused();
			// The desktop is exempt — and this one is guarded HERE rather
			// than at a wm_ setter because there is no setter to guard: a
			// close is a REQUEST delivered to the owner, so the only place
			// to decline it is where it is composed. Alt+F4 with nothing
			// but your desktop showing is a gesture nobody means, and its
			// second press would SIGTERM the shell.
			if (focus && (focus->flags & GUI_WINDOW_DESKTOP)) {
				printd(DEBUG_GUI, "guicomp: close declined — window %u is the desktop\n",
					focus->id);
				break;
			}
			if (focus && ev->type == INPUT_EVENT_KEY_DOWN) {
				if (focus->closeAskedTick != 0 &&
				    ev->tick - focus->closeAskedTick <= GUI_CLOSE_ESCALATE_TICKS) {
					printd(DEBUG_GUI, "guicomp: window %u asked to close twice — terminating owner %lu\n",
						focus->id, focus->owner);
					focus->closeAskedTick = 0;
					s_terminate_owner = focus->owner;
				} else {
					printd(DEBUG_GUI, "guicomp: window %u asked to close\n", focus->id);
					focus->closeAskedTick = ev->tick;
					input_event_t close = {
						.type = INPUT_EVENT_WINDOW_CLOSE,
						.tick = ev->tick,
					};
					wm_deliver_event(focus, &close);
				}
			}
			break;
		}

		// Ctrl+Alt+letter: the window-management verbs, on the FOCUSED
		// window. The same chord the mouse gestures use (GRAPHICS.md's
		// resize chapter ruled Ctrl+Alt the WM's escape hatch), so a plain
		// Ctrl+letter still reaches the app. Ctrl strips the letter to its
		// control code on both keyboard paths, so the ASCII is matched —
		// 0x10 is Ctrl+P — and the scancode, which differs between PS/2
		// and HID, is never consulted. Both edges are swallowed.
		if ((ev->key.modifiers & (KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT)) ==
		    (KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT) && ev->key.ascii >= 0x01 && ev->key.ascii <= 0x1A) {
			window_t *focus = wm_focused();
			if (focus && ev->type == INPUT_EVENT_KEY_DOWN) {
				switch (ev->key.ascii) {
				case 0x10: {   // Ctrl+Alt+P: pin on top (toggle)
					bool pin = !(focus->flags & GUI_WINDOW_PINNED);
					printd(DEBUG_GUI, "guicomp: window %u %s\n", focus->id, pin ? "pinned" : "unpinned");
					wm_set_pinned(focus, pin);
					break;
				}
				case 0x0D: {   // Ctrl+Alt+M: maximize (toggle) — 0x0D is Ctrl+M, a CR by 1963's table
					bool max = !(focus->flags & GUI_WINDOW_MAXIMIZED);
					printd(DEBUG_GUI, "guicomp: window %u %s\n", focus->id, max ? "maximized" : "restored");
					wm_set_maximized(focus, max);
					break;
				}
				// Ctrl+Alt+N: minimize. Alt+Tab brings it back — it shows in
				// the switcher strip as a dim row, and returns only if the
				// hold ENDS on it (walking past leaves it hidden).
				case 0x0E:
					printd(DEBUG_GUI, "guicomp: window %u minimized\n", focus->id);
					wm_set_minimized(focus, true);
					break;
				case 0x14: {   // Ctrl+Alt+T: titlebar (toggle)
					bool show = (focus->flags & GUI_WINDOW_NO_DECORATIONS) != 0;
					printd(DEBUG_GUI, "guicomp: window %u titlebar %s\n", focus->id, show ? "shown" : "hidden");
					wm_set_decorated(focus, show);
					break;
				}
				default:
					break;   // an unassigned letter is simply swallowed
				}
			}
			break;
		}

		window_t *focus = wm_focused();
		if (focus)
			wm_deliver_event(focus, ev);
		break;
	}
	}
}

bool guicomp_thread(bool daemon)
{
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	printd(DEBUG_GUI, "guicomp: compositor starting on APIC %u (thread=0x%08x, daemon=%u)\n",
		cls->apic_id, self->threadID, daemon);

	if (surface_init(&kBackbuffer, kFrameBuffer.width, kFrameBuffer.height) != 0 ||
	    surface_init(&kDesktop, kFrameBuffer.width, kFrameBuffer.height) != 0) {
		printd(DEBUG_GUI, "guicomp: FATAL: surface allocation failed, compositor exiting\n");
		return false;
	}
	printd(DEBUG_GUI, "guicomp: backbuffer %ux%u ready (%lu KB x2)\n",
		kBackbuffer.width, kBackbuffer.height,
		(uint64_t)kBackbuffer.width * kBackbuffer.height * 4 / 1024);

	cursor_build();
	s_cursor_x = (int32_t)kBackbuffer.width / 2;
	s_cursor_y = (int32_t)kBackbuffer.height / 2;
	gui_damage_add(cursor_rect());

	// THE FLOOR, and only the floor. The test pattern has been here since
	// the surface core came up, and it is what you see before /bin/desktop
	// starts, if it never starts, or if it dies — which is exactly why it
	// stays in the kernel now that the decor does not.
	//
	// The kernel used to paint the WALLPAPER here too: gui/desktop.c read
	// desktop.conf and decoded a PPM, in ring 0, from a file any user could
	// write. That file is deleted as of 2026-08-25 and the job belongs to
	// the desktop shell, which paints it into a real window in the bottom
	// z-band — a window, so that a click landing on no application has
	// somewhere to land.
	paint_desktop();

	// (THE "hello os64" WINDOW started here — the first window this
	// compositor ever drew, M5's proof that the client API worked, kept as a
	// legacy switch when gui.conf arrived. RETIRED 2026-08-25, Chris's call
	// on the day the desktop moved to ring 3: "lets remove it. If I want to
	// reminisce, I can run an old build." Its `hello` key was also the last
	// thing making the KERNEL read gui.conf, so retiring the window closes
	// the two-readers-of-one-file wart in the same stroke — gui.conf is
	// entirely /bin/desktop's now. git history has the window.)
	//
	// (The ring-0 console window started here from M5 until the VT8 chapter
	// retired it, 2026-08-19 — ruled: no ring-0-rendered windows. printf and
	// print_n land in VT1's grid now, one Alt+F1 away; Phase E's terminal is
	// the ring-3 heir. git history of gui/console_window.c has the grid-to-
	// pixels reference loop.)

	uint64_t frames = 0, flushes = 0;
	uint64_t last_heartbeat_tick = kTicksSinceStart;

	while (1) {
		frames++;

		// -------- Drain input, route events, recomposite (one lock hold) ---
		uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);

		input_event_t ev;
		while (input_pop(&ev))
			route_event_locked(&ev);

		// A VT switch landed on us since last frame: the whole backbuffer is
		// owed to the glass (it kept compositing while a text VT held the
		// iron — see the seated-predicate comment). Convert the ISR's one-
		// store flag into ordinary damage here, under our own lock.
		if (s_glass_regained) {
			s_glass_regained = false;
			// Whatever the console overlay had painted is gone with the VT
			// switch's repaint; it must not try to restore cells that now
			// belong to the desktop.
			vtsel_forget();
			// And a client grab that straddled the time away is stale: the
			// button that started it came up on a text console, where the
			// release went to vt_select and never reached us. Without this,
			// the first release after the return would land on a window
			// that finished its gesture a VT switch ago.
			s_pointer_window = NULL;
			// An Alt+Tab hold is stale for exactly the same reason, and it is
			// the keyboard's version of that bug: a hold ends on the first
			// key event arriving WITHOUT Alt, and while a text VT held the
			// iron every one of those went to the console instead. The hold
			// would otherwise still be open on our return — with the strip
			// repainted by the full-screen damage below, over a scene the
			// user has since stopped choosing from. Abandon it: leaving is
			// not choosing, so the scene is left exactly as it was found.
			if (s_alttab_active)
				alttab_end_locked(false);
			gui_damage_add_locked((rect_t){0, 0, (int32_t)kBackbuffer.width,
			                               (int32_t)kBackbuffer.height});
		}

		// Take the whole damage list in one hold and reset it, so clients
		// publishing during this frame's flush accumulate into the NEXT
		// frame instead of racing the one being drawn.
		rect_t damage[DAMAGE_MAX_RECTS];
		uint32_t damage_count = kPendingDamageCount;
		for (uint32_t i = 0; i < damage_count; i++)
			damage[i] = composite_locked(kPendingDamage[i]);
		kPendingDamageCount = 0;

		spinlock_release_irqrestore(&kGuiLock, irqflags);

		// A close escalation decided under the lock is carried out here,
		// outside it: signalling a task takes scheduler locks, and kGuiLock
		// must never nest under those (see gui_window_terminate_owner).
		if (s_terminate_owner != 0) {
			uint64_t owner = s_terminate_owner;
			s_terminate_owner = 0;
			gui_window_terminate_owner(owner);
		}

		// -------- Flush to the (slow, uncached) framebuffer, lock-free -----
		// One flush per surviving rect. composite_locked clipped each to the
		// screen and may have emptied it entirely; those are skipped here
		// rather than filtered above, so the indices keep matching.
		//
		// AND ONLY WHILE VT8 OWNS THE IRON (the VT8 chapter's one gate): with
		// a text terminal focused, the frame above still landed in the
		// backbuffer — current state, zero VRAM cost — and the return switch
		// pays one full-screen flush for all of it.
		// The other side of the fork: with a text terminal on the iron, this
		// frame's job is the console's mouse overlay instead of a flush.
		// OUTSIDE kGuiLock, deliberately — vtsel_paint takes the tty and
		// renderer locks, and reaching those while holding ours would invent
		// a lock order nothing else in the system has.
		if (!gui_owns_glass())
			vtsel_paint();

		if (gui_owns_glass())
		for (uint32_t i = 0; i < damage_count; i++) {
			if (rect_is_empty(damage[i]))
				continue;
			surface_flush_rect(&kBackbuffer, damage[i]);
			flushes++;
			printd(DEBUG_GUI | DEBUG_DETAILED,
				"guicomp: composited %dx%d at (%d,%d) [rect %u/%u], tick %lu\n",
				damage[i].w, damage[i].h, damage[i].x, damage[i].y,
				i + 1, damage_count, kTicksSinceStart);
		}

		if (kTicksSinceStart - last_heartbeat_tick >= TICKS_PER_SECOND) {
			printd(DEBUG_GUI, "guicomp: heartbeat, %lu frames, %lu flushes (tick %lu)\n",
				frames, flushes, kTicksSinceStart);
			last_heartbeat_tick = kTicksSinceStart;
		}

		if (!daemon)
			return true;

		// -------- Pace the next frame (see block comment above) --------
		// If there's nothing to do, HALT until the next interrupt: an input
		// IRQ (routed to this core) wakes us with work in hand; otherwise
		// the core's scheduler timer bounds the nap. At most one hlt per
		// pass, then fall through — the pass costs almost nothing when idle,
		// and falling through keeps the heartbeat/daemon checks alive.
		//
		// The cli / check / sti;hlt shape is the standard lost-wakeup-free
		// idle sequence: with IF clear an IRQ can't slip in between the
		// check and the hlt, and sti's one-instruction interrupt shadow
		// means a pending IRQ is delivered exactly AT the hlt — waking it —
		// never before it.
		//
		// (kPendingDamageCount is read unlocked here as a wake HINT only; a
		// torn read at worst wakes us early or costs one timer period. Reading
		// the COUNT rather than a rect is what makes that claim cheap: it is a
		// single aligned word, so "0 or not 0" is the only question a racing
		// reader can get wrong, and it can only get it wrong for one pass.)
		// The accounting bookends around the nap (smp_core.h): without
		// them, hlt time bills as run time — this thread is the one idler
		// the scheduler cannot see, and top spent a day showing it at 95%
		// of a core while the flush counter sat frozen. Begin settles the
		// frame's real work onto us and routes the nap to the idle thread;
		// End settles the nap and takes the meter back.
		__asm__ volatile("cli" ::: "memory");
		if (!input_pending() && kPendingDamageCount == 0 && !s_glass_regained) {
			mpAcctHaltBegin();
			__asm__ volatile("sti\n\thlt" ::: "memory");
			mpAcctHaltEnd();
		} else
			__asm__ volatile("sti" ::: "memory");
	}
}

// Pin the compositor to core 1 when we have one (kworker precedent): keeps
// steady frame work off the BSP, which handles IRQ0/signals. EXCEPT in
// tickless mode (the default): there the AP scheduler timers stay masked
// (smp_core.c enableAPScheduling_ISR) — a thread pinned to an AP runs
// un-preemptable and only when nudged, which wedged the compositor hard.
// Under tickless everything stays on the BSP's normal scheduling, which is
// why the GUI boot entries run SCHED=periodic until the damage-wake nudge
// (SCHEDULER.md) lets a pinned compositor and parked cores coexist.
uint64_t gui_compositor_affinity(void)
{
	return (kMPCoreCount > 1 && !kTicklessScheduler)
	           ? (uint64_t)kCPUInfo[1].apicID
	           : THREAD_NO_AFFINITY;
}

void gui_start(void)
{
	// THE KERNEL NO LONGER READS gui.conf (2026-08-25). It used to be read
	// right here, before anything was submitted, because the compositor
	// thread asked gui_startup_hello() while building its scene. Both the
	// window and its switch are gone, and the `start` lines belong to
	// /bin/desktop — so gui.conf has exactly one reader again, in ring 3,
	// which is the arrangement the config search path was built to protect.
	//
	// Open the unified input queue before the compositor (its consumer)
	// exists — events buffered during task startup are simply the first
	// ones drained.
	input_init();

	uint64_t affinity = gui_compositor_affinity();

	kGuiCompTask = task_create("/guicomp", 0, NULL, kKernelTask, true, affinity);
	// Pass daemon=true (first arg in RDI) to guicomp_thread
	kGuiCompTask->threads->regs.RDI = 1;
	scheduler_submit_new_task(kGuiCompTask);

	printd(DEBUG_GUI, "gui_start: compositor task 0x%08x submitted (affinity %s0x%08lx)\n",
		kGuiCompTask->taskID,
		affinity == THREAD_NO_AFFINITY ? "THREAD_NO_AFFINITY/" : "APIC ",
		affinity);

	// Demo apps — RING-3 ELFs since migration step 4 (2026-08-17). The
	// kernel-thread originals (gui/demo/) were GRAPHICS.md's placeholder
	// clients, and porting them to userland on libdraw was the stated
	// acceptance test for the whole boundary; the ports pass, so the boot
	// spawns the real thing now. What that buys, in kill(1) terms: these
	// are ordinary tasks — heap, /proc row, the syscall-boundary terminate
	// checkpoint ~100x a second, and an exit sweep for their windows — so
	// the experiment that discovered a kernel-thread ball cannot be shot
	// (Chris, 2026-08-17) now ends with a dead ball and a reclaimed window.
	// autoReap because ktask never waits (the husk-launch decree, task.c).
	// A launch failure is a report, not a boot failure: a disk image
	// without the demos is a valid image. The kernel-thread demo code
	// stays in the tree as the reference the ports were checked against;
	// nothing spawns it anymore.
	// (Non-const strings because task_create's path parameter predates
	// const-correctness — it does not modify them.)
	//
	// (THE LIST WAS gui.conf's from 2026-08-23 to 2026-08-25, read right
	// here, with the two demos as its default. What made THIS the right home
	// then: these apps belong to the DESKTOP's startup, and everything Chris
	// actually wanted at boot was stranded in husk.rc — which runs in every
	// husk (VT1 and VT2 both start one, so he got two gclocks and two gterms)
	// and runs on text boots too (where every GUI line failed once per
	// terminal). That argument still holds; what changed is WHO the desktop
	// is. It is a program now, so the list — and the demo default — moved
	// into /bin/desktop with it, and the kernel's list is the one line
	// below.)
	// THE KERNEL STARTS EXACTLY ONE GUI PROGRAM (2026-08-25): the desktop
	// shell. Everything else that starts with the desktop is started BY the
	// desktop, out of gui.conf, which is the shell's rc.
	//
	// The name is hardcoded, and that is the whole of the kernel's startup
	// policy — one line, deliberately. When husk-as-init lands, this line is
	// what moves to the init table, and /bin/desktop does not change,
	// because nothing in it ever asked who started it.
	//
	// If it is missing or fails to launch, the compositor's own test pattern
	// stays on the glass and the machine remains usable — the floor is in
	// the kernel precisely so the decor can be a program that might not be
	// there.
	static const char *const kShell[] = { "/bin/desktop" };
	for (size_t i = 0; i < sizeof(kShell) / sizeof(kShell[0]); i++) {
		const char *app = kShell[i];
		task_t *demo = task_create((char *)app, 0, NULL, kKernelTask, false,
		                           THREAD_NO_AFFINITY);
		if (demo == NULL) {
			// BOTH sinks, deliberately: printf is FRAMEBUFFER-ONLY (the
			// panic-pipeline scar), so a glass-only complaint vanishes the
			// moment the desktop paints over it — which on the P5 cost a
			// reboot and a log search that found nothing (2026-08-17, the
			// first GUI boot against a root that predated the ring-3
			// demos). The wire copy is unconditional: a missing binary at
			// boot is exactly the fact a log exists to keep.
			printf("gui_start: %s launch failed (not on the image?)\n", app);
			printd(DEBUG_BOOT, "gui_start: %s launch failed (not on the image?)\n", app);
			continue;
		}
		demo->autoReap = true;
		scheduler_submit_new_task(demo);
	}

	// Seat VT8 and take the stage (the VT8 chapter, 2026-08-19). TTY_LIVE is
	// what tells the knock-summon this terminal already has a shell — ours —
	// so a stray keystroke can never hang a husk on the GUI's VT. The focus
	// comes LAST: every boot printf after this line lands silently in VT1's
	// grid (print_n's fallback), waiting under Alt+F1, instead of scribbling
	// over the desktop the compositor is about to paint.
	kTTY[7].state = TTY_LIVE;
	s_gui_seated = true;
	tty_focus(7);
}
