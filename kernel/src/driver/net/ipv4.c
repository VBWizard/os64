// ipv4.c — RFC 791 for a host: validate in, route out, never forward.
//
// The receive path is mostly a bouncer — five ways a packet can be wrong
// before anyone above needs to think about it, each with its own counter.
// The transmit path is mostly a form letter — twenty bytes of header whose
// only interesting field decision is "next hop: the destination itself, or
// the gateway?" That question (one subnet-mask AND plus one compare) is the
// entire routing table of a single-homed host, and it's worth seeing it
// this small once before ever meeting a real router's version of it.

#include <stdint.h>
#include <stdbool.h>
#include "serial_logging.h"
#include "memcpy.h"
#include "CONFIG.h"
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/net_checksum.h"
#include "driver/net/ethernet.h"
#include "driver/net/arp.h"
#include "spinlock.h"
#include "kernel.h"   // kTicksSinceStart — the waiting room's expiry clock
#include "driver/net/ipv4.h"
#include "driver/net/icmp.h"
#include "driver/net/udp.h"
#include "driver/net/tcp.h"

ipv4_stats_t kIPv4Stats;

// ── Configuration ───────────────────────────────────────────────────────────
// The kernel_commandline.c table fills these strings from IP=/GW=/MASK=
// tokens; empty means "use the default". Defaults are the 10.0.2.x NAT
// convention shared by QEMU slirp AND VirtualBox NAT (guest .15, gateway
// .2) — the entire hypervisor era of this OS is network-live out of the
// box, and DHCP retires the whole arrangement in Phase 3.
char kNetIPString[20]   = "";
char kNetGWString[20]   = "";
char kNetMaskString[20] = "";

uint32_t kNetIPv4Address;
uint32_t kNetIPv4Gateway;
uint32_t kNetIPv4Netmask;

// Parse a dotted quad ("10.0.2.15") to a host-order address; `fallback` on
// any malformation. Strict enough to matter (each octet 0-255, exactly four
// of them), small enough to read. Lives here rather than a string library
// because dotted-quad is WIRE-ADJACENT syntax — when Phase 3's rulings give
// userland an address type, libos64 gets the real parser and this stays the
// kernel's private bootstrap tool.
static uint32_t ipv4_parse(const char* s, uint32_t fallback)
{
	if (s == NULL || *s == '\0')
		return fallback;

	uint32_t addr = 0;
	for (int octet = 0; octet < 4; octet++)
	{
		uint32_t value = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9' && digits < 3)
		{
			value = value * 10 + (uint32_t)(*s - '0');
			s++;
			digits++;
		}
		if (digits == 0 || value > 255)
			return fallback;
		addr = (addr << 8) | value;
		if (octet < 3)
		{
			if (*s != '.')
				return fallback;
			s++;
		}
	}
	if (*s != '\0')
		return fallback;
	return addr;
}

void ipv4_config_init(void)
{
	kNetIPv4Address = ipv4_parse(kNetIPString,   NET_IPV4(10, 0, 2, 15));
	kNetIPv4Gateway = ipv4_parse(kNetGWString,   NET_IPV4(10, 0, 2, 2));
	kNetIPv4Netmask = ipv4_parse(kNetMaskString, NET_IPV4(255, 255, 255, 0));
}

