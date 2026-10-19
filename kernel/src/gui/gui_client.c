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
#include "gui/surface.h"   // surface_free — the pivot retires the kmalloc canvas

#include "CONFIG.h"
#include "printd.h"
#include "video.h"
#include "memcpy.h"
#include "smp_core.h"   // get_core_local_storage — the caller's identity
#include "task.h"       // task_t.taskID — what a window's owner IS
#include "scheduler.h"  // kTaskList — the close escalation finds the owner on the spine
#include "memory/paging.h"     // the canvas mapping (surface pivot)
#include "memory/allocator.h"  // allocate_memory_aligned / free_memory — task canvas pages
#include "signals.h"    // SIGSLEEP / signal_park_must_end — event_wait's park and its exit
#include "os64/signal.h"   // OS64_INTERRUPTED — the value GUI_ERR_INTERRUPTED must BE

// THE TWO RINGS AGREE, OR THE BUILD STOPS (Codex #29 rd19): gui_event_wait's
// "a signal cut your wait short" is the system-wide sentinel, not a private
// GUI code. signals.c pins every signal NUMBER this way; the one error value
// that crosses subsystems gets the same treatment.
_Static_assert(GUI_WINDOW_HAS_SETTINGS == OS64_GUI_WINDOW_HAS_SETTINGS,"settings capability ABI");
_Static_assert(GUI_ERR_INTERRUPTED == OS64_INTERRUPTED,
               "GUI_ERR_INTERRUPTED must be OS64_INTERRUPTED (os64/signal.h) — one sentinel for every interrupted wait");
#include "kernel.h"     // kTicksSinceStart — the park's backstop deadline

extern struct Framebuffer kFrameBuffer;
extern uintptr_t kHHDMOffset;

// ── The ABI layout lock (kernel side) ───────────────────────────────────────
// abi/include/os64/gui.h defines ring 3's view of rect_t / surface_t /
// input_event_t. Both sides assert the sizes and offsets at this boundary;
// input.h also checks event numbers against the public constants.
_Static_assert(GUI_WINDOW_TITLE_UTF8 == OS64_GUI_WINDOW_TITLE_UTF8, "title encoding ABI");
_Static_assert(sizeof(rect_t) == 16, "rect_t drifted from the GUI ABI (os64/gui.h)");
_Static_assert(sizeof(surface_t) == 24, "surface_t drifted from the GUI ABI (os64/gui.h)");
_Static_assert(sizeof(input_event_t) == 32, "input_event_t drifted from the GUI ABI (os64/gui.h)");
_Static_assert(__builtin_offsetof(input_event_t, focus.other_handle) == 6, "focus handle ABI");
_Static_assert(__builtin_offsetof(input_event_t, doorbell.mask) == 4,
               "input_event_t doorbell moved — GUI ABI break");
_Static_assert(__builtin_offsetof(input_event_t, key) == 4,
               "input_event_t union moved — GUI ABI break");
_Static_assert(__builtin_offsetof(input_event_t, tick) == 24,
               "input_event_t tick moved — GUI ABI break");

// Legacy frame-sizing defaults remain ABI-stable. New clients can request
// content dimensions atomically; installed decoration metrics are dynamic.
_Static_assert(OS64_GUI_BORDER_WIDTH == GUI_BORDER_WIDTH,
               "the GUI ABI's border width drifted from the WM's");
_Static_assert(OS64_GUI_TITLEBAR_HEIGHT == GUI_TITLEBAR_HEIGHT,
               "the GUI ABI's titlebar height drifted from the WM's");
_Static_assert(OS64_GUI_MIN_CONTENT == GUI_MIN_CONTENT,
               "the GUI ABI's minimum content size drifted from the WM's");

// Handle table: handle = index + 1, so 0 is never a valid handle. 32 windows
// is plenty until real userland apps exist. Guarded by kGuiLock.
#define GUI_MAX_WINDOWS 32
static window_t *s_handles[GUI_MAX_WINDOWS];
_Static_assert(GUI_MAX_WINDOWS <= UINT16_MAX, "focus handle field capacity");
_Static_assert(GUI_ERR_BUSY == OS64_GUI_ERR_BUSY, "GUI busy ABI");
_Static_assert(GUI_ERR_NOT_FOCUSED == OS64_GUI_ERR_NOT_FOCUSED, "GUI focus refusal ABI");

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

