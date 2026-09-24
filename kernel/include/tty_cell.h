#ifndef TTY_CELL_H
#define TTY_CELL_H

// tty_cell.h — one character cell of a terminal's grid. Its own header, with
// nothing but stdint behind it, because tty_reflow.c works on cells and is
// built on the HOST (tools/test_tty_reflow_host.sh), where tty.h's spinlocks
// and keyboard rings do not exist.

#include <stdint.h>

// One character cell: the glyph, how it was painted, and the colours it was
// painted in. 8 bytes — at 1080p that is ~½MB per tty, ~4MB for the fleet,
// which is the cheapest possible price for repaint-from-state plus
// scrollback.
//
// An attribute byte and a background index keep the cell at 8 bytes. Its
// size and field offsets are checked against os64_pty_cell_t in syscall.c
// because gterm renders these same cells in ring 3. The index represents
// this implementation's sixteen-colour SGR subset plus default paper;
// extended indexed and RGB SGR colours are consumed without applying them.
// A second full XRGB would increase each cell's size and scrollback cost.
typedef struct tty_cell
{
	char ch;                           // 0 = blank
	uint8_t attrs;                     // OS64_ANSI_ATTR_* (bold, reverse), TTY_ATTR_WRAPPED
	uint8_t bg;                        // palette index+1; 0 = the tty's own
	uint8_t charset;                   // OS64_CHARSET_* this byte was written under
	uint32_t color;                    // foreground, XRGB
} tty_cell_t;

// THIS ROW CONTINUES THE ONE ABOVE IT. Set on a row's FIRST cell by
// tty_reflow when a line too long for a narrower grid is laid onto more than
// one row, and read by the next reflow to put the line back together — which
// is what makes a font change reversible instead of leaving the scrollback
// chopped at the narrowest width it ever had. It is the terminal's own
// bookkeeping, not a way of painting: os64_ansi_apply_attrs does not look at
// it, and a program that rewrites the cell writes its own attributes over
// it, which is right — a row somebody has rewritten is no longer the tail of
// the line above.
#define TTY_ATTR_WRAPPED 0x80

#endif // TTY_CELL_H
