// os64/pty.h — the pseudo-terminal ABI (PTY.md, ratified 2026-08-19).
//
// A pty is a kernel tty with no keyboard and no glass: the MASTER handle's
// holder stands where they stood. GRID mode (v1): the kernel's one terminal
// interpreter fills the slave's grid exactly as it fills a VT's, and the
// master's holder copies the interpreted screen out with pty_snapshot —
// grid in, keys out, which is the whole job of a terminal window. STREAM
// mode is reserved (the byte-ring flavor telnetd will want; its gate is TCP
// listen() — see PTY.md's fork ruling and the future-customer principle).
//
// No /dev names anywhere: masters are handles, slaves are a task's
// controlling terminal (task->tty). A future devfs is a naming layer over
// this, never a redesign.
//
// The verbs:
//   create   — SYSCALL_PTY_CREATE(cols, rows) -> master handle
//   seat     — spawn with OS64_SPAWN_SET_TTY | (master << OS64_SPAWN_TTY_SHIFT)
//   keys in  — plain write(master, bytes): each byte becomes a keystroke on
//              the slave; 0x03 runs the slave's Ctrl+C intercept and aims
//              SIGINT at the SLAVE's foreground (the program in the window),
//              never at the master's holder
//   screen   — SYSCALL_PTY_SNAPSHOT: header + interpreted cells, gated by a
//              generation counter so a frame-cadence poll is near-free
//   read()   — reserved for STREAM mode; in GRID mode it refuses (a grid is
//              not a stream, and pretending would teach the wrong lesson)
//   resize   — SYSCALL_PTY_RESIZE(master, cols, rows): the grid follows the
//              window, and every task seated on the slave that installed a
//              SIGWINCH handler gets the signal (the rest are not disturbed).
//              The program inside asks /proc/self/tty what the size is now

#ifndef OS64_PTY_H
#define OS64_PTY_H

#include <stdint.h>
#include <stddef.h>
#include "os64/syscall_numbers.h"
#include "os64/syscall.h"
#include "os64/ansi.h"   // what a cell's attributes and background byte mean

// ── the snapshot ────────────────────────────────────────────────────────────

// Snapshot flags.
#define OS64_PTY_HUNGUP 0x1   // the slave SEATED tasks once and now seats none
                              // — the session ended; a slave nothing has sat
                              // on yet is merely young, not hung up

typedef struct os64_pty_header
{
	uint32_t cols, rows;          // the live screen's geometry
	uint32_t cur_row, cur_col;    // cursor, in that screen
	uint64_t generation;          // bumps on every grid write — poll THIS
	uint32_t flags;               // OS64_PTY_*
	uint32_t _reserved;
} os64_pty_header_t;

// One interpreted cell — layout pinned to the kernel's tty_cell_t (the
// kernel static-asserts the match, the ext2-superblock trick): the glyph, 3
// pad bytes, the XRGB color it was written in.
// THE THREE BYTES AFTER THE GLYPH WERE PADDING AND ARE NOW STATE. The size
// is unchanged and so is the static assert below — what changed is that the
// bytes mean something, so a renderer that ignores them (an older gterm
// against a newer kernel) simply draws without attributes, and a kernel that
// never sets them (the reverse pairing) leaves zeros, which mean "plain".
// The two ends can therefore be updated in either order.
typedef struct os64_pty_cell
{
	char     ch;                  // 0 = never written (render as blank)
	uint8_t  attrs;               // OS64_ANSI_ATTR_* (os64/ansi.h)
	uint8_t  bg;                  // palette index+1; 0 = the terminal's own
	uint8_t  _pad;
	uint32_t color;               // foreground, XRGB
} os64_pty_cell_t;

_Static_assert(sizeof(os64_pty_cell_t) == 8, "pty cell ABI: 8 bytes");
_Static_assert(sizeof(os64_pty_header_t) == 32, "pty header ABI: 32 bytes");

// ── the calls ───────────────────────────────────────────────────────────────

// Create a GRID-mode pty sized cols x rows. Returns the master handle
// (>= 0), or a negative syscall error. Close it with os64_close like any
// handle; keystrokes go in with plain os64_write on it.
static inline int64_t os64_pty_create(uint32_t cols, uint32_t rows)
{
	return (int64_t)os64_syscall2(SYSCALL_PTY_CREATE, cols, rows);
}

// Copy the slave's live screen: header always, cells up to max_cells (size
// the buffer from a header-only probe, or just cols*rows once known).
// max_cells == 0 is the cheap poll: header only, no cell copy — compare
// header.generation against the last one you rendered.
// Returns cells copied (0 for a header-only probe), or a negative error.
static inline int64_t os64_pty_snapshot(int64_t master,
                                        os64_pty_header_t *hdr,
                                        os64_pty_cell_t *cells,
                                        uint32_t max_cells)
{
	return (int64_t)os64_syscall4(SYSCALL_PTY_SNAPSHOT, (uint64_t)master,
	                              (uint64_t)hdr, (uint64_t)cells, max_cells);
}

// Tell the slave its new size. The grid is reallocated with the text carried
// across (left edges kept, no reflow, cursor clamped — and if a shorter
// screen would hide the cursor, the top rows roll into scrollback so the
// line being typed stays on the glass), the generation bumps so your own
// snapshot poll repaints, and every task seated on the slave that installed
// a SIGWINCH handler gets the signal — a seat without one is not disturbed,
// and nothing is left pending for a handler it installs later. Same fence
// as create (2..512 x 2..256). Returns 0, or a negative error — and on
// error the grid is exactly as it was.
static inline int64_t os64_pty_resize(int64_t master, uint32_t cols, uint32_t rows)
{
	return (int64_t)os64_syscall3(SYSCALL_PTY_RESIZE, (uint64_t)master, cols, rows);
}

#endif // OS64_PTY_H