// Release a task-backed canvas: unmap the owner's VA, give the pages back,
// and blind wm_destroy's surface_free to the swap (pixels = NULL; its NULL
// guard makes that free a no-op). Caller holds kGuiLock and supplies the
// OWNING task — the mapping exists in exactly one address space.
// The VA itself stays burned forever (the bump allocator never reuses), so
// a stale canvas pointer faults instead of aliasing whatever comes next.
//
// UNDER THE OWNER'S signalLock (Codex #29 rd11), and the reason is not about
// the GUI at all. A canvas is allocator-backed, task-mapped, USER-WRITABLE
// memory — which means ring 3 can pivot RSP into it, and signal delivery
// reaches a user stack by resolving the page and storing through its HHDM
// alias, an alias that exists only WHILE THE PAGE IS ALLOCATED (task.h,
// signalLock's second job). Free it between delivery's resolve and its store
// and that store takes a ring-0 #PF: a sibling calling gui_window_destroy()
// could panic the kernel.
//
// This is the SAME hole rd8 closed in syscall_unmap, and it stayed open here
// because that round's audit grepped free_memory in syscall.c ONLY and
// concluded "unmap is the only ring-3-reachable path that frees task user
// pages". It was not — the GUI frees task pages too, from a syscall. The rule
// that survives, written where the next such allocator lives: ANY code that
// frees memory currently mapped into a task's address space as user-writable
// must take that task's signalLock across the free, or delivery can be
// standing in it.
//
// Lock order is kGuiLock -> signalLock (every caller here already holds
// kGuiLock, and no signal path ever takes kGuiLock), so this nests without a
// cycle. One free_memory for the whole extent, so the hold is bounded no
// matter how large the canvas.
// THE ONE PLACE TASK-MAPPED CANVAS PAGES GO BACK. Every caller goes through
// here so the signalLock rule above is enforced by CONSTRUCTION rather than by
// each site remembering it — which matters because the site that forgot it was
// found by review, not by testing, and the next one would be too.
//
// The owner is a PRECONDITION, not an optional extra: a task-backed canvas has
// exactly one owning address space by definition, and both the unmap and the
// lock need it. Refusing beats freeing pages we cannot unmap, which would
// leave the task a live mapping to memory the allocator has given away.
static void canvas_pages_release(task_t *owner, uintptr_t va, uint64_t bytes,
                                 uintptr_t phys)
{
	if (owner == NULL || phys == 0 || bytes == 0)
		return;

	uint64_t sf = spinlock_acquire_irqsave(&owner->signalLock);
	paging_unmap_pages((pt_entry_t *)owner->pml4v, va, (size_t)bytes);
	free_memory(phys);
	spinlock_release_irqrestore(&owner->signalLock, sf);
}

