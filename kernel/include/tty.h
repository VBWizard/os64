#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "driver/system/keyboard.h"   // keyboard_event_t — the input ring's coin
#include "spinlock.h"
#include "ansi.h"                     // the escape reader's state, held per tty
#include "os64/ansi.h"                // the palette both renderers paint from
#include "os64/charset.h"             // and which bitmap a byte over 0x7F draws as

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

#include "tty_cell.h"                  // tty_cell_t — one character cell of a grid

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
	tty_cell_t *cells;                 // GRID cell ring; NULL for STREAM
	uint32_t cols, rows;               // live-screen geometry (glass cells)
	uint32_t total_lines;              // GRID ring height; zero for STREAM
	uint32_t screen_top;               // ring index of live row 0
	uint32_t hist_lines;               // valid history lines above screen_top
	uint32_t view_offset;              // >0 = viewing history, this many lines up
	uint32_t cur_row, cur_col;         // cursor, relative to screen_top
	// Where SCP put the cursor away, for RCP to take it back out. POSITION
	// ONLY — the pen below is not saved with it, which is what ANSI.SYS's
	// pair has always meant and what the programs using it expect. Zero is a
	// good starting value: a restore with no save homes the cursor, the way
	// every terminal answers that question.
	uint32_t save_row, save_col;
	uint32_t color;                    // current write color (foreground)

	// ── What an escape sequence has said (guarded by `lock`) ───────────────
	// The pen: every cell written takes these until something changes them.
	// `glass_bg` is the terminal's OWN background — what a cell with no
	// background of its own is painted on, what form feed restores,
	// and what the margins beyond the last cell show. It is PER TTY, so VT2
	// can be a different colour from VT1, and a gterm from both.
	uint8_t  attrs;                    // OS64_ANSI_ATTR_*
	uint8_t  bg;                       // palette index+1; 0 = glass_bg
	// What a byte over 0x7F DRAWS as on this terminal — OS64_CHARSET_LATIN1
	// or _CP437, chosen by the program with `ESC ( B` / `ESC ( U`. Per tty
	// because os64 has both kinds of consumer at once: a gopher menu from
	// 1994 is Latin-1 and a BBS's art is CP437, and a machine-wide answer
	// would have to be wrong for one of them.
	uint8_t  charset;
	uint8_t  fg_index;                 // which palette entry `color` came from,
	                                   // or TTY_FG_NOT_INDEXED — kept so that a
	                                   // later bold can brighten a colour chosen
	                                   // before it, as a real terminal does
	uint32_t glass_bg;                 // XRGB (OSC 11)
	ansi_parser_t ansi;                // mid-sequence state, across writes
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
	// THE FOREGROUND'S MUTATION LOCK. Every write to fgTask (and to shell)
	// happens under it, and every check that decides a write ("is the
	// pointer still me?", "does it name a live child of mine?") is made
	// inside the same hold — so the hand-offs at a spawn, a wait's entry
	// and finish, a task's departure and a seat are each one indivisible
	// transition, never a check on one core and a store on another. A leaf
	// lock, irqsave, held for a pointer compare and a task-list walk and
	// nothing else; readers (the Ctrl+C intercept in IRQ context, the mode
	// query, /proc) read the pointer bare — a single aligned word.
	spinlock_t fg_lock;
	// Raw mode has no word here on purpose: the terminal is raw exactly
	// while fgTask->wantsRaw (task.h) — derived, never stored, so there is
	// no holder to publish, transfer, or clear at a death (SIGINT.md § Raw
	// mode). Reading it: console_tty_raw.

	// ── The summons (dormant ttys only) ─────────────────────────────────────
	volatile tty_state_t state;
	volatile bool spawnRequested;      // set by a keystroke, served by kworker

	// ── Change tracking (all ttys; PTY.md's snapshot poll reads it) ─────────
	// Bumped on grid mutations and geometry changes. A GRID master polls at
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
	uint8_t pty_mode;                  // PTY_MODE_*
	// STREAM mode's way out: a pipe. A seated task's console write is a
	// pipe_write into it (the syscall layer does that, so the write blocks
	// and ends like any pipe write); the master's read is a pipe_read. Its
	// write end closes when the seats empty (EOF to the master: the session
	// ended), its read end when the master closes (EPIPE to a child that
	// writes after). KERNEL text aimed at a STREAM slave — a death headline
	// from the exception path — may not park, so tty_write pushes what fits
	// and counts the rest here: the one byte this design chooses to lose.
	struct pipe *stream;
	uint64_t stream_dropped;
	// Each end of `stream` is closed exactly once: the write end when the
	// occupied seats and reservations empty (tty_pty_unref), or an unused
	// master's close; the read end closes with the master. The pipe's holds
	// keep it alive after its ends close while operations finish.
	volatile bool stream_writer_closed;
	volatile bool stream_reader_closed;
	// Seats count tasks, pending spawn reservations and the master-close
	// guard. everSeated arms HUNGUP: a slave that
	// EMPTIED is hung up; one nothing has sat on yet is merely young.
	volatile int32_t seats;
	volatile bool everSeated;
	volatile bool masterClosed;        // the terminal side hung up its handle
	// Operations INSIDE the slave, from either side: a master-side read
	// parked on the stream, a resize, a snapshot (pty_master_hold, the
	// pin's — handle.c § The pin), and a seated task's console read or
	// write (pty_seat_hold). Burial waits for zero. NOT a seat: a seat held
	// by the very reader waiting for the seats to empty would defer the EOF
	// it is waiting for, forever.
	volatile int32_t holds;
	struct tty *next_pty;              // the registry chain (kPtyList)
} tty_t;

