// compositor.c — the GUI compositor daemon and subsystem entry point.
//
// The pipeline (per frame): drain input → route events → recomposite damaged
// screen regions into the RAM backbuffer (scene layers bottom-up, cursor
// last) → flush only the damaged rects to the uncached hardware framebuffer.
// The backbuffer is the canonical screen image; the framebuffer only ever
// receives finished pixels, which is why nothing can flicker.
//
// Scene layers: the desktop surface (bottom), the windows in z-order, then
// the mouse cursor. The cursor needs no save-under trickery — recompositing
// the damaged region rebuilds whatever it covered.

#include "gui/compositor.h"
#include "gui/surface.h"
#include "gui/input.h"
#include "gui/window.h"
#include "gui/gui_client.h"
#include "gui/gui_internal.h"
#include "gui/console_window.h"

#include "BasicRenderer.h"   // kConsoleSink, for gui_emergency_disable

#include "CONFIG.h"
#include "kernel.h"
#include "printd.h"
#include "smp.h"
#include "smp_core.h"
#include "scheduler.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"
#include "video.h"

// kTicksSinceStart comes from kernel.h; kKernelTask is per-file extern by convention
extern task_t *kKernelTask;
extern struct Framebuffer kFrameBuffer;

bool kEnableGUI = false;

extern bool kTicklessScheduler;

// The compositor task, created by gui_start(). Kept for future use
// (diagnostics, wake-on-damage once an IRQ-safe wake primitive exists).
task_t *kGuiCompTask = NULL;

// ---------------------------------------------------------------------------
// Shared GUI state. Everything guarded by kGuiLock is touched by client
// threads (gui_window_* calls) and the compositor. Compositing into the RAM
// backbuffer happens UNDER the lock (sub-ms; keeps window lifetimes safe);
// only the flush to the slow uncached framebuffer runs after release.
// ---------------------------------------------------------------------------
spinlock_t kGuiLock = 0;   // shared with window.c / gui_client.c via gui_internal.h

// ---------------------------------------------------------------------------
// Damage accumulator, v2: a small rect LIST (2026-08-18).
//
// v1 kept ONE union rect, and the P5 shakedown priced that choice: a bouncing
// ball in one corner unioned with a cursor in the other produced a near-
// fullscreen rectangle, recomposited and pushed through the uncached
// framebuffer ~100 times a second. The screen was mostly unchanged; we paid
// retail for it anyway. Two small rects should cost two small rects.
//
// The design is deliberately the cheap one — a fixed array plus a merge
// heuristic, NOT region algebra. Regions (X11's, Cairo's) split rectangles on
// overlap to keep an exact non-overlapping cover, and they are the right
// answer when you have thousands; here the whole population is "one ball, one
// cursor, one console line, sometimes a dragged window", and an exact cover
// would cost more to maintain than the overdraw it saves.
//
// Two rules keep it honest:
//   1. MERGE WHEN MERGING IS CHEAP. Union two entries only if the union wastes
//      no more than DAMAGE_MERGE_SLACK pixels beyond their own areas — so
//      touching/overlapping rects collapse (and overlapping ones MUST, or the
//      overlap composites twice) while distant ones stay apart. Without this,
//      a list degenerates into a list of near-duplicates.
//   2. OVERFLOW FALLS BACK TO v1. When the list is full and nothing merges
//      cheaply, collapse everything into one union rect. Worst case is exactly
//      today's behavior — never a wrong screen, only a slower one.
//
// Entries may still overlap (rule 1 tolerates SLACK, and merging is
// pairwise-greedy with NO re-merge pass: a rect that bridges into entry i can
// grow it until it swallows entry j whole, and nothing goes back to notice).
// So the overlap is not strictly slack-bounded — the swallowed-entry case
// composites and UC-flushes a duplicate of j's whole area. It is WASTE, never
// wrongness: both passes composite identical bytes in identical z-order, so
// the glass cannot disagree with itself. The geometry that triggers it (a
// late rect bridging two established ones) is rare at this population size;
// if it ever shows in the flush counter, the fix is a containment sweep after
// a merge grows an entry, not a redesign. (Honesty upgrade from review,
// 2026-08-19 — the first version of this comment claimed the slack bound.)
// ---------------------------------------------------------------------------
#define DAMAGE_MAX_RECTS   8
// One 8x16 glyph cell (128 px) of tolerated waste per merge: generous enough
// that a scrolling console line's per-glyph damage collapses into one row
// rect, tight enough that opposite corners of a 1024x768 screen never do.
#define DAMAGE_MERGE_SLACK 128

