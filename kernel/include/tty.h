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
} tty_t;

extern tty_t kTTY[TTY_COUNT];
extern tty_t * volatile kTTYFocused;   // whose grid the glass is showing
extern volatile bool kTTYReady;        // false until tty_init: printf paints
                                       // direct (legacy) before, VT1 grid after

// Resolve a task's terminal. NULL-safe at every level (early boot, kernel
// threads created before tty_init): no tty means the system console, VT1.
tty_t *task_tty(struct task *t);

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

// ── Focus (called from the keyboard drivers' chord intercepts) ─────────────
void tty_focus(uint32_t index);        // Alt+F1..F8 — direct select
void tty_focus_step(int dir);          // Alt+←/→ — walk the ring, wrapping
void tty_view_scroll(int dir);         // Shift+PgUp(+1)/PgDn(-1) — half screens

// ── Shells and the summons ──────────────────────────────────────────────────
// Seat a controlling shell on a tty (LIVE, foreground, the works).
void tty_seat_shell(tty_t *t, struct task *shell);
// Called from task_exit_finish: if the dying task was a tty's seated shell,
// the tty goes dormant and announces how to summon a new one.
void tty_shell_departed(struct task *t);
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

#endif // TTY_H
