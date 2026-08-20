// test_r8125_host.c — HOST-side unit test for the RTL8125's ring protocol.
//
// WHY THIS EXISTS, stated plainly: QEMU emulates no RTL8125. The only rig
// that can run the real driver is a machine in Chris's house that must be
// booted by hand and watched. So the ring protocol was deliberately split
// out of the driver (r8125_ring.c — no kernel headers, no MMIO, no paging)
// so that the part where the bugs actually live can be compiled with the
// host's gcc and exercised in milliseconds.
//
// And they DO live here. A ring driver's real hazards are index arithmetic,
// ownership handoff, wrap, and one bit that must never be lost — none of
// which are about silicon. What the silicon owes us is register offsets;
// everything else is arithmetic, and arithmetic is testable.
//
// Build & run (one line; no trailing backslashes here, because a backslash
// ending a // comment silently swallows the NEXT line — which this file's
// first compile demonstrated):
//
//   gcc -g -Wall -Wextra -I kernel/include kernel/src/driver/net/r8125_ring.c tools/test_r8125_host.c -o /tmp/os64_r8125_test && /tmp/os64_r8125_test
//
// The house convention (test_heap_host.c, test_fmt_host.c): loud on
// failure, quiet on success, nonzero exit when anything fails.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "driver/net/r8125_ring.h"

static int g_failures = 0;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__); printf("\n"); \
            g_failures++; \
        } \
    } while (0)

#define RING_COUNT 8
#define BUF_SIZE   2048
#define BUF_PHYS   0x1000000ULL

// THE INVARIANT THAT OUTRANKS EVERY OTHER ASSERTION HERE.
//
// EOR marks the last descriptor and is the only thing telling the device to
// wrap rather than walk forward through unrelated memory. It shares opts1
// with OWN and the length, so every rewrite risks dropping it — and dropping
// it produces no error, no wrong return value, and no symptom until a DMA
// engine has scribbled somewhere it was never pointed at. So this is checked
// after EVERY operation in every test below, not once at the end.
static void check_eor(const r8125_desc_t* ring, const char* when)
{
	for (uint16_t i = 0; i < RING_COUNT; i++)
	{
		bool should = (i == RING_COUNT - 1);
		bool has    = (ring[i].opts1 & R8125_DESC_EOR) != 0;
		if (should != has)
		{
			printf("FAIL EOR invariant (%s): descriptor %u %s EOR\n",
			       when, i, has ? "wrongly HAS" : "is MISSING");
			g_failures++;
		}
	}
}

// ── RX ──────────────────────────────────────────────────────────────────

static void test_rx_init_gives_every_descriptor_to_the_device(void)
{
	r8125_desc_t ring[RING_COUNT];
	memset(ring, 0xAA, sizeof(ring));   // arrive DIRTY, like real memory
	r8125_ring_init_rx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	for (uint16_t i = 0; i < RING_COUNT; i++)
	{
		CHECK(ring[i].opts1 & R8125_DESC_OWN, "descriptor %u not owned by device", i);
		CHECK(ring[i].addr == BUF_PHYS + (uint64_t)i * BUF_SIZE,
		      "descriptor %u addr wrong: 0x%llx", i, (unsigned long long)ring[i].addr);
		CHECK((ring[i].opts1 & R8125_DESC_LEN_MASK) == BUF_SIZE,
		      "descriptor %u buffer size wrong", i);
	}
	check_eor(ring, "after rx init");
}

static void test_rx_not_ready_while_device_owns_it(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_rx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	uint16_t len; bool damaged;
	CHECK(!r8125_rx_ready(ring, 0, &len, &damaged),
	      "claimed a frame was ready while the device still owned it");
}

// The device "completing" a receive: it clears OWN and writes the length.
static void device_delivers(r8125_desc_t* ring, uint16_t index,
                            uint16_t length, uint32_t error_bits)
{
	uint32_t opts1 = ring[index].opts1;
	opts1 &= ~R8125_DESC_OWN;                 // hand it back to the driver
	opts1 &= ~(uint32_t)R8125_DESC_LEN_MASK;  // replace buffer size...
	opts1 |= (length & R8125_DESC_LEN_MASK);  // ...with the frame length
	opts1 |= error_bits;
	ring[index].opts1 = opts1;                // EOR untouched, as hardware does
}

