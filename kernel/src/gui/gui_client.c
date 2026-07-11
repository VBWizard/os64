// gui_client.c — implementation of the syscall-shaped client API.
//
// Each function: validate args → take kGuiLock → touch window state →
// release. Thin by design: all real logic lives in window.c/compositor.c.
// The "future syscall" comment on each function records its reserved number
// (syscall_numbers.h) and the user_ptr_mask it will need — bit i set means
// "arg i is a user pointer the dispatcher must range-check".

#include "gui/gui_client.h"
#include "gui/gui_internal.h"
#include "gui/window.h"
#include "gui/compositor.h"

#include "CONFIG.h"
#include "printd.h"
#include "video.h"
#include "memcpy.h"

extern struct Framebuffer kFrameBuffer;

// Handle table: handle = index + 1, so 0 is never a valid handle. 32 windows
// is plenty until real userland apps exist. Guarded by kGuiLock.
#define GUI_MAX_WINDOWS 32
static window_t *s_handles[GUI_MAX_WINDOWS];

// Look up a handle; NULL when out of range or closed. Call with lock held.
static window_t *handle_lookup(int64_t handle)
{
	if (handle < 1 || handle > GUI_MAX_WINDOWS)
		return NULL;
	return s_handles[handle - 1];
}

// Future syscall: SYSCALL_GUI_WINDOW_CREATE (16), user_ptr_mask 0b000001
// (title string; copied with copy_user_string before reaching here).
int64_t gui_window_create(const char *title, int32_t x, int32_t y,
                          uint32_t w, uint32_t h, uint64_t flags)
{
	if (!kEnableGUI)
		return GUI_ERR_NOT_RUNNING;
	if (w < 32 || h < 32 || w > 4096 || h > 4096)
		return GUI_ERR_BAD_ARGS;

	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);

	int64_t handle = 0;
	for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
		if (!s_handles[i]) {
			handle = i + 1;
			break;
		}
	}
	if (!handle) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return GUI_ERR_NO_RESOURCES;
	}

	window_t *win = wm_create(title, (rect_t){x, y, (int32_t)w, (int32_t)h}, (uint32_t)flags);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return GUI_ERR_NO_RESOURCES;
	}
	s_handles[handle - 1] = win;

	spinlock_release_irqrestore(&kGuiLock, irqflags);
	return handle;
}

// Future syscall: SYSCALL_GUI_WINDOW_DESTROY (17), user_ptr_mask 0.
int64_t gui_window_destroy(int64_t handle)
{
	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	window_t *win = handle_lookup(handle);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return GUI_ERR_INVALID_HANDLE;
	}
	s_handles[handle - 1] = NULL;
	wm_destroy(win);   // damages the vacated area itself
	spinlock_release_irqrestore(&kGuiLock, irqflags);
	return 0;
}

// Future syscall: SYSCALL_GUI_WINDOW_GET_SURFACE (18), user_ptr_mask 0b10
// (out-struct). Hands out the window's CANVAS — the client-owned back
// buffer; the compositor never reads it (it composites `content`, which
// present() snapshots into — see window.h). Under userland this call maps
// the canvas pages into the task's address space and returns the task VA.
int64_t gui_window_get_surface(int64_t handle, surface_t *out)
{
	if (!out)
		return GUI_ERR_BAD_ARGS;
	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	window_t *win = handle_lookup(handle);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return GUI_ERR_INVALID_HANDLE;
	}
	*out = win->canvas;
	spinlock_release_irqrestore(&kGuiLock, irqflags);
	return 0;
}

// Future syscall: SYSCALL_GUI_WINDOW_PRESENT (19), user_ptr_mask 0b10
// (damage rect, NULL allowed).
int64_t gui_window_present(int64_t handle, const rect_t *damage)
{
	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	window_t *win = handle_lookup(handle);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return GUI_ERR_INVALID_HANDLE;
	}

	// Clip the damage to the content bounds (NULL = the whole content).
	rect_t content_bounds = {0, 0, (int32_t)win->content.width, (int32_t)win->content.height};
	rect_t local;
	if (damage) {
		if (!rect_intersect(*damage, content_bounds, &local)) {
			spinlock_release_irqrestore(&kGuiLock, irqflags);
			return 0;   // empty damage: legal no-op
		}
	} else {
		local = content_bounds;
	}

	// Snapshot-on-publish (GRAPHICS.md "Atomic frames"): copy the damage rect
	// canvas → content under kGuiLock. The compositor only ever composites
	// content, so a frame on screen is always a frame the client FINISHED —
	// this is what fuses the mid-draw torn ball back into one ball. Cost is
	// one damage-bounded row-wise copy, the same order as the composite blit
	// that follows it.
	for (int32_t y = 0; y < local.h; y++)
		memcpy(win->content.pixels + (size_t)(local.y + y) * win->content.pitch_px + local.x,
		       win->canvas.pixels  + (size_t)(local.y + y) * win->canvas.pitch_px  + local.x,
		       (size_t)local.w * sizeof(uint32_t));

	// Translate content-local damage to screen coordinates while we still
	// hold the lock (the frame can't move under us here).
	rect_t content_screen = wm_content_rect_on_screen(win);
	rect_t screen_damage = (rect_t){content_screen.x + local.x,
	                                content_screen.y + local.y,
	                                local.w, local.h};
	spinlock_release_irqrestore(&kGuiLock, irqflags);

	// Separate acquisition inside — kGuiLock is not recursive.
	gui_damage_add(screen_damage);
	return 0;
}

// Future syscall: SYSCALL_GUI_EVENT_POLL (20), user_ptr_mask 0b10 (out-event).
int64_t gui_event_poll(int64_t handle, input_event_t *out)
{
	if (!out)
		return GUI_ERR_BAD_ARGS;
	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	window_t *win = handle_lookup(handle);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return GUI_ERR_INVALID_HANDLE;
	}
	bool got = wm_pop_event(win, out);
	spinlock_release_irqrestore(&kGuiLock, irqflags);
	return got ? 1 : 0;
}

// Future syscall: SYSCALL_GUI_SCREEN_INFO (21), user_ptr_mask 0b11.
int64_t gui_screen_info(uint32_t *width, uint32_t *height)
{
	if (!kEnableGUI)
		return GUI_ERR_NOT_RUNNING;
	if (width)
		*width = kFrameBuffer.width;
	if (height)
		*height = kFrameBuffer.height;
	return 0;
}
