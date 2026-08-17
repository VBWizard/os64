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

// Damage accumulator, v1: one union rect. Worst case this over-redraws the
// bounding box of two small distant rects; upgrading to a small rect LIST is
// a drop-in change confined to gui_damage_add + the frame loop.
static rect_t kPendingDamage;   // empty (w==0) means "no damage"

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
	kPendingDamage = rect_union(kPendingDamage, screen_rect);
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

	// Cursor last. Drawing the FULL cursor (not clipped to damage) is safe:
	// any backbuffer pixels outside the damage already showed the cursor on
	// screen, so backbuffer and framebuffer stay in agreement.
	rect_t overlap;
	if (rect_intersect(cursor_rect(), damage, &overlap))
		surface_blit_masked(&kBackbuffer, s_cursor_x, s_cursor_y,
		                    s_cursor_pixels, s_cursor_mask, CURSOR_W, CURSOR_H);

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

		rect_t damage = kPendingDamage;
		kPendingDamage = (rect_t){0, 0, 0, 0};
		if (!rect_is_empty(damage))
			damage = composite_locked(damage);

		spinlock_release_irqrestore(&kGuiLock, irqflags);

		// -------- Flush to the (slow, uncached) framebuffer, lock-free -----
		if (!rect_is_empty(damage)) {
			surface_flush_rect(&kBackbuffer, damage);
			flushes++;
			printd(DEBUG_GUI | DEBUG_DETAILED,
				"guicomp: composited %dx%d at (%d,%d), tick %lu\n",
				damage.w, damage.h, damage.x, damage.y, kTicksSinceStart);
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
		// (kPendingDamage is read unlocked here as a wake HINT only; a torn
		// read at worst wakes us early or costs one timer period.)
		__asm__ volatile("cli" ::: "memory");
		if (!input_pending() && rect_is_empty(kPendingDamage))
			__asm__ volatile("sti\n\thlt" ::: "memory");
		else
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

	// Demo apps. Unpinned — they're ordinary clients and can run anywhere;
	// their window calls synchronize through kGuiLock like anyone else's.
	task_t *demo;
	demo = task_create("/gbounce", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
	demo->threads->regs.RDI = 1;
	scheduler_submit_new_task(demo);

	demo = task_create("/gkeys", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
	demo->threads->regs.RDI = 1;
	scheduler_submit_new_task(demo);
}