static void test_rx_delivers_then_refills(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_rx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	device_delivers(ring, 0, 74, 0);

	uint16_t len; bool damaged;
	CHECK(r8125_rx_ready(ring, 0, &len, &damaged), "completed frame not seen");
	CHECK(len == 74, "wrong length: %u", len);
	CHECK(!damaged, "clean frame reported as damaged");

	r8125_rx_refill(ring, RING_COUNT, 0, BUF_PHYS, BUF_SIZE);
	CHECK(ring[0].opts1 & R8125_DESC_OWN, "refill did not return the descriptor");
	CHECK((ring[0].opts1 & R8125_DESC_LEN_MASK) == BUF_SIZE,
	      "refill did not restore the buffer size");
	check_eor(ring, "after refill");
}

static void test_rx_reports_damage(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_rx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	// Each error bit alone must be enough. Under uncertainty about the
	// 8125's exact error encoding, "any of these means drop it" is the
	// safe reading, and this asserts we actually implement that reading.
	const uint32_t bits[] = { R8125_RX_RES, R8125_RX_RWT, R8125_RX_RUNT, R8125_RX_CRC };
	for (unsigned b = 0; b < sizeof(bits) / sizeof(bits[0]); b++)
	{
		r8125_ring_init_rx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);
		device_delivers(ring, 3, 60, bits[b]);
		uint16_t len; bool damaged;
		CHECK(r8125_rx_ready(ring, 3, &len, &damaged), "damaged frame not seen at all");
		CHECK(damaged, "error bit 0x%08x not reported as damage", bits[b]);
	}
}

static void test_rx_strips_fcs_and_rejects_invalid_lengths(void)
{
	uint16_t frame_length = 0xFFFF;

	CHECK(r8125_rx_strip_fcs(68, BUF_SIZE, &frame_length),
	      "rejected a valid receive length");
	CHECK(frame_length == 64,
	      "did not strip the four-byte FCS: got %u, expected 64", frame_length);

	frame_length = 0xFFFF;
	CHECK(!r8125_rx_strip_fcs(R8125_RX_FCS_LEN - 1, BUF_SIZE, &frame_length),
	      "accepted a descriptor shorter than its FCS");
	CHECK(frame_length == 0xFFFF,
	      "changed the output length after rejecting an undersized descriptor");

	CHECK(!r8125_rx_strip_fcs(BUF_SIZE + 1, BUF_SIZE, &frame_length),
	      "accepted a descriptor longer than its DMA buffer");
}

// THE WRAP TEST, and the reason EOR gets checked every time: drive a full
// lap plus change through the ring, refilling as we go, and demand the
// invariant survives the last descriptor being rewritten repeatedly.
static void test_rx_wraps_without_losing_eor(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_rx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	uint16_t cursor = 0;
	for (int frame = 0; frame < RING_COUNT * 3 + 3; frame++)
	{
		device_delivers(ring, cursor, 100, 0);

		uint16_t len; bool damaged;
		CHECK(r8125_rx_ready(ring, cursor, &len, &damaged),
		      "frame %d not ready at cursor %u", frame, cursor);
		CHECK(len == 100, "frame %d wrong length %u", frame, len);

		r8125_rx_refill(ring, RING_COUNT, cursor,
		                BUF_PHYS + (uint64_t)cursor * BUF_SIZE, BUF_SIZE);
		check_eor(ring, "mid-wrap");
		cursor = r8125_ring_next(cursor, RING_COUNT);
	}
	CHECK(cursor == (RING_COUNT * 3 + 3) % RING_COUNT, "cursor drifted: %u", cursor);
}

// ── TX ──────────────────────────────────────────────────────────────────

static void test_tx_init_keeps_the_ring_ours(void)
{
	r8125_desc_t ring[RING_COUNT];
	memset(ring, 0xAA, sizeof(ring));
	r8125_ring_init_tx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	for (uint16_t i = 0; i < RING_COUNT; i++)
		CHECK(!(ring[i].opts1 & R8125_DESC_OWN),
		      "descriptor %u handed to the device at init — it would transmit garbage", i);
	check_eor(ring, "after tx init");
}

static void test_tx_post_sets_first_and_last(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_tx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	r8125_tx_post(ring, RING_COUNT, 0, 128);
	CHECK(ring[0].opts1 & R8125_DESC_OWN, "post did not hand the descriptor over");
	CHECK(ring[0].opts1 & R8125_DESC_FS, "post did not mark first segment");
	CHECK(ring[0].opts1 & R8125_DESC_LS, "post did not mark last segment");
	CHECK((ring[0].opts1 & R8125_DESC_LEN_MASK) == 128, "post wrote the wrong length");

	// ...and on the LAST descriptor, where EOR is at stake.
	r8125_tx_post(ring, RING_COUNT, RING_COUNT - 1, 64);
	check_eor(ring, "after posting to the last descriptor");
}

