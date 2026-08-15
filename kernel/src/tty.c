// tty.c — the virtual terminal engine (2026-08-08). See tty.h for the design
// doctrine; this file is the machinery: the grids, the one true terminal
// interpreter (print_n's old switch, retired here), focus switching as
// repaint-from-state (os32's three-line switchTerm, grown a shadow buffer),
// the per-tty input rings, and the summons that hangs a shell on a dark
// terminal the moment somebody knocks.

#include "tty.h"
#include "BasicRenderer.h"     // the glass: renderer_glass_* primitives
#include "kmalloc.h"
#include "memset.h"
#include "strings/sprintf.h"
#include "strings/strlen.h"
#include "task.h"
#include "thread.h"
#include "scheduler.h"
#include "signals.h"
#include "smp_core.h"
#include "kernel.h"
#include "printd.h"
#include "CONFIG.h"

extern BasicRenderer kRenderer;
extern task_t *kKernelTask;    // parent of every summoned shell (kernel.c)
extern task_t *kKWorkerTask;   // the undertaker, moonlighting as midwife (kernel.c)

tty_t kTTY[TTY_COUNT];
tty_t * volatile kTTYFocused = &kTTY[0];
volatile bool kTTYReady = false;
volatile bool kTTYDirect = false;

extern volatile uint64_t kTicksSinceStart;

// ── Deferred glass (2026-08-13, the frozen-cat fix) ─────────────────────────
// A focused-VT SCROLL no longer drags the glass along line by line. The old
// way called renderer_glass_scroll_locked() — a ~3MB shadow memmove — once
// PER SCROLLED LINE, inside tty_write's irqsave t->lock. `cat` on a big log
// meant minutes of memmoves with interrupts off on the writing core; and
// under tickless scheduling that core is often the BSP, the only core taking
// timer ticks, so kTicksSinceStart FROZE machine-wide for the whole write
// chunk: the blit throttle's window never opened (no repaints), the keyboard
// IRQ never landed (no Alt+F#), sleeps stalled. One writer, whole machine
// held hostage. Meanwhile the same flood on an UNFOCUSED terminal was
// instant, because a grid scroll is a ring-pointer walk. The asymmetry was
// the diagnosis: the grid is the truth, so let the glass be a THROTTLED VIEW.
//
// The mechanism: the first scroll of a write burst marks the glass STALE and
// stops mirroring (grid-only from there — the cheap path). The rider below
// (tty_flush_if_dirty, riding processSignals exactly like the renderer's
// blit throttle) repaints the visible window FROM THE GRID at most once per
// TTY_REPAINT_MIN_TICKS. A ten-thousand-line flood now costs the glass a
// handful of full repaints (~3k glyphs into cached shadow RAM + one blit
// each) instead of ten thousand 3MB memmoves. Per-character output that
// never scrolls — prompt echo, line editing, the cursor dance — still lands
// on the glass immediately; only scrolls defer.
//
// s_glassStale is written under the FOCUSED tty's lock (writers) and cleared
// by any full repaint (tty_repaint_locked, which every clearer runs under
// some t->lock). A focus switch can race a writer's set with its clear —
// benign by design: a lost clear costs one redundant repaint; a lost set is
// re-set by the flood's very next scroll. The rider re-checks under the
// focused tty's lock before repainting, so it never paints a half-written
// line.
static volatile bool s_glassStale = false;
static uint64_t s_glassRepaintTick = 0;
#define TTY_REPAINT_MIN_TICKS 3   // ~30Hz, matching the renderer's BLIT_MIN_TICKS

// ── Grid geometry helpers ───────────────────────────────────────────────────
// The grid is a RING of total_lines lines; screen_top is the ring index of
// the live screen's row 0. Lines "above" screen_top (going backwards through
// the ring, up to hist_lines of them) are scrollback. A grid scroll advances
// screen_top — the departing top line automatically BECOMES history, and the
// oldest history line is the one the new bottom line overwrites. Nothing is
// ever copied; scrolling is a pointer walk, exactly what a ring is for.

static inline tty_cell_t *tty_line(tty_t *t, uint32_t ring_line)
{
	return &t->cells[(size_t)ring_line * t->cols];
}

