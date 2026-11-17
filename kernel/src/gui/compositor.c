// compositor.c — the GUI compositor daemon and subsystem entry point.
//
// The pipeline (per frame): drain input → route events → recomposite damaged
// screen regions into the RAM backbuffer (the scene, cursor last) → flush
// only the damaged rects to the write-combining framebuffer. The backbuffer
// is the canonical screen image; the framebuffer only ever receives finished
// pixels, which is why nothing can flicker.
//
// The scene is the windows in z-order over the desktop surface, and each
// damaged pixel is written by the one of them that can be seen there
// (OCCLUSION.md); the band, the switcher and the mouse cursor go over it.
// The cursor needs no save-under trickery — recompositing the damaged
// region rebuilds whatever it covered.

#include "gui/compositor.h"
#include "gui/surface.h"
#include "gui/input.h"
#include "gui/window.h"
#include "gui/gui_client.h"
#include "gui/gui_internal.h"
#include "gui/glass.h"         // glass_damage_locked — every frame feeds /dev/glass
#include "os64/damage.h"       // the damage list's rule, shared with libos64
#include "driver/system/x86_64.h"   // rdtsc — the lock times itself for /sys/gui
#include "micros.h"          // micros_now — a publish's wait to reach the glass
#include "memory/memcmp.h"   // memcmp — /sys/gui's dial lines
#include "strings/sprintf.h" // snprintf — the stall tripwire's report
#include "driver/display/beam.h"   // where the display's sweep is, for the frame clock's verdict

#include "tty.h"             // kTTY/kTTYFocused — glass ownership IS VT focus (VT8 chapter)
#include "vt_select.h"       // the other side of the input fork: console mouse selection
#include "driver/system/keyboard.h"   // KEYBOARD_MOD_* — the Ctrl+Alt gestures

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
extern bool kNoFrameClock;                            // NOFRAMECLOCK: the clock is off this boot
extern volatile uint64_t kFrameWakeFires[];           // scheduler.c: frame wakes per CPU
extern volatile uint64_t kSchedBackstopFires[];       // scheduler.c: lease expiries per CPU
extern volatile uint64_t kDiagPassCount[];            // scheduler.c: passes per CPU

// The compositor task, created by gui_start(). Kept for diagnostics (the
// frame clock's wake on damage is gui_frame_nudge, which needs only the CPU).
task_t *kGuiCompTask = NULL;

// ---------------------------------------------------------------------------
// Shared GUI state. Everything guarded by kGuiLock is touched by client
// threads (gui_window_* calls) and the compositor. Compositing into the RAM
// backbuffer happens UNDER the lock (keeps window lifetimes safe; /sys/gui's
// composite line says how long it holds it);
// only the flush to the slower write-combining framebuffer runs after release.
// ---------------------------------------------------------------------------
spinlock_t kGuiLock = 0;   // shared with window.c / gui_client.c via gui_internal.h

// gui_lock's counts (gui_internal.h): one row a site, and who holds the lock
// now and since when, so gui_unlock charges the hold to the site that took it.
static gui_lock_count_t s_lock_counts[GUI_LOCK_SITES];
static gui_composite_count_t s_composite_counts;
static gui_lock_site_t s_lock_holder;
static uint64_t s_lock_since;

static const char *const kLockSiteNames[GUI_LOCK_SITES] = {
	"publish", "composite", "damage", "ring", "wait", "poll", "window", "glass", "other",
};

const char *gui_lock_site_name(gui_lock_site_t site)
{
	return site < GUI_LOCK_SITES ? kLockSiteNames[site] : "?";
}

uint64_t gui_lock(gui_lock_site_t site)
{
	uint64_t waited;
	uint64_t flags = spinlock_acquire_irqsave_measured(&kGuiLock, rdtsc, &waited);
	gui_lock_count_t *c = &s_lock_counts[site < GUI_LOCK_SITES ? site : GUI_LOCK_OTHER];
	c->acquires++;
	if (waited != 0) {
		c->contended++;
		c->wait_cycles += waited;
		if (waited > c->max_wait_cycles)
			c->max_wait_cycles = waited;
	}
	s_lock_holder = site < GUI_LOCK_SITES ? site : GUI_LOCK_OTHER;
	s_lock_since = rdtsc();
	return flags;
}

void gui_unlock(uint64_t irqflags)
{
	uint64_t now = rdtsc();
	uint64_t hold = now > s_lock_since ? now - s_lock_since : 0;
	gui_lock_count_t *c = &s_lock_counts[s_lock_holder];
	c->hold_cycles += hold;
	if (hold > c->max_hold_cycles)
		c->max_hold_cycles = hold;
	if (s_lock_holder == GUI_LOCK_COMPOSITE) {
		uint64_t per_us = kCPUCyclesPerSecond / 1000000;
		uint64_t us = per_us != 0 ? hold / per_us : 0;
		int k = 0;
		while (k < GUI_COMPOSITE_HOLD_BUCKETS - 1 && us >= (250ull << k))
			k++;
		s_composite_counts.holds[k]++;
	}
	spinlock_release_irqrestore(&kGuiLock, irqflags);
}

void gui_lock_counts(gui_lock_count_t *out, gui_composite_count_t *comp, bool restart)
{
	uint64_t flags = gui_lock(GUI_LOCK_OTHER);
	*comp = s_composite_counts;
	for (int s = 0; s < GUI_LOCK_SITES; s++) {
		out[s] = s_lock_counts[s];
		if (restart)
			s_lock_counts[s].max_wait_cycles = s_lock_counts[s].max_hold_cycles = 0;
	}
	if (restart) {
		s_composite_counts.glass_wait_max_since_read_us = 0;
		s_composite_counts.fc_since_read = (gui_frame_copies_t){ 0 };
		s_composite_counts.clk_since_read = (gui_frame_clock_count_t){ 0 };
	}
	gui_unlock(flags);
}

// ── VT8 glass ownership (Phase B, 2026-08-19 — GRAPHICS.md's VT8 chapter) ───
// The GUI is VT8's seated shell, not the framebuffer's owner-by-force. One
// predicate gates every flush: the compositor paints the IRON only while VT8
// is the focused terminal AND the compositor is seated there. Without the GUI
// cmdline flag VT8 is an ordinary text terminal — dormant, husk-on-knock —
// and this predicate stays false for the machine's whole life (ruling 2).
//
// Compositing into the RAM backbuffer continues while a text VT holds the
// glass: it is cheap, it keeps window state current, and it is what makes
// switching back ONE full-screen damage add instead of a scene rebuild — the
// backbuffer is VT8's grid, and this is its repaint-from-state.
static volatile bool s_gui_seated = false;

bool gui_vt8_seated(void)
{
	return s_gui_seated;
}

bool gui_owns_glass(void)
{
	return s_gui_seated && kTTYFocused == &kTTY[7];
}

// (gui_vt8_focus_gained lives below kBackbuffer's declaration.)

// ---------------------------------------------------------------------------
// Damage accumulator, v2: a small rect LIST (2026-08-18).
//
// v1 kept ONE union rect, and the P5 shakedown priced that choice: a bouncing
// ball in one corner unioned with a cursor in the other produced a near-
// fullscreen rectangle, recomposited and pushed through the write-combining
// framebuffer ~100 times a second. The screen was mostly unchanged; we paid
// retail for it anyway. Two small rects should cost two small rects.
//
// The list's rule (merge when merging is cheap, collapse to one union when
// full, and why entries may still overlap) is shared with libos64's window
// painting: shared/damage.c, os64/damage.h. What is the compositor's own is
// the list itself and its two numbers, below.
// ---------------------------------------------------------------------------
// Sixteen since the rubber band (2026-08-19). An interactive resize adds the
// four edges of the OLD outline and the four of the NEW one every time the
// mouse moves — eight thin rects that deliberately do not merge with each
// other (their unions are the whole window's area, which is exactly what the
// slack rule is there to refuse). At the old ceiling of eight, adding the
// cursor's rect on top tipped every band frame into the union fallback and a
// full-screen composite, which is the precise cost the list was built to
// avoid. The population argument in the comment above is unchanged; the
// population simply grew a member.
#define DAMAGE_MAX_RECTS   16
// One 8x16 glyph cell (128 px) of tolerated waste per merge: generous enough
// that a scrolling console line's per-glyph damage collapses into one row
// rect, tight enough that opposite corners of a 1024x768 screen never do.
#define DAMAGE_MERGE_SLACK 128

static rect_t   kPendingDamage[DAMAGE_MAX_RECTS];
static uint32_t kPendingDamageCount;   // 0 means "no damage"

// The canonical screen image (scene + cursor). Written by the compositor
// thread only; /dev/glass readers on other threads copy its pixels, which is
// why it is published through s_backbuffer_ready rather than by its pointer
// (surface_init stores the pointer before the size a reader needs with it).
static surface_t kBackbuffer;
static bool s_backbuffer_ready;

const surface_t *gui_backbuffer(void)
{
	return __atomic_load_n(&s_backbuffer_ready, __ATOMIC_ACQUIRE) ? &kBackbuffer : NULL;
}

// tty_focus calls this instead of a grid repaint when focus lands on the
// seated VT8. It runs in the SWITCHER's context — usually the keyboard IRQ —
// and GUI code never runs in ISR context (invariant 4; gui_damage_add's own
// header says NOT from IRQ handlers). So this is ONE STORE to a flag, the
// same lock-free shape as gui_emergency_disable: the compositor's frame loop
// converts it into full-screen damage under its own lock. The keystroke's
// interrupt is what ends the compositor's hlt, so the repaint follows within
// a frame anyway.
static volatile bool s_glass_regained = false;

void gui_vt8_focus_gained(void)
{
	s_glass_regained = true;
	gui_frame_nudge();
}

// The desktop: bottom layer of the scene, painted once at startup. Windows
// (M5) stack on top of it, the cursor on top of everything.
static surface_t kDesktop;

// ---------------------------------------------------------------------------
// Frame pacing: hlt-wait. With the frame clock on, the clock paces (its
// section below; FRAME_CLOCK.md § 9), and its stopped state is exactly this
// hlt-wait plus a nudge. What follows is the pacing with the clock off.
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
	// The rule is shared/damage.c's. A full list that nothing merges into
	// falls back to v1, one union rect for the whole frame, and is counted,
	// because a union across the screen makes the next pass long, and a long
	// pass lets the list fill again (GUI_LOCK.md).
	if (os64_damage_add(kPendingDamage, &kPendingDamageCount, DAMAGE_MAX_RECTS, DAMAGE_MERGE_SLACK,
	                    screen_rect))
		s_composite_counts.collapses++;
	gui_frame_nudge();   // after the damage is in: the compositor looks for it next
}

// ---------------------------------------------------------------------------
// Publish to glass: how long a publisher's pixels wait to be seen.
//
// Each publish with damage visible on the screen stamps itself here, beside
// the damage list and under the same lock (gui_glass_publish_locked); one
// with none (its window covered, minimized, or off the screen) is voided at
// once. The pass that takes the damage takes these stamps with it; once its
// flush has put pixels on the framebuffer, each publish's wait is now minus
// its stamp. A pass that flushes nothing (a text terminal holds the screen,
// or nothing it took survived the clip) voids what it took instead.
//
// The waits are measured after the flush, outside kGuiLock, by the compositor
// thread alone, so they are kept in s_glass_owed and folded into the counts
// at the start of the next pass, under the lock that pass takes anyway: no
// lock and no wait of their own. A reader therefore sees a pass's waits from
// the next pass on, which on a desktop with nothing to draw is the
// compositor's idle wake (the "Frame pacing" comment).
//
// More publishes between two passes than the list holds share its last slot
// and its stamp, which is the oldest of them: a shared wait is never counted
// short.
// ---------------------------------------------------------------------------
#define GLASS_PENDING_MAX 32

typedef struct {
	uint64_t stamp_us;
	uint32_t publishes;
} glass_pending_t;

static glass_pending_t s_glass_pending[GLASS_PENDING_MAX];   // under kGuiLock
static uint32_t s_glass_pending_count;

// Written by the compositor thread outside the lock, folded in under it.
static struct {
	uint64_t waits, wait_us, max_us, voided;
	uint64_t buckets[GUI_GLASS_WAIT_BUCKETS];
} s_glass_owed;

void gui_glass_publish_locked(const rect_t *pieces, size_t count)
{
	// Visible means some piece is on the screen: a window's visible damage is
	// not clipped to the screen, and a piece wholly off it composites to
	// nothing, so it could otherwise be counted as a wait whenever the same
	// pass flushed something else.
	rect_t screen = {0, 0, (int32_t)kBackbuffer.width, (int32_t)kBackbuffer.height};
	rect_t on_screen;
	bool visible = false;
	for (size_t i = 0; i < count && !visible; i++)
		visible = rect_intersect(pieces[i], screen, &on_screen);
	if (!visible) {
		s_composite_counts.glass_voided++;
		return;
	}
	if (s_glass_pending_count < GLASS_PENDING_MAX) {
		s_glass_pending[s_glass_pending_count].stamp_us = micros_now();
		s_glass_pending[s_glass_pending_count].publishes = 1;
		s_glass_pending_count++;
	} else {
		s_glass_pending[GLASS_PENDING_MAX - 1].publishes++;
	}
}

// The waits the previous pass measured, into the counts. Under kGuiLock.
static void glass_fold_locked(void)
{
	gui_composite_count_t *c = &s_composite_counts;
	c->glass_waits += s_glass_owed.waits;
	c->glass_wait_us += s_glass_owed.wait_us;
	c->glass_voided += s_glass_owed.voided;
	if (s_glass_owed.max_us > c->glass_wait_max_us)
		c->glass_wait_max_us = s_glass_owed.max_us;
	if (s_glass_owed.max_us > c->glass_wait_max_since_read_us)
		c->glass_wait_max_since_read_us = s_glass_owed.max_us;
	for (int k = 0; k < GUI_GLASS_WAIT_BUCKETS; k++)
		c->glass_buckets[k] += s_glass_owed.buckets[k];
	s_glass_owed.waits = s_glass_owed.wait_us = s_glass_owed.max_us = s_glass_owed.voided = 0;
	for (int k = 0; k < GUI_GLASS_WAIT_BUCKETS; k++)
		s_glass_owed.buckets[k] = 0;
}

// What a pass took has reached the glass at `now_us` (`flushed`), or never
// will. Compositor thread, outside the lock.
static void glass_settle(const glass_pending_t *taken, uint32_t count, bool flushed)
{
	uint64_t now_us = count != 0 && flushed ? micros_now() : 0;
	for (uint32_t i = 0; i < count; i++) {
		if (!flushed) {
			s_glass_owed.voided += taken[i].publishes;
			continue;
		}
		uint64_t wait = now_us > taken[i].stamp_us ? now_us - taken[i].stamp_us : 0;
		int k = 0;
		while (k < GUI_GLASS_WAIT_BUCKETS - 1 && wait >= (1000ull << k))
			k++;
		s_glass_owed.waits += taken[i].publishes;
		s_glass_owed.wait_us += wait * taken[i].publishes;
		s_glass_owed.buckets[k] += taken[i].publishes;
		if (wait > s_glass_owed.max_us)
			s_glass_owed.max_us = wait;
	}
}

