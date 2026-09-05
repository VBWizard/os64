#ifndef IPV4_H
#define IPV4_H

// ipv4.h — the Internet Protocol, version 4 (RFC 791, September 1981).
//
// This is the layer that made "the internet" a lowercase noun: everything
// below it is one link, everything above it is one conversation, and IP's
// whole job is the hop in between — take a packet, decide the next machine
// in the right direction, hand it to the link layer. os64 is a HOST, not a
// router (we never forward), so our slice of that job is small: validate
// what arrives for us, demux it by protocol, and for departures decide the
// one routing question a host has — "is the destination on my link, or does
// it go via the gateway?"
//
// The 20-byte header (options are legal but nobody sane sends them):
//
//   [0]  version/IHL     4 | header length in 32-bit words (5 = 20 bytes)
//   [1]  TOS             quality-of-service dreams; ignored for 40 years
//   [2]  total length    header + payload — how ethernet padding gets trimmed
//   [4]  identification  fragment reassembly key (we number ours anyway)
//   [6]  flags/fragment  DF / MF / offset — see the fragment stance below
//   [8]  TTL             hop budget; 64 spent one per router, so a stray
//                        packet dies in the wild instead of looping forever
//   [9]  protocol        the demux key: 1 = ICMP, 6 = TCP, 17 = UDP
//   [10] header checksum RFC 1071, header only — payloads guard themselves
//   [12] src IP (4), [16] dst IP (4)
//
// FRAGMENT STANCE (NETWORK.md Phase 2, stated in the constitution): v1
// sends nothing that needs fragmenting (we set DF) and drops fragmented
// arrivals LOUDLY — counted and logged, never silent. On modern paths
// fragmentation is a fossil (path-MTU discovery + DF won; middleboxes drop
// fragments anyway); reassembly is booked as a DEBT row, not pretended.

#include <stdint.h>
#include "driver/net/net_device.h"

#define IPV4_HDR_MIN     20
#define IPV4_PROTO_ICMP  1
#define IPV4_PROTO_TCP   6    // Phase 4 — named now so the demux reads complete
#define IPV4_PROTO_UDP   17   // Phase 3

// ── Configuration ───────────────────────────────────────────────────────────
// Host-order addresses (see net_wire.h for why host-order). Static config,
// parsed by ipv4_config_init from the IP=/GW=/MASK= cmdline tokens; the
// defaults are the 10.0.2.x NAT convention BOTH QEMU slirp and VirtualBox
// NAT use, so every hypervisor boot is network-live with zero setup. DHCP
// (Phase 3, UDP's first customer) will supersede all three — real hardware
// on a real LAN wants a leased address, not a hand-typed one.
extern uint32_t kNetIPv4Address;
extern uint32_t kNetIPv4Gateway;
extern uint32_t kNetIPv4Netmask;

// The raw cmdline strings behind the addresses (ipv4.c owns them; the
// kernel_commandline.c table fills them). Exposed because "was IP= given
// at all?" is itself config: an empty kNetIPString is what elects DHCP
// (see the policy note in dhcp.h and the call site in kernel.c).
extern char kNetIPString[];
extern char kNetGWString[];
extern char kNetMaskString[];

typedef struct ipv4_stats
{
	uint64_t rx_delivered;         // validated, addressed to us, demuxed
	uint64_t rx_bad_version;       // not 4 (IPv6 arrives via ethertype 0x86DD, not here)
	uint64_t rx_bad_checksum;      // header failed RFC 1071
	uint64_t rx_truncated;         // lengths don't add up
	uint64_t rx_fragment_dropped;  // the loud fragment drop (see stance above)
	uint64_t rx_not_for_us;        // unicast to someone else's address
	uint64_t rx_unknown_proto;     // protocol we don't parse yet
	uint64_t tx_sent;
	uint64_t tx_awaiting_arp;      // DROPPED: next hop unresolved and no room to hold it
	// PARKED: next hop unresolved, packet held in the ARP waiting room
	// and released when the reply landed (ipv4.c). The pair reads as a
	// score: parked is the fix working, awaiting is it running out of
	// room — and they were ONE counter until 2026-08-16, when the
	// difference between "delayed" and "lost" started mattering.
	uint64_t tx_parked_for_arp;
	uint64_t tx_too_big;           // payload over MTU (we refuse; we never fragment)
	uint64_t tx_errors;            // lower layer refused
} ipv4_stats_t;
extern ipv4_stats_t kIPv4Stats;

// Parse IP=/GW=/MASK= (or take the NAT-convention defaults). Called by
// init_net_stack before any NIC exists.
void ipv4_config_init(void);

// Inbound IPv4 packet, ethernet header already stripped. RX-handler
// context: quick, no sleeping.
void ipv4_input(net_device_t* dev, const void* pkt, uint16_t length);

// A neighbor's MAC just became known — release whatever packet was waiting
// on it. Called by arp.c the instant the cache learns an address, from any
// source (a reply to our question, or a neighbor asking one of its own).
// Cheap and silent when nothing was waiting, which is nearly always.
void ipv4_arp_resolved(net_device_t* dev, uint32_t ip);

// Wrap `payload` in an IPv4 header and send it toward dst_ip. Handles the
// host routing decision (on-link vs gateway) and ARP resolution. Returns
// 0 = handed to the wire; -2 = next hop not yet resolved (an ARP query was
// just broadcast — retry shortly; see the first-packet note at the
// implementation); other negatives = refused.
int32_t ipv4_send(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
                  const void* payload, uint16_t length);

// Same, with an explicit SOURCE address. Exists for exactly one honest
// customer: DHCP, which must speak from 0.0.0.0 before it owns an address
// (RFC 2131 requires it — a client claiming an address it hasn't leased
// would be lying to the wire). Everyone else uses ipv4_send, which fills
// in kNetIPv4Address.
int32_t ipv4_send_from(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                       uint8_t protocol, const void* payload, uint16_t length);

// Is the next hop toward dst_ip resolved — will a send go to the wire now,
// or be parked for ARP? The two share ipv4_send's -2 (a driver refusing a
// full ring says -2 too), and a sender keeping sequence books has to know
// which it got: a parked packet goes when ARP answers and stays on the
// books; a refused one never left and must not.
bool ipv4_next_hop_known(net_device_t* dev, uint32_t dst_ip);

#endif // IPV4_H
