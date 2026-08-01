// icmp.c — the echo pair, both directions.
//
// The responder half is Phase 2's headline act: when this file answers its
// first echo request, os64 stops being a machine that can only speak and
// becomes one you can CALL — "can you ping it?" gets a yes. The answering
// logic is deliberately mirror-simple, because that's what the protocol is:
// same identifier, same sequence, same payload to the byte, type flipped
// from 8 to 0, checksum recomputed. The sender's stopwatch/pattern lives in
// the payload; our whole diplomatic duty is to not touch it.

#include <stdint.h>
#include <stdbool.h>
#include "serial_logging.h"
#include "memcpy.h"
#include "CONFIG.h"
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/net_checksum.h"
#include "driver/net/ipv4.h"
#include "driver/net/icmp.h"

icmp_stats_t kIcmpStats;

// Echo requests can be big (the payload is sender's choice up to MTU), and
// the reply buffer must hold whatever arrived. One MTU-sized frame of
// kernel stack per nested call — RX handler depth is fixed at one, so this
// is a bounded 1480 bytes, not a recursion hazard.
#define ICMP_MAX_MSG (1500 - IPV4_HDR_MIN)

void icmp_input(net_device_t* dev, uint32_t src_ip, bool broadcast,
                const void* pkt, uint16_t length)
{
	const uint8_t* p = (const uint8_t*)pkt;

	if (length < ICMP_HDR_LEN)
	{
		kIcmpStats.truncated++;
		return;
	}

	// Whole-message checksum (ICMP guards its payload, unlike IPv4's
	// header-only): a valid message sums to zero, one call, no zeroing.
	if (net_checksum(p, length) != 0)
	{
		kIcmpStats.bad_checksum++;
		printd(DEBUG_NET, "icmp: bad checksum from %u.%u.%u.%u — dropped\n",
		       NET_IPV4_OCTETS(src_ip));
		return;
	}

	switch (p[0])
	{
		case ICMP_TYPE_ECHO_REQUEST:
		{
			// The broadcast rule: never answer a ping sent to everyone.
			// RFC 1122 left this a "MAY"; the 1998 smurf attack — spoof a
			// victim's address as the SOURCE of a broadcast ping and let a
			// whole subnet's replies bury them — turned the MAY into a
			// reflex. One if-statement of history.
			if (broadcast)
			{
				kIcmpStats.broadcast_ignored++;
				return;
			}
			kIcmpStats.echo_requests_received++;
			if (length > ICMP_MAX_MSG)
			{
				// Can't stage a reply this big (and it couldn't have
				// arrived unfragmented anyway) — counted, not answered.
				kIcmpStats.truncated++;
				return;
			}

			// The mirror: copy verbatim, flip the type, restate the sum.
			uint8_t reply[ICMP_MAX_MSG];
			memcpy(reply, p, length);
			reply[0] = ICMP_TYPE_ECHO_REPLY;
			net_write16(reply + 2, 0);
			net_write16(reply + 2, net_checksum(reply, length));

			// If the asker isn't in the ARP cache yet, ipv4_send returns
			// -2 after firing a query — but an asker's MAC was learned by
			// the frame that carried the question here (arp/eth learning
			// happens before this runs when the asker ARPed first, which
			// real stacks always do before pinging a fresh address). A
			// reply that still misses is counted by ipv4's stats; the
			// asker will re-ask — echo has retry built into its culture.
			ipv4_send(dev, src_ip, IPV4_PROTO_ICMP, reply, length);
			kIcmpStats.echo_replies_sent++;
			printd(DEBUG_NET, "icmp: echoed %u bytes back to %u.%u.%u.%u (id %u seq %u)\n",
			       length, NET_IPV4_OCTETS(src_ip), net_read16(p + 4), net_read16(p + 6));
			break;
		}

		case ICMP_TYPE_ECHO_REPLY:
			// An answer to one of OUR pings. Bank the identity where a
			// test or a debugger can see it; the real delivery-to-userland
			// story is a Phase 3 API question (Chris's rulings).
			kIcmpStats.echo_replies_received++;
			kIcmpStats.last_reply_src   = src_ip;
			kIcmpStats.last_reply_ident = net_read16(p + 4);
			kIcmpStats.last_reply_seq   = net_read16(p + 6);
			printd(DEBUG_NET, "icmp: reply from %u.%u.%u.%u (id %u seq %u, %u bytes)\n",
			       NET_IPV4_OCTETS(src_ip), kIcmpStats.last_reply_ident,
			       kIcmpStats.last_reply_seq, length);
			break;

		default:
			// Unreachables, time-exceeded, redirects — real messages with
			// real futures (TCP must parse unreachables), counted until
			// their phase arrives.
			kIcmpStats.unknown_type++;
			printd(DEBUG_NET | DEBUG_DETAILED, "icmp: type %u code %u from %u.%u.%u.%u not yet spoken\n",
			       p[0], p[1], NET_IPV4_OCTETS(src_ip));
			break;
	}
}

int32_t icmp_send_echo_request(net_device_t* dev, uint32_t dst_ip,
                               uint16_t ident, uint16_t sequence)
{
	// Header + 32 bytes of recognizable ramp. Unix ping traditionally
	// carries a timestamp then pattern filler; Windows ping sends the
	// alphabet. Ours is a 0x10..0x2F byte ramp — instantly recognizable in
	// a pcap hex dump, and any corruption shows as a broken stair-step.
	// (The kernel test fixture checksums it anyway; eyes and math agree.)
	uint8_t msg[ICMP_HDR_LEN + 32];
	msg[0] = ICMP_TYPE_ECHO_REQUEST;
	msg[1] = 0;
	net_write16(msg + 2, 0);
	net_write16(msg + 4, ident);
	net_write16(msg + 6, sequence);
	for (uint16_t i = 0; i < 32; i++)
		msg[ICMP_HDR_LEN + i] = (uint8_t)(0x10 + i);
	net_write16(msg + 2, net_checksum(msg, sizeof(msg)));

	int32_t rc = ipv4_send(dev, dst_ip, IPV4_PROTO_ICMP, msg, sizeof(msg));
	if (rc == 0)
		kIcmpStats.echo_requests_sent++;
	return rc;
}