// ---------------------------------------------------------------------------
// The frame clock, measured (FRAME_CLOCK.md slice F1; GUI_LOCK.md).
//
// The pass's flush is a copy to the framebuffer at whatever moment the pass
// runs, so where the display's sweep was when it began, and whether it
// overtook the copy, is the tearing in numbers. With a beam (driver/display/
// beam.h) the line is read before the first rectangle and after the last,
// and the pure verdict (gui/frame_clock.h) judges the copy: its pieces are
// the rectangles in the order flushed, each copied top to bottom.
//
// Two reads a pass that flushes, outside kGuiLock where the flush already
// is. The results are the compositor thread's alone, kept in s_fc_owed and
// folded into the counts at the next pass's hold, as the publish-to-glass
// waits are: no lock and no wait of their own.
// ---------------------------------------------------------------------------
static gui_frame_copies_t s_fc_owed;

static void frame_copies_add(gui_frame_copies_t *into, const gui_frame_copies_t *from)
{
	into->copies += from->copies;
	into->blank += from->blank;
	into->torn += from->torn;
	for (int b = 0; b < FC_BANDS; b++)
		into->bands[b] += from->bands[b];
}

// What the frame clock did since the last fold (the clock's section below),
// kept and folded the same way.
static gui_frame_clock_count_t s_fc_clk_owed;

static void frame_clock_counts_add(gui_frame_clock_count_t *into, const gui_frame_clock_count_t *from)
{
	into->frames += from->frames;
	for (int c = 0; c < FC_LATE_CAUSES; c++)
		into->late[c] += from->late[c];
	into->forced += from->forced;
	into->aims += from->aims;
	into->aim_us += from->aim_us;
	if (from->aim_max_us > into->aim_max_us)
		into->aim_max_us = from->aim_max_us;
	into->wakes += from->wakes;
	for (int k = 0; k < FC_WAKE_BUCKETS; k++)
		into->wake_late[k] += from->wake_late[k];
}

// The copies the previous pass judged, and what the clock did, into the
// counts. Under kGuiLock.
static void frame_clock_fold_locked(void)
{
	if (s_fc_owed.copies != 0) {
		frame_copies_add(&s_composite_counts.fc, &s_fc_owed);
		frame_copies_add(&s_composite_counts.fc_since_read, &s_fc_owed);
		s_fc_owed = (gui_frame_copies_t){ 0 };
	}
	frame_clock_counts_add(&s_composite_counts.clk, &s_fc_clk_owed);
	frame_clock_counts_add(&s_composite_counts.clk_since_read, &s_fc_clk_owed);
	s_fc_clk_owed = (gui_frame_clock_count_t){ 0 };
}

// One copy, begun with the sweep at `begin_line` at `begin_us`, as pieces in
// the order copied. Compositor thread, outside the lock, right after it.
static void frame_clock_judge_pieces(const fc_piece_t *pieces, uint32_t n, uint32_t begin_line,
                                     uint64_t begin_us)
{
	uint64_t end_us = micros_now();
	uint32_t end_line;
	fc_frame_t frame;
	if (!beam_line(&end_line) || !beam_frame(&frame))
		return;
	fc_verdict_t v = frame_clock_judge(&frame, begin_line, end_line, end_us - begin_us, pieces, n);
	s_fc_owed.copies++;
	s_fc_owed.blank += v.began_in_blank;
	s_fc_owed.torn += v.torn;
	s_fc_owed.bands[v.band]++;
}

// A pass's flush with the clock off: its pieces are the rectangles flushed.
static void frame_clock_judge_copy(const rect_t *damage, uint32_t count, uint32_t begin_line,
                                   uint64_t begin_us)
{
	fc_piece_t pieces[DAMAGE_MAX_RECTS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < count && n < DAMAGE_MAX_RECTS; i++) {
		if (rect_is_empty(damage[i]))
			continue;
		pieces[n++] = (fc_piece_t){ damage[i].y, damage[i].y + damage[i].h,
		                            (uint64_t)damage[i].w * (uint64_t)damage[i].h };
	}
	frame_clock_judge_pieces(pieces, n, begin_line, begin_us);
}

// ---------------------------------------------------------------------------
// The frame clock (FRAME_CLOCK.md § 3 to § 9; § 9's table is the states).
//
// With the clock on, a frame is: wake the lead before the blank (the CPU's
// one-shot, frame_wake_set), today's pass, aim (poll the beam until the
// blank begins), copy the frame top to bottom in strips, then plan the next.
// A frame whose blank is missed is LATE: nothing is copied, the rectangles
// are CARRIED, and the next frame copies them with whatever arrives since,
// so a late frame costs a refresh and does not tear. A frame held once and
// late again is FORCED, copied wherever the sweep is (fc_missed): it may
// tear, but the screen never stands still. With nothing to show and
// nobody waiting, the clock STOPS: the compositor halts as it does with the
// clock off, and new work NUDGES it awake (gui_frame_nudge). Without a beam
// the clock runs FREE: a sixtieth of a second on micros_now(), no aim.
//
// Everything here is the compositor thread's, outside kGuiLock, except what
// says otherwise: the nudge's flag, the dial and the times, which are read
// from other CPUs, and the reservation, which the scheduler reads.
// ---------------------------------------------------------------------------
#define FC_STRIP_ROWS  32      // a strip's height; 45 strips at 1440 rows
#define FC_COPY_PIECES 128     // pieces the verdict is given, one a strip

static const char *s_fc_off_why;       // decided once, at gui_start; NULL = on
static uint32_t s_fc_cpu;              // the compositor's CPU while the clock is on
static thread_t *s_fc_self;            // the compositor's thread
static bool s_fc_running;              // running, else stopped
static const char *volatile s_fc_stop_why = "nothing to show";
static volatile bool s_fc_reserve_dial = true;     // /sys/gui's `reserve on|off`
static volatile uint32_t s_fc_lead_now;            // the lead in use
static fc_lead_t s_fc_lead;
static fc_run_t s_fc_run;
static bool s_fc_had_damage;           // the last pass drew something

// The nudge's flag: true only while the compositor is halted with the clock
// stopped. Set by the compositor with interrupts off before it halts, taken
// (exchanged to false) by whoever nudges first.
static volatile bool s_fc_halted_stopped;

// How long the clock has run and stood stopped. One writer, the compositor
// thread, at each change, under kGuiLock; /sys/gui adds the stretch under
// way, under the same lock.
static struct {
	volatile uint64_t running_us, stopped_us, since_us, starts;
} s_fc_time;

// The frame being aimed at, from its plan to its copy or its lateness.
static bool s_fc_planned;
static uint64_t s_fc_wake_at, s_fc_blank_at, s_fc_woke, s_fc_free_ns, s_fc_loads;
static uint32_t s_fc_plan_lead;

// The copy owed: rectangles composed and not yet on the framebuffer, and
// the publishes behind them.
static rect_t s_fc_carry[DAMAGE_MAX_RECTS];
static uint32_t s_fc_carry_count;
static glass_pending_t s_fc_glass_carry[GLASS_PENDING_MAX];
static uint32_t s_fc_glass_carry_count;
// Whether what is carried was held at a missed blank already (the hold
// rule, frame_clock_hold_step). Whatever copies or drops the carry ends it.
static fc_hold_t s_fc_hold;

// The stall tripwire's view of the compositor (gui_frame_clock_watch_locked):
// a beat a loop, where in the frame it is, and since when composed damage
// has waited for the glass (0 = nothing owed). Written by the compositor,
// read from the BSP's pass without a lock; a stale read moves a report by
// a tick.
typedef enum { FC_AT_PASS, FC_AT_SLEEP, FC_AT_AIM, FC_AT_COPY, FC_AT_STOPPED, FC_AT_PLACES } fc_at_t;
static volatile uint64_t s_fc_beat;
static volatile uint32_t s_fc_at;
static volatile uint64_t s_fc_owed_since_us;
// The tripwire's record, for /sys/gui: written by the watcher alone.
static struct {
	volatile uint64_t episodes, longest_us;
} s_fc_stall;
// A stall asked for by /sys/gui's `stall <ms>` (gui_frame_clock_command),
// taken by the compositor's loop at its next turn: the tripwire's proof.
static volatile uint64_t s_fc_test_stall_ms;

static fc_piece_t s_fc_pieces[FC_COPY_PIECES];

// Why the clock is off this boot, or NULL. From boot facts alone, so
// gui_compositor_affinity can ask before the compositor exists.
static const char *frame_clock_off_why(void)
{
	if (kNoFrameClock)
		return "NOFRAMECLOCK on the command line";
	if (kCPUCount < 2)
		return "one CPU: nowhere to pin the compositor";
	if (!kTicklessScheduler)
		return "SCHED=periodic: every CPU's timer is the periodic tick, not the compositor's to set";
	// Without the lease nothing keeps the AP's timer masked between the
	// compositor's writes, and scheduler_trigger would arm a stray one-shot
	// in it (the frame wake shares the timer by the lease's discipline).
	if (!kSchedBackstopEnabled)
		return "NOBACKSTOP: no lease for the frame wake to share the timer with";
	return NULL;
}

