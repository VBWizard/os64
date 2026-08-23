// dhcp.c — the lease negotiation, event-driven end to end.
//
// Shape of the machine: dhcp_start() fires the opening DISCOVER; from
// there everything happens in two callbacks — dhcp_rx (bound to UDP port
// 68) advances the state on OFFER/ACK/NAK, and dhcp_poll (a processSignals
// rider) resends when the wire goes quiet. No task, no blocking, no state
// outside s_dhcp — the whole client is a struct and two functions, which
// is the right weight for a protocol whose entire job is four packets.

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"          // kTicksSinceStart — the retry clock
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — the lease announcement on glass
#include "memcpy.h"
#include "memset.h"
#include "CONFIG.h"
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/ipv4.h"
#include "driver/net/udp.h"
#include "driver/net/dhcp.h"

dhcp_stats_t kDhcpStats;

// BOOTP fixed-field offsets (RFC 951 layout, inherited whole by DHCP).
#define BOOTP_OP      0     // 1 = BOOTREQUEST (client), 2 = BOOTREPLY (server)
#define BOOTP_XID     4     // transaction id — matches answers to questions
#define BOOTP_FLAGS   10    // bit 15 = BROADCAST ("shout the reply")
#define BOOTP_YIADDR  16    // "your address" — the offer itself
#define BOOTP_CHADDR  28    // client hardware address (16 bytes, 6 used)
#define BOOTP_COOKIE  236   // 0x63825363 marks "options follow" (RFC 1497)
#define BOOTP_OPTIONS 240
#define DHCP_PKT_LEN  300   // padded so 1985's relay agents don't drop us

// Option tags we speak.
#define DHCP_OPT_PAD          0
#define DHCP_OPT_SUBNET_MASK  1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS          6     // "Domain Name Server", RFC 2132 §3.8 — a list; we keep the first
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_END          255

// Option 53 message types.
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

#define DHCP_RETRY_TICKS   (2 * TICKS_PER_SECOND)   // 2s of patience per send
#define DHCP_MAX_ATTEMPTS  4                        // per phase, then give up

// The whole client. Single instance like the NIC it serves; no lock —
// dhcp_rx and dhcp_poll both run under the processSignals pass (the UDP
// demux delivers from the same poll that calls dhcp_poll), and dhcp_start
// runs before the scheduler exists. Single-writer by construction; the
// day DHCP runs per-interface, this becomes an array with a lock and the
// comment changes.
static struct
{
	net_device_t* dev;
	uint32_t xid;
	uint32_t offered_ip;        // yiaddr from the OFFER we're pursuing
	uint32_t server_id;         // option 54 — whom to REQUEST from
	uint64_t last_send_tick;
	uint8_t  attempts;
} s_dhcp;

// ── Option walking ──────────────────────────────────────────────────────────
// Find `tag` in the options field; returns pointer to its payload and sets
// *out_len, or NULL. The format is TLV with two exceptions (PAD is a bare
// byte, END stops the world) — 1980s minimalism that still parses in ten
// lines.
static const uint8_t* dhcp_find_option(const uint8_t* pkt, uint16_t length,
                                       uint8_t tag, uint8_t* out_len)
{
	uint16_t off = BOOTP_OPTIONS;
	while (off + 1 < length)
	{
		uint8_t t = pkt[off];
		if (t == DHCP_OPT_PAD) { off++; continue; }
		if (t == DHCP_OPT_END) break;
		uint8_t olen = pkt[off + 1];
		if ((uint32_t)off + 2 + olen > length)
			break;   // truncated option — stop believing the packet
		if (t == tag)
		{
			*out_len = olen;
			return pkt + off + 2;
		}
		off += (uint16_t)(2 + olen);
	}
	return NULL;
}

