// window.c — the window system (GUI layer 3): z-order, decorations,
// hit-testing, per-window event queues.
//
// LOCKING: every function here assumes the caller holds kGuiLock (see
// gui/gui_internal.h). That's what makes the singly-owned z-list and the
// SPSC-ish event queues safe with zero locking code in this file.

#include "gui/window.h"
#include "gui/gui_internal.h"
#include "os64/gui.h"    // OS64_GUI_TITLE_MAX — the ABI's copy of the title
                         // capacity, pinned to ours by the assert below (the
                         // log.c/klog_format.h precedent: renumbering stops
                         // the build instead of lying to ring 3)
_Static_assert(GUI_WINDOW_COVERED == OS64_GUI_WINDOW_COVERED,
               "the COVERED flag crosses the ring boundary through get_state");
_Static_assert(GUI_WINDOW_TITLE_MAX == OS64_GUI_TITLE_MAX,
               "gui/window.h and abi os64/gui.h disagree on the title capacity");
#include "gui/surface.h"
#include "gui/compositor.h"

#include "CONFIG.h"
#include "kernel.h"      // kTicksSinceStart — the stamp on a synthesized event
#include "kmalloc.h"
#include "memset.h"
#include "printd.h"
#include "scheduler.h"   // scheduler_wake_isleep_thread — event_wait's alarm bell
#include "strcpy.h"
#include "video.h"       // kFrameBuffer — the screen IS the canvas capacity

extern struct Framebuffer kFrameBuffer;

// Z-order list. s_top is frontmost (first hit-tested), s_bottom is nearest
// the desktop (first composited). Both NULL when no windows exist.
static window_t *s_top = NULL;
static window_t *s_bottom = NULL;
static window_t *s_focused = NULL;
static uint32_t s_next_id = 1;

// Chrome colors: the focused window gets the saturated titlebar. The four
// WINDOW_TITLEBAR_*/WINDOW_BORDER_* values moved to window.h when the Alt+Tab
// switcher became a second consumer (see the palette note there); this one
// stays private because nothing outside window.c paints a client area.
#define WINDOW_CONTENT_INITIAL    GUI_COLOR_LIGHT_GRAY

// Focus recency, see window_t.focusSerial. EVERY assignment to s_focused
// goes through here so the serial can never be forgotten at one site —
// which is the whole reason it is a function and not a field write.
static uint64_t s_focus_serial = 0;
static void focus_window(window_t *w)
{
	s_focused = w;
	if (w)
		w->focusSerial = ++s_focus_serial;
}

static void unlink_window(window_t *w)
{
	if (w->above)
		w->above->below = w->below;
	else
		s_top = w->below;
	if (w->below)
		w->below->above = w->above;
	else
		s_bottom = w->above;
	w->above = w->below = NULL;
}

// WHICH BAND A WINDOW LIVES IN. The z-list had two bands from 2026-08-23
// (pin-on-top) and has THREE since the desktop moved to ring 3: the desktop
// shell's window is a client like any other, and the only thing that makes
// it the desktop is that nothing can get beneath it.
//
//   2  PINNED   — always on top; focused, moved and typed at, never buried
//   1  ordinary — everything else
//   0  DESKTOP  — always at the bottom; the thing clicks land on when they
//                 land on nothing
//
// Expressed as a NUMBER rather than as two more branches, because the old
// two-band code already carried the warning: "three copies of find the band
// boundary is three chances to disagree". A rank makes link_on_top and
// at_band_top say the same thing by construction, and a fourth band later
// costs one line here and nothing anywhere else.
static int band_of(const window_t *w)
{
	if (w->flags & GUI_WINDOW_PINNED)  return 2;
	if (w->flags & GUI_WINDOW_DESKTOP) return 0;
	return 1;
}

// Link `w` at the top of ITS BAND: directly beneath the lowest window of any
// HIGHER band, and above everything of its own band or lower. ONE function
// does the placement for create, raise and pin/unpin alike.
static void link_on_top(window_t *w)
{
	window_t *above = NULL;   // the window that will sit directly above w
	int band = band_of(w);
	for (window_t *p = s_top; p && band_of(p) > band; p = p->below)
		above = p;

	w->above = above;
	w->below = above ? above->below : s_top;
	if (w->below)
		w->below->above = w;
	else
		s_bottom = w;
	if (above)
		above->below = w;
	else
		s_top = w;
}