static void canvas_release_locked(window_t *win, task_t *owner)
{
	if (win->canvas_task_phys == 0)
		return;

	canvas_pages_release(owner, win->canvas_task_va,
	                     (uint64_t)win->canvas_pages * PAGE_SIZE,
	                     win->canvas_task_phys);
	win->canvas.pixels = NULL;
	win->canvas_task_phys = 0;
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
	if (w > wm_dim_max() || h > wm_dim_max())   // OS64_GUI_WINDOW_DIM_MAX, or the screen if larger — window.c
		return GUI_ERR_BAD_ARGS;

	// Only documented CLIENT flags cross the boundary. The window struct's
	// flag word also carries bits the WM owns — transient state, and the
	// popup property it latches at birth — which cannot be claimed
	// accidentally through this creation call.
	const uint32_t client_flags = GUI_WINDOW_NO_DECORATIONS |
	                              GUI_WINDOW_START_UNFOCUSED |
	                              GUI_WINDOW_PINNED |
	                              GUI_WINDOW_DESKTOP | GUI_WINDOW_TITLE_UTF8 | GUI_WINDOW_HAS_SETTINGS;
	uint32_t create_flags = (uint32_t)flags & client_flags;

	// DESKTOP AND PINNED ARE A CONTRADICTION, AND IT USED TO RESOLVE BADLY
	// (Codex #31). "Always at the bottom" and "always on top" cannot both
	// hold; band_of() checks PINNED first, so a window claiming both was
	// placed at the very TOP of the z-list while calling itself the desktop —
	// and it could never be fixed, because wm_set_pinned declines every
	// desktop window, so nothing could unpin it.
	//
	// REFUSED rather than silently resolved, which is this boundary's habit:
	// an over-long title is refused instead of truncated, a slurp cap that
	// cannot be honoured is refused instead of clamped. Picking a winner here
	// would mean the caller asked for one thing and got another, and the flag
	// word it later reads back would not be the one it passed.
	if ((create_flags & GUI_WINDOW_DESKTOP) && (create_flags & GUI_WINDOW_PINNED)) {
		printd(DEBUG_GUI, "gui: window_create refused — DESKTOP and PINNED are contradictory\n");
		return GUI_ERR_BAD_ARGS;
	}
    bool popup_capacity = (flags & OS64_GUI_CREATE_POPUP_CAPACITY) != 0;
    if (popup_capacity &&
        (create_flags & (GUI_WINDOW_NO_DECORATIONS | GUI_WINDOW_PINNED)) !=
        (GUI_WINDOW_NO_DECORATIONS | GUI_WINDOW_PINNED)) return GUI_ERR_BAD_ARGS;
    uint16_t parent_handle = (uint16_t)((flags & OS64_GUI_CREATE_OWNER_MASK) >> OS64_GUI_CREATE_OWNER_SHIFT);
    uint32_t parent_id = 0;
    int64_t failure = GUI_ERR_NO_RESOURCES;
    if (parent_handle) {
        if ((create_flags & (GUI_WINDOW_NO_DECORATIONS | GUI_WINDOW_PINNED)) !=
            (GUI_WINDOW_NO_DECORATIONS | GUI_WINDOW_PINNED) ||
            (create_flags & GUI_WINDOW_START_UNFOCUSED)) return GUI_ERR_BAD_ARGS;
        uint64_t lock = gui_lock(GUI_LOCK_WINDOW);
        int64_t err;
        window_t *parent = handle_lookup_owned(parent_handle, &err);
        if (parent) {
            parent_id = parent->id;
            err = (wm_focused() == parent && !parent->retiring) ? 0 : GUI_ERR_NOT_FOCUSED;
        }
        gui_unlock(lock);
        if (err) return err;
    }
	bool content_size=(flags & OS64_GUI_CREATE_CONTENT_SIZE)!=0;
	uint64_t sizing_flags=gui_lock(GUI_LOCK_WINDOW);
	os64_decor_insets_t insets=wm_decoration_insets(create_flags);
	int32_t content_w=(int32_t)w-(content_size?0:insets.left+insets.right);
	int32_t content_h=(int32_t)h-(content_size?0:insets.top+insets.bottom);
    // Match fitted popup capacity in the off-lock task allocation to birth.
    if (content_size && (flags & OS64_GUI_CREATE_FIT_SCREEN)) {
        int32_t max_w = (int32_t)kFrameBuffer.width-insets.left-insets.right;
        int32_t max_h = (int32_t)kFrameBuffer.height-insets.top-insets.bottom;
        if (content_w > max_w) content_w = max_w;
        if (content_h > max_h) content_h = max_h;
        w = (uint32_t)content_w; h = (uint32_t)content_h;
    }
	gui_unlock(sizing_flags);
	if (content_w < GUI_MIN_CONTENT || content_h < GUI_MIN_CONTENT)
		return GUI_ERR_BAD_ARGS;

	// ── THE SURFACE PIVOT (GRAPHICS.md, migration step 3) ───────────────
	// A ring-3 caller's canvas is not the kmalloc buffer wm_create builds:
	// it is task-owned pages, mapped into the CALLER's address space, with
	// the kernel keeping the HHDM alias as its own view of the same memory.
	// Decided long before it was built (the doc's "surface pivot" chapter):
	//   - allocate_memory_aligned, NOT kmalloc — task memory, isolated,
	//     HHDM-reachable exactly while allocated (the lazy-HHDM rule);
	//   - EAGERLY backed, never demand-paged — publish reads the HHDM
	//     alias, which only exists for allocated pages;
	//   - USER|WRITE|NO_EXECUTE — pixels are data; the W^X discipline
	//     applies to canvases like everything else;
	//   - the VA comes from the task's never-reuse bump allocator with a
	//     guard page, map()'s exact idiom — a dangling canvas pointer
	//     faults forever instead of aliasing the next mapping.
	// All of it happens BEFORE kGuiLock: allocation and page-table walks
	// are heavyweight, and the lock only needs to witness the finished
	// swap. Kernel-thread clients (task->kernelTask) skip all of this and
	// keep the kmalloc canvas — their pointers are kernel pointers.
	//
	// The content inset mirrors wm_create's math; when wm_create refuses a
	// degenerate size below, the pivot is undone on the same exit.
	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = (cls != NULL) ? cls->task : NULL;
	bool pivot = (task != NULL && !task->kernelTask &&
	              content_w >= GUI_MIN_CONTENT && content_h >= GUI_MIN_CONTENT);

	// The extent is sized to the CAPACITY, not to today's window — the
	// reservation that makes resize free (window.h's canvas_cap_w comment has
	// the full argument). wm_create applies the identical rule to its own
	// stores a few lines below; both call the one helper so they cannot
	// drift apart, which matters because a canvas smaller than the content it
	// is snapshotted into would be a buffer overrun with a view of the screen.
	uint32_t cap_w = 0, cap_h = 0;
	wm_canvas_capacity_for(content_w, content_h, popup_capacity, &cap_w, &cap_h);

	uint64_t canvas_phys = 0;
	uintptr_t canvas_va = 0;
	uint64_t canvas_bytes = 0;
	if (pivot) {
		canvas_bytes = ((uint64_t)cap_w * (uint64_t)cap_h * 4 +
		                PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
		canvas_phys = allocate_memory_aligned(canvas_bytes);
		if (canvas_phys == 0)
			return GUI_ERR_NO_RESOURCES;
		canvas_va = task->heapEnd;
		if (canvas_va + canvas_bytes + PAGE_SIZE >= TASK_HEAP_END) {
			free_memory(canvas_phys);
			return GUI_ERR_NO_RESOURCES;
		}
		task->heapEnd = canvas_va + canvas_bytes + PAGE_SIZE;   // + guard
		paging_map_pages((pt_entry_t *)task->pml4v, canvas_va, canvas_phys,
		                 canvas_bytes / PAGE_SIZE,
		                 PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NO_EXECUTE);
	}

	uint64_t irqflags = gui_lock(GUI_LOCK_WINDOW);

	// Re-derive against the style held for creation; an Apply may have won
	// while the canvas was allocated. Check the staged capacity before use.
	insets=wm_decoration_insets(create_flags);
	if (content_size && (flags & OS64_GUI_CREATE_FIT_SCREEN)) {
		int64_t available_w=(int64_t)kFrameBuffer.width-insets.left-insets.right;
		int64_t available_h=(int64_t)kFrameBuffer.height-insets.top-insets.bottom;
		if (available_w<GUI_MIN_CONTENT || available_h<GUI_MIN_CONTENT) {
			gui_unlock(irqflags);
			goto undo_pivot;
		}
		if (w>(uint64_t)available_w) w=(uint32_t)available_w;
		if (h>(uint64_t)available_h) h=(uint32_t)available_h;
	}
	uint64_t frame_w=(uint64_t)w+(content_size?(uint32_t)(insets.left+insets.right):0);
	uint64_t frame_h=(uint64_t)h+(content_size?(uint32_t)(insets.top+insets.bottom):0);
	int64_t final_cw=(int64_t)frame_w-insets.left-insets.right;
	int64_t final_ch=(int64_t)frame_h-insets.top-insets.bottom;
	if (frame_w>wm_dim_max() || frame_h>wm_dim_max() || final_cw<GUI_MIN_CONTENT ||
		final_ch<GUI_MIN_CONTENT || (pivot && (final_cw>cap_w || final_ch>cap_h))) {
		gui_unlock(irqflags);
		goto undo_pivot;
	}
	w=(uint32_t)frame_w;h=(uint32_t)frame_h;
	if (content_size && (flags & OS64_GUI_CREATE_FIT_SCREEN)) {
		if (x<0) x=0;
		if (y<0) y=0;
		if ((int64_t)x+w>kFrameBuffer.width) x=(int32_t)(kFrameBuffer.width-w);
		if ((int64_t)y+h>kFrameBuffer.height) y=(int32_t)(kFrameBuffer.height-h);
	}

	int64_t handle = 0;
	for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
		if (!s_handles[i]) {
			handle = i + 1;
			break;
		}
	}
	if (!handle) {
		gui_unlock(irqflags);
		goto undo_pivot;
	}

    if (parent_handle) {
        int64_t err;
        window_t *parent = handle_lookup_owned(parent_handle, &err);
        if (!parent || parent->id != parent_id || parent->retiring || wm_focused() != parent) {
            failure = !parent ? err : GUI_ERR_NOT_FOCUSED;
            gui_unlock(irqflags);
            goto undo_pivot;
        }
    }
    // Identity and association precede birth focus delivery in this lock hold.
    window_t *win = wm_create(title, (rect_t){x, y, (int32_t)w, (int32_t)h},
                              create_flags, gui_current_task_id(), (uint16_t)handle, parent_id, popup_capacity);
	if (!win) {
		gui_unlock(irqflags);
		goto undo_pivot;
	}

	if (pivot) {
		// The swap: the kmalloc back buffer retires, the task-backed pages
		// take its place. The kernel's view is the HHDM alias — valid
		// exactly while the extent is allocated, which is the lifetime the
		// release path (canvas_release_locked) enforces.
		surface_free(&win->canvas);
		win->canvas.pixels   = (uint32_t *)(canvas_phys | kHHDMOffset);
		win->canvas.width    = win->content.width;
		win->canvas.height   = win->content.height;
		// The RESERVATION's width, matching the extent that was just mapped
		// — and the number the client will address rows by for this window's
		// whole life, resizes included.
		win->canvas.pitch_px = win->canvas_cap_w;
		win->canvas_task_phys = canvas_phys;
		win->canvas_task_va   = canvas_va;
		win->canvas_pages     = (uint32_t)(canvas_bytes / PAGE_SIZE);
		// Freshly allocated pages are ZEROED (the choke-point rule) =
		// black; wm_create filled content with its initial color. Match
		// them, exactly as the kmalloc canvas was matched, so a client's
		// first PARTIAL publish doesn't snapshot a mismatched border
		// around its damage rect. Row-wise now that both surfaces are
		// capacity-backed: the rows are contiguous only up to `width`, and
		// beyond it lies reservation nobody has drawn in yet.
		for (uint32_t row = 0; row < win->content.height; row++)
			memcpy(win->canvas.pixels + (size_t)row * win->canvas.pitch_px,
			       win->content.pixels + (size_t)row * win->content.pitch_px,
			       (size_t)win->content.width * 4);
	}

	s_handles[handle - 1] = win;

	gui_unlock(irqflags);
	return handle;

undo_pivot:
	// The window never came to be; give back what the pivot staged. The VA
	// stays burned — never-reuse is the allocator's whole tripwire.
	//
	// THROUGH THE SAME CHOKE POINT as a normal release, and not because it is
	// tidier: these pages are already MAPPED into the task (the pivot mapped
	// them a few lines up), user-writable, at a VA a sibling can work out —
	// task->heapEnd is not a secret. So this rollback is subject to exactly
	// the rule the release path is: free them without the owner's signalLock
	// and a concurrent signal delivery can be mid-write inside them. Narrow,
	// since it only runs when window creation fails — but rd8's window was
	// narrow too, and it was a kernel panic. Routed here rather than fixed in
	// place so the next person to free a canvas cannot get it wrong.
	canvas_pages_release(task, canvas_va, canvas_bytes, canvas_phys);
	return failure;
}

