#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gui/gui_types.h"
#include "gui/input.h"

// The window system (GUI layer 3): window objects, z-order, decorations,
// hit-testing, event routing.
//
// LOCKING: every function here must be called with kGuiLock held (see
// gui/gui_internal.h). window.c contains no locking of its own.

#define GUI_WINDOW_TITLE_MAX   32
#define GUI_WINDOW_EVENTS_MAX  64

// Chrome geometry: a titlebar across the top (which includes the top border)
// and a 1-pixel border on the other three sides.
#define GUI_TITLEBAR_HEIGHT    20
#define GUI_BORDER_WIDTH       1

// The smallest content area a RESIZE may leave behind. Deliberately larger
// than wm_create's 8x8 degenerate-surface guard, and for a different reason:
// creation's floor exists so a 0-wide surface can't ripple NULLs through the
// compositor, while this one exists so a drag can't produce a window with
// nothing in it. (Ctrl+Alt+drag means even a postage stamp is still grabbable
// — this floor is about usefulness, not rescue.)
#define GUI_WINDOW_MIN_CONTENT_W  64
#define GUI_WINDOW_MIN_CONTENT_H  32

// flags
// NO TITLEBAR. Honored since 2026-08-23 (it was reserved from birth): the
// window keeps its 1-pixel border on all four sides — a window with no edge
// at all is unfindable on a desktop — and loses the 20-pixel bar. Content
// size is untouched; only the frame shrinks. A creation flag for clients
// AND a toggle for the user (Ctrl+Alt+T), because "hide the title" is a
// thing you decide while looking at the window, not only when writing the
// program. Ctrl+Alt+left-drag still moves it — the chord gestures were
// designed to need no chrome, and this is the day that pays off.
#define GUI_WINDOW_NO_DECORATIONS (1u << 0)
// Created on top of the z-order but WITHOUT stealing focus (unless nothing
// holds it yet — somebody must). Born 2026-08-17, the day the P5 booted the
// GUI with no mouse: which window got focus at boot was a RACE between the
// compositor's thread (hello, console) and the demo tasks, and on a machine
// with no pointer the loser is stuck wherever the race left it. The demos
// wear this flag so the shell's console deterministically ends up focused —
// the thing you TYPE at is the thing that should be listening first.
#define GUI_WINDOW_START_UNFOCUSED (1u << 1)
// PIN ON TOP (2026-08-23): the window lives in the upper band of the z-list
// and cannot be buried by an ordinary raise. A window-manager state, not a
// creation option — there is no client flag for it and no app asked for
// one; the user pins with Ctrl+Alt+P. (twm's f.raise-or-lower era had no
// such thing; it arrived with fvwm's StaysOnTop in 1994 and every desktop
// since has kept it, because a clock or a terminal you want to keep seeing
// is a real need.)
#define GUI_WINDOW_PINNED          (1u << 2)
// MAXIMIZED (2026-08-23): the frame is the whole screen and restoreFrame
// remembers where it came from. Ctrl+Alt+M toggles; so does a double-click
// on the titlebar (the 1990 habit). A manual move or resize clears it — the
// user has taken the geometry back, and a later "restore" to a frame they
// have since abandoned would be a surprise. The capacity reservation is
// what makes this trivial: the screen IS the canvas capacity, so a
// maximize can never fail and never allocates (window.h's canvas_cap_w).
#define GUI_WINDOW_MAXIMIZED       (1u << 3)
// MINIMIZED (2026-08-23): off the glass — not composited, not hit-tested,
// occluding nothing — but alive: it keeps its place in the z-list, its
// event queue, its canvas, and its focus recency. Ctrl+Alt+N hides; there
// is no taskbar and no launcher yet, so Alt+Tab is the way back: a
// minimized window stays in the recency walk and is drawn DIM in the
// switcher strip, and it comes back only if the hold ENDS on it — walking
// past does nothing (see the switcher chapter in GRAPHICS.md for the rule
// that had to die, and why). (Focus leaves a minimized window at once, to the next most recent —
// typing into something you cannot see is the one thing this must never
// allow.)
#define GUI_WINDOW_MINIMIZED       (1u << 4)

// Alt+F4 twice within this long (5s) on a window that did not go away is
// "I mean it": the owner task gets SIGTERM.
#define GUI_CLOSE_ESCALATE_TICKS   (5 * TICKS_PER_SECOND)

