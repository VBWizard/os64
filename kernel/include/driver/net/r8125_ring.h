#ifndef R8125_RING_H
#define R8125_RING_H

// r8125_ring.h — the RealTek descriptor-ring protocol, WITHOUT the hardware.
//
// THE POINT OF THIS FILE EXISTING SEPARATELY: QEMU emulates no RTL8125, so
// the driver's only test rig is a machine in Chris's house that has to be
// booted by hand. But almost nothing that can go wrong in a ring driver is
// actually about the silicon — it is index arithmetic, ownership handoff,
// wrap, and one bit that must never be lost. All of that is pure computation
// over a block of memory, so it belongs where a host compiler can run it a
// thousand times a second (tools/test_r8125_host.c).
//
// Nothing here includes a kernel header, touches MMIO, or knows what a page
// is. Keep it that way: the moment this file needs paging.h, it stops being
// testable and the bugs move back to the P5 where they are expensive.
//
// ── THE PROTOCOL, in one paragraph ──────────────────────────────────────
//
// Both rings are arrays of 16-byte descriptors, each holding flags+length,
// a second flags word, and a 64-bit BUFFER PHYSICAL ADDRESS (the device is
// a bus master; it has never heard of virtual memory). The high bit of
// opts1 is OWN, and it is the entire synchronization protocol: OWN=1 means
// the DEVICE owns this descriptor, OWN=0 means the driver does. RX gives
// the device empty buffers (OWN=1) and takes back filled ones (OWN=0); TX
// is the mirror — the driver fills a buffer and sets OWN=1, and the device
// clears it when the frame is on the wire. One bit, both directions,
// no locks with the hardware.
//
// ── THE BIT THAT MUST NEVER BE LOST ─────────────────────────────────────
//
// EOR (End Of Ring, opts1 bit 30) marks the LAST descriptor and is the only
// thing telling the device to wrap instead of walking forward through
// whatever memory follows the ring. It lives in the same word as OWN and
// the length, so every single rewrite of opts1 must preserve it. Dropping
// it does not fail a test or return an error — it hands a DMA engine
// permission to march through kernel memory. Every function here that
// touches opts1 preserves EOR explicitly, and the host test asserts it
// after every operation, because this is the one mistake whose symptom is
// unrelated corruption somewhere else entirely.

#include <stdint.h>
#include <stdbool.h>

// One descriptor. 16 bytes, little-endian, and the layout is the same on
// both rings. [8169-family] — unchanged for the 8125 generation.
typedef struct
{
	uint32_t opts1;    // OWN | EOR | FS | LS | ... | length
	uint32_t opts2;    // VLAN and checksum-offload hints; we write zero
	uint64_t addr;     // buffer PHYSICAL address
} __attribute__((packed)) r8125_desc_t;

// opts1 bits [8169-family]. These four are the lineage's bedrock and are
// the ones this driver actually depends on.
#define R8125_DESC_OWN   (1u << 31)   // 1 = the DEVICE owns this descriptor
#define R8125_DESC_EOR   (1u << 30)   // last descriptor in the ring — wrap here
#define R8125_DESC_FS    (1u << 29)   // first segment of a frame
#define R8125_DESC_LS    (1u << 28)   // last segment of a frame

// Frame length lives in the low bits of opts1. 14 bits on receive (the
// hardware reports what it wrote); the transmit side uses the same field
// for what we are asking it to send.
#define R8125_DESC_LEN_MASK 0x3FFFu

// RTL8125 receive descriptors include the Ethernet frame check sequence in
// the reported byte count. The network-device seam, like the rest of the
// stack, deals in frame lengths without those trailing CRC bytes.
#define R8125_RX_FCS_LEN 4u

