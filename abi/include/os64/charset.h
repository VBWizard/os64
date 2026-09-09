#ifndef OS64_CHARSET_H
#define OS64_CHARSET_H

// charset.h — what a byte over 0x7F draws as.
//
// ONE copy, on the ABI shelf, for the reason ansi.h is here: BOTH SIDES OF
// THE RING BOUNDARY DRAW THESE BYTES. The kernel paints the glass through
// BasicRenderer; gterm paints the same cells in ring 3 out of the PTY grid.
// Two tables would mean one terminal showing a block where the other shows
// an accented capital, and the difference would be invisible until somebody
// put them side by side.
//
// ── Why a terminal needs more than one answer ───────────────────────────
//
// Below 0x80 every encoding agrees and this file has nothing to say. Above
// it, os64 has two consumers that want different things from the same byte,
// and neither is wrong:
//
//   LATIN-1 is what the console has always drawn, because the renderer
//   indexes the font by the byte and the shipped face is ordered that way.
//   It has a real consumer: a gopher menu written by somebody in 1994 is
//   Latin-1, and /bin/gopher deliberately passes those bytes through
//   untouched (wire.h: "the renderer's font decides what a byte over 0x7F
//   looks like").
//
//   CP437 is what ANSI art is drawn in, and has been since the BBS days —
//   0xB0 through 0xDF are the shades, the half blocks and the box-drawing
//   set that the whole art form is made of. A board's opening screen is
//   thousands of those bytes.
//
// So the charset is PER TERMINAL and a program declares it: `ESC ( U` for
// CP437, `ESC ( B` to go back, which is the Linux console's own spelling
// and a sequence os64's parser already consumed. VT1 can be showing a
// Latin-1 gopher menu while VT2 shows a CP437 board.
//
// ── The table is INDICES, and it is checked ─────────────────────────────
//
// A glyph is found by codepoint in the shipped face's own Unicode table, so
// the numbers below are that face's indices and nothing more general. They
// are written down rather than derived because gterm's embedded font has no
// Unicode table to derive from (font_psf1.h: "the source file's unicode
// table is deliberately NOT embedded"), and a table only one of the two
// painters could build is exactly the disagreement this header exists to
// prevent.
//
// Written down is not the same as trusted: the KERNEL re-derives every entry
// from its font's Unicode table at boot and says so loudly if one moved
// (charset_verify, video.c). A face that shifts a glyph would otherwise
// change what a board's art looks like with nothing to point at.

#include <stdint.h>

#define OS64_CHARSET_LATIN1  0   // the byte IS the glyph index — the old behaviour
#define OS64_CHARSET_CP437   1   // the byte is a CP437 code point

// A glyph this face does not carry. Five of them are supplied below because
// ANSI art cannot be drawn without them; the rest draw blank, which reads as
// "missing" rather than as corruption. Approximating them was rejected for
// the reason tty.c rejects drawing blink as bold: it is a lie about what the
// program asked for, and a wrong box-drawing character is as broken as none.
#define OS64_CHARSET_NONE    0xFFFF
#define OS64_CHARSET_BUILTIN 0x0100   // + n: one of the bitmaps below

