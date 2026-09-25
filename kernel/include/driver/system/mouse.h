#ifndef DRIVER_SYSTEM_MOUSE_H
#define DRIVER_SYSTEM_MOUSE_H

#include <stdint.h>
#include <stdbool.h>

// PS/2 mouse driver (8042 AUX port, IRQ12).
//
// Initialized on text and GUI boots for pointer input and VT scrollback.
// mouse_handle_byte() is called from the shared 8042
// dispatch (ps2_handle_irq in keyboard.c) for every AUX-origin byte; before
// mouse_init() has run it just discards them.

void mouse_init(void);
void mouse_handle_byte(uint8_t data);

#endif // DRIVER_SYSTEM_MOUSE_H
