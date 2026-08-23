#ifndef DRIVER_SYSTEM_KEYBOARD_H
#define DRIVER_SYSTEM_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

// Depth of each terminal's input event ring (the ring itself lives in tty_t
// since the virtual-terminal slice — one per terminal, so type-ahead stays
// with its terminal; the size stays here because the coin is minted here).
#define KEYBOARD_BUFFER_SIZE 128

// Modifier bitmask carried in GUI input events (and used internally by the
// driver). Lives here (not keyboard.c) so event consumers can decode it.
typedef enum keyboard_modifiers {
    KEYBOARD_MOD_SHIFT = 1u << 0,
    KEYBOARD_MOD_CTRL  = 1u << 1,
    KEYBOARD_MOD_ALT   = 1u << 2,
    KEYBOARD_MOD_CAPS  = 1u << 3,
    KEYBOARD_MOD_NUM   = 1u << 4,
    // The keyboard's DIALECT, not a modifier (2026-08-23): set by the xHCI
    // path on every event it delivers, clear from PS/2. The scancode field
    // is a set-1 make code on one path and a HID usage on the other, and a
    // consumer that needs to name a key with no ASCII — the F-row — needs
    // to know which it is holding. (abi os64/gui.h publishes the same bit.)
    KEYBOARD_MOD_HID   = 1u << 7,
} keyboard_modifiers_t;

// Which F-key is this event, 1..12, or 0 if it is not one. The ONE place
// both dialects' F-row numbering is written: PS/2 set-1 puts F1..F10 at
// 0x3B..0x44 and F11/F12 at 0x57/0x58; HID puts F1..F12 at 0x3A..0x45.
static inline int keyboard_fkey_number(uint8_t scancode, uint8_t modifiers)
{
    if (modifiers & KEYBOARD_MOD_HID)
        return (scancode >= 0x3A && scancode <= 0x45) ? (int)(scancode - 0x3A + 1) : 0;
    if (scancode >= 0x3B && scancode <= 0x44) return (int)(scancode - 0x3B + 1);
    if (scancode == 0x57) return 11;
    if (scancode == 0x58) return 12;
    return 0;
}

typedef struct keyboard_event {
    char ascii;
    uint8_t scancode;
    bool shift;
    bool ctrl;
    bool alt;
} keyboard_event_t;

void keyboard_init(void);
void keyboard_handle_scancode(uint8_t scancode);

// THE delivery choke for every keyboard driver (PS/2 IRQ path and the USB
// HID poll both land here): pushes key-downs into the console event ring
// (spinlock-guarded — two producers now) and hands both edges to the GUI
// input queue. `modifiers` is the KEYBOARD_MOD_* bitmask at press time.
void keyboard_deliver_event(char ascii, uint8_t scancode, uint8_t modifiers, bool pressed);

// The modifier bitmask as of the last key event that reached the choke above.
// Read by the MOUSE path (input.c) so a mouse event can carry the keyboard
// state that was true when it happened — Ctrl+Alt+drag needs to know whether
// the chord is held, and a pointer packet has no idea by itself.
//
// Snapshotted in keyboard_deliver_event rather than read out of the PS/2
// driver's own s_modifiers, because that variable belongs to ONE of the two
// keyboard drivers: the xHCI HID path carries its own modifier byte and never
// touches it. Sampling at the shared choke makes the answer source-blind by
// construction, which is the same reason the choke exists at all.
uint8_t keyboard_current_modifiers(void);
// The USB half of the publication (the PS/2 half is keyboard.c's static
// keyboard_publish_modifiers). The xHCI HID driver calls this the moment a
// report changes its modifier byte, because a modifier-ONLY report — Ctrl+Alt
// held, no key usages, which is exactly what the window-management chord
// looks like on the wire — never reaches keyboard_deliver_event, and the
// snapshot above would otherwise stay stale until some unrelated key rode by.
// Same rule, each driver at its own change point: a modifier is state, and
// state has to be published where it CHANGES. (Found 2026-08-21: Ctrl+Alt
// move/resize worked in QEMU's PS/2 keyboard and did nothing on the P5's
// USB one.)
void keyboard_publish_hid_modifiers(uint8_t modifiers);
// The three-finger salute, shared by both keyboard drivers (PS/2 scancodes
// and xHCI HID usages both land here). v1 answers with a message; becomes
// the polite reboot when SYSCALL_SHUTDOWN verb 1 gets its meaning.
void keyboard_ctrl_alt_del(void);
// (keyboard_has_event / keyboard_pop_event moved to tty.h as
// tty_input_has / tty_input_pop when the ring went per-terminal.)

// Shared 8042 IRQ dispatch: drains the controller's output buffer and routes
// each byte by origin — status bit 5 set = AUX (mouse) byte, else keyboard.
// Called by BOTH the IRQ1 and IRQ12 asm handlers; whoever fires drains
// everything pending, so bytes can't be misdelivered when the two devices
// interleave ("byte stealing").
void ps2_handle_irq(void);

#endif // DRIVER_SYSTEM_KEYBOARD_H