// ── Packet building ─────────────────────────────────────────────────────────
// DISCOVER and REQUEST are the same 300 bytes with different options —
// one builder, one flag.
static void dhcp_send(uint8_t msg_type)
{
	uint8_t m[DHCP_PKT_LEN];
	memset(m, 0, sizeof(m));
	m[BOOTP_OP] = 1;                             // BOOTREQUEST
	m[1] = 1;  m[2] = 6;                         // htype ethernet, hlen 6
	net_write32(m + BOOTP_XID, s_dhcp.xid);
	// BROADCAST flag: "I can't receive unicast yet — shout." True in
	// spirit (we're claiming no address) and it sidesteps the entire
	// unicast-to-an-address-we-don't-own delivery question in v1.
	net_write16(m + BOOTP_FLAGS, 0x8000);
	memcpy(m + BOOTP_CHADDR, s_dhcp.dev->mac, NET_MAC_LEN);
	net_write32(m + BOOTP_COOKIE, 0x63825363);

	uint16_t o = BOOTP_OPTIONS;
	m[o++] = DHCP_OPT_MSG_TYPE; m[o++] = 1; m[o++] = msg_type;
	if (msg_type == DHCP_MSG_REQUEST)
	{
		// REQUEST names what we want and who offered it — broadcast, so
		// every server that offered hears which one won (RFC 2131 §3.1:
		// the losers reclaim their offers on hearing it).
		m[o++] = DHCP_OPT_REQUESTED_IP; m[o++] = 4;
		net_write32(m + o, s_dhcp.offered_ip); o += 4;
		m[o++] = DHCP_OPT_SERVER_ID; m[o++] = 4;
		net_write32(m + o, s_dhcp.server_id); o += 4;
	}
	m[o++] = DHCP_OPT_END;

	// From nobody, to everybody, per RFC — the placeholder static config
	// is deliberately NOT used as a source here (see the policy in dhcp.h).
	udp_send_from(s_dhcp.dev, 0, NET_IPV4(255, 255, 255, 255),
	              UDP_PORT_DHCP_CLIENT, UDP_PORT_DHCP_SERVER, m, sizeof(m));
	s_dhcp.last_send_tick = kTicksSinceStart;
	s_dhcp.attempts++;
	if (msg_type == DHCP_MSG_DISCOVER) kDhcpStats.discovers_sent++;
	else                               kDhcpStats.requests_sent++;
}

// ── Receive (UDP port 68 handler — RX context: quick, no sleeping) ──────────
static void dhcp_rx(net_device_t* dev, uint32_t src_ip, uint16_t src_port,
                    const void* data, uint16_t length, void* ctx)
{
	(void)dev; (void)src_ip; (void)src_port; (void)ctx;
	const uint8_t* p = (const uint8_t*)data;

	// Believe nothing until it proves itself: a reply, to OUR question,
	// with the cookie. Everything else is counted noise (a busy LAN's
	// port 68 hears other machines' leases all day).
	if (length < BOOTP_OPTIONS || p[BOOTP_OP] != 2 ||
	    net_read32(p + BOOTP_XID) != s_dhcp.xid ||
	    net_read32(p + BOOTP_COOKIE) != 0x63825363)
	{
		kDhcpStats.ignored++;
		return;
	}

	uint8_t olen = 0;
	const uint8_t* opt = dhcp_find_option(p, length, DHCP_OPT_MSG_TYPE, &olen);
	uint8_t msg = (opt && olen >= 1) ? opt[0] : 0;

	if (msg == DHCP_MSG_OFFER && kDhcpStats.state == DHCP_SELECTING)
	{
		kDhcpStats.offers_received++;
		s_dhcp.offered_ip = net_read32(p + BOOTP_YIADDR);
		opt = dhcp_find_option(p, length, DHCP_OPT_SERVER_ID, &olen);
		s_dhcp.server_id = (opt && olen >= 4) ? net_read32(opt) : 0;
		if (s_dhcp.offered_ip == 0 || s_dhcp.server_id == 0)
		{
			kDhcpStats.ignored++;   // an offer of nothing, or from no one
			return;
		}
		printd(DEBUG_NET, "dhcp: offered %u.%u.%u.%u by %u.%u.%u.%u\n",
		       NET_IPV4_OCTETS(s_dhcp.offered_ip), NET_IPV4_OCTETS(s_dhcp.server_id));
		// First reasonable offer wins (RFC lets clients collect and
		// choose; with one NIC on one segment, choosing is ceremony).
		kDhcpStats.state = DHCP_REQUESTING;
		s_dhcp.attempts = 0;
		dhcp_send(DHCP_MSG_REQUEST);
	}
	else if (msg == DHCP_MSG_ACK && kDhcpStats.state == DHCP_REQUESTING)
	{
		kDhcpStats.acks_received++;

		// The lease is real — apply it. yiaddr again (the ACK restates
		// it), mask and router from options, defaults retained for
		// anything the server omitted (a mask-less lease keeps the /24
		// convention rather than inventing one from address class — 1993
		// called CIDR and classes stopped being real).
		kDhcpStats.lease_ip = net_read32(p + BOOTP_YIADDR);
		opt = dhcp_find_option(p, length, DHCP_OPT_SUBNET_MASK, &olen);
		kDhcpStats.lease_mask = (opt && olen >= 4) ? net_read32(opt) : kNetIPv4Netmask;
		opt = dhcp_find_option(p, length, DHCP_OPT_ROUTER, &olen);
		kDhcpStats.lease_gateway = (opt && olen >= 4) ? net_read32(opt) : kNetIPv4Gateway;
		opt = dhcp_find_option(p, length, DHCP_OPT_LEASE_TIME, &olen);
		kDhcpStats.lease_seconds = (opt && olen >= 4) ? net_read32(opt) : 0;
		// The name server: kept, not used — the resolver lives in ring 3
		// and reads this back out of /sys/net/dhcp (dhcp.h). Option 6 may
		// list several; the first is the one every client tries first.
		opt = dhcp_find_option(p, length, DHCP_OPT_DNS, &olen);
		kDhcpStats.lease_dns = (opt && olen >= 4) ? net_read32(opt) : 0;
		kDhcpStats.lease_server = s_dhcp.server_id;

		kNetIPv4Address = kDhcpStats.lease_ip;
		kNetIPv4Netmask = kDhcpStats.lease_mask;
		kNetIPv4Gateway = kDhcpStats.lease_gateway;
		kDhcpStats.state = DHCP_BOUND;

		// The milestone line, on glass: the machine's address is now
		// something it ASKED THE NETWORK FOR. (Renewal when the lease
		// expires is the booked DEBT — see dhcp.h.)
		printf("net: DHCP lease %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u from %u.%u.%u.%u (%u s)\n",
		       NET_IPV4_OCTETS(kNetIPv4Address), NET_IPV4_OCTETS(kNetIPv4Netmask),
		       NET_IPV4_OCTETS(kNetIPv4Gateway), NET_IPV4_OCTETS(kDhcpStats.lease_server),
		       kDhcpStats.lease_seconds);
	}
	else if (msg == DHCP_MSG_NAK && kDhcpStats.state == DHCP_REQUESTING)
	{
		// The server changed its mind (lease taken between OFFER and
		// REQUEST, usually). Start the conversation over.
		kDhcpStats.naks_received++;
		printd(DEBUG_NET, "dhcp: NAK — restarting discovery\n");
		kDhcpStats.state = DHCP_SELECTING;
		s_dhcp.attempts = 0;
		dhcp_send(DHCP_MSG_DISCOVER);
	}
	else
		kDhcpStats.ignored++;   // right xid, wrong moment — stale reply
}

