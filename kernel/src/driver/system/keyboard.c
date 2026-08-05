#include "driver/system/keyboard.h"
#include "driver/system/mouse.h"
#include "console.h"
#include <stddef.h>
#include "memset.h"
#include "CONFIG.h"
#include "printd.h"
#include "io.h"
#include "gui/input.h"
#include "spinlock.h"

// Keystrokes are generated from PS/2 set-1 scancodes (this file) and USB HID
// boot reports (usb/xhci.c) — both feed keyboard_deliver_event, and land in
// one ring buffer other subsystems poll without blocking the producers.
// When the GUI is active, key-down AND key-up events are additionally injected
// into the unified GUI input queue (gui/input.h).

#define KEYBOARD_MAX_SCANCODE 128

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

// Two producers can reach the event ring now — the PS/2 IRQ path and the
// USB HID poll (xhci.c) — so the push is guarded. (The ring was born
// lock-free single-producer; the lock arrived with the second keyboard
// century. The consumer side, console_read, needs no lock: one consumer,
// and head/tail stay single-writer per side.)
static spinlock_t s_deliver_lock = 0;

// Record a completed keystroke and apply local debug toggling shortcuts.
static void keyboard_emit_event(uint8_t scancode, char ascii, uint8_t modifiers) {
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
        .shift = (modifiers & KEYBOARD_MOD_SHIFT) != 0,
        .ctrl = (modifiers & KEYBOARD_MOD_CTRL) != 0,
        .alt = (modifiers & KEYBOARD_MOD_ALT) != 0,
    };

    uint64_t flags = spinlock_acquire_irqsave(&s_deliver_lock);

    size_t head = s_event_head;
    size_t next_head = advance_index(head);

    if (next_head == s_event_tail) {
        // Buffer is full: keep existing keystrokes and drop this one.
        spinlock_release_irqrestore(&s_deliver_lock, flags);
        return;
    }

    s_event_buffer[head] = event;
    s_event_head = next_head;

    spinlock_release_irqrestore(&s_deliver_lock, flags);
}

// THE delivery choke: every keyboard — 1981's 8042 or this decade's USB
// HID — hands its translated keystrokes here, and everything downstream
// (the console ring husk reads, the GUI input queue) is source-blind.
// Key-downs enter the console ring; both edges reach the GUI (chords and
// modifier-drags need releases).
//
// One byte gets a veto first: console_intr_intercept may CONSUME an ETX
// (Ctrl+C -> 0x03) as the terminal interrupt character instead of letting it
// enter the ring as data. The POLICY (who is foreground, who gets SIGINT)
// lives entirely in console.c — this file stays a device driver and includes
// no task or signal headers, exactly the layering SIGINT.md prescribes.
void keyboard_deliver_event(char ascii, uint8_t scancode, uint8_t modifiers, bool pressed) {
    if (pressed) {
        if (!console_intr_intercept(ascii))
            keyboard_emit_event(scancode, ascii, modifiers);
    }
    input_inject_key(ascii, scancode, modifiers, pressed);
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
    bool ctrl_active = (s_modifiers & KEYBOARD_MOD_CTRL) != 0;

    if (keyboard_is_keypad(scancode)) {
        if (!num_active) {
            return 0;
        }
        // Keypad characters are not affected by shift or caps.
        return base;
    }

    if (keyboard_is_letter(base)) {
        // Ctrl+letter yields the ASCII control code (0x01..0x1A). This is not
        // a convention we're borrowing — it's what the Ctrl key was BUILT to
        // do: 1963 ASCII laid out the control codes so that Ctrl simply strips
        // the high bits of the letter. Ctrl+D = 0x04 = EOT ("End of
        // Transmission"), which is why console_read treats it as end-of-input
        // — same well Unix drank from, not an imitation of Unix.
        if (ctrl_active) {
            return (char)((base & ~0x20) - 'A' + 1);
        }
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

        // Arrow keys (extended make codes) become VT100 escape sequences on
        // the input stream: ESC '[' A/B/C/D for Up/Down/Right/Left — the
        // 1979 vocabulary every terminal since the VT100 has emitted, chosen
        // for exactly that interop precedent (a future vim-over-serial reads
        // these bytes unchanged; husk's history parser is merely their first
        // customer). Until 2026-08-04 arrows were silently dropped here.
        // Press-only: a release has no meaning on a character stream.
        // (The xHCI keyboard's usage table owes the same three bytes for
        // parity — its arrows are usages 0x4F-0x52 — when it next gets love.)
        if (!is_break) {
            char final = 0;
            switch (code) {
                case 0x48: final = 'A'; break;   // Up
                case 0x50: final = 'B'; break;   // Down
                case 0x4D: final = 'C'; break;   // Right
                case 0x4B: final = 'D'; break;   // Left
                default: break;
            }
            if (final != 0) {
                keyboard_deliver_event(0x1B, code, s_modifiers, true);
                keyboard_deliver_event('[',  code, s_modifiers, true);
                keyboard_deliver_event(final, code, s_modifiers, true);
            }
        }
        return;
    }

    keyboard_update_modifier(code, !is_break);

    if (code >= KEYBOARD_MAX_SCANCODE) {
        return;
    }

    if (!is_break) {
        // Make code (including hardware typematic repeats). Emit on PRESS:
        // interactive consumers want to react when the key goes down, and
        // holding a key repeats for free. (This used to emit on release,
        // which made every keystroke feel like it lagged by its own length.)
        s_key_state[code] = true;
        char ascii = keyboard_translate_scancode(code);
        keyboard_deliver_event(ascii, code, s_modifiers, true);
        return;
    }

    if (!s_key_state[code]) {
        // Spurious break; nothing to emit.
        return;
    }

    s_key_state[code] = false;

    // Break code: the legacy ring only carries keystrokes (presses), but the
    // GUI needs KEY_UP too — modifier-drag interactions and chords depend on
    // knowing when a key was released. (deliver_event routes releases to the
    // GUI queue only.)
    keyboard_deliver_event(keyboard_translate_scancode(code), code, s_modifiers, false);
}

void ps2_handle_irq(void) {
    // Drain the 8042 until its output buffer is empty. Status bit 0 = data
    // available; bit 5 = the byte came from the AUX (mouse) port. Dispatching
    // on bit 5 makes it harmless that IRQ1 and IRQ12 share this routine —
    // each byte reaches the right driver no matter which IRQ fired.
    uint8_t status;
    while ((status = inb(0x64)) & 0x01) {
        uint8_t data = inb(0x60);
        if (status & 0x20) {
            mouse_handle_byte(data);
        } else {
            keyboard_handle_scancode(data);
        }
    }
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