// The full-ring test. A ring that reports space it does not have overwrites
// a frame the device is still sending; one that reports full too early just
// costs throughput. This asserts the exact boundary — count-1 usable slots.
static void test_tx_reports_full_with_one_slot_spare(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_tx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	uint16_t tx_next = 0, tx_clean = 0, index = 0;
	int claimed = 0;
	while (r8125_tx_claim(RING_COUNT, tx_next, tx_clean, &index))
	{
		r8125_tx_post(ring, RING_COUNT, index, 64);
		tx_next = r8125_ring_next(tx_next, RING_COUNT);
		claimed++;
		if (claimed > RING_COUNT * 2) break;   // runaway guard
	}
	CHECK(claimed == RING_COUNT - 1,
	      "claimed %d slots from a %d-descriptor ring (expected %d — one stays spare)",
	      claimed, RING_COUNT, RING_COUNT - 1);
	check_eor(ring, "after filling the tx ring");
}

static void test_tx_reap_stops_at_the_first_unfinished(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_tx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	uint16_t tx_next = 0, tx_clean = 0, index = 0;
	for (int i = 0; i < 4; i++)
	{
		r8125_tx_claim(RING_COUNT, tx_next, tx_clean, &index);
		r8125_tx_post(ring, RING_COUNT, index, 64);
		tx_next = r8125_ring_next(tx_next, RING_COUNT);
	}

	// The device finishes 0 and 1, but NOT 2. A reaper that assumed
	// completions were contiguous-and-done would reclaim 3 as well — and
	// descriptor 2's buffer is still being transmitted.
	ring[0].opts1 &= ~R8125_DESC_OWN;
	ring[1].opts1 &= ~R8125_DESC_OWN;
	ring[3].opts1 &= ~R8125_DESC_OWN;

	uint16_t got = r8125_tx_reap(ring, RING_COUNT, &tx_clean, tx_next);
	CHECK(got == 2, "reaped %u, expected 2 (must stop at the unfinished one)", got);
	CHECK(tx_clean == 2, "tx_clean is %u, expected 2", tx_clean);

	// Now it finishes 2; the next reap should take 2 AND 3.
	ring[2].opts1 &= ~R8125_DESC_OWN;
	got = r8125_tx_reap(ring, RING_COUNT, &tx_clean, tx_next);
	CHECK(got == 2, "second reap took %u, expected 2", got);
	CHECK(tx_clean == tx_next, "ring not fully drained: clean %u next %u", tx_clean, tx_next);
}

static void test_tx_reap_on_empty_ring_is_a_no_op(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_tx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	uint16_t tx_clean = 5;
	uint16_t got = r8125_tx_reap(ring, RING_COUNT, &tx_clean, 5);
	CHECK(got == 0, "reaped %u from an empty ring", got);
	CHECK(tx_clean == 5, "reap moved tx_clean on an empty ring");
}

// A full lap of transmits, so wrap is exercised on the TX side too — and
// so EOR survives the last descriptor being posted to repeatedly.
static void test_tx_wraps(void)
{
	r8125_desc_t ring[RING_COUNT];
	r8125_ring_init_tx(ring, RING_COUNT, BUF_PHYS, BUF_SIZE);

	uint16_t tx_next = 0, tx_clean = 0, index = 0;
	for (int frame = 0; frame < RING_COUNT * 3; frame++)
	{
		CHECK(r8125_tx_claim(RING_COUNT, tx_next, tx_clean, &index),
		      "ring reported full at frame %d despite draining", frame);
		r8125_tx_post(ring, RING_COUNT, index, (uint16_t)(60 + frame));
		tx_next = r8125_ring_next(tx_next, RING_COUNT);

		ring[index].opts1 &= ~R8125_DESC_OWN;     // device completes it
		r8125_tx_reap(ring, RING_COUNT, &tx_clean, tx_next);
		check_eor(ring, "mid tx wrap");
	}
	CHECK(tx_clean == tx_next, "ring not drained after the lap");
}

int main(void)
{
	test_rx_init_gives_every_descriptor_to_the_device();
	test_rx_not_ready_while_device_owns_it();
	test_rx_delivers_then_refills();
	test_rx_reports_damage();
	test_rx_strips_fcs_and_rejects_invalid_lengths();
	test_rx_wraps_without_losing_eor();

	test_tx_init_keeps_the_ring_ours();
	test_tx_post_sets_first_and_last();
	test_tx_reports_full_with_one_slot_spare();
	test_tx_reap_stops_at_the_first_unfinished();
	test_tx_reap_on_empty_ring_is_a_no_op();
	test_tx_wraps();

	if (g_failures == 0)
	{
		printf("r8125 ring tests: all passed\n");
		return 0;
	}
	printf("r8125 ring tests: %d FAILURE(S)\n", g_failures);
	return 1;
}