// Is `w` already as high as its band allows? (wm_raise's "nothing to relink"
// test, which used to be `s_top == w` — true for an ordinary window only when
// nothing is pinned.)
static bool at_band_top(const window_t *w)
{
	return w->above == NULL || band_of(w->above) > band_of(w);
}

window_t *wm_create(const char *title, rect_t frame, uint32_t flags)
{
	// Content = frame minus chrome; refuse degenerate sizes rather than
	// letting a 0-wide surface ripple NULLs through the compositor.
	int32_t content_w = frame.w - 2 * wm_border_width(flags);
	int32_t content_h = frame.h - wm_chrome_top(flags) - wm_border_width(flags);
	if (content_w < GUI_MIN_CONTENT || content_h < GUI_MIN_CONTENT)
		return NULL;

	window_t *w = kmalloc(sizeof(window_t));
	if (!w)
		return NULL;
	memset(w, 0, sizeof(window_t));

	// Both stores are reserved at CAPACITY and report the content size — the
	// reservation that makes resize free of allocation, of pixel motion, and
	// (for the task-backed canvas gui_window_create swaps in below) of any
	// need to unmap a live user page. See canvas_cap_w's comment in window.h.
	uint32_t cap_w, cap_h;
	wm_canvas_capacity_for(content_w, content_h, &cap_w, &cap_h);
	w->canvas_cap_w = cap_w;
	w->canvas_cap_h = cap_h;

	if (surface_init_capacity(&w->content, (uint32_t)content_w, (uint32_t)content_h,
	                          cap_w, cap_h) != 0) {
		kfree(w);
		return NULL;
	}
	// The client-facing back buffer (see window.h). Filled identically to
	// content so a client's first PARTIAL present doesn't snapshot garbage
	// around its damage rect.
	if (surface_init_capacity(&w->canvas, (uint32_t)content_w, (uint32_t)content_h,
	                          cap_w, cap_h) != 0) {
		surface_free(&w->content);
		kfree(w);
		return NULL;
	}
	surface_fill_rect(&w->content,
	                  (rect_t){0, 0, content_w, content_h}, WINDOW_CONTENT_INITIAL);
	surface_fill_rect(&w->canvas,
	                  (rect_t){0, 0, content_w, content_h}, WINDOW_CONTENT_INITIAL);

	w->id = s_next_id++;
	w->flags = flags;
	w->frame = frame;
	strncpy(w->title, title ? title : "", GUI_WINDOW_TITLE_MAX);
	w->title[GUI_WINDOW_TITLE_MAX - 1] = '\0';

	link_on_top(w);
	// START_UNFOCUSED declines the focus grab a new window normally gets —
	// unless nothing holds focus yet, because SOMETHING must (an all-
	// declining boot would leave keys routing to NULL). See the flag's
	// comment in window.h for the no-mouse race that earned it.
	//
	// TITLEBARS REPAINT ON FOCUS CHANGE — the rule with three doors, and
	// this was the last to learn it (2026-08-19, Chris's find). wm_raise
	// always damaged the dethroned window; wm_destroy learned it 8/17 when
	// the first window to DIE holding focus left a half-gray, half-blue
	// titlebar; and this door — focus stolen AT BIRTH — hid until the first
	// focus-taking window was ever born onto a live desktop with a focused
	// window standing visible: `gclock &` typed inside gterm, whose titlebar
	// then kept wearing the focused blue in front of an eyewitness.
	window_t *old_focus = s_focused;
	if (!(flags & GUI_WINDOW_START_UNFOCUSED) || s_focused == NULL)
		focus_window(w);
	gui_damage_add_locked(w->frame);
	if (s_focused == w && old_focus != NULL && old_focus != w)
		gui_damage_add_locked(old_focus->frame);

	printd(DEBUG_GUI, "wm: created window %u '%s' at (%d,%d) %dx%d\n",
		w->id, w->title, frame.x, frame.y, frame.w, frame.h);
	return w;
}

void wm_destroy(window_t *w)
{
	// FIRST: whatever the compositor is holding this window BY — a titlebar
	// drag, a Ctrl+Alt gesture, a rubber band — stops naming it now, while the
	// pointer is still valid to compare. See gui_grab_release's comment for
	// the use-after-free this closes.
	gui_grab_release(w);

	gui_damage_add_locked(w->frame);   // repaint what the window covered
	if (s_focused == w)
		focus_window(w->below ? w->below : s_top);
	unlink_window(w);
	if (s_focused == w)
		focus_window(s_top);
	// If focus just moved, the inheriting window's titlebar changes color —
	// damage it, or only the slice under the dead window's frame repaints.
	// wm_raise always knew this; this path never did, and nobody noticed
	// until 2026-08-17 because no window had ever DIED holding focus: the
	// first ring-3 client exiting mid-focus left the console wearing half a
	// gray titlebar and half a blue one, split exactly at the dead window's
	// old edge — a screenshot's worth of exactly where damage tracking
	// stopped.
	if (s_focused != NULL && s_focused != w)
		gui_damage_add_locked(s_focused->frame);
	surface_free(&w->canvas);
	surface_free(&w->content);
	kfree(w);
}

