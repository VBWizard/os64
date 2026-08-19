// window.c — the window system (GUI layer 3): z-order, decorations,
// hit-testing, per-window event queues.
//
// LOCKING: every function here assumes the caller holds kGuiLock (see
// gui/gui_internal.h). That's what makes the singly-owned z-list and the
// SPSC-ish event queues safe with zero locking code in this file.

#include "gui/window.h"
#include "gui/gui_internal.h"
#include "gui/surface.h"
#include "gui/compositor.h"

#include "CONFIG.h"
#include "kmalloc.h"
#include "memset.h"
#include "printd.h"
#include "scheduler.h"   // scheduler_wake_isleep_thread — event_wait's alarm bell
#include "strcpy.h"

// Z-order list. s_top is frontmost (first hit-tested), s_bottom is nearest
// the desktop (first composited). Both NULL when no windows exist.
static window_t *s_top = NULL;
static window_t *s_bottom = NULL;
static window_t *s_focused = NULL;
static uint32_t s_next_id = 1;

// Chrome colors: the focused window gets the saturated titlebar.
#define WINDOW_TITLEBAR_FOCUSED   0xff2a62b8
#define WINDOW_TITLEBAR_UNFOCUSED 0xff6a6f78
#define WINDOW_BORDER_FOCUSED     0xffd8dce4
#define WINDOW_BORDER_UNFOCUSED   0xff40444c
#define WINDOW_CONTENT_INITIAL    GUI_COLOR_LIGHT_GRAY

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

static void link_on_top(window_t *w)
{
	w->above = NULL;
	w->below = s_top;
	if (s_top)
		s_top->above = w;
	s_top = w;
	if (!s_bottom)
		s_bottom = w;
}

window_t *wm_create(const char *title, rect_t frame, uint32_t flags)
{
	// Content = frame minus chrome; refuse degenerate sizes rather than
	// letting a 0-wide surface ripple NULLs through the compositor.
	int32_t content_w = frame.w - 2 * GUI_BORDER_WIDTH;
	int32_t content_h = frame.h - GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH;
	if (content_w < 8 || content_h < 8)
		return NULL;

	window_t *w = kmalloc(sizeof(window_t));
	if (!w)
		return NULL;
	memset(w, 0, sizeof(window_t));

	if (surface_init(&w->content, (uint32_t)content_w, (uint32_t)content_h) != 0) {
		kfree(w);
		return NULL;
	}
	// The client-facing back buffer (see window.h). Filled identically to
	// content so a client's first PARTIAL present doesn't snapshot garbage
	// around its damage rect.
	if (surface_init(&w->canvas, (uint32_t)content_w, (uint32_t)content_h) != 0) {
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
	if (!(flags & GUI_WINDOW_START_UNFOCUSED) || s_focused == NULL)
		s_focused = w;
	gui_damage_add_locked(w->frame);

	printd(DEBUG_GUI, "wm: created window %u '%s' at (%d,%d) %dx%d\n",
		w->id, w->title, frame.x, frame.y, frame.w, frame.h);
	return w;
}

void wm_destroy(window_t *w)
{
	gui_damage_add_locked(w->frame);   // repaint what the window covered
	if (s_focused == w)
		s_focused = w->below ? w->below : s_top;
	unlink_window(w);
	if (s_focused == w)
		s_focused = s_top;
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
		if (rect_contains_point(w->frame, x, y))
			return w;
	return NULL;
}

void wm_raise(window_t *w)
{
	window_t *old_focus = s_focused;

	if (s_top != w) {
		unlink_window(w);
		link_on_top(w);
		// Newly exposed stacking: repaint the whole raised frame.
	}
	s_focused = w;

	// Titlebars repaint on focus change (color flips on both windows).
	if (old_focus && old_focus != w)
		gui_damage_add_locked(old_focus->frame);
	gui_damage_add_locked(w->frame);
}

void wm_move(window_t *w, int32_t x, int32_t y)
{
	rect_t old = w->frame;
	w->frame.x = x;
	w->frame.y = y;
	gui_damage_add_locked(rect_union(old, w->frame));
}

window_t *wm_focused(void)
{
	return s_focused;
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

	// Border around everything (the titlebar overwrites the top edge).
	surface_draw_rect(&view, f,
	                  focused ? WINDOW_BORDER_FOCUSED : WINDOW_BORDER_UNFOCUSED);

	// Titlebar with centered-ish title text (8px/glyph, 16px tall font).
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
		if (rect_intersect(w->frame, damage, &overlap))
			composite_one(backbuffer, w, damage);
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
		// Fully inside means: the intersection IS the rect itself.
		if (rect_intersect(above->frame, screen_rect, &covered) &&
		    covered.x == screen_rect.x && covered.y == screen_rect.y &&
		    covered.w == screen_rect.w && covered.h == screen_rect.h)
			return true;
	}
	return false;
}
