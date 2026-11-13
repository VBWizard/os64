// xhci.c — xHCI controller, HID input and AX210 Bluetooth transport.
// The design rationale and v1 limits live in xhci.h; this file is the
// machine. Register/TRB layouts follow the xHCI 1.x specification;
// section references below are to that spec.
//
// Memory discipline: every DMA structure (rings, contexts, buffers) comes
// from kmalloc_aligned — zeroed at the allocator choke point, physically
// contiguous, HHDM-addressed (phys = virt - kHHDMOffset). The controller's
// MMIO BAR is mapped at (kHHDMOffset | bar_phys) — an UPPER-HALF VA — so
// the registers are reachable under ANY CR3. Mapped with PAGE_PCD: this is
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
#include "driver/system/hid_keyboard.h"   // the boot-keyboard interpreter every HID keyboard shares
#include "gui/input.h"
#include "driver/system/usb/xhci.h"
#include "driver/system/usb/xhci_service.h"   // the TRB, the event ring's consumer side, the visit
#include "kusb.h"                              // kUsbDoorbell — a holder's release rings it for a refused daemon
#include "driver/system/usb/bt_intel.h"
#include "driver/system/usb/bt_scan.h"
#include "driver/system/usb/bt_le.h"
#include "driver/system/usb/bt_manager.h"
#include "driver/system/usb/bt_host.h"
#include "crypto/wipe.h"
#include "strings/sprintf.h"
#include "driver/system/hid_mouse.h"
#include "x86_64.h"          // rdtsc — a pass's deadline and measurements
#include "smp_core.h"        // get_core_local_storage — the thread that sleeps for the flag
#include "signals.h"         // SIGSLEEP

extern pci_device_t* kPCIDeviceHeaders;
extern const uint8_t bt_ax210_sfi[], bt_ax210_sfi_end[];
extern const uint8_t bt_ax210_ddc[], bt_ax210_ddc_end[];
extern pci_device_t* kPCIDeviceFunctions;
extern uint16_t kPCIDeviceCount, kPCIFunctionCount;
extern uintptr_t kHHDMOffset;
extern uintptr_t kKernelPML4v;
extern bool kUSBQuiet;   // USBQUIET — the opt-in 2.4GHz hygiene flashlight
extern bool kUsbPoll;    // USBPOLL — the flashlight: no controller is given an interrupt
extern int  kUsbImod;    // USBIMOD=<n> — the moderation interval in 250 ns units; negative = the built-in value
extern volatile uint64_t kSystemCurrentTime;

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
#define USBCMD_INTE          (1u << 2)   // interrupter enable: the controller-wide gate over every IMAN.IE
#define USBSTS_HCH           (1u << 0)
#define USBSTS_CNR           (1u << 11)

#define PORTSC_CCS           (1u << 0)   // device connected
#define PORTSC_PED           (1u << 1)   // port enabled
#define PORTSC_PR            (1u << 4)   // port reset
#define PORTSC_PP            (1u << 9)   // port power
#define PORTSC_SPEED(v)      (((v) >> 10) & 0xF)  // 1=FS 2=LS 3=HS 4=SS

// Interrupter 0 (runtime base + 0x20)
#define XHCI_IR0_IMAN        0x20
#define XHCI_IR0_IMOD        0x24   // moderation: interval (bits 0..15, 250 ns units), countdown above
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
#define TRB_DISABLE_SLOT     10
#define TRB_ADDRESS_DEVICE   11
#define TRB_CONFIG_ENDPOINT  12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_RESET_ENDPOINT 14
#define TRB_SET_TR_DEQUEUE 16
#define TRB_EV_TRANSFER      32
#define TRB_EV_CMD_COMPLETE  33
#define TRB_EV_PORT_STATUS   34

#define TRB_CYCLE            XHCI_TRB_CYCLE
#define TRB_TOGGLE_CYCLE     (1u << 1)
#define TRB_IOC              (1u << 5)
#define TRB_ISP              (1u << 2)   // event on short packet
#define TRB_IDT              (1u << 6)   // immediate data (Setup stage)
#define TRB_RESET_TSP        (1u << 9)   // Reset Endpoint: preserve transfer state

#define TRB_CC(status)       (((status) >> 24) & 0xFF)
#define TRB_CC_SUCCESS       1
#define TRB_CC_USB_TRANSACTION_ERROR 4
#define TRB_CC_SHORT_PACKET  13

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

// Root-port enumeration state, retained for a bound HID interface. Keyboards
// use boot reports; mice use a parsed layout when available, with boot fallback.
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
	uint32_t     report_bytes;           // transfer length, bounded by endpoint MPS

	// Boot-keyboard state (hid_keyboard.h). Unused (and zero) for a mouse.
	hid_keyboard_t kbd;
	// Mouse buttons (gui/input.h). Unused (and zero) for a keyboard.
	input_pointer_source_t pointer;
	// Bounded DEBUG_USB evidence: one sample per length and eight nonzero
	// wheel samples, so pointer movement cannot exhaust wheel samples.
	uint64_t     mouse_lengths_seen;
	uint8_t      mouse_wheel_samples;
	bool         mouse_report_protocol;
	hid_mouse_layout_t mouse_layout;
} xhci_device_t;

typedef struct {
	xhci_ring_t ring;
	uint8_t *buffer;
	uint64_t pending_trb;
	bt_usb_endpoint_t endpoint;
	bt_hci_stream_t stream;
	bool retry_pending, retried, rearm_pending;
	// For xhci_bt_log_usb_state: completions taken, the last one's code,
	// and events for this endpoint that named a transfer not being waited on.
	uint32_t completions, unmatched;
	uint8_t last_cc;
} xhci_bt_endpoint_t;

// ACL packets on their way to the adapter (AUDIO_CLASSIC.md § T2). The
// transmit endpoint's storage is one allocation: its first page is what
// bring-up sends from, with the runtime command's bytes at offset 512 of
// it; after that page come BT_TX_SLOTS send slots. A packet is copied into
// a free slot and queued, so several can be with the controller at once and
// none can reach the command's bytes, whatever its length. Each slot lies
// inside one page.
#define BT_TX_SLOTS       4
#define BT_TX_SLOT_STRIDE 2048
#define BT_TX_SLOT_BYTES  (BT_LE_ACL_MAX + 4)   // an HCI ACL packet as large as the receive side assembles
#define BT_TX_BYTES       (PAGE_SIZE + BT_TX_SLOTS * BT_TX_SLOT_STRIDE)
_Static_assert(BT_TX_SLOT_BYTES <= BT_TX_SLOT_STRIDE && PAGE_SIZE % BT_TX_SLOT_STRIDE == 0,
               "a send slot lies inside one page");
typedef struct {
	uint64_t trb;        // the transfer queued for it
	uint64_t deadline;   // the tick by which that transfer must have completed
} xhci_bt_tx_slot_t;

// Intel transport state. Bring-up uses a stack instance, then transfers a
// successful device into controller-owned storage for discovery and LE input.
typedef struct {
	xhci_device_t *device;
	xhci_bt_endpoint_t rx[2]; // Interrupt events; bulk carries bootloader events, then ACL.
	xhci_bt_endpoint_t tx;
	bt_intel_events_t events;
	uint64_t deadline;
	uint32_t tx_bytes;
	// No transmit is with the controller. At runtime that is "no send slot
	// is in flight"; bring-up has the one transfer.
	bool tx_done;
	// The send slots in flight, oldest first: tx_slots of them, starting at
	// slot tx_oldest. Transfers on one endpoint complete in the order they
	// were queued, so the oldest is the one a completion is for.
	xhci_bt_tx_slot_t tx_slot[BT_TX_SLOTS];
	uint8_t tx_oldest, tx_slots;
	bool listening, failed;
	unsigned packets;
	bool runtime;
	bt_host_t host;
	// Preserve the last round's timing when LE cleanup clears scan results.
	struct {
		uint64_t started_at, finished_at, started_ms, finished_ms;
		bool valid, finished, failed, le_only;
	} scan_history;
	hid_keyboard_t keyboards[BT_HOST_PEERS];
	input_pointer_source_t pointers[BT_HOST_PEERS];
	uint8_t timeline_phase[BT_HOST_PEERS];   // each slot's phase as /sys/usb's timeline last wrote it
	// The adapter's port, as its status-change events left it, and control
	// endpoint events that named no request in flight (xhci_bt_log_usb_state).
	uint32_t port_changes, port_last_sc, ep0_unmatched;
	// The head of each of the last few HCI events, oldest overwritten: what
	// the adapter was saying when it stopped answering. Six bytes reach an
	// event's code, its length and a command's opcode, and stop short of
	// anything an event carries that is secret.
	uint8_t recent_event[4][6];
	uint32_t recent_events;
	// What xhci_bt_log_trouble has already said, so each trouble is one line.
	struct {
		uint8_t rounds;
		bool failed;
		uint32_t event_resyncs, acl_resyncs;
		const char *error[BT_HOST_PEERS];
		bool blocked[BT_HOST_PEERS];
	} logged;
} xhci_bt_probe_t;

// One controller. The P5 may place its keyboard and mouse on different xHCI
// controllers, so initialized controllers remain live and are all drained.
typedef struct {
	uint8_t pci_bus, pci_device, pci_function;
	uint32_t     pci_bars[6];     // as enumeration read them (the MSI-X table is found through one)
	bool         present;         // controller found and running
	uint8_t     *cap;             // MMIO: capability base (HHDM-aliased)
	uint8_t     *op;              // MMIO: operational base
	uint8_t     *rt;              // MMIO: runtime base
	uint32_t    *db;              // MMIO: doorbell array
	uint32_t     ctx_size;        // 32 or 64 (HCCPARAMS1.CSZ)
	uint32_t     max_ports;
	uint64_t    *dcbaa;           // device context base address array
	xhci_ring_t  cmd;             // command ring
	xhci_evt_ring_t evt;          // event ring segment (consumer side)
	xhci_wait_t  wait;            // how long an event on it may have waited (/sys/usb)

	// last command completion (filled by the event drain, consumed by
	// xhci_run_command's synchronous wait)
	volatile bool     cmd_done;
	volatile uint8_t  cmd_cc;
	volatile uint32_t cmd_slot;   // slot id byte from the completion
	uint64_t          command_trb;

	xhci_device_t keyboard;
	xhci_device_t mouse;
	xhci_bt_probe_t *bt_probe;
	xhci_bt_probe_t bluetooth;
	xhci_device_t bluetooth_device;

	// One EP0 request per controller: synchronous during enumeration,
	// asynchronous from the runtime Bluetooth step of kusb's pass.
	volatile uint32_t control_slot;
	volatile bool     xfer_done;
	volatile uint8_t  xfer_cc;
	uint64_t          control_setup_trb;
	uint64_t          control_data_trb;
	uint64_t          control_status_trb;
	volatile uint16_t xfer_actual;

	// Events kusb's passes have drained from this controller (/sys/usb).
	uint64_t          events_polled;

	// How it is driven: decided once, by kusb, in xhci_interrupts_enable.
	xhci_drive_choice_t drive;
	// THE TOP HALF'S MARK, written in interrupt context and taken by kusb's
	// drain in one exchange. Non-zero: an interrupt was acknowledged for
	// this ring since a drain last took the mark, so a drain that handles
	// nothing must read ERDP and retire the busy bit if it is set. The
	// value is the TSC at the OLDEST such interrupt, which is what the
	// interrupt-to-service time is measured from; one word, so the mark
	// and its stamp cannot be taken apart.
	volatile uint64_t irq_at;
	volatile uint64_t irq_fires;
	// From an interrupt to the start of the drain that answered it, in
	// cycles: the longest, the longest since /sys/usb was last read, and
	// how many took longer than a tick.
	uint64_t          irq_service_max, irq_service_recent, irq_service_late;
} xhci_t;

#define MAX_XHCI_CONTROLLERS 8
#define HID_INFLIGHT 8
#define HID_REPORT_BYTES HID_MOUSE_REPORT_BYTES

static xhci_t s_controllers[MAX_XHCI_CONTROLLERS];
static uint32_t s_controller_count;
static bool s_keyboard_claimed;
static bool s_mouse_claimed;
static uint32_t s_ax210_described;
// The controller being worked on: by the boot thread during setup, by kusb
// during a pass. Nothing else reads or writes it, so it is never changed
// concurrently.
static xhci_t *s_hc;
// THE TOP HALF'S LIST: the controllers that interrupt, as a prefix that only
// grows. kusb writes an entry whole and then raises the count past it; the
// handler reads the count and never looks beyond it, so it never sees a
// half-published controller. Through an entry it reads `op` and `rt`, which
// are set at bring-up and not changed after, and writes that controller's
// interrupt mark and fire count.
static xhci_t *s_irq_list[MAX_XHCI_CONTROLLERS];
static volatile uint32_t s_irq_count;
static volatile uint64_t s_irq_spurious;   // handler entries that found no controller signalling
static xhci_t *s_bluetooth_hc; // First operational AX210 retained for /sys/bluetooth.
// THE SERIALIZING FLAG. Whoever holds it owns the Bluetooth host state and,
// if it is kusb, the event rings. kusb's pass, a sysfs read or request and
// kworker's bond work take it by test-and-set and never wait for it, with
// one exception (kworker publishing a bond it has read or written).
// s_kusb_waiting is how a refused kusb is brought back: it sets the mark
// BEFORE it tries the flag, and every other holder releases through
// xhci_serial_release, which rings kusb's bell if the mark is up. Set then
// try on one side, release then look on the other: either kusb's try
// succeeds or the releaser sees the mark.
static volatile uint32_t s_poll_busy;
static volatile uint32_t s_kusb_waiting;

// Every holder but kusb lets go of the serializing flag here. The clearing
// store is sequentially consistent, like the exchange after it and kusb's
// two stores before its try: the handshake's claim is about the order of
// those four, and a weaker store would make it true of x86 only.
static void xhci_serial_release(void)
{
	__atomic_store_n(&s_poll_busy, 0, __ATOMIC_SEQ_CST);
	if (__atomic_exchange_n(&s_kusb_waiting, 0, __ATOMIC_SEQ_CST))
		doorbell_ring(&kUsbDoorbell);
}
static volatile uint32_t s_bond_io_busy;
static bool s_bond_manager_ready;

extern volatile uint64_t kTicksSinceStart;   // Typematic and USB/Bluetooth deadlines.
extern uint64_t kCPUCyclesPerSecond;         // TSC cycles to time, for a pass's measurements.

// ── What a pass measures about itself (/sys/usb) ────────────────────────────
//
// A device that is slow to answer has two possible causes that look alike
// from a chair: the controller held an event and nobody came round to read
// it, or the event had not happened yet (a Bluetooth scan that has not seen
// the device). These numbers tell them apart.
//
// A PASS is one visit by kusb that held the flag. Its GAP is the time since
// the pass before it began. The gap is a CADENCE: how often kusb comes
// round. It does not bound how long an event waited, because a pass can
// begin on time and reach a controller late, and work carried over from one
// pass to the next is older than any gap. That bound is kept per controller
// (xhci_t.wait, xhci_service.h): from the last time the controller's ring
// was seen empty to the drain that found events on it.
//
// The TIMELINE lists what Bluetooth did and when, each line with the
// passes, events and longest gap since the line before, so a slow step can
// be laid beside how often kusb came round during it.
//
// Everything here is written by kusb under s_poll_busy, on the one core it
// is pinned to, which is what makes the TSC differences meaningful.

