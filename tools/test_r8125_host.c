// test_r8125_host.c — HOST-side unit test for the RTL8125's ring protocol
// and, since 2026-09-05, its PHY arithmetic (r8125_phy.c — same split, same
// reason: the address map and the link decode are pure computation).
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
//   gcc -g -Wall -Wextra -I kernel/include kernel/src/driver/net/r8125_ring.c kernel/src/driver/net/r8125_phy.c tools/test_r8125_host.c -o /tmp/os64_r8125_test && /tmp/os64_r8125_test
//
// The house convention (test_heap_host.c, test_fmt_host.c): loud on
// failure, quiet on success, nonzero exit when anything fails.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "driver/net/r8125_ring.h"
#include "driver/net/r8125_phy.h"

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

// ── PHY ─────────────────────────────────────────────────────────────────
//
// The arithmetic behind r8125.c's PHY access (r8125_phy.c), checked
// against LITERAL transcriptions of the vendor's formulas rather than
// against numbers remembered from them. That distinction earned its keep
// on day one: the design note for this slice had the PHY window at 0x64
// (the CSI data register) until the vendor source was actually read.

// Realtek r8125_n.c, map_phy_ocp_addr, transcribed verbatim. If the
// driver's MII-to-OCP mapping and this ever disagree, the driver is wrong.
static uint16_t vendor_map_phy_ocp_addr(uint16_t PageNum, uint8_t RegNum)
{
	uint16_t OcpPageNum = 0;
	uint8_t OcpRegNum = 0;
	uint16_t OcpPhyAddress = 0;

	if (PageNum == 0) {
		OcpPageNum = 0x0A40 + (RegNum / 8);
		OcpRegNum = 0x10 + (RegNum % 8);
	} else {
		OcpPageNum = PageNum;
		OcpRegNum = RegNum;
	}

	OcpPageNum <<= 4;

	if (OcpRegNum < 16) {
		OcpPhyAddress = 0;
	} else {
		OcpRegNum -= 16;
		OcpRegNum <<= 1;
		OcpPhyAddress = OcpPageNum + OcpRegNum;
	}

	return OcpPhyAddress;
}

static void test_phy_mii_map_matches_the_vendor(void)
{
	for (uint8_t reg = 0; reg < 16; reg++)
		CHECK(r8125_phy_mii_ocp_addr(reg) == vendor_map_phy_ocp_addr(0, reg),
		      "MII reg %u: ours 0x%04x, vendor 0x%04x", reg,
		      r8125_phy_mii_ocp_addr(reg), vendor_map_phy_ocp_addr(0, reg));

	// The registers the driver names, at the addresses the vendor's own
	// direct accesses spell out (rtl8125_set_speed_xmii reads MII_CTRL1000
	// and writes 0xA5D4; realtek.c calls the 2.5G pair page 0xa5d, 0x12/0x13).
	CHECK(r8125_phy_mii_ocp_addr(R8125_MII_BMCR)    == 0xA400, "BMCR");
	CHECK(r8125_phy_mii_ocp_addr(R8125_MII_BMSR)    == 0xA402, "BMSR");
	CHECK(r8125_phy_mii_ocp_addr(R8125_MII_PHYID1)  == 0xA404, "PHYID1");
	CHECK(r8125_phy_mii_ocp_addr(R8125_MII_ANAR)    == 0xA408, "ANAR");
	CHECK(r8125_phy_mii_ocp_addr(R8125_MII_ANLPAR)  == 0xA40A, "ANLPAR");
	CHECK(r8125_phy_mii_ocp_addr(R8125_MII_GBCR)    == 0xA412, "GBCR");
	CHECK(r8125_phy_mii_ocp_addr(R8125_MII_GBSR)    == 0xA414, "GBSR");
	CHECK(r8125_phy_mii_ocp_addr(R8125_MII_ESTATUS) == 0xA41E, "ESTATUS");
	CHECK(vendor_map_phy_ocp_addr(0xA5D, 0x12) == R8125_PHY_OCP_ADV_2500, "2.5G advertisement register");
	CHECK(vendor_map_phy_ocp_addr(0xA5D, 0x13) == R8125_PHY_OCP_LPA_2500, "2.5G partner register");
}

