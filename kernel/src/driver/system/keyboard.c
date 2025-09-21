#include "driver/system/keyboard.h"
#include <stddef.h>
#include "memset.h"
#include "CONFIG.h"
#include "printd.h"
#include "io.h"

// Keystrokes are generated from PS/2 set-1 scancodes and exposed through a
// simple ring buffer so other subsystems can poll without blocking the IRQ path.

#define KEYBOARD_MAX_SCANCODE 128

typedef enum keyboard_modifiers {
    KEYBOARD_MOD_SHIFT = 1u << 0,
    KEYBOARD_MOD_CTRL  = 1u << 1,
    KEYBOARD_MOD_ALT   = 1u << 2,
    KEYBOARD_MOD_CAPS  = 1u << 3,
    KEYBOARD_MOD_NUM   = 1u << 4,
} keyboard_modifiers_t;

static keyboard_event_t s_event_buffer[KEYBOARD_BUFFER_SIZE];
static volatile size_t s_event_head;
static volatile size_t s_event_tail;
static uint8_t s_modifiers;
static bool s_extended_pending;
static bool s_key_state[KEYBOARD_MAX_SCANCODE];
static __uint128_t s_saved_debug_level;
static bool s_debug_suppressed;

// Base character map for scancodes when no modifier remaps the key.
static const char s_scancode_base_map[KEYBOARD_MAX_SCANCODE] = {
    [0x01] = '\x1B',    // Escape
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = 0x60,      // `
    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x37] = '*',       // Keypad *
    [0x39] = ' ',
    [0x47] = '7',       // Keypad
    [0x48] = '8',
    [0x49] = '9',
    [0x4A] = '-',
    [0x4B] = '4',
    [0x4C] = '5',
    [0x4D] = '6',
    [0x4E] = '+',
    [0x4F] = '1',
    [0x50] = '2',
    [0x51] = '3',
    [0x52] = '0',
    [0x53] = '.',
};

