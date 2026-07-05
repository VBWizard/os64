#ifndef DRIVER_SYSTEM_KEYBOARD_H
#define DRIVER_SYSTEM_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

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
bool keyboard_has_event(void);
bool keyboard_pop_event(keyboard_event_t *event);

// Shared 8042 IRQ dispatch: drains the controller's output buffer and routes
// each byte by origin — status bit 5 set = AUX (mouse) byte, else keyboard.
// Called by BOTH the IRQ1 and IRQ12 asm handlers; whoever fires drains
// everything pending, so bytes can't be misdelivered when the two devices
// interleave ("byte stealing").
void ps2_handle_irq(void);

#endif // DRIVER_SYSTEM_KEYBOARD_H
