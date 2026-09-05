#ifndef R8125_PHY_H
#define R8125_PHY_H

// r8125_phy.h — the RTL8125's PHY: register map, link decode, and the
// advertisement policy, WITHOUT the hardware.
//
// Same split as r8125_ring.h, for the same reason: QEMU emulates no
// RTL8125, so the only rig that runs the real driver is a machine in
// Chris's house that has to be booted by hand. What this file holds is
// arithmetic — which OCP address a MII register lives at, what a PHYstatus
// word means, which bits the advertisement should carry — and arithmetic
// runs on the host in milliseconds (tools/test_r8125_host.c). The driver
// keeps only what genuinely needs the silicon: the MMIO window, the spin
// waits, the log lines.
//
// NO KERNEL HEADERS. The host harness compiles this file's .c with the
// host's gcc, so the day it needs one is the day the PHY bugs move back
// to the P5.
//
// ── WHY THE DRIVER TALKS TO THE PHY AT ALL (2026-09-05) ─────────────────
//
// It didn't, until the P5 linked at 100/full on a gigabit switch: four
// cables, a cold boot, the switch's own LED agreeing it was 100M, and the
// PC on the neighbouring port at 1000. The driver had never touched the
// PHY — it read one status byte and reported whatever autonegotiation had
// produced, which is what an OS does when it trusts the firmware's idea of
// the link. Linux's r8169 does not trust it: genphy_config_aneg rewrites
// the advertisement and restarts negotiation at every probe, which is why
// Linux users never see this. Now so does os64.
//
// ── PROVENANCE ──────────────────────────────────────────────────────────
//
// Every register and bit here was read out of Realtek's GPL r8125 driver
// (r8125.h / r8125_n.c, the awesometic/realtek-r8125-dkms mirror of the
// vendor source) on 2026-09-05, function by function — map_phy_ocp_addr,
// mdio_real_direct_read_phy_ocp, rtl8125_set_speed_xmii,
// rtl8125_convert_link_speed — not from memory. The MII register numbers
// and their bit layouts are IEEE 802.3 clause 22 and clause 40, the same
// on every twisted-pair PHY since 1995.

#include <stdint.h>
#include <stdbool.h>

// ── The PHY OCP window (MAC register PHYOCP, 0xB8, 32-bit) ──────────────
//
// The 8169 generation reached its PHY through PHYAR (0x60), a plain MDIO
// shuttle. The 8125 replaced it with an "OCP" window over a much larger
// register space, and the standard MII registers are simply one page of
// that space. One 32-bit register carries both directions:
//
//   bits 31    : write flag on the way in; BUSY/DONE flag on the way out
//   bits 30..16: OCP address / 2 (addresses are always even)
//   bits 15..0 : data
//
// A READ posts the address and waits for bit 31 to come SET; a WRITE posts
// address|flag|data and waits for bit 31 to come CLEAR. The asymmetry is
// the vendor's, reproduced exactly (mdio_real_direct_read_phy_ocp /
// mdio_real_direct_write_phy_ocp).
#define R8125_REG_PHYOCP        0xB8
#define R8125_PHYOCP_FLAG       0x80000000u
#define R8125_PHYOCP_DATA_MASK  0x0000FFFFu

uint32_t r8125_phy_ocp_read_command(uint16_t ocp_addr);
uint32_t r8125_phy_ocp_write_command(uint16_t ocp_addr, uint16_t value);

// MII register N (0..15, page 0) lives at OCP 0xA400 + 2N. That is what
// the vendor's map_phy_ocp_addr computes for page 0 once its page/offset
// gymnastics are unwound: page 0xA40 + N/8, register 0x10 + N%8, and the
// page-shift-plus-double-the-offset arithmetic lands on exactly 0xA400 + 2N.
// The host test asserts that against a literal transcription of the
// vendor's formula, so a slip in either place fails on the host.
#define R8125_PHY_OCP_STD_BASE  0xA400
uint16_t r8125_phy_mii_ocp_addr(uint8_t mii_reg);

// ── Standard MII registers (IEEE 802.3 clause 22 / clause 40) ───────────
#define R8125_MII_BMCR     0    // basic control
#define R8125_MII_BMSR     1    // basic status
#define R8125_MII_PHYID1   2    // OUI, upper part
#define R8125_MII_PHYID2   3    // OUI lower part, model, revision
#define R8125_MII_ANAR     4    // what WE advertise (autoneg advertisement)
#define R8125_MII_ANLPAR   5    // what the PARTNER advertises (link partner ability)
#define R8125_MII_ANER     6    // autoneg expansion
#define R8125_MII_GBCR     9    // 1000BASE-T control: our gigabit advertisement
#define R8125_MII_GBSR     10   // 1000BASE-T status: the partner's
#define R8125_MII_ESTATUS  15   // extended status: what this PHY can do at gigabit