// ── Receive ─────────────────────────────────────────────────────────────────
void ipv4_input(net_device_t* dev, const void* pkt, uint16_t length)
{
	const uint8_t* p = (const uint8_t*)pkt;

	if (length < IPV4_HDR_MIN)
	{
		kIPv4Stats.rx_truncated++;
		return;
	}
	if ((p[0] >> 4) != 4)
	{
		kIPv4Stats.rx_bad_version++;
		return;
	}

	// IHL is in 32-bit words; >5 means options are present. We don't act
	// on any option (a host rarely must), but we honor the length so the
	// payload starts where the sender said it does.
	uint16_t ihl = (uint16_t)((p[0] & 0x0F) * 4);
	if (ihl < IPV4_HDR_MIN || ihl > length)
	{
		kIPv4Stats.rx_truncated++;
		return;
	}

	// RFC 1071's self-property: a header containing its own checksum sums
	// to zero. One call, no field-zeroing dance (see net_checksum.h).
	if (net_checksum(p, ihl) != 0)
	{
		kIPv4Stats.rx_bad_checksum++;
		printd(DEBUG_NET, "ipv4: bad header checksum from %u.%u.%u.%u — dropped\n",
		       NET_IPV4_OCTETS(net_read32(p + 12)));
		return;
	}

	// total_length is how the wire's padding comes off: ethernet pads
	// frames to 60 bytes (eth_send does it too), so a 28-byte ping arrives
	// inside 46 bytes of payload and THIS field is what knows the
	// difference. Trust it only downward — never past what actually arrived.
	uint16_t total = net_read16(p + 2);
	if (total < ihl || total > length)
	{
		kIPv4Stats.rx_truncated++;
		return;
	}

	// The fragment stance (NETWORK.md): drop loudly. MF set or a nonzero
	// offset means this is a piece of something; reassembly is a booked
	// DEBT, not a silent pretense. (0x2000 = MF, 0x1FFF = offset field;
	// bit 0x4000 = DF is fine and expected — we set it ourselves.)
	uint16_t frag = net_read16(p + 6);
	if (frag & 0x3FFF)
	{
		kIPv4Stats.rx_fragment_dropped++;
		printd(DEBUG_NET, "ipv4: fragment from %u.%u.%u.%u dropped (reassembly: DEBT)\n",
		       NET_IPV4_OCTETS(net_read32(p + 12)));
		return;
	}

	uint32_t src = net_read32(p + 12);
	uint32_t dst = net_read32(p + 16);

	// Are we the addressee? Three yeses: our unicast address, the limited
	// broadcast (255.255.255.255 — "this link, everyone", how DHCP DISCOVER
	// will arrive addressed to a machine that has no address yet), or our
	// subnet's directed broadcast. Anything else unicast is a NAT/switch
	// mistake — counted, dropped, never forwarded (hosts don't route).
	uint32_t subnet_bcast = (kNetIPv4Address & kNetIPv4Netmask) | ~kNetIPv4Netmask;
	bool broadcast = (dst == 0xFFFFFFFF) || (dst == subnet_bcast);
	if (dst != kNetIPv4Address && !broadcast)
	{
		kIPv4Stats.rx_not_for_us++;
		return;
	}

	kIPv4Stats.rx_delivered++;
	uint16_t payload_len = (uint16_t)(total - ihl);
	switch (p[9])
	{
		case IPV4_PROTO_ICMP:
			icmp_input(dev, src, broadcast, p + ihl, payload_len);
			break;
		case IPV4_PROTO_UDP:
			// UDP gets the raw src AND dst addresses (not just the
			// broadcast verdict) because its checksum covers a
			// pseudo-header built from them — see udp.c.
			udp_input(dev, src, dst, p + ihl, payload_len);
			break;
		case IPV4_PROTO_TCP:
			// Same pseudo-header reason, same two addresses. (Phase 4
			// filled this case; the default arm below is now reserved for
			// protocols os64 genuinely doesn't speak.)
			tcp_input(dev, src, dst, p + ihl, payload_len);
			break;
		default:
			// Everything we don't speak — visible on a counter, so a
			// premature "why no <protocol>?" has its answer. (Chris caught
			// this comment still claiming UDP during the 2026-08-01
			// review — comments rot one slice at a time.)
			kIPv4Stats.rx_unknown_proto++;
			printd(DEBUG_NET | DEBUG_DETAILED, "ipv4: protocol %u from %u.%u.%u.%u not yet spoken\n",
			       p[9], NET_IPV4_OCTETS(src));
			break;
	}
}

// ── Transmit ────────────────────────────────────────────────────────────────
int32_t ipv4_send(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
                  const void* payload, uint16_t length)
{
	return ipv4_send_from(dev, kNetIPv4Address, dst_ip, protocol, payload, length);
}

// ── The ARP waiting room ────────────────────────────────────────────────────
//
// One packet per neighbor, held only until its MAC arrives. Deliberately
// tiny: this is not a queue, it is the fix for FIRST-packet loss. A second
// packet to the same unresolved neighbor replaces the first, because by the
// time two are outstanding the sender is a protocol with its own
// retransmission and does not need us inventing a second one underneath it.
//
// Bounded four ways on purpose — an unbounded holding area for packets
// addressed to a machine that may not exist is a memory leak with a network
// interface: at most IPV4_PENDING_MAX neighbors, one packet each, MTU-sized,
// and every slot expires.
#define IPV4_PENDING_MAX      4
#define IPV4_PENDING_TTL_TICKS (3 * TICKS_PER_SECOND)

typedef struct
{
	uint32_t next_hop;    // 0 = free slot
	uint32_t src_ip;
	uint32_t dst_ip;
	uint64_t stamp;       // parked at; expires so a silent neighbor cannot leak
	uint16_t length;
	uint8_t  protocol;
	uint8_t  payload[NET_FRAME_MAX];
} ipv4_pending_t;

static ipv4_pending_t s_pending[IPV4_PENDING_MAX];
static spinlock_t s_pending_lock;

