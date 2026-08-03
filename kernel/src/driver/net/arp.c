// arp.c — RFC 826: the question, the answer, and the memory of answers.
//
// Three moving parts, none big:
//   the CACHE     — ip→mac with a freshness stamp, spinlock-guarded because
//                   RX context inserts while task context looks up
//   the RESPONDER — someone broadcast "who has OUR ip?"; we answer, because
//                   an unanswerable machine is an unreachable machine
//   the ASKER     — arp_send_request, used by ipv4_send on a cache miss
//
// The learning rule below is RFC 826's own merge algorithm (the RFC calls
// it ?do I already know you? / merge_flag): harvest the sender mapping from
// EVERY valid ARP we see — requests and replies alike. A request aimed at
// us almost always precedes traffic FROM that sender, so learning from the
// question saves the counter-question ("who are YOU?") a lesser design
// would broadcast a moment later.

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"          // kTicksSinceStart — the cache's clock
#include "serial_logging.h"
#include "memcpy.h"
#include "spinlock.h"
#include "CONFIG.h"
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/ethernet.h"
#include "driver/net/arp.h"
#include "driver/net/ipv4.h"   // kNetIPv4Address — the "is that us?" test

arp_stats_t kArpStats;

typedef struct arp_entry
{
	uint32_t ip;                  // host order, 0 = slot empty
	uint8_t  mac[NET_MAC_LEN];
	uint64_t stamp;               // kTicksSinceStart at learn time
} arp_entry_t;

static arp_entry_t s_cache[ARP_CACHE_SIZE];
// Guards the cache only. RX context (processSignals poll) inserts while a
// task-context caller (ipv4_send, the tests) looks up — irqsave on both
// sides, held for a handful of compares, never across the wire.
static spinlock_t s_cache_lock;

// ── The cache ───────────────────────────────────────────────────────────────
static void arp_cache_insert(uint32_t ip, const uint8_t mac[NET_MAC_LEN])
{
	uint64_t irqflags = spinlock_acquire_irqsave(&s_cache_lock);

	// Refresh if known; otherwise take an empty slot; otherwise evict the
	// stalest. One linear pass finds all three candidates — at 16 entries,
	// cleverness would cost more to read than it saves to run.
	arp_entry_t* target = NULL;
	arp_entry_t* oldest = &s_cache[0];
	for (int i = 0; i < ARP_CACHE_SIZE; i++)
	{
		if (s_cache[i].ip == ip) { target = &s_cache[i]; break; }
		if (s_cache[i].ip == 0 && target == NULL) target = &s_cache[i];
		if (s_cache[i].stamp < oldest->stamp) oldest = &s_cache[i];
	}
	if (target == NULL)
	{
		target = oldest;
		kArpStats.evicted++;
	}

	target->ip = ip;
	memcpy(target->mac, (void*)mac, NET_MAC_LEN);
	target->stamp = kTicksSinceStart;
	kArpStats.learned++;

	spinlock_release_irqrestore(&s_cache_lock, irqflags);
}

void arp_cache_flush(void)
{
	uint64_t irqflags = spinlock_acquire_irqsave(&s_cache_lock);
	for (int i = 0; i < ARP_CACHE_SIZE; i++)
		s_cache[i].ip = 0;   // same "slot empty" convention expiry uses
	spinlock_release_irqrestore(&s_cache_lock, irqflags);
}

bool arp_lookup(uint32_t ip, uint8_t mac_out[NET_MAC_LEN])
{
	bool hit = false;
	uint64_t irqflags = spinlock_acquire_irqsave(&s_cache_lock);
	for (int i = 0; i < ARP_CACHE_SIZE; i++)
	{
		if (s_cache[i].ip != ip)
			continue;
		// Expiry is lazy — checked at lookup, no sweeper task. A stale
		// entry costs nothing sitting there; it only matters the moment
		// someone asks, so that's the moment we judge it.
		if (kTicksSinceStart - s_cache[i].stamp > ARP_CACHE_TTL_TICKS)
		{
			s_cache[i].ip = 0;   // expired: free the slot, report a miss
			break;
		}
		memcpy(mac_out, s_cache[i].mac, NET_MAC_LEN);
		hit = true;
		break;
	}
	spinlock_release_irqrestore(&s_cache_lock, irqflags);
	return hit;
}