static void test_phy_ocp_command_words(void)
{
	// The vendor: data32 = RegAddr/2; data32 <<= 16; a write adds
	// OCPR_Write (bit 31) | value.
	CHECK(r8125_phy_ocp_read_command(0xA400) == 0x52000000u,
	      "read cmd 0x%08x", r8125_phy_ocp_read_command(0xA400));
	CHECK(r8125_phy_ocp_read_command(0xA5D4) == 0x52EA0000u,
	      "read cmd 0x%08x", r8125_phy_ocp_read_command(0xA5D4));
	CHECK(r8125_phy_ocp_write_command(0xA400, R8125_BMCR_RESTART_AUTONEG) == 0xD2001200u,
	      "write cmd 0x%08x", r8125_phy_ocp_write_command(0xA400, R8125_BMCR_RESTART_AUTONEG));

	// Across the whole even address space: a read never carries the flag,
	// a write always does, and the data rides in the low half untouched.
	for (uint32_t addr = 0; addr <= 0xFFFE; addr += 2)
	{
		uint32_t rd = r8125_phy_ocp_read_command((uint16_t)addr);
		uint32_t wr = r8125_phy_ocp_write_command((uint16_t)addr, 0xBEEF);
		if ((rd & R8125_PHYOCP_FLAG) != 0 || (rd & R8125_PHYOCP_DATA_MASK) != 0 ||
		    (wr & R8125_PHYOCP_FLAG) == 0 || (wr & R8125_PHYOCP_DATA_MASK) != 0xBEEF ||
		    ((wr ^ rd) & 0x7FFF0000u) != 0)
		{
			CHECK(false, "command words wrong at 0x%04x: rd 0x%08x wr 0x%08x", addr, rd, wr);
			break;
		}
	}
}

static void test_phy_bmcr_service(void)
{
	// The P5 as found (autoneg on, the 1000 speed-select bit) and mid-restart.
	CHECK(!r8125_phy_bmcr_out_of_service(0x1040), "the P5's as-found BMCR is in service");
	CHECK(!r8125_phy_bmcr_out_of_service(0x1200), "a restart in flight is in service");
	// The four states firmware can leave a PHY in that a restart write clears.
	CHECK(r8125_phy_bmcr_out_of_service(0x0000), "autonegotiation off");
	CHECK(r8125_phy_bmcr_out_of_service(0x2100), "forced 100/full");
	CHECK(r8125_phy_bmcr_out_of_service(0x1800), "power-down");
	CHECK(r8125_phy_bmcr_out_of_service(0x1400), "isolate");
	CHECK(r8125_phy_bmcr_out_of_service(0x5000), "loopback");
}

static void test_phy_id_check(void)
{
	CHECK(r8125_phy_id_is_realtek(0x001C, 0xC800), "RTL8125's own PHY id refused");
	CHECK(r8125_phy_id_is_realtek(0x001C, 0xC916), "another Realtek PHY (RTL8211F) refused");
	CHECK(!r8125_phy_id_is_realtek(0xFFFF, 0xFFFF), "a dead window (all ones) accepted");
	CHECK(!r8125_phy_id_is_realtek(0x0000, 0x0000), "a dead window (all zeroes) accepted");
	CHECK(!r8125_phy_id_is_realtek(0x0141, 0x0CC2), "a Marvell 88E1111 accepted");
}

static void test_phy_status_decode(void)
{
	r8125_link_t l;

	l = r8125_phy_decode_status(0x00000000);
	CHECK(!l.up && l.mbps == 0, "down word decoded as up");

	// The P5, 2026-09-05: link | 100 | full — the report this slice exists for.
	l = r8125_phy_decode_status(0x0000000B);
	CHECK(l.up && l.mbps == 100 && l.full_duplex, "100/full: up %d mbps %u fd %d", l.up, l.mbps, l.full_duplex);
	l = r8125_phy_decode_status(0x00000013);
	CHECK(l.up && l.mbps == 1000 && l.full_duplex, "1000/full");
	l = r8125_phy_decode_status(0x00000006);
	CHECK(l.up && l.mbps == 10 && !l.full_duplex, "10/half");
	l = r8125_phy_decode_status(0x00000403);
	CHECK(l.up && l.mbps == 2500 && l.full_duplex, "2500/full");
	// The vendor's precedence: 2500F outranks a stray 1000F bit.
	l = r8125_phy_decode_status(0x00000413);
	CHECK(l.mbps == 2500, "2500 should outrank 1000: got %u", l.mbps);
	// The two "lite" bits fold into 1000, as the vendor folds them.
	l = r8125_phy_decode_status(0x00000203);
	CHECK(l.mbps == 1000, "2500-lite should read as 1000: got %u", l.mbps);
	l = r8125_phy_decode_status(0x00080003);
	CHECK(l.mbps == 1000, "1000-lite should read as 1000: got %u", l.mbps);
	// Up with no speed bit at all: honest zero, not a guess.
	l = r8125_phy_decode_status(0x00000003);
	CHECK(l.up && l.mbps == 0, "up-without-speed should be unknown: got %u", l.mbps);
	// Pause resolution rides along.
	l = r8125_phy_decode_status(0x00000073);
	CHECK(l.rx_pause && l.tx_pause, "pause bits not decoded");
	l = r8125_phy_decode_status(0x00000013);
	CHECK(!l.rx_pause && !l.tx_pause, "pause bits invented");
	// Stale speed bits under a DOWN link must not become a speed.
	l = r8125_phy_decode_status(0x00000011);
	CHECK(!l.up && l.mbps == 0, "down link reported a speed: %u", l.mbps);
}