// PTY.md's mode seam: the flavor is decided at ONE choke point (tty_write),
// which is what makes STREAM an addition and never a rewrite. GRID feeds
// the interpreter and the grid; STREAM (SERVERS.md § 2) hands the child's
// bytes to `stream`, a pipe the master reads — a pipe wearing a tty's
// identity, so the blocking, the EOF and the EPIPE rules are pipe.c's.
// The flavor is NOT an ABI value: it is the syscall number — pty_create (44)
// for GRID, pty_create_stream (56) for STREAM — so 44 never widened its
// two-argument contract for a mode (Codex #101 P1). These are the kernel's
// internal names for what each syscall passes to pty_create_slave.
#define PTY_MODE_GRID   0
#define PTY_MODE_STREAM 1

extern tty_t kTTY[TTY_COUNT];
extern tty_t * volatile kTTYFocused;   // whose grid the glass is showing

// Is this one of the VT fleet? Answered by POINTER RANGE, not by reading a
// field: the fleet is a static array that is never freed, and anything else
// is a pty slave that may already be buried — a terminal of record can be
// freed by its task's own teardown while a sibling thread is still entering
// a console read. A caller that wants to know must ask this first, take the
// seat hold if the answer is no, and only then read the slave's fields.
static inline bool tty_is_vt(const tty_t *t)
{
	return t >= &kTTY[0] && t < &kTTY[TTY_COUNT];
}
extern volatile bool kTTYReady;        // false until tty_init: printf paints
                                       // direct (legacy) before, VT1 grid after

// Resolve a task's terminal. NULL-safe at every level (early boot, kernel
// threads created before tty_init): no tty means the system console, VT1.
tty_t *task_tty(struct task *t);

// The cells of a VISIBLE screen row (0..rows-1), honoring the scrollback
// view; NULL if the row is out of range. THE one place that knows how the
// ring, screen_top and view_offset combine. Caller holds t->lock.
tty_cell_t *tty_visible_line(tty_t *t, uint32_t screen_row);
// Resolve a cell for painting, before selection overlays. Caller holds t->lock.
void tty_cell_colors(const tty_t *t, const tty_cell_t *cell,
                     uint32_t *fg, uint32_t *bg);

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
// Deliver a translated keystroke to the FOCUSED tty — read ONCE, and that
// one terminal answers everything: the interrupt-character veto (a 0x03 on
// a cooked terminal becomes SIGINT here and never enters the ring), the
// knock (a dormant tty swallows the key and requests its shell), and the
// scrollback snap (a scrolled-back view returns to the present first — a
// keystroke means "I'm done reading history").
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

// ── Raw mode (SIGINT.md § Raw mode) ────────────────────────────────────────
// Record a task's wish for its terminal: raw or cooked. The wish is the
// task's own (task.h wantsRaw) and takes effect exactly while the task is
// the terminal's foreground — so only the foreground may ask (a background
// job's wish would be a lie about the seat), and a wish set by a task that
// is not the foreground changes nothing. Returns 0, or -1 when the caller
// is not the foreground.
int tty_set_raw(tty_t *t, struct task *caller, bool raw);

// ── Focus (called from the keyboard drivers' chord intercepts) ─────────────
void tty_focus(uint32_t index);        // Alt+F1..F8 — direct select
void tty_focus_step(int dir);          // Alt+←/→ — walk the ring, wrapping
void tty_view_scroll(int dir);         // Shift+PgUp(+1)/PgDn(-1) — half screens

// ── Shells and the summons ──────────────────────────────────────────────────
// Seat a controlling shell on a tty (LIVE, foreground, the works).
// For a pty, the caller transfers an owned seat reservation to the shell.
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
// Create a registered slave with input and geometry. GRID owns a cell ring;
// STREAM owns an output pipe and has no cells. Returns NULL for an invalid
// geometry or mode. The allocator panics on exhaustion.
tty_t *pty_create_slave(uint32_t cols, uint32_t rows, uint8_t mode);