window_t *wm_topmost_at(int32_t x, int32_t y)
{
	for (window_t *w = s_top; w; w = w->below)
		if (!wm_is_hidden(w) && rect_contains_point(w->frame, x, y))
			return w;
	return NULL;
}

void wm_raise(window_t *w)
{
	window_t *old_focus = s_focused;

	if (!at_band_top(w)) {
		unlink_window(w);
		link_on_top(w);
		// Newly exposed stacking: repaint the whole raised frame.
	}
	focus_window(w);

	// Titlebars repaint on focus change (color flips on both windows).
	if (old_focus && old_focus != w)
		gui_damage_add_locked(old_focus->frame);
	gui_damage_add_locked(w->frame);
}

// THE DESKTOP DECLINES THE WM VERBS THAT WOULD MAKE IT VANISH OR FLOAT.
// Guarded HERE, at the setters, rather than at the chord handlers: the
// chords are one caller among several (the client syscalls are another, and
// a taskbar will be a third), and a rule enforced at the door every caller
// walks through cannot be forgotten by the next one. Pin is included
// because "always on top" and "always at the bottom" are a contradiction,
// not a preference.
static bool desktop_declines(const window_t *w, const char *verb)
{
	if (!(w->flags & GUI_WINDOW_DESKTOP))
		return false;
	printd(DEBUG_GUI, "wm: %s declined — window %u is the desktop\n",
	       verb, w->id);
	return true;
}

void wm_set_pinned(window_t *w, bool pinned)
{
	if (desktop_declines(w, "pin"))
		return;
	if (pinned == ((w->flags & GUI_WINDOW_PINNED) != 0))
		return;
	if (pinned)
		w->flags |= GUI_WINDOW_PINNED;
	else
		w->flags &= ~GUI_WINDOW_PINNED;
	// Re-place in the new band. Pinning lifts it above everything; unpinning
	// drops it to the top of the ordinary band — still the most recent thing
	// you touched, just buriable again. Either way the stacking under its
	// frame changed, so the frame is damaged.
	unlink_window(w);
	link_on_top(w);
	gui_damage_add_locked(w->frame);
}

void wm_set_decorated(window_t *w, bool decorated)
{
	// The desktop has no chrome to toggle. wm_has_titlebar answers false for
	// it whatever this bit says (rd3), so flipping the bit would change
	// nothing on the glass — and a toggle that changes nothing but the flag
	// word is worse than one that is refused: get_state would report a
	// decoration the window does not have. (Before rd3, composite_one read
	// the bit directly, and the toggle really did paint a "desktop" titlebar
	// across the wallpaper that doubled as a drag handle — Fable's review,
	// 2026-08-25. The decline predates the fix that made it cosmetic.)
	if (desktop_declines(w, "decorate"))
		return;
	if (decorated == !(w->flags & GUI_WINDOW_NO_DECORATIONS))
		return;
	rect_t old = w->frame;
	// The content's screen position is the invariant; the frame's top edge
	// moves by the difference between the two chrome heights to keep it so.
	int32_t before = wm_chrome_top(w->flags);
	if (decorated)
		w->flags &= ~GUI_WINDOW_NO_DECORATIONS;
	else
		w->flags |= GUI_WINDOW_NO_DECORATIONS;
	int32_t after = wm_chrome_top(w->flags);
	w->frame.y += before - after;
	w->frame.h += after - before;
	gui_damage_add_locked(rect_union(old, w->frame));
}

void wm_set_maximized(window_t *w, bool maximized)
{
	// A desktop already fills the screen; "maximize" would be a no-op that
	// still clobbered restoreFrame, and un-maximizing would then shrink your
	// desktop to a window.
	if (desktop_declines(w, "maximize"))
		return;
	if (maximized == ((w->flags & GUI_WINDOW_MAXIMIZED) != 0))
		return;
	if (maximized) {
		w->restoreFrame = w->frame;
		w->flags |= GUI_WINDOW_MAXIMIZED;
		// Raised as well: a maximized window you cannot see (focused but
		// stacked under its siblings, which START_UNFOCUSED makes possible)
		// answers the chord with nothing visible happening.
		wm_raise(w);
		wm_resize(w, (rect_t){0, 0, (int32_t)kFrameBuffer.width, (int32_t)kFrameBuffer.height});
	} else {
		w->flags &= ~GUI_WINDOW_MAXIMIZED;
		wm_resize(w, w->restoreFrame);
	}
}

