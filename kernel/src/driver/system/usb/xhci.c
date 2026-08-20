// xhci.c — xHCI controller + USB HID boot-protocol keyboard and mouse.
// The design rationale and v1 limits live in xhci.h; this file is the
// machine. Register/TRB layouts follow the xHCI 1.x specification;
// section references below are to that spec.
//
// Memory discipline: every DMA structure (rings, contexts, buffers) comes
// from kmalloc_aligned — zeroed at the allocator choke point, physically
// contiguous, HHDM-addressed (phys = virt - kHHDMOffset). The controller's
// MMIO BAR is mapped at (kHHDMOffset | bar_phys) — an UPPER-HALF VA — so
// xhci_poll() can touch the doorbells from ANY CR3 (processSignals runs
// under whatever task was interrupted). Mapped with PAGE_PCD: this is
// device memory, caching it would be lying to ourselves.

#include <stdint.h>
#include <stdbool.h>
#include "CONFIG.h"
#include "kmalloc.h"
#include "memset.h"
#include "memcpy.h"
#include "paging.h"
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — framebuffer boot lines only
#include "panic.h"
#include "time.h"
#include "driver/system/pci.h"
#include "driver/system/keyboard.h"
#include "gui/input.h"
#include "tty.h"             // VT chords: Alt+F#, Alt+arrows, Shift+PgUp/PgDn
#include "driver/system/usb/xhci.h"

extern pci_device_t* kPCIDeviceHeaders;
extern pci_device_t* kPCIDeviceFunctions;
extern uint8_t kPCIDeviceCount, kPCIFunctionCount;
extern uintptr_t kHHDMOffset;
extern uintptr_t kKernelPML4v;
extern bool kUSBQuiet;   // USBQUIET — the opt-in 2.4GHz hygiene flashlight

// ── Register offsets (xHCI spec ch. 5) ──────────────────────────────────────

#define XHCI_CAP_CAPLENGTH   0x00   // byte: operational regs offset
#define XHCI_CAP_HCSPARAMS1  0x04   // slots / interrupters / ports
#define XHCI_CAP_HCSPARAMS2  0x08   // scratchpad demand hides in here
#define XHCI_CAP_HCCPARAMS1  0x10   // CSZ (context size) bit 2
#define XHCI_CAP_DBOFF       0x14
#define XHCI_CAP_RTSOFF      0x18

#define XHCI_OP_USBCMD       0x00
#define XHCI_OP_USBSTS       0x04
#define XHCI_OP_CRCR         0x18
#define XHCI_OP_DCBAAP       0x30
#define XHCI_OP_CONFIG       0x38
#define XHCI_OP_PORTSC(n)    (0x400 + 0x10 * ((n) - 1))   // ports are 1-based

#define USBCMD_RS            (1u << 0)
#define USBCMD_HCRST         (1u << 1)
#define USBSTS_HCH           (1u << 0)
#define USBSTS_CNR           (1u << 11)

#define PORTSC_CCS           (1u << 0)   // device connected
#define PORTSC_PED           (1u << 1)   // port enabled
#define PORTSC_PR            (1u << 4)   // port reset
#define PORTSC_PP            (1u << 9)   // port power
#define PORTSC_SPEED(v)      (((v) >> 10) & 0xF)  // 1=FS 2=LS 3=HS 4=SS

// Interrupter 0 (runtime base + 0x20)
#define XHCI_IR0_IMAN        0x20
#define XHCI_IR0_ERSTSZ      0x28
#define XHCI_IR0_ERSTBA      0x30
#define XHCI_IR0_ERDP        0x38

// TRB types (spec 6.4.6), already shifted into control bits 10-15
#define TRB_TYPE(t)          ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)      (((c) >> 10) & 0x3F)
#define TRB_NORMAL           1
#define TRB_SETUP            2
#define TRB_DATA             3
#define TRB_STATUS           4
#define TRB_LINK             6
#define TRB_ENABLE_SLOT      9
#define TRB_ADDRESS_DEVICE   11
#define TRB_CONFIG_ENDPOINT  12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_EV_TRANSFER      32
#define TRB_EV_CMD_COMPLETE  33
#define TRB_EV_PORT_STATUS   34

#define TRB_CYCLE            (1u << 0)
#define TRB_TOGGLE_CYCLE     (1u << 1)
#define TRB_IOC              (1u << 5)
#define TRB_IDT              (1u << 6)   // immediate data (Setup stage)

#define TRB_CC(status)       (((status) >> 24) & 0xFF)
#define TRB_CC_SUCCESS       1
#define TRB_CC_SHORT_PACKET  13

typedef struct {
	uint64_t param;
	uint32_t status;
	uint32_t control;
} __attribute__((packed)) xhci_trb_t;

#define RING_TRBS 256   // one 4KB page per ring, last TRB is the Link

// A producer ring (command ring and every transfer ring): we enqueue,
// the controller consumes. The Link TRB at the end points back to the
// start with Toggle Cycle set — the classic circular TRB ring.
typedef struct {
	xhci_trb_t *trb;         // HHDM virtual
	uint64_t    phys;
	uint32_t    enqueue;     // next index we write
	uint32_t    cycle;       // our current producer cycle state (1 or 0)
} xhci_ring_t;

typedef enum {
	HID_NONE = 0,
	HID_KEYBOARD,
	HID_MOUSE,
} hid_kind_t;

// One HID device attached directly to a root port. Boot keyboards and boot
// mice use the same xHCI machinery; only their fixed report decoders differ.
typedef struct {
	bool         present;
	hid_kind_t   kind;
	uint32_t     port;
	uint32_t     speed;
	uint32_t     slot;
	uint32_t     dci;
	xhci_ring_t  ep0;
	xhci_ring_t  intr;
	uint8_t     *dev_ctx;
	uint8_t     *input_ctx;
	uint64_t     input_ctx_phys;
	uint8_t     *reports;
	uint64_t     reports_phys;

	// Boot-keyboard state. Unused (and zero) for a mouse.
	uint8_t      prev_report[8];
	uint8_t      mods;
	uint8_t      rpt_usage;
	uint64_t     rpt_next_tick;
} xhci_hid_t;

// One controller. The P5 may place its keyboard and mouse on different xHCI
// controllers, so initialized controllers remain live and are all polled.
typedef struct {
	bool         present;         // controller found and running
	uint8_t     *cap;             // MMIO: capability base (HHDM-aliased)
	uint8_t     *op;              // MMIO: operational base
	uint8_t     *rt;              // MMIO: runtime base
	uint32_t    *db;              // MMIO: doorbell array
	uint32_t     ctx_size;        // 32 or 64 (HCCPARAMS1.CSZ)
	uint32_t     max_ports;
	uint64_t    *dcbaa;           // device context base address array
	xhci_ring_t  cmd;             // command ring
	xhci_trb_t  *evt;             // event ring segment (consumer side)
	uint64_t     evt_phys;
	uint32_t     evt_dequeue;
	uint32_t     evt_cycle;

	// last command completion (filled by the event drain, consumed by
	// xhci_run_command's synchronous wait)
	volatile bool     cmd_done;
	volatile uint8_t  cmd_cc;
	volatile uint32_t cmd_slot;   // slot id byte from the completion

	xhci_hid_t   keyboard;
	xhci_hid_t   mouse;

	// EP0 completion currently awaited during boot-time enumeration.
	volatile uint32_t control_slot;
	volatile bool     xfer_done;
	volatile uint8_t  xfer_cc;
} xhci_t;