static rect_t   kPendingDamage[DAMAGE_MAX_RECTS];
static uint32_t kPendingDamageCount;   // 0 means "no damage"

static inline int64_t rect_area(rect_t r)
{
	return rect_is_empty(r) ? 0 : (int64_t)r.w * (int64_t)r.h;
}

// Would unioning these two waste little enough to be worth one composite?
// Overlapping rects always qualify: their union is never bigger than the sum
// of their areas, so the test below passes by construction — which is what
// guarantees the list never composites the same pixel twice for free.
static inline bool damage_merge_is_cheap(rect_t a, rect_t b)
{
	return rect_area(rect_union(a, b)) <= rect_area(a) + rect_area(b) + DAMAGE_MERGE_SLACK;
}

// The canonical screen image (scene + cursor). Compositor-thread-only.
static surface_t kBackbuffer;

// The desktop: bottom layer of the scene, painted once at startup. Windows
// (M5) stack on top of it, the cursor on top of everything.
static surface_t kDesktop;

// ---------------------------------------------------------------------------
// Frame pacing: hlt-wait, always.
//
// The compositor never sleeps through the scheduler and never busy-waits:
// it HALTS its core until something interrupt-shaped happens. Because
// kernel_init routes the input IRQs (1, 12) at this core, a mouse packet or
// keystroke ends the hlt directly — input-to-screen latency is one
// interrupt — while an idle desktop costs ~10 trivial wakeups/sec (the
// core's scheduler timer) and effectively zero CPU, guest or host.
//
// History, so nobody resurrects the alternatives: SIGSLEEP pacing had
// 100-500ms wake latency (wakes ride the BSP's signal pass), and the
// "hot spin to the next tick" workaround ate one full core — a full HOST
// core under VBox/QEMU — whenever the mouse moved.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The mouse cursor. Classic arrow, built from readable string art at startup:
// 'X' = black outline, 'o' = white fill, ' ' = transparent (mask 0).
// ---------------------------------------------------------------------------
#define CURSOR_W 12
#define CURSOR_H 18

static const char *const kCursorArt[CURSOR_H] = {
	"X           ",
	"XX          ",
	"XoX         ",
	"XooX        ",
	"XoooX       ",
	"XooooX      ",
	"XoooooX     ",
	"XooooooX    ",
	"XoooooooX   ",
	"XooooooooX  ",
	"XoooooooooX ",
	"XooooooXXXX ",
	"XoooXooX    ",
	"XooX XooX   ",
	"XoX  XooX   ",
	"XX    XooX  ",
	"X     XooX  ",
	"       XX   ",
};

static uint32_t s_cursor_pixels[CURSOR_W * CURSOR_H];
static uint8_t  s_cursor_mask[CURSOR_W * CURSOR_H];

// Cursor position (top-left of the art; the hotspot is the arrow tip at 0,0).
// Written only by the compositor thread while handling MOUSE_MOVE events.
static int32_t s_cursor_x, s_cursor_y;

static void cursor_build(void)
{
	for (int y = 0; y < CURSOR_H; y++) {
		for (int x = 0; x < CURSOR_W; x++) {
			char c = kCursorArt[y][x];
			s_cursor_pixels[y * CURSOR_W + x] =
				(c == 'o') ? GUI_COLOR_WHITE : GUI_COLOR_BLACK;
			s_cursor_mask[y * CURSOR_W + x] = (c != ' ');
		}
	}
}

static inline rect_t cursor_rect(void)
{
	return (rect_t){s_cursor_x, s_cursor_y, CURSOR_W, CURSOR_H};
}