static uint64_t s_polls;               // passes
static uint64_t s_polls_with_events;   // of those, the ones that drained an event
static uint64_t s_poll_last_tsc;       // when the previous pass began
static uint64_t s_poll_gap_max;        // longest gap of any pass, in cycles
static uint64_t s_poll_gap_recent;     // the same, since /sys/usb was last read
// Gaps of the passes that drained events, by size. The first edge is two
// ticks, so a pass every tick lands in the first bucket and not on its
// boundary. The per-controller waits use the same edges.
static uint64_t s_event_gap_max;       // cycles
static uint64_t s_event_gap_20ms, s_event_gap_200ms, s_event_gap_2s, s_event_gap_over_2s;
// kusb turned away by a held flag: how often, when the wait in hand began
// (0: none), and the longest from a refusal to the pass that did the work.
static uint64_t s_refusals;
static uint64_t s_refused_at;
static uint64_t s_refusal_wait_max;    // cycles
// The longest time from a wake's start to the Bluetooth step that follows
// the drains: the deadline's bound, as it turned out (cycles).
static uint64_t s_step_delay_max;
// Passes that ended with a controller stopped by the deadline or not
// reached.
static uint64_t s_passes_left_work;
// Where the next pass starts, so the controller that used the time is
// served last.
static xhci_visit_t s_visit;

typedef enum {
	XHCI_TL_SCAN_STARTED,
	XHCI_TL_SCAN_FINISHED,   // `to` = devices found, `from` = 1 when the scan failed
	XHCI_TL_LE_PHASE,        // `slot` moved from phase `from` to phase `to`
} xhci_timeline_kind_t;

typedef struct {
	uint64_t at_ms;          // uptime
	uint8_t  kind, slot, from, to;
	uint32_t polls;          // passes since the previous line
	uint32_t events;         // events those passes drained
	uint64_t gap_max;        // the longest gap among them, in cycles
} xhci_timeline_entry_t;

// The last XHCI_TIMELINE lines. A reconnection walks about fifteen phases
// and a waiting peer costs two lines a scan round, so this holds a
// reconnection and several minutes of the waiting before it.
#define XHCI_TIMELINE 64
static xhci_timeline_entry_t s_timeline[XHCI_TIMELINE];
static uint64_t s_timeline_count;      // lines ever written; the ring keeps the newest
// Since the previous line.
static uint32_t s_interval_polls, s_interval_events;
static uint64_t s_interval_gap_max;

static void xhci_timeline_add(xhci_timeline_kind_t kind, unsigned slot, unsigned from, unsigned to)
{
	xhci_timeline_entry_t *e = &s_timeline[s_timeline_count++ % XHCI_TIMELINE];
	e->at_ms = kTicksSinceStart * 1000 / TICKS_PER_SECOND;
	e->kind = (uint8_t)kind;  e->slot = (uint8_t)slot;
	e->from = (uint8_t)from;  e->to = (uint8_t)to;
	e->polls = s_interval_polls;  e->events = s_interval_events;
	e->gap_max = s_interval_gap_max;
	s_interval_polls = s_interval_events = 0;
	s_interval_gap_max = 0;
}

// A pass begins at `now`: its gap is known at once, and counted before any
// work, so that a timeline line written during this pass includes it. The
// event that moves Bluetooth on is drained by the pass that writes the
// line. Returns the gap in cycles; the first pass has no pass before it and
// so no gap.
static uint64_t xhci_pass_began(uint64_t now)
{
	uint64_t gap = s_poll_last_tsc ? now - s_poll_last_tsc : 0;
	s_poll_last_tsc = now;
	s_polls++;
	s_interval_polls++;
	if (gap > s_poll_gap_max) s_poll_gap_max = gap;
	if (gap > s_poll_gap_recent) s_poll_gap_recent = gap;
	if (gap > s_interval_gap_max) s_interval_gap_max = gap;
	return gap;
}

// A pass ends having drained `events` after a gap of `gap` cycles.
static void xhci_pass_ended(uint64_t gap, uint32_t events)
{
	if (events == 0)
		return;
	s_polls_with_events++;
	if (gap > s_event_gap_max) s_event_gap_max = gap;
	uint64_t ms = kCPUCyclesPerSecond / 1000;
	if (gap <= 20 * ms) s_event_gap_20ms++;
	else if (gap <= 200 * ms) s_event_gap_200ms++;
	else if (gap <= 2000 * ms) s_event_gap_2s++;
	else s_event_gap_over_2s++;
}

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

#define XHCI_BT_PORT_LINES 8   // port changes of the Bluetooth adapter said in the log

// Acknowledge a port's change bits (PORTSC 17..23, write-one-to-clear). The
// controller raises a port event only when a change bit goes from clear to
// set, so a bit left standing hides every later change of its kind. Power
// is written back as it stands; enable and reset are written as zeros,
// which leave them alone.
#define PORTSC_CHANGES       0x00FE0000u
static void xhci_port_ack_changes(uint32_t port)
{
	uint32_t sc = mmio_r32(s_hc->op, XHCI_OP_PORTSC(port));
	mmio_w32(s_hc->op, XHCI_OP_PORTSC(port), (sc & PORTSC_PP) | (sc & PORTSC_CHANGES));
}

static void xhci_handle_transfer_event(const xhci_trb_t *ev);
static void xhci_hid_lost(xhci_device_t *dev);

// Handle one event of s_hc's ring: the bottom half's work, in kusb's thread
// (or the boot thread's, before there is one).
static void xhci_handle_event(void *ctx, const xhci_trb_t *ev)
{
	(void)ctx;
	uint32_t control = ev->control;
	switch (TRB_GET_TYPE(control)) {
		case TRB_EV_CMD_COMPLETE:
			// A late completion must not authorize freeing another slot's DMA.
			if (ev->param != s_hc->command_trb)
				break;
			s_hc->cmd_cc = (uint8_t)TRB_CC(ev->status);
			s_hc->cmd_slot = (control >> 24) & 0xFF;
			s_hc->cmd_done = true;
			break;
		case TRB_EV_TRANSFER:
			xhci_handle_transfer_event(ev);
			break;
		case TRB_EV_PORT_STATUS: {
			// v1: no hotplug, so nothing is enumerated here. A port that
			// has LOST its device still matters: whatever that device
			// held is released (xhci_hid_lost).
			uint32_t port = (uint32_t)(ev->param >> 24) & 0xFF;
			if (port < 1 || port > s_hc->max_ports)
				break;   // not a port this controller has
			uint32_t sc = mmio_r32(s_hc->op, XHCI_OP_PORTSC(port));
			bool connected = (sc & PORTSC_CCS) != 0;
			printd(DEBUG_USB, "xhci: port %u status change (%s)\n", port,
			       connected ? "connected" : "disconnected");
			// The Bluetooth adapter's port is said in the system log
			// whatever the change: an adapter that leaves the bus and comes
			// straight back reads "connected" here and is otherwise unseen.
			if (s_hc->bt_probe && s_hc->bt_probe->device->port == port) {
				xhci_bt_probe_t *bt = s_hc->bt_probe;
				bt->port_last_sc = sc;
				if (++bt->port_changes <= XHCI_BT_PORT_LINES)
					printd(DEBUG_EXCEPTIONS, "bluetooth: the adapter's USB port %u changed: PORTSC %08x (%s, %s, link state %u, changes %02x)%s\n",
					       port, sc, connected ? "connected" : "disconnected",
					       (sc & PORTSC_PED) ? "enabled" : "disabled", (sc >> 5) & 0xF, (sc >> 17) & 0x7F,
					       bt->port_changes == XHCI_BT_PORT_LINES ? "; further changes are counted, not logged" : "");
				xhci_port_ack_changes(port);
			}
			if (!connected) {
				if (s_hc->bt_probe && s_hc->bt_probe->device->port == port) {
					s_hc->bt_probe->failed = true;
					s_hc->bt_probe->listening = false;
				}
				if (s_hc->keyboard.present && s_hc->keyboard.port == port)
					xhci_hid_lost(&s_hc->keyboard);
				if (s_hc->mouse.present && s_hc->mouse.port == port)
					xhci_hid_lost(&s_hc->mouse);
			}
			break;
		}
		default:
			printd(DEBUG_USB, "xhci: unhandled event type %u cc %u\n",
			       TRB_GET_TYPE(control), TRB_CC(ev->status));
			break;
	}
}

static uint64_t xhci_clock(void *ctx)
{
	(void)ctx;
	return rdtsc();
}

// Drain s_hc's event ring until it is empty or the TSC passes `deadline`
// (0: no deadline, drain to empty), then write ERDP if the drain made
// progress: the empty slot when it drained to empty, the last event handled
// when the deadline stopped it (xhci_service.h says why they differ). A
// drain that handled nothing writes nothing, unless the top half
// acknowledged an interrupt for this ring and the busy bit reads set: then
// the pointer is written back once, to retire the bit.
static xhci_evt_drain_t xhci_drain_until(uint64_t deadline)
{
	// The mark is taken BEFORE the drain: an interrupt acknowledged while
	// the drain runs raises it again, for the next one. It is never set for
	// a tick-driven controller or before kusb enables interrupts, and then
	// ERDP is never read.
	uint64_t at = __atomic_exchange_n(&s_hc->irq_at, 0, __ATOMIC_SEQ_CST);
	bool owed = at != 0;
	if (owed) {
		uint64_t now = rdtsc();
		if (now > at) {
			uint64_t waited = now - at;
			if (waited > s_hc->irq_service_max) s_hc->irq_service_max = waited;
			if (waited > s_hc->irq_service_recent) s_hc->irq_service_recent = waited;
			if (waited > kCPUCyclesPerSecond / TICKS_PER_SECOND) s_hc->irq_service_late++;
		}
	}
	xhci_evt_drain_t drain = xhci_evt_drain(&s_hc->evt, xhci_handle_event, NULL,
	                                        deadline ? xhci_clock : NULL, NULL, deadline);
	bool reads_set = xhci_evt_erdp_wants_read(drain, owed) &&
	                 (mmio_r32(s_hc->rt, XHCI_IR0_ERDP) & XHCI_ERDP_EHB) != 0;
	xhci_erdp_t erdp = xhci_evt_erdp(&s_hc->evt, drain, owed, reads_set);
	if (erdp.write) {
		mmio_w64(s_hc->rt, XHCI_IR0_ERDP, erdp.value);
		xhci_evt_published(&s_hc->evt, erdp);
	}
	return drain;
}

// Consume every event the controller has posted. Returns the count. The
// boot-time waits call this; the scheduler is not running then, so there is
// nothing a deadline would be making room for.
static uint32_t xhci_drain_events(void)
{
	return xhci_drain_until(0).handled;
}

// ── Synchronous command execution (boot-time enumeration) ───────────────────

// Push a command TRB, ring the command doorbell, spin on the event ring
// until its completion arrives. Boot-time only — the scheduler isn't
// running yet, so spinning is honest. Returns the completion code
// (TRB_CC_SUCCESS == 1) or 0 on timeout.
static uint8_t xhci_run_command(uint64_t param, uint32_t status, uint32_t control)
{
	s_hc->cmd_done = false;
	s_hc->command_trb = ring_push(&s_hc->cmd, param, status, control);
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
static bool xhci_control_request(xhci_device_t *dev,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 void *data, uint16_t wLength)
{
	bool dir_in = (bmRequestType & 0x80) != 0;
	if (wLength > PAGE_SIZE || (wLength && data == NULL))
		return false;
	// Allocate before publishing Setup: an allocation failure must not leave
	// half a request on EP0's ring. Timeouts retain storage for possible late DMA.
	uint8_t *bounce = NULL;
	if (wLength > 0) {
		bounce = kmalloc_aligned(PAGE_SIZE);
		if (bounce == NULL)
			return false;
		if (!dir_in)
			memcpy(bounce, data, wLength);
	}

	// Setup stage: the 8 setup bytes ride IN the TRB (IDT). TRT (bits
	// 16-17 of control): 0 = no data, 2 = OUT data, 3 = IN data.
	uint64_t setup = (uint64_t)bmRequestType | ((uint64_t)bRequest << 8) |
	                 ((uint64_t)wValue << 16) | ((uint64_t)wIndex << 32) |
	                 ((uint64_t)wLength << 48);
	uint32_t trt = (wLength == 0) ? 0 : (dir_in ? 3 : 2);
	s_hc->control_setup_trb = ring_push(&dev->ep0, setup, 8,
	                                   TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));

	// Data stage (bounced through an HHDM scratch buffer — caller's buffer
	// may be anywhere; DMA needs a physical address we control).
	s_hc->control_data_trb = 0;
	s_hc->xfer_actual = wLength;
	if (wLength > 0) {
		s_hc->control_data_trb = ring_push(&dev->ep0, virt_to_phys(bounce), wLength,
		          TRB_TYPE(TRB_DATA) | TRB_ISP | (dir_in ? (1u << 16) : 0));
	}

	// Status stage: direction opposite the data stage (or IN when no data).
	// IOC — this is the completion we wait for.
	uint32_t status_dir = (wLength == 0 || !dir_in) ? (1u << 16) : 0;
	s_hc->control_slot = dev->slot;
	s_hc->xfer_done = false;
	s_hc->xfer_cc = 0;
	s_hc->control_status_trb = ring_push(&dev->ep0, 0, 0,
	                                    TRB_TYPE(TRB_STATUS) | status_dir | TRB_IOC);

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
		memcpy(data, bounce, s_hc->xfer_actual);
	if (bounce != NULL && s_hc->xfer_done)
		kfree(bounce);
	if (!ok)
	{
		printd(DEBUG_USB, "xhci: control req 0x%02x/0x%02x failed (cc=%u)\n",
		       bmRequestType, bRequest, s_hc->xfer_cc);
		printf("xhci: ctrl req %02x/%02x failed cc=%u\n", bmRequestType, bRequest, s_hc->xfer_cc);   // stays on the glass: a failure is what it's for
	}
	return ok;
}