// ── Packet building ─────────────────────────────────────────────────────────
// One builder for both operations — a reply IS a request with the blanks
// filled in and the operation flipped, which is most of why RFC 826 fits
// in 28 bytes.
static void arp_build(uint8_t pkt[ARP_PKT_LEN], net_device_t* dev, uint16_t oper,
                      const uint8_t target_mac[NET_MAC_LEN], uint32_t target_ip)
{
	net_write16(pkt + 0, 1);              // htype: ethernet
	net_write16(pkt + 2, ETH_TYPE_IPV4);  // ptype: IPv4 (shares the ethertype registry)
	pkt[4] = NET_MAC_LEN;                 // hlen
	pkt[5] = 4;                           // plen
	net_write16(pkt + 6, oper);
	memcpy(pkt + 8, dev->mac, NET_MAC_LEN);          // sender: always us
	net_write32(pkt + 14, kNetIPv4Address);
	memcpy(pkt + 18, (void*)target_mac, NET_MAC_LEN);
	net_write32(pkt + 24, target_ip);
}

int32_t arp_send_request(net_device_t* dev, uint32_t target_ip)
{
	uint8_t pkt[ARP_PKT_LEN];
	// Target MAC all-zeros: the blank on the form. (Some stacks send
	// ff:ff:ff:ff:ff:ff there; RFC 826 says the field is ignored in a
	// request. Zeros make the pcap read as the question it is.)
	static const uint8_t unknown_mac[NET_MAC_LEN] = {0, 0, 0, 0, 0, 0};
	arp_build(pkt, dev, ARP_OPER_REQUEST, unknown_mac, target_ip);
	kArpStats.requests_sent++;
	printd(DEBUG_NET, "arp: who has %u.%u.%u.%u?\n", NET_IPV4_OCTETS(target_ip));
	return eth_send(dev, kEthBroadcastMAC, ETH_TYPE_ARP, pkt, ARP_PKT_LEN);
}

// ── Receive ─────────────────────────────────────────────────────────────────
void arp_input(net_device_t* dev, const void* pkt, uint16_t length)
{
	const uint8_t* p = (const uint8_t*)pkt;

	// Validate the self-description. We speak exactly one dialect —
	// ethernet/IPv4 — and RFC 826's forward-compatibility means politely
	// ignoring the others (counted, not parsed).
	if (length < ARP_PKT_LEN ||
	    net_read16(p + 0) != 1 || net_read16(p + 2) != ETH_TYPE_IPV4 ||
	    p[4] != NET_MAC_LEN || p[5] != 4)
	{
		kArpStats.malformed++;
		return;
	}

	uint16_t oper       = net_read16(p + 6);
	const uint8_t* smac = p + 8;
	uint32_t sender_ip  = net_read32(p + 14);
	uint32_t target_ip  = net_read32(p + 24);

	// Learn the sender (the merge rule — see the file header). sender_ip 0
	// is an "ARP probe" (RFC 5227, duplicate-address detection): a machine
	// asking about an address while claiming none. Nothing to learn there.
	if (sender_ip != 0)
		arp_cache_insert(sender_ip, smac);

	if (oper == ARP_OPER_REQUEST)
	{
		if (target_ip != kNetIPv4Address)
			return;   // someone else's question; we learned the asker, done

		// The responder half: answer with our MAC, unicast back to the
		// asker (their address was in the question — no broadcast needed).
		kArpStats.requests_received++;
		uint8_t reply[ARP_PKT_LEN];
		arp_build(reply, dev, ARP_OPER_REPLY, smac, sender_ip);
		if (eth_send(dev, smac, ETH_TYPE_ARP, reply, ARP_PKT_LEN) == 0)
			kArpStats.replies_sent++;
		printd(DEBUG_NET, "arp: told %u.%u.%u.%u that %u.%u.%u.%u is us\n",
		       NET_IPV4_OCTETS(sender_ip), NET_IPV4_OCTETS(target_ip));
	}
	else if (oper == ARP_OPER_REPLY)
	{
		// The insert above already banked the answer; this is bookkeeping.
		kArpStats.replies_received++;
		printd(DEBUG_NET, "arp: %u.%u.%u.%u is at %02x:%02x:%02x:%02x:%02x:%02x\n",
		       NET_IPV4_OCTETS(sender_ip),
		       smac[0], smac[1], smac[2], smac[3], smac[4], smac[5]);
	}
	else
		kArpStats.malformed++;   // RARP and friends: not spoken here
}