void wm_set_minimized(window_t *w, bool minimized)
{
	// And there would be no way back: the desktop is skipped by Alt+Tab,
	// which is the only route a minimized window has home.
	if (desktop_declines(w, "minimize"))
		return;
	if (minimized == wm_is_hidden(w))
		return;
	if (minimized) {
		w->flags |= GUI_WINDOW_MINIMIZED;
		gui_damage_add_locked(w->frame);   // what it covered comes back
		if (s_focused == w) {
			// Focus goes to the most recently used VISIBLE window — which is
			// what Alt+Tab would have picked — not merely the next one down
			// the stack. NULL if nothing is left showing; keys then go
			// nowhere, which beats going somewhere invisible.
			window_t *next = NULL;
			for (window_t *c = s_top; c; c = c->below)
				if (!wm_is_hidden(c) && (next == NULL || c->focusSerial > next->focusSerial))
					next = c;
			focus_window(next);
			if (next)
				gui_damage_add_locked(next->frame);   // its titlebar turns blue
		}
	} else {
		w->flags &= ~GUI_WINDOW_MINIMIZED;
		wm_raise(w);   // back on top, focused, damaged
	}
}

void wm_move(window_t *w, int32_t x, int32_t y)
{
	// A moved window is no longer "the maximized one": the user took its
	// geometry back (see GUI_WINDOW_MAXIMIZED). Cleared before the move so a
	// restore can never return it to a place it has since left.
	w->flags &= ~GUI_WINDOW_MAXIMIZED;
	rect_t old = w->frame;
	w->frame.x = x;
	w->frame.y = y;
	gui_damage_add_locked(rect_union(old, w->frame));
}

// The client API's own ceiling: a window may be CREATED bigger than the
// screen (up to WINDOW_CAP_MAX a side), so capacity can never simply be "the
// screen" — it is the screen or the window, whichever is larger.
#define WINDOW_CAP_MAX 4096u
// Published to ring 3 as OS64_GUI_WINDOW_DIM_MAX (Codex #30 rd6), so an app
// can clamp before it asks; pinned here the way the title capacity is above.
_Static_assert(WINDOW_CAP_MAX == OS64_GUI_WINDOW_DIM_MAX,
               "gui/window.c and abi os64/gui.h disagree on the largest window side");

// ...OR THE SCREEN, WHICHEVER IS LARGER (Codex #31 rd2). 4096 was a bare
// constant in gui_window_create, and a framebuffer wider than that — a 5K
// panel is 5120 across — would have refused the desktop shell's own
// fullscreen window at the door, before it read gui.conf, so nothing in the
// GUI would have started. The screen is the one size every window system
// must be able to hold. ONE function answers the ceiling for both the
// create check and the capacity clamp below, because two copies of it were
// how the constant and the screen disagreed in the first place.
uint32_t wm_dim_max(void)
{
	uint32_t m = WINDOW_CAP_MAX;
	if (kFrameBuffer.width > m)  m = kFrameBuffer.width;
	if (kFrameBuffer.height > m) m = kFrameBuffer.height;
	return m;
}

void wm_canvas_capacity_for(int32_t content_w, int32_t content_h,
                            uint32_t *cap_w, uint32_t *cap_h)
{
	uint32_t cw = kFrameBuffer.width;
	uint32_t ch = kFrameBuffer.height;

	// Never smaller than what the window already is, or the surface it is
	// asked to hold would not fit its own reservation.
	if (content_w > 0 && (uint32_t)content_w > cw)
		cw = (uint32_t)content_w;
	if (content_h > 0 && (uint32_t)content_h > ch)
		ch = (uint32_t)content_h;

	if (cw > wm_dim_max())
		cw = wm_dim_max();
	if (ch > wm_dim_max())
		ch = wm_dim_max();

	*cap_w = cw;
	*cap_h = ch;
}