// BMCR
#define R8125_BMCR_ANRESTART  0x0200
#define R8125_BMCR_ISOLATE    0x0400
#define R8125_BMCR_PDOWN      0x0800
#define R8125_BMCR_ANENABLE   0x1000
#define R8125_BMCR_LOOPBACK   0x4000
#define R8125_BMCR_RESET      0x8000
// Autonegotiation restart, exactly as the vendor writes it
// (rtl8125_phy_restart_nway): enable + restart, nothing else. Writing the
// whole register rather than read-modify-write is deliberate — it also
// clears power-down, isolate and loopback, none of which we ever want.
#define R8125_BMCR_RESTART_AUTONEG (R8125_BMCR_ANENABLE | R8125_BMCR_ANRESTART)

// BMSR. LSTATUS is LATCHING-LOW: it reports "the link has been down since
// you last read this", not "the link is down", so it is printed for the
// record and PHYstatus (a MAC register, live) is what the driver believes.
#define R8125_BMSR_LSTATUS      0x0004
#define R8125_BMSR_ANEGCAPABLE  0x0008
#define R8125_BMSR_ANEGCOMPLETE 0x0020
#define R8125_BMSR_ESTATEN      0x0100   // ESTATUS is implemented

// ANAR and ANLPAR share a layout (802.3 annex 28B). The selector field is
// the low five bits and must say 802.3 (1); the speed bits are 5..8.
#define R8125_ADV_SELECTOR_MASK  0x001F
#define R8125_ADV_SELECTOR_8023  0x0001
#define R8125_ADV_10HALF         0x0020
#define R8125_ADV_10FULL         0x0040
#define R8125_ADV_100HALF        0x0080
#define R8125_ADV_100FULL        0x0100
#define R8125_ADV_PAUSE          0x0400
#define R8125_ADV_ASYM_PAUSE     0x0800
#define R8125_ADV_SPEED_MASK     (R8125_ADV_10HALF | R8125_ADV_10FULL | \
                                  R8125_ADV_100HALF | R8125_ADV_100FULL)

// GBCR (ours) and GBSR (theirs) put the same two abilities at different
// bits — 8/9 in control, 10/11 in status — a clause 40 quirk worth a
// named constant each, because "shift by two" is the kind of thing that is
// right in one function and wrong in the next.
#define R8125_GBCR_1000HALF      0x0100
#define R8125_GBCR_1000FULL      0x0200
#define R8125_GBSR_LP_1000HALF   0x0400
#define R8125_GBSR_LP_1000FULL   0x0800

// ESTATUS: the PHY's own claim about gigabit.
#define R8125_ESTATUS_1000HALF   0x1000
#define R8125_ESTATUS_1000FULL   0x2000

// ── The 2.5G pair, Realtek's own (no clause 22 register carries 2.5G) ────
// OCP 0xA5D4 bit 7 is our 2500BASE-T advertisement; OCP 0xA5D6 bit 5 is
// the partner's. Both straight from rtl8125_set_speed_xmii and
// rtl8125_link_on_patch; Linux's realtek.c reaches the same registers as
// "page 0xa5d, registers 0x12/0x13".
#define R8125_PHY_OCP_ADV_2500   0xA5D4
#define R8125_PHY_OCP_LPA_2500   0xA5D6
#define R8125_PHY_ADV_2500FULL   0x0080
#define R8125_PHY_LPA_2500FULL   0x0020

// ── Identity ────────────────────────────────────────────────────────────
// Realtek's OUI (00:E0:4C) lands in the PHY ID registers as PHYID1 = 0x001C
// and the top six bits of PHYID2 = 0xC8. The driver refuses to WRITE the
// PHY unless this matches: a window that is not answering reads as all
// ones or all zeroes, and advertising into it would be programming a
// register we cannot see.
#define R8125_PHYID1_REALTEK       0x001C
#define R8125_PHYID2_REALTEK_MASK  0xFC00
#define R8125_PHYID2_REALTEK       0xC800
bool r8125_phy_id_is_realtek(uint16_t id1, uint16_t id2);

// Is the PHY out of autonegotiating service — autonegotiation disabled (a
// forced mode), powered down, isolated, or in loopback? Any of these is a
// state firmware can leave a PHY in, none of them is one this driver ever
// wants, and the one BMCR write the driver makes (R8125_BMCR_RESTART_AUTONEG)
// clears all four. A PHY in such a state gets that write even when its
// advertisement registers already read exactly as planned (Codex, PR #66).
bool r8125_phy_bmcr_out_of_service(uint16_t bmcr);

