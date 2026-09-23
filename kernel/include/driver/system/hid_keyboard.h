#ifndef HID_KEYBOARD_H
#define HID_KEYBOARD_H

// hid_keyboard.h — the HID boot-keyboard interpreter (HID 1.11 Appendix B):
// an 8-byte report — modifier bits, a reserved byte, up to six held key
// usages — turned into what every os64 keyboard produces: keyboard_deliver_
// event calls, with the VT and three-finger chords consumed on the way, a
// release for every press, an event for every modifier edge, and software
// typematic, because a HID keyboard reports STATE and repeating is the
// host's job.
//
// Every keyboard that speaks this report shares this one interpreter, so the
// "every scancode check needs BOTH dialects" rule stays at two: a USB
// keyboard (usb/xhci.c) and a /dev/glass writer (gui/glass.c, REMOTE.md
// section 4) are the same dialect. Each owns one hid_keyboard_t and calls in
// with its own serialization; nothing here locks.
//
// The live-modifier snapshot (keyboard_current_modifiers) is machine-wide:
// with two keyboards, the one that changed a modifier last is what a mouse
// packet sees — as with any two keyboards on one machine.

#include <stdint.h>

typedef struct hid_keyboard
{
	uint8_t  prev_report[8];   // the last report, for press/release edges
	uint8_t  mods;             // KEYBOARD_MOD_* as last derived, CAPS latched
	uint8_t  rpt_usage;        // the typematic candidate, 0 = none
	uint64_t rpt_next_tick;
	const char *name;          // for the debug log: "xhci", "glass"
	uint64_t debug;            // the DEBUG_* bit its reports are logged under
} hid_keyboard_t;

// One report from the device.
void hid_keyboard_report(hid_keyboard_t *kbd, const uint8_t rep[8]);
// The typematic clock. The owner calls it at least once a tick.
void hid_keyboard_tick(hid_keyboard_t *kbd);

#endif // HID_KEYBOARD_H