rect_t wm_clamp_frame(const window_t *w, rect_t frame)
{
	// Frame in, content out — the same inset wm_create applies, so the two
	// can never disagree about where the client area begins.
	int32_t content_w = frame.w - 2 * wm_border_width(w->flags);
	int32_t content_h = frame.h - wm_chrome_top(w->flags) - wm_border_width(w->flags);

	// Clamp into [minimum, reservation]. Clamping rather than refusing is
	// deliberate: this is driven by a mouse, and a drag that runs past a
	// limit should STOP at the limit, not abandon the whole gesture.
	if (content_w < GUI_WINDOW_MIN_CONTENT_W)
		content_w = GUI_WINDOW_MIN_CONTENT_W;
	if (content_h < GUI_WINDOW_MIN_CONTENT_H)
		content_h = GUI_WINDOW_MIN_CONTENT_H;
	if ((uint32_t)content_w > w->canvas_cap_w)
		content_w = (int32_t)w->canvas_cap_w;
	if ((uint32_t)content_h > w->canvas_cap_h)
		content_h = (int32_t)w->canvas_cap_h;

	// Re-derive the frame from the clamped content so the chrome the
	// compositor draws and the surface the client draws stay the same window.
	frame.w = content_w + 2 * wm_border_width(w->flags);
	frame.h = content_h + wm_chrome_top(w->flags) + wm_border_width(w->flags);
	return frame;
}

bool wm_resize(window_t *w, rect_t frame)
{
	// The SAME clamp the rubber band previewed with — one function, so what
	// the user let go of is what they get. (When these were two copies of the
	// arithmetic they would have agreed right up until the first time one of
	// them changed.)
	frame = wm_clamp_frame(w, frame);
	int32_t content_w = frame.w - 2 * wm_border_width(w->flags);
	int32_t content_h = frame.h - wm_chrome_top(w->flags) - wm_border_width(w->flags);

	uint32_t old_cw = w->content.width;
	uint32_t old_ch = w->content.height;
	rect_t old_frame = w->frame;

	if (frame.x == old_frame.x && frame.y == old_frame.y &&
	    (uint32_t)content_w == old_cw && (uint32_t)content_h == old_ch)
		return false;   // a gesture that ended where it started

	// The resize itself: two size fields. No allocation, no copy, no pixel
	// relocation — the reservation already holds every pixel either surface
	// will ever address, at offsets that do not move because pitch does not
	// move. A false here would mean the capacity bookkeeping had gone wrong;
	// leave the window exactly as it was and say so.
	if (!surface_set_size(&w->content, (uint32_t)content_w, (uint32_t)content_h,
	                      w->canvas_cap_w, w->canvas_cap_h) ||
	    !surface_set_size(&w->canvas, (uint32_t)content_w, (uint32_t)content_h,
	                      w->canvas_cap_w, w->canvas_cap_h)) {
		printd(DEBUG_GUI, "wm: window %u resize to %dx%d refused by its own capacity %ux%u\n",
			w->id, content_w, content_h, w->canvas_cap_w, w->canvas_cap_h);
		surface_set_size(&w->content, old_cw, old_ch, w->canvas_cap_w, w->canvas_cap_h);
		surface_set_size(&w->canvas, old_cw, old_ch, w->canvas_cap_w, w->canvas_cap_h);
		return false;
	}

	// Paint what growing newly exposed. Those pixels are whatever the buffer
	// last held there — zeroes on a first grow, an older frame's picture on a
	// second — and the client cannot fix them until it processes the resize
	// event below. BOTH surfaces get it for the same reason wm_create fills
	// both: content is what the compositor shows in the meantime, and canvas
	// is what a PARTIAL publish would otherwise snapshot garbage from.
	rect_t grown[2];
	int grown_count = 0;
	if ((uint32_t)content_w > old_cw)
		grown[grown_count++] = (rect_t){(int32_t)old_cw, 0,
		                                content_w - (int32_t)old_cw, content_h};
	if ((uint32_t)content_h > old_ch)
		grown[grown_count++] = (rect_t){0, (int32_t)old_ch,
		                                content_w, content_h - (int32_t)old_ch};
	for (int i = 0; i < grown_count; i++) {
		surface_fill_rect(&w->content, grown[i], WINDOW_CONTENT_INITIAL);
		surface_fill_rect(&w->canvas, grown[i], WINDOW_CONTENT_INITIAL);
	}

	w->frame = frame;
	gui_damage_add_locked(rect_union(old_frame, w->frame));

	// Tell the owner. This is the ONLY way an app finds out — there is no
	// polling interface for geometry, deliberately (see the event type's
	// comment in input.h). An app that ignores it keeps drawing a correct
	// picture at the wrong size, which is a bug with a very obvious shape.
	input_event_t ev = {
		.type = INPUT_EVENT_WINDOW_RESIZE,
		.resize = { .w = content_w, .h = content_h },
		.tick = kTicksSinceStart,
	};
	wm_deliver_event(w, &ev);

	printd(DEBUG_GUI, "wm: window %u resized to %dx%d content at (%d,%d)\n",
		w->id, content_w, content_h, frame.x, frame.y);
	return true;
}