// Decode before injecting: unrelated report IDs and truncated reports must
// not synthesize movement or release buttons. HID Y is screen-positive;
// the wheel has the opposite sign to os64's positive-down convention.
static void hid_process_mouse_report(xhci_device_t *dev, const uint8_t *rep, uint32_t length)
{
    hid_mouse_sample_t sample;
    bool decoded = false;
    if (dev->mouse_report_protocol) {
        decoded = hid_mouse_decode(&dev->mouse_layout, rep, length, &sample);
    } else if (length >= 3) {
        sample = (hid_mouse_sample_t){.buttons = rep[0] & 7,
            .x = (int8_t)rep[1], .y = (int8_t)rep[2],
            .wheel = length >= 4 ? (int8_t)rep[3] : 0};
        decoded = true;
    }
    if ((kDebugLevel & DEBUG_USB) && length && length <= HID_REPORT_BYTES) {
        uint64_t length_bit = 1ull << (length - 1);
        bool new_length = !(dev->mouse_lengths_seen & length_bit);
        bool wheel = decoded && sample.wheel && dev->mouse_wheel_samples < 8;
        if (new_length || wheel) {
            uint8_t bytes[8] = {0};
            memcpy(bytes, rep, length < sizeof(bytes) ? length : sizeof(bytes));
            dev->mouse_lengths_seen |= length_bit;
            if (wheel) dev->mouse_wheel_samples++;
            // At most eight raw bytes; bytes beyond len are zero padding.
            printd(DEBUG_USB, "xhci: mouse report slot %u len %u decoded %u wheel %d bytes %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   dev->slot, length, decoded, decoded ? sample.wheel : 0,
                   bytes[0], bytes[1], bytes[2], bytes[3],
                   bytes[4], bytes[5], bytes[6], bytes[7]);
        }
    }
    if (!decoded) return;
    // Negating the most negative 16-bit wheel value needs a wider temporary.
    int32_t wheel = -(int32_t)sample.wheel;
    if (wheel > 32767) wheel = 32767;
    input_inject_mouse(&dev->pointer, sample.x, sample.y, sample.buttons, (int16_t)wheel);
}

// A device that stops reporting while holding something must let go of it:
// the machine counts held modifiers and buttons per source (keyboard.h,
// gui/input.h), and nothing else can bring a vanished source's count down,
// so a keyboard pulled with Ctrl held would leave Ctrl on every pointer
// packet until reboot. Its keys, modifiers and buttons are released as if
// it had reported letting go. Idempotent: a second call finds nothing held.
static void xhci_hid_lost(xhci_device_t *dev)
{
	if (dev->kind == HID_KEYBOARD) {
		static const uint8_t none[8];
		hid_keyboard_report(&dev->kbd, none);
	} else {
		input_pointer_unregister(&dev->pointer);
	}
}

// ── Transfer events (HID reports and Intel Bluetooth) ────────────────────────

static void xhci_arm_report_trb(xhci_device_t *dev, uint32_t buf_index)
{
	uint64_t buf_phys = dev->reports_phys + buf_index * HID_REPORT_BYTES;
	ring_push(&dev->intr, buf_phys, dev->report_bytes,
	          TRB_TYPE(TRB_NORMAL) | TRB_IOC);
	mmio_w32((uint8_t *)s_hc->db, 4 * dev->slot, dev->dci);
}