// ── PHYstatus (MAC register 0x6C), read as 32 bits ──────────────────────
//
// The 8169 generation's PHYstatus was one byte; the 8125 kept those eight
// bits and added the 2.5G answers ABOVE them, which is why a driver that
// reads the byte can never name a 2.5G link. Bits per the vendor's
// RTL8125_register_content enum:
#define R8125_PHYS_FULLDUP    0x00000001
#define R8125_PHYS_LINK       0x00000002
#define R8125_PHYS_10M        0x00000004
#define R8125_PHYS_100M       0x00000008
#define R8125_PHYS_1000F      0x00000010
#define R8125_PHYS_RXFLOW     0x00000020   // pause frames resolved: we may be paused
#define R8125_PHYS_TXFLOW     0x00000040   // pause frames resolved: we may pause them
#define R8125_PHYS_POWERSAVE  0x00000080
#define R8125_PHYS_2500L      0x00000200   // "lite" — see the decode
#define R8125_PHYS_2500F      0x00000400
#define R8125_PHYS_5000F      0x00001000   // the RTL8126's; never set on an 8125
#define R8125_PHYS_1000L      0x00080000   // "lite" — see the decode

typedef struct
{
	bool     up;
	uint32_t mbps;          // 10 / 100 / 1000 / 2500; 0 = up but not decodable
	bool     full_duplex;
	bool     rx_pause;      // the partner may pause us
	bool     tx_pause;      // we may pause the partner
} r8125_link_t;

r8125_link_t r8125_phy_decode_status(uint32_t phystatus);

// ── The advertisement ───────────────────────────────────────────────────
//
// The three registers that together say what we are willing to be: ANAR
// (10/100, pause), GBCR (1000), and Realtek's 2.5G control.
typedef struct
{
	uint16_t anar;
	uint16_t gbcr;
	uint16_t adv2500;
} r8125_phy_adv_t;

// Given what the PHY currently advertises, compute what it SHOULD, and
// return whether the two differ (= a write and an autoneg restart are
// needed). THE POLICY, stated once:
//
//   - 10 and 100, half and full: advertised. Half-duplex partners still
//     exist (a hub, a port forced to 100/half) and a link at 100/half beats
//     no link.
//   - 1000 full: advertised. 1000 HALF is not — 1000BASE-T half duplex was
//     specified and then never built; no switch offers it, and advertising
//     a mode nobody speaks only lengthens the negotiation.
//   - 2500 full: the CALLER's choice. r8125.c owns the reason.
//   - The selector field is forced to 802.3, because the speed bits mean
//     nothing under any other selector.
//   - PAUSE bits are LEFT AS FOUND. Flow control is a separate decision
//     with its own measurement (the overrun counter) and is deliberately
//     not changed in the same breath as the speed advertisement.
//   - Every other bit in each register is preserved: they are the PHY's
//     business, and a driver that zeroes what it does not understand is
//     how vendor-specific defaults get quietly destroyed.
bool r8125_phy_plan_advertisement(const r8125_phy_adv_t* have,
                                  r8125_phy_adv_t* want,
                                  bool advertise_2500);

// ── Rendering abilities for the log ─────────────────────────────────────
//
// One bitmask for "a set of link modes", whichever register pair it came
// from, so the boot log can say "we offer X, they offer Y" in one
// vocabulary. Ours comes from ANAR/GBCR/0xA5D4; theirs from ANLPAR/GBSR/
// 0xA5D6 — different bit positions, same meaning, hence two readers.
#define R8125_ABILITY_10H    0x01
#define R8125_ABILITY_10F    0x02
#define R8125_ABILITY_100H   0x04
#define R8125_ABILITY_100F   0x08
#define R8125_ABILITY_1000H  0x10
#define R8125_ABILITY_1000F  0x20
#define R8125_ABILITY_2500F  0x40

uint8_t r8125_phy_abilities_ours(uint16_t anar, uint16_t gbcr, uint16_t adv2500);
uint8_t r8125_phy_abilities_partner(uint16_t anlpar, uint16_t gbsr, uint16_t lpa2500);

// The speed a correct negotiation lands on: the fastest mode both sides
// offer. 0 when they share nothing — which is also what an empty partner
// page yields, so a caller cannot mistake "not reported" for "10 Mbit".
// A link running BELOW this number is a negotiation that went wrong (a
// marginal pair, a partner that fumbled a page), and one restart is the
// cure every OS applies before blaming the cable.
uint32_t r8125_phy_best_common_mbps(uint8_t ours, uint8_t theirs);

// "10H/10F/100H/100F/1000F", or "none". Writes at most `cap` bytes
// including the terminator and returns the length it wrote (not counting
// the terminator). `cap` of R8125_ABILITY_TEXT_CAP always fits.
#define R8125_ABILITY_TEXT_CAP 40
uint32_t r8125_phy_abilities_text(uint8_t abilities, char* out, uint32_t cap);

#endif // R8125_PHY_H