#define MAX_XHCI_CONTROLLERS 8
#define HID_INFLIGHT 8
#define HID_REPORT_BYTES 8

static xhci_t s_controllers[MAX_XHCI_CONTROLLERS];
static uint32_t s_controller_count;
static bool s_keyboard_claimed;
static bool s_mouse_claimed;
// Active controller while boot-time setup runs, or while xhci_poll owns its
// global serialization lock. It is never changed concurrently.
static xhci_t *s_hc;

extern volatile uint64_t kTicksSinceStart;   // the typematic clock

// ── MMIO accessors ──────────────────────────────────────────────────────────

static inline uint32_t mmio_r32(uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}
static inline void mmio_w32(uint8_t *base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}
static inline void mmio_w64(uint8_t *base, uint32_t off, uint64_t v)
{
	*(volatile uint64_t *)(base + off) = v;
}

static inline uint64_t virt_to_phys(void *v)
{
	return (uint64_t)((uintptr_t)v - kHHDMOffset);
}

// ── Rings ───────────────────────────────────────────────────────────────────

static bool ring_init(xhci_ring_t *r)
{
	r->trb = kmalloc_aligned(PAGE_SIZE);   // zeroed by the allocator
	if (r->trb == NULL)
		return false;
	r->phys = virt_to_phys(r->trb);
	r->enqueue = 0;
	r->cycle = 1;
	// The Link TRB: last slot points back to the first, Toggle Cycle set.
	r->trb[RING_TRBS - 1].param = r->phys;
	r->trb[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE;
	return true;
}

// Enqueue one TRB; handles the Link wrap + cycle toggle. Returns the
// PHYSICAL address of the TRB written (transfer events point back at it).
static uint64_t ring_push(xhci_ring_t *r, uint64_t param, uint32_t status, uint32_t control)
{
	xhci_trb_t *t = &r->trb[r->enqueue];
	uint64_t trb_phys = r->phys + r->enqueue * sizeof(xhci_trb_t);
	t->param = param;
	t->status = status;
	// Write everything BEFORE the cycle bit flips ownership to the HC.
	t->control = (control & ~TRB_CYCLE) | (r->cycle ? TRB_CYCLE : 0);

	r->enqueue++;
	if (r->enqueue == RING_TRBS - 1) {
		// Hand the Link TRB the current cycle so the HC follows it, then
		// wrap: after the toggle, our producer cycle inverts.
		xhci_trb_t *link = &r->trb[RING_TRBS - 1];
		link->control = (link->control & ~TRB_CYCLE) | (r->cycle ? TRB_CYCLE : 0);
		r->enqueue = 0;
		r->cycle ^= 1;
	}
	return trb_phys;
}

// ── Event ring drain ────────────────────────────────────────────────────────

static void xhci_handle_transfer_event(xhci_trb_t *ev);

// Consume every event the controller has posted. Returns the count.
// This is the whole "interrupt handler", minus the interrupt.
static uint32_t xhci_drain_events(void)
{
	uint32_t handled = 0;

	for (;;) {
		xhci_trb_t *ev = &s_hc->evt[s_hc->evt_dequeue];
		uint32_t control = ev->control;
		if ((control & TRB_CYCLE) != (s_hc->evt_cycle ? TRB_CYCLE : 0))
			break;   // controller hasn't written this slot yet

		switch (TRB_GET_TYPE(control)) {
			case TRB_EV_CMD_COMPLETE:
				s_hc->cmd_cc = (uint8_t)TRB_CC(ev->status);
				s_hc->cmd_slot = (control >> 24) & 0xFF;
				s_hc->cmd_done = true;
				break;
			case TRB_EV_TRANSFER:
				xhci_handle_transfer_event(ev);
				break;
			case TRB_EV_PORT_STATUS:
				// v1: no hotplug — note it and move on. (The port that
				// changed is (param >> 24) & 0xFF, for the day this grows.)
				printd(DEBUG_USB, "xhci: port status change (ignored, no hotplug in v1)\n");
				break;
			default:
				printd(DEBUG_USB, "xhci: unhandled event type %u cc %u\n",
				       TRB_GET_TYPE(control), TRB_CC(ev->status));
				break;
		}

		s_hc->evt_dequeue++;
		if (s_hc->evt_dequeue == RING_TRBS) {
			s_hc->evt_dequeue = 0;
			s_hc->evt_cycle ^= 1;
		}
		handled++;
	}

	if (handled > 0) {
		// Tell the controller where our dequeue pointer is now (EHB set to
		// clear the busy flag — harmless in polling mode, required form).
		uint64_t erdp = s_hc->evt_phys + s_hc->evt_dequeue * sizeof(xhci_trb_t);
		mmio_w64(s_hc->rt, XHCI_IR0_ERDP, erdp | (1u << 3));
	}
	return handled;
}

// ── Synchronous command execution (boot-time enumeration) ───────────────────

// Push a command TRB, ring the command doorbell, spin on the event ring
// until its completion arrives. Boot-time only — the scheduler isn't
// running yet, so spinning is honest. Returns the completion code
// (TRB_CC_SUCCESS == 1) or 0 on timeout.
static uint8_t xhci_run_command(uint64_t param, uint32_t status, uint32_t control)
{
	s_hc->cmd_done = false;
	ring_push(&s_hc->cmd, param, status, control);
	mmio_w32((uint8_t *)s_hc->db, 0, 0);   // doorbell 0, target 0 = command ring

	for (int spin = 0; spin < 1000; spin++) {
		xhci_drain_events();
		if (s_hc->cmd_done)
			return s_hc->cmd_cc;
		wait(1);
	}
	printd(DEBUG_USB, "xhci: command timed out (control=0x%08x)\n", control);
	printf("xhci: command timeout (0x%08x)\n", control);   // stays on the glass: a failure is what it's for
	// (This tag used to read "TEMP — P5 bring-up, serial-less". The P5 was
	// mute when these were written; it has a wire now, over the very NIC arc
	// that prompted this cleanup. The NARRATION went to the log 2026-08-20;
	// the FAILURES stayed here, which is what they were really for.)
	return 0;
}

// ── Control transfers on EP0 (boot-time, synchronous) ───────────────────────

// One GET/SET request. `data` NULL for no-data-stage requests. Direction
// is encoded in bmRequestType bit 7 (IN = device-to-host).
static bool xhci_control_request(xhci_hid_t *dev,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 void *data, uint16_t wLength)
{
	bool dir_in = (bmRequestType & 0x80) != 0;

	// Setup stage: the 8 setup bytes ride IN the TRB (IDT). TRT (bits
	// 16-17 of control): 0 = no data, 2 = OUT data, 3 = IN data.
	uint64_t setup = (uint64_t)bmRequestType | ((uint64_t)bRequest << 8) |
	                 ((uint64_t)wValue << 16) | ((uint64_t)wIndex << 32) |
	                 ((uint64_t)wLength << 48);
	uint32_t trt = (wLength == 0) ? 0 : (dir_in ? 3 : 2);
	ring_push(&dev->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));

	// Data stage (bounced through an HHDM scratch buffer — caller's buffer
	// may be anywhere; DMA needs a physical address we control).
	uint8_t *bounce = NULL;
	if (wLength > 0) {
		bounce = kmalloc_aligned(PAGE_SIZE);
		if (bounce == NULL)
			return false;
		if (!dir_in)
			memcpy(bounce, data, wLength);
		ring_push(&dev->ep0, virt_to_phys(bounce), wLength,
		          TRB_TYPE(TRB_DATA) | (dir_in ? (1u << 16) : 0));
	}

	// Status stage: direction opposite the data stage (or IN when no data).
	// IOC — this is the completion we wait for.
	uint32_t status_dir = (wLength == 0 || !dir_in) ? (1u << 16) : 0;
	s_hc->control_slot = dev->slot;
	s_hc->xfer_done = false;
	s_hc->xfer_cc = 0;
	ring_push(&dev->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | status_dir | TRB_IOC);

	mmio_w32((uint8_t *)s_hc->db, 4 * dev->slot, 1);   // doorbell: slot, DCI 1 = EP0

	bool ok = false;
	for (int spin = 0; spin < 1000; spin++) {
		xhci_drain_events();
		if (s_hc->xfer_done) {
			ok = (s_hc->xfer_cc == TRB_CC_SUCCESS ||
			      s_hc->xfer_cc == TRB_CC_SHORT_PACKET);
			break;
		}
		wait(1);
	}

	if (ok && dir_in && wLength > 0)
		memcpy(data, bounce, wLength);
	if (bounce != NULL)
		kfree(bounce);
	if (!ok)
	{
		printd(DEBUG_USB, "xhci: control req 0x%02x/0x%02x failed (cc=%u)\n",
		       bmRequestType, bRequest, s_hc->xfer_cc);
		printf("xhci: ctrl req %02x/%02x failed cc=%u\n", bmRequestType, bRequest, s_hc->xfer_cc);   // stays on the glass: a failure is what it's for
	}
	return ok;
}

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