void wm_census_locked(uint32_t *count, uint64_t *surface_bytes)
{
	uint32_t n = 0;
	uint64_t bytes = 0;

	for (const window_t *w = s_bottom; w; w = w->above) {
		n++;
		uint64_t cap_bytes = (uint64_t)w->canvas_cap_w * w->canvas_cap_h * 4;
		// content is always kmalloc'd at capacity...
		bytes += cap_bytes;
		// ...and the canvas is either the same kind of buffer (kernel-backed
		// windows) or a page-rounded task extent, which is the number that
		// actually left the physical allocator.
		bytes += (w->canvas_task_phys != 0)
		             ? (uint64_t)w->canvas_pages * PAGE_SIZE
		             : cap_bytes;
	}

	*count = n;
	*surface_bytes = bytes;
}

window_t *wm_window_by_id(uint32_t id)
{
	for (window_t *w = s_bottom; w; w = w->above)
		if (w->id == id)
			return w;
	return NULL;
}

window_t *wm_focused(void)
{
	return s_focused;
}

size_t wm_recency_ids(uint32_t *ids, size_t max)
{
	// Insertion-sort by focusSerial, newest first, as the z-list is walked.
	// Sixteen entries at most; the sort is the simplest thing that is
	// obviously right, which is the correct tool at this size.
	uint64_t serials[ALTTAB_RING_MAX];
	size_t n = 0;
	if (max > ALTTAB_RING_MAX)
		max = ALTTAB_RING_MAX;
	for (window_t *w = s_top; w; w = w->below) {
		// The desktop is not something you tab TO — it is what is left when
		// you tab away from everything. Listing it would put a permanent
		// last entry in every switcher strip and make Alt+Tab between two
		// windows a three-stop walk.
		if (w->flags & GUI_WINDOW_DESKTOP)
			continue;
		size_t i = n;
		while (i > 0 && serials[i - 1] < w->focusSerial) {
			if (i < max) { ids[i] = ids[i - 1]; serials[i] = serials[i - 1]; }
			i--;
		}
		if (i < max) { ids[i] = w->id; serials[i] = w->focusSerial; }
		if (n < max)
			n++;
	}
	return n;
}

void wm_deliver_event(window_t *w, const input_event_t *ev)
{
	uint32_t next = (w->evt_head + 1) % GUI_WINDOW_EVENTS_MAX;
	if (next == w->evt_tail)
		return;   // queue full: drop-newest, same policy as the input ring
	w->events[w->evt_head] = *ev;
	w->evt_head = next;

	// Aim a wake at a parked event_wait-er. Runs in the COMPOSITOR's thread
	// context under kGuiLock (never an ISR — invariant 4 holds), and the
	// wake takes only the scheduler queue lock inside, no trigger: the
	// woken thread runs on the next scheduler pass, which is the latency
	// the design already accepted. A waiter still mid-park is deliberately
	// left alone (the wake API's own rule) — its backstop re-runs the drain
	// moments later. The slot is NOT cleared here: the waiter owns its
	// registration, and a redundant wake at worst re-checks an empty queue.
	if (w->waiter != NULL)
		scheduler_wake_isleep_thread(w->waiter);
}

bool wm_pop_event(window_t *w, input_event_t *out)
{
	if (w->evt_head == w->evt_tail)
		return false;
	*out = w->events[w->evt_tail];
	w->evt_tail = (w->evt_tail + 1) % GUI_WINDOW_EVENTS_MAX;
	return true;
}

