#ifndef OS64_ABI_NET_H
#define OS64_ABI_NET_H

// os64/net.h — the network ABI contract (NETWORK.md rulings #1/#2/#4,
// ratified 2026-07-28).
//
// One struct names a destination. Every field is HOST byte order — the
// kernel owns the wire and swaps at the packet boundary (net_wire.h is the
// entire swap surface of the OS), so no htons() exists on either side of
// this header, and never will. An address here reads like an address in a
// debugger: 10.0.2.2 is 0x0A000202.
//
// The protocol values deliberately reuse IP's own protocol numbers (UDP=17,
// TCP=6 — the /etc/protocols registry, RFC 790 lineage): one less invented
// namespace, and the field drops straight into the IPv4 header's proto byte.

#include <stdint.h>

#define OS64_NET_UDP 17   // datagrams: each write is one packet, each read one packet
#define OS64_NET_TCP 6    // streams — Phase 4; dialing it today returns an error

typedef struct os64_netdest
{
	uint32_t ip;         // host order: NET destination address (10.0.2.2 == 0x0A000202)
	uint16_t port;       // host order: which door (53 = DNS, 67 = DHCP...)
	uint16_t protocol;   // OS64_NET_UDP / OS64_NET_TCP
} os64_netdest_t;

// The handle net_dial returns obeys the house read/write contract:
//   write(h, buf, len)  — one call = ONE datagram (atomic; oversize is an
//                         error, never a fragmenting loop)
//   read(h, buf, len)   — blocks until a datagram arrives from the dialed
//                         peer, then returns THAT datagram (short if buf is
//                         smaller — the tail is dropped, the classic UDP
//                         truncation contract)
//   close(h)            — hangs up; the local port is freed
// Datagrams from anyone OTHER than the dialed peer never reach the handle
// (connected-UDP semantics, ruling #4 option (a)); a future recvfrom-style
// call arrives only when a real consumer demands it (option (c)).

#endif // OS64_ABI_NET_H