// Alternate glyphs used when Shift is down.
static const char s_scancode_shift_map[KEYBOARD_MAX_SCANCODE] = {
    [0x01] = '\x1B',
    [0x02] = '!',
    [0x03] = '@',
    [0x04] = '#',
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '^',
    [0x08] = '&',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',
    [0x0C] = '_',
    [0x0D] = '+',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',
    [0x1A] = '{',
    [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',
    [0x27] = ':',
    [0x28] = '"',
    [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',
    [0x33] = '<',
    [0x34] = '>',
    [0x35] = '?',
    [0x37] = '*',
    [0x39] = ' ',
    [0x47] = '7',
    [0x48] = '8',
    [0x49] = '9',
    [0x4A] = '-',
    [0x4B] = '4',
    [0x4C] = '5',
    [0x4D] = '6',
    [0x4E] = '+',
    [0x4F] = '1',
    [0x50] = '2',
    [0x51] = '3',
    [0x52] = '0',
    [0x53] = '.',
};

// Ring buffer helper: wrap a cursor to the next slot.
static inline size_t advance_index(size_t index) {
    return (index + 1u) % KEYBOARD_BUFFER_SIZE;
}

// Record a completed keystroke and apply local debug toggling shortcuts.
static void keyboard_emit_event(uint8_t scancode, char ascii) {
    if (ascii == 0) {
        return;
    }

    if (ascii == '~') {
        if (!s_debug_suppressed) {
            printd(DEBUG_BOOT, "keyboard: debug level suppressed via `~`\n");
            s_saved_debug_level = kDebugLevel;
            kDebugLevel = DEBUG_BOOT | DEBUG_EXCEPTIONS;
            s_debug_suppressed = true;
        } else {
            kDebugLevel = s_saved_debug_level;
            s_debug_suppressed = false;
            printd(DEBUG_BOOT, "keyboard: debug level restored via `~`\n");
        }
    }

    keyboard_event_t event = {
        .ascii = ascii,
        .scancode = scancode,
        .shift = (s_modifiers & KEYBOARD_MOD_SHIFT) != 0,
        .ctrl = (s_modifiers & KEYBOARD_MOD_CTRL) != 0,
        .alt = (s_modifiers & KEYBOARD_MOD_ALT) != 0,
    };

    size_t head = s_event_head;
    size_t next_head = advance_index(head);

    if (next_head == s_event_tail) {
        // Buffer is full: keep existing keystrokes and drop this one.
        return;
    }

    s_event_buffer[head] = event;
    s_event_head = next_head;
}

// Decide whether Caps Lock should affect this character.
static bool keyboard_is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Identify keypad scancodes so Num Lock rules can be applied.
static bool keyboard_is_keypad(uint8_t scancode) {
    return scancode >= 0x47 && scancode <= 0x53;
}

// Convert a make-code into an ASCII glyph based on current modifier state.
static char keyboard_translate_scancode(uint8_t scancode) {
    if (scancode >= KEYBOARD_MAX_SCANCODE) {
        return 0;
    }

    char base = s_scancode_base_map[scancode];
    if (base == 0) {
        return 0;
    }

    bool shift_active = (s_modifiers & KEYBOARD_MOD_SHIFT) != 0;
    bool caps_active = (s_modifiers & KEYBOARD_MOD_CAPS) != 0;
    bool num_active = (s_modifiers & KEYBOARD_MOD_NUM) != 0;

    if (keyboard_is_keypad(scancode)) {
        if (!num_active) {
            return 0;
        }
        // Keypad characters are not affected by shift or caps.
        return base;
    }

    if (keyboard_is_letter(base)) {
        bool uppercase = shift_active ^ caps_active;
        if (uppercase) {
            if (base >= 'a' && base <= 'z') {
                base = (char)(base - 'a' + 'A');
            }
        } else {
            if (base >= 'A' && base <= 'Z') {
                base = (char)(base - 'A' + 'a');
            }
        }
        return base;
    }

    if (shift_active) {
        char shifted = s_scancode_shift_map[scancode];
        if (shifted != 0) {
            return shifted;
        }
    }

    return base;
}

// Update latch-style modifiers for non-extended keys.
static void keyboard_update_modifier(uint8_t scancode, bool pressed) {
    switch (scancode) {
        case 0x2A: // Left Shift
        case 0x36: // Right Shift
            if (pressed) {
                s_modifiers |= KEYBOARD_MOD_SHIFT;
            } else {
                s_modifiers &= (uint8_t)~KEYBOARD_MOD_SHIFT;
            }
            break;
        case 0x1D: // Left Control
            if (pressed) {
                s_modifiers |= KEYBOARD_MOD_CTRL;
            } else {
                s_modifiers &= (uint8_t)~KEYBOARD_MOD_CTRL;
            }
            break;
        case 0x38: // Left Alt
            if (pressed) {
                s_modifiers |= KEYBOARD_MOD_ALT;
            } else {
                s_modifiers &= (uint8_t)~KEYBOARD_MOD_ALT;
            }
            break;
        case 0x3A: // Caps Lock
            if (pressed) {
                s_modifiers ^= KEYBOARD_MOD_CAPS;
            }
            break;
        case 0x45: // Num Lock
            if (pressed) {
                s_modifiers ^= KEYBOARD_MOD_NUM;
            }
            break;
        default:
            break;
    }
}

// Extended scancodes provide right-side modifiers.
static void keyboard_update_modifier_extended(uint8_t scancode, bool pressed) {
    switch (scancode) {
        case 0x1D: // Right Control
            if (pressed) {
                s_modifiers |= KEYBOARD_MOD_CTRL;
            } else {
                s_modifiers &= (uint8_t)~KEYBOARD_MOD_CTRL;
            }
            break;
        case 0x38: // Right Alt (AltGr)
            if (pressed) {
                s_modifiers |= KEYBOARD_MOD_ALT;
            } else {
                s_modifiers &= (uint8_t)~KEYBOARD_MOD_ALT;
            }
            break;
        default:
            break;
    }
}

// Prepare keyboard state before IRQs are enabled.
void keyboard_init(void) {
    s_event_head = 0;
    s_event_tail = 0;
    s_modifiers = 0;
    s_extended_pending = false;
    memset((void*)s_key_state, 0, sizeof(s_key_state));
    s_saved_debug_level = kDebugLevel;
    s_debug_suppressed = false;

    // Enable keyboard interrupts
    outb(0x64, 0xAD); // Disable first port temporarily
    outb(0x64, 0xAE); // Enable first port again (keyboard)
}

// Entry point from IRQ1 after the raw byte is read from port 0x60.
void keyboard_handle_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        s_extended_pending = true;
        return;
    }

    bool is_break = (scancode & 0x80u) != 0;
    uint8_t code = (uint8_t)(scancode & 0x7Fu);

    if (s_extended_pending) {
        keyboard_update_modifier_extended(code, !is_break);
        s_extended_pending = false;
        return;
    }

    keyboard_update_modifier(code, !is_break);

    if (code >= KEYBOARD_MAX_SCANCODE) {
        return;
    }

    if (!is_break) {
        s_key_state[code] = true;
        return;
    }

    if (!s_key_state[code]) {
        // Spurious break; nothing to emit.
        return;
    }

    s_key_state[code] = false;

    char ascii = keyboard_translate_scancode(code);
    keyboard_emit_event(code, ascii);
}

// Quick check for queued keystrokes.
bool keyboard_has_event(void) {
    return s_event_head != s_event_tail;
}

// Pop the oldest keystroke; returns false when the buffer is empty.
bool keyboard_pop_event(keyboard_event_t *event) {
    if (event == NULL) {
        return false;
    }

    if (s_event_head == s_event_tail) {
        return false;
    }

    *event = s_event_buffer[s_event_tail];
    s_event_tail = advance_index(s_event_tail);
    return true;
}