// Resize geometry without touching the glass. STREAM updates dimensions;
// GRID carries text without reflow, keeps each line's left edge, clamps the
// cursor and returns to the live view. Shrinking below the cursor rolls the
// top rows into history so the current line stays visible.
// Returns 1 for a change (generation bumped), 0 for unchanged dimensions,
// or -1 for invalid geometry (no mutation). Caller must keep t alive.
int tty_resize(tty_t *t, uint32_t cols, uint32_t rows);

// Re-shape a GRID terminal and KEEP ITS TEXT — the carrier for a console font
// change, where nobody repaints (CONSOLE_FONTS.md § The contents survive).
// tty_resize's opposite in policy: long lines wrap and re-join through
// tty_reflow.h instead of losing their right-hand end, the scrolled-back view
// keeps its line, and the ring is never smaller than the scrollback it would
// have been born with. Touches no glass; the caller repaints.
//
// What the reshape could not carry is COUNTED into *dropped (oldest history,
// past TTY_REFONT_MAX_LINES) and *clipped (rows under the cursor on a shorter
// screen); either pointer may be NULL. Returns like tty_resize: 1 changed,
// 0 already that shape, -1 refused (geometry outside the fence, or a STREAM
// terminal, which has no cells). Task context: it allocates.
#define TTY_REFONT_MAX_LINES 8192u
int tty_refont(tty_t *t, uint32_t cols, uint32_t rows,
               uint32_t *dropped, uint32_t *clipped);

// Repaint whichever terminal has the glass, from its grid. For a caller that
// changed what the glass should show without writing to a terminal.
void tty_repaint_focused(void);

// The master's write half: bytes become synthesized key events into the
// slave's input ring — after 0x03 runs the per-tty interrupt intercept
// against the SLAVE (a windowed Ctrl+C aims at the slave's foreground, not
// the terminal app's). Returns bytes accepted — and that number is HONEST
// (since 2026-08-22): the slave's input ring is small, and when it fills the
// write stops there and says how far it got, pipe-style. Zero is not an
// error, it is "come back later". A consumed 0x03 counts as accepted.
int64_t pty_master_write(tty_t *slave, const char *bytes, size_t length);

// Seat references: every task whose ->tty is this pty holds one (taken at
// inheritance in task_create or reserved before spawn's explicit seating;
// dropped in task teardown). The slave frees itself when the master is
// closed AND the seats are empty AND no operation is inside it (holds)
// — whichever of the three happens last does the burial.
// tty_pty_ref takes the seat BY POINTER against the registry, under its
// lock, and answers false for an unlisted slave, a closed master or a STREAM
// whose writer has closed. A parent whose terminal a child inherits can be
// torn down by a sibling while the spawn is in flight, and its slave buried.
// A VT is always true.
bool tty_pty_ref(tty_t *t);          // no-op (true) unless t is a pty
// Counts a future seat without marking the pty as previously occupied.
// Commit with tty_seat_shell after loading, or cancel with tty_pty_unref.
// A failed first load leaves the STREAM writer open for a later attempt.
bool tty_pty_reserve_seat(tty_t *t);
void tty_pty_unref(tty_t *t);        // no-op unless t->is_pty

// A task's terminal of record, HELD: the VT fleet needs no hold, a pty
// slave gets the seat hold (pty_seat_hold below) — or NULL when the slave is
// already buried, which a caller reads as "the line is dead". Every reader
// of a task's terminal that is not already holding it goes through this
// pair, because a task's own seat stops protecting the slave the moment a
// sibling thread's teardown drops it (handle.c § The pin, met on the
// terminal). Release with task_tty_release; both are no-ops for a VT.
tty_t *task_tty_hold(struct task *t);
void task_tty_release(tty_t *tty);
void pty_master_close(tty_t *slave); // the handle-table close hook
// The pin's hold on a master (handle.c § The pin): the slave stays unburied
// and its stream keeps its read end while a master-side operation is inside
// it. The stream's read end is held because the master's read IS a
// pipe_read on it, and the master's close may close that end underneath.
// Taken before publishing a new master, or while its handle is locked live.
void pty_master_hold(tty_t *slave);
void pty_master_unhold(tty_t *slave);
// The SEAT side's hold, for a seated task's console read or write on the
// slave: the task's seat keeps the slave alive only until the task's own
// teardown drops it, and a sibling thread parked in the read can outlive
// that by a scheduler pass. Taken by pointer against the registry under its
// lock — burial unlinks under the same lock — so a slave already buried is
// simply not found, and the answer is false: the line is dead.
bool pty_seat_hold(tty_t *slave);
void pty_seat_unhold(tty_t *slave);

// console_wake_if_ready's pty leg: wake any slave's parked reader whose ring
// has input. Lives here because the registry walk needs the (private) list
// lock. Caller holds the scheduler queue lock (processSignals context).
void tty_pty_wake_readers(void);

// The registry head (walks require the private list lock — use the sweep
// above; exported for diagnostics only).
extern tty_t * volatile kPtyList;

#endif // TTY_H
