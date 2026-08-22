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

static rect_t composite_locked(rect_t damage)
{
	rect_t screen = {0, 0, (int32_t)kBackbuffer.width, (int32_t)kBackbuffer.height};
	if (!rect_intersect(damage, screen, &damage))
		return (rect_t){0, 0, 0, 0};

	// Layer 0: desktop (same coordinates both sides — straight copy).
	surface_blit(&kBackbuffer, damage.x, damage.y, &kDesktop, damage);

	// Layer 1: windows, bottom-up by z-order.
	wm_composite(&kBackbuffer, damage);

	// Layer 2: the interactive-resize outline, when one is up. No-op
	// otherwise, and the check is one pointer compare per damage rect.
	band_composite(&kBackbuffer, damage);

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
	int32_t content_w = s_band_rect.w - 2 * GUI_BORDER_WIDTH;
	int32_t content_h = s_band_rect.h - GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH;
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
		if (wm_chord_held(ev)) {
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

	paint_desktop();

	// A first window, created through the CLIENT API — so M5 exercises the
	// exact path demo apps (and later, userland) will use: click it to
	// focus, drag it by the titlebar.
	int64_t hello = gui_window_create("hello os64", 340, 250, 340, 240, 0);
	if (hello > 0) {
		surface_t s;
		gui_window_get_surface(hello, &s);
		const char msg1[] = "The os64 window system lives!";
		const char msg2[] = "Drag me by the title bar.";
		surface_draw_text(&s, 12, 16, msg1, sizeof(msg1) - 1,
		                  GUI_COLOR_BLACK, GUI_COLOR_LIGHT_GRAY);
		surface_draw_text(&s, 12, 40, msg2, sizeof(msg2) - 1,
		                  GUI_COLOR_DARK_GRAY, GUI_COLOR_LIGHT_GRAY);
		gui_window_publish(hello, NULL);
	}

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

		// -------- Flush to the (slow, uncached) framebuffer, lock-free -----
		// One flush per surviving rect. composite_locked clipped each to the
		// screen and may have emptied it entirely; those are skipped here
		// rather than filtered above, so the indices keep matching.
		//
		// AND ONLY WHILE VT8 OWNS THE IRON (the VT8 chapter's one gate): with
		// a text terminal focused, the frame above still landed in the
		// backbuffer — current state, zero VRAM cost — and the return switch
		// pays one full-screen flush for all of it.
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
	static char demo_bounce[] = "/bin/gbounce";
	static char demo_keys[]   = "/bin/gkeys";
	char *demos[] = { demo_bounce, demo_keys };
	for (unsigned i = 0; i < sizeof(demos) / sizeof(demos[0]); i++) {
		task_t *demo = task_create(demos[i], 0, NULL, kKernelTask, false,
		                           THREAD_NO_AFFINITY);
		if (demo == NULL) {
			// BOTH sinks, deliberately: printf is FRAMEBUFFER-ONLY (the
			// panic-pipeline scar), so a glass-only complaint vanishes the
			// moment the desktop paints over it — which on the P5 cost a
			// reboot and a log search that found nothing (2026-08-17, the
			// first GUI boot against a root that predated the ring-3
			// demos). The wire copy is unconditional: a missing binary at
			// boot is exactly the fact a log exists to keep.
			printf("gui_start: %s launch failed (not on the image?)\n", demos[i]);
			printd(DEBUG_BOOT, "gui_start: %s launch failed (not on the image?)\n", demos[i]);
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