// Draw one window's chrome + content into the damaged part of the backbuffer.
//
// STRICTLY INSIDE `damage` — that is the contract, and it is load-bearing.
// This function used to paint the window's WHOLE frame and lean on a comment
// that said stray pixels were harmless because "backbuffer pixels outside the
// damage simply get rewritten with what they already showed." That was true
// only while a frame carried exactly ONE damage rect. The moment damage became
// a LIST (2026-08-18), painting outside rect i landed inside rect j — already
// composited in correct z-order, not yet flushed — so a lower window's repaint
// erased a higher window's pixels there and rect j pushed the wreckage to the
// glass. It showed up on the P5 as the console's background bleeding into
// gbounce: on raise, and as a trail behind the ball that appeared ONLY while
// the mouse moved over the console (a cursor rect over the console is exactly
// what makes the console repaint). Chris found it in an hour.
//
// The view makes the contract structural rather than remembered: pass the
// damage rect, draw in its coordinates, and the clipping in surface.c cannot
// be talked out of it. It is also a straight win — a 25x25 ball no longer
// repaints an entire 768x480 console underneath it.
static void composite_one(surface_t *backbuffer, const window_t *w, rect_t damage)
{
	surface_t view = surface_view(backbuffer, damage);
	bool focused = (w == s_focused);
	// Frame in VIEW coordinates; everything below is drawn in that space.
	rect_t f = {w->frame.x - damage.x, w->frame.y - damage.y,
	            w->frame.w, w->frame.h};

	// Border around everything (the titlebar overwrites the top edge) — for
	// every window that HAS one. The desktop does not: a border separates a
	// window from what is behind it, and there is nothing behind the desktop.
	// Drawing it put a 1px line around the edge of the screen that changed
	// colour whenever you clicked the wallpaper, which reads as a rendering
	// fault rather than as focus (Chris, the first afternoon the shell ran).
	if (wm_border_width(w->flags) > 0)
		surface_draw_rect(&view, f,
		                  focused ? WINDOW_BORDER_FOCUSED : WINDOW_BORDER_UNFOCUSED);

	// Titlebar with centered-ish title text (8px/glyph, 16px tall font) —
	// unless the window declined one, in which case the border IS the chrome
	// and focus shows only in the border's color.
	if (wm_has_titlebar(w->flags)) {   // not the bit: a desktop has none regardless (window.h)
		rect_t bar = {f.x + GUI_BORDER_WIDTH, f.y + GUI_BORDER_WIDTH,
		              f.w - 2 * GUI_BORDER_WIDTH, GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH};
		uint32_t bar_color = focused ? WINDOW_TITLEBAR_FOCUSED : WINDOW_TITLEBAR_UNFOCUSED;
		surface_fill_rect(&view, bar, bar_color);

		size_t title_len = 0;
		while (w->title[title_len] && title_len < GUI_WINDOW_TITLE_MAX - 1)
			title_len++;
		// Text clips per glyph cell against the view, so a titlebar sliced down
		// the middle by a damage rect draws its half-glyph and stops.
		surface_draw_text(&view, bar.x + 6, bar.y + (bar.h - 16) / 2,
		                  w->title, title_len, GUI_COLOR_WHITE, bar_color);

		// A pinned window wears a small white square at the right end of its
		// bar — the only chrome the pin has, and enough to answer "why won't
		// this thing go behind?" at a glance.
		if (w->flags & GUI_WINDOW_PINNED)
			surface_fill_rect(&view, (rect_t){bar.x + bar.w - 14, bar.y + (bar.h - 8) / 2, 8, 8},
			                  GUI_COLOR_WHITE);
	}

	// Client content. Blit the WHOLE content rect at its view-space position
	// and let surface_blit clip both ends: it already mirrors destination
	// clipping back into the source origin, so the visible sliver comes from
	// the right pixels without any arithmetic here.
	rect_t content_screen = wm_content_rect_on_screen(w);
	surface_blit(&view, content_screen.x - damage.x, content_screen.y - damage.y,
	             &w->content,
	             (rect_t){0, 0, (int32_t)w->content.width, (int32_t)w->content.height});
}

void wm_composite(surface_t *backbuffer, rect_t damage)
{
	// Bottom-up so overlaps resolve by z-order; skip windows that don't
	// touch the damage at all.
	rect_t overlap;
	for (window_t *w = s_bottom; w; w = w->above)
		if (!wm_is_hidden(w) && rect_intersect(w->frame, damage, &overlap))
			composite_one(backbuffer, w, damage);
}

