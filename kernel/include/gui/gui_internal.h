#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "spinlock.h"

// GUI-internal shared state, including appearance publication and notification.
// Client API lives in gui_client.h.
//
// kGuiLock serializes ALL mutable window-system state: the z-order list,
// per-window event queues, the damage accumulator, and the handle table.
// It also protects the appearance store and its publication broadcast.
// Rules:
//  * gui_client.c and the compositor ACQUIRE it, through gui_lock (below);
//    window.c (wm_*) functions assume the caller already holds it.
//    appearance.c acquires it for the session store and broadcasts while
//    holding it.
//  * Compositing into the backbuffer happens UNDER the lock (RAM work, which
//    keeps window lifetimes trivially safe; /sys/gui's composite line says
//    how long it holds the lock), but the flush to the framebuffer, mapped
//    write-combining and slower than RAM, happens strictly AFTER release.
//  * Never taken from IRQ context, so the irqsave discipline is belt-and-
//    suspenders — it keeps a panic-during-composite from ever deadlocking a
//    fault path that might want GUI state later.

extern spinlock_t kGuiLock;

// kGuiLock is taken through gui_lock(site) and let go through gui_unlock, so
// that /sys/gui can say who waits for it and who holds it, and how long:
// every taker names where it is (the site), and the hold is charged to the
// site that took it. The counts are written under the lock they describe.
typedef enum {
	GUI_LOCK_PUBLISH,       // a client's frame copied in (gui_window_publish)
	GUI_LOCK_COMPOSITE,     // the compositor's frame: input, the WM, compositing
	GUI_LOCK_DAMAGE,        // damage added from outside the lock (gui_damage_add)
	GUI_LOCK_RING,          // a window's doorbell rung (gui_event_ring)
	GUI_LOCK_WAIT,          // a client waiting for its events (gui_event_wait*)
	GUI_LOCK_POLL,          // a client taking its events (gui_event_poll)
	GUI_LOCK_WINDOW,        // a window made, unmade, asked about or changed
	GUI_LOCK_GLASS,         // /dev/glass: a viewer's view, read or input
	GUI_LOCK_OTHER,         // the appearance and decoration stores, the census
	GUI_LOCK_SITES
} gui_lock_site_t;

// One site's numbers, in TSC cycles (kCPUCyclesPerSecond converts):
// acquisitions, how many waited, all the waiting and holding, and the worst
// of each since the counts were last read (gui_lock_counts' restart).
typedef struct {
	uint64_t acquires, contended, wait_cycles, hold_cycles;
	uint64_t max_wait_cycles, max_hold_cycles;
} gui_lock_count_t;

// What the compositor's passes did (/sys/gui): every pass, the passes that
// composited anything, the rectangles and pixels composited, how often the
// damage list was full and fell back to one union of everything
// (gui_damage_add_locked), and its holds of kGuiLock by length, so a run of
// long passes shows. Bucket k counts holds under 250 us << k, the last
// those longer. Written under kGuiLock.
#define GUI_COMPOSITE_HOLD_BUCKETS 7
typedef struct {
	uint64_t passes, drawn, rects, pixels, collapses;
	uint64_t holds[GUI_COMPOSITE_HOLD_BUCKETS];
} gui_composite_count_t;

uint64_t gui_lock(gui_lock_site_t site);
void gui_unlock(uint64_t irqflags);
// Every site's counts into `out` (GUI_LOCK_SITES of them) and the
// compositor's into `comp`, in one hold of the lock so the two agree (the
// composite site's acquisitions are the compositor's passes). The worst wait
// and hold of each site restart when `restart`, so each read reports the
// stretch since the last. The read is counted as OTHER.
void gui_lock_counts(gui_lock_count_t *out, gui_composite_count_t *comp, bool restart);
const char *gui_lock_site_name(gui_lock_site_t site);

#include "gui/gui_types.h"

// Damage accumulation for callers that ALREADY hold kGuiLock (wm_* internals,
// the compositor's routing). The public gui_damage_add() (compositor.h) is
// the take-the-lock-yourself variant; kGuiLock is not recursive, so picking
// the wrong one deadlocks — hence the loud name.
void gui_damage_add_locked(rect_t screen_rect);

#endif // GUI_INTERNAL_H