// Which glyph of the shipped face draws this CP437 byte — an index, or one
// of the two sentinels above. Its own function so that the boot-time check
// can ask the same question the painter asks, of the same table.
static inline uint16_t os64_charset_entry(uint8_t byte)
{
    // The high half of CP437, as indices into the shipped face. Generated
    // from that face's Unicode table by way of the CP437 code page; the
    // kernel checks it back at boot.
    static const uint16_t cp437[128] = {
              135,       252,       233,       226,       228,       224,       229,       231,
              234,       235,       232,       239,       238,       236,       132,       133,
              137,       230,       134,       244,       246,       242,       251,       249,
              255,       150,       156,       162,       163,       165,    0xFFFF,        13,
              225,       237,       243,       250,       241,       145,       170,       186,
              191,    0xFFFF,       172,         8,         7,       161,       171,       187,
              203,       160, 0x0100 + 0,      193,       199,    0xFFFF,    0xFFFF,    0xFFFF,
           0xFFFF,       215,       209,       211,       213,    0xFFFF,    0xFFFF,       195,
              196,       201,       200,       198,       192,       202,    0xFFFF,    0xFFFF,
              212,       210,       217,       216,       214,       208,       218,    0xFFFF,
           0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,
           0xFFFF,       197,       194,       219, 0x0100 + 2, 0x0100 + 3, 0x0100 + 4, 0x0100 + 1,
           0xFFFF,       159,    0xFFFF,         1,    0xFFFF,    0xFFFF,       181,    0xFFFF,
           0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,    0xFFFF,
           0xFFFF,       177,         4,         3,    0xFFFF,    0xFFFF,       247,    0xFFFF,
              176,       127,       183,    0xFFFF,    0xFFFF,       178,         5,        32,
    };

    return byte < 0x80 ? byte : cp437[byte - 0x80];
}

// Resolve one byte to the CHARSIZE bytes of bitmap that draw it.
//
// `glyphs`/`nglyphs`/`charsize` describe the caller's own face — the kernel's
// loaded PSF1, or gterm's embedded one. Always returns something drawable.
static inline const uint8_t *os64_charset_glyph(uint8_t byte, uint8_t charset,
                                                const uint8_t *glyphs,
                                                uint32_t nglyphs,
                                                uint32_t charsize)
{
    // THE FIVE THE FACE DOES NOT CARRY, and they are the ones ANSI art is
    // mostly MADE of — in one captured board screen the shades and half
    // blocks were 2,357 of 5,142 high bytes. Pure geometry, so nothing is
    // being drawn by hand here: the halves are exact fills, and the dark
    // shade is the arithmetic inverse of the light shade the face already
    // has (0x88/0x22 inverts to 0x77/0xDD), which is what puts it in the
    // same family as its neighbours — 2 lit pixels per row, then 4, then 6,
    // then 8.
    static const uint8_t builtin[5][16] = {
        {   // 0: U+2593 dark shade
            0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD,
            0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD,
        },
        {   // 1: U+2580 upper half
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        },
        {   // 2: U+2584 lower half
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        },
        {   // 3: U+258C left half
            0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
            0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
        },
        {   // 4: U+2590 right half
            0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
            0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        },
    };
    static const uint8_t blank[16] = { 0 };

    const uint8_t *own = glyphs + (uint32_t)byte * charsize;

    // The supplied bitmaps are 8x16 because that is the cell every os64
    // terminal draws. A face of another size gets the old answer rather than
    // a glyph of the wrong height.
    if (charset != OS64_CHARSET_CP437 || byte < 0x80 || charsize != 16)
        return byte < nglyphs ? own : glyphs;

    uint16_t entry = os64_charset_entry(byte);
    if (entry == OS64_CHARSET_NONE)
        return blank;
    if (entry >= OS64_CHARSET_BUILTIN)
    {
        // The sentinel space is not the face's, which is what caps a face
        // index at the first 256 glyphs — the shipped face needs no more.
        // A sentinel past the bitmaps that exist draws blank rather than
        // reading whatever the linker put after them.
        uint16_t which = (uint16_t)(entry - OS64_CHARSET_BUILTIN);
        return which < sizeof(builtin) / sizeof(builtin[0]) ? builtin[which] : blank;
    }
    return entry < nglyphs ? glyphs + (uint32_t)entry * charsize : blank;
}

// The code point CP437 gives a byte, for the boot-time check that the table
// above still names the right glyph in the face it was generated from.
// Only the high half: below 0x80 CP437 is ASCII, and a terminal never draws
// the control codes it puts in 0x00-0x1F.
static inline uint16_t os64_cp437_codepoint(uint8_t byte)
{
    static const uint16_t high[128] = {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
        0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
        0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
        0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
        0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
        0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
        0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
        0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
        0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
        0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
        0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
        0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
        0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
        0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
    };
    return byte < 0x80 ? byte : high[byte - 0x80];
}

#endif // OS64_CHARSET_H