// THE SAME QUESTION, ASKED ABOUT THE BACKGROUND (2026-08-25). The compositor
// paints kDesktop into every damage rect before it paints any window — which
// was free-ish when the background was the only thing under them, and stopped
// being free the day the desktop shell became a FULLSCREEN WINDOW. Then every
// damage rect cost two screenful-sized blits of the same pixels: the
// compositor's background, and the shell's window painted straight over it.
//
// Chris measured it within the hour on the P5: guicomp at 17-20% while
// dragging a large gterm, against 1% for a small gclock — the tell being that
// the cost tracked the RECT AREA, which is what a redundant full-rect blit
// looks like.
//
// So: if some window already covers this rect completely, the background
// underneath it cannot be seen and is not painted. Self-correcting by
// construction — if the shell dies, nothing covers the rect any more and the
// kernel's test pattern comes straight back, which is exactly the floor
// behaviour we wanted. It also pays off for a case that predates the shell: a
// maximized window now suppresses the background blit too.
bool wm_rect_is_covered(rect_t screen_rect)
{
	if (rect_is_empty(screen_rect))
		return true;

	for (const window_t *w = s_top; w; w = w->below) {
		rect_t covered;
		if (wm_is_hidden(w))
			continue;
		// Same containment test as wm_rect_is_occluded, and the same
		// deliberate simplicity: ONE window must cover the whole rect. Two
		// that jointly cover it read as uncovered and the background is
		// painted — wasted work, never a wrong pixel.
		//
		// And the same opacity caveat, which matters MORE here: every os64
		// window is fully opaque today. The day translucency lands, a
		// see-through window must stop suppressing the background, or you
		// will see through it to whatever the backbuffer last held.
		if (rect_intersect(w->frame, screen_rect, &covered) &&
		    covered.x == screen_rect.x && covered.y == screen_rect.y &&
		    covered.w == screen_rect.w && covered.h == screen_rect.h)
			return true;
	}
	return false;
}

// Occlusion, the cheap half (2026-08-18). Painter's algorithm draws a covered
// window and then draws over it: correct, and entirely wasted work — plus a
// flush of uncached pixels that cannot possibly change. This answers the one
// question that lets publish skip both.
//
// DELIBERATELY the simple test: is this rect inside ONE window above me? Two
// windows that JOINTLY cover it read as visible here and still get composited.
// Doing better means subtracting a real region (X11/Cairo territory) and
// maintaining it as windows move — a bigger idea, its own slice, and one this
// z-list can't answer in a walk. The common desktop shape is one window over
// another, which is exactly what this catches.
//
// Note it takes no view on WINDOW opacity, because os64 has none yet: every
// window is fully opaque (the X byte in XRGB is reserved for the translucency
// row that hasn't landed). The day alpha exists, this function is where it
// must be taught to stop trusting a covering frame — a window you can see
// through occludes nothing.
bool wm_rect_is_occluded(const window_t *w, rect_t screen_rect)
{
	if (rect_is_empty(screen_rect))
		return true;   // nothing to draw is trivially invisible

	for (const window_t *above = w->above; above; above = above->above) {
		rect_t covered;
		if (wm_is_hidden(above))
			continue;   // a minimized window covers nothing
		// Fully inside means: the intersection IS the rect itself.
		if (rect_intersect(above->frame, screen_rect, &covered) &&
		    covered.x == screen_rect.x && covered.y == screen_rect.y &&
		    covered.w == screen_rect.w && covered.h == screen_rect.h)
			return true;
	}
	return false;
}

// The other half of occlusion: telling the CLIENT. The compositor side
// (publish drops damage nobody would see) got guicomp to zero under a
// stack of hidden windows; what was left was every hidden client still
// computing and painting frames for nobody, and the only cure for that
// lives in the client. So each frame, after the window manager has moved
// whatever it moved, every window is asked the same question publish asks
// — is your whole frame behind one window above you? — plus the two ways to
// be invisible that have nothing to do with stacking: minimized, and a text
// terminal holding the glass. A changed answer flips the flag and queues
// one event. The flag is the state and the event the nudge (gui.h says why
// a client must never trust the event alone).
//
// The desktop is a window like the others here: a ring-3 client that can
// animate and bind a frame clock, covered when a maximized window hides
// all of it or a text terminal holds the glass, and owed the same answer.
void wm_cover_sweep_locked(void)
{
	bool glass = gui_owns_glass();
	for (window_t *w = s_top; w; w = w->below) {
		bool covered = !glass || wm_is_hidden(w) || wm_rect_is_occluded(w, w->frame);
		bool was = (w->flags & GUI_WINDOW_COVERED) != 0;
		if (covered == was)
			continue;
		if (covered)
			w->flags |= GUI_WINDOW_COVERED;
		else
			w->flags &= ~GUI_WINDOW_COVERED;
		input_event_t ev = {
			.type = covered ? INPUT_EVENT_WINDOW_COVERED : INPUT_EVENT_WINDOW_UNCOVERED,
			.tick = kTicksSinceStart,
		};
		wm_deliver_event(w, &ev);
	}
}