// Future syscall: SYSCALL_GUI_WINDOW_DESTROY (17), user_ptr_mask 0.
int64_t gui_window_destroy(int64_t handle)
{
	int64_t err;
	uint64_t irqflags = gui_lock(GUI_LOCK_WINDOW);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		gui_unlock(irqflags);
		return err;
	}
    for (unsigned i = 0; i < GUI_MAX_WINDOWS; ++i) {
        if (s_handles[i] && s_handles[i]->popup_owner_id == win->id) {
            gui_unlock(irqflags);
            return GUI_ERR_BUSY;
        }
    }
	s_handles[handle - 1] = NULL;
	// The owner check above means the CALLER's address space is where a
	// task-backed canvas lives — release it through the caller's own pml4v
	// before wm_destroy (whose surface_free then no-ops on the NULLed
	// pixels). RELOAD_CR3 after: this task keeps running, and a stale TLB
	// entry for the unmapped canvas VA would let it scribble on pages
	// already recycled to someone else. (A SIBLING thread on another core
	// could still hold that stale entry until its next CR3 load — accepted
	// for v1: GUI clients are single-threaded, the burned VA can never
	// alias a new mapping, and the exposure ends at the sibling's next
	// context switch.)
	if (win->canvas_task_phys != 0) {
		core_local_storage_t *cls = get_core_local_storage();
		canvas_release_locked(win, cls->task);
		RELOAD_CR3
	}
	wm_destroy(win);   // damages the vacated area itself
	gui_unlock(irqflags);
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
	uint64_t irqflags = gui_lock(GUI_LOCK_WINDOW);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		gui_unlock(irqflags);
		return err;
	}
	*out = win->canvas;
	// A task-backed canvas is answered with the TASK's address for it — the
	// kernel's HHDM alias in canvas.pixels means nothing in ring 3. The
	// owner check above guarantees the asker is exactly the task this VA
	// belongs to. Kernel-backed windows keep the kernel pointer, which is
	// what their kernel-thread owners dereference.
	if (win->canvas_task_phys != 0)
		out->pixels = (uint32_t *)win->canvas_task_va;
	gui_unlock(irqflags);
	return 0;
}