void gui_damage_add_locked(rect_t screen_rect)
{
	if (rect_is_empty(screen_rect))
		return;

	// Merge into the first entry where merging is cheap. First-fit, not
	// best-fit: the list is at most DAMAGE_MAX_RECTS long and the common case
	// is one or two entries, so hunting for the optimal partner would cost
	// more than the overdraw it saves.
	for (uint32_t i = 0; i < kPendingDamageCount; i++) {
		if (damage_merge_is_cheap(kPendingDamage[i], screen_rect)) {
			kPendingDamage[i] = rect_union(kPendingDamage[i], screen_rect);
			return;
		}
	}

	if (kPendingDamageCount < DAMAGE_MAX_RECTS) {
		kPendingDamage[kPendingDamageCount++] = screen_rect;
		return;
	}

	// Full, and nothing merged cheaply — fall back to v1: one union rect for
	// the whole frame. Over-redraws, never under-redraws.
	rect_t all = screen_rect;
	for (uint32_t i = 0; i < kPendingDamageCount; i++)
		all = rect_union(all, kPendingDamage[i]);
	kPendingDamage[0] = all;
	kPendingDamageCount = 1;
}

void gui_damage_add(rect_t screen_rect)
{
	uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
	gui_damage_add_locked(screen_rect);
	spinlock_release_irqrestore(&kGuiLock, flags);
}

void gui_emergency_disable(void)
{
	// One store, no locks: panic() calls this first so its output takes the
	// direct-to-framebuffer path even if the GUI is mid-composite (or the
	// GUI is what died). The desktop gets scribbled over — intentionally.
	kConsoleSink = NULL;
}

// ---------------------------------------------------------------------------
// Desktop scene: background + primitive sampler, so every drawing primitive
// stays exercised on real hardware. Painted into kDesktop once.
// ---------------------------------------------------------------------------
static void paint_desktop(void)
{
	int32_t w = (int32_t)kDesktop.width;
	int32_t h = (int32_t)kDesktop.height;

	surface_fill_rect(&kDesktop, (rect_t){0, 0, w, h}, GUI_COLOR_DESKTOP);

	// Deliberately overlapping and partially off-screen rects prove clipping.
	surface_fill_rect(&kDesktop, (rect_t){40, 60, 220, 140}, GUI_COLOR_BLUE);
	surface_fill_rect(&kDesktop, (rect_t){120, 130, 220, 140}, GUI_COLOR_GREEN);
	surface_fill_rect(&kDesktop, (rect_t){-60, h - 100, 200, 200}, GUI_COLOR_RED);    // clips left+bottom
	surface_fill_rect(&kDesktop, (rect_t){w - 120, -40, 200, 120}, GUI_COLOR_YELLOW); // clips right+top
	surface_draw_rect(&kDesktop, (rect_t){36, 56, 310, 220}, GUI_COLOR_WHITE);

	surface_draw_hline(&kDesktop, 40, h / 2, w - 80, GUI_COLOR_LIGHT_GRAY);
	surface_draw_vline(&kDesktop, w / 2, 40, h - 80, GUI_COLOR_LIGHT_GRAY);

	const char banner[] = "os64 GUI: surface core online";
	surface_draw_text(&kDesktop, (w - (int32_t)(sizeof(banner) - 1) * 8) / 2, 24,
	                  banner, sizeof(banner) - 1, GUI_COLOR_WHITE, GUI_COLOR_DESKTOP);

	gui_damage_add((rect_t){0, 0, w, h});
}

// ---------------------------------------------------------------------------
// Recomposite `damage` (screen coords) into the backbuffer: scene layers
// bottom-up (desktop, windows), cursor last. Caller holds kGuiLock; the
// caller flushes AFTER releasing it. This is the only writer of the
// backbuffer.
// ---------------------------------------------------------------------------
static rect_t composite_locked(rect_t damage)
{
	rect_t screen = {0, 0, (int32_t)kBackbuffer.width, (int32_t)kBackbuffer.height};
	if (!rect_intersect(damage, screen, &damage))
		return (rect_t){0, 0, 0, 0};

	// Layer 0: desktop (same coordinates both sides — straight copy).
	surface_blit(&kBackbuffer, damage.x, damage.y, &kDesktop, damage);

	// Layer 1: windows, bottom-up by z-order.
	wm_composite(&kBackbuffer, damage);

	// Cursor last, and clipped to this rect like everything else. The old code
	// drew the FULL cursor art on the argument that stray pixels were harmless
	// (the cursor is topmost, so anywhere it paints is somewhere it really is).
	// That argument still holds — but "writes stay inside the damage rect" is
	// now a contract the whole multi-rect frame depends on, and a layer that
	// keeps its own private exemption is how the next person gets bitten.
	rect_t overlap;
	if (rect_intersect(cursor_rect(), damage, &overlap)) {
		surface_t view = surface_view(&kBackbuffer, damage);
		surface_blit_masked(&view, s_cursor_x - damage.x, s_cursor_y - damage.y,
		                    s_cursor_pixels, s_cursor_mask, CURSOR_W, CURSOR_H);
	}

	return damage;
}