// Ring line index of live-screen row r (0..rows-1).
static inline uint32_t tty_row_line(tty_t *t, uint32_t row)
{
	return (t->screen_top + row) % t->total_lines;
}

static void tty_clear_line(tty_t *t, uint32_t ring_line)
{
	memset(tty_line(t, ring_line), 0, (size_t)t->cols * sizeof(tty_cell_t));
}

tty_t *task_tty(struct task *t)
{
	if (t != NULL && t->tty != NULL)
		return (tty_t *)t->tty;
	return &kTTY[0];   // no terminal of record = the system console, VT1
}

// ── The interpreter (caller holds t->lock) ──────────────────────────────────
// This is print_n's old control-character switch, verbatim in SEMANTICS (one
// deliberate quirk and all — '\t' advances one cell, not a tab stop), applied
// to the GRID first and mirrored to the glass only when `glass` says this
// tty is the one on stage. The grid is the truth; the glass is a projection.
//
// `glass` is BY REFERENCE because a scroll retires it mid-write: the first
// scroll marks the glass stale and hands the rest of the burst to the
// repaint rider (see the deferred-glass doctrine above the statics). The
// caller must keep releasing the renderer lock it acquired even after the
// flag drops — the flag says "stop painting", not "you never held the lock".
static void tty_putc_locked(tty_t *t, char ch, bool *glass)
{
	switch (ch)
	{
		case '\n':
			t->cur_col = 0;
			t->cur_row++;
			break;
		case '\t':
			t->cur_col++;         // one cell — the legacy quirk, kept on purpose
			break;
		case '\b':
			// Move back one cell, clamped at the line start (a terminal never
			// backspaces up a line). Only MOVES — erasure stays the caller's
			// job by overprint, which is why husk rubs out with "\b \b".
			if (t->cur_col > 0)
				t->cur_col--;
			break;
		case '\r':
			t->cur_col = 0;
			break;
		case '\f':
			// Form feed wipes THIS terminal's live screen and homes the
			// cursor — each console interprets control bytes against its own
			// state, the doctrine that made clear(1) five bytes of printf.
			// Scrollback survives: the page was ejected, not burned.
			for (uint32_t r = 0; r < t->rows; r++)
				tty_clear_line(t, tty_row_line(t, r));
			t->cur_row = 0;
			t->cur_col = 0;
			// '\f' stays IMMEDIATE (one-shot wipe, not a per-line cost):
			// clear(1) deserves its instant blank page, and a form feed in
			// the middle of a flood resets the fall-behind anyway.
			if (*glass)
				renderer_glass_clear_locked();
			break;
		default:
		{
			tty_cell_t *cell = &tty_line(t, tty_row_line(t, t->cur_row))[t->cur_col];
			cell->ch = ch;
			cell->color = t->color;
			if (*glass)
				renderer_glass_putc_locked(ch, t->cur_row, t->cur_col, t->color);
			t->cur_col++;
			break;
		}
	}

	// Wrap, then scroll — same order, same conditions as the glass-only days.
	if (t->cur_col >= t->cols)
	{
		t->cur_col = 0;
		t->cur_row++;
	}
	if (t->cur_row >= t->rows)
	{
		// Grid scroll: advance the ring and blank the incoming bottom line.
		// The old top line is now history; hist_lines grows until the ring's
		// capacity is reached, after which the oldest line pays for each new one.
		t->screen_top = (t->screen_top + 1) % t->total_lines;
		if (t->hist_lines < t->total_lines - t->rows)
			t->hist_lines++;
		tty_clear_line(t, tty_row_line(t, t->rows - 1));
		t->cur_row = t->rows - 1;
		// THE deferral point: no glass scroll, no 3MB memmove. Mark the
		// glass stale, stop mirroring for the rest of this write, and let
		// tty_flush_if_dirty repaint from the grid on its ~30Hz clock.
		if (*glass)
		{
			s_glassStale = true;
			*glass = false;
		}
	}
}

