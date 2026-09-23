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
//
// THE HANDS (REMOTE.md section 4). A viewer opened with mode "u" may also
// WRITE, one record per write, and what it writes arrives as input exactly
// as a keyboard and mouse at the machine would deliver it. "r" only watches;
// a write to it is refused.
//
//   OS64_GLASS_KEYBOARD  a HID boot-keyboard report: the modifier bits, a
//                        reserved byte, up to six held key usages (HID 1.11
//                        Appendix B). The viewer IS a USB keyboard to the
//                        system — the same interpreter, so the chords
//                        (Alt+F8 brings the desktop back), the modifier
//                        edges and typematic repeat are the ones a local
//                        keyboard gets. Send the state after every change;
//                        a held key repeats on its own.
//   OS64_GLASS_POINTER   an ABSOLUTE position in screen pixels and the
//                        buttons held (bit 0 left, bit 1 right, bit 2
//                        middle). A position off the screen, or a button bit
//                        above those, refuses the record.
//
// A write of anything else, or of a record whose length is not its kind's,
// is refused whole. CLOSING the viewer lifts every key and button it holds,
// so a dropped connection does not leave Ctrl down or a drag in progress,
// short of a GUI event queue being full at that moment (DEBTS).

#include <stdint.h>

#define OS64_GLASS_TEXT_VT  0x1u   // a text terminal, not the desktop, is on the screen

typedef struct __attribute__((packed)) os64_glass_rect
{
	uint16_t x, y, w, h;           // the rectangle whose pixels follow
	uint16_t screen_w, screen_h;   // the whole screen, in pixels
	uint32_t flags;                // OS64_GLASS_*
} os64_glass_rect_t;

_Static_assert(sizeof(os64_glass_rect_t) == 16, "glass rect ABI: 16 bytes");

#define OS64_GLASS_KEYBOARD  1u
#define OS64_GLASS_POINTER   2u

#define OS64_GLASS_BUTTON_LEFT    0x1u
#define OS64_GLASS_BUTTON_RIGHT   0x2u
#define OS64_GLASS_BUTTON_MIDDLE  0x4u

typedef struct __attribute__((packed)) os64_glass_keyboard
{
	uint8_t kind;                  // OS64_GLASS_KEYBOARD
	uint8_t report[8];             // modifiers, reserved, six usages
} os64_glass_keyboard_t;

typedef struct __attribute__((packed)) os64_glass_pointer
{
	uint8_t  kind;                 // OS64_GLASS_POINTER
	uint8_t  buttons;              // OS64_GLASS_BUTTON_*
	uint16_t x, y;                 // absolute, in screen pixels
} os64_glass_pointer_t;

_Static_assert(sizeof(os64_glass_keyboard_t) == 9, "glass keyboard record ABI: 9 bytes");
_Static_assert(sizeof(os64_glass_pointer_t) == 6, "glass pointer record ABI: 6 bytes");

#endif // OS64_GLASS_H