// Hold `payload` until next_hop resolves. The CALLER IS STILL TOLD -2 —
// see the essay at the call site. Best effort: false means it could not hold
// the packet (oversized, or every slot busy with another neighbor).
static bool ipv4_park_pending(net_device_t* dev, uint32_t next_hop,
                              uint32_t src_ip, uint32_t dst_ip,
                              uint8_t protocol, const void* payload,
                              uint16_t length)
{
	(void)dev;
	if (length > sizeof(((ipv4_pending_t*)0)->payload))
		return false;   // too big to hold; the caller is told -2 either way

	uint64_t flags = spinlock_acquire_irqsave(&s_pending_lock);

	// Pick a slot: ours if this neighbor already has one (replace — see
	// above), else a free one, else the oldest expired one. Expiry is lazy
	// like the ARP cache's, and for the same reason: a stale entry costs
	// nothing sitting there, it only matters when someone needs the space.
	int slot = -1, freeSlot = -1, oldest = -1;
	for (int i = 0; i < IPV4_PENDING_MAX; i++)
	{
		if (s_pending[i].next_hop == next_hop) { slot = i; break; }
		if (s_pending[i].next_hop == 0 && freeSlot < 0) freeSlot = i;
		if (s_pending[i].next_hop != 0 &&
		    kTicksSinceStart - s_pending[i].stamp > IPV4_PENDING_TTL_TICKS &&
		    oldest < 0)
			oldest = i;
	}
	if (slot < 0) slot = (freeSlot >= 0) ? freeSlot : oldest;
	if (slot < 0)
	{
		// Every slot holds a live wait for a different neighbor. Give up
		// on holding this one rather than evicting somebody else's — the
		// caller was told -2 either way, so it loses nothing it ever had.
		spinlock_release_irqrestore(&s_pending_lock, flags);
		return false;
	}

	s_pending[slot].next_hop = next_hop;
	s_pending[slot].src_ip   = src_ip;
	s_pending[slot].dst_ip   = dst_ip;
	s_pending[slot].protocol = protocol;
	s_pending[slot].length   = length;
	s_pending[slot].stamp    = kTicksSinceStart;
	memcpy(s_pending[slot].payload, payload, length);

	spinlock_release_irqrestore(&s_pending_lock, flags);
	kIPv4Stats.tx_parked_for_arp++;
	return true;
}

// A MAC just arrived. Called from arp.c the moment the cache learns one —
// including from an unsolicited reply or a neighbor's own request, since
// any of those teaches us what we were waiting for.
//
// The slot is CLAIMED AND RELEASED BEFORE the send: ipv4_send_from will
// look up ARP again (a hit now), and re-entering the park path while
// holding its own lock would be a deadlock rather than a bug report.
void ipv4_arp_resolved(net_device_t* dev, uint32_t ip)
{
	ipv4_pending_t held;
	bool have = false;

	uint64_t flags = spinlock_acquire_irqsave(&s_pending_lock);
	for (int i = 0; i < IPV4_PENDING_MAX; i++)
	{
		if (s_pending[i].next_hop != ip)
			continue;
		if (kTicksSinceStart - s_pending[i].stamp <= IPV4_PENDING_TTL_TICKS)
		{
			held = s_pending[i];
			have = true;
		}
		s_pending[i].next_hop = 0;   // claimed (or expired) either way
		break;
	}
	spinlock_release_irqrestore(&s_pending_lock, flags);

	if (!have)
		return;

	printd(DEBUG_NET, "ipv4: ARP for %u.%u.%u.%u resolved — releasing the packet it was holding\n",
	       NET_IPV4_OCTETS(ip));
	ipv4_send_from(dev, held.src_ip, held.dst_ip, held.protocol,
	               held.payload, held.length);
}

int32_t ipv4_send_from(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                       uint8_t protocol, const void* payload, uint16_t length)
{
	return ipv4_send_from_ex(dev, src_ip, dst_ip, protocol, payload, length, NULL);
}