// Receive error bits [8169-family] — UNCONFIRMED for the 8125's descriptor
// and treated accordingly: a descriptor carrying ANY of them is counted as
// an rx_error and dropped rather than delivered. That is the safe direction
// under uncertainty — the worst case of a wrong bit here is discarding a
// good frame, which shows up as a counter and a retransmit, whereas the
// worst case of ignoring errors is feeding the protocol stack garbage that
// the hardware already knew was damaged.
#define R8125_RX_RES   (1u << 21)   // receive error summary
#define R8125_RX_RWT   (1u << 22)   // watchdog timer expired
#define R8125_RX_RUNT  (1u << 20)   // runt packet
#define R8125_RX_CRC   (1u << 19)   // CRC error
#define R8125_RX_ERRORS (R8125_RX_RES | R8125_RX_RWT | R8125_RX_RUNT | R8125_RX_CRC)

// ── Ring construction ───────────────────────────────────────────────────

// Lay out a fresh RECEIVE ring: every descriptor points at its standing
// buffer and is handed to the device (OWN=1), with EOR on the last.
void r8125_ring_init_rx(r8125_desc_t* ring, uint16_t count,
                        uint64_t buf_phys, uint32_t buf_size);

// Lay out a fresh TRANSMIT ring: every descriptor points at its standing
// buffer and belongs to US (OWN=0), with EOR on the last.
void r8125_ring_init_tx(r8125_desc_t* ring, uint16_t count,
                        uint64_t buf_phys, uint32_t buf_size);

// ── Receive ─────────────────────────────────────────────────────────────

// Has the device finished with the descriptor at *cursor? If so return true
// and report its frame length and whether it arrived damaged; the caller
// then copies the buffer out and calls r8125_rx_refill. Advancing the
// cursor is the caller's job precisely so a caller that cannot take the
// frame right now can simply not advance.
bool r8125_rx_ready(const r8125_desc_t* ring, uint16_t cursor,
                    uint16_t* length_out, bool* damaged_out);

// Validate the device-reported receive length and convert it to the length
// expected by the network stack. Returns false rather than underflowing if
// the descriptor is shorter than its included FCS, or larger than the DMA
// buffer that received it.
bool r8125_rx_strip_fcs(uint16_t descriptor_length, uint16_t buffer_size,
                        uint16_t* frame_length_out);

// Hand descriptor `index` back to the device, empty. Preserves EOR.
void r8125_rx_refill(r8125_desc_t* ring, uint16_t count, uint16_t index,
                     uint64_t buf_phys, uint32_t buf_size);

// ── Transmit ────────────────────────────────────────────────────────────

// Claim a free transmit slot, or return false if the ring is full.
//
// ONE SLOT IS ALWAYS LEFT UNUSED. In any head/tail ring, "completely full"
// and "completely empty" are the same pair of indices, so a spare slot is
// what makes them distinguishable — the same arithmetic the pipe buffer and
// the e1000's rings live by, and cheaper than carrying a separate count
// that can disagree with the indices.
bool r8125_tx_claim(uint16_t count, uint16_t tx_next, uint16_t tx_clean,
                    uint16_t* index_out);

// Post a single-descriptor frame: first AND last segment, length, OWN to
// the device. Preserves EOR. The caller has already copied the bytes into
// the buffer this descriptor points at.
void r8125_tx_post(r8125_desc_t* ring, uint16_t count, uint16_t index,
                   uint16_t length);

// Reclaim every descriptor the device has finished with, advancing
// *tx_clean. Returns how many were reclaimed. Out-of-order completion is
// not a thing the hardware does on a single queue, but the loop is written
// to stop at the first still-owned descriptor rather than assuming — a
// scan that "knows" completions are contiguous will reclaim a live buffer
// the day it turns out to be wrong.
uint16_t r8125_tx_reap(const r8125_desc_t* ring, uint16_t count,
                       uint16_t* tx_clean, uint16_t tx_next);

// Ring index advance. Trivial, and centralized anyway: an open-coded
// `(i + 1) % count` that gets `count` from the wrong ring is a bug that
// reads as correct.
static inline uint16_t r8125_ring_next(uint16_t index, uint16_t count)
{
	return (uint16_t)((index + 1u) % count);
}

#endif // R8125_RING_H
