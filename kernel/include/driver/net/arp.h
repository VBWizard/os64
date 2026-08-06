#ifndef ARP_H
#define ARP_H

// arp.h — Address Resolution Protocol (RFC 826, November 1982).
//
// The problem ARP solves is the seam between two address worlds: IPv4 knows
// WHERE a machine is (10.0.2.2, routable, hierarchical), ethernet knows WHO
// to hand a frame to (52:55:0a:00:02:02, flat, burned into hardware). ARP
// is the question that bridges them — "who has 10.0.2.2? tell 10.0.2.15" —
// broadcast to everyone, answered by the owner, remembered in a cache.
//
// David Plummer's 1982 design is self-describing on purpose: the packet
// carries its OWN field sizes (hlen/plen), so the same 28 bytes that mapped
// IP onto 3Mbit experimental ethernet still maps it onto hardware nobody
// had imagined — which is why the format has never needed a version 2.
//
// This is also the first protocol STATE MACHINE in os64 — a deliberately
// tiny one (a cache with expiry) serving as the dry run for TCP's giant one
// (NETWORK.md Phase 4). The packet itself, after ethernet strips its header:
//
//   [0]  htype (2)   hardware type: 1 = ethernet
//   [2]  ptype (2)   protocol type: 0x0800 = IPv4 (yes, the ethertype value)
//   [4]  hlen  (1)   hardware address length: 6
//   [5]  plen  (1)   protocol address length: 4
//   [6]  oper  (2)   1 = request, 2 = reply
//   [8]  sender MAC (6), sender IP (4)
//   [18] target MAC (6), target IP (4)   — target MAC zero in a request:
//                                          that IS the question being asked

#include <stdint.h>
#include <stdbool.h>
#include "driver/net/net_device.h"

#define ARP_PKT_LEN     28
#define ARP_OPER_REQUEST 1
#define ARP_OPER_REPLY   2

// Cache sizing. 16 entries is generous for this OS's social circle (a
// gateway, a host, some test fixtures); eviction is oldest-first when full.
// TTL of 60 seconds is the Linux-ish choice (classic BSD held entries for
// 20 minutes; modern stacks re-verify much sooner so a machine that changes
// NICs — or a failover that moves an IP — isn't unreachable for 20 minutes).
#define ARP_CACHE_SIZE       16
#define ARP_CACHE_TTL_TICKS  (60ULL * TICKS_PER_SECOND)

typedef struct arp_stats
{
	uint64_t requests_sent;      // questions we asked
	uint64_t replies_received;   // answers we got
	uint64_t requests_received;  // questions asked OF us (target IP = ours)
	uint64_t replies_sent;       // answers we gave — the responder half
	uint64_t learned;            // cache inserts/refreshes, either direction
	uint64_t evicted;            // cache full, oldest entry sacrificed
	uint64_t malformed;          // wrong htype/ptype/sizes, or truncated
} arp_stats_t;
extern arp_stats_t kArpStats;

// Inbound ARP packet (ethernet header already stripped). Learns the sender,
// answers requests aimed at our IP. Runs in RX-handler context: quick,
// no sleeping, irqsave locking inside.
void arp_input(net_device_t* dev, const void* pkt, uint16_t length);

// Cache-only lookup: true and mac_out filled if `ip` is cached and fresh.
// Never touches the wire — pair with arp_send_request and re-ask, which is
// exactly what ipv4_send does (see the first-packet note in ipv4.c).
bool arp_lookup(uint32_t ip, uint8_t mac_out[NET_MAC_LEN]);

// Empty the whole cache — every next send starts from a cold neighbor
// table. What the tests use to make "the 60s TTL just expired" happen on
// demand instead of once a minute; the future netstat-alike's flush verb.
void arp_cache_flush(void);

// Broadcast a who-has for `target_ip`. The answer arrives asynchronously
// through arp_input into the cache; there is deliberately no blocking
// resolve at this layer (RX context must never wait on the wire).
int32_t arp_send_request(net_device_t* dev, uint32_t target_ip);

#endif // ARP_H