int32_t ipv4_send_from_ex(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                          uint8_t protocol, const void* payload, uint16_t length,
                          ipv4_tx_t* how)
{
	if ((uint32_t)length + IPV4_HDR_MIN > dev->mtu)
	{
		// Over-MTU means fragmenting, and we don't (DF world, see the
		// header stance). Refused here, loudly, instead of half-sent.
		kIPv4Stats.tx_too_big++;
		if (how) *how = IPV4_TX_INVALID;
		return -1;
	}

	// The host routing decision, complete: same subnet = hand it to the
	// destination directly; different subnet = hand it to the gateway and
	// make it the gateway's problem. Broadcasts skip ARP entirely — you
	// don't ask "who has everyone?"
	uint32_t subnet_bcast = (kNetIPv4Address & kNetIPv4Netmask) | ~kNetIPv4Netmask;
	bool broadcast = (dst_ip == 0xFFFFFFFF) || (dst_ip == subnet_bcast);
	uint8_t next_hop_mac[NET_MAC_LEN];
	if (broadcast)
		memcpy(next_hop_mac, (void*)kEthBroadcastMAC, NET_MAC_LEN);
	else
	{
		bool on_link = ((dst_ip ^ kNetIPv4Address) & kNetIPv4Netmask) == 0;
		uint32_t next_hop = on_link ? dst_ip : kNetIPv4Gateway;
		if (!arp_lookup(next_hop, next_hop_mac))
		{
			// Cache miss. This USED to fire the query and drop the packet
			// — the honest 1982 behavior, and the reason "why does the
			// first ping always fail?" was a generation's introduction to
			// ARP. The DEBTS row that booked the upgrade named the exact
			// condition that would justify paying it: "when first-packet
			// loss annoys a caller that can't retry-loop."
			//
			// That caller arrived on 2026-08-16. os64get opens ONE TCP
			// CONNECTION PER FILE, and a dropped SYN does not look like a
			// dropped packet to whoever is watching — it looks like a ten
			// second hang and then "cannot reach the host", because
			// tcp_conn_dial has nothing to retransmit yet and simply waits
			// out its handshake timeout. Refreshing fifty binaries would
			// meet an expired cache over and over.
			//
			// So we PARK it: one packet per neighbor, sent the moment the
			// reply lands (ipv4_arp_resolved, called from arp.c). Success
			// is the honest answer to the caller — the packet is accepted
			// for delivery, and if ARP never answers it is dropped exactly
			// like any other packet the wire ate, which every protocol
			// above here already copes with.
			// AND WE STILL RETURN -2, which is the whole trick. The
			// first version of this returned SUCCESS on the theory that
			// "accepted for delivery" is the honest answer and every
			// caller should carry its own timeout — which is how real
			// stacks behave, and which BROKE THIS OS within one boot.
			// Callers here had been written against the old contract and
			// used -2 as "no route, stop now"; told success instead, they
			// sailed past the point where they used to give up and sat
			// waiting for replies that were never coming. A test suite
			// that used to fail fast hung instead.
			//
			// So the parking is a BONUS, not a promise. Nobody's contract
			// changes: every existing caller sees exactly what it saw
			// before and behaves exactly as it did. The packet simply also
			// gets a second chance it never used to get — and TCP, which
			// ignores this return value entirely (tcp.c's tcp_send), is
			// the one that needed it, because a dropped SYN is not a
			// dropped packet to a human: it is a ten second hang and then
			// "cannot reach the host".
			//
			// The cost is a possible DUPLICATE when a retrying caller and
			// the released packet both go out. ICMP and UDP are defined to
			// tolerate duplicates, TCP discards them by sequence number,
			// and a duplicate on the first packet after an idle gap is a
			// far smaller thing than either a hang or a lost connection.
			bool parked = ipv4_park_pending(dev, next_hop, src_ip, dst_ip,
			                                protocol, payload, length);
			arp_send_request(dev, next_hop);
			if (!parked)
				kIPv4Stats.tx_awaiting_arp++;
			if (how) *how = IPV4_TX_PARKED;   // held, or dropped as a first packet: the wire's either way
			return -2;
		}
	}

	// The form letter. Identification numbers every departure — nothing
	// reassembles DF'd packets, but a monotonic id makes pcap reading and
	// duplicate-spotting humane, and it costs one increment. (Relaxed
	// atomicity: two cores sharing an id would bother no one, but the
	// counter also feeds tx_sent bookkeeping, so keep it exact.)
	static uint16_t s_ident = 0;
	uint16_t ident = __sync_fetch_and_add(&s_ident, 1);

	uint8_t pkt[NET_FRAME_MAX - ETH_HDR_LEN];
	pkt[0] = 0x45;                                  // version 4, IHL 5 (no options, ever, from us)
	pkt[1] = 0;                                     // TOS: the field of unfulfilled dreams
	net_write16(pkt + 2, (uint16_t)(IPV4_HDR_MIN + length));
	net_write16(pkt + 4, ident);
	net_write16(pkt + 6, 0x4000);                   // DF: we do not fragment, so say so
	pkt[8] = 64;                                    // TTL: the classic default hop budget
	pkt[9] = protocol;
	net_write16(pkt + 10, 0);                       // checksum: zero while computing...
	net_write32(pkt + 12, src_ip);
	net_write32(pkt + 16, dst_ip);
	net_write16(pkt + 10, net_checksum(pkt, IPV4_HDR_MIN));   // ...then the truth
	memcpy(pkt + IPV4_HDR_MIN, payload, length);

	int32_t rc = eth_send(dev, next_hop_mac, ETH_TYPE_IPV4, pkt,
	                      (uint16_t)(IPV4_HDR_MIN + length));
	if (rc == 0)
		kIPv4Stats.tx_sent++;
	else
		kIPv4Stats.tx_errors++;
	if (how) *how = (rc == 0) ? IPV4_TX_SENT : IPV4_TX_DROPPED;
	return rc;
}