void gui_frame_nudge(void)
{
	if (!__atomic_exchange_n(&s_fc_halted_stopped, false, __ATOMIC_SEQ_CST))
		return;
	// An interrupt handler on the compositor's own CPU has already ended
	// the halt by arriving; an interrupt to itself would buy nothing. The
	// interrupt sent is a pass, so it carries the claim (scheduler.h): the
	// pass keeps the CPU for the compositor rather than handing it to a
	// thread that aged while it halted.
	if (cpu_self() != s_fc_cpu) {
		__atomic_store_n(&kSchedFrameNudged, true, __ATOMIC_SEQ_CST);
		send_ipi(s_fc_cpu, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
	}
}

// The reservation follows the state and the dial. The compositor thread
// alone writes it (FRAME_CLOCK.md § 9, the reservation).
static void fc_apply_reserve(void)
{
	bool reserve = frame_clock_reserves(kCPUCount, s_fc_reserve_dial, s_fc_running);
	int32_t want = reserve ? (int32_t)s_fc_cpu : SCHED_NO_RESERVED_CPU;
	if (kSchedReservedCPU != want)
		kSchedReservedCPU = want;
}

// The change is made under kGuiLock, which /sys/gui's read holds too
// (gui_frame_clock_status), so a read never sees the times half moved: a
// stretch counted twice there would become the since-read baseline and the
// next read would wrap. A start or a stop is rare enough to pay for it.
static void fc_set_running(bool running)
{
	if (running == s_fc_running)
		return;
	uint64_t flags = gui_lock(GUI_LOCK_OTHER);
	uint64_t now = micros_now();
	uint64_t spent = now - s_fc_time.since_us;
	if (s_fc_running)
		s_fc_time.running_us += spent;
	else
		s_fc_time.stopped_us += spent;
	s_fc_time.since_us = now;
	if (running)
		s_fc_time.starts++;
	s_fc_running = running;
	gui_unlock(flags);
	fc_apply_reserve();
}

static void fc_carry_add(rect_t r)
{
	if (rect_is_empty(r))
		return;
	if (s_fc_carry_count == 0)
		s_fc_owed_since_us = micros_now();
	if (s_fc_carry_count < DAMAGE_MAX_RECTS) {
		s_fc_carry[s_fc_carry_count++] = r;
		return;
	}
	// Full: the last becomes the box around it and the newcomer. The strips
	// copy each pixel once whatever overlaps (frame_clock_strip).
	rect_t *last = &s_fc_carry[DAMAGE_MAX_RECTS - 1];
	int32_t x0 = r.x < last->x ? r.x : last->x, y0 = r.y < last->y ? r.y : last->y;
	int32_t x1 = r.x + r.w > last->x + last->w ? r.x + r.w : last->x + last->w;
	int32_t y1 = r.y + r.h > last->y + last->h ? r.y + r.h : last->y + last->h;
	*last = (rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

// More publishes than the list holds share its last slot and that slot's
// stamp, the oldest of them, as gui_glass_publish_locked's list does.
static void fc_glass_carry_add(const glass_pending_t *taken, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (s_fc_glass_carry_count < GLASS_PENDING_MAX)
			s_fc_glass_carry[s_fc_glass_carry_count++] = taken[i];
		else
			s_fc_glass_carry[GLASS_PENDING_MAX - 1].publishes += taken[i].publishes;
	}
}

// What was carried will never reach the glass as composed (a text terminal
// took the screen; its return repaints it all): the publishes are voided.
static void fc_drop_carry(void)
{
	glass_settle(s_fc_glass_carry, s_fc_glass_carry_count, false);
	s_fc_glass_carry_count = 0;
	s_fc_carry_count = 0;
	s_fc_owed_since_us = 0;
	s_fc_hold = (fc_hold_t){0};
	s_fc_planned = false;
}

// Halt until `wake_at`, setting the timer again after every return from the
// halt, whatever ended it (the two-writers invariant, smp_core.c). The wake
// is published first as the frame's window (kSchedFrameDueUs): from it until
// the next plan, a pass on this CPU keeps the CPU for the compositor, and a
// thread that takes the CPU while the compositor sleeps runs only until it.
static void fc_sleep_until(uint64_t wake_at)
{
	s_fc_at = FC_AT_SLEEP;
	kSchedFrameDueUs = wake_at;
	for (;;) {
		__asm__ volatile("cli" ::: "memory");
		uint64_t now = micros_now();
		if (now >= wake_at) {
			__asm__ volatile("sti" ::: "memory");
			return;
		}
		frame_wake_set(wake_at - now);
		mpAcctHaltBegin();
		__asm__ volatile("sti\n\thlt" ::: "memory");
		mpAcctHaltEnd();
	}
}

// Stopped: one halt, as the clock-off compositor's, with the timer the lease
// alone and the nudge's flag raised. The flag goes up BEFORE the last look
// for work, both with interrupts off, and a producer adds its work before it
// takes the flag, so work that arrives after the look always finds the flag
// up and interrupts the halt (sti's shadow delivers it at the hlt).
static void fc_stopped_halt(void)
{
	s_fc_at = FC_AT_STOPPED;
	__asm__ volatile("cli" ::: "memory");
	__atomic_store_n(&s_fc_halted_stopped, true, __ATOMIC_SEQ_CST);
	if (!input_pending() && kPendingDamageCount == 0 && !s_glass_regained) {
		frame_wake_cancel();
		mpAcctHaltBegin();
		__asm__ volatile("sti\n\thlt" ::: "memory");
		mpAcctHaltEnd();
	} else {
		__asm__ volatile("sti" ::: "memory");
	}
	__atomic_store_n(&s_fc_halted_stopped, false, __ATOMIC_SEQ_CST);
}

// The copy: what is carried, in strips top to bottom across all of it
// (frame_clock_strip), so the copy only ever moves down the screen. With a
// beam, F1's verdict judges it from the aim's last reading.
//
// THE SCREEN IS ASKED FOR AGAIN before the copy and before every strip: the
// pass asked once, but the sleep and the aim since can take most of a frame,
// and a text terminal taking the screen in between repaints the console,
// which a copy would then overwrite. The moment it is not ours, what is
// carried is voided (fc_drop_carry); the return switch repaints it all.
static void fc_copy(bool judge, uint32_t begin_line, uint64_t begin_us, uint64_t *flushes)
{
	s_fc_at = FC_AT_COPY;
	fc_rect_t rects[DAMAGE_MAX_RECTS], out[DAMAGE_MAX_RECTS];
	for (uint32_t i = 0; i < s_fc_carry_count; i++)
		rects[i] = (fc_rect_t){ s_fc_carry[i].x, s_fc_carry[i].y, s_fc_carry[i].w, s_fc_carry[i].h };
	uint32_t pieces = 0;
	int32_t height = (int32_t)kBackbuffer.height;
	for (int32_t top = 0; top < height; top += FC_STRIP_ROWS) {
		if (!gui_owns_glass()) {
			fc_drop_carry();
			return;
		}
		uint32_t k = frame_clock_strip(rects, s_fc_carry_count, top, FC_STRIP_ROWS, out);
		if (k == 0)
			continue;
		uint64_t px = 0;
		for (uint32_t i = 0; i < k; i++) {
			surface_flush_rect(&kBackbuffer, (rect_t){ out[i].x, out[i].y, out[i].w, out[i].h });
			px += (uint64_t)out[i].w * (uint64_t)out[i].h;
			(*flushes)++;
		}
		int32_t bottom = top + FC_STRIP_ROWS < height ? top + FC_STRIP_ROWS : height;
		if (pieces < FC_COPY_PIECES) {
			s_fc_pieces[pieces++] = (fc_piece_t){ top, bottom, px };
		} else {
			s_fc_pieces[FC_COPY_PIECES - 1].bottom = bottom;
			s_fc_pieces[FC_COPY_PIECES - 1].pixels += px;
		}
	}
	if (judge)
		frame_clock_judge_pieces(s_fc_pieces, pieces, begin_line, begin_us);
	glass_settle(s_fc_glass_carry, s_fc_glass_carry_count, true);
	s_fc_glass_carry_count = 0;
	s_fc_carry_count = 0;
	s_fc_owed_since_us = 0;
	s_fc_hold = (fc_hold_t){0};
	s_fc_clk_owed.frames++;
}

// The frame missed its blank: held for the next one if this is its first
// miss, forced if it was held already (the hold rule, frame_clock_hold_step),
// so the screen keeps changing whatever makes every frame late. `cause` is
// counted as late; FC_LATE_CAUSES is a miss that is not lateness (no reading
// of the sweep to aim by) and counts nothing but a force. A forced copy is
// judged from a fresh reading when one can be had, so `torn` says what
// forcing cost.
static void fc_missed(fc_late_t cause, uint64_t *flushes)
{
	if (cause < FC_LATE_CAUSES)
		s_fc_clk_owed.late[cause]++;
	if (frame_clock_hold_step(&s_fc_hold, false) != FC_SHOW_FORCE)
		return;
	s_fc_clk_owed.forced++;
	uint32_t line = 0;
	bool read = beam_present() && beam_line(&line);
	fc_copy(read, line, read ? micros_now() : 0, flushes);
}

// After the pass, with the clock driving and the screen ours: carry what it
// composed, then aim and copy, or hold the frame late (or force it, if it
// was held already).
static void frame_clock_present(const rect_t *damage, uint32_t count, const glass_pending_t *taken,
                                uint32_t taken_count, uint64_t *flushes)
{
	uint64_t pass_end = micros_now();
	bool had = false;
	for (uint32_t i = 0; i < count; i++) {
		had |= !rect_is_empty(damage[i]);
		fc_carry_add(damage[i]);
	}
	s_fc_had_damage = had;
	fc_glass_carry_add(taken, taken_count);
	if (s_fc_planned)
		frame_clock_lead_record(&s_fc_lead, pass_end - s_fc_woke);
	if (s_fc_carry_count == 0) {
		// Publishes whose damage composited to nothing: nothing will show.
		glass_settle(s_fc_glass_carry, s_fc_glass_carry_count, false);
		s_fc_glass_carry_count = 0;
		s_fc_planned = false;
		return;
	}

	fc_frame_t f;
	uint32_t line = 0;
	if (!beam_present() || !beam_frame(&f)) {
		fc_copy(false, 0, 0, flushes);   // free: the copy follows the pass
		s_fc_planned = false;
		return;
	}
	if (!s_fc_planned) {
		// The first frame since the clock stopped: no wake was planned for
		// it, so aim at the first blank the shortest lead away.
		if (!beam_line(&line)) {
			if (!beam_present())
				fc_copy(false, 0, 0, flushes);
			else
				fc_missed(FC_LATE_CAUSES, flushes);   // a failed read: nothing to aim by
			return;
		}
		fc_plan_t p = frame_clock_plan(&f, line, micros_now(), FC_LEAD_MIN_US);
		s_fc_blank_at = p.blank_at_us;
		s_fc_wake_at = p.wake_at_us;
		s_fc_plan_lead = FC_LEAD_MIN_US;
		s_fc_loads = s_fc_self->timesLoaded;
		fc_sleep_until(s_fc_wake_at);
		s_fc_woke = pass_end = micros_now();
	}
	s_fc_planned = false;

	// The aim (§ 9): poll the beam until the blank, within the bound.
	s_fc_at = FC_AT_AIM;
	uint64_t bound = frame_clock_aim_bound_us(&f, s_fc_plan_lead);
	uint64_t t0 = micros_now(), now = t0;
	uint32_t first = 0;
	bool seen = false, moved = false;
	fc_aim_t verdict = FC_AIM_WAIT;
	for (;;) {
		now = micros_now();
		if (beam_line(&line)) {
			if (!seen) {
				first = line;
				seen = true;
			} else if (line != first) {
				moved = true;
			}
			verdict = frame_clock_aim(&f, line, now, s_fc_blank_at);
			if (verdict != FC_AIM_WAIT)
				break;
		} else if (!beam_present()) {
			fc_copy(false, 0, 0, flushes);   // the reads withdrew it: free from now
			return;
		}
		if (now - t0 > bound)
			break;
		__builtin_ia32_pause();
	}

	if (verdict == FC_AIM_COPY && !moved) {
		// The first reading was already in the blank, so nothing yet says the
		// counter moves, and one stopped at a blank line would look like a
		// perfect aim every frame. It must change within FC_AIM_CONFIRM_LINES.
		//
		// Only a window actually WATCHED is evidence: each reading is taken
		// before the clock is looked at, and a gap between readings longer
		// than a line (an interrupt, or a pass that took the CPU) means the
		// counter may have moved unseen. Such a window, or a failed read, is
		// inconclusive: nothing is withdrawn, and a fresh reading decides this
		// frame alone (the next frame's aim confirms again).
		uint64_t span = frame_clock_lines_us(&f, FC_AIM_CONFIRM_LINES);
		uint64_t gap_max = frame_clock_lines_us(&f, 1);
		uint64_t start = micros_now(), last = start;
		bool changed = false, inconclusive = false;
		for (;;) {
			uint32_t again;
			bool read = beam_line(&again);
			uint64_t t = micros_now();
			if (t - last > gap_max)
				inconclusive = true;
			last = t;
			if (!read) {
				inconclusive = true;
				break;
			}
			if (again != line) {
				changed = true;
				line = again;
				break;
			}
			if (inconclusive || t - start > span)
				break;
			__builtin_ia32_pause();
		}
		if (!beam_present()) {
			fc_copy(false, 0, 0, flushes);
			return;
		}
		if (!changed && !inconclusive) {
			beam_withdraw("the line counter stopped in the blank");
			fc_copy(false, 0, 0, flushes);
			return;
		}
		if (!changed && !beam_line(&line)) {
			fc_missed(FC_LATE_PAST, flushes);   // no reading to copy by
			return;
		}
		now = micros_now();
		if (!frame_clock_in_blank(&f, line)) {
			fc_missed(FC_LATE_PAST, flushes);   // the blank ended while it was confirmed
			return;
		}
	}

	if (verdict == FC_AIM_COPY) {
		frame_clock_hold_step(&s_fc_hold, true);
		uint64_t aim = now - t0;
		s_fc_clk_owed.aims++;
		s_fc_clk_owed.aim_us += aim;
		if (aim > s_fc_clk_owed.aim_max_us)
			s_fc_clk_owed.aim_max_us = aim;
		fc_copy(true, line, now, flushes);
	} else if (verdict == FC_AIM_LATE) {
		bool off_cpu = s_fc_self->timesLoaded != s_fc_loads;
		fc_late_t cause = frame_clock_late_cause(off_cpu, s_fc_wake_at, s_fc_woke, pass_end,
		                                         frame_clock_blank_end_us(&f, s_fc_blank_at));
		fc_missed(cause, flushes);
	} else if (seen && !moved) {
		beam_withdraw("the line counter stopped during an aim");
		fc_copy(false, 0, 0, flushes);
	} else if (seen) {
		fc_missed(FC_LATE_OVERRUN, flushes);
	} else {
		// No reading succeeded within the bound, and too few failed in a row
		// to withdraw the beam: nothing to aim by, so this frame goes unaimed
		// rather than counted as a counter that moved.
		fc_copy(false, 0, 0, flushes);
	}
}

// After the frame, with the clock on: stop, or plan the next frame and
// sleep until its wake.
static void frame_clock_pace(void)
{
	fc_apply_reserve();   // the dial may have moved since the last frame
	bool owns = gui_owns_glass();
	bool work = input_pending() || kPendingDamageCount != 0 || s_glass_regained;
	bool show = frame_clock_keep_running(&s_fc_run, s_fc_had_damage || work, 0) || s_fc_carry_count > 0;
	s_fc_had_damage = false;
	if (!owns || !show) {
		s_fc_stop_why = owns ? "nothing to show" : "a text terminal holds the screen";
		fc_set_running(false);
		s_fc_planned = false;
		kSchedFrameDueUs = 0;   // no frame planned: the CPU's claim is the nudge's alone
		fc_stopped_halt();
		return;
	}
	fc_set_running(true);

	uint32_t lead = frame_clock_lead_us(&s_fc_lead);
	s_fc_lead_now = lead;
	s_fc_loads = s_fc_self->timesLoaded;
	uint64_t now = micros_now();
	fc_frame_t f;
	uint32_t line;
	if (beam_present() && beam_frame(&f) && beam_line(&line)) {
		fc_plan_t p = frame_clock_plan(&f, line, now, lead);
		s_fc_blank_at = p.blank_at_us;
		s_fc_wake_at = p.wake_at_us;
	} else {
		s_fc_free_ns = frame_clock_free_next_ns(s_fc_free_ns, now * 1000);
		s_fc_wake_at = s_fc_blank_at = s_fc_free_ns / 1000;
	}
	s_fc_plan_lead = lead;
	s_fc_planned = true;
	fc_sleep_until(s_fc_wake_at);
	s_fc_woke = micros_now();
	s_fc_clk_owed.wakes++;
	s_fc_clk_owed.wake_late[frame_clock_wake_bucket(s_fc_woke - s_fc_wake_at)]++;
}

void gui_frame_clock_status(gui_frame_clock_status_t *out, bool restart)
{
	static uint64_t base_running, base_stopped, base_starts;
	*out = (gui_frame_clock_status_t){ 0 };
	out->off_why = s_fc_off_why;
	out->cpu = s_fc_cpu;
	out->reserve_dial = s_fc_reserve_dial;
	out->reserve_applies = kCPUCount >= FC_RESERVE_MIN_CPUS;
	if (s_fc_off_why != NULL)
		return;
	uint64_t flags = gui_lock(GUI_LOCK_OTHER);   // one reader's baseline at a time
	bool running = s_fc_running;
	uint64_t partial = micros_now() - s_fc_time.since_us;
	out->running = running;
	out->stop_why = s_fc_stop_why;
	out->free = !beam_present();
	out->reserved = kSchedReservedCPU == (int32_t)s_fc_cpu;
	out->lead_us = s_fc_lead_now;
	out->running_us = s_fc_time.running_us + (running ? partial : 0);
	out->stopped_us = s_fc_time.stopped_us + (running ? 0 : partial);
	out->starts = s_fc_time.starts;
	out->running_since_read_us = out->running_us - base_running;
	out->stopped_since_read_us = out->stopped_us - base_stopped;
	out->starts_since_read = out->starts - base_starts;
	if (restart) {
		base_running = out->running_us;
		base_stopped = out->stopped_us;
		base_starts = out->starts;
	}
	gui_unlock(flags);
	out->frame_wake_fires = kFrameWakeFires[s_fc_cpu];
	out->lease_fires = kSchedBackstopFires[s_fc_cpu];
	out->stalls = s_fc_stall.episodes;
	out->stall_longest_us = s_fc_stall.longest_us;
}

// ── The stall tripwire (FRAME_CLOCK.md § 8) ────────────────────────────────
// With the hold rule, composed damage reaches the glass within two of the
// compositor's passes, and its loop turns at least once a lease even when
// stopped. So a half second with damage owed and nothing shown, or with
// the loop standing still, is a fault, and this names it: once an episode,
// under DEBUG_EXCEPTIONS, with where the compositor was, its state, what its
// CPU was running and what that CPU's passes and timer did. It logs; it
// does not heal. Called from the BSP's pass once a tick, holding the queue
// lock (the tenant's name is read under it); its own state is that pass's.
#define FC_STALL_US 500000

static const char *fc_at_name(uint32_t at)
{
	static const char *const names[FC_AT_PLACES] = {
		"in its pass (composing, or waiting for kGuiLock)", "asleep until its wake",
		"aiming at the blank", "copying", "stopped",
	};
	return at < FC_AT_PLACES ? names[at] : "?";
}

static const char *fc_thread_state_name(uint32_t state)
{
	switch (state) {
	case THREAD_STATE_RUNNING:  return "running";
	case THREAD_STATE_RUNNABLE: return "runnable";
	case THREAD_STATE_STOPPED:  return "stopped";
	case THREAD_STATE_USLEEP:   return "usleep";
	case THREAD_STATE_ISLEEP:   return "isleep";
	case THREAD_STATE_ZOMBIE:   return "zombie";
	default:                    return "none";
	}
}

void gui_frame_clock_watch_locked(void)
{
	static uint64_t beat, beat_us, at_beat_passes, at_beat_wakes, at_beat_leases;
	static uint64_t start_us, start_passes, start_wakes, start_leases;
	static bool in;
	thread_t *self = kSchedFrameThread;
	if (self == NULL)
		return;   // the clock is off, or the compositor has not published itself
	uint32_t cpu = s_fc_cpu;
	uint64_t now = micros_now();
	uint64_t passes = kDiagPassCount[cpu], wakes = kFrameWakeFires[cpu], leases = kSchedBackstopFires[cpu];
	if (s_fc_beat != beat || beat_us == 0) {
		beat = s_fc_beat;
		beat_us = now;
		at_beat_passes = passes;
		at_beat_wakes = wakes;
		at_beat_leases = leases;
	}
	uint64_t owed_since = s_fc_owed_since_us;
	uint64_t still_us = now - beat_us;
	uint64_t unshown_us = owed_since != 0 && now > owed_since ? now - owed_since : 0;
	bool stalled = still_us > FC_STALL_US || unshown_us > FC_STALL_US;

	if (stalled && !in) {
		in = true;
		start_us = now - (still_us > unshown_us ? still_us : unshown_us);   // when it began, not when it was seen
		start_passes = passes;
		start_wakes = wakes;
		start_leases = leases;
		s_fc_stall.episodes++;
		thread_t *tenant = scheduler_cpu_tenant_locked(cpu);
		const char *tenant_name = tenant == NULL ? "(mid-switch)"
		                        : tenant == self ? "the compositor"
		                        : ((task_t *)tenant->ownerTask)->exename;
		uint64_t due = kSchedFrameDueUs;
		char window[48];
		if (due == 0)
			snprintf(window, sizeof window, "no frame planned");
		else if (now >= due)
			snprintf(window, sizeof window, "its window opened %lu us ago", now - due);
		else
			snprintf(window, sizeof window, "its window opens in %lu us", due - now);
		printd(DEBUG_EXCEPTIONS,
		       "frame clock: STALL %lu: nothing shown for %lu ms with damage owed, the loop still for %lu ms; "
		       "the compositor is %s, %s%s, the frame %s, the clock %s; CPU %u runs %s (thread 0x%lx); "
		       "since the last beat its CPU ran %lu passes, %lu frame wakes, %lu lease expiries; "
		       "%s; reserve %s\n",
		       s_fc_stall.episodes, unshown_us / 1000, still_us / 1000,
		       fc_at_name(s_fc_at), fc_thread_state_name(self->threadState),
		       self->expedite ? " (expedite set)" : "",
		       s_fc_hold.held ? "held once" : "not held", s_fc_running ? "running" : "stopped",
		       cpu, tenant_name, tenant != NULL ? (uint64_t)tenant->threadID : 0,
		       passes - at_beat_passes, wakes - at_beat_wakes, leases - at_beat_leases,
		       window, kSchedReservedCPU == (int32_t)cpu ? "in force" : "not in force");
	} else if (!stalled && in) {
		in = false;
		uint64_t lasted = now - start_us;
		if (lasted > s_fc_stall.longest_us)
			s_fc_stall.longest_us = lasted;
		printd(DEBUG_EXCEPTIONS,
		       "frame clock: STALL %lu over after about %lu ms; since it was seen CPU %u ran %lu passes, "
		       "%lu frame wakes, %lu lease expiries\n",
		       s_fc_stall.episodes, lasted / 1000, cpu,
		       passes - start_passes, wakes - start_wakes, leases - start_leases);
	}
}

long gui_frame_clock_command(const char *line, size_t length)
{
	if (length == 10 && memcmp(line, "reserve on", 10) == 0) {
		s_fc_reserve_dial = true;
		return 0;
	}
	if (length == 11 && memcmp(line, "reserve off", 11) == 0) {
		s_fc_reserve_dial = false;
		return 0;
	}
	// `stall <ms>`, 1 to 5000: the compositor stands still once, on purpose,
	// so the tripwire's report can be seen on any machine (its proof).
	if (length > 6 && memcmp(line, "stall ", 6) == 0) {
		uint64_t ms = 0;
		for (size_t i = 6; i < length; i++) {
			if (line[i] < '0' || line[i] > '9' || ms > 5000)
				return -1;
			ms = ms * 10 + (uint64_t)(line[i] - '0');
		}
		if (ms == 0 || ms > 5000)
			return -1;
		s_fc_test_stall_ms = ms;
		return 0;
	}
	return -1;
}

uint32_t gui_damage_take_locked(rect_t *out, uint32_t cap)
{
	uint32_t n = kPendingDamageCount < cap ? kPendingDamageCount : cap;
	for (uint32_t i = 0; i < n; i++)
		out[i] = kPendingDamage[i];
	for (uint32_t i = n; i < kPendingDamageCount; i++)
		kPendingDamage[i - n] = kPendingDamage[i];
	kPendingDamageCount -= n;
	return n;
}

void gui_damage_add(rect_t screen_rect)
{
	uint64_t flags = gui_lock(GUI_LOCK_DAMAGE);
	gui_damage_add_locked(screen_rect);
	gui_unlock(flags);
}

void gui_census(uint32_t *windows, uint64_t *surface_bytes)
{
	*windows = 0;
	*surface_bytes = 0;
	if (!kEnableGUI)
		return;   // no compositor, no z-list, nothing to lock

	uint64_t flags = gui_lock(GUI_LOCK_OTHER);
	wm_census_locked(windows, surface_bytes);
	gui_unlock(flags);
}

void gui_emergency_disable(void)
{
	// One store, no locks — the doctrine survives the sink it used to serve:
	// panic() calls this first so the compositor stops flushing and cannot
	// overpaint the dying words (tty_emergency_direct handles the grid side;
	// panic text then goes straight at the iron). The desktop gets scribbled
	// over — intentionally.
	s_gui_seated = false;
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
// Recomposite `damage` (screen coords) into the backbuffer: the scene (each
// pixel by the window or desktop visible there), then the band, the
// switcher and the cursor over it. Caller holds kGuiLock; the caller
// flushes AFTER releasing it. This is the only writer of the backbuffer.
// ---------------------------------------------------------------------------
// The rubber band lives with the gesture state that drives it (below), but it
// is a SCENE LAYER and has to be drawn from here — hence the one forward
// declaration. It sits above the windows and below the cursor: an outline you
// could lose the pointer inside would be a poor tool.
static void band_composite(surface_t *backbuffer, rect_t damage);
// Same arrangement for the Alt+Tab strip, and the same reason. Like the band
// it is compositor-owned pixels rather than a window: no window_t, no owner,
// no event queue, not in the z-list, not hit-tested.
static void switcher_composite(surface_t *backbuffer, rect_t damage);

static rect_t composite_locked(rect_t damage)
{
	rect_t screen = {0, 0, (int32_t)kBackbuffer.width, (int32_t)kBackbuffer.height};
	if (!rect_intersect(damage, screen, &damage))
		return (rect_t){0, 0, 0, 0};

	// The scene: each pixel by the one window that can be seen there, or by
	// the desktop surface where no window is.
	wm_composite_result_t scene = wm_composite(&kBackbuffer, &kDesktop, damage);
	s_composite_counts.painted += scene.painted;
	s_composite_counts.stops += scene.stopped;
	s_composite_counts.faults += scene.fault;

	// Over the scene, the interactive-resize outline, when one is up. No-op
	// otherwise, and the check is one pointer compare per damage rect.
	band_composite(&kBackbuffer, damage);

	// Then the Alt+Tab strip. Above the band because it is the more modal
	// of the two — though in practice they cannot coexist, one needing a
	// Ctrl+Alt mouse drag and the other Alt held on the keyboard.
	switcher_composite(&kBackbuffer, damage);

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
static uint8_t s_drag_buttons;
static window_t *s_drag_window = NULL;
static int32_t s_drag_dx, s_drag_dy;   // grab offset: cursor minus frame origin

// The start snapshot makes an unchanged gesture decidable; grab offsets do
// not record the original frame or content size. kGuiLock owns gesture state.
static struct {
	int32_t x, y;
	uint32_t width, height;
	bool maximized;
} s_placement_start;

static void placement_begin_locked(const window_t *w)
{
	s_placement_start.x = w->frame.x;
	s_placement_start.y = w->frame.y;
	s_placement_start.width = w->content.width;
	s_placement_start.height = w->content.height;
	s_placement_start.maximized = (w->flags & GUI_WINDOW_MAXIMIZED) != 0;
}

// Automatic geometry changes do not reach this helper: its callers complete
// user gestures, before any cancellation-triggered automatic adjustment.
// Moving an already fitted window reports positions, preserving preferred size.
static void placement_complete_locked(window_t *w, bool resized)
{
	if ((w->flags & GUI_WINDOW_DESKTOP) || w->popup_owner_id) return;
	uint32_t changed = 0;
	if (w->frame.x != s_placement_start.x) changed |= OS64_GUI_PLACEMENT_X;
	if (w->frame.y != s_placement_start.y) changed |= OS64_GUI_PLACEMENT_Y;
	if (resized && w->content.width != s_placement_start.width)
		changed |= OS64_GUI_PLACEMENT_WIDTH;
	if (resized && w->content.height != s_placement_start.height)
		changed |= OS64_GUI_PLACEMENT_HEIGHT;
	if (!changed) return;
	if (s_placement_start.maximized) changed = OS64_GUI_PLACEMENT_ALL;
	input_event_t ev = { .type = INPUT_EVENT_WINDOW_PLACEMENT,
		.placement = { .x = w->frame.x, .y = w->frame.y,
			.width = w->content.width, .height = w->content.height,
			.changed = changed }, .tick = kTicksSinceStart };
	wm_deliver_event(w, &ev);
}

// An Alt+F4 escalation decided by the router (under kGuiLock) and carried
// out by the frame loop after the lock drops. The owner's task id, 0 = none.
static uint64_t s_terminate_owner = 0;

// Double-click detection on titlebars (maximize toggle). By id, not pointer
// — the first click's window may be dead by the second click.
#define DOUBLE_CLICK_TICKS (TICKS_PER_SECOND / 2)
static uint32_t s_titlebar_click_window = 0;
static uint64_t s_titlebar_click_tick = 0;

// ── Alt+Tab: cycling focus from the keyboard (2026-08-23) ───────────────────
//
// The first QoL chord, and the one every other window-management feature
// leans on: minimize has nowhere to restore FROM without it (there is no
// taskbar and no launcher yet), so it goes first.
//
// The shape is the 1990 one (Windows 3.0 introduced it; every WM since has
// kept it because it is right): while Alt is HELD, each Tab steps one window
// further down the MOST-RECENTLY-FOCUSED order as it stood WHEN THE HOLD
// BEGAN, and a STRIP of window titles shows where the step has landed.
//
// THE FIRST VERSION HAD NO STRIP, and raised each window as it was stepped
// to — the raise WAS the feedback, on the argument that at this size nothing
// more was needed. It cost one rule that had to go: "stepping onto a
// minimized window restores it, and it stays restored if the walk moves on."
// Chris, the first afternoon minimize existed: "I minimized everything, and
// the only way back to the gterm I was on was to Alt+Tab through a bunch of
// things, which brought them all back up." Walking past a window is not a
// request to un-hide it, and with raise-as-feedback there was no way to say
// so — showing you where you were REQUIRED disturbing the scene.
//
// A visible list is the other way, and it is what every desktop grew for
// exactly this reason: Windows 3.1's (1992) was a box of icons. os64's is a
// box of TITLES, because os64 windows have no icons and a title is what they
// have. With the strip up, the scene can hold still — see
// switcher_composite. Release Alt and the hold
// ends; the next hold starts from the new order, which makes a quick Alt+Tab
// a toggle between the two most recent windows, exactly the case people
// reach for. (Recency, not z-order: the first version walked the stack, and
// pin-on-top broke it the same afternoon — a pinned window holds the top of
// the stack while focus goes elsewhere, so "second from the top" stopped
// meaning "the one I was just using". window_t.focusSerial is the fact.)
//
// Why a SNAPSHOT and not a depth counter against the live order: every raise
// rewrites it. With windows A B C D, three presses walking "index = press
// count" do reach B, C, D — but the fourth lands on index 0, which is now D
// again, and the cycle never returns to A. Walking a copy taken at hold
// start returns to A on the fourth press, by construction.
// The copy holds IDS, not pointers: a window can die mid-hold (its task
// exits), and wm_window_by_id answers NULL for the dead where a pointer
// would have answered with freed memory — the exact bug the resize slice
// found in the drag state.
//
// Shift+Alt+Tab walks the other way. Sixteen windows is the most a snapshot
// holds; past that the cycle simply covers the top sixteen, which is more
// windows than this desktop has ever shown at once.

// THE SNAPSHOT is taken once, at the first Tab, and holds everything the
// strip needs to draw itself: ids, titles, and which entries were minimized.
// Not pointers — a window can die mid-hold (its task exits), and an id lets
// wm_window_by_id answer NULL for the dead where a pointer would answer with
// freed memory (the exact bug the resize slice found in the drag state).
//
// Copying the TITLES too, rather than reading them back through the window
// on every repaint, is the same argument one level up: the draw path then
// dereferences nothing at all, and the picture the user is choosing from
// cannot reflow under their hand halfway through a walk. The switcher moves
// no window during a hold, but a client can still rename one
// (gui_window_set_title — a browser does it unprompted as pages load): its
// row keeps the name it had when the hold began, and the new one shows from
// the next hold on.
static bool     s_alttab_active = false;
static uint32_t s_alttab_ring[ALTTAB_RING_MAX];
static char     s_alttab_titles[ALTTAB_RING_MAX][GUI_WINDOW_TITLE_MAX];
static uint8_t  s_alttab_title_len[ALTTAB_RING_MAX];
static bool     s_alttab_dim[ALTTAB_RING_MAX];   // minimized when the hold began
static size_t   s_alttab_count = 0;
static size_t   s_alttab_step  = 0;

// Set when Escape cancelled a hold, so the router can swallow that same
// Escape's release edge (see the cancel arm in route_event_locked).
static bool     s_alttab_eat_esc_up = false;

// The strip's pixels, computed once per hold by switcher_layout_locked.
static bool     s_alttab_shown = false;
static rect_t   s_alttab_strip = {0, 0, 0, 0};
static int32_t  s_alttab_row_w = 0;
static int32_t  s_alttab_chars = 0;   // characters that fit in one row

// Strip geometry. The boot PSF1 font is 8x16, which sets every number here.
#define SWITCHER_PAD      8    // left/right breathing room inside a row
#define SWITCHER_ROW_H    24   // pitch: a 16px glyph cell with 4px above and below
#define SWITCHER_ROW_GAP  1    // see switcher_composite — the separator IS this gap
#define SWITCHER_BORDER   4    // strip frame around the column of rows
#define SWITCHER_MARGIN   16   // smallest gap the strip keeps from a screen edge
#define SWITCHER_ROW_MIN_W (8 * 8 + 2 * SWITCHER_PAD)   // eight characters, floor

// Size and centre the strip for the ring currently snapshotted.
//
// THE LIST IS VERTICAL, one window per row (ruled 2026-08-23 by Chris, who
// asked what was in the boxes and thereby found the answer). The horizontal
// row of tiles every desktop shows works because those tiles hold ICONS:
// square, small, sixteen across a screen without complaint. os64 has no
// icons — a window has a TITLE, and titles are wide and variable-length,
// which is the one shape a horizontal strip handles worst. The arithmetic
// is brutal: sixteen windows sharing 1024 pixels leaves 61 per cell, which
// at 8 pixels a glyph is FIVE CHARACTERS — "gterm", "scrib", "bounc" — and
// it gets worse the more descriptive the titles become, which is exactly
// the direction they are heading. The same sixteen rows stacked cost 384
// pixels of height, fit inside 640x480 with room to spare, and show all 32
// characters at a comfortable width.
//
// The lineage agrees. The vertical list of window titles is the older and
// specifically Unix answer — twm's window menu (1987), then fvwm's
// WindowList (1993) — arrived at by people who likewise had no icons to
// show; even Microsoft's pre-icon Task List (Alt+Esc, Windows 3.0) was a
// vertical list box, and went horizontal only once there were icons for it.
//
// ROWS ARE UNIFORM WIDTH, not sized to their own titles: surface_draw_text
// paints an OPAQUE background per glyph, so a title allowed to run past its
// row would repaint the frame beside it. Clipping to a character count
// computed from the row width makes the spill impossible rather than merely
// unlikely.
static void switcher_layout_locked(void)
{
	int32_t screen_w = (int32_t)kBackbuffer.width;
	int32_t screen_h = (int32_t)kBackbuffer.height;

	int32_t longest = 0;
	for (size_t i = 0; i < s_alttab_count; i++)
		if ((int32_t)s_alttab_title_len[i] > longest)
			longest = (int32_t)s_alttab_title_len[i];

	int32_t row_w = 8 * longest + 2 * SWITCHER_PAD;
	if (row_w < SWITCHER_ROW_MIN_W)
		row_w = SWITCHER_ROW_MIN_W;

	// A full-width row on a very narrow framebuffer: clip the titles rather
	// than run off the glass. (32 characters plus padding is 272px, so this
	// only bites below a 320-wide screen — the clamp is the belt to that
	// pair of suspenders.)
	int32_t available_w = screen_w - 2 * SWITCHER_BORDER - 2 * SWITCHER_MARGIN;
	if (row_w > available_w)
		row_w = available_w;

	s_alttab_row_w = row_w;
	s_alttab_chars = (row_w - 2 * SWITCHER_PAD) / 8;
	if (s_alttab_chars < 0)
		s_alttab_chars = 0;

	// Height: rows at SWITCHER_ROW_H pitch, the last one ending flush with
	// the frame (hence the one subtracted gap — a trailing separator would
	// read as lopsided padding).
	s_alttab_strip.w = row_w + 2 * SWITCHER_BORDER;
	s_alttab_strip.h = SWITCHER_ROW_H * (int32_t)s_alttab_count
	                 - SWITCHER_ROW_GAP + 2 * SWITCHER_BORDER;
	s_alttab_strip.x = (screen_w - s_alttab_strip.w) / 2;
	s_alttab_strip.y = (screen_h - s_alttab_strip.h) / 2;
	if (s_alttab_strip.x < 0)
		s_alttab_strip.x = 0;
	if (s_alttab_strip.y < 0)
		s_alttab_strip.y = 0;
}

// Republish the whole strip. The rubber band damages its four thin edges
// because it is window-sized and moves on every mouse packet; the strip is
// small, moves once per keystroke, and repaints its highlight in the MIDDLE
// of itself — so the union is both simpler and cheaper than the bookkeeping
// that would avoid it.
static void switcher_damage_locked(void)
{
	if (s_alttab_shown)
		gui_damage_add_locked(s_alttab_strip);
}

// Draw the strip into the damaged region. Same contract as band_composite:
// everything in VIEW coordinates, so surface.c's clipping makes "writes stay
// inside the damage rect" structural rather than remembered.
static void switcher_composite(surface_t *backbuffer, rect_t damage)
{
	if (!s_alttab_shown)
		return;

	surface_t view = surface_view(backbuffer, damage);
	int32_t sx = s_alttab_strip.x - damage.x;
	int32_t sy = s_alttab_strip.y - damage.y;

	// Frame: a filled slab in the unfocused border colour with the focused
	// border colour outlining it — the same two greys the chrome uses to say
	// "this one is live", one level up from a window.
	surface_fill_rect(&view, (rect_t){sx, sy, s_alttab_strip.w, s_alttab_strip.h},
	                  wm_border_color(false));
	surface_draw_rect(&view, (rect_t){sx, sy, s_alttab_strip.w, s_alttab_strip.h},
	                  wm_border_color(true));

	// NO SEPARATOR LINES. Each row is a tile laid on the dark slab and one
	// pixel shorter than its pitch, so the frame itself shows through between
	// them: the gap IS the separator. It costs no colour, no extra draw and
	// no decision about how dark a divider should be, and it makes the
	// highlighted row read as a raised tile rather than a painted stripe —
	// which still works when two adjacent rows are the same grey.
	for (size_t i = 0; i < s_alttab_count; i++) {
		rect_t row = {sx + SWITCHER_BORDER,
		              sy + SWITCHER_BORDER + (int32_t)i * SWITCHER_ROW_H,
		              s_alttab_row_w, SWITCHER_ROW_H - SWITCHER_ROW_GAP};
		uint32_t bg = (i == s_alttab_step) ? wm_titlebar_color(true)
		                                   : wm_titlebar_color(false);
		surface_fill_rect(&view, row, bg);

		// A MINIMIZED WINDOW IS IN THE RING AND DRAWN DIM. That is the whole
		// point of the slice: "bring back the one I hid" has to be a visible
		// choice rather than a guess, and dim-but-present says both halves —
		// it is here, and it is not on the glass.
		int32_t len = (int32_t)s_alttab_title_len[i];
		if (len > s_alttab_chars)
			len = s_alttab_chars;
		if (len > 0)
			surface_draw_text(&view, row.x + SWITCHER_PAD,
			                  row.y + (SWITCHER_ROW_H - SWITCHER_ROW_GAP - 16) / 2,
			                  s_alttab_titles[i], (size_t)len,
			                  // LIGHT grey, not GUI_COLOR_GRAY: the dim title
			                  // has to stay legible on BOTH row colours, and
			                  // its worst case is the one that matters most —
			                  // a minimized window that is also the highlighted
			                  // one, i.e. the window you are in the act of
			                  // choosing to bring back. 0x808080 on the
			                  // focused blue was muddy; this reads clearly and
			                  // is still obviously not white.
			                  s_alttab_dim[i] ? GUI_COLOR_LIGHT_GRAY : GUI_COLOR_WHITE,
			                  bg);
	}
}

// Take the snapshot the strip draws from: recency order, titles, dim flags.
static void alttab_snapshot_locked(void)
{
	s_alttab_count = wm_recency_ids(s_alttab_ring, ALTTAB_RING_MAX);
	for (size_t i = 0; i < s_alttab_count; i++) {
		window_t *w = wm_window_by_id(s_alttab_ring[i]);
		size_t n = 0;
		if (w != NULL) {
			while (n < GUI_WINDOW_TITLE_MAX && w->title[n] != '\0') {
				s_alttab_titles[i][n] = w->title[n];
				n++;
			}
			s_alttab_dim[i] = wm_is_hidden(w);
		} else {
			s_alttab_dim[i] = false;   // cannot happen: the ids came from the live list
		}
		s_alttab_title_len[i] = (uint8_t)n;
	}
}

static void alttab_step_locked(bool backwards)
{
	bool first = !s_alttab_active;
	if (first) {
		alttab_snapshot_locked();
		s_alttab_step = 0;
		s_alttab_active = true;
		if (s_alttab_count >= 2) {
			switcher_layout_locked();
			s_alttab_shown = true;
		}
	}
	if (s_alttab_count < 2)
		return;   // nothing to cycle between; the hold still starts, harmlessly

	// THE FIRST STEP FROM THE WALLPAPER LANDS ON THE MOST RECENT APP (Codex
	// #31 rd3). The ring never lists the desktop, so when the desktop holds
	// focus the focused window is absent from the snapshot and entry 0 is
	// not "where you already are" — it is the app you most recently used.
	// Advancing past it, as every other first step does, skipped that app
	// and offered the second one. When the focus is not in the ring, the
	// first step stays on 0.
	if (first) {
		window_t *focus = wm_focus_root(wm_focused());
		if (focus == NULL || s_alttab_ring[0] != focus->id)
			goto chosen;
	}
	s_alttab_step = backwards
		? (s_alttab_step + s_alttab_count - 1) % s_alttab_count
		: (s_alttab_step + 1) % s_alttab_count;

chosen:
	// A STEP MOVES THE HIGHLIGHT AND NOTHING ELSE. No raise, no restore, no
	// focus change, no recency stamp — the z-order the user had is the
	// z-order they keep until they let go, and exactly one window changes at
	// the end of the hold. (PASSING THROUGH IS NOT USING — Chris's nit, the
	// first hour Alt+Tab existed on the P5 — used to need a save/restore of
	// focusSerial around the raise. Now it needs nothing, because nothing
	// happens.)
	printd(DEBUG_GUI, "guicomp: alt-tab step %lu/%lu -> window %u\n",
		(uint64_t)s_alttab_step, (uint64_t)s_alttab_count,
		s_alttab_ring[s_alttab_step]);
	switcher_damage_locked();
}

// The hold is over. `commit` false = Escape was pressed: the strip goes and
// the scene is left exactly as it was found, which is the promise that makes
// walking the ring safe to do idly.
//
// Committing acts on the HIGHLIGHTED window, not on whatever holds focus:
// nothing moved focus during the hold, so the highlight is the only record of
// what the user chose. Restore it if it was minimized, raise it otherwise —
// both paths focus and stamp recency through wm_raise, so the window you
// released on becomes the most recent and the quick Alt+Tab that follows
// bounces back to where you came from.
static void alttab_end_locked(bool commit)
{
	s_alttab_active = false;
	if (s_alttab_shown) {
		s_alttab_shown = false;
		gui_damage_add_locked(s_alttab_strip);   // erase: repaint what was under it
	}

	if (commit && s_alttab_count > 0) {
		window_t *w = wm_window_by_id(s_alttab_ring[s_alttab_step]);
		if (w == NULL) {
			// Chosen window died mid-hold. Nothing to switch to, and nothing
			// to clean up — the id was the whole of our hold on it.
			printd(DEBUG_GUI, "guicomp: alt-tab chose window %u, which is gone\n",
				s_alttab_ring[s_alttab_step]);
		} else if (wm_is_hidden(w)) {
			wm_set_minimized(w, false);   // back on the glass, raised, focused
		} else {
			wm_raise(w);
		}
	}
	printd(DEBUG_GUI, "guicomp: alt-tab hold %s\n",
		commit ? "ended" : "cancelled");
}

// ── Interactive resize: the Ctrl+Alt gesture and its rubber band ────────────
//
// THE GESTURE (ruled 2026-08-19). Ctrl+Alt held: left-drag moves the window,
// right-drag resizes it — anywhere in the window, no handle to hit. This is
// fvwm's bargain, and before fvwm it was uwm's and mwm's: the pointer plus a
// modifier is a complete window-management vocabulary, so the chrome does not
// have to grow targets for it. os64 gets three things out of choosing it
// first: no chrome change at all (composite_one is untouched), no 1-pixel
// border to hit (the border stays a hairline because nobody has to grab it),
// and it still works on a window dragged half off the screen — the case
// corner grips famously cannot reach. Ctrl+Alt rather than plain Alt because
// Alt belongs to the terminal stack here (Alt+arrows, Alt+F1..F8 switch VTs
// and are consumed before any of this); GRAPHICS.md's VT8 ruling named
// Ctrl+Alt as the escape hatch for exactly this day.
//
// THE BAND. A resize shows a wireframe outline and a live size readout; the
// window itself does not change until the button comes up. That is twm's
// 1987 look, and the reason to start there is not nostalgia: the app receives
// exactly ONE resize event per gesture instead of one per mouse packet, so a
// program that repaints slowly cannot be dragged into a repaint storm it will
// never catch up with. Opaque (live) resize is the natural upgrade once
// somebody wants it — it is the same commit, just called from the MOVE arm
// instead of the UP arm.
//
// The readout reports CONTENT size, not frame size: the client area is what
// an app actually gets, and for a terminal it is the number that matters.
static window_t *s_band_window = NULL;   // the window being rubber-banded
// Buttons owned by a resize or cancelled WM move. Ownership outlives the
// outline/target; draining releases needs no live window pointer.
static uint8_t s_wm_buttons;
static rect_t    s_band_rect;            // the outline, screen coords, as drawn
static rect_t    s_band_origin;          // its frame when the gesture began
static int32_t   s_band_anchor_x, s_band_anchor_y;   // where the press landed
// Which edges follow the pointer, chosen from the quadrant the press landed
// in: grab the left half and the LEFT edge moves (so the right edge stays
// pinned), grab the top half and the top edge moves. mwm's rule, and the one
// that makes "drag toward the corner you grabbed" mean what it looks like.
static bool      s_band_west, s_band_north;

#define BAND_THICKNESS   2
#define BAND_COLOR       GUI_COLOR_WHITE
// The top strip of the outline is damaged this tall rather than
// BAND_THICKNESS tall, because the size readout is drawn inside it.
#define BAND_LABEL_STRIP 24

// Minimal unsigned-to-decimal, because the band needs a string and the
// kernel's formatter writes to the log rather than to a buffer. Returns the
// length written; the caller sizes `out` (a 32-bit pixel count is 10 digits).
static size_t band_utoa(uint32_t v, char *out)
{
	char tmp[12];
	size_t n = 0;
	do {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v != 0);
	for (size_t i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

// Damage the four edges of an outline instead of the rectangle it encloses.
// The whole point of the band is that it costs four thin strips per frame
// rather than one window-sized recomposite; unioning them here would hand
// back exactly the cost we are avoiding.
static void band_damage_locked(rect_t b)
{
	if (rect_is_empty(b))
		return;
	gui_damage_add_locked((rect_t){b.x, b.y, b.w, BAND_LABEL_STRIP});
	gui_damage_add_locked((rect_t){b.x, b.y + b.h - BAND_THICKNESS, b.w, BAND_THICKNESS});
	gui_damage_add_locked((rect_t){b.x, b.y, BAND_THICKNESS, b.h});
	gui_damage_add_locked((rect_t){b.x + b.w - BAND_THICKNESS, b.y, BAND_THICKNESS, b.h});
}

// Draw the band into the damaged region. Same contract as composite_one:
// everything is done in VIEW coordinates so surface.c's clipping makes
// "writes stay inside the damage rect" structural rather than remembered.
static void band_composite(surface_t *backbuffer, rect_t damage)
{
	if (s_band_window == NULL)
		return;

	surface_t view = surface_view(backbuffer, damage);
	rect_t b = {s_band_rect.x - damage.x, s_band_rect.y - damage.y,
	            s_band_rect.w, s_band_rect.h};

	for (int32_t i = 0; i < BAND_THICKNESS; i++)
		surface_draw_rect(&view,
		                  (rect_t){b.x + i, b.y + i, b.w - 2 * i, b.h - 2 * i},
		                  BAND_COLOR);

	// "640x480" — the CONTENT the client will be handed, derived with the
	// same chrome inset wm_resize will apply to the frame we commit.
	char label[24];
	size_t len = 0;
	int32_t content_w = s_band_rect.w - 2 * wm_border_width(s_band_window->flags);
	int32_t content_h = s_band_rect.h - wm_chrome_top(s_band_window->flags) - wm_border_width(s_band_window->flags);
	if (content_w < 0) content_w = 0;
	if (content_h < 0) content_h = 0;
	len += band_utoa((uint32_t)content_w, label + len);
	label[len++] = 'x';
	len += band_utoa((uint32_t)content_h, label + len);
	surface_draw_text(&view, b.x + BAND_THICKNESS + 4, b.y + BAND_THICKNESS + 3,
	                  label, len, GUI_COLOR_WHITE, GUI_COLOR_BLACK);
}

// Recompute the outline from the current cursor position and republish the
// damage. Called on every MOUSE_MOVE while a band is up.
static void band_track_locked(int32_t x, int32_t y)
{
	int32_t dx = x - s_band_anchor_x;
	int32_t dy = y - s_band_anchor_y;

	rect_t f = s_band_origin;
	// A west/north edge moving means the ORIGIN moves and the extent shrinks
	// by the same amount — the opposite corner stays exactly where it is,
	// which is the entire visual promise of grabbing a corner.
	if (s_band_west) {
		f.x += dx;
		f.w -= dx;
	} else {
		f.w += dx;
	}
	if (s_band_north) {
		f.y += dy;
		f.h -= dy;
	} else {
		f.h += dy;
	}

	// Preview EXACTLY what the commit will do — same function, so the outline
	// cannot promise a size wm_resize would then clamp away.
	rect_t clamped = wm_clamp_frame(s_band_window, f);
	// Clamping shrinks from the far edge; when the near edge is the one
	// moving, re-pin the far edge so the window grows out of the corner the
	// user grabbed instead of sliding.
	if (s_band_west)
		clamped.x = s_band_origin.x + s_band_origin.w - clamped.w;
	if (s_band_north)
		clamped.y = s_band_origin.y + s_band_origin.h - clamped.h;

	if (clamped.x == s_band_rect.x && clamped.y == s_band_rect.y &&
	    clamped.w == s_band_rect.w && clamped.h == s_band_rect.h)
		return;   // sub-pixel wobble inside a clamp: nothing to redraw

	band_damage_locked(s_band_rect);   // erase where it was
	s_band_rect = clamped;
	band_damage_locked(s_band_rect);   // draw where it is
}

// THE IMPLICIT POINTER GRAB (2026-08-21). While a button is down on a
// client's content area, that client owns the pointer: every move and the
// release go to IT, wherever the cursor has wandered to.
//
// The WM's own gestures had this from birth (s_drag_window, s_band_window,
// and the comment above them says why: "a gesture in progress OWNS the
// pointer... losing this is how a drag ends up half-delivered to three
// different windows"). Clients did not, and hit-testing every event is not
// the same promise: a drag-select that leaves the window lost its tail, and
// the BUTTON_UP that ends the gesture was delivered to whatever happened to
// be under the cursor — a phantom release for a stranger, and no release at
// all for the app that was mid-drag. scribe wore that as a quirk for a day;
// gterm's select-is-copy would have worn it as "the copy sometimes doesn't
// happen", which is worse because it is silent.
//
// X11 named this in 1987 — the IMPLICIT PASSIVE GRAB: a button press grabs
// the pointer for the client until the last button comes up. Every window
// system since has one, because every window system without one grew this
// same bug.
static os64_decor_capture_t s_control_capture;
static uint32_t s_control_hover;

void gui_cancel_decoration(const struct window *w)
{
	if (!w || s_control_capture.window==w->id) {
		window_t *pressed=wm_window_by_id(s_control_capture.window);
		if (pressed) wm_decoration_state(pressed,0,0);
		os64_decor_capture_cancel(&s_control_capture);
	}
	if (!w || s_control_hover==w->id) {
		window_t *hover=wm_window_by_id(s_control_hover);
		if (hover) wm_decoration_state(hover,0,0);
		s_control_hover=0;
	}
}

static window_t *s_pointer_window = NULL;
static window_t *s_hover_window = NULL;
static int32_t s_hover_x, s_hover_y;
static int32_t s_hover_w, s_hover_h;

static void control_state_locked(void)
{
	if (!gui_owns_glass() || s_glass_regained) gui_cancel_decoration(NULL);
	window_t *under=NULL;
	uint32_t hit=0;
	if (gui_owns_glass() && !s_drag_window && !s_wm_buttons && !s_pointer_window) {
		under=wm_topmost_at(s_cursor_x,s_cursor_y);
		if (under) hit=wm_decoration_hit(under,s_cursor_x,s_cursor_y);
		if (under && (wm_decoration_disabled(under) & (1u<<hit))) hit=0;
	}
	if (s_control_capture.buttons) {
		(void)os64_decor_capture_step(&s_control_capture,OS64_DECOR_POINTER_MOVE,0,
			under?under->id:0,hit);
		if (!s_control_capture.armed) hit=0;
	}
	uint32_t id=hit && under?under->id:0;
	if (s_control_hover && s_control_hover!=id) {
		window_t *old=wm_window_by_id(s_control_hover);
		if (old) wm_decoration_state(old,0,0);
	}
	if (id) wm_decoration_state(under,hit,s_control_capture.armed?hit:0);
	s_control_hover=id;
}

// Reconcile against the final scene each frame, including geometry changes
// with no mouse motion. Grabs suppress hover in other windows; WM gestures
// and text VTs have no client hover target. kGuiLock protects this and teardown.
static void pointer_state_locked(void)
{
	window_t *under = NULL;
	int32_t x = 0, y = 0;
	int32_t width = 0, height = 0;
	if (gui_owns_glass() && !s_drag_window && !s_wm_buttons && !s_control_capture.buttons) {
		under = wm_topmost_at(s_cursor_x, s_cursor_y);
		if (under && s_pointer_window && under != s_pointer_window) under = NULL;
		if (under) {
			rect_t content = wm_content_rect_on_screen(under);
			if (!rect_contains_point(content, s_cursor_x, s_cursor_y)) under = NULL;
			else {
				x = s_cursor_x - content.x; y = s_cursor_y - content.y;
				width = content.w; height = content.h;
			}
		}
	}
	if (under != s_hover_window && s_hover_window) {
		input_event_t leave = { .type = INPUT_EVENT_POINTER_STATE,
		    .pointer = { .inside = 0 }, .tick = kTicksSinceStart };
		wm_deliver_event(s_hover_window, &leave);
	}
	if (under && (under != s_hover_window || x != s_hover_x || y != s_hover_y ||
	              width != s_hover_w || height != s_hover_h)) {
		input_event_t enter = { .type = INPUT_EVENT_POINTER_STATE,
		    .pointer = { .x = x, .y = y, .inside = 1 }, .tick = kTicksSinceStart };
		wm_deliver_event(under, &enter);
	}
	s_hover_window = under;
	s_hover_x = x;
	s_hover_y = y;
	s_hover_w = width;
	s_hover_h = height;
}

void gui_grab_release(const struct window *w)
{
	gui_cancel_decoration(w);
	// Called from wm_destroy under kGuiLock while the window is still linked
	// and alive. Clear its control state before releasing pointer references.
	if (s_drag_window == (const window_t *)w) {
		s_wm_buttons |= s_drag_buttons;
		s_drag_window = NULL;
	}
	if (s_pointer_window == (const window_t *)w)
		s_pointer_window = NULL;   // a window that dies mid-drag lets go
	if (s_hover_window == (const window_t *)w)
		s_hover_window = NULL;
	gui_cancel_resize(w);
}

void gui_cancel_resize(const struct window *w)
{
	if (s_band_window == (const window_t *)w) {
		band_damage_locked(s_band_rect);
		s_band_window = NULL;
		// Keep s_wm_buttons: its matching releases still belong to us.
	}
}

// Install a new frame geometry without retaining a gesture's old offsets.
// Client grabs remain paired; WM-owned edges drain after cancellation.
void gui_decoration_changed(void)
{
	gui_cancel_decoration(NULL);
	if (s_drag_window) {
		placement_complete_locked(s_drag_window, false);
		s_wm_buttons |= s_drag_buttons;
		s_drag_window = NULL;
	}
	if (s_band_window) gui_cancel_resize(s_band_window);
	s_titlebar_click_window=0;
}

void gui_geometry_changing(const struct window *w)
{
	if (s_drag_window == w || s_band_window == w) gui_decoration_changed();
}

// Deliver a mouse event to a window, in ITS coordinates. `require_inside`
// is the difference between routing and grabbing: a hit-tested event must
// land in the content area (chrome clicks are the window system's business,
// not the client's), while a GRABBED event is delivered wherever the pointer
// has got to — including coordinates outside the window, which is exactly
// what an app tracking a drag past its own edge needs to see. Returns
// whether the client got it.
static bool deliver_mouse_to_window(window_t *w, input_event_t ev,
                                    bool require_inside)
{
	rect_t content = wm_content_rect_on_screen(w);
	if (require_inside && !rect_contains_point(content, ev.mouse.x, ev.mouse.y))
		return false;
	ev.mouse.x -= content.x;
	ev.mouse.y -= content.y;
	wm_deliver_event(w, &ev);
	return true;
}

// Is the window-management chord held? BOTH modifiers, so a plain Ctrl-click
// or Alt-click still reaches the app underneath — the chord has to be
// deliberate. Decoration buttons are handled before this chord; elsewhere
// it overrides the client's interpretation of the click.
static inline bool wm_chord_held(const input_event_t *ev)
{
	const uint8_t both = KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT;
	return (ev->mouse.modifiers & both) == both;
}

// Report a chord's OUTCOME, not its intent. The wm_ setters are free to
// decline — a desktop, a popup — and a line printed BEFORE the call claimed
// a change that never happened, leaving "window 2 maximized" sitting
// directly above "maximize declined". Nothing is logged when nothing moved.
static void chord_report(const window_t *w, uint32_t bit, bool before,
                         const char *on, const char *off)
{
	bool now = (w->flags & bit) != 0;
	if (now != before)
		printd(DEBUG_GUI, "guicomp: window %u %s\n", w->id, now ? on : off);
}

static void route_event_locked(const input_event_t *ev)
{
	// Finalize a live move before a text-VT event can consume its release.
	if (!gui_owns_glass() || s_glass_regained) gui_decoration_changed();
	if (s_drag_window) {
		if (ev->type == INPUT_EVENT_MOUSE_BUTTON_DOWN) s_drag_buttons |= (uint8_t)(1u << ev->mouse.button);
		if (ev->type == INPUT_EVENT_MOUSE_BUTTON_UP) s_drag_buttons &= (uint8_t)~(1u << ev->mouse.button);
	}
	// Track the physical pointer on text VTs too, so returning to the GUI
	// can reconcile hover without waiting for another movement.
	if (ev->type == INPUT_EVENT_MOUSE_MOVE) {
		rect_t moved = cursor_rect();
		s_cursor_x = ev->mouse.x;
		s_cursor_y = ev->mouse.y;
		gui_damage_add_locked(rect_union(moved, cursor_rect()));
	}
	if (!gui_owns_glass() || s_glass_regained) gui_cancel_decoration(NULL);
	if (s_control_capture.buttons && (ev->type==INPUT_EVENT_MOUSE_MOVE ||
		ev->type==INPUT_EVENT_MOUSE_BUTTON_DOWN || ev->type==INPUT_EVENT_MOUSE_BUTTON_UP)) {
		uint32_t owner=s_control_capture.window;
		window_t *under=wm_topmost_at(ev->mouse.x,ev->mouse.y);
		uint32_t hit=under?wm_decoration_hit(under,ev->mouse.x,ev->mouse.y):0;
		if (under && (wm_decoration_disabled(under) & (1u<<hit))) hit=0;
		uint32_t kind=ev->type==INPUT_EVENT_MOUSE_MOVE?OS64_DECOR_POINTER_MOVE:
			ev->type==INPUT_EVENT_MOUSE_BUTTON_DOWN?OS64_DECOR_POINTER_DOWN:OS64_DECOR_POINTER_UP;
		uint32_t action=os64_decor_capture_step(&s_control_capture,kind,ev->mouse.button,
			under?under->id:0,hit);
		window_t *w=wm_window_by_id(owner);
		if (w) wm_decoration_state(w,0,0);
		if (action && w) wm_decoration_action(w,action,ev->tick);
		control_state_locked();
		return;
	}
	if (s_wm_buttons && (ev->type == INPUT_EVENT_MOUSE_MOVE ||
	                        ev->type == INPUT_EVENT_MOUSE_BUTTON_DOWN ||
	                        ev->type == INPUT_EVENT_MOUSE_BUTTON_UP)) {
		// Drain individual edges, not mouse.buttons: one physical report can
		// enqueue several releases with the same final (empty) button mask.
		if (ev->type == INPUT_EVENT_MOUSE_BUTTON_DOWN)
			s_wm_buttons |= (uint8_t)(1u << ev->mouse.button);
		else if (ev->type == INPUT_EVENT_MOUSE_BUTTON_UP)
			s_wm_buttons &= (uint8_t)~(1u << ev->mouse.button);
		// Cancellation removes the outline, not ownership of its press. Do
		// this before the VT fork so a release on a text VT is consumed too.
		if (!s_band_window || !gui_owns_glass()) {
			if (s_band_window) gui_cancel_resize(s_band_window);
			return;
		}
		// Additional button presses were consumed during this resize; their
		// releases must not fall through to a client while the band is live.
		if (ev->type == INPUT_EVENT_MOUSE_BUTTON_UP &&
		    ev->mouse.button != INPUT_MOUSE_BUTTON_RIGHT)
			return;
	}
	// THE INPUT FORK (2026-08-21). Mouse events belong to whoever holds the
	// glass: the window system when VT8 is up, the focused TEXT TERMINAL
	// otherwise — where they become gpm's old gesture, select-to-copy and
	// right-click-to-paste (vt_select.c). Keys are NOT forked here: the
	// keyboard driver routes those at its own end, tty by tty, and always
	// did. Only the pointer had nowhere to go.
	if (!gui_owns_glass()) {
		switch (ev->type) {
		case INPUT_EVENT_MOUSE_WHEEL:
			return;   // GUI wheel queued before a VT switch; text routes at arrival
		case INPUT_EVENT_MOUSE_MOVE:
		case INPUT_EVENT_MOUSE_BUTTON_DOWN:
		case INPUT_EVENT_MOUSE_BUTTON_UP:
			vtsel_mouse_event(ev);   // state only; painting waits for the frame
			return;
		default:
			break;
		}
	}

	switch (ev->type) {
	case INPUT_EVENT_MOUSE_WHEEL: {
		// Window-management gestures own the pointer but do not scroll.
		if (s_band_window || s_drag_window || s_wm_buttons || s_control_capture.buttons)
			break;
		if (s_pointer_window)
			deliver_mouse_to_window(s_pointer_window, *ev, false);
		else {
			window_t *under = wm_topmost_at(ev->mouse.x, ev->mouse.y);
			if (under) deliver_mouse_to_window(under, *ev, true);
		}
		break;
	}
	case INPUT_EVENT_MOUSE_MOVE: {
		// A gesture in progress OWNS the pointer: no hit-testing, no delivery
		// to whatever the cursor happens to sweep across. Losing this is how
		// a drag ends up half-delivered to three different windows.
		if (s_band_window)
			band_track_locked(ev->mouse.x, ev->mouse.y);
		else if (s_drag_window)
			wm_move(s_drag_window,
			        ev->mouse.x - s_drag_dx, ev->mouse.y - s_drag_dy);
		else if (s_pointer_window)
			deliver_mouse_to_window(s_pointer_window, *ev, false);
		else {
			window_t *under = wm_topmost_at(ev->mouse.x, ev->mouse.y);
			if (under)
				deliver_mouse_to_window(under, *ev, true);
		}
		break;
	}
	case INPUT_EVENT_MOUSE_BUTTON_DOWN: {
		if (s_band_window || s_drag_window)
			break;   // a second button during a gesture is not a new gesture
		if (s_pointer_window) {
			// Additional buttons belong to the existing client gesture.
			deliver_mouse_to_window(s_pointer_window, *ev, false);
			break;
		}

		window_t *w = wm_topmost_at(ev->mouse.x, ev->mouse.y);
		if (!w)
			break;   // no window contains this point, so there is nothing to
			         // raise and nobody to deliver to. While the desktop
			         // shell runs, ITS window answers for bare wallpaper —
			         // this is the case where the glass is truly empty.
		// The modifiers are logged because a chord that does not fire looks
		// exactly like a chord that was never held — this line is the
		// difference between those two, and it costs one printd per click.
		printd(DEBUG_GUI, "guicomp: button %u down on window %u at (%d,%d), mods 0x%02x\n",
			ev->mouse.button, w->id, ev->mouse.x, ev->mouse.y, ev->mouse.modifiers);
		wm_raise(w);

		uint32_t action=wm_decoration_hit(w,ev->mouse.x,ev->mouse.y);
		if (action) {
			s_titlebar_click_window=0;
			uint32_t accepted=ev->mouse.button==INPUT_MOUSE_BUTTON_LEFT &&
				!(wm_decoration_disabled(w) & (1u<<action))?action:0;
			os64_decor_capture_begin(&s_control_capture,w->id,accepted,
				ev->mouse.buttons | (1u<<ev->mouse.button));
			if (!accepted) os64_decor_capture_cancel(&s_control_capture);
			control_state_locked();
			break;
		}

		// The chord's two verbs (see the gesture comment above the band
		// state): left moves, right resizes, anywhere in the window.
		//
		// NOT ON THE DESKTOP. The wallpaper is a window, so without this
		// check Ctrl+Alt+drag on empty desktop MOVED it, and the right-button
		// chord shrank it to reveal the test pattern around it (Fable's
		// review, 2026-08-25). Move and resize have no wm_ setter of their
		// own to guard — wm_move/wm_resize are the WM's internal verbs and
		// the chord is their only outside caller — so the decline lives at
		// the gesture, the same way Alt+F4's does.
		if (wm_chord_held(ev) && (w->flags & GUI_WINDOW_DESKTOP)) {
			printd(DEBUG_GUI, "guicomp: chord move/resize declined — window %u is the desktop\n",
				w->id);
		} else if (wm_chord_held(ev)) {
			if (ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT) {
				s_drag_window = w;
				placement_begin_locked(w);
				s_drag_buttons = ev->mouse.buttons;
				s_drag_dx = ev->mouse.x - w->frame.x;
				s_drag_dy = ev->mouse.y - w->frame.y;
			} else if (ev->mouse.button == INPUT_MOUSE_BUTTON_RIGHT) {
				s_band_window = w;
				placement_begin_locked(w);
				s_wm_buttons = ev->mouse.buttons;
				s_band_origin = w->frame;
				s_band_rect   = w->frame;
				s_band_anchor_x = ev->mouse.x;
				s_band_anchor_y = ev->mouse.y;
				// The quadrant picks the edges: grabbing in the left half
				// moves the left edge, the top half moves the top edge.
				s_band_west  = (ev->mouse.x - w->frame.x) < w->frame.w / 2;
				s_band_north = (ev->mouse.y - w->frame.y) < w->frame.h / 2;
				band_damage_locked(s_band_rect);   // paint the initial outline
				printd(DEBUG_GUI, "guicomp: band resize on window %u from %dx%d (%s%s corner)\n",
					w->id, w->frame.w, w->frame.h,
					s_band_north ? "N" : "S", s_band_west ? "W" : "E");
			}
			break;
		}

		if (ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT &&
		    wm_point_in_titlebar(w, ev->mouse.x, ev->mouse.y)) {
			// DOUBLE-CLICK ON THE TITLEBAR MAXIMIZES (and restores) — the
			// 1990 habit, kept because it is the one thing everyone tries
			// first. Two presses on the SAME window's bar within the
			// double-click window make the second one a toggle instead of
			// a drag. Windows' default interval has been 500ms since 3.0.
			if (w->id == s_titlebar_click_window &&
			    ev->tick - s_titlebar_click_tick <= DOUBLE_CLICK_TICKS) {
				s_titlebar_click_window = 0;   // a third click starts over
				s_wm_buttons=ev->mouse.buttons | (1u<<ev->mouse.button);
				bool was = (w->flags & GUI_WINDOW_MAXIMIZED) != 0;
				wm_decoration_action(w,OS64_DECOR_MAXIMIZE,ev->tick);
				if (((w->flags & GUI_WINDOW_MAXIMIZED) != 0) != was)
					printd(DEBUG_GUI, "guicomp: titlebar double-click: window %u %s\n",
						w->id, was ? "restored" : "maximized");
				break;
			}
			s_titlebar_click_window = w->id;
			s_titlebar_click_tick = ev->tick;
			// Grab for dragging; remember where in the frame we grabbed so
			// the window doesn't jump under the cursor.
			s_drag_window = w;
			placement_begin_locked(w);
			s_drag_buttons = ev->mouse.buttons;
			s_drag_dx = ev->mouse.x - w->frame.x;
			s_drag_dy = ev->mouse.y - w->frame.y;
		} else if (deliver_mouse_to_window(w, *ev, true)) {
			// It landed in the client's own area, so the grab begins here —
			// and only here: a press on chrome belongs to the window system,
			// and grabbing for it would hand the client a release it never
			// asked for.
			s_pointer_window = w;
		}
		break;
	}
	case INPUT_EVENT_MOUSE_BUTTON_UP: {
		// The band commits on release — one wm_resize, therefore one resize
		// event for the app, no matter how far the pointer travelled. The
		// chord is deliberately NOT re-checked here: the gesture belongs to
		// the button that started it, so letting go of Ctrl+Alt mid-drag
		// finishes the resize instead of abandoning a window at whatever size
		// the outline happened to be. (Every WM that got this wrong taught
		// its users to release the mouse first, which is not a lesson worth
		// teaching.)
		if (s_band_window && ev->mouse.button == INPUT_MOUSE_BUTTON_RIGHT) {
			window_t *w = s_band_window;
			rect_t final = s_band_rect;
			band_damage_locked(s_band_rect);   // erase the outline
			s_band_window = NULL;
			uint32_t maximized = w->flags & GUI_WINDOW_MAXIMIZED;
			w->flags &= ~GUI_WINDOW_MAXIMIZED;
			if (wm_resize(w, final)) placement_complete_locked(w, true);
			else w->flags |= maximized;
			break;
		}
		if (s_drag_window && ev->mouse.button == INPUT_MOUSE_BUTTON_LEFT) {
			placement_complete_locked(s_drag_window, false);
			s_drag_window = NULL;
			break;
		}
		if (s_pointer_window) {
			// The grab holder gets its release even if the cursor left the
			// window — that release is the END of its gesture, and an app
			// that never hears it is an app stuck mid-drag forever.
			deliver_mouse_to_window(s_pointer_window, *ev, false);
			if (ev->mouse.buttons == 0)
				s_pointer_window = NULL;   // last button up: the grab ends
			break;
		}
		window_t *under = wm_topmost_at(ev->mouse.x, ev->mouse.y);
		if (under)
			deliver_mouse_to_window(under, *ev, true);
		break;
	}
	case INPUT_EVENT_KEY_DOWN:
	case INPUT_EVENT_KEY_UP: {
		printd(DEBUG_GUI | DEBUG_DETAILED,
			"guicomp: key %s sc 0x%02x ascii 0x%02x mods 0x%02x\n",
			ev->type == INPUT_EVENT_KEY_DOWN ? "down" : "up  ",
			ev->key.scancode, (uint8_t)ev->key.ascii, ev->key.modifiers);

		// The window system's own chords are taken BEFORE delivery, so an
		// app never sees half a gesture. Alt+Tab is the whole of that set
		// today (Alt+F1..F8 and Alt+arrows never reach here — the keyboard
		// driver consumes them for the VT switch).
		//
		// TWO DIALECTS, ONE TEST (the chord-publish lesson of 2026-08-21,
		// re-learned here the same day Alt+Tab shipped: it worked in QEMU's
		// PS/2 and did nothing on the P5's USB keyboard). The scancode field
		// is PS/2 set-1 on one path and a HID usage on the other — Tab is
		// 0x0F there and 0x2B here — so the key is recognized by its ASCII,
		// which both paths translate identically. And the hold ends not on
		// "the Alt key's release event" (PS/2 never delivers one for Right
		// Alt, HID delivered none for either until today) but on the first
		// key event of any kind that arrives WITHOUT Alt in its modifiers —
		// a fact both drivers publish on every event they send.
		if (s_alttab_active && !(ev->key.modifiers & KEYBOARD_MOD_ALT))
			alttab_end_locked(true);
		// Escape abandons the hold: the strip goes away and the scene is left
		// exactly as it was found. Swallowed, since the app never saw the
		// press that opened the strip either — half a gesture is worse than
		// none. (Only while a hold is running; Escape is the app's otherwise.)
		//
		// THE SCANCODE TEST IS LOAD-BEARING, not belt-and-braces: every arrow
		// press arrives as a three-event VT100 burst whose first event also
		// carries ascii 0x1B, so `ascii == 0x1B` alone would cancel the hold
		// on the first third of the Up key immediately below. See
		// keyboard_is_escape_key for the whole trap.
		//
		// The cancel happens on the key-DOWN (the strip should go the
		// instant you say so), which leaves its key-UP arriving after the
		// hold is already over — and an app handed the release half of a
		// press it never saw is exactly the "half a gesture" this router
		// swallows Tab's release to avoid. One flag closes it, rather than
		// eating Alt+Esc unconditionally and quietly taking a chord away
		// from every app that might want it.
		if (ev->key.ascii == 0x1b &&
		    keyboard_is_escape_key(ev->key.scancode, ev->key.modifiers) &&
		    (s_alttab_active || s_alttab_eat_esc_up)) {
			if (ev->type == INPUT_EVENT_KEY_DOWN) {
				if (s_alttab_active) {
					alttab_end_locked(false);
					s_alttab_eat_esc_up = true;
				}
			} else {
				s_alttab_eat_esc_up = false;
			}
			break;
		}
		// Up/Down walk the list too, because on a VERTICAL list they are what
		// the keys look like they should do. They are available precisely
		// because the driver spends Alt+Left/Right on the virtual-terminal
		// cycle and leaves the vertical pair alone — horizontal arrows walk
		// terminals, vertical arrows walk windows.
		//
		// Arrows do not START a hold (Tab does): an Alt+Up that reached no
		// switcher would be silently stolen from whatever app wanted it.
		if (s_alttab_active) {
			int arrow = keyboard_arrow_updown(ev->key.scancode, ev->key.modifiers);
			if (arrow != 0) {
				// One press is three events (ESC, '[', 'A'/'B'). Step on the
				// last of them so a press is a step, and swallow all three so
				// no app is ever handed half an escape sequence.
				if (ev->type == INPUT_EVENT_KEY_DOWN &&
				    (ev->key.ascii == 'A' || ev->key.ascii == 'B'))
					alttab_step_locked(arrow < 0);
				break;
			}
		}
		if (ev->key.ascii == '\t' && (ev->key.modifiers & KEYBOARD_MOD_ALT)) {
			if (ev->type == INPUT_EVENT_KEY_DOWN)
				alttab_step_locked((ev->key.modifiers & KEYBOARD_MOD_SHIFT) != 0);
			break;   // the Tab's release is swallowed too: the app never saw the press
		}

		// Alt+F4: close (1990 again, and the reason the VT switch now wants
		// Ctrl under the GUI). The first press is a REQUEST to the owner —
		// INPUT_EVENT_WINDOW_CLOSE on its queue, because the window is the
		// app's and an editor with unsaved work gets to answer. The second
		// press within GUI_CLOSE_ESCALATE_TICKS, on a window without a popup
		// owner, is "I mean it": SIGTERM to its task, carried out by
		// the frame loop after the lock drops. Explicit rather than timed —
		// a stuck program is killed by the user's hand, never by a clock.
		// Owned popups repeat the close request instead: killing their task
		// would also kill the document whose menu is being dismissed.
		// F-keys have no ASCII, so this is the one chord that reads the
		// scancode — through keyboard_fkey_number, which knows both
		// dialects. Both edges swallowed.
		if ((ev->key.modifiers & KEYBOARD_MOD_ALT) && !(ev->key.modifiers & KEYBOARD_MOD_CTRL) &&
		    keyboard_fkey_number(ev->key.scancode, ev->key.modifiers) == 4) {
			window_t *focus = wm_focused();
			// The desktop is exempt — and this one is guarded HERE rather
			// than relying on the action helper: escalation bypasses the normal
			// close request, so its guard must cover both paths here. Alt+F4 with nothing
			// but your desktop showing is a gesture nobody means, and its
			// second press would SIGTERM the shell.
			if (focus && (focus->flags & GUI_WINDOW_DESKTOP)) {
				printd(DEBUG_GUI, "guicomp: close declined — window %u is the desktop\n",
					focus->id);
				break;
			}
			if (focus && ev->type == INPUT_EVENT_KEY_DOWN) {
				if (!focus->popup_owner_id && focus->closeAskedTick != 0 &&
				    ev->tick - focus->closeAskedTick <= GUI_CLOSE_ESCALATE_TICKS) {
					printd(DEBUG_GUI, "guicomp: window %u asked to close twice — terminating owner %lu\n",
						focus->id, focus->owner);
					focus->closeAskedTick = 0;
					s_terminate_owner = focus->owner;
				} else {
					printd(DEBUG_GUI, "guicomp: window %u asked to close\n", focus->id);
					focus->closeAskedTick = focus->popup_owner_id ? 0 : ev->tick;
					wm_decoration_action(focus,OS64_DECOR_CLOSE,ev->tick);
				}
			}
			break;
		}

		// Ctrl+Alt+letter: the window-management verbs, on the FOCUSED
		// window. The same chord the mouse gestures use (GRAPHICS.md's
		// resize chapter ruled Ctrl+Alt the WM's escape hatch), so a plain
		// Ctrl+letter still reaches the app. Ctrl strips the letter to its
		// control code on both keyboard paths, so the ASCII is matched —
		// 0x10 is Ctrl+P — and the scancode, which differs between PS/2
		// and HID, is never consulted. Both edges are swallowed.
		if ((ev->key.modifiers & (KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT)) ==
		    (KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT) && ev->key.ascii >= 0x01 && ev->key.ascii <= 0x1A) {
			window_t *focus = wm_focused();
			if (focus && ev->type == INPUT_EVENT_KEY_DOWN) {
				switch (ev->key.ascii) {
				case 0x13: // Ctrl+Alt+S: settings, including when chrome is hidden
					wm_decoration_action(focus,OS64_DECOR_SETTINGS,ev->tick);
					break;
				case 0x10: {   // Ctrl+Alt+P: pin on top (toggle)
					bool was = (focus->flags & GUI_WINDOW_PINNED) != 0;
					wm_decoration_action(focus,OS64_DECOR_PIN,ev->tick);
					chord_report(focus, GUI_WINDOW_PINNED, was, "pinned", "unpinned");
					break;
				}
				case 0x0D: {   // Ctrl+Alt+M: maximize (toggle) — 0x0D is Ctrl+M, a CR by 1963's table
					bool was = (focus->flags & GUI_WINDOW_MAXIMIZED) != 0;
					wm_decoration_action(focus,OS64_DECOR_MAXIMIZE,ev->tick);
					chord_report(focus, GUI_WINDOW_MAXIMIZED, was, "maximized", "restored");
					break;
				}
				// Ctrl+Alt+N: minimize. Alt+Tab brings it back — it shows in
				// the switcher strip as a dim row, and returns only if the
				// hold ENDS on it (walking past leaves it hidden).
				case 0x0E: {
					bool was = wm_is_hidden(focus);
					wm_decoration_action(focus,OS64_DECOR_MINIMIZE,ev->tick);
					chord_report(focus, GUI_WINDOW_MINIMIZED, was, "minimized", "restored");
					break;
				}
				case 0x14: {   // Ctrl+Alt+T: titlebar (toggle)
					bool was = (focus->flags & GUI_WINDOW_NO_DECORATIONS) != 0;
					wm_set_decorated(focus, was);
					chord_report(focus, GUI_WINDOW_NO_DECORATIONS, was,
					             "titlebar hidden", "titlebar shown");
					break;
				}
				default:
					break;   // an unassigned letter is simply swallowed
				}
			}
			break;
		}

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

	printd(DEBUG_GUI, "guicomp: compositor starting on CPU %u (thread=0x%08x, daemon=%u)\n",
		cls->cpu, self->threadID, daemon);

	if (surface_init(&kBackbuffer, kFrameBuffer.width, kFrameBuffer.height) != 0 ||
	    surface_init(&kDesktop, kFrameBuffer.width, kFrameBuffer.height) != 0) {
		printd(DEBUG_GUI, "guicomp: FATAL: surface allocation failed, compositor exiting\n");
		return false;
	}
	__atomic_store_n(&s_backbuffer_ready, true, __ATOMIC_RELEASE);
	printd(DEBUG_GUI, "guicomp: backbuffer %ux%u ready (%lu KB x2)\n",
		kBackbuffer.width, kBackbuffer.height,
		(uint64_t)kBackbuffer.width * kBackbuffer.height * 4 / 1024);

	// The frame clock begins stopped (its states: FRAME_CLOCK.md § 9). With
	// it on, the scheduler learns which thread's frames claim which CPU
	// (scheduler.h): the CPU first, the thread last, since the thread is
	// what a pass looks at first. The compositor is a daemon that never
	// exits, so the pointer never dangles.
	s_fc_self = self;
	s_fc_time.since_us = micros_now();
	if (s_fc_off_why == NULL && daemon) {
		kSchedFrameCPU = s_fc_cpu;
		__atomic_store_n(&kSchedFrameThread, self, __ATOMIC_RELEASE);
	}

	cursor_build();
	s_cursor_x = (int32_t)kBackbuffer.width / 2;
	s_cursor_y = (int32_t)kBackbuffer.height / 2;
	gui_damage_add(cursor_rect());

	// THE FLOOR, and only the floor. The test pattern has been here since
	// the surface core came up, and it is what you see before /bin/desktop
	// starts, if it never starts, or if it dies — which is exactly why it
	// stays in the kernel now that the decor does not.
	//
	// The kernel used to paint the WALLPAPER here too: gui/desktop.c read
	// desktop.conf and decoded a PPM, in ring 0, from a file any user could
	// write. That file is deleted as of 2026-08-25 and the job belongs to
	// the desktop shell, which paints it into a real window in the bottom
	// z-band — a window, so that a click landing on no application has
	// somewhere to land.
	paint_desktop();

	// (THE "hello os64" WINDOW started here — the first window this
	// compositor ever drew, M5's proof that the client API worked, kept as a
	// legacy switch when gui.conf arrived. RETIRED 2026-08-25, Chris's call
	// on the day the desktop moved to ring 3: "lets remove it. If I want to
	// reminisce, I can run an old build." Its `hello` key was also the last
	// thing making the KERNEL read gui.conf, so retiring the window closes
	// the two-readers-of-one-file wart in the same stroke — gui.conf is
	// entirely /bin/desktop's now. git history has the window.)
	//
	// (The ring-0 console window started here from M5 until the VT8 chapter
	// retired it, 2026-08-19 — ruled: no ring-0-rendered windows. printf and
	// print_n land in VT1's grid now, one Alt+F1 away; Phase E's terminal is
	// the ring-3 heir. git history of gui/console_window.c has the grid-to-
	// pixels reference loop.)

	uint64_t frames = 0, flushes = 0;
	uint64_t last_heartbeat_tick = kTicksSinceStart;

	while (1) {
		frames++;
		s_fc_beat++;          // the stall tripwire's heartbeat
		s_fc_at = FC_AT_PASS;
		uint64_t stall_ms = __atomic_exchange_n(&s_fc_test_stall_ms, 0, __ATOMIC_SEQ_CST);
		if (stall_ms != 0) {
			// The proof the tripwire asked for: stand still, interrupts on,
			// holding nothing, so only the beat stops.
			uint64_t until = micros_now() + stall_ms * 1000;
			while (micros_now() < until)
				__builtin_ia32_pause();
		}

		// -------- Drain input, route events, recomposite (one lock hold) ---
		uint64_t irqflags = gui_lock(GUI_LOCK_COMPOSITE);
		glass_fold_locked();   // the last pass's publish-to-glass waits
		frame_clock_fold_locked();   // and its copy's verdict
		// A VT transition can occur without another pointer event.
		if (!gui_owns_glass() || s_glass_regained) gui_decoration_changed();

		input_event_t ev;
		while (input_pop(&ev))
			route_event_locked(&ev);

		// A VT switch landed on us since last frame: the whole backbuffer is
		// owed to the glass (it kept compositing while a text VT held the
		// iron — see the seated-predicate comment). Convert the ISR's one-
		// store flag into ordinary damage here, under our own lock.
		if (s_glass_regained) {
			gui_cancel_decoration(NULL);
			s_glass_regained = false;
			// Whatever the console overlay had painted is gone with the VT
			// switch's repaint; it must not try to restore cells that now
			// belong to the desktop.
			vtsel_forget();
			// And a client grab that straddled the time away is stale: the
			// button that started it came up on a text console, where the
			// release went to vt_select and never reached us. Without this,
			// the first release after the return would land on a window
			// that finished its gesture a VT switch ago.
			s_pointer_window = NULL;
			// An Alt+Tab hold is stale for exactly the same reason, and it is
			// the keyboard's version of that bug: a hold ends on the first
			// key event arriving WITHOUT Alt, and while a text VT held the
			// iron every one of those went to the console instead. The hold
			// would otherwise still be open on our return — with the strip
			// repainted by the full-screen damage below, over a scene the
			// user has since stopped choosing from. Abandon it: leaving is
			// not choosing, so the scene is left exactly as it was found.
			if (s_alttab_active)
				alttab_end_locked(false);
			gui_damage_add_locked((rect_t){0, 0, (int32_t)kBackbuffer.width,
			                               (int32_t)kBackbuffer.height});
		}

		// The window manager has had its say for this frame — raises, moves,
		// minimizes, a VT switch — so this is the moment the answer to "can
		// anyone see window W?" is settled. Tell the clients whose answer
		// changed (window.c). Every frame, not only damaged ones: a client's
		// own create or destroy arrives as damage anyway, and the walk costs
		// less than one damage rect's composite.
		wm_cover_sweep_locked();
		control_state_locked();
		pointer_state_locked();

		// Take the whole damage list in one hold and reset it, so clients
		// publishing during this frame's flush accumulate into the NEXT
		// frame instead of racing the one being drawn.
		rect_t damage[DAMAGE_MAX_RECTS];
		uint32_t damage_count = kPendingDamageCount;
		bool drew = false;
		for (uint32_t i = 0; i < damage_count; i++) {
			damage[i] = composite_locked(kPendingDamage[i]);
			if (!rect_is_empty(damage[i])) {
				drew = true;
				s_composite_counts.rects++;
				s_composite_counts.pixels += (uint64_t)damage[i].w * (uint64_t)damage[i].h;
			}
		}
		// Drawn means something survived the clip to the screen: damage from
		// a window moved wholly off it composites nothing.
		s_composite_counts.passes++;
		s_composite_counts.drawn += drew;
		kPendingDamageCount = 0;
		// The publishes behind this damage leave the list with it.
		glass_pending_t glass_taken[GLASS_PENDING_MAX];
		uint32_t glass_taken_count = s_glass_pending_count;
		for (uint32_t i = 0; i < glass_taken_count; i++)
			glass_taken[i] = s_glass_pending[i];
		s_glass_pending_count = 0;
		// Every /dev/glass viewer learns what this frame changed. Here,
		// after the pixels are down and before the lock goes, is what makes
		// a viewer's copy eventually exact (gui/glass.h): any composite that
		// overlaps a copy records its damage after the copy's take.
		glass_damage_locked(damage, damage_count);

		gui_unlock(irqflags);

		// A /dev/glass keyboard's held key repeats here, outside kGuiLock as
		// delivery must be (gui/glass.h, INPUT and glass_input_tick).
		glass_input_tick();

		// A close escalation decided under the lock is carried out here,
		// outside it: signalling a task takes scheduler locks, and kGuiLock
		// must never nest under those (see gui_window_terminate_owner).
		if (s_terminate_owner != 0) {
			uint64_t owner = s_terminate_owner;
			s_terminate_owner = 0;
			gui_window_terminate_owner(owner);
		}

		// -------- Flush to the (write-combining) framebuffer, lock-free ----
		// One flush per surviving rect. composite_locked clipped each to the
		// screen and may have emptied it entirely; those are skipped here
		// rather than filtered above, so the indices keep matching.
		//
		// AND ONLY WHILE VT8 OWNS THE IRON (the VT8 chapter's one gate): with
		// a text terminal focused, the frame above still landed in the
		// backbuffer — current state, zero VRAM cost — and the return switch
		// pays one full-screen flush for all of it.
		// The other side of the fork: with a text terminal on the iron, this
		// frame's job is the console's mouse overlay instead of a flush.
		// OUTSIDE kGuiLock, deliberately — vtsel_paint takes the tty and
		// renderer locks, and reaching those while holding ours would invent
		// a lock order nothing else in the system has.
		// With the clock on, the copy is the clock's: carried to the blank and
		// made there (frame_clock_present). It drives only while the screen
		// is ours; a text terminal taking it voids what was carried, as a
		// pass that flushes nothing voids what it took.
		bool clock_drives = s_fc_off_why == NULL && daemon && gui_owns_glass();
		if (clock_drives) {
			frame_clock_present(damage, damage_count, glass_taken, glass_taken_count, &flushes);
			goto flushed;
		}
		if (s_fc_off_why == NULL && daemon && (s_fc_carry_count != 0 || s_fc_glass_carry_count != 0))
			fc_drop_carry();

		if (!gui_owns_glass())
			vtsel_paint();

		bool flushed_any = false;
		// The frame clock's measurement: where the sweep is as the copy
		// begins, read only when there is a copy to judge (the rest is
		// frame_clock_judge_copy's).
		uint32_t fc_begin_line = 0;
		uint64_t fc_begin_us = 0;
		bool fc_reading = false;
		if (gui_owns_glass() && beam_present()) {
			for (uint32_t i = 0; i < damage_count && !fc_reading; i++)
				fc_reading = !rect_is_empty(damage[i]);
			if (fc_reading)
				fc_reading = beam_line(&fc_begin_line);
			if (fc_reading)
				fc_begin_us = micros_now();
		}
		if (gui_owns_glass())
		for (uint32_t i = 0; i < damage_count; i++) {
			if (rect_is_empty(damage[i]))
				continue;
			surface_flush_rect(&kBackbuffer, damage[i]);
			flushes++;
			flushed_any = true;
			printd(DEBUG_GUI | DEBUG_DETAILED,
				"guicomp: composited %dx%d at (%d,%d) [rect %u/%u], tick %lu\n",
				damage[i].w, damage[i].h, damage[i].x, damage[i].y,
				i + 1, damage_count, kTicksSinceStart);
		}
		if (fc_reading && flushed_any)
			frame_clock_judge_copy(damage, damage_count, fc_begin_line, fc_begin_us);
		// This pass's publishes are on the glass now, or never will be.
		glass_settle(glass_taken, glass_taken_count, flushed_any);

	flushed:
		if (kTicksSinceStart - last_heartbeat_tick >= TICKS_PER_SECOND) {
			printd(DEBUG_GUI, "guicomp: heartbeat, %lu frames, %lu flushes (tick %lu)\n",
				frames, flushes, kTicksSinceStart);
			last_heartbeat_tick = kTicksSinceStart;
		}

		if (!daemon)
			return true;

		// With the clock on, it paces: stop, or sleep until the next frame's
		// wake (frame_clock_pace, § 9). What follows is the clock-off pacing.
		if (s_fc_off_why == NULL) {
			frame_clock_pace();
			continue;
		}

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
		// frame's real work onto us and routes the nap to the idle thread.
		// IRQ entry closes the nap before billing its handler; End settles
		// our post-wake tail.
		__asm__ volatile("cli" ::: "memory");
		if (!input_pending() && kPendingDamageCount == 0 && !s_glass_regained) {
			mpAcctHaltBegin();
			__asm__ volatile("sti\n\thlt" ::: "memory");
			mpAcctHaltEnd();
		} else
			__asm__ volatile("sti" ::: "memory");
	}
}

// With the frame clock on, the compositor is pinned to the LAST CPU brought
// up (FRAME_CLOCK.md § Rulings 5): never the BSP, which takes every device
// interrupt, and with the roster's order never the BSP's SMT sibling on a
// machine with more than two; and away from the low numbers people pin
// their own work to. The input interrupts follow it (kernel_init routes them
// here).
//
// Under tickless a pinned compositor used to be refused: an AP's timer was
// masked, so a thread pinned there ran unpreemptable and only when nudged,
// which wedged it. What makes the pin sound now: the AP's timer is the
// preemption lease and the frame wake (smp_core.c), the compositor sets it
// before every halt, and new work nudges it (gui_frame_nudge).
//
// With the clock off the pin is as it was before the clock: CPU 1 under
// SCHED=periodic, none under tickless or with one CPU.
uint64_t gui_compositor_affinity(void)
{
	if (frame_clock_off_why() == NULL)
		return (uint64_t)(kCPUCount - 1);
	return (kCPUCount > 1 && !kTicklessScheduler)
	           ? (uint64_t)1
	           : THREAD_NO_AFFINITY;
}

void gui_start(void)
{
	// THE KERNEL NO LONGER READS gui.conf (2026-08-25). It used to be read
	// right here, before anything was submitted, because the compositor
	// thread asked gui_startup_hello() while building its scene. Both the
	// window and its switch are gone, and the `start` lines belong to
	// /bin/desktop — so gui.conf has exactly one reader again, in ring 3,
	// which is the arrangement the config search path was built to protect.
	//
	// Open the unified input queue before the compositor (its consumer)
	// exists — events buffered during task startup are simply the first
	// ones drained.
	input_init();

	uint64_t affinity = gui_compositor_affinity();
	// The frame clock's boot-long answer, before the compositor that runs it
	// exists (FRAME_CLOCK.md § 9, "off").
	s_fc_off_why = frame_clock_off_why();
	if (s_fc_off_why == NULL) {
		s_fc_cpu = (uint32_t)affinity;
		printd(DEBUG_BOOT | DEBUG_GUI, "gui_start: frame clock on, the compositor pinned to CPU %u\n", s_fc_cpu);
	} else {
		printd(DEBUG_BOOT | DEBUG_GUI, "gui_start: frame clock off (%s)\n", s_fc_off_why);
	}

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
	//
	// (THE LIST WAS gui.conf's from 2026-08-23 to 2026-08-25, read right
	// here, with the two demos as its default. What made THIS the right home
	// then: these apps belong to the DESKTOP's startup, and everything Chris
	// actually wanted at boot was stranded in husk.rc — which runs in every
	// husk (VT1 and VT2 both start one, so he got two gclocks and two gterms)
	// and runs on text boots too (where every GUI line failed once per
	// terminal). That argument still holds; what changed is WHO the desktop
	// is. It is a program now, so the list — and the demo default — moved
	// into /bin/desktop with it, and the kernel's list is the one line
	// below.)
	// THE KERNEL STARTS EXACTLY ONE GUI PROGRAM (2026-08-25): the desktop
	// shell. Everything else that starts with the desktop is started BY the
	// desktop, out of gui.conf, which is the shell's rc.
	//
	// The name is hardcoded, and that is the whole of the kernel's startup
	// policy — one line, deliberately. When husk-as-init lands, this line is
	// what moves to the init table, and /bin/desktop does not change,
	// because nothing in it ever asked who started it.
	//
	// If it is missing or fails to launch, the compositor's own test pattern
	// stays on the glass and the machine remains usable — the floor is in
	// the kernel precisely so the decor can be a program that might not be
	// there.
	static const char *const kShell[] = { "/bin/desktop" };
	for (size_t i = 0; i < sizeof(kShell) / sizeof(kShell[0]); i++) {
		const char *app = kShell[i];
		task_t *demo = task_create((char *)app, 0, NULL, kKernelTask, false,
		                           THREAD_NO_AFFINITY);
		if (demo == NULL) {
			// BOTH sinks, deliberately: printf is FRAMEBUFFER-ONLY (the
			// panic-pipeline scar), so a glass-only complaint vanishes the
			// moment the desktop paints over it — which on the P5 cost a
			// reboot and a log search that found nothing (2026-08-17, the
			// first GUI boot against a root that predated the ring-3
			// demos). The wire copy is unconditional: a missing binary at
			// boot is exactly the fact a log exists to keep.
			printf("gui_start: %s launch failed (not on the image?)\n", app);
			printd(DEBUG_BOOT, "gui_start: %s launch failed (not on the image?)\n", app);
			continue;
		}
		demo->autoReap = true;
		scheduler_submit_new_task(demo);
	}

	// Seat VT8 and take the stage (the VT8 chapter, 2026-08-19). TTY_LIVE is
	// what tells the knock-summon this terminal already has a shell — ours —
	// so a stray keystroke can never hang a husk on the GUI's VT. The focus
	// comes LAST: every boot printf after this line lands silently in VT1's
	// grid (print_n's fallback), waiting under Alt+F1, instead of scribbling
	// over the desktop the compositor is about to paint.
	kTTY[7].state = TTY_LIVE;
	s_gui_seated = true;
	tty_focus(7);
}