static void hid_deliver_usage(xhci_hid_t *kbd, uint8_t usage)
{
	if (usage == 0x39) {                      // Caps Lock: a latch, not a key
		kbd->mods ^= KEYBOARD_MOD_CAPS;
		return;
	}
	// The three-finger salute, HID spelling: Delete Forward (0x4C) or keypad
	// Del (0x63) with Ctrl+Alt. Same hook the PS/2 driver calls — one chord,
	// two dialects (2026-08-08, the P5's corded keyboard).
	if ((usage == 0x4C || usage == 0x63) &&
	    (kbd->mods & KEYBOARD_MOD_CTRL) && (kbd->mods & KEYBOARD_MOD_ALT)) {
		keyboard_ctrl_alt_del();
		return;
	}
	// Virtual-terminal chords, HID spelling — same policy hooks as the PS/2
	// driver, consumed before anything can reach an input ring. F1-F8 are
	// usages 0x3A..0x41; PgUp/PgDn are 0x4B/0x4E; arrows checked here for
	// Alt BEFORE the VT100 burst below (Alt+Left switches terminals, never
	// leaks an ESC [ D). Held chords ride the typematic engine like any key
	// — a held Alt+Right walks the terminal ring at 25 cps, which is a
	// feature if you squint.
	if (kbd->mods & KEYBOARD_MOD_ALT) {
		if (usage >= 0x3A && usage <= 0x41) { tty_focus(usage - 0x3A); return; }
		if (usage == 0x50) { tty_focus_step(-1); return; }   // Alt+Left
		if (usage == 0x4F) { tty_focus_step(+1); return; }   // Alt+Right
	}
	if (kbd->mods & KEYBOARD_MOD_SHIFT) {
		if (usage == 0x4B) { tty_view_scroll(+1); return; }  // Shift+PgUp
		if (usage == 0x4E) { tty_view_scroll(-1); return; }  // Shift+PgDn
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
			keyboard_deliver_event(0x1B, usage, kbd->mods, true);
			keyboard_deliver_event('[',  usage, kbd->mods, true);
			keyboard_deliver_event(final, usage, kbd->mods, true);
			return;
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
			keyboard_deliver_event(0x1B, usage, kbd->mods, true);
			keyboard_deliver_event('[',  usage, kbd->mods, true);
			keyboard_deliver_event(param, usage, kbd->mods, true);
			keyboard_deliver_event('~', usage, kbd->mods, true);
			return;
		}
	}
	if (usage >= sizeof(s_hid_base))
		return;                                // F-keys/keypad: later
	char c = s_hid_base[usage];
	if (c == 0)
		return;

	bool shift = (kbd->mods & KEYBOARD_MOD_SHIFT) != 0;
	bool caps  = (kbd->mods & KEYBOARD_MOD_CAPS) != 0;
	bool ctrl  = (kbd->mods & KEYBOARD_MOD_CTRL) != 0;

	if (c >= 'a' && c <= 'z') {
		if (ctrl) {
			c = (char)(c - 'a' + 1);           // Ctrl+letter -> 0x01..0x1A (EOT & friends)
		} else if (shift ^ caps) {
			c = (char)(c - 'a' + 'A');
		}
	} else if (shift && usage < sizeof(s_hid_shift) && s_hid_shift[usage] != 0) {
		c = s_hid_shift[usage];
	}

	// Scancode field: HID usage stands in — nothing downstream interprets
	// it except the GUI's key-code passthrough, which is source-agnostic.
	printd(DEBUG_USB | DEBUG_DETAILED, "xhci: key usage 0x%02x -> 0x%02x\n",
	       usage, (uint8_t)c);
	keyboard_deliver_event(c, usage, kbd->mods, true);
}

// Typematic cadence (engine below, state in the keyboard device): half a second of grace,
// then ~25 cps — the classic feel, done host-side because HID reports state.
#define HID_TYPEMATIC_DELAY_TICKS  (TICKS_PER_SECOND / 2)   // 500ms to first repeat
#define HID_TYPEMATIC_PERIOD_TICKS 4                        // then ~25 cps

