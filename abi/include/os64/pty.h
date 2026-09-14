// os64/pty.h — the pseudo-terminal ABI (PTY.md, ratified 2026-08-19).
//
// A pty is a kernel tty with no keyboard and no glass: the MASTER handle's
// holder stands where they stood. Two flavors, chosen at create and
// differing only in the way OUT (PTY.md's fork ruling; SERVERS.md):
//   GRID   — the kernel's one terminal interpreter fills the slave's grid
//            exactly as it fills a VT's, and the master's holder copies the
//            interpreted screen out with pty_snapshot: grid in, keys out,
//            the whole job of a terminal WINDOW.
//   STREAM — the child's bytes reach read(master) uninterpreted, a pipe
//            wearing a tty's identity: what a REMOTE terminal wants, since
//            the rendering happens at its end (telnetd, sshd).
//
// No /dev names anywhere: masters are handles, slaves are a task's
// controlling terminal (task->tty). A future devfs is a naming layer over
// this, never a redesign.
//
// The verbs:
//   create   — SYSCALL_PTY_CREATE(cols, rows) -> a GRID master handle;
//              SYSCALL_PTY_CREATE_STREAM(cols, rows) -> a STREAM one
//   seat     — spawn with OS64_SPAWN_SET_TTY | (master << OS64_SPAWN_TTY_SHIFT)
//   keys in  — plain write(master, bytes), BOTH modes: each byte becomes a
//              keystroke on the slave; 0x03 runs the slave's Ctrl+C
//              intercept and aims SIGINT at the SLAVE's foreground (the
//              program in the window), never at the master's holder
//   screen   — SYSCALL_PTY_SNAPSHOT, GRID only: header + interpreted cells,
//              gated by a generation counter so a frame-cadence poll is
//              near-free. A STREAM pty refuses it (a byte stream has no
//              screen)
//   read()   — STREAM only: the child's output, bytes, blocking like a pipe
//              read; 0 once the slave's seats have emptied — THE SESSION
//              ENDED, the stream spelling of OS64_PTY_HUNGUP. A GRID pty
//              refuses it (a grid is not a stream, and pretending would
//              teach the wrong lesson)
//   resize   — SYSCALL_PTY_RESIZE(master, cols, rows), BOTH modes: the
//              geometry follows the window. When it changes, each task on the
//              slave that installed a SIGWINCH handler gets the signal (the
//              rest are not disturbed). The program inside asks
//              /proc/self/tty what the size is now

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

// One interpreted cell: glyph, attributes, background index, the character
// SET the glyph was written under, and the XRGB foreground. syscall.c checks
// the size and field offsets against the kernel's tty_cell_t because
// snapshots copy these bytes directly.
typedef struct os64_pty_cell
{
	char     ch;                  // 0 = blank
	uint8_t  attrs;               // OS64_ANSI_ATTR_* (os64/ansi.h)
	uint8_t  bg;                  // palette index+1; 0 = the terminal's own
	// WHICH SET THIS BYTE WAS WRITTEN UNDER (OS64_CHARSET_*, os64/charset.h)
	// — a property of the CELL and not of the terminal, so that a repaint,
	// a scroll back through history, and gterm's own painter all draw what
	// the program actually wrote rather than what the terminal was told
	// most recently. It cost nothing: this byte was padding.
	uint8_t  charset;
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

// Create a STREAM-mode pty: same handle, same seating, same keystrokes in —
// but the child's output comes back out of os64_read on the master as the
// bytes it wrote, uninterpreted, and pty_snapshot is refused.
// After its seated tasks and pending seats empty, output drains to EOF;
// spawn_seated then refuses that master. Create a fresh pty for a new session.
// Geometry is what /proc/self/tty reports to the child and what
// os64_pty_resize changes. The FLAVOR is a SEPARATE SYSCALL, not an argument
// on pty_create, so the two-argument GRID call never had its ABI widened.
static inline int64_t os64_pty_create_stream(uint32_t cols, uint32_t rows)
{
	return (int64_t)os64_syscall2(SYSCALL_PTY_CREATE_STREAM, cols, rows);
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
