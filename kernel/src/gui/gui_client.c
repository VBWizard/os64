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
#include "smp_core.h"   // get_core_local_storage — the caller's identity
#include "task.h"       // task_t.taskID — what a window's owner IS

extern struct Framebuffer kFrameBuffer;

// Handle table: handle = index + 1, so 0 is never a valid handle. 32 windows
// is plenty until real userland apps exist. Guarded by kGuiLock.
#define GUI_MAX_WINDOWS 32
static window_t *s_handles[GUI_MAX_WINDOWS];

// The calling task's identity, for ownership. Client calls only ever arrive
// from scheduled thread context — never from ISRs (invariant 4) — so CLS
// holds a live task; the guards make the answer 0 rather than a fault if a
// future early-boot caller breaks that assumption. 0 is deliberately not a
// valid owner: gui_task_destroy_windows refuses to sweep for it.
static uint64_t gui_current_task_id(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	return (cls != NULL && cls->task != NULL) ? cls->task->taskID : 0;
}

// Look up a handle; NULL when out of range or closed. Call with lock held.
static window_t *handle_lookup(int64_t handle)
{
	if (handle < 1 || handle > GUI_MAX_WINDOWS)
		return NULL;
	return s_handles[handle - 1];
}

// Look up a handle AND verify the caller owns it — the check every
// handle-taking client call makes, because under userland any task can put
// any integer in a register. A task must never be able to present into,
// poll events from, or destroy a window it did not create; today's callers
// are kernel threads, but the fence goes up BEFORE the first task can hold
// a surface (GRAPHICS.md migration order, step 1 — deliberately first).
// Call with lock held; *err is written only on the NULL return.
static window_t *handle_lookup_owned(int64_t handle, int64_t *err)
{
	window_t *win = handle_lookup(handle);
	if (win == NULL) {
		*err = GUI_ERR_INVALID_HANDLE;
		return NULL;
	}
	if (win->owner != gui_current_task_id()) {
		*err = GUI_ERR_NOT_OWNER;
		return NULL;
	}
	return win;
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
	// Stamped here, not in wm_create: ownership is a client-API concept and
	// the wm_* layer stays policy-free. Whoever asked, owns — and their exit
	// path (gui_task_destroy_windows) will collect.
	win->owner = gui_current_task_id();
	s_handles[handle - 1] = win;

	spinlock_release_irqrestore(&kGuiLock, irqflags);
	return handle;
}

// Future syscall: SYSCALL_GUI_WINDOW_DESTROY (17), user_ptr_mask 0.
int64_t gui_window_destroy(int64_t handle)
{
	int64_t err;
	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return err;
	}
	s_handles[handle - 1] = NULL;
	wm_destroy(win);   // damages the vacated area itself
	spinlock_release_irqrestore(&kGuiLock, irqflags);
	return 0;
}

// Future syscall: SYSCALL_GUI_WINDOW_GET_SURFACE (18), user_ptr_mask 0b10
// (out-struct). Hands out the window's CANVAS — the client-owned back
// buffer; the compositor never reads it (it composites `content`, which
// publish() snapshots into — see window.h). Under userland this call maps
// the canvas pages into the task's address space and returns the task VA.
int64_t gui_window_get_surface(int64_t handle, surface_t *out)
{
	if (!out)
		return GUI_ERR_BAD_ARGS;
	int64_t err;
	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return err;
	}
	*out = win->canvas;
	spinlock_release_irqrestore(&kGuiLock, irqflags);
	return 0;
}

// Future syscall: SYSCALL_GUI_WINDOW_PUBLISH (19) — renamed from "present"
// at design review ("present" doubles as an adjective and is swapchain
// jargon besides). Damage rect is NULLABLE, so it stays OUT of the
// user_ptr_mask (the SETENV precedent) and the handler validates its copy.
int64_t gui_window_publish(int64_t handle, const rect_t *damage)
{
	int64_t err;
	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return err;
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
	int64_t err;
	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		spinlock_release_irqrestore(&kGuiLock, irqflags);
		return err;
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

// The task-exit sweep — contract in compositor.h. NOT a client call and
// never dispatched as a syscall: task.c invokes it on the way out, so a
// task cannot decline its own cleanup any more than it can decline
// handle_close_all. This is the enforcement half of the ownership rule;
// the checks above are merely the courtesy half.
void gui_task_destroy_windows(uint64_t taskID)
{
	// taskID 0 is the not-a-task sentinel gui_current_task_id() returns
	// when CLS has no task — refusing it here means a sweep can never
	// match windows created from such a context by mistake.
	if (!kEnableGUI || taskID == 0)
		return;

	uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);
	for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
		window_t *win = s_handles[i];
		if (win == NULL || win->owner != taskID)
			continue;
		s_handles[i] = NULL;
		printd(DEBUG_GUI, "gui: task 0x%08x exited owning window '%s' (handle %d) — destroying\n",
		       taskID, win->title, i + 1);
		wm_destroy(win);   // damages the vacated area itself
	}
	spinlock_release_irqrestore(&kGuiLock, irqflags);
}
