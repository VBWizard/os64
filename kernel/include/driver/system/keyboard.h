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
} keyboard_modifiers_t;

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
