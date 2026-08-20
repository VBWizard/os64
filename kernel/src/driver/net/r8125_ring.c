// r8125_ring.c — the descriptor-ring protocol, hardware-free.
//
// See r8125_ring.h for the contract and for why this file exists apart from
// the driver. The one-line version: everything that can go wrong in a ring
// driver except the register offsets lives here, and here it can be tested
// on the host in milliseconds instead of on a machine that has to be booted
// by hand.
//
// NO KERNEL HEADERS. Not an accident, and not negotiable — tools/
// test_r8125_host.c compiles this exact file with the host's gcc. The day
// this needs paging.h is the day the bugs move back to the P5.

#include "driver/net/r8125_ring.h"

// Compiler fence, NOT a hardware one — and that distinction is the whole
// comment. x86's TSO already keeps stores in program order as far as the
// bus (and therefore the DMA engine) is concerned; the only party free to
// reorder them is the COMPILER, because these descriptors are plain memory
// to it. Without this, nothing forbids sinking the buffer memcpy or the
// addr/opts2 stores PAST the opts1 store that flips OWN — handing the
// device a descriptor whose contents arrive after the permission to read
// them. Masked at -O0, live at -O2: the exact "one rebuild away" fingerprint
// CLAUDE.md warns about, on the one machine with no debugger attached.
// An empty asm with a memory clobber is portable to the host harness's gcc,
// which keeps this file's no-kernel-headers contract intact.
static inline void r8125_ring_compiler_fence(void)
{
	__asm__ volatile("" ::: "memory");
}

// Every write to opts1 goes through here, and that is the entire reason it
// exists: EOR shares the word with OWN and the length, so any rewrite that
// forgets it silently converts "wrap at the end of the ring" into "keep
// walking into whatever memory comes next". A DMA engine given that
// permission does not fail an assertion — it corrupts something unrelated,
// somewhere else, later. One choke point, one place to be right.
//
// The store itself is the OWNERSHIP HANDOFF, so it is fenced and volatile:
// the fence pins every prior plain store (buffer bytes, addr, opts2) below
// it, and volatile stops the compiler eliding or duplicating the store that
// a bus master is watching for.
static inline void r8125_set_opts1(r8125_desc_t* ring, uint16_t count,
                                   uint16_t index, uint32_t value)
{
	if (index == (uint16_t)(count - 1))
		value |= R8125_DESC_EOR;
	r8125_ring_compiler_fence();
	*(volatile uint32_t*)&ring[index].opts1 = value;
}

void r8125_ring_init_rx(r8125_desc_t* ring, uint16_t count,
                        uint64_t buf_phys, uint32_t buf_size)
{
	for (uint16_t i = 0; i < count; i++)
	{
		ring[i].addr  = buf_phys + (uint64_t)i * buf_size;
		ring[i].opts2 = 0;
		// The device may fill up to buf_size bytes here, and it is ours to
		// declare: the length field on a receive descriptor is the BUFFER
		// SIZE going in and the FRAME LENGTH coming back.
		r8125_set_opts1(ring, count, i, R8125_DESC_OWN | (buf_size & R8125_DESC_LEN_MASK));
	}
}

void r8125_ring_init_tx(r8125_desc_t* ring, uint16_t count,
                        uint64_t buf_phys, uint32_t buf_size)
{
	for (uint16_t i = 0; i < count; i++)
	{
		ring[i].addr  = buf_phys + (uint64_t)i * buf_size;
		ring[i].opts2 = 0;
		// OWN clear: the ring starts entirely ours. A transmit ring handed
		// to the device at init would have it sending whatever those buffers
		// happened to contain.
		r8125_set_opts1(ring, count, i, 0);
	}
}

bool r8125_rx_ready(const r8125_desc_t* ring, uint16_t cursor,
                    uint16_t* length_out, bool* damaged_out)
{
	// The mirror of set_opts1's discipline: volatile, because the DEVICE
	// writes this word and a compiler that proved "nothing in this program
	// stores here" would be entitled to reuse a stale read. The fence pins
	// the caller's subsequent reads of the BUFFER after this load — seeing
	// OWN clear is the permission to look at the bytes, so the look must
	// not be hoisted above the permission.
	uint32_t opts1 = *(const volatile uint32_t*)&ring[cursor].opts1;
	if (opts1 & R8125_DESC_OWN)
		return false;   // still the device's — nothing has arrived here yet
	r8125_ring_compiler_fence();

	*length_out  = (uint16_t)(opts1 & R8125_DESC_LEN_MASK);
	*damaged_out = (opts1 & R8125_RX_ERRORS) != 0;
	return true;
}

bool r8125_rx_strip_fcs(uint16_t descriptor_length, uint16_t buffer_size,
                        uint16_t* frame_length_out)
{
	if (descriptor_length < R8125_RX_FCS_LEN ||
	    descriptor_length > buffer_size)
		return false;

	*frame_length_out = (uint16_t)(descriptor_length - R8125_RX_FCS_LEN);
	return true;
}

void r8125_rx_refill(r8125_desc_t* ring, uint16_t count, uint16_t index,
                     uint64_t buf_phys, uint32_t buf_size)
{
	ring[index].addr  = buf_phys;
	ring[index].opts2 = 0;
	r8125_set_opts1(ring, count, index,
	                R8125_DESC_OWN | (buf_size & R8125_DESC_LEN_MASK));
}

bool r8125_tx_claim(uint16_t count, uint16_t tx_next, uint16_t tx_clean,
                    uint16_t* index_out)
{
	uint16_t candidate = r8125_ring_next(tx_next, count);
	if (candidate == tx_clean)
		return false;   // full — see the spare-slot note in the header
	*index_out = tx_next;
	return true;
}

void r8125_tx_post(r8125_desc_t* ring, uint16_t count, uint16_t index,
                   uint16_t length)
{
	ring[index].opts2 = 0;
	// FS and LS together: this driver never splits a frame across
	// descriptors. Scatter-gather is what those bits are FOR, and it is
	// worth having only when there is something to gather — os64's transmit
	// path hands down one contiguous buffer.
	r8125_set_opts1(ring, count, index,
	                R8125_DESC_OWN | R8125_DESC_FS | R8125_DESC_LS |
	                (length & R8125_DESC_LEN_MASK));
}

uint16_t r8125_tx_reap(const r8125_desc_t* ring, uint16_t count,
                       uint16_t* tx_clean, uint16_t tx_next)
{
	uint16_t reclaimed = 0;
	while (*tx_clean != tx_next)
	{
		// Volatile for the same reason as rx_ready: OWN is cleared by the
		// device, and this loop polls it. No fence needed here — reclaiming
		// only moves an index; nobody reads the buffer behind it.
		if (*(const volatile uint32_t*)&ring[*tx_clean].opts1 & R8125_DESC_OWN)
			break;      // the device has not finished this one; stop here
		*tx_clean = r8125_ring_next(*tx_clean, count);
		reclaimed++;
	}
	return reclaimed;
}