static void xhci_bt_arm(xhci_bt_probe_t *probe, xhci_bt_endpoint_t *rx)
{
	// One USB packet per TD keeps an exact-MPS response from waiting for a
	// short packet to complete a larger receive. HCI framing is reassembled.
	rx->pending_trb = ring_push(&rx->ring, virt_to_phys(rx->buffer),
	                            rx->endpoint.packet_bytes,
	                            TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
	uint32_t dci = (rx->endpoint.address & 15) * 2 + 1;
	mmio_w32((uint8_t *)s_hc->db, 4 * probe->device->slot, dci);
}

static void xhci_bt_pointer_register(xhci_bt_probe_t *p,unsigned peer)
{
    if(p->pointers[peer].settings_slot) return;
    const bt_le_t *s=&p->host.peers[peer].le;
    const uint8_t *a=s->bond.valid?s->bond.peer:s->peer;
    unsigned type=s->bond.valid?s->bond.address_type:s->address_type;
    char key[64],name[64];
    snprintf(key,sizeof(key),"bt-%u-%02x%02x%02x%02x%02x%02x",type,a[5],a[4],a[3],a[2],a[1],a[0]);
    snprintf(name,sizeof(name),"Bluetooth mouse (%02x:%02x:%02x:%02x:%02x:%02x)",
        a[5],a[4],a[3],a[2],a[1],a[0]);
    input_pointer_register(&p->pointers[peer],key,name);
}

static void xhci_bt_input_report(void *context, unsigned peer, const bt_le_input_t *report)
{
    xhci_bt_probe_t *p = context;
    if (report->mouse) {
        if(!bt_le_input_active(&p->host.peers[peer].le)) {
            input_pointer_unregister(&p->pointers[peer]); return;
        }
        xhci_bt_pointer_register(p,peer);
        int32_t wheel = -(int32_t)report->pointer.wheel;
        if (wheel > 32767) wheel = 32767;
        input_inject_mouse(&p->pointers[peer], report->pointer.x, report->pointer.y,
                           report->pointer.buttons, (int16_t)wheel);
    } else hid_keyboard_report(&p->keyboards[peer], report->keys);
}

// THE ADAPTER IS RESET ON THE WAY DOWN (xhci_bluetooth_quiesce_begin). Its
// firmware runs on through a warm reboot, links and all, and on the P5 an
// adapter met in that state has refused its start-up reset and every
// command after it until its power was cycled (DEBTS). Asked while the
// machine is still up and the adapter healthy, the same reset leaves the
// next boot an idle controller. The shutdown thread asks; kusb, which owns
// the adapter, sends; the reply is taken where events arrive.
enum { BT_QUIESCE_NONE, BT_QUIESCE_ASKED, BT_QUIESCE_SENT, BT_QUIESCE_ANSWERED, BT_QUIESCE_REFUSED };
static volatile uint8_t s_bt_quiesce;

static void xhci_bt_runtime_event(void *context, const uint8_t *event, size_t bytes)
{
	xhci_bt_probe_t *p = context;
	uint8_t *head = p->recent_event[p->recent_events++ % 4];
	memset(head, 0, 6);
	memcpy(head, event, bytes < 6 ? bytes : 6);
	if (s_bt_quiesce != BT_QUIESCE_NONE) {
		// The host is no longer run, so nothing is fed to it; the one event
		// that matters is the reset's Command Complete.
		if (s_bt_quiesce == BT_QUIESCE_SENT && bytes >= 6 && event[0] == 0x0e &&
		    event[3] == 0x03 && event[4] == 0x0c)
			s_bt_quiesce = event[5] == 0 ? BT_QUIESCE_ANSWERED : BT_QUIESCE_REFUSED;
		crypto_wipe(p->rx[0].stream.bytes, sizeof(p->rx[0].stream.bytes));
		return;
	}
	bt_host_event(&p->host, event, bytes);
	crypto_wipe(p->rx[0].stream.bytes, sizeof(p->rx[0].stream.bytes));
}

static uint8_t *xhci_bt_tx_slot(const xhci_bt_probe_t *p, unsigned slot)
{
	return p->tx.buffer + PAGE_SIZE + slot * BT_TX_SLOT_STRIDE;
}

static bool xhci_bt_transfer(const xhci_trb_t *ev, uint32_t slot, uint32_t dci, uint8_t cc)
{
	xhci_bt_probe_t *probe = s_hc->bt_probe;
	if (!probe || slot != probe->device->slot)
		return false;
	if (probe->tx.endpoint.address &&
	    dci == (uint32_t)(probe->tx.endpoint.address & 15) * 2) {
		bool mine;
		if (probe->runtime) {
			// The oldest slot in flight, or not ours: an event for anything
			// else (a transfer from before the runtime began) is counted
			// and frees nothing. A slot whose completion never comes is
			// found by its deadline (xhci_bt_runtime_poll).
			mine = probe->tx_slots && ev->param == probe->tx_slot[probe->tx_oldest].trb;
			if (mine) {
				crypto_wipe(xhci_bt_tx_slot(probe, probe->tx_oldest), BT_TX_SLOT_BYTES);
				probe->tx_oldest = (uint8_t)((probe->tx_oldest + 1) % BT_TX_SLOTS);
				probe->tx_done = --probe->tx_slots == 0;
			} else
				probe->tx.unmatched++;
		} else {
			mine = ev->param == probe->tx.pending_trb && !probe->tx_done;
			if (mine) probe->tx_done = true;
		}
		if (mine && (cc != TRB_CC_SUCCESS || (ev->status & 0xffffff))) {
			// The endpoint halts on a failed transfer, and what was queued
			// behind it will not complete: the transport is finished.
			probe->failed = true;
			printd(DEBUG_USB, "xhci: AX210 bulk OUT failed cc=%u residual=%u\n",
			       cc, ev->status & 0xffffff);
		}
		return true;
	}
	for (unsigned i = 0; i < 2; i++) {
		xhci_bt_endpoint_t *rx = &probe->rx[i];
		if (dci != (uint32_t)(rx->endpoint.address & 15) * 2 + 1)
			continue;
		if (!probe->listening || ev->param != rx->pending_trb) {
			rx->unmatched++;
			return true;
		}
		uint32_t residual = ev->status & 0xffffff;
		rx->completions++;
		rx->last_cc = cc;
		// Firmware startup can interrupt a receive without transferring data.
		// Defer one host-side soft reset per endpoint to the wait loop; issuing
		// a synchronous command here would recursively drain this same event.
		if (cc == TRB_CC_USB_TRANSACTION_ERROR && residual == rx->endpoint.packet_bytes &&
		    probe->events.watch_boot && probe->events.opcode == 0xfc01 &&
		    !rx->retried && !rx->retry_pending) {
			rx->retry_pending = true;
			printd(DEBUG_USB, "xhci: AX210 receive endpoint %02x transaction error during firmware start; scheduling retry\n",
			       rx->endpoint.address);
			return true;
		}
		if ((cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PACKET) ||
		    residual > rx->endpoint.packet_bytes) {
			printd(DEBUG_USB, "xhci: AX210 receive endpoint %02x failed cc=%u residual=%u\n",
			       rx->endpoint.address, cc, residual);
			probe->failed = true;
			probe->listening = false;
			return true;
		}
		if (probe->runtime) {
			// Operational HCI events use interrupt IN; bulk IN carries ACL data.
			// Re-arm after the drain so radio traffic cannot monopolize a pass.
			if (i == 0)
				bt_hci_feed(&rx->stream, xhci_bt_runtime_event, probe, rx->buffer,
				            rx->endpoint.packet_bytes - residual, rx->endpoint.packet_bytes);
			else
				bt_host_receive(&probe->host, rx->buffer, rx->endpoint.packet_bytes - residual,
				              rx->endpoint.packet_bytes,
				              xhci_bt_input_report, probe);
			crypto_wipe(rx->buffer, rx->endpoint.packet_bytes);
			rx->rearm_pending = true;
			return true;
		}
		probe->packets++;
		bt_intel_feed_events(&rx->stream, &probe->events, rx->buffer,
		                     rx->endpoint.packet_bytes - residual);
		// Bound unsolicited traffic as well as time: a continuously ready
		// device must not keep the boot-time event drain spinning indefinitely.
		if (!probe->events.malformed && !probe->events.download_failed && probe->packets < 64)
			xhci_bt_arm(probe, rx);
		else {
			probe->failed = true;
			probe->listening = false;
		}
		return true;
	}
	return false;
}

static void xhci_handle_transfer_event(const xhci_trb_t *ev)
{
	uint32_t slot = (ev->control >> 24) & 0xFF;
	uint32_t dci  = (ev->control >> 16) & 0x1F;
	uint8_t  cc   = (uint8_t)TRB_CC(ev->status);

	// A short Data stage reports its actual length, but the request is complete
	// at the Status stage. Match TRBs too, so a late timed-out request cannot
	// satisfy the next wait on this slot. HID endpoints may complete meanwhile.
	if (dci == 1 && slot == s_hc->control_slot) {
		if (ev->param == s_hc->control_data_trb && cc == TRB_CC_SHORT_PACKET) {
			uint32_t residual = ev->status & 0xffffff;
			if (residual <= s_hc->xfer_actual)
				s_hc->xfer_actual -= residual;
			else
				s_hc->xfer_actual = 0;
			return;
		}
		if (ev->param != s_hc->control_status_trb &&
		    ev->param != s_hc->control_data_trb &&
		    ev->param != s_hc->control_setup_trb) {
			if (s_hc->bt_probe && slot == s_hc->bt_probe->device->slot)
				s_hc->bt_probe->ep0_unmatched++;
			return;
		}
		s_hc->xfer_cc = cc;
		s_hc->xfer_done = true;
		return;
	}
	if (xhci_bt_transfer(ev, slot, dci, cc))
		return;

	xhci_device_t *dev = NULL;
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
		xhci_hid_lost(dev);
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
	if (residual <= dev->report_bytes) {
		uint32_t actual = dev->report_bytes - residual;
		const uint8_t *report = dev->reports + buf_index * HID_REPORT_BYTES;
		if (dev->kind == HID_KEYBOARD && actual >= 8)
			hid_keyboard_report(&dev->kbd, report);
		else if (dev->kind == HID_MOUSE)
			hid_process_mouse_report(dev, report, actual);
	}
	xhci_arm_report_trb(dev, buf_index);   // hand the same buffer back
}

// ── Device enumeration (boot-time) ──────────────────────────────────────────

// Write one endpoint/slot context field set. `ctx` points at the START of
// the input context; index 0 = input control, 1 = slot, 2 = EP0 (DCI 1),
// DCI n lives at index n+1. Context size honors HCCPARAMS1.CSZ.
static uint32_t *ictx(xhci_device_t *dev, uint32_t index)
{
	return (uint32_t *)(dev->input_ctx + index * s_hc->ctx_size);
}

// Disable Slot stops endpoint DMA, including queued receives, before storage
// is released. Callers retain their additional DMA allocations on failure.
static bool xhci_release_probe(xhci_device_t *dev)
{
	if (xhci_run_command(0, 0, TRB_TYPE(TRB_DISABLE_SLOT) |
	                     (dev->slot << 24)) != TRB_CC_SUCCESS)
		return false;
	s_hc->dcbaa[dev->slot] = 0;
	kfree(dev->input_ctx);
	kfree(dev->ep0.trb);
	kfree(dev->dev_ctx);
	return true;
}

enum { BT_WAIT_COMMAND, BT_WAIT_DOWNLOAD, BT_WAIT_BOOT };

static bool xhci_bt_retry_receive(xhci_bt_probe_t *p)
{
	for (unsigned i = 0; i < 2; i++) {
		xhci_bt_endpoint_t *rx = &p->rx[i];
		if (!rx->retry_pending) continue;
		rx->retry_pending = false;
		rx->retried = true;
		uint32_t dci = (rx->endpoint.address & 15) * 2 + 1;
		// TSP resumes the halted TD with its transfer state intact. Keep the
		// pending TRB and buffer; neither a new receive nor Set Dequeue is needed.
		uint8_t cc = xhci_run_command(0, 0, TRB_TYPE(TRB_RESET_ENDPOINT) |
		    TRB_RESET_TSP | (dci << 16) | (p->device->slot << 24));
		if (cc != TRB_CC_SUCCESS) {
			printd(DEBUG_USB, "xhci: AX210 receive endpoint %02x soft reset failed cc=%u\n",
			       rx->endpoint.address, cc);
			p->failed = true;
		} else if (!p->failed && p->listening) {
			mmio_w32((uint8_t *)s_hc->db, 4 * p->device->slot, dci);
			printd(DEBUG_USB, "xhci: AX210 receive endpoint %02x resumed after soft reset\n",
			       rx->endpoint.address);
		}
		return true;
	}
	return false;
}

static bool xhci_bt_wait(xhci_bt_probe_t *p, unsigned kind, unsigned timeout_ms)
{
	uint64_t start = kTicksSinceStart;
	uint64_t ticks = ((uint64_t)timeout_ms * TICKS_PER_SECOND + 999) / 1000;
	for (;;) {
		xhci_drain_events();
		if (p->failed || p->events.malformed || p->events.download_failed ||
		    (p->events.command_done && p->events.status)) {
			const bt_intel_events_t *e = &p->events;
			printd(DEBUG_USB, "xhci: AX210 wait failed kind=%u opcode=%04x status=%02x malformed=%u secure_failed=%u probe_failed=%u packets=%u partial=%u/%u\n",
			       kind, e->opcode, e->status, e->malformed, e->download_failed,
			       p->failed, p->packets, p->rx[0].stream.used, p->rx[1].stream.used);
			if (e->failure_bytes) {
				const uint8_t *b = e->failure_event;
				printd(DEBUG_USB, "xhci: AX210 rejected event len=%u prefix=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				       e->failure_bytes, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
				       b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
			}
			return false;
		}
		if (xhci_bt_retry_receive(p)) continue;
		bool done = kind == BT_WAIT_BOOT ? p->events.booted :
		            kind == BT_WAIT_DOWNLOAD ? p->events.download_done :
		            p->events.command_done && p->events.credits;
		if (done && p->tx_done) return true;
		if (kTicksSinceStart - start >= ticks || kTicksSinceStart >= p->deadline) {
			printd(DEBUG_USB, "xhci: AX210 wait timeout kind=%u opcode=%04x USB_done=%u HCI_done=%u packets=%u\n",
			       kind, p->events.opcode, p->tx_done, p->events.command_done, p->packets);
			return false;
		}
		// Boot-time polling: waiting a full 10ms timer tick for each of the
		// thousands of small fragments would dominate the USB transfer time.
		__asm__ volatile("pause");
	}
}

static bool xhci_bt_command(xhci_bt_probe_t *p, uint16_t opcode,
                             const uint8_t *params, uint8_t length, bool bulk)
{
	if (p->failed || kTicksSinceStart >= p->deadline) return false;
	uint8_t command[258] = {opcode & 255, opcode >> 8, length};
	if (length) memcpy(command + 3, params, length);
	p->events.opcode = opcode;
	p->events.command_done = false;
	p->events.status = 0;
	p->packets = 0;
	p->tx_done = true;
	if (opcode == BT_INTEL_READ_VERSION) p->events.version = (bt_intel_reply_t){0};
	if (opcode == 0xfc01) {
		p->events.watch_boot = true;
		p->events.booted = false;
	}
	if (bulk) {
		p->tx_bytes = 3u + length;
		memcpy(p->tx.buffer, command, p->tx_bytes);
		p->tx_done = false;
		p->tx.pending_trb = ring_push(&p->tx.ring, virt_to_phys(p->tx.buffer),
		                             p->tx_bytes, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
		mmio_w32((uint8_t *)s_hc->db, 4 * p->device->slot,
		         (p->tx.endpoint.address & 15) * 2);
	} else if (!xhci_control_request(p->device, 0x20, 0, 0, 0, command, 3u + length)) {
		return false;
	}
	// Intel Reset does not send Command Complete in bootloader mode. The
	// operational firmware's vendor boot notification is the readiness signal.
	bool ok = xhci_bt_wait(p, opcode == 0xfc01 ? BT_WAIT_BOOT : BT_WAIT_COMMAND, 2000);
	if (!ok)
		printd(DEBUG_USB, "xhci: AX210 command %04x failed status=%02x malformed=%u receive_error=%u secure_failed=%u\n",
		       opcode, p->events.status, p->events.malformed, p->failed, p->events.download_failed);
	return ok;
}

static bool xhci_bt_fragment(xhci_bt_probe_t *p, uint8_t type,
                              const uint8_t *data, size_t bytes)
{
	while (bytes) {
		uint8_t params[253] = {type};
		size_t n = bytes > 252 ? 252 : bytes;
		memcpy(params + 1, data, n);
		if (!xhci_bt_command(p, 0xfc09, params, n + 1, true)) return false;
		data += n;
		bytes -= n;
	}
	return true;
}

static bool xhci_ax210_load(xhci_bt_probe_t *p)
{
	const bt_intel_reply_t *v = &p->events.version;
	bt_intel_sfi_t sfi;
	size_t size = (uintptr_t)bt_ax210_sfi_end - (uintptr_t)bt_ax210_sfi;
	size_t ddc_size = (uintptr_t)bt_ax210_ddc_end - (uintptr_t)bt_ax210_ddc;
	if (!bt_intel_is_ax210_bootloader(v)) {
		printd(DEBUG_USB, "xhci: AX210 firmware refused: fields=%08x CNVi=%08x TOP=%08x/%08x SBE=%u limited=%u\n",
		       v->fields, v->cnvi, v->cnvi_top, v->cnvr_top, v->sbe, v->limited);
		return false;
	}
	if (!bt_intel_sfi_validate(bt_ax210_sfi, size, &sfi) ||
	    !bt_intel_ddc_valid(bt_ax210_ddc, ddc_size)) {
		printd(DEBUG_USB, "xhci: AX210 firmware container validation failed\n");
		return false;
	}
	uint8_t sbe = v->sbe;
	printd(DEBUG_USB, "xhci: AX210 loading intel/ibt-0041-0041.sfi bytes=%u SBE=%u boot=%08x version=%u-%u.%u\n",
	       (unsigned)size, sbe, sfi.boot_address, sfi.build, sfi.week, sfi.year);
	printf("USB Bluetooth: Intel AX210 loading firmware (%u bytes)\n", (unsigned)size);
	p->events.watch_download = true;
	// The container holds both authentication formats; the bootloader's SBE
	// field selects the header, public key and signature to send unchanged.
	bool ok;
	if (sbe == 0) {
		ok = xhci_bt_fragment(p, 0, sfi.data, 128) &&
		     xhci_bt_fragment(p, 3, sfi.data + 128, 256) &&
		     xhci_bt_fragment(p, 2, sfi.data + 388, 256);
	} else {
		ok = xhci_bt_fragment(p, 0, sfi.data + 644, 128) &&
		     xhci_bt_fragment(p, 3, sfi.data + 772, 96) &&
		     xhci_bt_fragment(p, 2, sfi.data + 868, 96);
	}
	if (!ok) return false;
	unsigned groups = 0;
	for (size_t off = BT_INTEL_SFI_PAYLOAD; off < size;) {
		size_t bytes = bt_intel_sfi_group(&sfi, off);
		if (!bytes || !xhci_bt_fragment(p, 1, sfi.data + off, bytes)) {
			printd(DEBUG_USB, "xhci: AX210 firmware transfer stopped at byte %u\n", (unsigned)off);
			return false;
		}
		off += bytes;
		if (++groups % 512 == 0)
			printd(DEBUG_USB, "xhci: AX210 firmware transferred %u/%u bytes\n", (unsigned)off, (unsigned)size);
	}
	printd(DEBUG_USB, "xhci: AX210 firmware payload sent: %u/%u bytes; waiting for secure result\n",
	       (unsigned)size, (unsigned)size);
	if (!xhci_bt_wait(p, BT_WAIT_DOWNLOAD, 5000)) return false;
	printd(DEBUG_USB, "xhci: AX210 secure download complete; starting firmware\n");
	p->events.watch_download = false;
	uint8_t reset[8] = {0, 1, 0, 1, sfi.boot_address & 255,
	                   (sfi.boot_address >> 8) & 255, (sfi.boot_address >> 16) & 255,
	                   sfi.boot_address >> 24};
	if (!xhci_bt_command(p, 0xfc01, reset, sizeof(reset), false)) return false;
	p->events.watch_boot = false;
	uint8_t tlv = 0xff;
	if (!xhci_bt_command(p, BT_INTEL_READ_VERSION, &tlv, 1, false) ||
	    p->events.version.result != BT_INTEL_VERSION || p->events.version.image != 3) {
		printd(DEBUG_USB, "xhci: AX210 firmware did not enter operational mode\n");
		return false;
	}
	printd(DEBUG_USB, "xhci: AX210 firmware started: image=%02x build=%u SHA1=%08x\n",
	       p->events.version.image, p->events.version.build, p->events.version.sha1);
	for (size_t off = 0; off < ddc_size;) {
		uint8_t length = bt_ax210_ddc[off] + 1;
		if (!xhci_bt_command(p, 0xfc8b, bt_ax210_ddc + off, length, false)) return false;
		off += length;
	}
	printd(DEBUG_USB, "xhci: AX210 DDC configuration applied\n");
	printf("USB Bluetooth: Intel AX210 firmware loaded and operational\n");
	return true;
}

static void xhci_ax210_bringup(xhci_device_t *dev, const uint8_t *cfg, uint16_t bytes)
{
	// The discovery interface owns one adapter. Do not replace its live probe
	// if another matching USB device is enumerated on this controller.
	if (s_bluetooth_hc) {
		xhci_release_probe(dev);
		return;
	}
	bt_intel_usb_t usb;
	xhci_bt_probe_t probe = {.device = dev, .tx_done = true,
	    .deadline = kTicksSinceStart + 120u * TICKS_PER_SECOND};
	bool queried = false, attempted = false;
	const char *stage = "USB layout";
	// The P5's AX210 is full-speed. Reject other layouts before configuring
	// endpoints rather than applying full-speed packet and interval rules to them.
	if (dev->speed != 1 || !bt_intel_find_usb(cfg, bytes, &usb)) {
		printd(DEBUG_USB, "xhci: AX210 Read Version skipped: unsupported USB layout/speed\n");
		goto cleanup;
	}
	probe.rx[0].endpoint = usb.interrupt_in;
	probe.rx[1].endpoint = usb.bulk_in;
	probe.tx.endpoint = usb.bulk_out;
	stage = "endpoint buffer allocation";
	for (unsigned i = 0; i < 3; i++) {
		xhci_bt_endpoint_t *endpoint = i < 2 ? &probe.rx[i] : &probe.tx;
		if (!ring_init(&endpoint->ring)) goto cleanup;
		// The transmit endpoint's page is followed by its send slots.
		endpoint->buffer = kmalloc_aligned(i < 2 ? PAGE_SIZE : BT_TX_BYTES);
		if (!endpoint->buffer) goto cleanup;
	}
	stage = "SET_CONFIGURATION";
	if (!xhci_control_request(dev, 0x00, 9 /*SET_CONFIGURATION*/,
	                          usb.configuration, 0, NULL, 0))
		goto cleanup;

	memset(dev->input_ctx, 0, PAGE_SIZE);
	uint32_t additions = 1, last_dci = 1;
	for (unsigned i = 0; i < 3; i++) {
		xhci_bt_endpoint_t *endpoint = i < 2 ? &probe.rx[i] : &probe.tx;
		uint32_t dci = (endpoint->endpoint.address & 15) * 2 + (i < 2);
		additions |= 1u << dci;
		if (dci > last_dci) last_dci = dci;
		uint32_t *ep = ictx(dev, dci + 1);
		uint32_t packet = endpoint->endpoint.packet_bytes;
		if (i == 0) {
			uint32_t interval = 3;
			while (interval < 10 && (1u << (interval - 3)) < endpoint->endpoint.interval)
				interval++;
			ep[0] = interval << 16;
			ep[4] = (packet << 16) | packet; // Max ESIT payload / average TRB length.
		} else {
			ep[4] = packet;
		}
		ep[1] = ((i == 0 ? 7u : i == 1 ? 6u : 2u) << 3) | (3u << 1) | (packet << 16);
		ep[2] = (uint32_t)(endpoint->ring.phys | 1);
		ep[3] = (uint32_t)(endpoint->ring.phys >> 32);
	}
	ictx(dev, 0)[1] = additions;
	ictx(dev, 1)[0] = (last_dci << 27) | (dev->speed << 20);
	ictx(dev, 1)[1] = dev->port << 16;
	stage = "Configure Endpoint";
	if (xhci_run_command(dev->input_ctx_phys, 0,
	        TRB_TYPE(TRB_CONFIG_ENDPOINT) | (dev->slot << 24)) != TRB_CC_SUCCESS)
		goto cleanup;

	s_hc->bt_probe = &probe;
	probe.listening = true;
	for (unsigned i = 0; i < 2; i++) xhci_bt_arm(&probe, &probe.rx[i]);
	// USB carries the HCI command header directly, without a UART packet-type
	// byte. Intel's ff parameter requests the TLV form of Read Version.
	uint8_t tlv = 0xff;
	printd(DEBUG_USB, "xhci: AX210 Read Version sending fc05(ff), interrupt %02x / bulk %02x\n",
	       usb.interrupt_in.address, usb.bulk_in.address);
	attempted = true;
	queried = xhci_bt_command(&probe, BT_INTEL_READ_VERSION, &tlv, 1, false);
	if (queried && !probe.failed && probe.events.version.result == BT_INTEL_VERSION) {
		const bt_intel_reply_t *v = &probe.events.version;
		const char *state = v->image == 1 ? "bootloader (firmware required)" :
		                    v->image == 3 ? "operational firmware" : "unknown image type";
		printd(DEBUG_USB, "xhci: AX210 Read Version: %s, image=%02x, USB packets=%u\n",
		       state, v->image, probe.packets);
		printf("USB Bluetooth: Intel AX210 %s\n", state);
		// In the log as well as on the glass: whether the firmware was found
		// already running (a warm boot) is the first fact a start-up
		// failure is read against.
		printd(DEBUG_BOOT, "bluetooth: Intel AX210 found %s (image %u)\n", state, v->image);
		if (v->fields & BT_INTEL_CNVI)
			printd(DEBUG_USB, "xhci: AX210 CNVi Bluetooth %08x\n", v->cnvi);
		if (v->fields & BT_INTEL_CNVR)
			printd(DEBUG_USB, "xhci: AX210 CNVr Bluetooth %08x\n", v->cnvr);
		if (v->fields & BT_INTEL_TIMESTAMP)
			printd(DEBUG_USB, "xhci: AX210 firmware timestamp year=%u week=%u\n",
			       2000u + (v->timestamp >> 8), v->timestamp & 255);
		if (v->fields & BT_INTEL_BUILD)
			printd(DEBUG_USB, "xhci: AX210 firmware build %u\n", v->build);
		if (v->fields & BT_INTEL_SHA1)
			printd(DEBUG_USB, "xhci: AX210 firmware SHA1 %08x\n", v->sha1);
		if (v->image == 1 && !xhci_ax210_load(&probe)) {
			printd(DEBUG_USB, "xhci: AX210 firmware load failed (no retry this boot)\n");
			printf("USB Bluetooth: Intel AX210 firmware load failed (see USB log)\n");
			goto cleanup;
		}
		if (probe.events.version.image == 3 && !probe.failed) {
			s_hc->bluetooth_device = *dev;
			s_hc->bluetooth = probe;
			s_hc->bluetooth.device = &s_hc->bluetooth_device;
			s_hc->bluetooth.runtime = true;
			// From here a change on the adapter's port is news: clear what
			// enumeration left standing so the next one raises an event.
			xhci_port_ack_changes(s_hc->bluetooth.device->port);
			bt_host_init(&s_hc->bluetooth.host, kTicksSinceStart * 1000 / TICKS_PER_SECOND);
			for (unsigned i = 0; i < BT_HOST_PEERS; i++) {
				s_hc->bluetooth.keyboards[i].name = "bluetooth";
				s_hc->bluetooth.keyboards[i].debug = DEBUG_USB;
			}
			s_hc->bt_probe = &s_hc->bluetooth;
			s_bluetooth_hc = s_hc;
			printd(DEBUG_USB, "xhci: AX210 slot %u retained for runtime discovery\n", dev->slot);
			printf("USB Bluetooth: discovery ready at /sys/bluetooth/scan\n");
			return;
		}
	} else {
		printd(DEBUG_USB, "xhci: AX210 Read Version incomplete: query_ok=%u receive_error=%u result=%u status=%02x packets=%u partial=%u/%u\n",
		       queried, probe.failed, probe.events.version.result, probe.events.version.status,
		       probe.packets, probe.rx[0].stream.used, probe.rx[1].stream.used);
		printf("USB Bluetooth: Intel AX210 Read Version incomplete (see USB log)\n");
	}
cleanup:
	if (!attempted) {
		printd(DEBUG_USB, "xhci: AX210 Read Version setup failed at %s\n", stage);
		printf("USB Bluetooth: Intel AX210 probe stopped at %s\n", stage);
	}
	probe.listening = false;
	// An idle receive is still owned by the HC. Disable the slot before freeing
	// its buffers, and remove the stack pointer even when disabling times out.
	bool released = xhci_release_probe(dev);
	s_hc->bt_probe = NULL;
	printd(DEBUG_USB, "xhci: AX210 probe slot %u %s\n", dev->slot,
	       released ? "released" : "retained after Disable Slot failure");
	if (released) {
		for (unsigned i = 0; i < 3; i++) {
			xhci_bt_endpoint_t *endpoint = i < 2 ? &probe.rx[i] : &probe.tx;
			if (endpoint->buffer) kfree(endpoint->buffer);
			if (endpoint->ring.trb) kfree(endpoint->ring.trb);
		}
	}
}

// The runtime command's bytes are at offset 512 of the transmit storage's
// first page; ACL data is in the send slots after that page (BT_TX_SLOTS).
// Runtime HCI submits after USB Status and the preceding HCI reply agree;
// it never waits or allocates: it runs inside kusb's pass, against that
// pass's deadline.
static void xhci_bt_runtime_send(void *context, uint16_t opcode,
                              const uint8_t *params, uint8_t length)
{
	xhci_bt_probe_t *p = context;
	uint8_t *command = p->tx.buffer + 512;
	crypto_wipe(command, 258);
	command[0] = opcode & 255; command[1] = opcode >> 8; command[2] = length;
	if (length) memcpy(command + 3, params, length);
	uint16_t bytes = 3u + length;
	s_hc->control_slot = p->device->slot;
	s_hc->xfer_done = false;
	s_hc->xfer_cc = 0;
	s_hc->xfer_actual = bytes;
	s_hc->control_setup_trb = ring_push(&p->device->ep0,
	    0x20ull | ((uint64_t)bytes << 48), 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (2u << 16));
	s_hc->control_data_trb = ring_push(&p->device->ep0, virt_to_phys(command), bytes,
	    TRB_TYPE(TRB_DATA) | TRB_ISP);
	s_hc->control_status_trb = ring_push(&p->device->ep0, 0, 0,
	    TRB_TYPE(TRB_STATUS) | (1u << 16) | TRB_IOC);
	mmio_w32((uint8_t *)s_hc->db, 4 * p->device->slot, 1);
	printd(DEBUG_USB | DEBUG_DETAILED, "xhci: AX210 runtime command %04x\n", opcode);
}

static void xhci_bt_acl_send(void *context, const uint8_t *packet, size_t bytes)
{
	xhci_bt_probe_t *p = context;
	// A packet no slot can hold, or no slot to hold it: the sender was told
	// neither could happen (it asks before it sends), so its state and the
	// controller's no longer agree. Never truncated, never written past a
	// slot: the transport fails, and says why.
	if (bytes > BT_TX_SLOT_BYTES || p->tx_slots == BT_TX_SLOTS) {
		if (!p->failed)
			printd(DEBUG_EXCEPTIONS, "bluetooth: an ACL packet of %lu bytes could not be sent (%u of %u send slots in flight)\n",
			       (unsigned long)bytes, p->tx_slots, BT_TX_SLOTS);
		p->failed = true;
		return;
	}
	unsigned slot = (p->tx_oldest + p->tx_slots) % BT_TX_SLOTS;
	uint8_t *buffer = xhci_bt_tx_slot(p, slot);
	memcpy(buffer, packet, bytes);
	p->tx_bytes = bytes;
	p->tx_done = false;
	p->tx.pending_trb = ring_push(&p->tx.ring, virt_to_phys(buffer),
	                             bytes, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
	// Its own five seconds: one deadline pushed back by each send would
	// let the oldest wait for as long as anything was being sent.
	p->tx_slot[slot] = (xhci_bt_tx_slot_t){ .trb = p->tx.pending_trb,
	                                        .deadline = kTicksSinceStart + 5 * TICKS_PER_SECOND };
	p->tx_slots++;
	mmio_w32((uint8_t *)s_hc->db, 4 * p->device->slot, (p->tx.endpoint.address & 15) * 2);
}

// Bluetooth's troubles, in the SYSTEM LOG. The status files say what is
// wrong now; this is what lets the log say that something went wrong, and
// when, to somebody who was not looking. The state machines are pure code
// with no kernel headers, so they cannot log; this watches them for a change
// and says each one once, on the bit that is on by default: these are
// faults, not narration.
#define XHCI_BT_RESYNC_LINES 8

// Where the adapter stands on USB, as two log lines: what a failed start-up
// round needs beside its own line, because "no reply" has several causes
// that look alike from the HCI side (the adapter left the bus, an endpoint
// halted, the receive was never armed) and they are told apart here. The
// slot and endpoint states are the controller's own, read from the device
// context it maintains.
static void xhci_bt_log_usb_state(const xhci_bt_probe_t *p)
{
	const xhci_device_t *d = p->device;
	uint32_t sc = mmio_r32(s_hc->op, XHCI_OP_PORTSC(d->port));
	const uint32_t *slot_ctx = (const uint32_t *)d->dev_ctx;
	printd(DEBUG_EXCEPTIONS, "bluetooth: adapter on USB: port %u PORTSC %08x (%s, %s, link state %u), %u port change(s), last PORTSC %08x; slot %u state %u address %u\n",
	       d->port, sc, (sc & PORTSC_CCS) ? "connected" : "disconnected",
	       (sc & PORTSC_PED) ? "enabled" : "disabled", (sc >> 5) & 0xF,
	       p->port_changes, p->port_last_sc, d->slot, slot_ctx[3] >> 27, slot_ctx[3] & 0xFF);
	unsigned dci[3] = { 1, (p->rx[0].endpoint.address & 15u) * 2 + 1, (p->rx[1].endpoint.address & 15u) * 2 + 1 };
	unsigned state[3];
	for (unsigned i = 0; i < 3; i++)
		state[i] = *(const uint32_t *)(d->dev_ctx + dci[i] * s_hc->ctx_size) & 7;
	printd(DEBUG_EXCEPTIONS, "bluetooth: adapter endpoints (state 1=running 2=halted 3=stopped 4=error): control state %u, current request %s cc %u, %u unmatched; events state %u, %s, %u taken, last cc %u, %u unmatched; data in state %u, %s, %u taken, last cc %u, %u unmatched; data out %u of %u send slots in flight, %u unmatched; listening %s, transport failed %s\n",
	       state[0], s_hc->control_slot != d->slot ? "not ours" : s_hc->xfer_done ? "done" : "in flight",
	       s_hc->xfer_cc, p->ep0_unmatched,
	       state[1], p->rx[0].rearm_pending ? "waiting to be armed" : "armed", p->rx[0].completions, p->rx[0].last_cc, p->rx[0].unmatched,
	       state[2], p->rx[1].rearm_pending ? "waiting to be armed" : "armed", p->rx[1].completions, p->rx[1].last_cc, p->rx[1].unmatched,
	       p->tx_slots, BT_TX_SLOTS, p->tx.unmatched,
	       p->listening ? "yes" : "no", p->failed ? "yes" : "no");
	// Oldest first; a row of zeros is a slot no event has filled.
	const uint8_t (*e)[6] = p->recent_event;
	unsigned o = p->recent_events % 4;
	printd(DEBUG_EXCEPTIONS, "bluetooth: %u HCI event(s) since start; heads of the last four, oldest first: %02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x\n",
	       p->recent_events,
	       e[o][0], e[o][1], e[o][2], e[o][3], e[o][4], e[o][5],
	       e[(o+1)%4][0], e[(o+1)%4][1], e[(o+1)%4][2], e[(o+1)%4][3], e[(o+1)%4][4], e[(o+1)%4][5],
	       e[(o+2)%4][0], e[(o+2)%4][1], e[(o+2)%4][2], e[(o+2)%4][3], e[(o+2)%4][4], e[(o+2)%4][5],
	       e[(o+3)%4][0], e[(o+3)%4][1], e[(o+3)%4][2], e[(o+3)%4][3], e[(o+3)%4][4], e[(o+3)%4][5]);
}

static void xhci_bt_log_trouble(xhci_bt_probe_t *p)
{
	const bt_host_t *h = &p->host;
	if (h->init_failed_rounds != p->logged.rounds) {
		p->logged.rounds = h->init_failed_rounds;
		xhci_bt_log_usb_state(p);
		printd(DEBUG_EXCEPTIONS, "bluetooth: start-up round %u of %u failed: %s (opcode %04x status %02x)%s; %s\n",
		       h->init_failed_rounds, BT_HOST_INIT_ROUNDS, h->round_error ? h->round_error : "no error recorded",
		       h->round_opcode, h->round_status, h->round_timeout ? h->round_timeout : "",
		       h->init_failed_rounds < BT_HOST_INIT_ROUNDS ? "trying again" : "giving up");
	}
	if (h->failed && !p->logged.failed) {
		p->logged.failed = true;
		printd(DEBUG_EXCEPTIONS, "bluetooth: the controller has failed (%s); Bluetooth is off until a reboot\n",
		       h->scan.error ? h->scan.error : h->round_error ? h->round_error : "see /sys/bluetooth/scan");
		// On the glass too: it happens seconds after the boot messages end,
		// with nothing else on screen to show for it.
		printf("Bluetooth did not start (%s); the system log has the details\n",
		       h->round_error ? h->round_error : "controller failed");
	}
	// A controller that keeps handing over broken framing would otherwise
	// write a line a pass: the first few are said, the rest only counted.
	uint32_t resyncs = p->rx[0].stream.resyncs;
	if (resyncs != p->logged.event_resyncs) {
		if (resyncs <= XHCI_BT_RESYNC_LINES)
			printd(DEBUG_EXCEPTIONS, "bluetooth: the event stream was out of step with its transfers and was resynchronised (%u so far%s)\n",
			       resyncs, resyncs == XHCI_BT_RESYNC_LINES ? "; further ones are counted in /sys/bluetooth/scan, not logged" : "");
		p->logged.event_resyncs = resyncs;
	}
	if (h->wire_resyncs != p->logged.acl_resyncs) {
		if (h->wire_resyncs <= XHCI_BT_RESYNC_LINES)
			printd(DEBUG_EXCEPTIONS, "bluetooth: the ACL data stream was out of step with its transfers and was resynchronised (%u so far%s)\n",
			       h->wire_resyncs, h->wire_resyncs == XHCI_BT_RESYNC_LINES ? "; further ones are counted in /sys/bluetooth/connection, not logged" : "");
		p->logged.acl_resyncs = h->wire_resyncs;
	}
	for (unsigned i = 0; i < BT_HOST_PEERS; i++) {
		const bt_le_t *s = &h->peers[i].le;
		const bt_manager_t *m = &h->peers[i].manager;
		// A slot that stopped or failed with an error. Said when the error
		// CHANGES, so a slot failing the same way at every attempt is one
		// line, not one an attempt. Input flowing again clears the memory.
		// A retryable stop that never had a link and carries no failing
		// status is a device that is not there yet (off, or not yet
		// advertising after a boot): ordinary, and not said.
		bool absent = s->phase == BT_LE_STOPPED && s->retryable &&
		              s->disconnect_reason == 0 && s->error_status == 0;
		if (bt_le_input_active(s)) {
			p->logged.error[i] = NULL;
		} else if ((s->phase == BT_LE_STOPPED || s->phase == BT_LE_FAILED) && s->error && !absent &&
		           s->error != p->logged.error[i]) {
			p->logged.error[i] = s->error;
			printd(DEBUG_EXCEPTIONS, "bluetooth: slot %u stopped: %s (command %04x status %02x, disconnect reason %02x); %s\n",
			       i, s->error, s->error_opcode, s->error_status, s->disconnect_reason,
			       s->phase == BT_LE_FAILED ? "the controller has failed" :
			       s->retryable ? "it will be retried" : "it waits for the user");
		}
		if (m->blocked != p->logged.blocked[i]) {
			p->logged.blocked[i] = m->blocked;
			if (m->blocked)
				printd(DEBUG_EXCEPTIONS, "bluetooth: slot %u's automatic connection has stopped and waits for the user (write reconnect to /sys/bluetooth/connection)\n", i);
		}
	}
}

static void xhci_bt_runtime_poll(void)
{
	xhci_bt_probe_t *p = s_hc->bt_probe;
	if (!p || !p->runtime) return;
	for (unsigned i = 0; i < 2; i++) {
		if (p->listening && !p->failed && p->rx[i].rearm_pending) {
			p->rx[i].rearm_pending = false;
			xhci_bt_arm(p, &p->rx[i]);
		}
	}
    bool usb_failed = p->failed ||
        (p->tx_slots && kTicksSinceStart >= p->tx_slot[p->tx_oldest].deadline) ||
        (s_hc->control_slot == p->device->slot && s_hc->xfer_done &&
        (s_hc->xfer_cc != TRB_CC_SUCCESS ||
        (p->host.owner && s_hc->xfer_actual != 3u + p->tx.buffer[514])));
    bool usb_done = s_hc->control_slot != p->device->slot || s_hc->xfer_done;
    if (s_bt_quiesce != BT_QUIESCE_NONE) {
        // Going down: the host's clockwork has stopped for good. The reset
        // goes out once the control endpoint is free of the host's last
        // request; HCI lets a reset be sent whatever commands are
        // outstanding.
        if (s_bt_quiesce == BT_QUIESCE_ASKED && usb_done && !p->failed) {
            xhci_bt_runtime_send(p, 0x0c03, NULL, 0);
            s_bt_quiesce = BT_QUIESCE_SENT;
        }
        return;
    }
    bt_scan_t *scan = &p->host.scan;
    bool scanning = !bt_scan_quiescent(scan) && scan->phase != BT_SCAN_FAILED;
    if (scanning && (!p->scan_history.valid || p->scan_history.finished)) {
        p->scan_history.started_at = kSystemCurrentTime;
        p->scan_history.started_ms = kTicksSinceStart * 1000 / TICKS_PER_SECOND;
        p->scan_history.valid = true;
        p->scan_history.finished = p->scan_history.failed = false;
        p->scan_history.le_only = scan->le_only;
        xhci_timeline_add(XHCI_TL_SCAN_STARTED, 0, 0, 0);
    }
    bt_le_phase_t before[BT_HOST_PEERS];
    for (unsigned i = 0; i < BT_HOST_PEERS; i++) before[i] = p->host.peers[i].le.phase;
    bt_host_tick(&p->host, kTicksSinceStart * 1000 / TICKS_PER_SECOND,
                 usb_done, p->tx_done, usb_failed, xhci_bt_runtime_send,
                 xhci_bt_acl_send, xhci_bt_input_report, p);
    if (p->scan_history.valid && !p->scan_history.finished &&
        (bt_scan_quiescent(scan) || scan->phase == BT_SCAN_FAILED)) {
        p->scan_history.finished_at = kSystemCurrentTime;
        p->scan_history.finished_ms = kTicksSinceStart * 1000 / TICKS_PER_SECOND;
        p->scan_history.finished = true;
        p->scan_history.failed = scan->phase != BT_SCAN_DONE;
        xhci_timeline_add(XHCI_TL_SCAN_FINISHED, 0, p->scan_history.failed,
                          scan->count > 255 ? 255 : scan->count);
    }
    for (unsigned i = 0; i < BT_HOST_PEERS; i++) {
        const bt_le_t *s = &p->host.peers[i].le;
        if(s->mouse && bt_le_input_active(s)) xhci_bt_pointer_register(p,i);
        else if(p->pointers[i].settings_slot || p->pointers[i].settings_key[0]) input_pointer_unregister(&p->pointers[i]);
        // Against the phase the timeline last wrote, not this pass's
        // `before`: a phase also moves while events are drained, outside
        // the tick above, and a line must not miss an arrival at ready.
        // Phases passed through between two looks are not listed.
        if (s->phase != p->timeline_phase[i]) {
            xhci_timeline_add(XHCI_TL_LE_PHASE, i, p->timeline_phase[i], s->phase);
            p->timeline_phase[i] = (uint8_t)s->phase;
        }
        if (s->phase != before[i])
            printd(DEBUG_USB, "xhci: AX210 slot %u LE phase=%u error=%s opcode=%04x status=%02x\n",
                   i, s->phase, s->error ? s->error : "none", s->error_opcode, s->error_status);
    }
    xhci_bt_log_trouble(p);
    if (usb_done && !p->host.owner) crypto_wipe(p->tx.buffer + 512, 258);
    // A send slot is wiped as its transfer completes, and not before: one
    // still queued is the controller's to read, and zeroing it would put a
    // packet of zeroes on the air. That holds on a failed transport too,
    // whose endpoint may still be running.
    for (unsigned i = 0; i < BT_HOST_PEERS; i++)
        if (bt_le_input_active(&p->host.peers[i].le)) hid_keyboard_tick(&p->keyboards[i]);
}

bool xhci_bluetooth_scan(void)
{
    if (__sync_lock_test_and_set(&s_poll_busy, 1)) return false;
    bool ok = s_bluetooth_hc && !s_bluetooth_hc->bluetooth.failed &&
        bt_host_scan(&s_bluetooth_hc->bluetooth.host, kTicksSinceStart * 1000 / TICKS_PER_SECOND);
    xhci_serial_release();
    return ok;
}

bool xhci_bluetooth_connection(const char *data, size_t bytes)
{
    if (__sync_lock_test_and_set(&s_poll_busy, 1)) return false;
    xhci_bt_probe_t *p = s_bluetooth_hc ? &s_bluetooth_hc->bluetooth : NULL;
    bool ok = p && bt_host_command(&p->host, data, bytes,
                                   kTicksSinceStart * 1000 / TICKS_PER_SECOND);
    xhci_serial_release();
    return ok;
}

void xhci_bluetooth_start_manager(void)
{ __atomic_store_n(&s_bond_manager_ready, true, __ATOMIC_RELEASE); }

void xhci_bluetooth_maintain(void)
{
	// The maintenance worker owns disk I/O; USB keeps progressing while it is
	// outside the serializing flag. A generation prevents stale saves
	// acknowledging forget. The connection policy is not run here: it is
	// timing, and kusb runs it every pass (bt_host_tick).
	if (!__atomic_load_n(&s_bond_manager_ready, __ATOMIC_ACQUIRE)) return;
	if (__sync_lock_test_and_set(&s_bond_io_busy, 1)) return;
	if (__sync_lock_test_and_set(&s_poll_busy, 1)) {
		__sync_lock_release(&s_bond_io_busy); return;
	}
	xhci_bt_probe_t *p = s_bluetooth_hc ? &s_bluetooth_hc->bluetooth : NULL;
	uint64_t now = kTicksSinceStart * 1000 / TICKS_PER_SECOND;
	if (!p || !p->runtime) {
		xhci_serial_release(); __sync_lock_release(&s_bond_io_busy); return;
	}
    unsigned slot;
    for (slot = 0; slot < BT_HOST_PEERS; slot++) {
        bt_manager_t *candidate = &p->host.peers[slot].manager;
        if (!candidate->loaded || (candidate->dirty && now >= candidate->next_save)) break;
    }
    if (slot == BT_HOST_PEERS) {
        xhci_serial_release(); __sync_lock_release(&s_bond_io_busy); return;
    }
    bt_host_peer_t *peer = &p->host.peers[slot];
    bt_manager_t *m = &peer->manager;
	bool loading = !m->loaded;
	if (!loading && (!m->dirty || now < m->next_save)) {
		xhci_serial_release(); __sync_lock_release(&s_bond_io_busy); return;
	}
	uint64_t generation = m->generation;
	bt_le_bond_t snapshot = peer->le.bond;
	bool automatic = m->automatic;
	xhci_serial_release();
	bool good = loading ? bt_bond_load_slot(slot, &snapshot, &automatic) : bt_bond_save_slot(slot, &snapshot, automatic);
	// The result must be published, so this waits for the flag where every
	// other holder gives up. It sleeps between tries: kusb may hold the flag
	// for a tick, which is too long to spin a core through.
	while (__sync_lock_test_and_set(&s_poll_busy, 1)) {
		thread_t *self = get_core_local_storage()->currentThread;
		signal_raise(SIGSLEEP, kTicksSinceStart + 1, self);
	}
	now = kTicksSinceStart * 1000 / TICKS_PER_SECOND;
	bt_manager_observe(m, &peer->le, now);
	bool loaded = false;
	if (loading) {
		m->loaded = true;
		if (good && m->generation == generation) {
			peer->le.bond = snapshot; m->automatic = automatic;
		}
		m->storage_error = good ? NULL : "saved bond unavailable or invalid";
		// The restored bond is published. Discovery for it starts on kusb's
		// next pass, which runs the connection policy.
		loaded = true;
	} else {
		if (good && m->generation == generation) m->dirty = false;
		m->storage_error = good ? NULL : "save failed; RAM bond retained";
		m->next_save = m->generation == generation ? now + 10000 : now;
	}
	xhci_serial_release();
	// Bring kusb round for the restored bond now, not at the next tick.
	// After the release: a ring while the flag is held would wake kusb only
	// to be refused.
	if (loaded)
		doorbell_ring(&kUsbDoorbell);
	crypto_wipe(&snapshot, sizeof(snapshot));
	__sync_lock_release(&s_bond_io_busy);
}

static void xhci_bt_scan_date(uint64_t epoch, char out[32])
{
	if (!epoch) { snprintf(out, 32, "clock unavailable"); return; }
	time_t stamp = (time_t)epoch;
	struct tm date = {0};
	gmtime(&stamp, &date);
	snprintf(out, 32, "%04d-%02d-%02d %02d:%02d:%02d UTC", date.tm_year + 1900,
	         date.tm_mon + 1, date.tm_mday, date.tm_hour, date.tm_min, date.tm_sec);
}

static size_t xhci_bt_scan_history(const xhci_bt_probe_t *p, char *out, size_t cap)
{
	int n;
	if (!p->scan_history.valid) n = snprintf(out, cap, "last scan: none this boot\n");
	else {
		char started[32], finished[96];
		xhci_bt_scan_date(p->scan_history.started_at, started);
		if (p->scan_history.finished) {
			char date[32];
			xhci_bt_scan_date(p->scan_history.finished_at, date);
			snprintf(finished, sizeof(finished), "%s (uptime ms: %lu)", date, p->scan_history.finished_ms);
		}
		else snprintf(finished, sizeof(finished), "in progress");
		n = snprintf(out, cap, "last scan: %s; %s\nlast scan started: %s (uptime ms: %lu)\n"
		             "last scan finished: %s\n",
		             p->scan_history.le_only ? "background LE" : "manual Classic + LE",
		             !p->scan_history.finished ? "active" : p->scan_history.failed ? "failed" : "complete",
		             started, p->scan_history.started_ms, finished);
	}
	return n < 0 || !cap ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t xhci_bluetooth_read(char *out, size_t capacity, unsigned file)
{
	if (!capacity) return 0;
	if (__sync_lock_test_and_set(&s_poll_busy, 1)) {
		int n = snprintf(out, capacity, "USB busy; retry this read\n");
		return n < 0 ? 0 : (size_t)n < capacity ? (size_t)n : capacity - 1;
	}
	size_t n;
	if (!s_bluetooth_hc) {
		int written = snprintf(out, capacity, "No operational AX210 available\n");
		n = written < 0 ? 0 : (size_t)written < capacity ? (size_t)written : capacity - 1;
	} else {
		const bt_scan_t *scan = &s_bluetooth_hc->bluetooth.host.scan;
		n = file == 2 ? bt_host_status(&s_bluetooth_hc->bluetooth.host, out, capacity) :
		    file == 1 ? bt_scan_devices(scan, out, capacity) : bt_scan_status(scan, out, capacity);

		if (file == 0 && n < capacity - 1)
			n += xhci_bt_scan_history(&s_bluetooth_hc->bluetooth, out + n, capacity - n);
		// How often the event assembler was found out of step with the
		// transfers and emptied (bt_intel.h). Not zero after a warm reboot
		// is the adapter handing over the tail of an event from before.
		if (file == 0 && n < capacity - 1) {
			int more = snprintf(out + n, capacity - n, "event framing resyncs: %u\n",
			                    s_bluetooth_hc->bluetooth.rx[0].stream.resyncs);
			if (more > 0) n += (size_t)more < capacity - n ? (size_t)more : capacity - n - 1;
		}
	}
	xhci_serial_release();
	return n;
}

// The LE phases by name, for the timeline; bt_le.h's enum in its order.
static const char *const kLePhaseNames[] = {
	"idle", "reset", "mask", "host", "events",
	"address", "resolve", "buffer", "shared_buffer", "create", "connecting",
	"passkey", "random_low", "random_high", "pair",
	"pair_wait", "confirm_low", "confirm_high", "confirm_wait",
	"random_wait", "verify_low", "verify_high", "stk",
	"encrypt", "encrypt_wait", "bond_keys", "services", "characteristics",
	"descriptors", "report_map", "report_descriptors", "report_reference",
	"protocol", "subscribe", "verify_ccc", "verify_protocol", "ready",
	"cleanup", "failed", "stopped",
};
_Static_assert(sizeof(kLePhaseNames) / sizeof(kLePhaseNames[0]) == BT_LE_STOPPED + 1,
               "kLePhaseNames must name every bt_le_phase_t");

static const char *xhci_le_phase_name(unsigned phase)
{
	return phase <= BT_LE_STOPPED ? kLePhaseNames[phase] : "?";
}

// Append to a bounded text; what does not fit is dropped and the text stays
// terminated.
static void xhci_report_addf(char *out, size_t cap, size_t *len, const char *fmt, ...)
{
	if (*len + 1 >= cap) return;
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(out + *len, cap - *len, fmt, args);
	va_end(args);
	if (n < 0) return;
	*len = (size_t)n < cap - *len ? *len + (size_t)n : cap - 1;
}

// /sys/usb: the controllers, how long an event on each may have waited,
// kusb's passes and latencies, and the Bluetooth timeline (see "What a
// pass measures about itself").
size_t xhci_usb_report(char *out, size_t cap)
{
	if (!cap) return 0;
	size_t len = 0;
	out[0] = '\0';
	if (__sync_lock_test_and_set(&s_poll_busy, 1)) {
		xhci_report_addf(out, cap, &len, "USB busy; retry this read\n");
		return len;
	}
	uint64_t per_us = kCPUCyclesPerSecond / 1000000;
	if (per_us == 0) per_us = 1;

	xhci_report_addf(out, cap, &len, "controllers: %u\n", s_controller_count);
	for (uint32_t i = 0; i < s_controller_count; i++) {
		xhci_t *hc = &s_controllers[i];
		xhci_report_addf(out, cap, &len,
		                 "controller %u: %02x:%02x.%x driven=%s ports=%u events=%lu devices:",
		                 i, hc->pci_bus, hc->pci_device, hc->pci_function,
		                 xhci_drive_text(hc->drive), hc->max_ports, hc->events_polled);
		if (hc->keyboard.present)
			xhci_report_addf(out, cap, &len, " keyboard@port%u", hc->keyboard.port);
		if (hc->mouse.present)
			xhci_report_addf(out, cap, &len, " mouse@port%u", hc->mouse.port);
		if (hc->bt_probe)
			xhci_report_addf(out, cap, &len, " bluetooth@port%u", hc->bt_probe->device->port);
		if (!hc->keyboard.present && !hc->mouse.present && !hc->bt_probe)
			xhci_report_addf(out, cap, &len, " none");
		xhci_report_addf(out, cap, &len, "\n");
		// How long an event found on this controller may have waited: from
		// the last time its ring was seen empty to the drain that found it.
		xhci_report_addf(out, cap, &len,
		                 "controller %u wait: max_us=%lu since_read_max_us=%lu to_20ms=%lu to_200ms=%lu to_2s=%lu over_2s=%lu\n",
		                 i, hc->wait.max / per_us, hc->wait.recent_max / per_us, hc->wait.within[0],
		                 hc->wait.within[1], hc->wait.within[2], hc->wait.over);
		hc->wait.recent_max = 0;
		// An interrupt-driven controller: how often it interrupted, and how
		// long from an interrupt to the start of the drain that answered it.
		if (hc->drive.driven != XHCI_DRIVEN_TICK) {
			xhci_report_addf(out, cap, &len,
			                 "controller %u interrupt: fires=%lu to_service_max_us=%lu since_read_max_us=%lu over_a_tick=%lu imod=%u\n",
			                 i, hc->irq_fires, hc->irq_service_max / per_us,
			                 hc->irq_service_recent / per_us, hc->irq_service_late,
			                 mmio_r32(hc->rt, XHCI_IR0_IMOD) & 0xFFFF);
			hc->irq_service_recent = 0;
		}
	}
	xhci_report_addf(out, cap, &len, "interrupt_spurious: %lu\n", s_irq_spurious);

	// kusb, in /sys/net/knet's words (kusb.h).
	xhci_report_addf(out, cap, &len,
	                 "kusb: %s\nwakes: %lu\nwake_cycles_max: %lu\n"
	                 "wake_latency_samples: %lu\nwake_latency_max_us: %lu\nwake_latency_avg_us: %lu\n"
	                 "wake_latency_late_1ms: %lu\nwake_latency_late_10ms: %lu\n"
	                 "response_latency_samples: %lu\nresponse_latency_max_us: %lu\n"
	                 "response_latency_late_10ms: %lu\n",
	                 kKusbTask != NULL ? "running" : "none",
	                 kKusbWakes, kKusbWakeCyclesMax,
	                 kKusbWakeLatencySamples, kKusbWakeLatencyMax / per_us,
	                 kKusbWakeLatencySamples != 0
	                     ? (kKusbWakeLatencyTotal / per_us) / kKusbWakeLatencySamples : 0,
	                 kKusbWakeLatencyLate1ms, kKusbWakeLatencyLate10ms,
	                 kKusbResponseLatencySamples, kKusbResponseLatencyMax / per_us,
	                 kKusbResponseLatencyLate10ms);
	xhci_report_addf(out, cap, &len, "bell_rings: %lu\nbell_wakes: %lu\nbell_rung_runnable: %lu\n",
	                 kUsbDoorbell.rings, kUsbDoorbell.wakes, kUsbDoorbell.rung_runnable);
	// A pass is a wake that held the serializing flag. passes_refused are
	// the wakes that did not; refusal_wait is from a refusal to the pass
	// that did the work. step_delay is from a wake's start to the Bluetooth
	// step after the drains: one tick plus one event's handling at most.
	xhci_report_addf(out, cap, &len,
	                 "polls: %lu\npolls_with_events: %lu\npasses_left_work: %lu\n"
	                 "passes_refused: %lu\nrefusal_wait_max_us: %lu\nstep_delay_max_us: %lu\n"
	                 "poll_gap_max_us: %lu\npoll_gap_since_read_max_us: %lu\n",
	                 s_polls, s_polls_with_events, s_passes_left_work,
	                 s_refusals, s_refusal_wait_max / per_us, s_step_delay_max / per_us,
	                 s_poll_gap_max / per_us, s_poll_gap_recent / per_us);
	// "Since read" starts again here: read the file before and after a
	// workload and the second reading is that workload's.
	s_poll_gap_recent = 0;
	// The gap of the passes that drained events: how often kusb comes
	// round when there is something to find. A cadence, not a wait (the
	// per-controller lines above are the wait).
	xhci_report_addf(out, cap, &len,
	                 "event_gap_max_us: %lu\nevent_gap_to_20ms: %lu\nevent_gap_to_200ms: %lu\n"
	                 "event_gap_to_2s: %lu\nevent_gap_over_2s: %lu\n",
	                 s_event_gap_max / per_us, s_event_gap_20ms, s_event_gap_200ms,
	                 s_event_gap_2s, s_event_gap_over_2s);

	// Oldest first. polls, events and gap_max_us cover the time since the
	// line above (since boot, for the first line ever written).
	uint64_t shown = s_timeline_count < XHCI_TIMELINE ? s_timeline_count : XHCI_TIMELINE;
	xhci_report_addf(out, cap, &len, "timeline: %lu of %lu lines\n", shown, s_timeline_count);
	for (uint64_t n = s_timeline_count - shown; n < s_timeline_count; n++) {
		const xhci_timeline_entry_t *e = &s_timeline[n % XHCI_TIMELINE];
		xhci_report_addf(out, cap, &len, "  ms=%lu polls=%u events=%u gap_max_us=%lu ",
		                 e->at_ms, e->polls, e->events, e->gap_max / per_us);
		if (e->kind == XHCI_TL_SCAN_STARTED)
			xhci_report_addf(out, cap, &len, "scan started\n");
		else if (e->kind == XHCI_TL_SCAN_FINISHED)
			xhci_report_addf(out, cap, &len, "scan %s found=%u\n",
			                 e->from ? "failed" : "finished", e->to);
		else
			xhci_report_addf(out, cap, &len, "slot %u %s -> %s\n", e->slot,
			                 xhci_le_phase_name(e->from), xhci_le_phase_name(e->to));
	}
	xhci_serial_release();
	return len;
}

// Validate lengths and descriptor ordering before walking untrusted USB data.
static bool xhci_valid_config(const uint8_t *cfg, uint16_t total)
{
	if (total < 9 || cfg[0] != 9 || cfg[1] != 2 || (cfg[2] | (cfg[3] << 8)) != total)
		return false;
	bool have_interface = false;
	for (uint16_t off = 9; off < total; off += cfg[off]) {
		if (total - off < 2 || cfg[off] < 2 || cfg[off] > total - off)
			return false;
		if (cfg[off + 1] == 4) {
			if (cfg[off] < 9)
				return false;
			have_interface = true;
		} else if (cfg[off + 1] == 5 && (!have_interface || cfg[off] < 7)) {
			return false;
		}
	}
	return have_interface;
}

// Describe alternate settings, query the image type and load cold firmware. A complete
// configuration can exceed the buffer used for HID interface selection.
// Success here means descriptors were read; the HCI result is reported separately.
static bool xhci_probe_ax210(xhci_device_t *dev, const uint8_t *desc)
{
	uint8_t cfg[PAGE_SIZE] = {0};
	if (!xhci_control_request(dev, 0x80, 6, 0x0200, 0, cfg, 9) || s_hc->xfer_actual != 9)
		return false;
	uint16_t total = cfg[2] | (cfg[3] << 8);
	if (cfg[0] != 9 || cfg[1] != 2 || total < 9 || total > sizeof(cfg)) {
		printd(DEBUG_USB, "xhci: AX210 invalid/oversized configuration length %u\n", total);
		return false;
	}
	if (!xhci_control_request(dev, 0x80, 6, 0x0200, 0, cfg, total) || s_hc->xfer_actual != total)
		return false;
	if (!xhci_valid_config(cfg, total))
		return false;

	printd(DEBUG_USB, "xhci: AX210 controller %u port %u slot %u speed %u VID:PID 8087:0032 revision %04x USB %04x class %02x/%02x/%02x EP0 mps %u configurations %u\n",
	       s_controller_count + 1, dev->port, dev->slot, dev->speed,
	       desc[12] | (desc[13] << 8), desc[2] | (desc[3] << 8),
	       desc[4], desc[5], desc[6], desc[7], desc[17]);
	printd(DEBUG_USB, "xhci: AX210 configuration %u bytes %u interfaces %u\n",
	       cfg[5], total, cfg[4]);
	uint8_t iface = 0, alternate = 0;
	static const char *const transfer_types[] = {
		"control", "isochronous", "bulk", "interrupt"
	};
	for (uint16_t off = 9; off < total; off += cfg[off]) {
		if (cfg[off + 1] == 4) {
			iface = cfg[off + 2];
			alternate = cfg[off + 3];
			printd(DEBUG_USB, "xhci: AX210 interface %u alternate %u class %02x/%02x/%02x endpoints %u\n",
			       iface, alternate, cfg[off + 5], cfg[off + 6], cfg[off + 7], cfg[off + 4]);
		} else if (cfg[off + 1] == 5) {
			uint16_t packet = cfg[off + 4] | (cfg[off + 5] << 8);
			printd(DEBUG_USB, "xhci: AX210 interface %u alternate %u endpoint %02x %s %s maxpacket %u interval %u\n",
			       iface, alternate, cfg[off + 2], (cfg[off + 2] & 0x80) ? "IN" : "OUT",
			       transfer_types[cfg[off + 3] & 3], packet & 0x7ff, cfg[off + 6]);
		}
	}
	printf("USB Bluetooth: Intel AX210 8087:0032 on controller %u port %u\n",
	       s_controller_count + 1, dev->port);
	xhci_ax210_bringup(dev, cfg, total);
	return true;
}

static bool xhci_probe_device(uint32_t port, uint32_t speed)
{
	xhci_device_t candidate;
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
	if (!xhci_control_request(&candidate, 0x80, 6 /*GET_DESCRIPTOR*/, 0x0100, 0, desc, 8) ||
	    s_hc->xfer_actual != 8)
		return false;
	if (desc[0] != sizeof(desc) || desc[1] != 1 ||
	    (speed == 4 ? desc[7] != 9 :
	     (desc[7] != 8 && desc[7] != 16 && desc[7] != 32 && desc[7] != 64)))
		return false;
	uint32_t real_mps0 = (speed == 4) ? (1u << desc[7]) : desc[7];
	if (real_mps0 != mps0 && real_mps0 != 0) {
		ictx(&candidate, 0)[1] = 0x2;                              // touch only EP0
		ictx(&candidate, 2)[1] = (4u << 3) | (3u << 1) | (real_mps0 << 16);
		if (xhci_run_command(candidate.input_ctx_phys, 0,
		    TRB_TYPE(TRB_EVALUATE_CONTEXT) | (slot << 24)) != TRB_CC_SUCCESS)
			return false;
	}
	if (!xhci_control_request(&candidate, 0x80, 6, 0x0100, 0, desc, sizeof(desc)) ||
	    s_hc->xfer_actual != sizeof(desc) || desc[0] != sizeof(desc) || desc[1] != 1)
		return false;
	uint16_t vendor = desc[8] | (desc[9] << 8);
	uint16_t product = desc[10] | (desc[11] << 8);
	printd(DEBUG_USB, "xhci: device controller %u port %u slot %u VID:PID %04x:%04x revision %04x\n",
	       s_controller_count + 1, port, slot, vendor, product, desc[12] | (desc[13] << 8));
	if (vendor == 0x8087 && product == 0x0032) {
		if (xhci_probe_ax210(&candidate, desc)) {
			s_ax210_described++;
		} else {
			printf("USB Bluetooth: Intel AX210 found; descriptor read failed\n");
		}
		return false;
	}

	// 5. Configuration descriptor: find a HID boot keyboard (protocol 1) or
	//    boot mouse (protocol 2) that has not already been claimed.
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
	uint16_t report_length = 0;
	for (uint16_t off = 0; off + 1 < total && cfg[off] != 0; off += cfg[off]) {
		uint8_t len = cfg[off], type = cfg[off + 1];
		if (len < 2 || len > total - off) return false;
		if (type == 4 && len >= 9) {
			// v1 binds one HID interface per physical device. Once its
			// interrupt endpoint is known, do not let a later interface on a
			// composite receiver overwrite candidate.kind while ep_addr still
			// names the first interface's endpoint.
			if (ep_addr != 0)
				break;
			matching_iface = false;
			report_length = 0;
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
		} else if (type == 0x21 && len >= 6 && matching_iface) {
            if (cfg[off + 5] > (len - 6) / 3) return false;
            for (unsigned n = 0; n < cfg[off + 5]; ++n) {
                unsigned at = off + 6 + n * 3;
                if (cfg[at] == 0x22)
                    report_length = cfg[at + 1] | (cfg[at + 2] << 8);
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
		xhci_release_probe(&candidate);
		return false;
	}
	if (ep_addr == 0) {
		printd(DEBUG_USB, "xhci: boot HID on port %u has no interrupt-IN endpoint\n", port);
		return false;
	}

    // Stop each TD within one interrupt packet: requesting more than MPS
    // can combine successive full-size reports into one completion.
    if (!ep_mps) return false;
    candidate.report_bytes = ep_mps < HID_REPORT_BYTES ? ep_mps : HID_REPORT_BYTES;

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
	printd(DEBUG_USB, "xhci: %s interface %u SET_PROTOCOL boot %s\n",
	       candidate.kind == HID_MOUSE ? "mouse" : "keyboard", (uint16_t)iface_num,
	       boot_protocol ? "accepted" : "failed");
	if (boot_protocol && candidate.kind == HID_KEYBOARD)
		xhci_control_request(&candidate, 0x21, 0x0A /*SET_IDLE*/, 0,
		                     (uint16_t)iface_num, NULL, 0);
    // Boot mode is established before probing the report descriptor. A
    // rejected descriptor leaves the working boot decoder in place.
    if (boot_protocol && candidate.kind == HID_MOUSE && report_length &&
        report_length <= HID_MOUSE_DESCRIPTOR_BYTES) {
        uint8_t report_desc[HID_MOUSE_DESCRIPTOR_BYTES] = {0};
        bool got = xhci_control_request(&candidate, 0x81, 6, 0x2200,
                                       (uint16_t)iface_num, report_desc, report_length);
        if (got && (kDebugLevel & DEBUG_USB)) {
            for (unsigned off = 0; off < report_length; off += 8) {
                uint8_t bytes[8] = {0};
                unsigned n = report_length - off;
                memcpy(bytes, report_desc + off, n < 8 ? n : 8);
                printd(DEBUG_USB, "xhci: mouse descriptor slot %u length %u offset %u: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       slot, report_length, off, bytes[0], bytes[1], bytes[2], bytes[3],
                       bytes[4], bytes[5], bytes[6], bytes[7]);
            }
        }
        if (got && hid_mouse_parse(report_desc, report_length, &candidate.mouse_layout) &&
            candidate.mouse_layout.max_bytes <= ep_mps) {
            if (xhci_control_request(&candidate, 0x21, 0x0B, 1 /*report*/,
                                     (uint16_t)iface_num, NULL, 0)) {
                candidate.mouse_report_protocol = true;
                const hid_mouse_layout_t *m = &candidate.mouse_layout;
                printd(DEBUG_USB, "xhci: mouse report protocol ID %u bytes %u X %u/%u Y %u/%u wheel %u/%u\n",
                       m->report_id, m->bytes, m->x.bit, m->x.size,
                       m->y.bit, m->y.size, m->wheel.bit, m->wheel.size);
            } else {
                // A STALL halts EP0. Reset it and skip the failed TD before
                // explicitly restoring boot mode; never guess the active mode.
                if (s_hc->xfer_cc != 6 ||
                    xhci_run_command(0, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | (1u << 16) | (slot << 24)) != TRB_CC_SUCCESS ||
                    xhci_run_command(candidate.ep0.phys + candidate.ep0.enqueue * sizeof(xhci_trb_t) + candidate.ep0.cycle,
                        0, TRB_TYPE(TRB_SET_TR_DEQUEUE) | (1u << 16) | (slot << 24)) != TRB_CC_SUCCESS ||
                    !xhci_control_request(&candidate, 0x21, 0x0B, 0, (uint16_t)iface_num, NULL, 0))
                    return false;
            }
        }
    }
    if (candidate.kind == HID_MOUSE && !candidate.mouse_report_protocol) {
        printd(DEBUG_USB, "xhci: mouse using boot layout (descriptor length %u)\n", report_length);
    }

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
	ep[4] = (ep_mps << 16) | candidate.report_bytes;          // max ESIT | avg TRB len

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
	xhci_device_t *dev = candidate.kind == HID_KEYBOARD ?
	                  &s_hc->keyboard : &s_hc->mouse;
	*dev = candidate;
	if (dev->kind == HID_KEYBOARD)
	{
		dev->kbd.name = "xhci";
		dev->kbd.debug = DEBUG_USB;
		s_keyboard_claimed = true;
	}
	else {
        char key[64],name[64];
        snprintf(key,sizeof(key),"usb-%02x-%02x-%x-p%u-%04x-%04x",
            s_hc->pci_bus,s_hc->pci_device,s_hc->pci_function,port,vendor,product);
        snprintf(name,sizeof(name),"USB mouse %04x:%04x (port %u)",vendor,product,port);
        input_pointer_register(&dev->pointer,key,name);
        s_mouse_claimed = true;
    }
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
    s_hc->pci_bus=dev->busNo; s_hc->pci_device=dev->deviceNo; s_hc->pci_function=dev->funcNo;
	for (unsigned i = 0; i < 6; i++) s_hc->pci_bars[i] = dev->baseAdd[i];
	s_hc->drive = (xhci_drive_choice_t){ XHCI_DRIVEN_TICK, XHCI_TICK_NOT_ASKED };
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
	// it is reachable from every task's CR3: a sysfs request reaches the
	// driver under its caller's page tables, and the interrupt's top half
	// runs under whatever CR3 it interrupted. PCD: MMIO.
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

	// Command ring + event ring (interrupter 0, its interrupt left DISABLED
	// here: bring-up drains by polling, and kusb turns the interrupt on
	// once it exists, xhci_interrupts_enable).
	if (!ring_init(&s_hc->cmd))
		return false;
	mmio_w64(s_hc->op, XHCI_OP_CRCR, s_hc->cmd.phys | 1);   // | RCS

	s_hc->evt.trb = kmalloc_aligned(PAGE_SIZE);
	if (s_hc->evt.trb == NULL)
		return false;
	s_hc->evt.phys = virt_to_phys(s_hc->evt.trb);
	s_hc->evt.size = RING_TRBS;
	s_hc->evt.dequeue = 0;
	s_hc->evt.cycle = 1;
	s_hc->evt.published = 0;   // ERDP is written with the segment's start just below
	uint64_t *erst = kmalloc_aligned(PAGE_SIZE);   // segment table (1 entry)
	if (erst == NULL)
		return false;
	erst[0] = s_hc->evt.phys;
	erst[1] = RING_TRBS;                            // segment size in TRBs
	mmio_w32(s_hc->rt, XHCI_IR0_ERSTSZ, 1);
	mmio_w64(s_hc->rt, XHCI_IR0_ERDP, s_hc->evt.phys);
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
		xhci_probe_device(port, speed);
		// Continue beyond the input devices: the AX210 can occupy a later port.
		// Unbound successful probes release their slots; failed probes retain
		// DMA storage because a timed-out operation may still reference it.
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

	// Retain initialized controllers and drain their event rings even without
	// bound HID devices. Bluetooth discovery must reach controllers after the
	// ones owning the keyboard and mouse.
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
		// USBQUIET gates the hygiene pass: shared VBUS between paired ports
		// makes powering down a seemingly empty port unsafe on some hardware.
		if (kUSBQuiet)
			xhci_unpower_empty_ports();
		s_controller_count++;
	}
	printf("USB input: %s, %s\n",
	       s_keyboard_claimed ? "keyboard attached" : "no keyboard",
	       s_mouse_claimed ? "mouse attached" : "no mouse");
	printd(DEBUG_USB, "xhci: AX210 discovery complete: %u described\n", s_ax210_described);
}

// ── The interrupt (XHCI_INTERRUPT.md § D5-D9) ───────────────────────────────

// 250 us: at most four thousand interrupts a second from one controller and
// at most a quarter of a millisecond added to an event. Far below a tick and
// far below audio's pacing; the reason for any value at all is a mouse at a
// high report rate. USBIMOD=<n> overrides it for a boot.
#define XHCI_IMOD_DEFAULT 1000u

static uint32_t xhci_irq_read(void *ctx, xhci_irq_reg_t reg)
{
	xhci_t *hc = ctx;
	return reg == XHCI_REG_USBSTS ? mmio_r32(hc->op, XHCI_OP_USBSTS) : mmio_r32(hc->rt, XHCI_IR0_IMAN);
}

static void xhci_irq_write(void *ctx, xhci_irq_reg_t reg, uint32_t value)
{
	xhci_t *hc = ctx;
	if (reg == XHCI_REG_USBSTS)
		mmio_w32(hc->op, XHCI_OP_USBSTS, value);
	else
		mmio_w32(hc->rt, XHCI_IR0_IMAN, value);
}

// THE TOP HALF, in interrupt context on the BSP, under whatever CR3 was
// interrupted. It takes no lock, prints nothing, and touches neither an
// event ring nor s_hc (the doorbell rule, doorbell.h): it acknowledges, marks
// and rings, and kusb does the work. One vector serves every controller, so
// every published controller is asked on every entry; a message that
// arrives while this runs sets the LAPIC's request bit again and this runs
// again.
void xhci_isr(void)
{
	uint32_t count = __atomic_load_n(&s_irq_count, __ATOMIC_ACQUIRE);
	uint64_t now = rdtsc();
	bool any = false;
	for (uint32_t i = 0; i < count; i++) {
		xhci_t *hc = s_irq_list[i];
		if (!xhci_irq_acknowledge(xhci_irq_read, xhci_irq_write, hc))
			continue;
		// The mark, and the OLDEST unanswered interrupt keeps the stamp.
		uint64_t none = 0;
		__atomic_compare_exchange_n(&hc->irq_at, &none, now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		hc->irq_fires++;
		any = true;
	}
	if (!any) {
		s_irq_spurious++;
		return;
	}
	doorbell_ring(&kUsbDoorbell);
}

// kusb gives each controller its interrupt, once, holding the serializing
// flag; false means the flag was held and nothing was done, so it asks
// again at its next wake. By now the handler's vector is installed and
// kusb's bell is registered, so a message has somewhere to go from the
// first moment one can be sent.
bool xhci_interrupts_enable(void)
{
	if (__sync_lock_test_and_set(&s_poll_busy, 1))
		return false;
	uint8_t bsp = (uint8_t)kCPUs[BSP_CPU].apic.id;   // a message address names an APIC ID
	// The interval is sixteen bits; a larger request gets the longest there is.
	uint32_t imod = kUsbImod < 0 ? XHCI_IMOD_DEFAULT : kUsbImod > 0xFFFF ? 0xFFFFu : (uint32_t)kUsbImod;
	for (uint32_t i = 0; i < s_controller_count; i++) {
		xhci_t *hc = &s_controllers[i];
		const pci_caps_t *caps = pci_caps_of(hc->pci_bus, hc->pci_device, hc->pci_function);
		xhci_drive_choice_t choice = xhci_drive_choose(caps && caps->has_msi, caps && caps->has_msix, kUsbPoll);
		bool took = false;
		if (choice.driven == XHCI_DRIVEN_MSI) {
			pci_msi_result_t r = pci_msi_enable(hc->pci_bus, hc->pci_device, hc->pci_function,
			                                    XHCI_IRQ_VECTOR, bsp);
			took = r.took;
			printd(DEBUG_BOOT, "xhci %02x:%02x.%x: MSI capability at 0x%02x (%s-bit), address 0x%08x data 0x%04x, control 0x%04x -> 0x%04x\n",
			       hc->pci_bus, hc->pci_device, hc->pci_function, r.cap, r.is64 ? "64" : "32",
			       r.address, r.data, r.control_before, r.control_after);
		} else if (choice.driven == XHCI_DRIVEN_MSIX) {
			pci_msix_result_t r = pci_msix_enable_one(hc->pci_bus, hc->pci_device, hc->pci_function,
			                                          hc->pci_bars, XHCI_IRQ_VECTOR, bsp);
			took = r.took;
			printd(DEBUG_BOOT, "xhci %02x:%02x.%x: MSI-X capability at 0x%02x, %u entries in BAR %u at 0x%x, entry 0 address 0x%08x data 0x%04x, control 0x%04x%s%s\n",
			       hc->pci_bus, hc->pci_device, hc->pci_function, r.cap, r.entries, r.bar, r.offset,
			       r.address, r.data, r.control_after, r.refused ? "; refused: " : "", r.refused ? r.refused : "");
		}
		if (choice.driven != XHCI_DRIVEN_TICK && !took) {
			// The device's own enables are never set, so a capability that
			// half took sends nothing.
			choice = (xhci_drive_choice_t){ XHCI_DRIVEN_TICK, XHCI_TICK_DID_NOT_TAKE };
			printf("xhci %02x:%02x.%x: its interrupt did not take - staying tick-driven\n",
			       hc->pci_bus, hc->pci_device, hc->pci_function);
		}
		hc->drive = choice;
		if (choice.driven == XHCI_DRIVEN_TICK) {
			printd(DEBUG_BOOT, "xhci %02x:%02x.%x: driven by the %s\n",
			       hc->pci_bus, hc->pci_device, hc->pci_function, xhci_drive_text(choice));
			continue;
		}

		mmio_w32(hc->rt, XHCI_IR0_IMOD, imod);
		// Whatever boot left: an event the boot-time polling did not take
		// has the busy bit set and will never signal. The mark makes this
		// controller's first drain retire it, so nothing depends on a
		// message for a condition older than the enable.
		mmio_w32(hc->op, XHCI_OP_USBSTS, XHCI_USBSTS_EINT);
		mmio_w32(hc->rt, XHCI_IR0_IMAN, XHCI_IMAN_IP);
		__atomic_store_n(&hc->irq_at, rdtsc(), __ATOMIC_SEQ_CST);
		// Published whole before it can interrupt.
		uint32_t n = s_irq_count;
		s_irq_list[n] = hc;
		__atomic_store_n(&s_irq_count, n + 1, __ATOMIC_RELEASE);
		mmio_w32(hc->rt, XHCI_IR0_IMAN, XHCI_IMAN_IE);
		mmio_w32(hc->op, XHCI_OP_USBCMD, mmio_r32(hc->op, XHCI_OP_USBCMD) | USBCMD_INTE);
		// An event that arrived between the clear above and the two gates
		// opening may have set the pending bit with nothing to send it to,
		// and a controller that signals only when the bit RISES would then
		// stay silent for good. Cleared once more, with the gates open: the
		// mark already has this controller's first drain taking whatever is
		// on the ring, and the next event finds the bit clear.
		mmio_w32(hc->rt, XHCI_IR0_IMAN, XHCI_IMAN_IP | XHCI_IMAN_IE);
		printd(DEBUG_BOOT, "xhci %02x:%02x.%x: interrupts live, %s vector 0x%02x at APIC %u, moderation %u (x250 ns)\n",
		       hc->pci_bus, hc->pci_device, hc->pci_function, xhci_drive_text(choice),
		       XHCI_IRQ_VECTOR, bsp, mmio_r32(hc->rt, XHCI_IR0_IMOD) & 0xFFFF);
	}
	// kusb's own release: it is the caller, and it drains next.
	__atomic_store_n(&s_poll_busy, 0, __ATOMIC_SEQ_CST);
	return true;
}

// Shutdown's half of the reset on the way down. Nothing is asked of an
// adapter that never started or has already failed: it would not answer,
// and the descent would only wait out its patience.
void xhci_bluetooth_quiesce_begin(void)
{
	xhci_bt_probe_t *p = s_bluetooth_hc ? &s_bluetooth_hc->bluetooth : NULL;
	if (p && p->runtime && !p->failed && p->host.initialized && !p->host.failed &&
	    s_bt_quiesce == BT_QUIESCE_NONE)
		s_bt_quiesce = BT_QUIESCE_ASKED;
}

bool xhci_bluetooth_quiescing(void)
{
	return s_bt_quiesce == BT_QUIESCE_ASKED || s_bt_quiesce == BT_QUIESCE_SENT;
}

const char *xhci_bluetooth_quiesce_verdict(void)
{
	return s_bt_quiesce == BT_QUIESCE_ANSWERED ? "reset" :
	       s_bt_quiesce == BT_QUIESCE_REFUSED ? "refused its reset" : "did not answer its reset";
}

uint32_t xhci_controller_count(void)
{
	return s_controller_count;
}

// One controller's turn in a pass: drain it against the deadline and
// account for how long what was found may have waited.
static xhci_evt_drain_t xhci_pass_drain(void *ctx, uint32_t index, uint64_t deadline)
{
	(void)ctx;
	s_hc = &s_controllers[index];
	uint64_t ms = kCPUCyclesPerSecond / 1000;
	const uint64_t edges[3] = { 20 * ms, 200 * ms, 2000 * ms };
	xhci_evt_drain_t drain = xhci_drain_until(deadline);
	xhci_wait_account(&s_hc->wait, drain, rdtsc(), edges);
	s_hc->events_polled += drain.handled;
	s_interval_events += drain.handled;
	return drain;
}

bool xhci_pass(uint64_t wake_began, uint64_t deadline, bool *work_left)
{
	*work_left = false;
	// The mark goes up BEFORE the try (see s_kusb_waiting).
	__atomic_store_n(&s_kusb_waiting, 1, __ATOMIC_SEQ_CST);
	if (__sync_lock_test_and_set(&s_poll_busy, 1) != 0) {
		s_refusals++;
		if (s_refused_at == 0)
			s_refused_at = wake_began;
		return false;
	}
	__atomic_store_n(&s_kusb_waiting, 0, __ATOMIC_SEQ_CST);
	if (s_refused_at != 0) {
		if (wake_began - s_refused_at > s_refusal_wait_max)
			s_refusal_wait_max = wake_began - s_refused_at;
		s_refused_at = 0;
	}

	uint64_t gap = xhci_pass_began(wake_began);
	s_visit.count = s_controller_count;
	xhci_visit_result_t visit = xhci_visit(&s_visit, xhci_pass_drain, NULL, deadline);
	if (visit.work_left) {
		s_passes_left_work++;
		*work_left = true;
	}

	// Once per pass, after the drains, whatever the deadline did: Bluetooth's
	// clockwork (which is also what re-arms the adapter's receive
	// endpoints) and the keyboards' typematic repeat.
	uint64_t step_delay = rdtsc() - wake_began;
	if (step_delay > s_step_delay_max)
		s_step_delay_max = step_delay;
	for (uint32_t i = 0; i < s_controller_count; i++) {
		s_hc = &s_controllers[i];
		xhci_bt_runtime_poll();
		if (s_hc->keyboard.present)
			hid_keyboard_tick(&s_hc->keyboard.kbd);
	}
	xhci_pass_ended(gap, visit.handled);
	// kusb's own release: there is nobody to ring.
	__atomic_store_n(&s_poll_busy, 0, __ATOMIC_SEQ_CST);
	return true;
}
