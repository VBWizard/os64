// hid_keyboard.c — the HID boot-keyboard interpreter: an 8-byte report in,
// keystrokes out through keyboard_deliver_event. hid_keyboard.h says who
// feeds it. The code is the xHCI keyboard's, moved here unchanged so a second
// kind of keyboard (/dev/glass's) is not a second dialect.

#include <stdint.h>
#include <stdbool.h>
#include "CONFIG.h"
#include "memcpy.h"
#include "serial_logging.h"
#include "kernel.h"                 // kTicksSinceStart — the typematic clock
#include "driver/system/keyboard.h"
#include "driver/system/hid_keyboard.h"
#include "gui/compositor.h"         // gui_owns_glass — the VT chord wants Ctrl under the GUI
#include "tty.h"                    // VT chords: Alt+F#, Alt+arrows, Shift+PgUp/PgDn

// ── HID report → keystrokes ─────────────────────────────────────────────────

// HID boot keyboard report: [modifier bits][reserved][6 key usages].
// Usage tables (HID Usage Tables ch. 10): 0x04..0x1D = a..z, 0x1E..0x27 =
// 1..0, then enter/esc/backspace/tab/space and punctuation. Same
// translation SEMANTICS as the PS/2 driver: shift^caps for letters,
// shift map for symbols, Ctrl+letter strips to its 1963 control code.
static const char s_hid_base[0x39] = {
	[0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',[0x09]='f',
	[0x0A]='g',[0x0B]='h',[0x0C]='i',[0x0D]='j',[0x0E]='k',[0x0F]='l',
	[0x10]='m',[0x11]='n',[0x12]='o',[0x13]='p',[0x14]='q',[0x15]='r',
	[0x16]='s',[0x17]='t',[0x18]='u',[0x19]='v',[0x1A]='w',[0x1B]='x',
	[0x1C]='y',[0x1D]='z',
	[0x1E]='1',[0x1F]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',
	[0x24]='7',[0x25]='8',[0x26]='9',[0x27]='0',
	[0x28]='\n',[0x29]=27,[0x2A]='\b',[0x2B]='\t',[0x2C]=' ',
	[0x2D]='-',[0x2E]='=',[0x2F]='[',[0x30]=']',[0x31]='\\',
	[0x33]=';',[0x34]='\'',[0x35]='`',[0x36]=',',[0x37]='.',[0x38]='/',
};
static const char s_hid_shift[0x39] = {
	[0x1E]='!',[0x1F]='@',[0x20]='#',[0x21]='$',[0x22]='%',[0x23]='^',
	[0x24]='&',[0x25]='*',[0x26]='(',[0x27]=')',
	[0x2D]='_',[0x2E]='+',[0x2F]='{',[0x30]='}',[0x31]='|',
	[0x33]=':',[0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',[0x38]='?',
};

static char hid_usage_ascii(const hid_keyboard_t *kbd, uint8_t usage);

// Deliver one press. Returns whether it reached keyboard_deliver_event: a
// chord or latch this driver consumes here returns false, and the release
// of a press that was never delivered is never delivered either (the
// report's release loop asks kbd->delivered).
//
// A KEY A CHORD TOOK BELONGS TO THE CHORD UNTIL IT IS LET GO (X's passive
// grab). Its typematic repeats run the chord tests again (a held Alt+Right
// keeps walking the terminals) but `may_deliver` is false for them, so a
// repeat that no longer matches is dropped rather than delivered: after
// Alt+F8 switches a text terminal to the desktop, where bare Alt+F8 is not
// a chord, the held F8 must not start arriving as an ordinary key.
static bool hid_deliver_usage(hid_keyboard_t *kbd, uint8_t usage, bool may_deliver)
{
	if (usage == 0x39) {                      // Caps Lock: a latch, not a key
		uint8_t before = kbd->mods;
		kbd->mods ^= KEYBOARD_MOD_CAPS;
		keyboard_publish_hid_modifiers(before, kbd->mods);   // the latch is modifier state too
		return false;
	}
	// The three-finger salute, HID spelling: Delete Forward (0x4C) or keypad
	// Del (0x63) with Ctrl+Alt. Same hook the PS/2 driver calls — one chord,
	// two dialects (2026-08-08, the P5's corded keyboard).
	if ((usage == 0x4C || usage == 0x63) &&
	    (kbd->mods & KEYBOARD_MOD_CTRL) && (kbd->mods & KEYBOARD_MOD_ALT)) {
		keyboard_ctrl_alt_del();
		return false;
	}
	// Virtual-terminal chords, HID spelling — same policy hooks as the PS/2
	// driver, consumed before anything can reach an input ring. F1-F8 are
	// usages 0x3A..0x41; PgUp/PgDn are 0x4B/0x4E; arrows checked here for
	// Alt BEFORE the VT100 burst below (Alt+Left switches terminals, never
	// leaks an ESC [ D). Held chords ride the typematic engine like any key
	// — a held Alt+Right walks the terminal ring at 25 cps, which is a
	// feature if you squint.
	if (kbd->mods & KEYBOARD_MOD_ALT) {
		// F-row: Alt+F1..F8 on a text terminal, CTRL+Alt+F1..F8 while the
		// GUI holds the glass — X11's convention, and the 2026-08-19 ruling's
		// own escape hatch, opened the day Alt+F4 was wanted for close.
		// (The PS/2 driver makes the identical test; keep them in step.)
		if (usage >= 0x3A && usage <= 0x41 &&
		    (!gui_owns_glass() || (kbd->mods & KEYBOARD_MOD_CTRL))) { tty_focus(usage - 0x3A); return false; }
		if (usage == 0x50) { tty_focus_step(-1); return false; }   // Alt+Left
		if (usage == 0x4F) { tty_focus_step(+1); return false; }   // Alt+Right
	}
	if (kbd->mods & KEYBOARD_MOD_SHIFT) {
		if (usage == 0x4B) { tty_view_scroll(+1); return false; }  // Shift+PgUp
		if (usage == 0x4E) { tty_view_scroll(-1); return false; }  // Shift+PgDn
	}
	// Arrows: the SAME three VT100 bytes the PS/2 path emits (ESC '[' A/B/C/D
	// — see keyboard.c for the 1979 lineage). This was the parity debt that
	// file's comment recorded; paid 2026-08-08, the day the P5's corded
	// keyboard proved husk history worked everywhere except on real hardware.
	{
		char final = 0;
		switch (usage) {
			case 0x4F: final = 'C'; break;   // Right
			case 0x50: final = 'D'; break;   // Left
			case 0x51: final = 'B'; break;   // Down
			case 0x52: final = 'A'; break;   // Up
			case 0x4A: final = 'H'; break;   // Home
			case 0x4D: final = 'F'; break;   // End
			default: break;
		}
		if (final != 0) {
			if (!may_deliver)
				return false;
			keyboard_deliver_event(0x1B, usage, kbd->mods, true);
			keyboard_deliver_event('[',  usage, kbd->mods, true);
			keyboard_deliver_event(final, usage, kbd->mods, true);
			return true;
		}
		// The digit-parameter family, HID spelling — Insert=2, Delete=3,
		// PgUp=5, PgDn=6, xterm's vocabulary, mirroring keyboard.c's PS/2
		// switch. Delete and Insert joined 2026-08-16 (PR #26): the PS/2
		// side learned them for husk's editor, and the reviewer presented
		// this file's old parity IOU for payment on behalf of the one
		// machine that has no PS/2 port to fall back on — the P5, whose
		// corded keyboard is exactly who Delete-at-the-prompt was for.
		char param = 0;
		switch (usage) {
			case 0x49: param = '2'; break;   // Insert
			case 0x4C: param = '3'; break;   // Delete (the salute case exits above)
			case 0x4B: param = '5'; break;   // Page Up
			case 0x4E: param = '6'; break;   // Page Down
			default: break;
		}
		if (param != 0) {
			if (!may_deliver)
				return false;
			keyboard_deliver_event(0x1B, usage, kbd->mods, true);
			keyboard_deliver_event('[',  usage, kbd->mods, true);
			keyboard_deliver_event(param, usage, kbd->mods, true);
			keyboard_deliver_event('~', usage, kbd->mods, true);
			return true;
		}
	}
	// A key with no ASCII (the F-row, the keypad without NumLock) is still
	// delivered, with ASCII 0 — exactly what the PS/2 driver does for the
	// same keys, and what Alt+F4 needs: the window system names the F-row
	// by scancode + dialect (keyboard_fkey_number), and a key this path
	// dropped here "for later" (until 2026-08-23) was a chord the GUI could
	// never see from a USB keyboard. The text path ignores ASCII 0 at its
	// door (keyboard_emit_event), so the terminals see nothing new.
	if (!may_deliver)
		return false;
	char c = hid_usage_ascii(kbd, usage);

	// Scancode field: HID usage stands in. Downstream, the GUI's key-code
	// passthrough hands it to apps untouched, and the window system itself
	// never matches on it — its chords test the ASCII, precisely because
	// this field means different things on the two keyboard paths.
	printd(kbd->debug | DEBUG_DETAILED, "%s: key usage 0x%02x -> 0x%02x\n",
	       kbd->name, usage, (uint8_t)c);
	keyboard_deliver_event(c, usage, kbd->mods, true);
	return true;
}

// The ASCII a usage produces under the keyboard's current modifiers, or 0 for
// a usage the boot table doesn't cover. Split out of hid_deliver_usage
// (2026-08-23) so a RELEASE can report the same character its press did —
// the PS/2 driver translates break codes the same way.
static char hid_usage_ascii(const hid_keyboard_t *kbd, uint8_t usage)
{
	if (usage >= sizeof(s_hid_base))
		return 0;
	char c = s_hid_base[usage];
	if (c == 0)
		return 0;

	bool shift = (kbd->mods & KEYBOARD_MOD_SHIFT) != 0;
	bool caps  = (kbd->mods & KEYBOARD_MOD_CAPS) != 0;
	bool ctrl  = (kbd->mods & KEYBOARD_MOD_CTRL) != 0;

	if (c >= 'a' && c <= 'z') {
		if (shift ^ caps)
			c = (char)(c - 'a' + 'A');
	} else if (shift && usage < sizeof(s_hid_shift) && s_hid_shift[usage] != 0) {
		c = s_hid_shift[usage];
	}

	// Ctrl LAST, on the character the other modifiers settled on, and over
	// the punctuation 1963 ASCII gave a control code as well as the letters —
	// keyboard_has_control_code (keyboard.h) is the shared answer to which
	// characters have one, so the two keyboard dialects cannot disagree about
	// what Ctrl+] is.
	if (ctrl && keyboard_has_control_code(c))
		c = (char)(c & 0x1F);
	return c;
}

// Typematic cadence (engine below, state in hid_keyboard_t): half a second of grace,
// then ~25 cps — the classic feel, done host-side because HID reports state.
#define HID_TYPEMATIC_DELAY_TICKS  (TICKS_PER_SECOND / 2)   // 500ms to first repeat
#define HID_TYPEMATIC_PERIOD_TICKS 4                        // then ~25 cps

void hid_keyboard_report(hid_keyboard_t *kbd, const uint8_t rep[8])
{
	// The raw report, before any interpretation — the line that tells you
	// whether a modifier the guest never acted on was the device's fault or
	// ours (it earned its keep on day one: see the chord-publish comment
	// below).
	printd(kbd->debug, "%s: kbd report m=0x%02x keys %02x %02x %02x %02x %02x %02x\n",
	       kbd->name, rep[0], rep[2], rep[3], rep[4], rep[5], rep[6], rep[7]);

	// Phantom state: every slot 0x01 = rollover error, report is garbage.
	if (rep[2] == 0x01 && rep[3] == 0x01 && rep[4] == 0x01)
		return;

	// ONE REPORT, THREE STEPS, IN THE ORDER A HAND DOES THEM: the keys that
	// lifted, then the modifiers that changed, then the keys that went down.
	// A report that lets go of Alt+Tab whole (a viewer closing, or two keys
	// lifted inside one USB poll) must deliver the Tab-up while Alt still
	// holds, or the Alt-up ends the Alt-Tab hold first and the Tab-up, now
	// carrying no Alt, reaches whichever window was just focused as half a
	// chord it never saw begin.
	//
	// RELEASE EDGES (2026-08-23): a usage present in the previous report and
	// absent now is a key-UP, delivered under the modifiers held until this
	// report (kbd->mods is still the previous report's here), so a chord's
	// key lifts inside its chord.
	// The GUI has wanted these since keyboard.c grew break-code delivery for
	// PS/2 ("modifier-drag interactions and chords depend on knowing when a
	// key was released"); the text path ignores them.
	for (int j = 2; j < 8; j++) {
		uint8_t u = kbd->prev_report[j];
		if (u == 0)
			continue;
		bool still_down = false;
		for (int i = 2; i < 8; i++)
			if (rep[i] == u)
				still_down = true;
		// A press this driver consumed (a terminal switch, the salute, Caps
		// Lock) was never delivered, so neither is its release: after Alt+F8
		// the GUI holds the glass, and an F8-up would reach its focused
		// window as half a key it never saw. The PS/2 driver's s_key_state
		// is the same rule.
		uint8_t bit = (uint8_t)(1u << (u & 7));
		if (!still_down && (kbd->delivered[u >> 3] & bit)) {
			kbd->delivered[u >> 3] &= (uint8_t)~bit;
			keyboard_deliver_event(hid_usage_ascii(kbd, u), u, kbd->mods, false);
		}
	}

	// HID modifier bits: LCtrl,LShift,LAlt,LGui,RCtrl,RShift,RAlt,RGui.
	uint8_t m = rep[0];
	uint8_t mods = (uint8_t)(kbd->mods & KEYBOARD_MOD_CAPS);   // caps latch survives
	if (m & 0x11) mods |= KEYBOARD_MOD_CTRL;
	if (m & 0x22) mods |= KEYBOARD_MOD_SHIFT;
	if (m & 0x44) mods |= KEYBOARD_MOD_ALT;
	mods |= KEYBOARD_MOD_HID;   // the dialect tag: every event from here says "HID usage" (keyboard.h)
	uint8_t before = kbd->mods;
	kbd->mods = mods;
	// Publish this keyboard's change NOW, at the report that made it. A
	// modifier-only report (Ctrl+Alt held, no key usages) delivers no event at
	// all, and the mouse path samples keyboard_current_modifiers() to decide
	// whether the window-management chord is held — without this line the chord
	// was invisible on USB keyboards (worked in QEMU's PS/2, dead on the P5).
	keyboard_publish_hid_modifiers(before, mods);

	// MODIFIER EDGES (2026-08-23). A modifier-only report changes state and
	// — until today — sent no event: the GUI learned that Alt was held only
	// from the modifier byte riding on the NEXT key, and learned that it was
	// released from nothing at all. That is what left Alt+Tab's hold stuck
	// open on the P5's USB keyboard after working in QEMU's PS/2. The PS/2
	// driver delivers a modifier key's make AND break like any other key
	// (ASCII 0, the window system sees the modifiers change), so this path
	// does the same: one event per modifier bit that flipped, scancode the
	// HID usage of that modifier (0xE0 + bit: LCtrl, LShift, LAlt, LGui,
	// RCtrl, RShift, RAlt, RGui). keyboard_deliver_event drops releases on
	// the text path and ASCII-0 presses are already what PS/2 modifiers
	// look like there, so the terminals see nothing new.
	uint8_t prev_m = kbd->prev_report[0];
	for (int bit = 0; bit < 8; bit++) {
		uint8_t mask = (uint8_t)(1u << bit);
		if ((m & mask) != (prev_m & mask))
			keyboard_deliver_event(0, (uint8_t)(0xE0 + bit), mods, (m & mask) != 0);
	}

	// Edge detection: a usage present now but absent from the previous
	// report is a key-DOWN. NOTE, corrected
	// 2026-08-08: this comment used to claim boot-protocol keyboards
	// auto-repeat internally. They do not — that is PS/2 lore. A HID
	// keyboard reports STATE, and repeat is the HOST's job (which is why
	// holding a key did nothing on the P5: the report said "still down"
	// and this filter correctly said "nothing new", and nobody anywhere
	// was in charge of inventing the repeats). The typematic engine below
	// (hid_keyboard_tick) is that somebody now.
	for (int i = 2; i < 8; i++) {
		uint8_t u = rep[i];
		if (u == 0)
			continue;
		bool was_down = false;
		for (int j = 2; j < 8; j++)
			if (kbd->prev_report[j] == u)
				was_down = true;
		if (!was_down) {
			if (hid_deliver_usage(kbd, u, true))
				kbd->delivered[u >> 3] |= (uint8_t)(1u << (u & 7));
			// The LAST key pressed is the repeat candidate — classic
			// typematic semantics since the 5150: press-and-hold J while
			// holding K, and J is what repeats. Caps Lock is a latch, and
			// a latch toggles on its press only: repeating it would flip it
			// every period. It still ends the current repeat, as it does on
			// hardware whose typematic moves to the last key pressed.
			kbd->rpt_usage = u == 0x39 ? 0 : u;
			kbd->rpt_next_tick = kTicksSinceStart + HID_TYPEMATIC_DELAY_TICKS;
		}
	}
	// If the candidate is no longer held, repeat ends with the key.
	if (kbd->rpt_usage != 0) {
		bool still_down = false;
		for (int i = 2; i < 8; i++)
			if (rep[i] == kbd->rpt_usage)
				still_down = true;
		if (!still_down)
			kbd->rpt_usage = 0;
	}
	memcpy(kbd->prev_report, (void *)rep, 8);
}

// ── Software typematic (2026-08-08 — "holding down a key doesn't work") ─────
// Called by the keyboard's owner at least once a tick: while the repeat
// candidate stays held, re-deliver it on the classic cadence. Repeats flow
// through hid_deliver_usage, so a held arrow repeats its whole VT100
// sequence and a held Ctrl+letter repeats its control code — everything a
// fresh press would do, which is the definition of typematic done at the
// right layer.
void hid_keyboard_tick(hid_keyboard_t *kbd)
{
	if (kbd->rpt_usage == 0 ||
	    kTicksSinceStart < kbd->rpt_next_tick)
		return;
	kbd->rpt_next_tick = kTicksSinceStart + HID_TYPEMATIC_PERIOD_TICKS;
	// A repeat may deliver only what its press delivered (hid_deliver_usage).
	uint8_t u = kbd->rpt_usage;
	hid_deliver_usage(kbd, u, (kbd->delivered[u >> 3] & (1u << (u & 7))) != 0);
}