static void hid_process_keyboard_report(xhci_hid_t *kbd, const uint8_t *rep)
{
	// Phantom state: every slot 0x01 = rollover error, report is garbage.
	if (rep[2] == 0x01 && rep[3] == 0x01 && rep[4] == 0x01)
		return;

	// HID modifier bits: LCtrl,LShift,LAlt,LGui,RCtrl,RShift,RAlt,RGui.
	uint8_t m = rep[0];
	uint8_t mods = (uint8_t)(kbd->mods & KEYBOARD_MOD_CAPS);   // caps latch survives
	if (m & 0x11) mods |= KEYBOARD_MOD_CTRL;
	if (m & 0x22) mods |= KEYBOARD_MOD_SHIFT;
	if (m & 0x44) mods |= KEYBOARD_MOD_ALT;
	kbd->mods = mods;

	// Edge detection: a usage present now but absent from the previous
	// report is a key-DOWN — the only edge we emit. NOTE, corrected
	// 2026-08-08: this comment used to claim boot-protocol keyboards
	// auto-repeat internally. They do not — that is PS/2 lore. A HID
	// keyboard reports STATE, and repeat is the HOST's job (which is why
	// holding a key did nothing on the P5: the report said "still down"
	// and this filter correctly said "nothing new", and nobody anywhere
	// was in charge of inventing the repeats). The typematic engine below
	// (hid_typematic_tick) is that somebody now.
	for (int i = 2; i < 8; i++) {
		uint8_t u = rep[i];
		if (u == 0)
			continue;
		bool was_down = false;
		for (int j = 2; j < 8; j++)
			if (kbd->prev_report[j] == u)
				was_down = true;
		if (!was_down) {
			hid_deliver_usage(kbd, u);
			// The LAST key pressed is the repeat candidate — classic
			// typematic semantics since the 5150: press-and-hold J while
			// holding K, and J is what repeats.
			kbd->rpt_usage = u;
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

// HID boot mouse report: buttons, signed X, signed Y. HID Y is already in
// screen orientation (positive is down), unlike the PS/2 packet decoder.
static void hid_process_mouse_report(const uint8_t *rep)
{
	input_inject_mouse((int8_t)rep[1], (int8_t)rep[2], rep[0] & 0x07);
}

// ── Software typematic (2026-08-08 — "holding down a key doesn't work") ─────
// Called from xhci_poll (every scheduler pass, ~10ms): while the repeat
// candidate stays held, re-deliver it on the classic cadence. Repeats flow
// through hid_deliver_usage, so a held arrow repeats its whole VT100
// sequence and a held Ctrl+letter repeats its control code — everything a
// fresh press would do, which is the definition of typematic done at the
// right layer.
static void hid_typematic_tick(xhci_hid_t *kbd)
{
	if (!kbd->present || kbd->rpt_usage == 0 ||
	    kTicksSinceStart < kbd->rpt_next_tick)
		return;
	kbd->rpt_next_tick = kTicksSinceStart + HID_TYPEMATIC_PERIOD_TICKS;
	hid_deliver_usage(kbd, kbd->rpt_usage);
}

// ── Transfer events (keyboard/mouse reports arriving) ────────────────────────

static void xhci_arm_report_trb(xhci_hid_t *dev, uint32_t buf_index)
{
	uint64_t buf_phys = dev->reports_phys + buf_index * HID_REPORT_BYTES;
	ring_push(&dev->intr, buf_phys, HID_REPORT_BYTES,
	          TRB_TYPE(TRB_NORMAL) | TRB_IOC);
	mmio_w32((uint8_t *)s_hc->db, 4 * dev->slot, dev->dci);
}

static void xhci_handle_transfer_event(xhci_trb_t *ev)
{
	uint32_t slot = (ev->control >> 24) & 0xFF;
	uint32_t dci  = (ev->control >> 16) & 0x1F;
	uint8_t  cc   = (uint8_t)TRB_CC(ev->status);

	// EP0 completions during boot-time control transfers. Match the slot as
	// well as DCI: another already-live HID endpoint may complete meanwhile.
	if (dci == 1 && slot == s_hc->control_slot) {
		s_hc->xfer_cc = cc;
		s_hc->xfer_done = true;
		return;
	}

	xhci_hid_t *dev = NULL;
	if (s_hc->keyboard.present && slot == s_hc->keyboard.slot &&
	    dci == s_hc->keyboard.dci)
		dev = &s_hc->keyboard;
	else if (s_hc->mouse.present && slot == s_hc->mouse.slot &&
	         dci == s_hc->mouse.dci)
		dev = &s_hc->mouse;
	if (dev == NULL)
		return;

	if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PACKET) {
		printd(DEBUG_USB, "xhci: HID %s transfer error cc=%u\n",
		       dev->kind == HID_KEYBOARD ? "keyboard" : "mouse", cc);
		return;   // deliberately NOT re-armed: a dead endpoint stays quiet
	}

	// Which report buffer completed? The event's param is the TRB's
	// physical address; the TRB's param is the buffer's physical address.
	uint64_t trb_phys = ev->param;
	uint32_t trb_index = (uint32_t)((trb_phys - dev->intr.phys) / sizeof(xhci_trb_t));
	if (trb_index >= RING_TRBS - 1)
		return;
	uint64_t buf_phys = dev->intr.trb[trb_index].param;
	uint32_t buf_index = (uint32_t)((buf_phys - dev->reports_phys) /
	                                HID_REPORT_BYTES);
	if (buf_index >= HID_INFLIGHT)
		return;

	uint32_t residual = ev->status & 0xFFFFFF;
	if (residual <= HID_REPORT_BYTES) {
		uint32_t actual = HID_REPORT_BYTES - residual;
		const uint8_t *report = dev->reports + buf_index * HID_REPORT_BYTES;
		if (dev->kind == HID_KEYBOARD && actual >= 8)
			hid_process_keyboard_report(dev, report);
		else if (dev->kind == HID_MOUSE && actual >= 3)
			hid_process_mouse_report(report);
	}
	xhci_arm_report_trb(dev, buf_index);   // hand the same buffer back
}

// ── Device enumeration (boot-time) ──────────────────────────────────────────

// Write one endpoint/slot context field set. `ctx` points at the START of
// the input context; index 0 = input control, 1 = slot, 2 = EP0 (DCI 1),
// DCI n lives at index n+1. Context size honors HCCPARAMS1.CSZ.
static uint32_t *ictx(xhci_hid_t *dev, uint32_t index)
{
	return (uint32_t *)(dev->input_ctx + index * s_hc->ctx_size);
}

static bool xhci_setup_hid(uint32_t port, uint32_t speed)
{
	xhci_hid_t candidate;
	memset(&candidate, 0, sizeof(candidate));
	candidate.port = port;
	candidate.speed = speed;

	// 1. A slot for the device.
	if (xhci_run_command(0, 0, TRB_TYPE(TRB_ENABLE_SLOT)) != TRB_CC_SUCCESS)
		return false;
	uint32_t slot = s_hc->cmd_slot;
	if (slot == 0)
		return false;
	candidate.slot = slot;

	// 2. Output device context — the controller's copy of the truth.
	candidate.dev_ctx = kmalloc_aligned(PAGE_SIZE);
	if (candidate.dev_ctx == NULL)
		return false;
	s_hc->dcbaa[slot] = virt_to_phys(candidate.dev_ctx);

	// 3. EP0 transfer ring + input context for Address Device.
	if (!ring_init(&candidate.ep0))
		return false;
	candidate.input_ctx = kmalloc_aligned(PAGE_SIZE);
	if (candidate.input_ctx == NULL)
		return false;
	candidate.input_ctx_phys = virt_to_phys(candidate.input_ctx);

	// Default EP0 max packet by speed (LS/FS 8, HS 64, SS 512); corrected
	// from the device descriptor below if the device disagrees.
	uint32_t mps0 = (speed == 3) ? 64 : (speed == 4) ? 512 : 8;

	memset(candidate.input_ctx, 0, PAGE_SIZE);
	ictx(&candidate, 0)[1] = 0x3;                                  // add slot + EP0 contexts
	ictx(&candidate, 1)[0] = (1u << 27) | (speed << 20);           // context entries=1, speed
	ictx(&candidate, 1)[1] = (port << 16);                         // root hub port (1-based)
	ictx(&candidate, 2)[1] = (4u << 3) | (3u << 1) | (mps0 << 16); // EP type 4 (control), CErr 3
	ictx(&candidate, 2)[2] = (uint32_t)(candidate.ep0.phys | 1);   // TR dequeue | DCS
	ictx(&candidate, 2)[3] = (uint32_t)(candidate.ep0.phys >> 32);
	ictx(&candidate, 2)[4] = 8;                                   // average TRB length

	if (xhci_run_command(candidate.input_ctx_phys, 0,
	        TRB_TYPE(TRB_ADDRESS_DEVICE) | (slot << 24)) != TRB_CC_SUCCESS) {
		printd(DEBUG_USB, "xhci: Address Device failed\n");
		printf("xhci: Address Device failed\n");   // stays on the glass: a failure is what it's for
		return false;
	}

	// 4. Device descriptor — and the real bMaxPacketSize0.
	uint8_t desc[18];
	memset(desc, 0, sizeof(desc));
	if (!xhci_control_request(&candidate, 0x80, 6 /*GET_DESCRIPTOR*/, 0x0100, 0, desc, 8))
		return false;
	uint32_t real_mps0 = (speed == 4) ? (1u << desc[7]) : desc[7];
	if (real_mps0 != mps0 && real_mps0 != 0) {
		ictx(&candidate, 0)[1] = 0x2;                              // touch only EP0
		ictx(&candidate, 2)[1] = (4u << 3) | (3u << 1) | (real_mps0 << 16);
		xhci_run_command(candidate.input_ctx_phys, 0,
		    TRB_TYPE(TRB_EVALUATE_CONTEXT) | (slot << 24));
	}

	// 5. Configuration descriptor: find a HID boot keyboard (protocol 1) or
	//    boot mouse (protocol 2) that this controller does not already own.
	uint8_t cfg[256];
	memset(cfg, 0, sizeof(cfg));
	if (!xhci_control_request(&candidate, 0x80, 6, 0x0200, 0, cfg, 9))
		return false;
	uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
	if (total > sizeof(cfg))
		total = sizeof(cfg);
	if (!xhci_control_request(&candidate, 0x80, 6, 0x0200, 0, cfg, total))
		return false;

	uint8_t config_value = cfg[5];
	int32_t iface_num = -1;
	bool matching_iface = false;
	uint32_t ep_addr = 0, ep_mps = 0, ep_interval = 0;
	for (uint16_t off = 0; off + 1 < total && cfg[off] != 0; off += cfg[off]) {
		uint8_t len = cfg[off], type = cfg[off + 1];
		if (type == 4 && len >= 9) {
			// v1 binds one HID interface per physical device. Once its
			// interrupt endpoint is known, do not let a later interface on a
			// composite receiver overwrite candidate.kind while ep_addr still
			// names the first interface's endpoint.
			if (ep_addr != 0)
				break;
			matching_iface = false;
			if (cfg[off + 5] == 3 && cfg[off + 6] == 1) {
				uint8_t protocol = cfg[off + 7];
				if (protocol == 1 && !s_keyboard_claimed) {
					candidate.kind = HID_KEYBOARD;
					matching_iface = true;
				} else if (protocol == 2 && !s_mouse_claimed) {
					candidate.kind = HID_MOUSE;
					matching_iface = true;
				}
				if (matching_iface)
					iface_num = cfg[off + 2];
			}
		} else if (type == 5 && len >= 7 && matching_iface &&
		         (cfg[off + 2] & 0x80) &&
		         (cfg[off + 3] & 0x3) == 3 && ep_addr == 0) {
			ep_addr = cfg[off + 2] & 0xF;
			ep_mps = (uint32_t)(cfg[off + 4] | (cfg[off + 5] << 8)) & 0x7FF;
			ep_interval = cfg[off + 6];
		}
		if (len == 0)
			break;
	}
	if (candidate.kind == HID_NONE) {
		printd(DEBUG_USB, "xhci: device on port %u has no wanted boot HID interface\n", port);
		return false;
	}
	if (ep_addr == 0) {
		printd(DEBUG_USB, "xhci: boot HID on port %u has no interrupt-IN endpoint\n", port);
		return false;
	}

	// 6. Configure + boot protocol + idle.
	if (!xhci_control_request(&candidate, 0x00, 9 /*SET_CONFIGURATION*/,
	                          config_value, 0, NULL, 0))
		return false;
	// A STALL leaves EP0 halted. In particular, VBox's emulated USB mouse
	// stalls SET_PROTOCOL but still sends the conventional mouse report layout.
	// Do not follow that failure with SET_IDLE: it can only time out on the
	// halted endpoint and used to turn one immediate cc=6 into a long boot pause.
	bool boot_protocol = xhci_control_request(&candidate, 0x21,
	                          0x0B /*SET_PROTOCOL*/, 0 /*boot*/,
	                          (uint16_t)iface_num, NULL, 0);
	if (boot_protocol && candidate.kind == HID_KEYBOARD)
		xhci_control_request(&candidate, 0x21, 0x0A /*SET_IDLE*/, 0,
		                     (uint16_t)iface_num, NULL, 0);

	// 7. The interrupt-IN endpoint: DCI = ep*2+1 for IN.
	uint32_t dci = ep_addr * 2 + 1;
	if (!ring_init(&candidate.intr))
		return false;

	// xHCI interval field is in 125us frames, log2-encoded. LS/FS devices
	// give bInterval in ms (interval = log2(ms) + 3); HS/SS give it as
	// 2^(n-1) frames already (interval = n - 1).
	uint32_t interval;
	if (speed == 1 || speed == 2) {
		uint32_t ms = ep_interval ? ep_interval : 10;
		interval = 3;
		while ((1u << (interval - 3)) < ms && interval < 10)
			interval++;
	} else {
		interval = ep_interval ? ep_interval - 1 : 3;
	}

	memset(candidate.input_ctx, 0, PAGE_SIZE);
	ictx(&candidate, 0)[1] = 0x1 | (1u << dci);                    // slot + this endpoint
	ictx(&candidate, 1)[0] = (dci << 27) | (speed << 20);          // context entries = max DCI
	ictx(&candidate, 1)[1] = (port << 16);
	uint32_t *ep = ictx(&candidate, dci + 1);
	ep[0] = interval << 16;
	ep[1] = (7u << 3) | (3u << 1) | (ep_mps << 16);    // type 7: interrupt IN, CErr 3
	ep[2] = (uint32_t)(candidate.intr.phys | 1);        // TR dequeue | DCS
	ep[3] = (uint32_t)(candidate.intr.phys >> 32);
	ep[4] = (ep_mps << 16) | HID_REPORT_BYTES;          // max ESIT | avg TRB len

	if (xhci_run_command(candidate.input_ctx_phys, 0,
	        TRB_TYPE(TRB_CONFIG_ENDPOINT) | (slot << 24)) != TRB_CC_SUCCESS) {
		printd(DEBUG_USB, "xhci: Configure Endpoint failed\n");
		printf("xhci: Configure Endpoint failed\n");   // stays on the glass: a failure is what it's for
		return false;
	}

	// 8. Report buffers + the standing army of in-flight TRBs.
	candidate.reports = kmalloc_aligned(PAGE_SIZE);
	if (candidate.reports == NULL)
		return false;
	candidate.reports_phys = virt_to_phys(candidate.reports);
	candidate.dci = dci;
	candidate.present = true;
	xhci_hid_t *dev = candidate.kind == HID_KEYBOARD ?
	                  &s_hc->keyboard : &s_hc->mouse;
	*dev = candidate;
	if (dev->kind == HID_KEYBOARD)
		s_keyboard_claimed = true;
	else
		s_mouse_claimed = true;
	for (uint32_t i = 0; i < HID_INFLIGHT; i++)
		xhci_arm_report_trb(dev, i);

	const char *kind = dev->kind == HID_KEYBOARD ? "keyboard" : "mouse";
	// The per-device topology (which port, which slot, which endpoint) is
	// diagnosis, not news — init_xHCI's closing "USB input:" line already
	// tells the glass what you can type on. To the log, 2026-08-20.
	printd(DEBUG_USB, "USB %s: port %u slot %u ep %u (interval %u)\n",
	       kind, port, slot, ep_addr, interval);
	printd(DEBUG_USB, "xhci: %s live — port %u slot %u dci %u mps %u\n",
	       kind, port, slot, dci, ep_mps);
	return true;
}

// ── Controller bring-up ─────────────────────────────────────────────────────

static bool xhci_init_controller(pci_device_t *dev)
{
	// BAR0 (possibly 64-bit — bits 2:1 == 10b means the high half lives in
	// BAR1). Mask the low flag bits off to get the MMIO physical base.
	uint64_t bar = dev->baseAdd[0] & ~0xFULL;
	if ((dev->baseAdd[0] & 0x6) == 0x4)
		bar |= ((uint64_t)dev->baseAdd[1]) << 32;
	if (bar == 0)
		return false;

	// Bus mastering + memory space on (offset 4 = PCI command register).
	writePCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4,
	                 dev->command | 0x6);

	// Map the register file at an UPPER-half alias (kHHDMOffset | phys) so
	// it is reachable from every task's CR3 — xhci_poll runs from the
	// scheduler pass under whoever's page tables were live. PCD: MMIO.
	uint64_t map_base = bar & ~(uint64_t)(PAGE_SIZE - 1);
	paging_map_pages((pt_entry_t *)kKernelPML4v, kHHDMOffset + map_base,
	                 map_base, 0x10000 / PAGE_SIZE,
	                 PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	s_hc->cap = (uint8_t *)(kHHDMOffset + bar);

	uint8_t caplength = *(volatile uint8_t *)(s_hc->cap + XHCI_CAP_CAPLENGTH);
	s_hc->op = s_hc->cap + caplength;

	// BIOS LEGACY HANDOFF — real hardware only, and mandatory there. On real
	// machines the firmware owns the controller at boot (its SMM code is what
	// made USB keyboards work in the BIOS menu) and keeps poking it until the
	// OS formally claims ownership through the USB Legacy Support extended
	// capability. Skip this and firmware fights the driver — resets, races,
	// port weirdness. QEMU/VBox have no BIOS in the loop (no such capability
	// advertised), so this walk simply finds nothing there.
	{
		uint32_t hcc = mmio_r32(s_hc->cap, XHCI_CAP_HCCPARAMS1);
		uint32_t xecp = (hcc >> 16) & 0xFFFF;   // in 32-bit dwords from cap base
		while (xecp != 0) {
			uint32_t cap_hdr = mmio_r32(s_hc->cap, xecp * 4);
			if ((cap_hdr & 0xFF) == 1) {         // USB Legacy Support
				// Bit 24 = OS Owned semaphore; bit 16 = BIOS Owned.
				mmio_w32(s_hc->cap, xecp * 4, cap_hdr | (1u << 24));
				for (int i = 0; i < 1000; i++) {
					cap_hdr = mmio_r32(s_hc->cap, xecp * 4);
					if ((cap_hdr & (1u << 16)) == 0)
						break;
					wait(1);
				}
				if (cap_hdr & (1u << 16))
					printd(DEBUG_USB, "xhci: BIOS refused to release ownership — proceeding anyway\n");
				else
					printd(DEBUG_USB, "xhci: legacy handoff complete (OS owns the controller)\n");
				// Silence firmware's SMI sources for good measure (USBLEGCTLSTS,
				// the next dword): clear every SMI enable, ack pending bits.
				uint32_t ctlsts = mmio_r32(s_hc->cap, xecp * 4 + 4);
				mmio_w32(s_hc->cap, xecp * 4 + 4, ctlsts & 0xE0000000u);
				break;
			}
			uint32_t next = (cap_hdr >> 8) & 0xFF;
			xecp = next ? xecp + next : 0;
		}
	}
	s_hc->rt = s_hc->cap + (mmio_r32(s_hc->cap, XHCI_CAP_RTSOFF) & ~0x1Fu);
	s_hc->db = (uint32_t *)(s_hc->cap + (mmio_r32(s_hc->cap, XHCI_CAP_DBOFF) & ~0x3u));

	uint32_t hcs1 = mmio_r32(s_hc->cap, XHCI_CAP_HCSPARAMS1);
	uint32_t hcc1 = mmio_r32(s_hc->cap, XHCI_CAP_HCCPARAMS1);
	uint32_t max_slots = hcs1 & 0xFF;
	s_hc->max_ports = (hcs1 >> 24) & 0xFF;
	s_hc->ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;   // QEMU: 32. Real HW: often 64.

	printd(DEBUG_USB, "xhci: BAR 0x%lx, %u ports, %u slots, %u-byte contexts\n",
	       bar, s_hc->max_ports, max_slots, s_hc->ctx_size);

	// Halt (if running), then reset, then wait for Controller Not Ready
	// to clear — the spec's mandatory sequence.
	mmio_w32(s_hc->op, XHCI_OP_USBCMD, mmio_r32(s_hc->op, XHCI_OP_USBCMD) & ~USBCMD_RS);
	for (int i = 0; i < 100 && !(mmio_r32(s_hc->op, XHCI_OP_USBSTS) & USBSTS_HCH); i++)
		wait(1);
	mmio_w32(s_hc->op, XHCI_OP_USBCMD, USBCMD_HCRST);
	for (int i = 0; i < 500 && (mmio_r32(s_hc->op, XHCI_OP_USBCMD) & USBCMD_HCRST); i++)
		wait(1);
	for (int i = 0; i < 500 && (mmio_r32(s_hc->op, XHCI_OP_USBSTS) & USBSTS_CNR); i++)
		wait(1);
	if (mmio_r32(s_hc->op, XHCI_OP_USBSTS) & USBSTS_CNR) {
		printd(DEBUG_USB, "xhci: controller stuck in reset\n");
		printf("xhci: controller stuck in reset\n");   // stays on the glass: a failure is what it's for
		return false;
	}

	// DCBAA (+ scratchpads, if the controller demands them — QEMU wants 0,
	// real silicon usually wants a few; refusing = undefined behavior).
	s_hc->dcbaa = kmalloc_aligned(PAGE_SIZE);
	if (s_hc->dcbaa == NULL)
		return false;
	uint32_t hcs2 = mmio_r32(s_hc->cap, XHCI_CAP_HCSPARAMS2);
	uint32_t n_scratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
	if (n_scratch > 0) {
		uint64_t *spb_array = kmalloc_aligned(PAGE_SIZE);
		if (spb_array == NULL)
			return false;
		for (uint32_t i = 0; i < n_scratch && i < PAGE_SIZE / 8; i++) {
			void *page = kmalloc_aligned(PAGE_SIZE);
			if (page == NULL)
				return false;
			spb_array[i] = virt_to_phys(page);
		}
		s_hc->dcbaa[0] = virt_to_phys(spb_array);
		printd(DEBUG_USB, "xhci: %u scratchpad pages granted\n", n_scratch);
	}
	mmio_w64(s_hc->op, XHCI_OP_DCBAAP, virt_to_phys(s_hc->dcbaa));

	// Command ring + event ring (interrupter 0, interrupts left DISABLED —
	// we poll; see xhci.h for why that's a decision and not a shortcut).
	if (!ring_init(&s_hc->cmd))
		return false;
	mmio_w64(s_hc->op, XHCI_OP_CRCR, s_hc->cmd.phys | 1);   // | RCS

	s_hc->evt = kmalloc_aligned(PAGE_SIZE);
	if (s_hc->evt == NULL)
		return false;
	s_hc->evt_phys = virt_to_phys(s_hc->evt);
	s_hc->evt_cycle = 1;
	uint64_t *erst = kmalloc_aligned(PAGE_SIZE);   // segment table (1 entry)
	if (erst == NULL)
		return false;
	erst[0] = s_hc->evt_phys;
	erst[1] = RING_TRBS;                            // segment size in TRBs
	mmio_w32(s_hc->rt, XHCI_IR0_ERSTSZ, 1);
	mmio_w64(s_hc->rt, XHCI_IR0_ERDP, s_hc->evt_phys);
	mmio_w64(s_hc->rt, XHCI_IR0_ERSTBA, virt_to_phys(erst));

	mmio_w32(s_hc->op, XHCI_OP_CONFIG, max_slots < 16 ? max_slots : 16);
	mmio_w32(s_hc->op, XHCI_OP_USBCMD, USBCMD_RS);   // run

	s_hc->present = true;
	return true;
}

// Cut power to every port with NOTHING CONNECTED, once enumeration is done.
//
// The scan above powers every dark port (it has to — an unpowered port
// cannot even assert "connected"), but a powered EMPTY port never goes
// quiet: it sits in link-training/polling forever, and SuperSpeed
// signaling radiates broadband hash straight across the 2.4GHz band —
// Intel wrote the canonical whitepaper on USB3 ports jamming wireless
// receivers back in 2012, and it is why Logitech ships extension cradles.
// Windows hides this by parking idle links in low-power states; os64 has
// no link power management yet, so the honest v1 move is to stop lighting
// ports nobody is using. MEASURED CAUSE (the P5, 2026-08-17): the same
// wireless mouse dongle reached 10 feet under Windows and 6 inches under
// os64, same physical port — the difference was every other port on the
// machine shouting next to it.
//
// WHOLE CONTROLLERS ONLY, and the restriction is a burn scar hours old:
// the first version of this pass doused every CCS=0 port everywhere, and
// the P5 answered with both dongles enumerating ("attached") and then
// going deaf at any distance. The suspected mechanism: one physical USB3
// CONNECTOR is TWO xHCI ports — a USB2 protocol port and a USB3 protocol
// port, PAIRED, sharing the connector's VBUS. A full-speed dongle lives
// on the USB2 twin; its USB3 twin reads empty; dousing the "empty" twin
// cuts the CONNECTOR's power and browns out the device that just
// enumerated — invisible on QEMU, whose twins are not electrically
// paired. Discriminating pairing from genuine emptiness needs the
// Supported Protocol capability walk (a later slice), so v1 keeps the
// blunt-but-safe rule: a controller with ANY connected port is left
// entirely alone, and only controllers with NOTHING anywhere go dark —
// which were the loudest nuisance regardless (every port empty and
// shouting). Writing 0 to PORTSC is safe here: PP=0 is the point, the
// RW1C change bits ignore written zeros, and PED only acts on ones.
static void xhci_unpower_empty_ports(void)
{
	for (uint32_t port = 1; port <= s_hc->max_ports; port++) {
		uint32_t sc = mmio_r32(s_hc->op, XHCI_OP_PORTSC(port));
		if (sc & PORTSC_CCS) {
			printd(DEBUG_USB, "xhci: controller has connected port(s) — leaving all its ports powered\n");
			return;
		}
	}

	uint32_t doused = 0;
	for (uint32_t port = 1; port <= s_hc->max_ports; port++) {
		uint32_t sc = mmio_r32(s_hc->op, XHCI_OP_PORTSC(port));
		if (sc & PORTSC_PP) {
			mmio_w32(s_hc->op, XHCI_OP_PORTSC(port), 0);
			doused++;
		}
	}
	if (doused > 0)
		printd(DEBUG_USB, "xhci: idle controller — %u port(s) unpowered (2.4GHz hygiene)\n",
		       doused);
}

static void xhci_scan_ports(void)
{
	// Real root hubs frequently power up with ports OFF (PP=0) — a state a
	// hypervisor never shows you (QEMU ports are born powered). A device on
	// an unpowered port can't even assert "connected", so: power every dark
	// port first, then give attach detection a beat before scanning.
	bool powered_any = false;
	for (uint32_t port = 1; port <= s_hc->max_ports; port++) {
		uint32_t sc = mmio_r32(s_hc->op, XHCI_OP_PORTSC(port));
		if (!(sc & PORTSC_PP)) {
			mmio_w32(s_hc->op, XHCI_OP_PORTSC(port), PORTSC_PP);
			powered_any = true;
		}
	}
	if (powered_any)
		wait(100);   // spec allows 100ms from power-on to connect detection

	for (uint32_t port = 1; port <= s_hc->max_ports; port++) {
		uint32_t sc = mmio_r32(s_hc->op, XHCI_OP_PORTSC(port));
		if (sc & (PORTSC_CCS | (1u << 17)))   // connected, or connect-change
			// One line PER PORT on a machine with a dozen of them was the
			// single loudest thing on the boot screen. To the log (2026-08-20).
			printd(DEBUG_USB, "xhci: port %u portsc=0x%08x\n", port, sc);
		if (!(sc & PORTSC_CCS))
			continue;

		// USB3 ports enable themselves on attach; USB2 ports need a reset.
		// Writing PP|PR only: RW1C bits ignore written zeros, so nothing
		// gets acknowledged by accident.
		if (!(sc & PORTSC_PED)) {
			mmio_w32(s_hc->op, XHCI_OP_PORTSC(port), PORTSC_PP | PORTSC_PR);
			for (int i = 0; i < 200; i++) {
				sc = mmio_r32(s_hc->op, XHCI_OP_PORTSC(port));
				if ((sc & PORTSC_PED) && !(sc & PORTSC_PR))
					break;
				wait(1);
			}
		}
		if (!(sc & PORTSC_PED)) {
			printd(DEBUG_USB, "xhci: port %u connected but wouldn't enable\n", port);
			printf("xhci: port %u stuck (connected, not enabled)\n", port);   // stays on the glass: a failure is what it's for
			continue;
		}

		uint32_t speed = PORTSC_SPEED(sc);
		printd(DEBUG_USB, "xhci: port %u enabled, speed %u\n", port, speed);
		xhci_setup_hid(port, speed);
		if (s_keyboard_claimed && s_mouse_claimed)
			return;   // both input classes are now live, possibly across HCs

		// Not a wanted HID device (or setup failed): leave the slot as-is and keep
		// scanning. v1 doesn't disable-slot on failure — boot-time only,
		// nothing leaks that matters, and the code stays readable.
	}
	if (!s_hc->keyboard.present && !s_hc->mouse.present)
		printd(DEBUG_USB, "xhci: no boot-protocol keyboard or mouse found on root ports\n");
}

void init_xHCI(void)
{
	// Collect EVERY xHCI controller — real machines routinely have several
	// (Ryzen boxes like the P5 carry one in the CPU die and more in the
	// chipset), and the keyboard is plugged into whichever one owns its
	// physical port. QEMU/VBox have exactly one, which is how a first-match
	// probe survived every hypervisor and died on metal.
	// (pci.c fills `prog`, not `progIF` — the struct carries both fields
	// and only one is real. 0x30 = xHCI specifically, not OHCI/UHCI/EHCI.)
	pci_device_t *ctrls[8];
	int nctrl = 0;
	for (int i = 0; i < kPCIDeviceCount && nctrl < 8; i++)
		if (kPCIDeviceHeaders[i].class == 0x0C && kPCIDeviceHeaders[i].subClass == 0x03 &&
		    kPCIDeviceHeaders[i].prog == 0x30)
			ctrls[nctrl++] = &kPCIDeviceHeaders[i];
	for (int i = 0; i < kPCIFunctionCount && nctrl < 8; i++)
		if (kPCIDeviceFunctions[i].class == 0x0C && kPCIDeviceFunctions[i].subClass == 0x03 &&
		    kPCIDeviceFunctions[i].prog == 0x30)
			ctrls[nctrl++] = &kPCIDeviceFunctions[i];

	if (nctrl == 0) {
		printf("no xHCI controller\n");
		return;
	}

	// Keep every controller that owns a supported input device. This matters
	// on the P5, where physical USB ports may belong to different controllers.
	// Controllers with no supported root-port device remain running but need
	// no polling; their small boot-time allocations are deliberately orphaned.
	for (int c = 0; c < nctrl; c++) {
		if (s_controller_count >= MAX_XHCI_CONTROLLERS)
			break;
		// Introduced by NAME, courtesy of the OS's own PCI id database
		// (pci_devices.bin + getDeviceNameP — Chris's discovery layer doing
		// the honors, not anyone's memory).
		char devname[256];
		printf("xhci: controller %u/%u at %02x:%02x.%u — %s\n", c + 1, nctrl,
		       ctrls[c]->busNo, ctrls[c]->deviceNo, ctrls[c]->funcNo,
		       getDeviceNameP(ctrls[c], devname));   // stays on the glass: a failure is what it's for
		s_hc = &s_controllers[s_controller_count];
		memset(s_hc, 0, sizeof(*s_hc));
		if (!xhci_init_controller(ctrls[c])) {
			printf("xhci: bring-up failed on this controller\n");
			continue;
		}
		xhci_scan_ports();
		// The hygiene pass runs ONLY under USBQUIET (kernel.c has the two
		// burn scars — paired-VBUS twins, possibly ACROSS controllers on
		// this AMD platform). When enabled, every scanned controller gets
		// the pass, kept or not — an idle controller is all empty powered
		// ports, the loudest radio nuisance of the lot.
		if (kUSBQuiet)
			xhci_unpower_empty_ports();
		if (s_hc->keyboard.present || s_hc->mouse.present) {
			s_controller_count++;
		}
		if (s_keyboard_claimed && s_mouse_claimed)
			break;
	}
	printf("USB input: %s, %s\n",
	       s_keyboard_claimed ? "keyboard attached" : "no keyboard",
	       s_mouse_claimed ? "mouse attached" : "no mouse");
}

void xhci_poll(void)
{
	// Called every scheduler pass, potentially from any core. The guards
	// make the idle cost one predictable branch; the drain itself reads
	// cached RAM and only touches MMIO (ERDP/doorbells) when something
	// actually happened. The event ring is single-consumer state, so a
	// non-blocking busy flag serializes cores: a pass that loses the race
	// just skips — the winner drains everything anyway.
	static volatile uint32_t s_poll_busy = 0;

	if (s_controller_count == 0)
		return;
	if (__sync_lock_test_and_set(&s_poll_busy, 1) != 0)
		return;
	for (uint32_t i = 0; i < s_controller_count; i++) {
		s_hc = &s_controllers[i];
		xhci_drain_events();
		hid_typematic_tick(&s_hc->keyboard);
	}
	__sync_lock_release(&s_poll_busy);
}