// Repaint the visible window from the grid — the whole trick of having one.
// Caller holds t->lock. Uses the deferred-VRAM gait: every glyph lands in the
// shadow only, then ONE full blit pushes the finished frame, so a terminal
// switch costs one memcpy on the glass instead of sixteen thousand pokes.
static void tty_repaint_locked(tty_t *t)
{
	uint64_t rflags = renderer_glass_begin();
	renderer_glass_defer_locked();

	// Top visible ring line: the live screen top, backed up view_offset lines.
	uint32_t top = (t->screen_top + t->total_lines - t->view_offset) % t->total_lines;
	for (uint32_t r = 0; r < t->rows; r++)
	{
		tty_cell_t *line = tty_line(t, (top + r) % t->total_lines);
		for (uint32_t c = 0; c < t->cols; c++)
		{
			// A never-written cell paints as a space — full coverage means
			// the repaint needs no separate clear pass (and no flicker).
			char ch = line[c].ch ? line[c].ch : ' ';
			uint32_t color = line[c].ch ? line[c].color : t->color;
			renderer_glass_putc_locked(ch, r, c, color);
		}
	}
	renderer_glass_blit_locked();

	// A full repaint IS the glass catching up — whatever staleness a write
	// burst left behind is now painted. Clearing here (under the caller's
	// t->lock) covers every repaint door: the rider, a focus switch, a
	// scrollback view move, a history snap.
	s_glassStale = false;
	s_glassRepaintTick = kTicksSinceStart;

	// Relight the listening light only when it is honest: a parked reader,
	// the live screen (a cursor has no business in the scrollback), and a
	// terminal that actually has someone seated at it.
	bool show = (t->waiter != NULL && t->view_offset == 0 && t->state == TTY_LIVE);
	renderer_glass_end(rflags, t->cur_row, t->cur_col, show);
}

// ── Output ──────────────────────────────────────────────────────────────────

void tty_write(tty_t *t, const char *bytes, size_t length)
{
	if (t == NULL || bytes == NULL || length == 0)
		return;

	// GUI diversion first, same as print_n always did: when the compositor
	// owns the console, ALL console bytes flow to its window. (The day GUI
	// terminal windows become tty sinks, this line is where they plug in.)
	console_sink_fn sink = kConsoleSink;
	if (sink)
	{
		sink(bytes, length);
		return;
	}

	// Not ready (early boot) or post-panic: the legacy direct-to-glass path.
	if (!kTTYReady || kTTYDirect || t->cells == NULL)
	{
		print_n_direct(bytes, length);
		return;
	}

	uint64_t tflags = spinlock_acquire_irqsave(&t->lock);
	// While the glass is stale a repaint is already owed — skip per-char
	// mirroring entirely and let the rider deliver these bytes with the
	// rest (mid-flood, this is the common case and the whole win: the
	// writer never touches the renderer at all).
	bool glass = (kTTYFocused == t && t->view_offset == 0 && !s_glassStale);
	bool glass_lock_held = false;
	uint64_t rflags = 0;
	if (glass)
	{
		rflags = renderer_glass_begin();
		glass_lock_held = true;
		// Re-check under the renderer lock: a focus switch on another core
		// publishes kTTYFocused before it repaints, so losing this race means
		// painting into a frame the switch is about to replace — skip the
		// glass, keep the grid (the repaint will show these bytes anyway).
		if (kTTYFocused != t)
			glass = false;
	}

	// tty_putc_locked may retire `glass` at the first scroll (deferred-glass
	// doctrine) — that changes who paints, not who holds the renderer lock,
	// which is why the release below keys off glass_lock_held instead.
	for (size_t i = 0; i < length; i++)
		tty_putc_locked(t, bytes[i], &glass);

	if (glass_lock_held)
		renderer_glass_end(rflags, t->cur_row, t->cur_col, false);
	spinlock_release_irqrestore(&t->lock, tflags);
}

