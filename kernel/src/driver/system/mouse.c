// mouse.c — PS/2 mouse driver (8042 AUX port, IRQ12).
//
// Initialization talks to two different chips and it matters which is which:
//  * 0x64/0x60 command/config traffic programs the 8042 CONTROLLER
//  * 0xD4-prefixed writes go THROUGH the controller to the MOUSE itself
//
// Every wait is bounded: a machine with no mouse (or a flaky controller) must
// boot cursor-less, never hang. All bytes the mouse sends after streaming is
// enabled arrive through ps2_handle_irq (keyboard.c), which dispatches on the
// 8042 status AUX bit and calls mouse_handle_byte() here.

#include "driver/system/mouse.h"

#include "CONFIG.h"
#include "kernel.h"
#include "printd.h"
#include "io.h"
#include "gui/input.h"

// 8042 controller ports/bits
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64   // read: status
#define PS2_COMMAND_PORT 0x64   // write: controller command
#define PS2_STATUS_OUTPUT_FULL 0x01  // a byte is waiting in 0x60
#define PS2_STATUS_INPUT_FULL  0x02  // controller hasn't consumed our last write

// Controller commands
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_ENABLE_AUX    0xA8
#define PS2_CMD_WRITE_TO_AUX  0xD4  // next 0x60 write goes to the mouse

// Config byte bits
#define PS2_CONFIG_IRQ12_ENABLE    0x02  // bit 1: AUX port generates IRQ12
#define PS2_CONFIG_AUX_CLOCK_OFF   0x20  // bit 5: AUX clock DISABLED when set

// Mouse device commands (sent via 0xD4 prefix) and replies
#define MOUSE_CMD_SET_DEFAULTS    0xF6
#define MOUSE_CMD_ENABLE_STREAM   0xF4
#define MOUSE_REPLY_ACK           0xFA

// Bounded-spin limit for controller handshakes. At port-IO speeds this is
// multiple milliseconds — far beyond any healthy 8042's response time.
#define PS2_WAIT_SPINS 100000

static bool s_mouse_active = false;

// 3-byte packet assembly state
static uint8_t s_packet[3];
static uint8_t s_packet_index = 0;
// Tick of the last AUX byte, for timeout resync: if the controller dropped a
// byte mid-packet (e.g. its buffer overflowed during a motion burst), the
// bit-3 header check alone can stay misaligned FOREVER — data bytes often
// have bit 3 set too. Inter-byte gaps within one packet are sub-millisecond,
// so any gap of a few ticks means "whatever we were assembling is dead;
// treat the next byte as a packet start".
static uint64_t s_last_byte_tick = 0;
#define MOUSE_RESYNC_GAP_TICKS 3   // 30ms at TICKS_PER_SECOND=100

// Wait until we may WRITE to 0x60/0x64 (controller consumed the last byte).
static bool ps2_wait_input_clear(void)
{
	for (int i = 0; i < PS2_WAIT_SPINS; i++) {
		if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL))
			return true;
		__builtin_ia32_pause();
	}
	return false;
}

// Wait until a byte is available to READ from 0x60.
static bool ps2_wait_output_full(void)
{
	for (int i = 0; i < PS2_WAIT_SPINS; i++) {
		if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
			return true;
		__builtin_ia32_pause();
	}
	return false;
}

// Send one byte to the MOUSE (0xD4 prefix) and wait for its ACK (0xFA).
static bool mouse_send(uint8_t cmd)
{
	if (!ps2_wait_input_clear())
		return false;
	outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_TO_AUX);
	if (!ps2_wait_input_clear())
		return false;
	outb(PS2_DATA_PORT, cmd);

	// The mouse answers with ACK; a self-test (BAT) result 0xAA followed by
	// a device ID can also be in flight right after reset — skip past
	// anything that isn't the ACK we're waiting for, within bounds.
	for (int tries = 0; tries < 8; tries++) {
		if (!ps2_wait_output_full())
			return false;
		uint8_t reply = inb(PS2_DATA_PORT);
		if (reply == MOUSE_REPLY_ACK)
			return true;
		printd(DEBUG_GUI, "mouse: expected ACK for 0x%02x, got 0x%02x (skipping)\n", cmd, reply);
	}
	return false;
}