// THE CHROME PALETTE. Private to window.c until 2026-08-23, when the Alt+Tab
// switcher became a second consumer: its highlighted cell has to be the same
// blue a focused titlebar is, or the strip is telling the user about a
// different desktop than the one behind it. One copy, shared — the alternative
// was a second set of literals drifting a shade at a time.
#define WINDOW_TITLEBAR_FOCUSED   0xff2a62b8
#define WINDOW_TITLEBAR_UNFOCUSED 0xff6a6f78
#define WINDOW_BORDER_FOCUSED     0xffd8dce4
#define WINDOW_BORDER_UNFOCUSED   0xff40444c

typedef struct window
{
    // Z-order links. `above` points toward the front of the screen,
    // `below` toward the desktop.
    struct window *above, *below;

    uint32_t  id;
    uint32_t  flags;

    // When this window last took focus, as a monotonic serial (window.c's
    // s_focus_serial, stamped wherever s_focused is assigned). Alt+Tab
    // orders windows by THIS, not by z-order: the day pin-on-top arrived
    // (2026-08-23) a pinned window held the top of the list while focus
    // went elsewhere, and a cycle that read recency off the stacking order
    // stopped toggling back. Recency is its own fact; this is where it lives.
    uint64_t  focusSerial;

    // Where the window was before it was maximized — what Ctrl+Alt+M puts
    // back. Meaningful only while GUI_WINDOW_MAXIMIZED is set.
    rect_t    restoreFrame;

    // When Alt+F4 last asked this window to close (kTicksSinceStart), 0 if
    // never. A second ask within GUI_CLOSE_ESCALATE_TICKS while the window
    // is still here escalates to SIGTERM on the owner — see
    // INPUT_EVENT_WINDOW_CLOSE in input.h for why the first one is only a
    // request.
    uint64_t  closeAskedTick;

    // The task that created this window through the client API (task.h
    // taskID), stamped by gui_window_create. Ownership is a CLIENT-API
    // concept: gui_client.c enforces it on every handle lookup and
    // gui_task_destroy_windows sweeps by it on task exit — the wm_* layer
    // never reads it (mechanism here, policy at the boundary). Kernel
    // daemons (guicomp's hello window, the console) own theirs the same
    // way; they simply never exit.
    uint64_t  owner;

    char      title[GUI_WINDOW_TITLE_MAX];

    rect_t    frame;      // screen rect INCLUDING decorations
    surface_t content;    // what the COMPOSITOR composites (the front buffer)
    // The client's drawing target (the back buffer). gui_window_get_surface
    // hands this out; gui_window_publish snapshots the damage rect
    // canvas→content under kGuiLock, so the compositor only ever sees
    // finished frames (GRAPHICS.md "Atomic frames" — snapshot-on-publish).
    // TWO FLAVORS since the surface pivot (2026-08-17):
    //   kernel-backed (canvas_task_phys == 0): pixels is a kmalloc buffer,
    //     surface_free's to release — the kernel-thread clients' world.
    //   task-backed: pixels is the KERNEL's HHDM alias of task-owned pages
    //     that are ALSO mapped into the owning task's address space at
    //     canvas_task_va (USER|WRITE|NO_EXECUTE, eagerly backed). The task
    //     draws through its VA, publish snapshots through the alias — the
    //     same physical memory, one copy, zero pixels crossing the ring.
    surface_t canvas;
    uint64_t  canvas_task_phys;   // 0 = kernel-backed; else the extent base
    uintptr_t canvas_task_va;     // where the OWNER task sees the canvas
    uint32_t  canvas_pages;       // extent length, for the unmap

    // THE RESIZE RESERVATION (2026-08-19). Both pixel stores are allocated at
    // this capacity and REPORT the current content size; their pitch_px is
    // cap_w for life. Everything good about resize in os64 falls out of that
    // one decision:
    //   * a resize moves no pixels — the image already drawn stays at the
    //     same offsets, because the row stride never changes;
    //   * a resize allocates nothing and therefore CANNOT FAIL;
    //   * and for the task-backed canvas, the kernel never unmaps a live user
    //     page, so there is no TLB shootdown and no window during which the
    //     owner could write through a stale entry into recycled memory. That
    //     last one is the whole reason this shape was chosen over
    //     realloc-and-remap; the hazard it avoids is silent, not a fault.
    // The price is standing memory: capacity is the SCREEN (see
    // wm_canvas_capacity_for), so a small window still reserves a screenful
    // twice over. Committing lazily is possible but needs a task reference or
    // a deferred-work channel to map pages into a foreign address space
    // safely — booked in DEBTS rather than built.
    uint32_t  canvas_cap_w, canvas_cap_h;

    // Per-window event queue: the compositor pushes routed events, the
    // owning app thread pops them via gui_event_poll(). Drop-newest on full.
    input_event_t events[GUI_WINDOW_EVENTS_MAX];
    uint32_t  evt_head, evt_tail;

    // The thread parked in gui_event_wait on this window, or NULL. Owned by
    // the WAITER (it registers and unregisters itself, console_read's
    // discipline — a stale slot is a spurious wake later); the deliverer
    // only reads it to aim a wake. Owner-checked windows mean the waiter is
    // always a thread of the owning task, which is what makes teardown
    // safe: a task's windows die in its own exit path, before its threads
    // do, so the slot can never outlive the thread it names.
    // (struct s_thread is thread_t's tag — forward-declared so this header
    // stays free of kernel includes, same bargain compositor.h struck.)
    struct s_thread *waiter;
} window_t;

