#ifndef GUI_CLIENT_H
#define GUI_CLIENT_H

#include <stdint.h>
#include "gui/gui_types.h"
#include "gui/input.h"

// The GUI client API — the ONLY interface apps use. Kernel-thread apps call
// these as plain functions today; each is shaped to become a syscall
// unchanged (handle-based, ≤6 register-sized args, int64_t result, negative
// = error). The reserved syscall numbers live in syscall_numbers.h; the
// future user_ptr_mask for each call is noted at its definition in
// gui_client.c. See GRAPHICS.md for the migration recipe.

#define GUI_ERR_INVALID_HANDLE  (-1)
#define GUI_ERR_NO_RESOURCES    (-2)
#define GUI_ERR_BAD_ARGS        (-3)
#define GUI_ERR_NOT_RUNNING     (-4)
// Handle exists but belongs to another task. Distinct from INVALID_HANDLE
// on purpose: "no such window" and "exists, and is none of your business"
// are different answers, and only the first should tempt a caller to retry.
#define GUI_ERR_NOT_OWNER       (-5)

// Create a window (frame includes decorations). Returns a handle > 0.
int64_t gui_window_create(const char *title, int32_t x, int32_t y,
                          uint32_t w, uint32_t h, uint64_t flags);

int64_t gui_window_destroy(int64_t handle);

// Get the window's drawable CANVAS (the client-owned back buffer). Draw at
// will — nothing shows until present() snapshots your damage rect into the
// compositor-side content surface (atomic frames: the screen only ever
// shows finished frames). Under userland the canvas pages become a
// shared-memory mapping; the snapshot semantics stay identical.
int64_t gui_window_get_surface(int64_t handle, surface_t *out);

// Publish content changes: `damage` in CONTENT coordinates (NULL = all).
// The compositor picks it up on its next frame.
int64_t gui_window_present(int64_t handle, const rect_t *damage);

// Poll the window's event queue. 1 = event copied out, 0 = queue empty.
// Mouse coordinates in events are CONTENT-local.
int64_t gui_event_poll(int64_t handle, input_event_t *out);

int64_t gui_screen_info(uint32_t *width, uint32_t *height);

#endif // GUI_CLIENT_H