// ---------------------------------------------------------------------------
// Event routing (kGuiLock held): hit-test clicks to windows, drive
// click-to-focus/raise and titlebar dragging, and translate whatever belongs
// to a window into content-local coordinates on its queue.
// ---------------------------------------------------------------------------
static window_t *s_drag_window = NULL;
static int32_t s_drag_dx, s_drag_dy;   // grab offset: cursor minus frame origin

// Deliver a mouse event to the window IF the point is inside its content
// area (chrome clicks are the window system's business, not the client's).
static void deliver_mouse_to_window(window_t *w, input_event_t ev)
{
	rect_t content = wm_content_rect_on_screen(w);
	if (!rect_contains_point(content, ev.mouse.x, ev.mouse.y))
		return;
	ev.mouse.x -= content.x;
	ev.mouse.y -= content.y;
	wm_deliver_event(w, &ev);
}

static void route_event_locked(const input_event_t *ev)
{
	switch (ev->type) {
	case INPUT_EVENT_MOUSE_MOVE: {
		// Damage where the cursor WAS and where it lands; the recomposite
		// repaints the scene under the old position.
		rect_t moved = cursor_rect();
		s_cursor_x = ev->mouse.x;
		s_cursor_y = ev->mouse.y;
		moved = rect_union(moved, cursor_rect());
		gui_damage_add_locked(moved);

		if (s_drag_window)
			wm_move(s_drag_window,
			        ev->mouse.x - s_drag_dx, ev->mouse.y - s_drag_dy);
		else {
			window_t *under = wm_topmost_at(ev->mouse.x, ev->mouse.y);
			if (under)
				deliver_mouse_to_window(under, *ev);
		}
		break;
	}
	case INPUT_EVENT_MOUSE_BUTTON_DOWN: {
		window_t *w = wm_topmost_at(ev->mouse.x, ev->mouse.y);
		if (!w)
			break;   // desktop click: nothing to do (yet)
		wm_raise(w);
		if (ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT &&
		    wm_point_in_titlebar(w, ev->mouse.x, ev->mouse.y)) {
			// Grab for dragging; remember where in the frame we grabbed so
			// the window doesn't jump under the cursor.
			s_drag_window = w;
			s_drag_dx = ev->mouse.x - w->frame.x;
			s_drag_dy = ev->mouse.y - w->frame.y;
		} else {
			deliver_mouse_to_window(w, *ev);
		}
		break;
	}
	case INPUT_EVENT_MOUSE_BUTTON_UP: {
		if (s_drag_window && ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT) {
			s_drag_window = NULL;
			break;
		}
		window_t *under = wm_topmost_at(ev->mouse.x, ev->mouse.y);
		if (under)
			deliver_mouse_to_window(under, *ev);
		break;
	}
	case INPUT_EVENT_KEY_DOWN:
	case INPUT_EVENT_KEY_UP: {
		window_t *focus = wm_focused();
		if (focus)
			wm_deliver_event(focus, ev);
		break;
	}
	}
}

