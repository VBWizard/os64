// r8125_phy.c — the RTL8125 PHY's arithmetic, hardware-free.
//
// See r8125_phy.h for the contract, the provenance, and why this lives
// apart from the driver. NO KERNEL HEADERS: tools/test_r8125_host.c
// compiles this exact file with the host's gcc.

#include "driver/net/r8125_phy.h"

// ── The OCP window ──────────────────────────────────────────────────────

uint32_t r8125_phy_ocp_read_command(uint16_t ocp_addr)
{
	// The vendor: data32 = RegAddr/2; data32 <<= OCPR_Addr_Reg_shift (16).
	return ((uint32_t)ocp_addr / 2u) << 16;
}

uint32_t r8125_phy_ocp_write_command(uint16_t ocp_addr, uint16_t value)
{
	// The vendor: the read command | OCPR_Write | value.
	return r8125_phy_ocp_read_command(ocp_addr) | R8125_PHYOCP_FLAG | value;
}

uint16_t r8125_phy_mii_ocp_addr(uint8_t mii_reg)
{
	return (uint16_t)(R8125_PHY_OCP_STD_BASE + 2u * (mii_reg & 0x0F));
}

// ── Identity ────────────────────────────────────────────────────────────

bool r8125_phy_id_is_realtek(uint16_t id1, uint16_t id2)
{
	return id1 == R8125_PHYID1_REALTEK &&
	       (id2 & R8125_PHYID2_REALTEK_MASK) == R8125_PHYID2_REALTEK;
}

// ── PHYstatus ───────────────────────────────────────────────────────────

r8125_link_t r8125_phy_decode_status(uint32_t phystatus)
{
	r8125_link_t link;
	link.up          = (phystatus & R8125_PHYS_LINK) != 0;
	link.full_duplex = (phystatus & R8125_PHYS_FULLDUP) != 0;
	link.rx_pause    = (phystatus & R8125_PHYS_RXFLOW) != 0;
	link.tx_pause    = (phystatus & R8125_PHYS_TXFLOW) != 0;
	link.mbps        = 0;

	if (!link.up)
		return link;

	// The order and the grouping are the vendor's rtl8125_convert_link_speed,
	// reproduced rather than reinterpreted. The two "lite" bits are folded
	// into 1000 exactly as the vendor folds them: whatever a lite link is
	// electrically, that is the speed the vendor's own ethtool reports for
	// it, and inventing a different number here would only make os64 and
	// Linux disagree about the same wire.
	if (phystatus & R8125_PHYS_2500F)
		link.mbps = 2500;
	else if (phystatus & (R8125_PHYS_1000F | R8125_PHYS_2500L | R8125_PHYS_1000L))
		link.mbps = 1000;
	else if (phystatus & R8125_PHYS_100M)
		link.mbps = 100;
	else if (phystatus & R8125_PHYS_10M)
		link.mbps = 10;
	// else: up, and none of the speed bits we know — mbps stays 0 (=
	// unknown), and the raw word the driver prints beside it is the clue.
	return link;
}

// ── The advertisement ───────────────────────────────────────────────────

bool r8125_phy_plan_advertisement(const r8125_phy_adv_t* have,
                                  r8125_phy_adv_t* want,
                                  bool advertise_2500)
{
	// ANAR: every 10/100 mode, selector 802.3, everything else (pause,
	// remote fault, next page, reserved) as found.
	want->anar = (uint16_t)((have->anar & ~(R8125_ADV_SPEED_MASK | R8125_ADV_SELECTOR_MASK))
	                        | R8125_ADV_SPEED_MASK | R8125_ADV_SELECTOR_8023);

	// GBCR: 1000 full only; 1000 half off; master/slave and test bits as
	// found.
	want->gbcr = (uint16_t)((have->gbcr & ~(R8125_GBCR_1000HALF | R8125_GBCR_1000FULL))
	                        | R8125_GBCR_1000FULL);

	// 2.5G: one bit, the caller's policy; the rest of the register as found.
	want->adv2500 = (uint16_t)((have->adv2500 & ~R8125_PHY_ADV_2500FULL)
	                           | (advertise_2500 ? R8125_PHY_ADV_2500FULL : 0));

	return want->anar != have->anar ||
	       want->gbcr != have->gbcr ||
	       want->adv2500 != have->adv2500;
}

// ── Abilities ───────────────────────────────────────────────────────────