void mouse_init(void)
{
	// The whole handshake runs with interrupts off: the IRQ1 handler is
	// already live and its 8042 drain (ps2_handle_irq) would eat our ACK
	// bytes mid-handshake. With IF clear we poll the ports directly and
	// nothing can race us; this takes well under a millisecond.
	uint64_t rflags;
	__asm__ volatile("pushfq\n\tpop %0" : "=r"(rflags) :: "memory");
	__asm__ volatile("cli" ::: "memory");

	bool ok = false;
	do {
		// Turn the AUX port on at the controller.
		if (!ps2_wait_input_clear())
			break;
		outb(PS2_COMMAND_PORT, PS2_CMD_ENABLE_AUX);

		// Config byte: enable IRQ12 generation, make sure the AUX clock runs.
		if (!ps2_wait_input_clear())
			break;
		outb(PS2_COMMAND_PORT, PS2_CMD_READ_CONFIG);
		if (!ps2_wait_output_full())
			break;
		uint8_t config = inb(PS2_DATA_PORT);
		config |= PS2_CONFIG_IRQ12_ENABLE;
		config &= (uint8_t)~PS2_CONFIG_AUX_CLOCK_OFF;
		if (!ps2_wait_input_clear())
			break;
		outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_CONFIG);
		if (!ps2_wait_input_clear())
			break;
		outb(PS2_DATA_PORT, config);

		// Program the mouse itself: sane defaults, then start streaming
		// movement packets.
		if (!mouse_send(MOUSE_CMD_SET_DEFAULTS))
			break;
		if (!mouse_send(MOUSE_CMD_ENABLE_STREAM))
			break;

		ok = true;
	} while (0);

	if (ok) {
		s_packet_index = 0;
		s_mouse_active = true;
		printd(DEBUG_GUI, "mouse: PS/2 mouse initialized, streaming enabled\n");
	} else {
		printd(DEBUG_GUI, "mouse: no PS/2 mouse detected (or handshake timed out) — continuing cursor-less\n");
	}

	if (rflags & 0x200)
		__asm__ volatile("sti" ::: "memory");
}

void mouse_handle_byte(uint8_t data)
{
	// Until mouse_init() brings the device up, AUX bytes are stray (e.g.
	// hot-plug chatter or BAT codes) — swallow them so they can't be
	// misread as keyboard scancodes.
	if (!s_mouse_active)
		return;

	// Timeout resync (see s_last_byte_tick above).
	if (s_packet_index != 0 &&
	    kTicksSinceStart - s_last_byte_tick >= MOUSE_RESYNC_GAP_TICKS) {
		printd(DEBUG_GUI | DEBUG_DETAILED,
		       "mouse: inter-byte gap, resyncing packet assembly\n");
		s_packet_index = 0;
	}
	s_last_byte_tick = kTicksSinceStart;

	// Standard 3-byte PS/2 packet:
	//   byte 0: buttons (bits 0-2), ALWAYS-1 (bit 3), X/Y sign (4/5), X/Y overflow (6/7)
	//   byte 1: X movement       byte 2: Y movement (positive = up)
	switch (s_packet_index) {
	case 0:
		if (!(data & 0x08)) {
			// Bit 3 must be set in the first byte. If it isn't, we lost
			// sync (dropped byte somewhere) — discard until a plausible
			// packet header comes by.
			printd(DEBUG_GUI | DEBUG_DETAILED, "mouse: resync, dropped byte 0x%02x\n", data);
			return;
		}
		s_packet[0] = data;
		s_packet_index = 1;
		break;
	case 1:
		s_packet[1] = data;
		s_packet_index = 2;
		break;
	case 2:
		s_packet[2] = data;
		s_packet_index = 0;

		// Overflow means the deltas are garbage; drop the whole packet.
		if (s_packet[0] & 0xC0)
			return;

		// 9-bit signed deltas: the sign bits live in byte 0.
		int16_t dx = (int16_t)s_packet[1] - ((s_packet[0] & 0x10) ? 0x100 : 0);
		int16_t dy = (int16_t)s_packet[2] - ((s_packet[0] & 0x20) ? 0x100 : 0);

		// PS/2 y is positive-UP; screen coordinates are positive-DOWN.
		dy = (int16_t)-dy;

		// Button bits 0/1/2 = L/R/M — same order as INPUT_MOUSE_BUTTON_*.
		input_inject_mouse(dx, dy, s_packet[0] & 0x07);
		break;
	}
}
