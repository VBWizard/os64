#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "driver/system/keyboard.h"   // keyboard_event_t — the input ring's coin
#include "spinlock.h"

// tty.h — the virtual terminal object (2026-08-08, "I WANT MY VTs!").
//
// THE IDEA (older than either of us): a TTY is the kernel object that means
// "a place a program reads keystrokes and writes characters", regardless of
// what iron is on the other end. Bell Labs invented it because 1970s Unix
// faced a zoo of Teletypes and glass terminals; os64 needs it because one
// framebuffer is about to carry eight terminals — and someday a GUI terminal
// window or a serial line will be just another sink bound to the same object.
//
// THE DESIGN (os32's terminfo_t screenBuffer, grown up):
//   - Every tty owns a CHARACTER-CELL GRID — a ring of lines holding
//     TTY_SCROLLBACK_SCREENS screens' worth of text. The grid is the TRUTH.
//     The glass (framebuffer) is a PROJECTION of the focused tty's grid.
//   - Writes land in the grid ALWAYS, and paint the glass ONLY when this tty
//     is focused. Switching terminals is therefore a repaint-from-state —
//     os32's switchTerm() was three lines for exactly this reason, and the
//     cell buffer is what os64's pixel-only console always lacked.
//   - ONE terminal interpreter (tty_write): \n \t \b \r \f, wrap, scroll are
//     applied to the grid, and the glass mirrors each operation. The old
//     print_n interpreter retired into this file; two interpreters running
//     in lockstep would only ever have drifted apart.
//   - The scrollback os32 never got around to falls out for free: the ring
//     already holds the history, Shift+PgUp just moves the view.
//
// THE SINGLETONS, MULTIPLIED (each was born with a comment promising this):
//   console.c's one sleeping reader, one pending EOF, one pushback slot, and
//   task.c's one kForegroundTask / controllingShell — all become fields here,
//   exactly as SIGINT.md prescribed ("per-tty_t fields, not a rewrite").
//
// LAYERING (unchanged from the seed):
//   keyboard.c / xhci.c — the device drivers; deliver translated keys, stay
//                         blind to tasks, signals, and terminals.
//   tty.c               — THIS: grids, focus, the input rings, the summons.
//   console.c           — the blocking-read discipline (per-tty now).
//   syscall read/write  — the ring-3 bridge; a task talks to ITS tty
//                         (task->tty, inherited at creation — the controlling
//                         terminal, by lineage if not yet by name).

#define TTY_COUNT 8                    // tty1..tty8 — the os32 loadout, kept
#define TTY_SCROLLBACK_SCREENS 4       // grid holds 4 screens: 1 live + 3 history

// One character cell: the glyph and the color it was painted in. 8 bytes
// with padding — at 1080p that is ~½MB per tty, ~4MB for the fleet, which is
// the cheapest possible price for repaint-from-state plus scrollback.
typedef struct tty_cell
{
	char ch;                           // 0 = blank (never painted)
	uint32_t color;
} tty_cell_t;

// A tty with no shell seated: dark glass, waiting. First keystroke on a
// dormant tty summons a fresh husk (the getty ritual, on demand — V6 read
// /etc/ttys at boot and hung a shell on every line; os64 hangs one the
// moment you knock). A shell that exits returns its tty to this state.
typedef enum tty_state
{
	TTY_DORMANT = 0,
	TTY_LIVE,
} tty_state_t;

struct task;
struct s_thread;   // thread_t's tag (thread.h)