uint8_t r8125_phy_abilities_ours(uint16_t anar, uint16_t gbcr, uint16_t adv2500)
{
	uint8_t a = 0;
	if (anar & R8125_ADV_10HALF)            a |= R8125_ABILITY_10H;
	if (anar & R8125_ADV_10FULL)            a |= R8125_ABILITY_10F;
	if (anar & R8125_ADV_100HALF)           a |= R8125_ABILITY_100H;
	if (anar & R8125_ADV_100FULL)           a |= R8125_ABILITY_100F;
	if (gbcr & R8125_GBCR_1000HALF)         a |= R8125_ABILITY_1000H;
	if (gbcr & R8125_GBCR_1000FULL)         a |= R8125_ABILITY_1000F;
	if (adv2500 & R8125_PHY_ADV_2500FULL)   a |= R8125_ABILITY_2500F;
	return a;
}

uint8_t r8125_phy_abilities_partner(uint16_t anlpar, uint16_t gbsr, uint16_t lpa2500)
{
	uint8_t a = 0;
	if (anlpar & R8125_ADV_10HALF)          a |= R8125_ABILITY_10H;
	if (anlpar & R8125_ADV_10FULL)          a |= R8125_ABILITY_10F;
	if (anlpar & R8125_ADV_100HALF)         a |= R8125_ABILITY_100H;
	if (anlpar & R8125_ADV_100FULL)         a |= R8125_ABILITY_100F;
	if (gbsr & R8125_GBSR_LP_1000HALF)      a |= R8125_ABILITY_1000H;
	if (gbsr & R8125_GBSR_LP_1000FULL)      a |= R8125_ABILITY_1000F;
	if (lpa2500 & R8125_PHY_LPA_2500FULL)   a |= R8125_ABILITY_2500F;
	return a;
}

// Append a NUL-terminated piece to out[] at *at, never past cap-1, keeping
// the result terminated. Hand-rolled because this file may not include a
// string library the host and the kernel would spell differently.
static void abilities_append(char* out, uint32_t cap, uint32_t* at, const char* piece)
{
	while (*piece != '\0' && *at + 1 < cap)
		out[(*at)++] = *piece++;
	out[*at] = '\0';
}

uint32_t r8125_phy_abilities_text(uint8_t abilities, char* out, uint32_t cap)
{
	static const struct { uint8_t bit; const char* name; } names[] = {
		{ R8125_ABILITY_10H,   "10H"   },
		{ R8125_ABILITY_10F,   "10F"   },
		{ R8125_ABILITY_100H,  "100H"  },
		{ R8125_ABILITY_100F,  "100F"  },
		{ R8125_ABILITY_1000H, "1000H" },
		{ R8125_ABILITY_1000F, "1000F" },
		{ R8125_ABILITY_2500F, "2500F" },
	};

	uint32_t at = 0;
	if (cap == 0)
		return 0;
	out[0] = '\0';

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
	{
		if ((abilities & names[i].bit) == 0)
			continue;
		if (at != 0)
			abilities_append(out, cap, &at, "/");
		abilities_append(out, cap, &at, names[i].name);
	}
	if (at == 0)
		abilities_append(out, cap, &at, "none");
	return at;
}

uint32_t r8125_phy_best_common_mbps(uint8_t ours, uint8_t theirs)
{
	uint8_t both = (uint8_t)(ours & theirs);
	if (both & R8125_ABILITY_2500F)                          return 2500;
	if (both & (R8125_ABILITY_1000F | R8125_ABILITY_1000H))  return 1000;
	if (both & (R8125_ABILITY_100F | R8125_ABILITY_100H))    return 100;
	if (both & (R8125_ABILITY_10F | R8125_ABILITY_10H))      return 10;
	return 0;
}

bool r8125_phy_bmcr_out_of_service(uint16_t bmcr)
{
	if ((bmcr & R8125_BMCR_ANENABLE) == 0)
		return true;
	return (bmcr & (R8125_BMCR_PDOWN | R8125_BMCR_ISOLATE | R8125_BMCR_LOOPBACK)) != 0;
}

bool r8125_phy_restart_taken(uint16_t bmsr_before, uint32_t phystatus_before,
                             uint16_t bmcr_now, uint16_t bmsr_now, uint32_t phystatus_now)
{
	if ((bmcr_now & R8125_BMCR_ANRESTART) == 0)
		return true;   // the PHY consumed the restart bit
	bool complete_dropped = (bmsr_before & R8125_BMSR_ANEGCOMPLETE) != 0 &&
	                        (bmsr_now & R8125_BMSR_ANEGCOMPLETE) == 0;
	bool link_fell = (phystatus_before & R8125_PHYS_LINK) != 0 &&
	                 (phystatus_now & R8125_PHYS_LINK) == 0;
	return complete_dropped || link_fell;
}