// Create a window whose FRAME is `frame` (content is automatically inset by
// the chrome), put it on top, focus it, and damage it. Returns NULL if the
// content surface can't be allocated.
window_t *wm_create(const char *title, rect_t frame, uint32_t flags);

// Unlink + free. (No owner-notification semantics yet — the caller is the
// owner.) Damages the vacated area.
void wm_destroy(window_t *w);

// Topmost window whose frame contains the point, or NULL for the desktop.
window_t *wm_topmost_at(int32_t x, int32_t y);

// Bring to front and focus (damages both titlebars when focus moves).
void wm_raise(window_t *w);

// Move the frame origin (drag). Damages the vacated and occupied areas.
void wm_move(window_t *w, int32_t x, int32_t y);

// Pin (or unpin) a window — see GUI_WINDOW_PINNED. Re-places it at the top
// of its new band and damages its frame. A no-op if already in that state.
void wm_set_pinned(window_t *w, bool pinned);

// The capacity rule, in ONE place because two allocators must agree on it:
// wm_create sizes the kernel-side stores with it, and gui_window_create sizes
// the task-backed canvas extent with it. A window may be created larger than
// the screen (the client API allows up to 4096), so capacity is the larger of
// the two — never smaller than what the window already is.
void wm_canvas_capacity_for(int32_t content_w, int32_t content_h,
                            uint32_t *cap_w, uint32_t *cap_h);

// The frame this window would actually ADOPT for a requested one: content
// clamped to [GUI_WINDOW_MIN_CONTENT_*, capacity], then re-inflated by the
// chrome. Exported so an interactive resize can PREVIEW exactly what it will
// commit — the rubber band and wm_resize share this function precisely so the
// outline can never promise a size the window then refuses.
rect_t wm_clamp_frame(const window_t *w, rect_t frame);

// Resize to `frame` (screen rect INCLUDING decorations), clamping the content
// to [GUI_WINDOW_MIN_CONTENT_*, the canvas capacity]. Because both pixel
// stores were reserved at capacity this only re-reports their size, paints
// whatever the growth newly exposed, damages old ∪ new, and hands the owner
// an INPUT_EVENT_WINDOW_RESIZE. It allocates nothing and cannot fail; the
// return value is whether anything actually CHANGED, which is what lets a
// caller skip a redundant repaint at the end of a drag that went nowhere.
bool wm_resize(window_t *w, rect_t frame);

// Census for /sys/gui: how many windows exist, and what their two pixel
// stores cost. Follows this header's locking rule — caller holds kGuiLock.
//
// The bytes matter because HALF of them are structurally invisible from
// /proc: the canvas is task memory and shows in the owner's heap range, but
// the content surface is kmalloc'd and belongs to no address space a process
// can inspect. Found the hard way 2026-08-19, when Chris read 0x301000 of
// canvas out of gterm's status file and reasonably concluded that was the
// whole cost. A window is the only object in os64 that spends memory in two
// worlds at once, so it needs a file that adds them up.
void wm_census_locked(uint32_t *count, uint64_t *surface_bytes);

// Find a live window by id, without dereferencing anything but the z-list.
// The id (not the pointer) is the safe handle to hold across a lock release:
// a freed window_t could be re-kmalloc'd at the same address, and comparing
// pointers alone would happily match its replacement.
window_t *wm_window_by_id(uint32_t id);