// The repaint rider (called from processSignals, right beside the renderer's
// renderer_flush_if_dirty — same gait, one layer up): if a write burst left
// the glass stale and the ~30Hz window has passed, repaint the focused
// terminal from its grid. The unlocked s_glassStale peek is safe for the
// same reason the renderer's is — a stale read costs one pass, and the
// locked re-check decides for real. Lock order matches every other door:
// t->lock outside, renderer lock inside (tty_repaint_locked takes it).
void tty_flush_if_dirty(void)
{
	if (!s_glassStale || !kTTYReady || kTTYDirect)
		return;
	if (kTicksSinceStart - s_glassRepaintTick < TTY_REPAINT_MIN_TICKS)
		return;

	tty_t *t = kTTYFocused;
	if (t == NULL || t->cells == NULL)
		return;

	uint64_t flags = spinlock_acquire_irqsave(&t->lock);
	// Re-check under the lock; repaint only the terminal that is still on
	// stage AND showing the present (a scrollback viewer's screen must not
	// snap forward because a background flood is writing history).
	if (s_glassStale && kTTYFocused == t && t->view_offset == 0)
		tty_repaint_locked(t);   // clears s_glassStale itself
	spinlock_release_irqrestore(&t->lock, flags);
}

// ── Input ───────────────────────────────────────────────────────────────────

void tty_input_event(const keyboard_event_t *ev)
{
	tty_t *t = kTTYFocused;
	if (t == NULL)
		t = &kTTY[0];

	// A keystroke on a dormant terminal is not input, it is a KNOCK: swallow
	// the key and request a shell (kworker answers — see tty_summon_sweep).
	// Gated on kTTYReady so pre-boot type-ahead still queues into VT1's ring
	// — losing a human's first keystrokes was a bug once already (console.h
	// has the origin story), and it stays fixed.
	if (kTTYReady && t->state == TTY_DORMANT)
	{
		t->spawnRequested = true;
		return;
	}

	// A keystroke while viewing history means "I'm done reading" — snap to
	// the present before the byte lands (the Linux console's rule, kept).
	if (t->view_offset != 0)
	{
		uint64_t flags = spinlock_acquire_irqsave(&t->lock);
		if (t->view_offset != 0)
		{
			t->view_offset = 0;
			tty_repaint_locked(t);
		}
		spinlock_release_irqrestore(&t->lock, flags);
	}

	// Push into THIS tty's ring. Producers lock (PS/2 IRQ, xHCI poll, and
	// typematic can interleave); the consumer side stays lock-free — one
	// reader per tty, the same single-consumer contract the global ring had.
	uint64_t flags = spinlock_acquire_irqsave(&t->ring_lock);
	size_t head = t->ring_head;
	size_t next = (head + 1u) % KEYBOARD_BUFFER_SIZE;
	if (next == t->ring_tail)
	{
		// Full: keep the oldest keystrokes, drop this one (legacy behavior).
		spinlock_release_irqrestore(&t->ring_lock, flags);
		return;
	}
	t->ring[head] = *ev;
	t->ring_head = next;
	spinlock_release_irqrestore(&t->ring_lock, flags);
}

bool tty_input_has(tty_t *t)
{
	return t->ring_head != t->ring_tail;
}

bool tty_input_pop(tty_t *t, keyboard_event_t *ev)
{
	if (ev == NULL || t->ring_head == t->ring_tail)
		return false;
	*ev = t->ring[t->ring_tail];
	t->ring_tail = (t->ring_tail + 1u) % KEYBOARD_BUFFER_SIZE;
	return true;
}

// ── Focus ───────────────────────────────────────────────────────────────────
// Deliberately synchronous from the keypress (PS/2 IRQ or the xHCI poll):
// the repaint writes the shadow (cached RAM) and pushes one blit, the same
// cost class as the full-screen scroll that already runs under the renderer
// lock with interrupts off — correctness beats the few-ms jitter, and a
// human presses Alt+F3 a lot less often than a log burst scrolls.

void tty_focus(uint32_t index)
{
	if (index >= TTY_COUNT || !kTTYReady || kTTYDirect)
		return;

	tty_t *next = &kTTY[index];
	tty_t *prev = kTTYFocused;
	if (prev == next)
		return;

	// Leaving a terminal snaps its view to the present — it greets you live
	// when you return. (One tty lock at a time, always: prev, then next.)
	if (prev != NULL)
	{
		uint64_t flags = spinlock_acquire_irqsave(&prev->lock);
		prev->view_offset = 0;
		spinlock_release_irqrestore(&prev->lock, flags);
	}

	uint64_t flags = spinlock_acquire_irqsave(&next->lock);
	// Publish BEFORE the repaint: any concurrent tty_write that grabs the
	// renderer lock first re-checks this pointer and stands down (see the
	// re-check in tty_write) — so the repaint below always paints last.
	kTTYFocused = next;
	tty_repaint_locked(next);
	spinlock_release_irqrestore(&next->lock, flags);
}