typedef struct tty
{
	uint32_t index;                    // 0-based; humans say "tty1" = index 0

	// ── The grid (guarded by `lock`) ────────────────────────────────────────
	tty_cell_t *cells;                 // total_lines * cols, kmalloc'd at init
	uint32_t cols, rows;               // live-screen geometry (glass cells)
	uint32_t total_lines;              // rows * TTY_SCROLLBACK_SCREENS (ring)
	uint32_t screen_top;               // ring index of live row 0
	uint32_t hist_lines;               // valid history lines above screen_top
	uint32_t view_offset;              // >0 = viewing history, this many lines up
	uint32_t cur_row, cur_col;         // cursor, relative to screen_top
	uint32_t color;                    // current write color
	spinlock_t lock;                   // irqsave — grid/cursor/view; ALWAYS
	                                   // taken BEFORE the renderer lock, never
	                                   // after, and never two tty locks at once

	// ── The input ring (one per tty — type-ahead stays with its terminal) ──
	// Producers (PS/2 IRQ, xHCI poll, typematic) push under ring_lock; the
	// consumer side is lock-free because each tty has at most ONE reader —
	// the same single-consumer contract the global ring lived by.
	keyboard_event_t ring[KEYBOARD_BUFFER_SIZE];
	volatile size_t ring_head;
	volatile size_t ring_tail;
	spinlock_t ring_lock;

	// ── The multiplied singletons (see console.c for each one's doctrine) ──
	struct s_thread * volatile waiter; // the ONE thread parked in console_read
	volatile bool eofPending;          // "abc<Ctrl+D>": bytes now, EOF next read
	volatile char pushback[4];         // console_unread's LIFO slot
	volatile int pushbackCount;
	struct task * volatile fgTask;     // who Ctrl+C aims at ON THIS tty
	struct task * volatile shell;      // the controlling shell seated here

	// ── The summons (dormant ttys only) ─────────────────────────────────────
	volatile tty_state_t state;
	volatile bool spawnRequested;      // set by a keystroke, served by kworker

	// ── Change tracking (all ttys; PTY.md's snapshot poll reads it) ─────────
	// Bumped on every grid mutation. A pty master's holder polls this at
	// frame cadence and copies cells only when it moved; VTs carry it too
	// because the counter is free and a future dirty-aware consumer (the
	// client-notification seam) will want it everywhere.
	volatile uint64_t generation;

	// ── The pty fields (PTY.md, 2026-08-19) — zero for the kTTY[] fleet ────
	// A pty slave is THIS STRUCT with no keyboard and no glass: the master's
	// holder stands where they stood. is_pty gates the handful of places
	// that must not treat one like a VT (naming, the focus/summon iterators
	// never see them — they walk kTTY[] only — and repaint can't happen: a
	// pty is never kTTYFocused).
	bool is_pty;
	uint8_t pty_mode;                  // PTY_MODE_* — GRID today, STREAM reserved
	// Seats = tasks whose ->tty this is (the child and everything it spawns,
	// via task_create's inheritance). everSeated arms HUNGUP: a slave that
	// EMPTIED is hung up; one nothing has sat on yet is merely young.
	volatile int32_t seats;
	volatile bool everSeated;
	volatile bool masterClosed;        // the terminal side hung up its handle
	struct tty *next_pty;              // the registry chain (kPtyList)
} tty_t;

// PTY.md's mode seam: the flavor is decided at ONE choke point (tty_write),
// which is what makes STREAM an addition and never a rewrite. GRID is v1;
// STREAM's gate is TCP listen() and its customer is telnetd.
#define PTY_MODE_GRID   0
#define PTY_MODE_STREAM 1   // reserved — bytes to a ring instead of the grid

extern tty_t kTTY[TTY_COUNT];
extern tty_t * volatile kTTYFocused;   // whose grid the glass is showing
extern volatile bool kTTYReady;        // false until tty_init: printf paints
                                       // direct (legacy) before, VT1 grid after

// Resolve a task's terminal. NULL-safe at every level (early boot, kernel
// threads created before tty_init): no tty means the system console, VT1.
tty_t *task_tty(struct task *t);

// The cells of a VISIBLE screen row (0..rows-1), honoring the scrollback
// view; NULL if the row is out of range. THE one place that knows how the
// ring, screen_top and view_offset combine. Caller holds t->lock.
tty_cell_t *tty_visible_line(tty_t *t, uint32_t screen_row);

// Build the grids and take over the console. Call once kmalloc is up —
// right after renderer_attach_shadow, so nearly all boot spew lands in VT1's
// grid. (The handful of pre-init lines exist only as pixels; the first
// switch away from VT1 and back repaints from the grid and they are gone.
// The grid is the truth, and text older than the truth is archaeology.)
void tty_init(void);

// The terminal interpreter: bytes into t's grid, mirrored to the glass iff
// t is focused (and the view isn't scrolled back, and the GUI hasn't taken
// the console, and no panic forced direct mode). One call = one atomic
// paint, same contract print_n always had — EXCEPT during a scroll burst:
// the first scroll marks the glass stale and hands rendering to the repaint
// rider below, so a flood costs the glass ~30Hz grid repaints instead of a
// 3MB shadow memmove per line (the frozen-cat fix, 2026-08-13; the doctrine
// comment above tty.c's s_glassStale tells the whole story).
void tty_write(tty_t *t, const char *bytes, size_t length);

// The repaint rider — tty layer's half of the ~30Hz glass discipline,
// called from processSignals beside renderer_flush_if_dirty. If a write
// burst left the glass stale, repaints the focused terminal from its grid.
void tty_flush_if_dirty(void);

// ── Input (called by tty.c's producers and console.c's consumer) ───────────
// Deliver a translated keystroke to the FOCUSED tty. A dormant tty swallows
// the key and requests its shell instead; a scrolled-back view snaps to the
// present first (a keystroke means "I'm done reading history").
void tty_input_event(const keyboard_event_t *ev);
bool tty_input_has(tty_t *t);
bool tty_input_pop(tty_t *t, keyboard_event_t *ev);
// The ring push alone, aimed at a SPECIFIC tty — the keyboard path above
// wraps it with focus/knock/scrollback policy; a pty master's write is a
// producer with no such ceremony (the terminal app already decided whose
// keystrokes these are).
void tty_input_push(tty_t *t, const keyboard_event_t *ev);
// The same, refusing a full ring instead of dropping the event — for a
// producer that can come back later (the clipboard paste feeds a snarf in
// across frames rather than truncating it). Returns false when full.
bool tty_input_push_if_room(tty_t *t, const keyboard_event_t *ev);

