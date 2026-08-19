#include "driver/system/keyboard.h"
#include "driver/system/mouse.h"
#include "console.h"
#include "tty.h"             // the input rings live per-tty now; chords switch them
#include <stddef.h>
#include "memset.h"
#include "CONFIG.h"
#include "printd.h"
#include "io.h"
#include "gui/input.h"
#include "gui/compositor.h"  // gui_owns_glass — the input-routing fork (VT8 chapter)
#include "spinlock.h"
#include "BasicRenderer.h"   // printf — the Ctrl+Alt+Del salute's answer

extern volatile uint64_t kTicksSinceStart;

// Keystrokes are generated from PS/2 set-1 scancodes (this file) and USB HID
// boot reports (usb/xhci.c) — both feed keyboard_deliver_event. Since the
// virtual-terminal slice the ring lives in tty_t (one per terminal, so
// type-ahead stays with its terminal); this driver just translates and
// delivers, blind to which terminal is focused — the same way it stays blind
// to tasks and signals. When the GUI is active, key-down AND key-up events
// are additionally injected into the unified GUI input queue (gui/input.h).

#define KEYBOARD_MAX_SCANCODE 128

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

// Record a completed keystroke and apply local debug toggling shortcuts.
// The push itself (ring choice, dormant-terminal summons, producer locking)
// lives in tty_input_event — the driver's job ends at "here is a keystroke".
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

    tty_input_event(&event);
}

// THE delivery choke: every keyboard — 1981's 8042 or this decade's USB
// HID — hands its translated keystrokes here, and everything downstream
// (the console ring husk reads, the GUI input queue) is source-blind.
//
// INPUT FOLLOWS THE GLASS (the VT8 chapter, 2026-08-19). Until then every
// keystroke was delivered TWICE — into the focused tty's ring AND the GUI
// queue — which is how typing at a gkeys window also typed into husk
// ("wake upkill -9 57", 2026-08-17: a kill command half-eaten by the window
// it was aimed past). Now exactly one world receives: the one whose pixels
// you are looking at. The VT-switch chords are immune to the fork by
// position — keyboard_handle_scancode consumes them before delivery, so
// Alt+F1..F8 and Alt+arrows work identically from either side.
//
// GUI side gets BOTH edges (chords and modifier-drags need releases); the
// text side keeps its press-only console discipline, with one byte vetoed
// first: console_intr_intercept may CONSUME an ETX (Ctrl+C -> 0x03) as the
// terminal interrupt character instead of letting it enter the ring as data.
// On the GUI side Ctrl+C is an ordinary event to the focused window — a
// terminal's interrupt character belongs to terminals. The POLICY (who is
// foreground, who gets SIGINT) lives entirely in console.c — this file stays
// a device driver, exactly the layering SIGINT.md prescribes; gui_owns_glass
// is a routing predicate, not policy, same standing as tty_input_event.
void keyboard_deliver_event(char ascii, uint8_t scancode, uint8_t modifiers, bool pressed) {
    if (gui_owns_glass()) {
        input_inject_key(ascii, scancode, modifiers, pressed);
        return;
    }
    if (pressed) {
        if (!console_intr_intercept(ascii))
            keyboard_emit_event(scancode, ascii, modifiers);
    }
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
    s_modifiers = 0;
    s_extended_pending = false;
    memset((void*)s_key_state, 0, sizeof(s_key_state));
    s_saved_debug_level = kDebugLevel;
    s_debug_suppressed = false;

    // Enable keyboard interrupts
    outb(0x64, 0xAD); // Disable first port temporarily
    outb(0x64, 0xAE); // Enable first port again (keyboard)
}

