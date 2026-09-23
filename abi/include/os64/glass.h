#ifndef OS64_GLASS_H
#define OS64_GLASS_H

// os64/glass.h — /dev/glass, the screen as a stream of changes (REMOTE.md
// section 3). The consumer is vncd; anything that wants to watch the screen
// reads the same records.
//
// open("/dev/glass", "r") returns a handle that is one VIEWER: it has its own
// record of what changed since it last looked, so two viewers never steal
// each other's changes. The first read after open is the whole screen.
//
// A READ YIELDS ONE CHANGED RECTANGLE: an os64_glass_rect_t, then h rows of w
// pixels, row-packed, each pixel XRGB8888 as a little-endian uint32 (blue in
// the low byte; the top byte is not alpha and carries nothing). It blocks
// until something has changed; os64_read_for's patience works
// (OS64_ERR_TIMEOUT when the screen is still), and a caught signal ends the
// wait. A rectangle taller than the buffer holds comes back as a band of
// whole rows from its top, and the rest stays pending for the next read. A
// buffer that cannot hold the header and ONE row of the pending rectangle is
// refused, never half-filled: size it for at least one full screen row.
//
// The screen is EVENTUALLY exact. Pixels are copied after the viewer's
// record is taken, so a frame drawn during the copy can tear what one read
// returns, but that frame's own change is recorded after the take and comes
// back on the next read. The mouse pointer is part of the pixels.
//
// Phase 1 shows the DESKTOP. While a text terminal holds the physical screen
// the desktop's pixels are still served, and every record says so with
// OS64_GLASS_TEXT_VT; a change of who holds the screen re-sends the whole
// frame, so a viewer learns of it on its next read.
//
// A boot with no desktop has nothing to show: the open is refused.

#include <stdint.h>

#define OS64_GLASS_TEXT_VT  0x1u   // a text terminal, not the desktop, is on the screen

typedef struct __attribute__((packed)) os64_glass_rect
{
	uint16_t x, y, w, h;           // the rectangle whose pixels follow
	uint16_t screen_w, screen_h;   // the whole screen, in pixels
	uint32_t flags;                // OS64_GLASS_*
} os64_glass_rect_t;

_Static_assert(sizeof(os64_glass_rect_t) == 16, "glass rect ABI: 16 bytes");

#endif // OS64_GLASS_H