// ── The retry rider ─────────────────────────────────────────────────────────
void dhcp_poll(void)
{
	dhcp_state_t st = kDhcpStats.state;
	if (st != DHCP_SELECTING && st != DHCP_REQUESTING)
		return;   // the one-compare fast path (IDLE/BOUND/GAVE_UP)

	if (kTicksSinceStart - s_dhcp.last_send_tick < DHCP_RETRY_TICKS)
		return;

	if (s_dhcp.attempts >= DHCP_MAX_ATTEMPTS)
	{
		// Nobody answered. Keep the static convention defaults and say
		// so — a silent failure here would look exactly like a working
		// static config, which is the kind of lie this OS doesn't tell.
		kDhcpStats.state = DHCP_GAVE_UP;
		printf("net: DHCP got no answer (%u tries) — using static %u.%u.%u.%u\n",
		       (uint32_t)s_dhcp.attempts, NET_IPV4_OCTETS(kNetIPv4Address));
		return;
	}
	dhcp_send(st == DHCP_SELECTING ? DHCP_MSG_DISCOVER : DHCP_MSG_REQUEST);
}

// ── Entry ───────────────────────────────────────────────────────────────────
void dhcp_start(net_device_t* dev)
{
	s_dhcp.dev = dev;
	// xid: unique-enough per boot (tick count entropy over a recognizable
	// "o6" brand for pcap readers). Uniqueness matters across REBOOTS —
	// a server matching a stale xid from our previous life could answer
	// the wrong conversation.
	s_dhcp.xid = 0x6F360000 | (uint32_t)(kTicksSinceStart & 0xFFFF);

	if (udp_bind(UDP_PORT_DHCP_CLIENT, dhcp_rx, NULL) != 0)
	{
		// Port 68 taken means someone is already doing DHCP — refuse to
		// fight over the mailbox.
		printd(DEBUG_NET, "dhcp: port %u already bound — client not started\n",
		       UDP_PORT_DHCP_CLIENT);
		return;
	}
	kDhcpStats.state = DHCP_SELECTING;
	s_dhcp.attempts = 0;
	dhcp_send(DHCP_MSG_DISCOVER);
	printd(DEBUG_NET, "dhcp: DISCOVER sent (xid 0x%x)\n", s_dhcp.xid);
}
