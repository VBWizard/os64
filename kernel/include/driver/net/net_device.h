#ifndef NET_DEVICE_H
#define NET_DEVICE_H

// net_device.h — the NIC seam: "frame in, frame out."
//
// This is the boundary between network DRIVERS (virtio-net, e1000, someday
// a USB dongle, someday the Intel 9260) and the protocol STACK (ethernet →
// ARP/IPv4 → ICMP/UDP/TCP). Same architectural move as block_device.h: the
// stack above speaks only to this abstraction, drivers register into it,
// and the seam is designed against TWO implementations from day one — a
// seam proven against one driver is just that driver's private wrapper
// (AHCI/NVMe taught the block layer that; we inherit the lesson for free).
//
// THE ONE RULE OF THE SEAM (from NETWORK.md, and it is load-bearing for
// the WiFi future): a net_device moves FRAMES. Not "Ethernet cables", not
// "PCI devices" — frames. Firmware-MAC WiFi hardware (the 9260 class)
// presents ethernet-shaped frames too, so nothing here may gratuitously
// assume a cable, a PHY, or a PCI bus. If a field only makes sense for
// one transport, it belongs in the driver's private struct, reached
// through driver_data below.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Ethernet's address size. WiFi uses the same 6-byte EUI-48 space (one
// address administration for both, a 1980s IEEE decision that quietly
// makes the frame-not-cable rule above workable 45 years later).
#define NET_MAC_LEN 6

// v1 frame ceiling: 1500-byte Ethernet payload MTU + 14 header + 4 FCS,
// rounded to a comfortable buffer size. Jumbo frames are a non-goal until
// something measurable wants them (they mostly matter at 10GbE+).
#define NET_FRAME_MAX 1536

// How many NICs one machine may register. Generous for the realistic
// fleet (QEMU virtio + e1000 test twins, later a USB dongle beside a
// WiFi part); bump it the day a real machine presents more.
#define NET_MAX_DEVICES 4

struct net_device;   // forward — ops receive the device they belong to

// The driver side of the seam: what a NIC must be able to DO.
// Return convention (house rule, in-band, no errno): 0 = success,
// negative = error. Never a byte count — the block layer's DRESULT
// confusion (see block_operations in vfs.h) is a warning label we read.
typedef struct net_operations
{
	// Transmit one frame. BUFFER OWNERSHIP: the buffer belongs to the
	// CALLER and the driver must be completely done with it (copied into
	// its TX ring) by the time this returns — the caller is free to reuse
	// or free it immediately after. v1 is copy-based on purpose: zero-copy
	// TX is a real optimization with real lifetime headaches (whose page
	// is pinned? until which interrupt?), and it is exactly the kind of
	// cleverness you add AFTER the pcap says the simple thing works.
	int32_t (*transmit)(struct net_device* dev, const void* frame, uint16_t length);

	// Take what the hardware has: reclaim finished transmit descriptors and
	// deliver every received frame through net_device_rx. Returns whether
	// anything moved, so the caller knows to come around again. Called by
	// knet, the network drainer thread, never from an interrupt handler —
	// a handler RINGS (doorbell.h); the drain is the work the ring defers.
	// The verb the seam lacked while there were two drivers and a per-driver
	// poll call in the scheduler pass; the third driver made it pay.
	bool (*drain)(struct net_device* dev);
} net_operations_t;