// ── Ctrl+Alt+Del (2026-08-08) ───────────────────────────────────────────────
// The three-finger salute, caught by BOTH keyboard drivers (PS/2 here, xHCI
// in its usage handler) and answered with a message instead of a mystery.
// David Bradley wired this chord to reboot the IBM PC in 1981 as a five-
// minute development shortcut ("I invented it, but Bill Gates made it
// famous"); on os64 an unannounced reboot would throw away the descent we
// just built, so v1 catches the chord and says what to do instead. When
// reboot(1) lands (verb 1 of SYSCALL_SHUTDOWN, already reserved), this
// becomes the polite reboot itself. Rate-limited: the chord held down must
// not machine-gun the console.
void keyboard_ctrl_alt_del(void)
{
    static uint64_t s_lastSaluteTick = 0;
    if (kTicksSinceStart - s_lastSaluteTick < TICKS_PER_SECOND)
        return;
    s_lastSaluteTick = kTicksSinceStart;
    printf("\nCtrl+Alt+Del: caught. os64 prefers a polite exit — type 'shutdown'. (reboot(1) is on the roadmap)\n");
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
            // The three-finger salute: extended 0x53 is the dedicated Del key.
            if (code == 0x53 &&
                (s_modifiers & KEYBOARD_MOD_CTRL) && (s_modifiers & KEYBOARD_MOD_ALT)) {
                keyboard_ctrl_alt_del();
                return;
            }
            // Virtual-terminal chords, extended block (checked BEFORE the
            // arrow burst below — Alt+Left must switch terminals, not leak
            // an ESC [ D onto somebody's input stream). A chord is a command
            // to the terminal STACK, not an input byte; it is consumed here
            // the same way Ctrl+Alt+Del is. Policy lives in tty.c — this
            // driver just recognizes the knock. (Alt+arrow cycling is the
            // os32 heritage gait, Alt+A/Alt+D's grandchild; Shift+PgUp is
            // scrollback's chord since the Linux 0.x console.)
            if (s_modifiers & KEYBOARD_MOD_ALT) {
                if (code == 0x4B) { tty_focus_step(-1); return; }   // Alt+Left
                if (code == 0x4D) { tty_focus_step(+1); return; }   // Alt+Right
            }
            if (s_modifiers & KEYBOARD_MOD_SHIFT) {
                if (code == 0x49) { tty_view_scroll(+1); return; }  // Shift+PgUp
                if (code == 0x51) { tty_view_scroll(-1); return; }  // Shift+PgDn
            }
            char final = 0;
            switch (code) {
                case 0x48: final = 'A'; break;   // Up
                case 0x50: final = 'B'; break;   // Down
                case 0x4D: final = 'C'; break;   // Right
                case 0x4B: final = 'D'; break;   // Left
                case 0x47: final = 'H'; break;   // Home
                case 0x4F: final = 'F'; break;   // End
                default: break;
            }
            if (final != 0) {
                keyboard_deliver_event(0x1B, code, s_modifiers, true);
                keyboard_deliver_event('[',  code, s_modifiers, true);
                keyboard_deliver_event(final, code, s_modifiers, true);
                return;
            }
            // The digit-parameter family: ESC [ <n> ~ — the vocabulary xterm
            // standardized for the keys the VT100 didn't have. Delete is 3
            // (2026-08-16 — until then a plain Del press emitted NOTHING;
            // only the Ctrl+Alt salute ever saw the key), Insert is 2
            // (emitted for parity: standard bytes cost one line, and keytest
            // can already show them), PgUp/PgDn are 5/6 as before.
            char param = 0;
            switch (code) {
                case 0x52: param = '2'; break;   // Insert
                case 0x53: param = '3'; break;   // Delete (salute handled above)
                case 0x49: param = '5'; break;   // Page Up
                case 0x51: param = '6'; break;   // Page Down
                default: break;
            }
            if (param != 0) {
                keyboard_deliver_event(0x1B, code, s_modifiers, true);
                keyboard_deliver_event('[',  code, s_modifiers, true);
                keyboard_deliver_event(param, code, s_modifiers, true);
                keyboard_deliver_event('~', code, s_modifiers, true);
            }
        }
        return;
    }

    keyboard_update_modifier(code, !is_break);

    if (code >= KEYBOARD_MAX_SCANCODE) {
        return;
    }

    if (!is_break) {
        // The salute's keypad spelling: plain 0x53 is keypad-Del ("."), and
        // Ctrl+Alt makes it the same chord (both Dels counted since the
        // 5150 — Bradley's handler read either).
        if (code == 0x53 &&
            (s_modifiers & KEYBOARD_MOD_CTRL) && (s_modifiers & KEYBOARD_MOD_ALT)) {
            keyboard_ctrl_alt_del();
            return;
        }
        // Alt+F1..F8: direct-select a virtual terminal (the Linux console's
        // chord since 1991, on the loadout os32 shipped — eight terminals).
        // F1-F8 are 0x3B..0x42; bare F-keys stay silent on the input stream
        // (no ASCII, same as always), so the F-row remains free for future
        // full-screen apps to claim.
        if (code >= 0x3B && code <= 0x42 &&
            (s_modifiers & KEYBOARD_MOD_ALT)) {
            tty_focus(code - 0x3B);
            return;
        }
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

// (keyboard_has_event / keyboard_pop_event retired 2026-08-08: the event
// ring moved into tty_t — one per terminal — and the consumer-side API went
// with it. See tty_input_has / tty_input_pop in tty.h.)