void tty_focus_step(int dir)
{
	tty_t *cur = kTTYFocused;
	uint32_t idx = (cur != NULL) ? cur->index : 0;
	uint32_t step = (dir > 0) ? 1u : (TTY_COUNT - 1u);   // -1, spelled unsigned
	tty_focus((idx + step) % TTY_COUNT);
}

void tty_view_scroll(int dir)
{
	tty_t *t = kTTYFocused;
	if (t == NULL || !kTTYReady || kTTYDirect)
		return;

	uint64_t flags = spinlock_acquire_irqsave(&t->lock);
	uint32_t step = t->rows / 2;   // half a screen per press — the classic gait
	if (step == 0)
		step = 1;
	uint32_t was = t->view_offset;
	if (dir > 0)
	{
		t->view_offset += step;
		if (t->view_offset > t->hist_lines)
			t->view_offset = t->hist_lines;
	}
	else
	{
		t->view_offset = (t->view_offset > step) ? (t->view_offset - step) : 0;
	}
	if (t->view_offset != was)
		tty_repaint_locked(t);
	spinlock_release_irqrestore(&t->lock, flags);
}

// ── Shells and the summons ──────────────────────────────────────────────────

void tty_seat_shell(tty_t *t, struct task *shell)
{
	if (t == NULL || shell == NULL)
		return;
	// The full seat, before the shell can run: Ctrl+C immunity at the prompt
	// (controllingShell), the terminal of record (shell->tty — its children
	// inherit it), foreground (who Ctrl+C aims at HERE), and the lights on.
	shell->controllingShell = true;
	shell->tty = t;
	t->shell = shell;
	t->fgTask = shell;
	t->spawnRequested = false;
	t->state = TTY_LIVE;
}

void tty_shell_departed(struct task *task)
{
	if (task == NULL || task->tty == NULL)
		return;
	tty_t *t = (tty_t *)task->tty;
	if (t->shell != task)
		return;   // a child died, not the seat-holder — not our business

	t->shell = NULL;
	t->fgTask = NULL;
	t->spawnRequested = false;
	t->state = TTY_DORMANT;

	// The 1971 logout, os64 dialect: the shell is gone, the line is quiet,
	// and the next knock summons a fresh one (getty, on demand).
	static const char msg[] =
		"\n[shell departed -- press any key to summon another]\n";
	tty_write(t, msg, sizeof(msg) - 1);
}

bool tty_summon_pending(void)
{
	for (uint32_t i = 0; i < TTY_COUNT; i++)
		if (kTTY[i].spawnRequested)
			return true;
	return false;
}

// processSignals context, scheduler queue lock HELD: if a terminal has been
// knocked on, get the midwife out of bed early (kworker's own backstop nap
// is 2s — fine for burials, rude for a human standing at a dark terminal).
// Same wake idiom as console_wake_if_ready, for the same lost-wakeup reasons.
void tty_summon_wake(void)
{
	if (!tty_summon_pending())
		return;
	if (kKWorkerTask == NULL || kKWorkerTask->threads == NULL)
		return;
	thread_t *w = kKWorkerTask->threads;
	if (w->threadState == THREAD_STATE_ISLEEP)
	{
		w->signals.sigind &= ~SIGSLEEP;
		w->signals.sigdata[SIGSLEEP] = 0;
		scheduler_change_thread_queue_locked(w, THREAD_STATE_RUNNABLE);
	}
}