// The readback half of create (SYSCALL_GUI_WINDOW_GET_STATE, 2026-08-23).
// Answers with the FRAME rect — create's own units, so a saved state hands
// straight back — and the live flag word, MASKED to the bits the ABI
// publishes. The mask is not paranoia: window_t.flags is the window system's
// scratch space and whatever state it grows next would otherwise leak into a
// contract ring 3 was told is stable.
int64_t gui_window_get_state(int64_t handle, os64_gui_window_state_t *out)
{
	if (!out)
		return GUI_ERR_BAD_ARGS;
	int64_t err;
	uint64_t irqflags = gui_lock(GUI_LOCK_WINDOW);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		gui_unlock(irqflags);
		return err;
	}
	out->x      = win->frame.x;
	out->y      = win->frame.y;
	out->width  = (uint32_t)win->frame.w;
	out->height = (uint32_t)win->frame.h;
	out->flags  = win->flags & (GUI_WINDOW_NO_DECORATIONS |
	                            GUI_WINDOW_START_UNFOCUSED |
	                            GUI_WINDOW_PINNED |
	                            GUI_WINDOW_DESKTOP |   // the band is state too (Codex #31 rd3): without it a saved-and-recreated desktop came back an ordinary window
	                            GUI_WINDOW_MAXIMIZED |
	                            GUI_WINDOW_MINIMIZED |
	                            GUI_WINDOW_COVERED | GUI_WINDOW_TITLE_UTF8 | GUI_WINDOW_HAS_SETTINGS);   // can anyone see me? — the frame clock's question
	gui_unlock(irqflags);
	return 0;
}

