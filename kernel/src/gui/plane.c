// plane.c — a window's planes (plane.h; docs/design/pending/VIDEO_PLANE.md
// § 3). The calls, the commit a window publish makes with its edits, the
// compositor's part, and the pairs of stores a task keeps.

#include "gui/plane.h"
#include "gui/gui_client.h"
#include "gui/gui_internal.h"
#include "gui/window.h"
#include "gui/surface.h"
#include "CONFIG.h"
#include "memcpy.h"
#include "memory/memset.h"
#include "smp_core.h"
#include "task.h"
#include "thread.h"
#include "memory/allocator.h"
#include "memory/kmalloc.h"
#include "memory/paging.h"
#include "driver/system/x86_64.h"   // rdtsc — the compositor's part times itself
#include "gui/compositor.h"   // the frame epoch and the request's nudge
#include "scheduler.h"        // the wait's wake
#include "signals.h"          // the wait's park and its exit
#include "kernel.h"           // kTicksSinceStart — the park's deadline
#include "micros.h"           // the patience
#include "video.h"            // kFrameBuffer — "shown" ends at the screen's edge

_Static_assert(GUI_WINDOW_PLANES == OS64_GUI_PLANES, "the plane ABI's slots drifted from the kernel's");
_Static_assert(PLANE_OVERLAYS_MAX == OS64_GUI_PLANE_OVERLAYS, "the plane ABI's overlays drifted from the composer's");

extern bool kEnableGUI;
extern uint64_t kHHDMOffset;
extern uint64_t kCPUCyclesPerSecond;

// A pair of stores and its record. The record is made first, outside
// kGuiLock, and joins its task's list (task->planeStores) before its pair is
// made, also outside the lock; it stays there until the task is buried, in
// use or kept, unless its pair could not be made, when it leaves the list and
// is freed. `ready` is false while the pair is being made: its bytes count
// against the budget from the moment the record joins the list, so two
// creators cannot both slip under it.
typedef struct plane_store {
	struct plane_store *next;
	uint32_t *canvas;       // the kernel's way to the canvas (its HHDM alias, or a kernel task's own buffer)
	uintptr_t canvas_va;    // where the task sees the canvas
	uint64_t canvas_phys;   // 0: a kernel task's canvas, kmalloc'd
	uint64_t canvas_bytes;  // page-rounded
	uint32_t *front;
	uint32_t cap_w, cap_h;
	bool in_use;
	bool ready;
} plane_store_t;

// The capacity classes (VIDEO_PLANE.md § 3.8): the smallest holding what
// was asked.
static const struct {
	uint32_t w, h;
} kPlaneClasses[] = {
	{640, 360}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160},
};
#define PLANE_CLASSES (sizeof(kPlaneClasses) / sizeof(kPlaneClasses[0]))

// The machine's pairs and refusals, for /sys/gui: written under kGuiLock,
// except the refusals, which are counted where they happen, some outside it.
static uint64_t s_pairs, s_pairs_kept, s_pair_bytes;
static uint64_t s_refused[GUI_PLANE_REFUSALS];
static uint64_t s_budget_for_test;

static const char *const kRefusalNames[GUI_PLANE_REFUSALS] = {
	"not the creator", "no such plane", "no slot", "above the largest class", "over budget",
	"no memory", "busy", "damage short of the edits", "clips meet or overlays out", "picture size",
};

const char *gui_plane_refusal_name(gui_plane_refusal_t why)
{
	return why < GUI_PLANE_REFUSALS ? kRefusalNames[why] : "?";
}

static int64_t refuse(gui_plane_refusal_t why, int64_t code)
{
	__atomic_add_fetch(&s_refused[why], 1, __ATOMIC_RELAXED);
	return code;
}

static uint64_t plane_budget(const task_t *t)
{
	return t->kernelTask && s_budget_for_test ? s_budget_for_test : GUI_PLANE_BYTES;
}