// kworker context ONLY (task context — spawning loads an ELF from disk, and
// disk I/O is no work for an IRQ). Serve every terminal that asked.
bool tty_summon_sweep(void)
{
	bool spawned = false;
	for (uint32_t i = 0; i < TTY_COUNT; i++)
	{
		tty_t *t = &kTTY[i];
		if (!t->spawnRequested)
			continue;
		if (t->state != TTY_DORMANT)
		{
			// Raced with a seat (boot husk, another sweep): request is stale.
			t->spawnRequested = false;
			continue;
		}

		task_t *shell = task_create("/bin/husk", 0, NULL, kKernelTask, false,
		                            THREAD_NO_AFFINITY);
		if (shell == NULL)
		{
			t->spawnRequested = false;   // another keystroke retries — harmless
			static const char msg[] =
				"\n[no shell came: /bin/husk failed to load -- press a key to retry]\n";
			tty_write(t, msg, sizeof(msg) - 1);
			continue;
		}

		// autoReap: COLLECTED BY DECREE — ktask is this shell's parent and
		// ktask never waits, so without the decree every `exit` on a summoned
		// shell mints an immortal zombie holding ~1.1MB of stacks until
		// reboot. THE 2026-08-15 P5 FIND: Chris's "15 zombies kworker won't
		// touch" were fifteen of exactly these — one per shell he had ever
		// exited that boot. Full story at the boot-husk launch in kernel.c.
		shell->autoReap = true;
		tty_seat_shell(t, shell);
		scheduler_submit_new_task(shell);
		printd(DEBUG_TASK, "tty: summoned %s onto tty%u\n",
		       shell->exename, t->index + 1);
		spawned = true;
	}
	return spawned;
}

// ── Panic escape hatch ──────────────────────────────────────────────────────
// Same doctrine as renderer_bust_lock (panic.c calls both): a dead core may
// hold any of these locks, and a panic that deadlocks on a terminal lock is
// a panic nobody reads. Force direct mode FIRST so every print from here on
// bypasses the grids entirely, then bust the locks for good measure.
void tty_emergency_direct(void)
{
	kTTYDirect = true;
	for (uint32_t i = 0; i < TTY_COUNT; i++)
	{
		__sync_lock_release(&kTTY[i].lock);
		__sync_lock_release(&kTTY[i].ring_lock);
	}
}

// ── Birth ───────────────────────────────────────────────────────────────────

void tty_init(void)
{
	uint32_t cols = renderer_cols();
	uint32_t rows = renderer_rows();

	for (uint32_t i = 0; i < TTY_COUNT; i++)
	{
		tty_t *t = &kTTY[i];
		t->index = i;
		t->cols = cols;
		t->rows = rows;
		t->total_lines = rows * TTY_SCROLLBACK_SCREENS;
		// kmalloc zeroes every allocation at the choke point (house doctrine),
		// so every cell starts blank and every lock/counter starts clear.
		t->cells = kmalloc((size_t)t->total_lines * cols * sizeof(tty_cell_t));
		t->color = 0xffffffff;
		t->state = TTY_DORMANT;
	}

	// VT1 takes over the live console MID-SENTENCE: seed its cursor from
	// wherever early boot left the glass cursor, so the first grid-routed
	// printf continues exactly where the last direct one stopped. The lines
	// already on screen exist only as pixels — the first switch away and
	// back repaints from the grid and they are gone. The grid is the truth,
	// and text older than the truth is archaeology.
	unsigned int cx = 0, cy = 0;
	get_cursor_pos(&kRenderer, &cx, &cy);
	kTTY[0].cur_col = (cx < cols) ? cx : 0;
	kTTY[0].cur_row = (cy < rows) ? cy : (rows - 1);
	kTTY[0].state = TTY_LIVE;   // the system console is always somebody's tty
	kTTYFocused = &kTTY[0];

	kTTYReady = true;

	// Dark terminals get a sign on the door. VT2 gets none — kernel_init
	// seats a shell there at boot (the os32 loadout: 8 terminals, 2 shells),
	// and if that seating ever fails, a blank dormant VT2 still answers a
	// knock like any other.
	for (uint32_t i = 2; i < TTY_COUNT; i++)
	{
		char banner[96];
		int n = sprintf(banner,
			"os64 virtual terminal %u\n\n  press any key to summon a shell.\n",
			i + 1);
		tty_write(&kTTY[i], banner, (size_t)n);
	}
}