static void test_phy_plan_leaves_a_correct_advertisement_alone(void)
{
	// ANAR 0x0DE1: selector 802.3, 10H/10F/100H/100F, pause + asym pause
	// (a common reset value). GBCR 0x0200: 1000 full only. 2.5G off.
	r8125_phy_adv_t have = { 0x0DE1, 0x0200, 0x0000 }, want;
	CHECK(!r8125_phy_plan_advertisement(&have, &want, false),
	      "planned a change for an advertisement that is already right");
	CHECK(want.anar == have.anar && want.gbcr == have.gbcr && want.adv2500 == have.adv2500,
	      "'no change' but the plan differs: %04x %04x %04x", want.anar, want.gbcr, want.adv2500);
}

static void test_phy_plan_adds_gigabit_and_drops_2500(void)
{
	// A PHY found advertising 1000 half as well as full, with 2.5G on: the
	// likely power-on state of a 2.5G part.
	r8125_phy_adv_t have = { 0x01E1, 0x0300, 0x0080 }, want;
	CHECK(r8125_phy_plan_advertisement(&have, &want, false), "must plan a change");
	CHECK(want.anar == 0x01E1, "ANAR changed needlessly: %04x", want.anar);
	CHECK(want.gbcr == 0x0200, "1000 half should drop, full stay: %04x", want.gbcr);
	CHECK(want.adv2500 == 0x0000, "2.5G should clear: %04x", want.adv2500);

	// A PHY somebody left at 10/100 only (the "speed down on shutdown"
	// pattern): gigabit comes back.
	have.anar = 0x0DE1; have.gbcr = 0x0000; have.adv2500 = 0x0000;
	CHECK(r8125_phy_plan_advertisement(&have, &want, false), "must plan a change");
	CHECK(want.gbcr == 0x0200, "1000 full not restored: %04x", want.gbcr);

	// A PHY with the 10/100 bits stripped: all four come back.
	have.anar = 0x0001; have.gbcr = 0x0200;
	CHECK(r8125_phy_plan_advertisement(&have, &want, false), "must plan a change");
	CHECK(want.anar == 0x01E1, "10/100 not restored: %04x", want.anar);

	// The caller asking for 2.5G gets exactly that one bit.
	have.anar = 0x0DE1; have.gbcr = 0x0200; have.adv2500 = 0x0000;
	CHECK(r8125_phy_plan_advertisement(&have, &want, true), "must plan a change");
	CHECK(want.adv2500 == 0x0080 && want.anar == have.anar && want.gbcr == have.gbcr,
	      "2.5G on should touch one bit: %04x %04x %04x", want.anar, want.gbcr, want.adv2500);
}

static void test_phy_plan_preserves_what_it_does_not_own(void)
{
	// ANAR: next-page (15), remote-fault (13), asym pause (11), pause (10)
	// all set and NONE of the speed bits; GBCR: master/slave manual + value
	// (12, 11); 2.5G register: every bit but the one we own.
	r8125_phy_adv_t have = { 0xAC01, 0x1800, 0x1F7F }, want;
	r8125_phy_plan_advertisement(&have, &want, false);
	CHECK(want.anar == 0xADE1, "ANAR: foreign bits disturbed or speeds missing: %04x", want.anar);
	CHECK(want.gbcr == 0x1A00, "GBCR: master/slave bits disturbed: %04x", want.gbcr);
	CHECK(want.adv2500 == 0x1F7F, "2.5G register: foreign bits disturbed: %04x", want.adv2500);

	// The selector field is forced to 802.3 whatever it read.
	have.anar = 0x0000;
	r8125_phy_plan_advertisement(&have, &want, false);
	CHECK((want.anar & R8125_ADV_SELECTOR_MASK) == R8125_ADV_SELECTOR_8023, "selector not forced: %04x", want.anar);
	have.anar = 0x001F;
	r8125_phy_plan_advertisement(&have, &want, false);
	CHECK((want.anar & R8125_ADV_SELECTOR_MASK) == R8125_ADV_SELECTOR_8023, "selector not forced: %04x", want.anar);
}