bool guicomp_thread(bool daemon)
{
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	printd(DEBUG_GUI, "guicomp: compositor starting on APIC %u (thread=0x%08x, daemon=%u)\n",
		cls->apic_id, self->threadID, daemon);

	if (surface_init(&kBackbuffer, kFrameBuffer.width, kFrameBuffer.height) != 0 ||
	    surface_init(&kDesktop, kFrameBuffer.width, kFrameBuffer.height) != 0) {
		printd(DEBUG_GUI, "guicomp: FATAL: surface allocation failed, compositor exiting\n");
		return false;
	}
	printd(DEBUG_GUI, "guicomp: backbuffer %ux%u ready (%lu KB x2)\n",
		kBackbuffer.width, kBackbuffer.height,
		(uint64_t)kBackbuffer.width * kBackbuffer.height * 4 / 1024);

	cursor_build();
	s_cursor_x = (int32_t)kBackbuffer.width / 2;
	s_cursor_y = (int32_t)kBackbuffer.height / 2;
	gui_damage_add(cursor_rect());

	paint_desktop();

	// A first window, created through the CLIENT API — so M5 exercises the
	// exact path demo apps (and later, userland) will use: click it to
	// focus, drag it by the titlebar.
	int64_t hello = gui_window_create("hello os64", 340, 250, 340, 240, 0);
	if (hello > 0) {
		surface_t s;
		gui_window_get_surface(hello, &s);
		const char msg1[] = "The os64 window system lives!";
		const char msg2[] = "Drag me by the title bar.";
		surface_draw_text(&s, 12, 16, msg1, sizeof(msg1) - 1,
		                  GUI_COLOR_BLACK, GUI_COLOR_LIGHT_GRAY);
		surface_draw_text(&s, 12, 40, msg2, sizeof(msg2) - 1,
		                  GUI_COLOR_DARK_GRAY, GUI_COLOR_LIGHT_GRAY);
		gui_window_publish(hello, NULL);
	}

	// The console window: from here on, printf/print_n output lands in the
	// desktop instead of scribbling on the framebuffer under our windows.
	gui_console_start();

	uint64_t frames = 0, flushes = 0;
	uint64_t last_heartbeat_tick = kTicksSinceStart;

	while (1) {
		frames++;

		// -------- Console grid → pixels (own lock; must NOT hold kGuiLock) --
		gui_console_render_if_dirty();

		// -------- Drain input, route events, recomposite (one lock hold) ---
		uint64_t irqflags = spinlock_acquire_irqsave(&kGuiLock);

		input_event_t ev;
		while (input_pop(&ev))
			route_event_locked(&ev);

		// Take the whole damage list in one hold and reset it, so clients
		// publishing during this frame's flush accumulate into the NEXT
		// frame instead of racing the one being drawn.
		rect_t damage[DAMAGE_MAX_RECTS];
		uint32_t damage_count = kPendingDamageCount;
		for (uint32_t i = 0; i < damage_count; i++)
			damage[i] = composite_locked(kPendingDamage[i]);
		kPendingDamageCount = 0;

		spinlock_release_irqrestore(&kGuiLock, irqflags);

		// -------- Flush to the (slow, uncached) framebuffer, lock-free -----
		// One flush per surviving rect. composite_locked clipped each to the
		// screen and may have emptied it entirely; those are skipped here
		// rather than filtered above, so the indices keep matching.
		for (uint32_t i = 0; i < damage_count; i++) {
			if (rect_is_empty(damage[i]))
				continue;
			surface_flush_rect(&kBackbuffer, damage[i]);
			flushes++;
			printd(DEBUG_GUI | DEBUG_DETAILED,
				"guicomp: composited %dx%d at (%d,%d) [rect %u/%u], tick %lu\n",
				damage[i].w, damage[i].h, damage[i].x, damage[i].y,
				i + 1, damage_count, kTicksSinceStart);
		}

		if (kTicksSinceStart - last_heartbeat_tick >= TICKS_PER_SECOND) {
			printd(DEBUG_GUI, "guicomp: heartbeat, %lu frames, %lu flushes (tick %lu)\n",
				frames, flushes, kTicksSinceStart);
			last_heartbeat_tick = kTicksSinceStart;
		}

		if (!daemon)
			return true;

		// -------- Pace the next frame (see block comment above) --------
		// If there's nothing to do, HALT until the next interrupt: an input
		// IRQ (routed to this core) wakes us with work in hand; otherwise
		// the core's scheduler timer bounds the nap. At most one hlt per
		// pass, then fall through — the pass costs almost nothing when idle,
		// and falling through keeps the heartbeat/daemon checks alive.
		//
		// The cli / check / sti;hlt shape is the standard lost-wakeup-free
		// idle sequence: with IF clear an IRQ can't slip in between the
		// check and the hlt, and sti's one-instruction interrupt shadow
		// means a pending IRQ is delivered exactly AT the hlt — waking it —
		// never before it.
		//
		// (kPendingDamageCount is read unlocked here as a wake HINT only; a
		// torn read at worst wakes us early or costs one timer period. Reading
		// the COUNT rather than a rect is what makes that claim cheap: it is a
		// single aligned word, so "0 or not 0" is the only question a racing
		// reader can get wrong, and it can only get it wrong for one pass.)
		// The accounting bookends around the nap (smp_core.h): without
		// them, hlt time bills as run time — this thread is the one idler
		// the scheduler cannot see, and top spent a day showing it at 95%
		// of a core while the flush counter sat frozen. Begin settles the
		// frame's real work onto us and routes the nap to the idle thread;
		// End settles the nap and takes the meter back.
		__asm__ volatile("cli" ::: "memory");
		if (!input_pending() && kPendingDamageCount == 0) {
			mpAcctHaltBegin();
			__asm__ volatile("sti\n\thlt" ::: "memory");
			mpAcctHaltEnd();
		} else
			__asm__ volatile("sti" ::: "memory");
	}
}