static uint64_t page_round(uint64_t bytes)
{
	return (bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static uint64_t pair_bytes(uint32_t cap_w, uint32_t cap_h)
{
	uint64_t one = (uint64_t)cap_w * cap_h * 4;
	return page_round(one) + one;
}

static bool plane_class(uint32_t w, uint32_t h, uint32_t *cap_w, uint32_t *cap_h)
{
	for (unsigned i = 0; i < PLANE_CLASSES; i++) {
		if (w <= kPlaneClasses[i].w && h <= kPlaneClasses[i].h) {
			*cap_w = kPlaneClasses[i].w;
			*cap_h = kPlaneClasses[i].h;
			return true;
		}
	}
	return false;
}

// A plane's name: its slot (1-based) in the low byte, its generation in the
// 24 bits above. Never 0. A slot's generation moves at every create, so an
// old name meets a new plane only after 2^24 of them in that slot.
#define PLANE_GENERATION_MASK 0xFFFFFFu
static uint32_t plane_name(unsigned slot, uint32_t generation)
{
	return ((generation & PLANE_GENERATION_MASK) << 8) | (slot + 1);
}

static pc_rect_t to_pc(rect_t r) { return (pc_rect_t){r.x, r.y, r.w, r.h}; }
static rect_t to_rect(pc_rect_t r) { return (rect_t){r.x, r.y, r.w, r.h}; }
static bool pc_empty(pc_rect_t r) { return r.w <= 0 || r.h <= 0; }

static bool pc_same(pc_rect_t a, pc_rect_t b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// The part of `r` inside `bounds`, empty (all zero) when they do not meet.
static pc_rect_t pc_cut(pc_rect_t r, pc_rect_t bounds)
{
	rect_t out;
	if (pc_empty(r) || !rect_intersect(to_rect(r), to_rect(bounds), &out))
		return (pc_rect_t){0, 0, 0, 0};
	return to_pc(out);
}

// Whether `inner` (empty counts as inside anything) lies inside `outer`.
static bool pc_inside(pc_rect_t inner, pc_rect_t outer)
{
	if (pc_empty(inner))
		return true;
	return !pc_empty(outer) && inner.x >= outer.x && inner.y >= outer.y &&
	       (int64_t)inner.x + inner.w <= (int64_t)outer.x + outer.w &&
	       (int64_t)inner.y + inner.h <= (int64_t)outer.y + outer.h;
}

static pc_rect_t content_bounds(const window_t *w)
{
	return (pc_rect_t){0, 0, (int32_t)w->content.width, (int32_t)w->content.height};
}

// The plane a name gives in an owned window, or NULL with the error.
static gui_plane_t *plane_lookup(window_t *win, uint32_t name, int64_t *err)
{
	unsigned slot = (name & 0xFF) - 1;
	if ((name & 0xFF) == 0 || slot >= GUI_WINDOW_PLANES || win->planes[slot].state == GUI_PLANE_FREE ||
	    win->planes[slot].generation != (name >> 8)) {
		*err = refuse(GUI_PLANE_REFUSED_NO_PLANE, OS64_GUI_ERR_NO_PLANE);
		return NULL;
	}
	return &win->planes[slot];
}

static thread_t *current_thread(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	return cls ? cls->currentThread : NULL;
}

// The plane the creator names, or NULL with the error. Under kGuiLock.
static gui_plane_t *creator_plane_locked(int64_t handle, uint32_t name, window_t **win_out, int64_t *err)
{
	window_t *win = gui_handle_owned_locked(handle, err);
	if (win == NULL)
		return NULL;
	gui_plane_t *p = plane_lookup(win, name, err);
	if (p == NULL)
		return NULL;
	if (p->creator != current_thread()) {
		*err = refuse(GUI_PLANE_REFUSED_NOT_CREATOR, OS64_GUI_ERR_NOT_CREATOR);
		return NULL;
	}
	if (win_out)
		*win_out = win;
	return p;
}

// The visible pieces of a part of the content damaged, the part given in
// the content's coordinates and cut to the content as it stands. `glass`
// starts the publish-to-glass wait for them (a picture published).
static void damage_content_locked(window_t *win, pc_rect_t part, bool glass)
{
	pc_rect_t local = pc_cut(part, content_bounds(win));
	if (pc_empty(local))
		return;
	rect_t cs = wm_content_rect_on_screen(win);
	rect_t screen = {cs.x + local.x, cs.y + local.y, local.w, local.h};
	rect_t seen[4];
	size_t n = wm_visible_damage(win, screen, seen, 4);
	for (size_t i = 0; i < n; i++)
		gui_damage_add_locked(seen[i]);
	if (glass)
		gui_glass_publish_locked(seen, n);
}

// The slot back to FREE, its pair kept for the task. The generation stays,
// so the old name stays refused. Under kGuiLock; cannot fail.
static void plane_free_locked(gui_plane_t *p)
{
	if (p->store != NULL && p->store->in_use) {
		p->store->in_use = false;
		s_pairs_kept++;
	}
	uint32_t generation = p->generation;
	memset(p, 0, sizeof(*p));
	p->generation = generation;
	p->state = GUI_PLANE_FREE;
}

bool gui_window_has_planes(const window_t *w)
{
	for (unsigned i = 0; i < GUI_WINDOW_PLANES; i++)
		if (w->planes[i].state != GUI_PLANE_FREE)
			return true;
	return false;
}

static int free_slot(const window_t *w)
{
	for (unsigned i = 0; i < GUI_WINDOW_PLANES; i++)
		if (w->planes[i].state == GUI_PLANE_FREE)
			return (int)i;
	return -1;
}

// ── The pairs ───────────────────────────────────────────────────────────────

static uint64_t task_pair_bytes_locked(const task_t *t)
{
	uint64_t bytes = 0;
	for (const plane_store_t *s = t->planeStores; s != NULL; s = s->next)
		bytes += pair_bytes(s->cap_w, s->cap_h);
	return bytes;
}

// The smallest kept pair that holds the class asked for: a larger one serves
// a smaller plane, so a task whose kept pairs fill its budget is not refused
// a plane they can hold.
static plane_store_t *kept_pair_locked(task_t *t, uint32_t cap_w, uint32_t cap_h)
{
	plane_store_t *best = NULL;
	for (plane_store_t *s = t->planeStores; s != NULL; s = s->next)
		if (s->ready && !s->in_use && s->cap_w >= cap_w && s->cap_h >= cap_h &&
		    (best == NULL || (uint64_t)s->cap_w * s->cap_h < (uint64_t)best->cap_w * best->cap_h))
			best = s;
	return best;
}

static void unlink_store_locked(task_t *t, plane_store_t *s)
{
	for (plane_store_t **at = &t->planeStores; *at != NULL; at = &(*at)->next) {
		if (*at == s) {
			*at = s->next;
			return;
		}
	}
}

// A pair's two stores, made outside kGuiLock: the canvas mapped into the
// task through its one door (a kernel task's is kernel memory), the front
// kernel memory. False and nothing left allocated when either fails.
static bool pair_prepare(task_t *t, plane_store_t *s)
{
	uint64_t one = (uint64_t)s->cap_w * s->cap_h * 4;
	uint64_t bytes = page_round(one);
	uint32_t *front = kmalloc_try(one);
	if (front == NULL)
		return false;
	if (t->kernelTask) {
		uint32_t *canvas = kmalloc_try(bytes);
		if (canvas == NULL) {
			kfree(front);
			return false;
		}
		s->canvas = canvas;
		s->canvas_va = (uintptr_t)canvas;
		s->canvas_phys = 0;
	} else {
		uint64_t phys = allocate_memory_try(bytes);
		uintptr_t va = 0;
		if (phys == 0) {
			kfree(front);
			return false;
		}
		// The range is burned if the mapping is not made: the door never
		// hands a range out twice.
		if (!task_va_reserve(t, bytes, &va)) {
			free_memory(phys);
			kfree(front);
			return false;
		}
		paging_map_pages((pt_entry_t *)t->pml4v, va, phys, bytes / PAGE_SIZE,
		                 PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NO_EXECUTE);
		s->canvas = (uint32_t *)(phys | kHHDMOffset);
		s->canvas_va = va;
		s->canvas_phys = phys;
	}
	s->canvas_bytes = bytes;
	s->front = front;
	return true;
}

static void pair_release(task_t *t, plane_store_t *s)
{
	if (s->canvas_phys != 0)
		gui_canvas_pages_release(t, s->canvas_va, s->canvas_bytes, s->canvas_phys);
	else if (s->canvas != NULL)
		kfree(s->canvas);
	if (s->front != NULL)
		kfree(s->front);
	kfree(s);
}

// Every pair of a list given back to the allocator, after kGuiLock.
static void release_list(task_t *t, plane_store_t *list)
{
	while (list != NULL) {
		plane_store_t *next = list->next;
		pair_release(t, list);
		list = next;
	}
}

// A list's pairs struck from the machine's counts, before they are freed.
static void forget_list_locked(const plane_store_t *list)
{
	for (const plane_store_t *s = list; s != NULL; s = s->next) {
		if (!s->ready)
			continue;
		s_pairs--;
		s_pair_bytes -= pair_bytes(s->cap_w, s->cap_h);
		if (!s->in_use)
			s_pairs_kept--;
	}
}

void gui_task_planes_release(task_t *t)
{
	if (t == NULL || t->planeStores == NULL)
		return;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	plane_store_t *list = t->planeStores;
	t->planeStores = NULL;
	forget_list_locked(list);
	gui_unlock(flags);
	release_list(t, list);
}

void gui_plane_release_kept_for_test(task_t *t)
{
	if (t == NULL || !t->kernelTask)
		return;
	plane_store_t *kept = NULL;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	plane_store_t **at = &t->planeStores;
	while (*at != NULL) {
		plane_store_t *s = *at;
		if (s->ready && !s->in_use) {
			*at = s->next;
			s->next = kept;
			kept = s;
		} else {
			at = &s->next;
		}
	}
	forget_list_locked(kept);
	gui_unlock(flags);
	release_list(t, kept);
}

void gui_plane_budget_for_test(uint64_t bytes)
{
	s_budget_for_test = bytes;
}

// ── The calls ───────────────────────────────────────────────────────────────

int64_t gui_plane_create(int64_t handle, uint32_t width, uint32_t height)
{
	if (!kEnableGUI)
		return GUI_ERR_NOT_RUNNING;
	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	thread_t *self = cls ? cls->currentThread : NULL;
	if (task == NULL || self == NULL || width == 0 || height == 0)
		return GUI_ERR_BAD_ARGS;
	uint32_t cap_w, cap_h;
	if (!plane_class(width, height, &cap_w, &cap_h))
		return refuse(GUI_PLANE_REFUSED_TOO_BIG, OS64_GUI_ERR_ABOVE_CAPACITY);

	plane_store_t *fresh = kmalloc_try(sizeof(*fresh));
	if (fresh == NULL)
		return refuse(GUI_PLANE_REFUSED_MEMORY, GUI_ERR_NO_RESOURCES);

	int64_t err;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	window_t *win = gui_handle_owned_locked(handle, &err);
	if (win == NULL || free_slot(win) < 0) {
		gui_unlock(flags);
		kfree(fresh);
		return win == NULL ? err : refuse(GUI_PLANE_REFUSED_NO_SLOT, GUI_ERR_NO_RESOURCES);
	}
	// The window's own id, never reused: a handle can be, by a window made
	// while the lock is let go below.
	uint32_t win_id = win->id;
	plane_store_t *store = kept_pair_locked(task, cap_w, cap_h);
	if (store != NULL) {
		store->in_use = true;
		s_pairs_kept--;
	} else {
		if (task_pair_bytes_locked(task) + pair_bytes(cap_w, cap_h) > plane_budget(task)) {
			gui_unlock(flags);
			kfree(fresh);
			return refuse(GUI_PLANE_REFUSED_BUDGET, OS64_GUI_ERR_OVER_BUDGET);
		}
		fresh->cap_w = cap_w;
		fresh->cap_h = cap_h;
		fresh->in_use = true;
		fresh->ready = false;
		fresh->next = task->planeStores;
		task->planeStores = fresh;
		store = fresh;
		fresh = NULL;
	}
	gui_unlock(flags);
	if (fresh != NULL)
		kfree(fresh);   // a kept pair served

	if (!store->ready) {
		bool made = pair_prepare(task, store);
		flags = gui_lock(GUI_LOCK_PLANE);
		if (!made) {
			unlink_store_locked(task, store);
			gui_unlock(flags);
			kfree(store);
			return refuse(GUI_PLANE_REFUSED_MEMORY, GUI_ERR_NO_RESOURCES);
		}
		store->ready = true;
		s_pairs++;
		s_pair_bytes += pair_bytes(cap_w, cap_h);
	} else {
		flags = gui_lock(GUI_LOCK_PLANE);
	}

	// The window and its slot again: either may have changed while the
	// lock was let go, and the handle may now name another window. A loser
	// keeps its pair for the task.
	win = gui_handle_owned_locked(handle, &err);
	if (win != NULL && win->id != win_id) {
		win = NULL;
		err = GUI_ERR_INVALID_HANDLE;
	}
	int slot = win ? free_slot(win) : -1;
	if (slot < 0) {
		store->in_use = false;
		s_pairs_kept++;
		gui_unlock(flags);
		return win == NULL ? err : refuse(GUI_PLANE_REFUSED_NO_SLOT, GUI_ERR_NO_RESOURCES);
	}
	gui_plane_t *p = &win->planes[slot];
	uint32_t generation = (p->generation + 1) & PLANE_GENERATION_MASK;
	memset(p, 0, sizeof(*p));
	p->generation = generation;
	p->state = GUI_PLANE_CREATED;
	p->creator = self;
	p->store = store;
	p->front = store->front;
	p->cap_w = store->cap_w;   // a kept larger pair may serve
	p->cap_h = store->cap_h;
	gui_unlock(flags);
	return (int64_t)plane_name((unsigned)slot, generation);
}

int64_t gui_plane_surface(int64_t handle, uint32_t name, os64_gui_plane_surface_t *out)
{
	if (out == NULL)
		return GUI_ERR_BAD_ARGS;
	int64_t err;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	gui_plane_t *p = creator_plane_locked(handle, name, NULL, &err);
	if (p == NULL) {
		gui_unlock(flags);
		return err;
	}
	*out = (os64_gui_plane_surface_t){
		.pixels = (uint32_t *)p->store->canvas_va,
		.pitch_px = p->cap_w,
		.cap_w = p->cap_w,
		.cap_h = p->cap_h,
	};
	gui_unlock(flags);
	return 0;
}

int64_t gui_plane_publish(int64_t handle, uint32_t name, uint32_t width, uint32_t height)
{
	int64_t err;
	window_t *win;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	gui_plane_t *p = creator_plane_locked(handle, name, &win, &err);
	if (p == NULL) {
		gui_unlock(flags);
		return err;
	}
	if (width == 0 || height == 0 || width > p->cap_w || height > p->cap_h) {
		gui_unlock(flags);
		return GUI_ERR_BAD_ARGS;
	}
	// A plane's picture changes size only while it is hidden (§ 3.5, rule 4).
	if (p->state == GUI_PLANE_PLACED && (width != p->pic_w || height != p->pic_h)) {
		gui_unlock(flags);
		return refuse(GUI_PLANE_REFUSED_PICTURE, OS64_GUI_ERR_PLANE_PICTURE);
	}
	const uint32_t *canvas = p->store->canvas;
	for (uint32_t y = 0; y < height; y++)
		memcpy(p->front + (size_t)y * p->cap_w, canvas + (size_t)y * p->cap_w, (size_t)width * 4);
	p->pic_w = width;
	p->pic_h = height;
	if (p->state == GUI_PLANE_CREATED)
		p->state = GUI_PLANE_PICTURED;
	p->publishes++;
	if (p->state == GUI_PLANE_PLACED)
		damage_content_locked(win, p->place.clip, true);
	gui_unlock(flags);
	return 0;
}

int64_t gui_plane_destroy(int64_t handle, uint32_t name)
{
	int64_t err;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	gui_plane_t *p = creator_plane_locked(handle, name, NULL, &err);
	if (p == NULL) {
		gui_unlock(flags);
		return err;
	}
	if (p->state == GUI_PLANE_PLACED) {
		gui_unlock(flags);
		return refuse(GUI_PLANE_REFUSED_BUSY, GUI_ERR_BUSY);
	}
	plane_free_locked(p);
	gui_unlock(flags);
	return 0;
}

void gui_thread_planes_exit(thread_t *self)
{
	if (!kEnableGUI || self == NULL || self->ownerTask == NULL)
		return;
	task_t *task = (task_t *)self->ownerTask;
	// A plane this thread created put a record on its task's list in this
	// thread's own create, so an empty list read here (without the lock)
	// means it created none.
	if (__atomic_load_n(&task->planeStores, __ATOMIC_RELAXED) == NULL)
		return;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	for (unsigned i = 0; i < GUI_MAX_WINDOWS; i++) {
		window_t *win = gui_handle_at_locked(i);
		if (win == NULL || win->owner != task->taskID)
			continue;
		for (unsigned s = 0; s < GUI_WINDOW_PLANES; s++) {
			gui_plane_t *p = &win->planes[s];
			if (p->state == GUI_PLANE_FREE || p->creator != self)
				continue;
			// Hidden first: the hole shows the content as it stands.
			if (p->state == GUI_PLANE_PLACED)
				damage_content_locked(win, p->place.clip, false);
			plane_free_locked(p);
		}
	}
	gui_unlock(flags);
}

// ── The commit ──────────────────────────────────────────────────────────────

static pc_place_t edit_place(const os64_gui_plane_edit_t *e)
{
	pc_place_t place;
	memset(&place, 0, sizeof(place));
	if (e->hide)
		return place;
	place.origin = to_pc(e->origin);
	place.clip = to_pc(e->clip);
	place.noverlays = e->noverlays;
	for (uint32_t i = 0; i < e->noverlays && i < PLANE_OVERLAYS_MAX; i++)
		place.overlay[i] = to_pc(e->overlay[i]);
	if (pc_empty(place.clip))
		memset(&place, 0, sizeof(place));   // an empty clip is no place
	return place;
}

static bool place_same(const pc_place_t *a, const pc_place_t *b)
{
	if (pc_empty(a->clip) && pc_empty(b->clip))
		return true;
	if (!pc_same(a->clip, b->clip) || !pc_same(a->origin, b->origin) || a->noverlays != b->noverlays)
		return false;
	for (uint32_t i = 0; i < a->noverlays; i++)
		if (!pc_same(a->overlay[i], b->overlay[i]))
			return false;
	return true;
}

// What can be checked of an edit list with no window: the count, the
// reserved fields, each place's arithmetic, and no plane named twice. A
// place is held to the largest content any window can have (wm_dim_max), so
// its overlay area is counted over rows no window could exceed: against
// INT32_MAX a clip a pixel wide and INT32_MAX tall would have the count walk
// two billion rows before any window was looked up.
static int64_t edits_shape(const os64_gui_plane_edit_t *edits, uint32_t count)
{
	if (count > OS64_GUI_PLANE_EDITS_MAX || (count > 0 && edits == NULL))
		return GUI_ERR_BAD_ARGS;
	for (uint32_t i = 0; i < count; i++) {
		const os64_gui_plane_edit_t *e = &edits[i];
		if (e->_reserved != 0)
			return GUI_ERR_BAD_ARGS;
		for (uint32_t j = 0; j < i; j++)
			if (edits[j].plane == e->plane)
				return GUI_ERR_BAD_ARGS;
		if (e->hide)
			continue;
		if (e->noverlays > PLANE_OVERLAYS_MAX)
			return GUI_ERR_BAD_ARGS;
		pc_place_t place;
		memset(&place, 0, sizeof(place));
		place.origin = to_pc(e->origin);
		place.clip = to_pc(e->clip);
		place.noverlays = e->noverlays;
		for (uint32_t k = 0; k < e->noverlays; k++)
			place.overlay[k] = to_pc(e->overlay[k]);
		if (!plane_place_valid(&place, wm_dim_max(), wm_dim_max(), GUI_PLANE_OVERLAY_PIXELS))
			return refuse(GUI_PLANE_REFUSED_OVERLAP, OS64_GUI_ERR_PLANE_OVERLAP);
	}
	return 0;
}

int64_t gui_window_publish_edits(int64_t handle, const os64_gui_rect_t *damage,
                                 const os64_gui_plane_edit_t *edits, uint32_t count)
{
	int64_t rc = edits_shape(edits, count);
	if (rc != 0)
		return rc;
	if (count == 0)
		return gui_window_publish(handle, damage);

	int64_t err;
	uint64_t flags = gui_lock(GUI_LOCK_PUBLISH);
	window_t *win = gui_handle_owned_locked(handle, &err);
	if (win == NULL) {
		gui_unlock(flags);
		return err;
	}
	pc_rect_t bounds = content_bounds(win);
	pc_rect_t local = damage ? pc_cut(to_pc(*damage), bounds) : bounds;

	// Phase 1: every rule, on the places as they would be. Nothing changes
	// unless all of them pass.
	pc_place_t next[GUI_WINDOW_PLANES];
	bool changed[GUI_WINDOW_PLANES] = {false};
	for (unsigned s = 0; s < GUI_WINDOW_PLANES; s++)
		next[s] = win->planes[s].place;
	for (uint32_t i = 0; i < count; i++) {
		gui_plane_t *p = plane_lookup(win, edits[i].plane, &err);
		if (p == NULL) {
			gui_unlock(flags);
			return err;
		}
		unsigned s = (unsigned)(p - win->planes);
		pc_place_t place = edit_place(&edits[i]);
		if (!pc_empty(place.clip)) {
			if (!pc_inside(place.clip, bounds)) {
				gui_unlock(flags);
				return refuse(GUI_PLANE_REFUSED_OVERLAP, OS64_GUI_ERR_PLANE_OVERLAP);
			}
			if (p->state == GUI_PLANE_CREATED || place.origin.w != (int32_t)p->pic_w ||
			    place.origin.h != (int32_t)p->pic_h) {
				gui_unlock(flags);
				return refuse(GUI_PLANE_REFUSED_PICTURE, OS64_GUI_ERR_PLANE_PICTURE);
			}
		}
		const pc_place_t *old = &p->place;
		if (place_same(old, &place))
			continue;
		bool covered;
		if (!pc_empty(old->clip) && !pc_empty(place.clip) && pc_same(old->clip, place.clip) &&
		    pc_same(old->origin, place.origin)) {
			// The overlay list alone: its new rectangles are what the
			// content must carry fresh.
			covered = true;
			for (uint32_t k = 0; k < place.noverlays; k++)
				covered = covered && pc_inside(place.overlay[k], local);
		} else {
			// Shown, hidden or moved: the old clip (as the content now
			// stands) and the new.
			covered = pc_inside(pc_cut(old->clip, bounds), local) && pc_inside(place.clip, local);
		}
		if (!covered) {
			gui_unlock(flags);
			return refuse(GUI_PLANE_REFUSED_DAMAGE, OS64_GUI_ERR_PLANE_DAMAGE);
		}
		next[s] = place;
		changed[s] = true;
	}
	for (unsigned a = 0; a < GUI_WINDOW_PLANES; a++) {
		for (unsigned b = a + 1; b < GUI_WINDOW_PLANES; b++) {
			if (pc_empty(next[a].clip) || pc_empty(next[b].clip))
				continue;
			if (!pc_empty(pc_cut(next[a].clip, next[b].clip))) {
				gui_unlock(flags);
				return refuse(GUI_PLANE_REFUSED_OVERLAP, OS64_GUI_ERR_PLANE_OVERLAP);
			}
		}
	}

	// Phase 2: the window's pixels. Phase 3: the places, with the old clip
	// and the new damaged.
	if (!pc_empty(local))
		gui_window_snapshot_locked(win, to_rect(local));
	for (unsigned s = 0; s < GUI_WINDOW_PLANES; s++) {
		if (!changed[s])
			continue;
		gui_plane_t *p = &win->planes[s];
		damage_content_locked(win, p->place.clip, false);
		p->place = next[s];
		p->state = pc_empty(p->place.clip) ? GUI_PLANE_PICTURED : GUI_PLANE_PLACED;
		p->place_changes++;
		damage_content_locked(win, p->place.clip, false);
	}
	gui_unlock(flags);
	return 0;
}

// ── Frames (VIDEO_PLANE.md § 3.6) ───────────────────────────────────────────

// The one test of whether a plane is shown: its window not covered, its
// place not empty, and its clip, as the content stands and cut to the
// screen, meeting what of the window is visible (wm_visible_damage's walk,
// which answers with the pieces' bounding box when it runs out of them, and
// knows only the windows above, not the screen's edge). Under kGuiLock.
static bool plane_shown_locked(window_t *win, const gui_plane_t *p)
{
	if (p->state != GUI_PLANE_PLACED || (win->flags & GUI_WINDOW_COVERED))
		return false;
	pc_rect_t clip = pc_cut(p->place.clip, content_bounds(win));
	if (pc_empty(clip))
		return false;
	rect_t cs = wm_content_rect_on_screen(win);
	pc_rect_t on_screen = pc_cut((pc_rect_t){cs.x + clip.x, cs.y + clip.y, clip.w, clip.h},
	                             (pc_rect_t){0, 0, (int32_t)kFrameBuffer.width, (int32_t)kFrameBuffer.height});
	if (pc_empty(on_screen))
		return false;
	rect_t seen[4];
	return wm_visible_damage(win, to_rect(on_screen), seen, 4) > 0;
}

void gui_planes_frame_waiters_locked(uint32_t *shown, uint32_t *hidden)
{
	uint32_t s = 0, h = 0;
	for (unsigned i = 0; i < GUI_MAX_WINDOWS; i++) {
		window_t *win = gui_handle_at_locked(i);
		if (win == NULL)
			continue;
		for (unsigned k = 0; k < GUI_WINDOW_PLANES; k++) {
			gui_plane_t *p = &win->planes[k];
			if (p->state == GUI_PLANE_FREE || !p->frame_wanted)
				continue;
			p->shown = plane_shown_locked(win, p);
			if (p->shown)
				s++;
			else
				h++;
		}
	}
	*shown = s;
	*hidden = h;
}

uint32_t gui_planes_frame_answer_locked(uint64_t number, uint64_t blank_us, uint32_t period_ns, uint64_t before,
                                        uint32_t *held_back)
{
	uint32_t answered = 0, held = 0;
	for (unsigned i = 0; i < GUI_MAX_WINDOWS; i++) {
		window_t *win = gui_handle_at_locked(i);
		if (win == NULL)
			continue;
		for (unsigned k = 0; k < GUI_WINDOW_PLANES; k++) {
			gui_plane_t *p = &win->planes[k];
			if (p->state == GUI_PLANE_FREE || !p->frame_wanted || p->frame_epoch >= before)
				continue;
			if (!p->shown) {
				held++;
				continue;
			}
			p->frame_wanted = false;
			p->answered = true;
			p->answer_number = number;
			p->answer_blank_us = blank_us;
			p->answer_period_ns = period_ns;
			p->frames_answered++;
			if (p->waiter != NULL)
				scheduler_wake_or_owe(p->waiter);
			answered++;
		}
	}
	*held_back = held;
	return answered;
}

// The park's backstop, as a window's event wait has: a wake missed costs at
// most this, never a hang.
#define PLANE_WAIT_BACKSTOP_TICKS (TICKS_PER_SECOND / 4)

// The wait's lifetime is P1's design state (§ 5 there): its request is
// made at entry; it parks as gui_event_wait does (register under the lock,
// void the registration at the top of every pass, the backstop); it ends at
// an answer, a poke, the patience's end or a caught signal, and every end
// clears the request, the answer and any poke it took. The plane is looked
// up again every pass, as a window is: it may be gone when the wait wakes.
// Patience 0 makes no request (it could never be answered): it takes a
// poke owed, or ends by its patience. A request starts a stopped frame
// clock only if the plane is shown when it is made; one hidden then is
// counted by the next pass that finds it shown.
int64_t gui_plane_frame_wait(int64_t handle, uint32_t name, uint32_t patience_ms, os64_gui_plane_frame_t *out)
{
	thread_t *self = current_thread();
	if (self == NULL || out == NULL)
		return GUI_ERR_BAD_ARGS;
	uint64_t deadline = patience_ms == OS64_GUI_PLANE_WAIT_FOREVER ? UINT64_MAX
	                                                               : micros_now() + (uint64_t)patience_ms * 1000;
	bool asked = false;
	for (;;) {
		int64_t err;
		window_t *win;
		uint64_t flags = gui_lock(GUI_LOCK_PLANE);
		scheduler_forget_owed_wake(self);
		gui_plane_t *p = creator_plane_locked(handle, name, &win, &err);
		if (p == NULL) {
			gui_unlock(flags);
			return err;
		}
		if (p->waiter == self)
			p->waiter = NULL;
		if (!asked && !p->poke_owed && patience_ms != 0) {
			p->frame_wanted = true;
			p->answered = false;
			p->frame_epoch = gui_frame_epoch_locked();
			p->frames_asked++;
			asked = true;
			if (plane_shown_locked(win, p))
				gui_frame_wait_arrived();   // a stopped clock starts for a shown waiter
		}
		uint32_t why = p->poke_owed ? OS64_GUI_PLANE_POKED
		             : p->answered ? OS64_GUI_PLANE_FRAME
		             : signal_park_must_end(self) ? 0
		             : micros_now() >= deadline ? OS64_GUI_PLANE_PATIENCE
		             : UINT32_MAX;
		if (why != UINT32_MAX) {
			*out = (os64_gui_plane_frame_t){
				.why = why,
				.period_ns = why == OS64_GUI_PLANE_FRAME ? p->answer_period_ns : 0,
				.number = why == OS64_GUI_PLANE_FRAME ? p->answer_number : 0,
				.blank_us = why == OS64_GUI_PLANE_FRAME ? p->answer_blank_us : 0,
				.placed = p->state == GUI_PLANE_PLACED,
				.place_changes = p->place_changes,
			};
			p->frame_wanted = false;
			p->answered = false;
			if (why == OS64_GUI_PLANE_POKED) {
				p->poke_owed = false;
				p->waits_poked++;
			} else if (why == OS64_GUI_PLANE_PATIENCE) {
				p->waits_patience++;
			} else if (why == 0) {
				p->waits_interrupted++;
			}
			gui_unlock(flags);
			return why == 0 ? GUI_ERR_INTERRUPTED : 0;
		}
		p->waiter = self;
		gui_unlock(flags);
		uint64_t now = micros_now(), ticks = PLANE_WAIT_BACKSTOP_TICKS;
		if (deadline != UINT64_MAX) {
			// The deadline may have passed since the check above.
			uint64_t left = now >= deadline ? 1 : (deadline - now) * TICKS_PER_SECOND / 1000000 + 1;
			ticks = left < ticks ? left : ticks;
		}
		signal_raise(SIGSLEEP, kTicksSinceStart + ticks, self);
	}
}

int64_t gui_plane_poke(int64_t handle, uint32_t name)
{
	int64_t err;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	window_t *win = gui_handle_owned_locked(handle, &err);
	gui_plane_t *p = win ? plane_lookup(win, name, &err) : NULL;
	if (p == NULL) {
		gui_unlock(flags);
		return err;
	}
	p->poke_owed = true;
	if (p->waiter != NULL)
		scheduler_wake_or_owe(p->waiter);
	gui_unlock(flags);
	return 0;
}

// ── The compositor's part ───────────────────────────────────────────────────

// One plane's part `area` (content coordinates, inside its cut clip) drawn
// into the view.
static void compose_part(window_t *w, gui_plane_t *p, const pc_place_t *cut, surface_t *view,
                         rect_t damage, rect_t cs, pc_rect_t area)
{
	if (pc_empty(area))
		return;
	pc_image_t dst = {
		view->pixels + (size_t)(area.y + cs.y - damage.y) * view->pitch_px + (area.x + cs.x - damage.x),
		view->pitch_px, (uint32_t)area.w, (uint32_t)area.h,
	};
	pc_image_t content = {w->content.pixels, w->content.pitch_px, w->content.width, w->content.height};
	pc_image_t picture = {p->front, p->cap_w, p->pic_w, p->pic_h};
	uint64_t t0 = rdtsc();
	plane_compose(&dst, &content, &picture, cut, area, &p->composed);
	p->compose_cycles += rdtsc() - t0;
}

// The content's part `band` (content coordinates) blitted as composite_one
// would.
static void blit_band(const window_t *w, surface_t *view, rect_t damage, rect_t cs, pc_rect_t band)
{
	if (pc_empty(band))
		return;
	surface_blit(view, cs.x - damage.x + band.x, cs.y - damage.y + band.y, &w->content, to_rect(band));
}

bool gui_planes_compose(window_t *w, surface_t *view, rect_t damage, rect_t cs)
{
	pc_rect_t bounds = content_bounds(w);
	pc_rect_t area = pc_cut((pc_rect_t){damage.x - cs.x, damage.y - cs.y, damage.w, damage.h}, bounds);
	if (pc_empty(area))
		return false;
	unsigned meets[GUI_WINDOW_PLANES], n = 0;
	pc_place_t cut[GUI_WINDOW_PLANES];
	for (unsigned s = 0; s < GUI_WINDOW_PLANES; s++) {
		const gui_plane_t *p = &w->planes[s];
		if (p->state != GUI_PLANE_PLACED)
			continue;
		// The clip as the content stands: a window made smaller after the
		// commit never has either store read past.
		cut[n] = p->place;
		cut[n].clip = pc_cut(p->place.clip, bounds);
		if (pc_empty(pc_cut(cut[n].clip, area)))
			continue;
		meets[n++] = s;
	}
	if (n == 0)
		return false;
	if (n == 1) {
		// The content around the one clip, in up to four bands, then the
		// clip's part: no pixel drawn twice.
		pc_rect_t c = pc_cut(cut[0].clip, area);
		blit_band(w, view, damage, cs, (pc_rect_t){area.x, area.y, area.w, c.y - area.y});
		blit_band(w, view, damage, cs,
		          (pc_rect_t){area.x, c.y + c.h, area.w, area.y + area.h - (c.y + c.h)});
		blit_band(w, view, damage, cs, (pc_rect_t){area.x, c.y, c.x - area.x, c.h});
		blit_band(w, view, damage, cs, (pc_rect_t){c.x + c.w, c.y, area.x + area.w - (c.x + c.w), c.h});
		compose_part(w, &w->planes[meets[0]], &cut[0], view, damage, cs, c);
		return true;
	}
	blit_band(w, view, damage, cs, area);
	for (unsigned i = 0; i < n; i++)
		compose_part(w, &w->planes[meets[i]], &cut[i], view, damage, cs, pc_cut(cut[i].clip, area));
	return true;
}

// ── /sys/gui ────────────────────────────────────────────────────────────────

uint32_t gui_planes_status(gui_plane_status_t *out, uint32_t cap, uint64_t refused[GUI_PLANE_REFUSALS],
                           uint64_t *pairs, uint64_t *pairs_kept, uint64_t *pair_bytes_out)
{
	uint32_t n = 0;
	uint64_t flags = gui_lock(GUI_LOCK_PLANE);
	for (unsigned i = 0; i < GUI_MAX_WINDOWS; i++) {
		window_t *win = gui_handle_at_locked(i);
		if (win == NULL)
			continue;
		for (unsigned s = 0; s < GUI_WINDOW_PLANES; s++) {
			const gui_plane_t *p = &win->planes[s];
			if (p->state == GUI_PLANE_FREE)
				continue;
			if (n < cap) {
				gui_plane_status_t *r = &out[n];
				r->window = i + 1;
				r->name = plane_name(s, p->generation);
				r->state = p->state;
				r->cap_w = p->cap_w;
				r->cap_h = p->cap_h;
				r->pic_w = p->pic_w;
				r->pic_h = p->pic_h;
				r->origin = p->place.origin;
				r->clip = p->place.clip;
				r->noverlays = p->place.noverlays;
				r->overlay_area = plane_overlay_area(&p->place);
				r->publishes = p->publishes;
				r->composed = p->composed;
				r->shown = plane_shown_locked(win, p);
				r->waiting = p->waiter != NULL;
				r->frames_asked = p->frames_asked;
				r->frames_answered = p->frames_answered;
				r->waits_poked = p->waits_poked;
				r->waits_patience = p->waits_patience;
				r->waits_interrupted = p->waits_interrupted;
				r->compose_ns = kCPUCyclesPerSecond
				    ? p->compose_cycles / kCPUCyclesPerSecond * 1000000000ull +
				      p->compose_cycles % kCPUCyclesPerSecond * 1000000000ull / kCPUCyclesPerSecond
				    : 0;
			}
			n++;
		}
	}
	*pairs = s_pairs;
	*pairs_kept = s_pairs_kept;
	*pair_bytes_out = s_pair_bytes;
	gui_unlock(flags);
	for (unsigned i = 0; i < GUI_PLANE_REFUSALS; i++)
		refused[i] = __atomic_load_n(&s_refused[i], __ATOMIC_RELAXED);
	return n;
}