int64_t gui_window_focus(int64_t handle)
{
    int64_t err;
    uint64_t flags=gui_lock(GUI_LOCK_WINDOW);
    window_t *win=handle_lookup_owned(handle,&err);
    if(win && (win->flags & (GUI_WINDOW_DESKTOP | GUI_WINDOW_POPUP))) {
        win=NULL;err=GUI_ERR_BAD_ARGS;
    }
    if(win) { wm_set_minimized(win,false);wm_raise(win); }
    gui_unlock(flags);
    return win?0:err;
}

int64_t gui_window_set_min_size(int64_t handle, uint32_t width, uint32_t height)
{
	int64_t err;
	uint64_t irqflags = gui_lock(GUI_LOCK_WINDOW);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		gui_unlock(irqflags);
		return err;
	}
	bool valid = wm_set_min_size(win, width, height);
	gui_unlock(irqflags);
	return valid ? 0 : GUI_ERR_BAD_ARGS;
}

// The most pieces of one publish's visible part that go on the damage list;
// past it, their bounding box goes instead. Four is one rectangle minus one
// cover in its middle, the ring. It is small because the list is: a full
// list collapses into one union of everything pending, far worse than one
// window's box, and /sys/gui's collapse count says whether this is right.
#define PUBLISH_PIECES_MAX 4