window_t *wm_focused(void);

// Snapshot every window's id (the safe handle — see wm_window_by_id) in
// MOST-RECENTLY-FOCUSED order, the focused window first. Returns how many
// were written, at most `max`. Alt+Tab walks this snapshot rather than any
// live list — originally because every raise it performed rewrote the live
// state under it, and now (since the switcher strip, 2026-08-23) for the
// stronger reason that a hold must not change the scene AT ALL: the picture
// the user is choosing from has to be the one that was there when they
// started, or the cell under the highlight moves as they walk.
//
// There is no companion "stamp the focused window" call: the hold's commit
// goes through wm_raise/wm_set_minimized, which focus and stamp on their own.
// (wm_touch_focus lived here until the switcher; its whole purpose was to put
// back a recency stamp that raise-as-feedback kept taking, and nothing raises
// during a hold any more.)
size_t wm_recency_ids(uint32_t *ids, size_t max);
#define ALTTAB_RING_MAX 16   // the most a snapshot holds; the cycle covers the top sixteen

// Push a routed event onto the window's queue (drops when full).
void wm_deliver_event(window_t *w, const input_event_t *ev);

// Pop for the owner side; false when empty.
bool wm_pop_event(window_t *w, input_event_t *out);

// Composite every window that intersects `damage` (screen coords) into the
// backbuffer, bottom-up, chrome + content. The desktop below and the cursor
// above are the compositor's business, not ours.
void wm_composite(surface_t *backbuffer, rect_t damage);

// Is this screen rect completely hidden behind some window ABOVE `w`? The
// only question publish needs to ask before spending a composite and a slow
// uncached flush on pixels nobody can see. Caller holds kGuiLock.
bool wm_rect_is_occluded(const window_t *w, rect_t screen_rect);

// How much chrome sits ABOVE the content: the titlebar (which includes the
// top border) for a decorated window, the bare border for an undecorated
// one. THE one place the question is answered — seven sites used to spell
// "GUI_TITLEBAR_HEIGHT" by hand, which is seven sites to miss the day the
// answer stopped being constant. Takes flags, not a window, because the
// client boundary sizes a content area before any window exists.
static inline int32_t wm_chrome_top(uint32_t flags)
{
    return (flags & GUI_WINDOW_NO_DECORATIONS) ? GUI_BORDER_WIDTH : GUI_TITLEBAR_HEIGHT;
}

// Where the content area sits on screen (for event coordinate translation
// and publish-rect mapping).
static inline rect_t wm_content_rect_on_screen(const window_t *w)
{
    return (rect_t){
        w->frame.x + GUI_BORDER_WIDTH,
        w->frame.y + wm_chrome_top(w->flags),
        (int32_t)w->content.width,
        (int32_t)w->content.height,
    };
}

// True if the point (screen coords) lands in the window's titlebar — the
// grab-handle for dragging and NOT part of the client content. An
// undecorated window has none; its top border is content-adjacent chrome
// like the other three sides, and a click there is the window system's.
static inline bool wm_point_in_titlebar(const window_t *w, int32_t x, int32_t y)
{
    if (w->flags & GUI_WINDOW_NO_DECORATIONS)
        return false;
    return rect_contains_point(
        (rect_t){w->frame.x, w->frame.y, w->frame.w, GUI_TITLEBAR_HEIGHT}, x, y);
}

// Show or hide the titlebar (Ctrl+Alt+T). The CONTENT STAYS WHERE IT IS —
// the frame's top edge moves to meet it — because the content is what the
// user is looking at, and a window that jumps 19 pixels when you hide its
// title is a window you hid the title of by mistake. Damages old ∪ new.
void wm_set_decorated(window_t *w, bool decorated);

// Maximize to the screen, or restore the remembered frame. See
// GUI_WINDOW_MAXIMIZED. Goes through wm_resize, so the owner gets its
// resize event exactly as it would for a drag.
void wm_set_maximized(window_t *w, bool maximized);

// Hide from the glass, or bring back (restored = raised + focused). See
// GUI_WINDOW_MINIMIZED.
void wm_set_minimized(window_t *w, bool minimized);

// Hidden from the glass? The one test composite, hit-testing and occlusion
// all ask, so they cannot disagree about what "hidden" means.
static inline bool wm_is_hidden(const window_t *w)
{
    return (w->flags & GUI_WINDOW_MINIMIZED) != 0;
}

#endif // GUI_WINDOW_H
