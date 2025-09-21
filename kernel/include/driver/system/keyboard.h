#ifndef DRIVER_SYSTEM_KEYBOARD_H
#define DRIVER_SYSTEM_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#define KEYBOARD_BUFFER_SIZE 128

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

#endif // DRIVER_SYSTEM_KEYBOARD_H
