#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <stdbool.h>
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

// flags
#define GUI_WINDOW_NO_DECORATIONS (1u << 0)   // reserved; not yet honored
// Created on top of the z-order but WITHOUT stealing focus (unless nothing
// holds it yet — somebody must). Born 2026-08-17, the day the P5 booted the
// GUI with no mouse: which window got focus at boot was a RACE between the
// compositor's thread (hello, console) and the demo tasks, and on a machine
// with no pointer the loser is stuck wherever the race left it. The demos
// wear this flag so the shell's console deterministically ends up focused —
// the thing you TYPE at is the thing that should be listening first.
#define GUI_WINDOW_START_UNFOCUSED (1u << 1)

typedef struct window
{
    // Z-order links. `above` points toward the front of the screen,
    // `below` toward the desktop.
    struct window *above, *below;

    uint32_t  id;
    uint32_t  flags;

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

window_t *wm_focused(void);

// Push a routed event onto the window's queue (drops when full).
void wm_deliver_event(window_t *w, const input_event_t *ev);

// Pop for the owner side; false when empty.
bool wm_pop_event(window_t *w, input_event_t *out);

// Composite every window that intersects `damage` (screen coords) into the
// backbuffer, bottom-up, chrome + content. The desktop below and the cursor
// above are the compositor's business, not ours.
void wm_composite(surface_t *backbuffer, rect_t damage);

// Where the content area sits on screen (for event coordinate translation
// and publish-rect mapping).
static inline rect_t wm_content_rect_on_screen(const window_t *w)
{
    return (rect_t){
        w->frame.x + GUI_BORDER_WIDTH,
        w->frame.y + GUI_TITLEBAR_HEIGHT,
        (int32_t)w->content.width,
        (int32_t)w->content.height,
    };
}

// True if the point (screen coords) lands in the window's titlebar — the
// grab-handle for dragging and NOT part of the client content.
static inline bool wm_point_in_titlebar(const window_t *w, int32_t x, int32_t y)
{
    return rect_contains_point(
        (rect_t){w->frame.x, w->frame.y, w->frame.w, GUI_TITLEBAR_HEIGHT}, x, y);
}

#endif // GUI_WINDOW_H