// Future syscall: SYSCALL_GUI_WINDOW_PUBLISH (19) — renamed from "present"
// at design review ("present" doubles as an adjective and is swapchain
// jargon besides). Damage rect is NULLABLE, so it stays OUT of the
// user_ptr_mask (the SETENV precedent) and the handler validates its copy.
int64_t gui_window_publish(int64_t handle, const rect_t *damage)
{
	int64_t err;
	uint64_t irqflags = gui_lock(GUI_LOCK_PUBLISH);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		gui_unlock(irqflags);
		return err;
	}

	// Clip the damage to the content bounds (NULL = the whole content).
	rect_t content_bounds = {0, 0, (int32_t)win->content.width, (int32_t)win->content.height};
	rect_t local;
	if (damage) {
		if (!rect_intersect(*damage, content_bounds, &local)) {
			gui_unlock(irqflags);
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

	// Occlusion (OCCLUSION.md): only the part of this rect no window above
	// covers can change the glass, so only that is damaged. None of it: no
	// damage, no composite, no flush.
	//
	// The SNAPSHOT above still happened, and that is exactly why this check
	// comes last. `content` holds the client's finished frame, so when the
	// covering window moves away the newly uncovered region is damaged by the
	// MOVE and recomposites from an up-to-date snapshot. Skipping the snapshot
	// as well would save a memcpy and show a stale frame on reveal — the
	// classic occlusion bug, and not one worth reinventing.
	//
	// This is the compositor-side half only. The CLIENT still draws and
	// publishes at full rate into a canvas nobody can see; teaching it to idle
	// needs a publish acknowledgment it can wait on — Wayland's frame-callback
	// insight, and the reason X11's VisibilityNotify never worked (it asked
	// applications to volunteer, and they didn't). That half rides libui; see
	// the occlusion row in DEBTS.
	rect_t seen[PUBLISH_PIECES_MAX];
	size_t n = wm_visible_damage(win, screen_damage, seen, PUBLISH_PIECES_MAX);
	for (size_t i = 0; i < n; i++)
		gui_damage_add_locked(seen[i]);
	gui_unlock(irqflags);
	return 0;
}

// Future syscall: SYSCALL_GUI_EVENT_POLL (20), user_ptr_mask 0b10 (out-event).
int64_t gui_event_poll(int64_t handle, input_event_t *out)
{
	if (!out)
		return GUI_ERR_BAD_ARGS;
	int64_t err;
	uint64_t irqflags = gui_lock(GUI_LOCK_POLL);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		gui_unlock(irqflags);
		return err;
	}
	bool got = wm_pop_event(win, out);
	gui_unlock(irqflags);
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

int64_t gui_event_ring(int64_t handle, uint32_t mask)
{
	if (mask == 0)
		return GUI_ERR_BAD_ARGS;
	int64_t err;
	uint64_t irqflags = gui_lock(GUI_LOCK_RING);
	window_t *win = handle_lookup_owned(handle, &err);
	if (!win) {
		gui_unlock(irqflags);
		return err;
	}
	input_event_t ev = {.type = INPUT_EVENT_DOORBELL,
		.doorbell = {.mask = mask}, .tick = kTicksSinceStart};
	wm_deliver_event(win, &ev);
	gui_unlock(irqflags);
	return 0;
}

// Future syscall: SYSCALL_GUI_EVENT_WAIT (22), user_ptr_mask 0b10 — the
// blocking poll, and the LAST piece of the migration order on purpose:
// everything else worked without it, so it shipped when the plumbing could
// be its whole slice.
//
// The gait is console_read's, ported: drain → return if got → register as
// the window's waiter → park on a SIGSLEEP backstop → re-loop on wake. The
// wake is EDGE-triggered (wm_deliver_event aims at the waiter the moment it
// pushes) and closed against the lost race: a deliverer that finds us still
// mid-park (registered, not yet ISLEEP) OWES us the wake, and the requeue
// that would park us pays it instead (scheduler_wake_or_owe, WAKE_OWED.md).
// The backstop stays as the house rule that a missed edge costs a tick,
// never a hang. A pending TERMINATE — or a signal a handler will catch —
// outranks the wait, checked every pass (processSignals wakes sleepers for
// exactly those; this check is how a woken waiter LEAVES).
#define GUI_EVENT_WAIT_BACKSTOP_TICKS (TICKS_PER_SECOND / 4)

//
// out == NULL is the PEEK-WAIT (2026-08-27, the frame clock's covered
// pause): block until an event is queued, return 1, and leave the event
// where it is for the caller's own poll to take. Same park, same wake,
// same exits; the only difference is that the pop becomes a look.
int64_t gui_event_wait(int64_t handle, input_event_t *out)
{
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = (cls != NULL) ? cls->currentThread : NULL;
	if (self == NULL)
		return GUI_ERR_BAD_ARGS;   // no thread context — nothing to park

	for (;;) {
		int64_t err;
		uint64_t irqflags = gui_lock(GUI_LOCK_WAIT);

		// Awake at the top of a pass: a wake owed for the last park is void
		// — what it announced is in the queue, and the pop below takes it —
		// and no deliverer can owe us another until we register again, since
		// they owe under this lock (WAKE_OWED.md).
		scheduler_forget_owed_wake(self);

		// The window is re-looked-up EVERY pass: it can die while we sleep
		// (our own task's exit sweep is the usual killer), and a handle is
		// only ever as fresh as the last time the lock said so.
		window_t *win = handle_lookup_owned(handle, &err);
		if (!win) {
			gui_unlock(irqflags);
			return err;
		}

		// The registration the last park made is void too (the signals
		// doctrine): a slot left naming a thread that returned is a spurious
		// wake out of some LATER unrelated sleep — console_read's scar.
		if (win->waiter == self)
			win->waiter = NULL;

		if (signal_park_must_end(self)) {   // a terminate, or a signal a handler is waiting for
			gui_unlock(irqflags);
			return GUI_ERR_INTERRUPTED;
		}

        if (win->waiter && win->waiter != self) {
            gui_unlock(irqflags);
            return GUI_ERR_BUSY;
        }

		if (out ? wm_pop_event(win, out) : wm_has_event(win)) {
			gui_unlock(irqflags);
			return 1;
		}

		// Empty-handed: register and park. Registration happens in the SAME
		// critical section as the failed pop, so a deliverer either pushed
		// before our pop (we returned above) or will see our registration.
		win->waiter = self;
		gui_unlock(irqflags);

		signal_raise(SIGSLEEP,
		             kTicksSinceStart + GUI_EVENT_WAIT_BACKSTOP_TICKS, self);
	}
}

int64_t gui_event_wait_any(int64_t *out_handle)
{
    if (!kEnableGUI) return GUI_ERR_NOT_RUNNING;
    core_local_storage_t *cls = get_core_local_storage();
    thread_t *self = cls ? cls->currentThread : NULL;
    if (!self || !out_handle) return GUI_ERR_BAD_ARGS;
    const uint64_t owner = gui_current_task_id();
    for (;;) {
        uint64_t irqflags = gui_lock(GUI_LOCK_WAIT);
        // Awake at the top: the last park's debt is void with its slots
        // (gui_event_wait says why). Several windows' deliverers may have
        // owed one wake; the scan below finds every event they announced.
        scheduler_forget_owed_wake(self);
        unsigned count = 0;
        bool busy = false;
        // Cleanup covers the whole table even if a former selected slot died.
        for (unsigned i = 0; i < GUI_MAX_WINDOWS; ++i) {
            window_t *w = s_handles[i];
            if (!w) continue;
            if (w->waiter == self) w->waiter = NULL;
            if (w->owner != owner) continue;
            ++count;
            if (w->waiter) busy = true;
        }
        int64_t rc = signal_park_must_end(self) ? GUI_ERR_INTERRUPTED :
                     !count ? GUI_ERR_INVALID_HANDLE : busy ? GUI_ERR_BUSY : 0;
        if (rc) {
            gui_unlock(irqflags);
            return rc;
        }
        for (unsigned n = 0; n < GUI_MAX_WINDOWS; ++n) {
            unsigned i = (self->gui_wait_cursor + n) % GUI_MAX_WINDOWS;
            window_t *w = s_handles[i];
            if (w && w->owner == owner && wm_has_event(w)) {
                *out_handle = (int64_t)i + 1;
                self->gui_wait_cursor = (i + 1) % GUI_MAX_WINDOWS;
                gui_unlock(irqflags);
                return 1;
            }
        }
        for (unsigned i = 0; i < GUI_MAX_WINDOWS; ++i)
            if (s_handles[i] && s_handles[i]->owner == owner)
                s_handles[i]->waiter = self;
        gui_unlock(irqflags);
        signal_raise(SIGSLEEP, kTicksSinceStart + GUI_EVENT_WAIT_BACKSTOP_TICKS, self);
    }
}

int64_t gui_window_get_content_rect(int64_t handle, rect_t *out)
{
    if (!out) return GUI_ERR_BAD_ARGS;
    int64_t err;
    uint64_t irqflags = gui_lock(GUI_LOCK_WINDOW);
    window_t *win = handle_lookup_owned(handle, &err);
    if (win) *out = wm_content_rect_on_screen(win);
    gui_unlock(irqflags);
    return win ? 0 : err;
}

// For a window without a popup owner, Alt+F4 can escalate an unanswered close:
// the owner is told to stop the way `kill` would tell it — SIGTERM, which a
// program may still catch to say goodbye. The window itself is never
// destroyed from here: it dies in the task's own exit path with everything
// else the task owned, which is the only path that can give the canvas
// pages back safely (see gui_task_destroy_windows). Kernel-thread owners —
// the compositor's own window, the demos — cannot be signalled and are
// refused by name. Walks kTaskList the way procfs does; runs in the
// compositor's thread, OUTSIDE kGuiLock (task_signal_and_nudge takes
// scheduler locks, and the GUI lock must never nest under those).
void gui_window_terminate_owner(uint64_t owner)
{
	if (owner == 0)
		return;
	for (task_t *t = kTaskList; t != NULL && t != (task_t *)NO_TASK; t = t->next) {
		if (t->taskID != owner)
			continue;
		if (t->kernelTask || t->exited) {
			printd(DEBUG_GUI, "gui: close escalation refused — task %lu is %s\n",
			       owner, t->kernelTask ? "a kernel thread" : "already exiting");
			return;
		}
		printd(DEBUG_GUI, "gui: close escalation — SIGTERM to task %lu (%s)\n", owner, t->path);
		task_signal_and_nudge(t, SIGTERM);
		return;
	}
	printd(DEBUG_GUI, "gui: close escalation — no task %lu (already gone)\n", owner);
}

// The task-exit sweep — contract in compositor.h. NOT a client call and
// never dispatched as a syscall: task.c invokes it on the way out, so a
// task cannot decline its own cleanup any more than it can decline
// handle_close_all. This is the enforcement half of the ownership rule;
// the checks above are merely the courtesy half.
void gui_task_destroy_windows(struct task *t)
{
	// taskID 0 is the not-a-task sentinel gui_current_task_id() returns
	// when CLS has no task — refusing it here means a sweep can never
	// match windows created from such a context by mistake.
	if (!kEnableGUI || t == NULL || t->taskID == 0)
		return;

	uint64_t irqflags = gui_lock(GUI_LOCK_WINDOW);
    // Exclude the whole exiting family before any focus succession runs.
    for (unsigned i = 0; i < GUI_MAX_WINDOWS; ++i) {
        if (s_handles[i] && s_handles[i]->owner == t->taskID)
            s_handles[i]->retiring = true;
    }
	for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
		window_t *win = s_handles[i];
		if (win == NULL || win->owner != t->taskID)
			continue;
		s_handles[i] = NULL;
		printd(DEBUG_GUI, "gui: task 0x%08x exited owning window '%s' (handle %d) — destroying\n",
		       t->taskID, win->title, i + 1);
		// A task-backed canvas goes back to the allocator HERE — nothing
		// else records those pages (deliberately not VMAs), so the sweep is
		// their only undertaker. The address space is still intact at both
		// call sites (exit teardown and pre-teardown burial), which is what
		// makes the unmap legal; no TLB flush needed — the task never runs
		// again, and free_memory's HHDM shootdown covers the alias side.
		canvas_release_locked(win, t);
		wm_destroy(win);   // damages the vacated area itself
	}
	gui_unlock(irqflags);
}