// ── Focus (called from the keyboard drivers' chord intercepts) ─────────────
void tty_focus(uint32_t index);        // Alt+F1..F8 — direct select
void tty_focus_step(int dir);          // Alt+←/→ — walk the ring, wrapping
void tty_view_scroll(int dir);         // Shift+PgUp(+1)/PgDn(-1) — half screens

// ── Shells and the summons ──────────────────────────────────────────────────
// Seat a controlling shell on a tty (LIVE, foreground, the works).
void tty_seat_shell(tty_t *t, struct task *shell);
// Called from the exit path: if the dying task was a tty's seated shell, the
// tty goes dormant and announces how to summon a new one; if it was the
// tty's FOREGROUND job, the console goes back to the shell (a dead task must
// never remain a Ctrl+C target — see the comment in the body).
void tty_task_departed(struct task *t);
// The summons, split across contexts: pending() is the cheap check;
// wake() runs in processSignals (queue lock held) and rousts kworker early
// when a terminal has been knocked on; sweep() runs IN KWORKER (task
// context — task_create loads an ELF from disk, no place for an IRQ) and
// actually spawns a husk on every tty that asked. Returns true if it did.
bool tty_summon_pending(void);
void tty_summon_wake(void);
bool tty_summon_sweep(void);

// Panic escape hatch (called by panic.c alongside renderer_bust_lock): force
// every print onto the legacy direct-to-glass path and bust the tty locks a
// dead core may hold. Panic text lands on whatever terminal is showing —
// which is exactly what you want from a dead system.
void tty_emergency_direct(void);
extern volatile bool kTTYDirect;

// ── The pty family (PTY.md; mechanism here, the syscall skin in syscall.c) ──
// Create a GRID-mode slave: a live tty_t with its own grid + scrollback
// ring, registered on kPtyList (NEVER in kTTY[] — the VT iterators stay
// blind to ptys by construction). Returns NULL on a bad geometry — the
// only refusal (the allocator panics on exhaustion, never returns NULL).
tty_t *pty_create_slave(uint32_t cols, uint32_t rows);

// Resize a grid IN PLACE (the SIGWINCH slice, PTY.md § Resize). The ring is
// reallocated at the new geometry and the old text carried across, then the
// generation bumps so a snapshot poller repaints. Policy, stated so nobody
// files it as a bug: NO REFLOW. Rows keep their left edge (a narrower grid
// clips each line's tail, a wider one blanks the new cells), the cursor is
// clamped into the new bounds, and the view snaps back to the live screen.
// ONE refinement over "preserve the origin": when fewer rows would leave
// the cursor below the glass, the top rows roll into scrollback instead so
// the line being typed stays visible — what xterm does, and the difference
// between a shrink that keeps your prompt and one that eats it. Rewrapping
// logical lines is a scrollback feature and waits for that row.
//
// Grid-only: it never touches the glass, so it is a PTY verb today (the
// syscall gates on is_pty). A VT could use it the day the renderer's cell
// geometry can change underneath one. Returns false on a bad geometry —
// the only refusal there is (the allocator panics on exhaustion rather
// than returning NULL) — and then the grid is untouched.
bool tty_resize_grid(tty_t *t, uint32_t cols, uint32_t rows);

// The master's write half: bytes become synthesized key events into the
// slave's input ring — after 0x03 runs the per-tty interrupt intercept
// against the SLAVE (a windowed Ctrl+C aims at the slave's foreground, not
// the terminal app's). Returns bytes accepted — and that number is HONEST
// (since 2026-08-22): the slave's input ring is small, and when it fills the
// write stops there and says how far it got, pipe-style. Zero is not an
// error, it is "come back later". A consumed 0x03 counts as accepted.
int64_t pty_master_write(tty_t *slave, const char *bytes, size_t length);

// Seat references: every task whose ->tty is this pty holds one (taken at
// inheritance in task_create and at spawn's explicit seating; dropped in
// task teardown). The slave frees itself when the master is closed AND the
// seats are empty — whichever happens last does the burial.
void tty_pty_ref(tty_t *t);          // no-op unless t->is_pty
void tty_pty_unref(tty_t *t);        // no-op unless t->is_pty
void pty_master_close(tty_t *slave); // the handle-table close hook

// console_wake_if_ready's pty leg: wake any slave's parked reader whose ring
// has input. Lives here because the registry walk needs the (private) list
// lock. Caller holds the scheduler queue lock (processSignals context).
void tty_pty_wake_readers(void);

// The registry head (walks require the private list lock — use the sweep
// above; exported for diagnostics only).
extern tty_t * volatile kPtyList;

#endif // TTY_H