// One registered NIC. Drivers allocate this (kmalloc), fill it in, and
// hand it to net_device_register(); it stays alive for the life of the
// system (there is no unregister until hot-unplug is a real requirement —
// stated so the absence reads as a decision, not an oversight).
typedef struct net_device
{
	char name[16];              // "virtio0", "e1000_0" — for logs and, someday, Chris's netstat-alike
	uint8_t mac[NET_MAC_LEN];   // read from the hardware at init, never invented
	uint16_t mtu;               // largest PAYLOAD the link carries (1500 for ethernet)
	bool link_up;               // best-effort; drivers update it when the hardware says so

	// HOW FAST, and IN BOTH DIRECTIONS AT ONCE? Same story as rx_errors
	// below, one driver later: the r8125 decoded speed and duplex for its own
	// boot line and kept them private, so the concept lived in one driver's
	// struct and the seam never learned it. The e1000 reads the very same
	// facts out of STATUS bits 7:6 and 0 — and /sys/net wants to print them
	// for ANY card, which is what made the omission visible (Chris, 2026-08-20:
	// "the e1000 doesn't list the link speed anywhere"). A fact two drivers
	// both have is a seam fact.
	//
	// ZERO MEANS UNKNOWN, and it is a legitimate answer, not a failure: an
	// 8125 negotiating 2.5GbE reports through a field this driver only partly
	// decodes, and virtio-net is software with no wire to have a speed at all.
	// Callers print nothing rather than guessing — the same contract model and
	// location keep.
	uint32_t link_mbps;         // 10 / 100 / 1000 / 2500 …, 0 = not known
	bool full_duplex;           // meaningless unless link_mbps is nonzero

	// THE HARDWARE'S OWN LINK WORD, verbatim, for the day the decode above is
	// not the whole story (2026-09-05: the P5 at 100/full on a gigabit
	// switch, and the number that explained it was in no file anyone could
	// read). A driver formats its status register here as hex — PHYstatus on
	// the r8125, STATUS on the e1000 — meaningful only to someone with that
	// chip's register map, and never decoded by the seam. Same contract as
	// model and location: empty means the driver has no such word (virtio
	// has no wire), and /sys/net omits the line.
	char link_raw[16];          // "0x0000004b"

	// WHAT THIS CARD ACTUALLY IS, for /sys/net/<card> (2026-08-20). The name
	// above is the stack's handle for it; these two are the hardware's own
	// answer, and they are what a person reads to check that the driver that
	// bound is the driver they meant. BOTH OPTIONAL: a driver that leaves
	// them empty simply omits those lines, which is honest for a device that
	// has no such identity (virtio-net is a contract, not a part number).
	// `model` points at a string literal or a driver-owned buffer that
	// outlives the device — never at a stack local.
	const char* model;          // "RTL8125B", "82540EM" — the part, not the family
	char location[16];          // "02:00.0" — lspci's spelling, so /sys/net and
	                            // /sys/bus/pci can be read against each other

	net_operations_t* ops;      // the driver's verbs (transmit)
	void* driver_data;          // the driver's private world (rings, BARs, locks) —
	                            // the seam never looks inside, same job as
	                            // block_extra_info in block_device_info_t

	// Counters — the "no silent caps" doctrine in struct form. Every frame
	// that arrives, leaves, or DIES does so on a counter, so a lying link
	// is visible from a debugger (and eventually from userland) instead of
	// being a shrug. 64-bit on purpose: 32-bit packet counters wrap in
	// minutes at line rate, and a counter that wraps is a counter that lies.
	uint64_t tx_frames, tx_bytes, tx_errors;
	uint64_t rx_frames, rx_bytes;
	uint64_t rx_dropped_no_handler;  // arrived before the stack claimed RX (see net_set_rx_handler)
	uint64_t rx_dropped_too_big;     // frame exceeded NET_FRAME_MAX — counted, never truncated
	// Frames the HARDWARE says arrived damaged: bad CRC, alignment error,
	// symbol error — the wire lied, and we drop them exactly as the medium
	// would have. Added 2026-08-05 by the e1000, and worth recording WHY:
	// this field is the seam's charter paying out. virtio-net is software
	// pretending to be a NIC, and software does not corrupt frames in
	// flight, so for one whole driver the concept had no referent and the
	// asymmetry with tx_errors looked like symmetry. The second
	// implementation — real silicon with a real errors byte in every
	// receive descriptor — is what made the hole visible. A seam proven
	// against one driver is that driver's private wrapper; this is what
	// the sentence meant.
	uint64_t rx_errors;
} net_device_t;

// The STACK side of the seam: where inbound frames go. Phase 2's ethernet
// input claims this slot; until then, frames are counted and dropped
// (rx_dropped_no_handler) rather than silently vanishing.
//
// CONTEXT CONTRACT (v1, deliberately conservative): drivers may call
// net_device_rx from interrupt context, therefore the handler must be
// quick, must not sleep, and must not take locks a non-IRQ path holds
// without irqsave discipline. The frame buffer belongs to the DRIVER and
// is valid only for the duration of the call — the handler copies out
// what it keeps. If Phase 2 profiling says IRQ-context protocol work is
// too heavy, the queue-to-kworker upgrade happens INSIDE net_device_rx,
// and neither drivers nor the stack change shape — that is what the seam
// is for.
typedef void (*net_rx_handler_t)(net_device_t* dev, const void* frame, uint16_t length);

// --- Registration and lookup (net_device.c) --------------------------------
extern net_device_t* kNetDevices[NET_MAX_DEVICES];
extern uint32_t kNetDeviceCount;

// Register a NIC. Called by drivers during init_* (single-threaded boot
// context today — a registration lock arrives the day hotplug does, and
// the comment at the call site in net_device.c says so too).
// Returns 0 on success, negative if the table is full or dev is malformed.
int32_t net_device_register(net_device_t* dev);

// The stack (or a test fixture) claims/releases inbound delivery.
void net_set_rx_handler(net_rx_handler_t handler);

// Drivers deliver every received frame here — never directly to the stack.
// This indirection is what lets a pcap-style tap, a counter, or a kworker
// queue slide in later without touching a single driver.
void net_device_rx(net_device_t* dev, const void* frame, uint16_t length);

#endif // NET_DEVICE_H