static void test_phy_abilities_text(void)
{
	char buf[R8125_ABILITY_TEXT_CAP];

	uint8_t ours = r8125_phy_abilities_ours(0x0DE1, 0x0200, 0x0000);
	CHECK(ours == (R8125_ABILITY_10H | R8125_ABILITY_10F | R8125_ABILITY_100H |
	               R8125_ABILITY_100F | R8125_ABILITY_1000F), "ours 0x%02x", ours);
	r8125_phy_abilities_text(ours, buf, sizeof buf);
	CHECK(strcmp(buf, "10H/10F/100H/100F/1000F") == 0, "got '%s'", buf);

	// A partner that offers everything: ANLPAR with ACK set, GBSR with
	// both partner bits (10, 11) plus the idle-error/status bits above.
	uint8_t theirs = r8125_phy_abilities_partner(0xC1E1, 0x7C00, 0x0020);
	CHECK(theirs == 0x7F, "theirs 0x%02x", theirs);
	uint32_t n = r8125_phy_abilities_text(theirs, buf, sizeof buf);
	CHECK(n == 35 && strcmp(buf, "10H/10F/100H/100F/1000H/1000F/2500F") == 0, "got '%s' (%u)", buf, n);

	// The 2.5G partner bit is bit 5 of ITS register, not bit 7 like ours.
	CHECK(r8125_phy_abilities_partner(0, 0, 0x0080) == 0, "our 2.5G bit position accepted as the partner's");
	CHECK(r8125_phy_abilities_ours(0, 0, 0x0020) == 0, "the partner's 2.5G bit position accepted as ours");

	n = r8125_phy_abilities_text(0, buf, sizeof buf);
	CHECK(n == 4 && strcmp(buf, "none") == 0, "got '%s'", buf);

	// Truncation is clean and terminated; a zero cap writes nothing.
	n = r8125_phy_abilities_text(0x7F, buf, 8);
	CHECK(n == 7 && strcmp(buf, "10H/10F") == 0, "truncated to '%s' (%u)", buf, n);
	n = r8125_phy_abilities_text(0x7F, buf, 1);
	CHECK(n == 0 && buf[0] == '\0', "cap 1 should yield an empty string");
	buf[0] = 'X';
	n = r8125_phy_abilities_text(0x7F, buf, 0);
	CHECK(n == 0 && buf[0] == 'X', "cap 0 wrote something");
}

static void test_phy_best_common_speed(void)
{
	// The P5 as found on 2026-09-05: we offered everything including 2.5G,
	// the switch offered 10/100/1000 — a correct negotiation lands on 1000.
	uint8_t ours   = r8125_phy_abilities_ours(0x1DE1, 0x0200, 0x0081);
	uint8_t theirs = r8125_phy_abilities_partner(0xC5E1, 0x3800, 0x0000);
	CHECK(r8125_phy_best_common_mbps(ours, theirs) == 1000, "P5 pair should be 1000: %u",
	      r8125_phy_best_common_mbps(ours, theirs));
	// Both 2.5G-capable: 2500. Half-duplex-only partner at 100: 100.
	CHECK(r8125_phy_best_common_mbps(0x7F, 0x7F) == 2500, "2.5G pair");
	CHECK(r8125_phy_best_common_mbps(0x7F, R8125_ABILITY_100H) == 100, "100H partner");
	CHECK(r8125_phy_best_common_mbps(0x7F, R8125_ABILITY_10F) == 10, "10F partner");
	// Nothing in common, and the empty partner page, both read as 0 — never
	// a speed a caller could compare a link against.
	CHECK(r8125_phy_best_common_mbps(R8125_ABILITY_1000F, R8125_ABILITY_100F) == 0, "disjoint");
	CHECK(r8125_phy_best_common_mbps(0x7F, 0x00) == 0, "empty partner page");
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

	test_phy_mii_map_matches_the_vendor();
	test_phy_ocp_command_words();
	test_phy_id_check();
	test_phy_bmcr_service();
	test_phy_status_decode();
	test_phy_plan_leaves_a_correct_advertisement_alone();
	test_phy_plan_adds_gigabit_and_drops_2500();
	test_phy_plan_preserves_what_it_does_not_own();
	test_phy_abilities_text();
	test_phy_best_common_speed();

	if (g_failures == 0)
	{
		printf("r8125 ring + PHY tests: all passed\n");
		return 0;
	}
	printf("r8125 ring + PHY tests: %d FAILURE(S)\n", g_failures);
	return 1;
}
