#ifndef ICMP_H
#define ICMP_H

// icmp.h — Internet Control Message Protocol (RFC 792, September 1981).
//
// ICMP is IP's back-channel: how the network itself talks — "that host is
// unreachable", "your TTL ran out" (traceroute is built entirely on that
// one), and the pair everyone knows: echo request and echo reply. `ping`
// (Mike Muuss, 1983, named for the sonar sound, written in an evening at
// the Army Research Lab) is nothing but those two messages and a stopwatch,
// and it is still the first question anyone asks a sick network.
//
// v1 speaks exactly the echo pair, both directions. The error messages
// (unreachable, time-exceeded) become worth EMITTING when os64 routes or
// serves — booked for the phases that need them; parsing arrivals of them
// lands with TCP, which must react to unreachables.
//
// The echo message, after the IP header comes off:
//
//   [0] type (1)      8 = request, 0 = reply
//   [1] code (1)      0 for the echo pair
//   [2] checksum (2)  RFC 1071 over the WHOLE ICMP message (unlike IPv4's
//                     header-only — payload corruption shows up here)
//   [4] identifier(2) whose conversation this is (classically the pid)
//   [6] sequence (2)  which ping of the conversation
//   [8] payload...    arbitrary; the reply must echo it BYTE FOR BYTE —
//                     that mirror is the whole diagnostic (ping's RTT
//                     stopwatch rides in here as a timestamp the echo
//                     brings home)

#include <stdint.h>
#include <stdbool.h>
#include "driver/net/net_device.h"

#define ICMP_HDR_LEN       8
#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

typedef struct icmp_stats
{
	uint64_t echo_requests_received;  // pings of us
	uint64_t echo_replies_sent;       //   ...answered (the responder half)
	uint64_t echo_requests_sent;      // pings by us
	uint64_t echo_replies_received;   //   ...answered (the wire works end to end)
	uint64_t bad_checksum;
	uint64_t truncated;
	uint64_t unknown_type;            // the RFC 792 catalog we don't parse yet
	uint64_t broadcast_ignored;       // echo requests to broadcast (see icmp.c)

	// Identity of the LAST echo reply that arrived — enough state for the
	// test fixtures and for a debugger to confirm "the gateway answered
	// ping #3". Phase 3's rulings decide how userland ping really receives
	// replies; this is deliberately not that mechanism, just visibility.
	uint32_t last_reply_src;          // host-order source address
	uint16_t last_reply_ident;
	uint16_t last_reply_seq;
} icmp_stats_t;
extern icmp_stats_t kIcmpStats;

// Inbound ICMP message (IP header stripped; src host-order; broadcast =
// whether the enclosing IP packet was addressed to a broadcast). RX-handler
// context: quick, no sleeping.
void icmp_input(net_device_t* dev, uint32_t src_ip, bool broadcast,
                const void* pkt, uint16_t length);

// Send one echo request ("ping") to dst_ip with the given conversation
// identity. Payload is a fixed recognizable byte ramp (see icmp.c).
// Returns ipv4_send's verdict — including -2 "ARP still resolving, retry
// shortly", which first-time callers should expect (see ipv4.c).
int32_t icmp_send_echo_request(net_device_t* dev, uint32_t dst_ip,
                               uint16_t ident, uint16_t sequence);

#endif // ICMP_H