// Pin the compositor to core 1 when we have one (kworker precedent): keeps
// steady frame work off the BSP, which handles IRQ0/signals. EXCEPT in
// tickless mode (the default): there the AP scheduler timers stay masked
// (smp_core.c enableAPScheduling_ISR) — a thread pinned to an AP runs
// un-preemptable and only when nudged, which wedged the compositor hard.
// Under tickless everything stays on the BSP's normal scheduling, which is
// why the GUI boot entries run SCHED=periodic until the damage-wake nudge
// (SCHEDULER.md) lets a pinned compositor and parked cores coexist.
uint64_t gui_compositor_affinity(void)
{
	return (kMPCoreCount > 1 && !kTicklessScheduler)
	           ? (uint64_t)kCPUInfo[1].apicID
	           : THREAD_NO_AFFINITY;
}

void gui_start(void)
{
	// Open the unified input queue before the compositor (its consumer)
	// exists — events buffered during task startup are simply the first
	// ones drained.
	input_init();

	uint64_t affinity = gui_compositor_affinity();

	kGuiCompTask = task_create("/guicomp", 0, NULL, kKernelTask, true, affinity);
	// Pass daemon=true (first arg in RDI) to guicomp_thread
	kGuiCompTask->threads->regs.RDI = 1;
	scheduler_submit_new_task(kGuiCompTask);

	printd(DEBUG_GUI, "gui_start: compositor task 0x%08x submitted (affinity %s0x%08lx)\n",
		kGuiCompTask->taskID,
		affinity == THREAD_NO_AFFINITY ? "THREAD_NO_AFFINITY/" : "APIC ",
		affinity);

	// Demo apps — RING-3 ELFs since migration step 4 (2026-08-17). The
	// kernel-thread originals (gui/demo/) were GRAPHICS.md's placeholder
	// clients, and porting them to userland on libdraw was the stated
	// acceptance test for the whole boundary; the ports pass, so the boot
	// spawns the real thing now. What that buys, in kill(1) terms: these
	// are ordinary tasks — heap, /proc row, the syscall-boundary terminate
	// checkpoint ~100x a second, and an exit sweep for their windows — so
	// the experiment that discovered a kernel-thread ball cannot be shot
	// (Chris, 2026-08-17) now ends with a dead ball and a reclaimed window.
	// autoReap because ktask never waits (the husk-launch decree, task.c).
	// A launch failure is a report, not a boot failure: a disk image
	// without the demos is a valid image. The kernel-thread demo code
	// stays in the tree as the reference the ports were checked against;
	// nothing spawns it anymore.
	// (Non-const strings because task_create's path parameter predates
	// const-correctness — it does not modify them.)
	static char demo_bounce[] = "/bin/gbounce";
	static char demo_keys[]   = "/bin/gkeys";
	char *demos[] = { demo_bounce, demo_keys };
	for (unsigned i = 0; i < sizeof(demos) / sizeof(demos[0]); i++) {
		task_t *demo = task_create(demos[i], 0, NULL, kKernelTask, false,
		                           THREAD_NO_AFFINITY);
		if (demo == NULL) {
			// BOTH sinks, deliberately: printf is FRAMEBUFFER-ONLY (the
			// panic-pipeline scar), so a glass-only complaint vanishes the
			// moment the desktop paints over it — which on the P5 cost a
			// reboot and a log search that found nothing (2026-08-17, the
			// first GUI boot against a root that predated the ring-3
			// demos). The wire copy is unconditional: a missing binary at
			// boot is exactly the fact a log exists to keep.
			printf("gui_start: %s launch failed (not on the image?)\n", demos[i]);
			printd(DEBUG_BOOT, "gui_start: %s launch failed (not on the image?)\n", demos[i]);
			continue;
		}
		demo->autoReap = true;
		scheduler_submit_new_task(demo);
	}
}
