#ifndef OS64_ANSI_H
#define OS64_ANSI_H

// ansi.h — the sixteen colours an escape sequence can name, and the
// attributes a cell can carry. ONE copy, on the ABI shelf, for the reason
// klog_format.h is here: both sides of the ring boundary render these cells.
// The kernel paints the glass through BasicRenderer; gterm paints the same
// cells in ring 3 out of the PTY grid. Two palettes would mean one terminal
// showing a different blue from the other, and the difference would be
// invisible until somebody put them side by side.
//
// WHY AN INDEX AND NOT A COLOUR. A tty cell is eight bytes — glyph,
// attributes, background INDEX, and a 32-bit foreground — and the eight is
// pinned by a static assert against the PTY grid's ABI. A second full XRGB
// would not fit without doubling the fleet's scrollback (~4MB to ~8MB), and
// it would buy nothing: an SGR sequence can only NAME sixteen backgrounds,
// so sixteen is the whole of what a program can ask for. The foreground
// stays a full XRGB because the kernel wrote colours there long before
// escape sequences existed, and nothing is gained by narrowing it.

#include <stdint.h>

// The palette is xterm's, which is what the world's terminals, BBSes and
// text browsers were coloured against. The VGA set is dimmer and equally
// defensible; this one is what somebody's ANSI art was drawn to look like.
#define OS64_ANSI_COLORS 16

// Reached through a function rather than as a bare table, so that every file
// including this header does not have to USE it: a static array at file
// scope is an unused variable in most of them, and this tree builds with
// -Werror.
static inline uint32_t os64_ansi_color(uint8_t index)
{
    static const uint32_t palette[OS64_ANSI_COLORS] = {
        0x000000,  //  0 black
        0xcd0000,  //  1 red
        0x00cd00,  //  2 green
        0xcdcd00,  //  3 yellow
        0x0000ee,  //  4 blue
        0xcd00cd,  //  5 magenta
        0x00cdcd,  //  6 cyan
        0xe5e5e5,  //  7 white
        0x7f7f7f,  //  8 bright black (grey)
        0xff0000,  //  9 bright red
        0x00ff00,  // 10 bright green
        0xffff00,  // 11 bright yellow
        0x5c5cff,  // 12 bright blue
        0xff00ff,  // 13 bright magenta
        0x00ffff,  // 14 bright cyan
        0xffffff,  // 15 bright white
    };
    return palette[index & (OS64_ANSI_COLORS - 1)];
}

// A CELL'S BACKGROUND IS STORED WITH AN OFFSET OF ONE, and this is the kind
// of off-by-one that has to be shouted rather than mentioned: zero means THE
// TERMINAL'S OWN BACKGROUND, not black. Every cell of every tty starts as
// eight zero bytes (a line is cleared with memset), so zero has to mean
// "nothing was ever said about this cell" — and a screen full of cells that
// each insisted on black would be a screen that ignored the background the
// terminal was set to.
#define OS64_ANSI_BG_DEFAULT 0
#define OS64_ANSI_BG_INDEX(i) ((uint8_t)((i) + 1))

// Resolve a stored background byte to a colour. `fallback` is the terminal's
// own background — what OS64_ANSI_BG_DEFAULT means.
static inline uint32_t os64_ansi_bg_color(uint8_t stored, uint32_t fallback)
{
    if (stored == OS64_ANSI_BG_DEFAULT || stored > OS64_ANSI_COLORS)
        return fallback;
    return os64_ansi_color((uint8_t)(stored - 1));
}

// What a cell can be besides coloured. BOLD brightens the foreground rather
// than thickening the glyph, which is what every terminal since the VT100
// has done with a font that has no bold cut — os64's PSF1 font has one
// weight, so "bold" means "the bright half of the palette" here, honestly.
//
// AND IT IS A WEAK SIGNAL, which is worth knowing before a program leans on
// it. The bright half of this palette is close to the dim half at the top
// end: white to bright white is 0xe5e5e5 to 0xffffff, and red to bright red
// is 0xcd0000 to 0xff0000. Bold reads as emphasis on a word; it does NOT
// reliably separate two KINDS of text down a whole screen. A program that
// needs that wants two palette entries with a gap between them — grey
// against white — or two different hues. (/bin/gopher marked its links bold
// and they were indistinguishable from the prose beneath them; Chris spotted
// it against /tests/ansiprobe's own attributes line.)
#define OS64_ANSI_ATTR_BOLD    0x01
#define OS64_ANSI_ATTR_REVERSE 0x02   // swap foreground and background

// Apply the attributes to a resolved pair. Called by BOTH renderers, so
// reverse video cannot mean one thing on the glass and another in a gterm.
static inline void os64_ansi_apply_attrs(uint8_t attrs, uint32_t *fg, uint32_t *bg)
{
    if (attrs & OS64_ANSI_ATTR_REVERSE) {
        uint32_t swap = *fg;
        *fg = *bg;
        *bg = swap;
    }
}

#endif // OS64_ANSI_H
