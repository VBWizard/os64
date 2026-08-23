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
#include "os64/syscall_numbers.h"   // OS64_ERR_TIMEOUT — the alias below

#define OS64_NET_UDP  17   // datagrams: each write is one packet, each read one packet
#define OS64_NET_TCP  6    // streams: read/write bytes, like a pipe with a peer
#define OS64_NET_ICMP 1    // echoes: write asks, read hears the answer come back

typedef struct os64_netdest
{
	uint32_t ip;         // host order: NET destination address (10.0.2.2 == 0x0A000202)
	uint16_t port;       // host order: which door (53 = DNS, 67 = DHCP...)
	                     // IGNORED for OS64_NET_ICMP — echo has no ports (below)
	uint16_t protocol;   // OS64_NET_UDP / OS64_NET_TCP / OS64_NET_ICMP
} os64_netdest_t;

// ── Why a dial failed ───────────────────────────────────────────────────
// dial returns a handle (>= 0) or exactly ONE of these. os64 has no errno —
// the return value IS the reason, so every code answers the caller's next
// question ("was my string mangled, or was the peer just not home?")
// without a debugger. The first two are the syscall boundary's house-wide
// verdicts, named here so a dialer can read them; the rest are dial's own.
// Codes -3..-5 are the LIBRARY parser's (the kernel never sees text);
// -6..-10 come up from the kernel.
#define OS64_NET_ERR_INVALID       (-1)  // generic refusal (boundary-owned;
                                         //  dial itself always says more below)
#define OS64_NET_ERR_BAD_POINTER   (-2)  // the netdest pointer wasn't yours
                                         //  (boundary user-range check)
#define OS64_NET_ERR_BAD_STRING    (-3)  // parser: not a bang path, or the
                                         //  protocol segment isn't udp/tcp/icmp
#define OS64_NET_ERR_BAD_ADDRESS   (-4)  // parser: address isn't a dotted quad
                                         //  (four octets, each 0-255)
#define OS64_NET_ERR_BAD_SERVICE   (-5)  // parser: service missing / not
                                         //  1-65535 — or PRESENT on icmp,
                                         //  which has no doors
#define OS64_NET_ERR_BAD_DEST      (-6)  // kernel: struct refused (ip 0,
                                         //  unknown protocol, port 0)
#define OS64_NET_ERR_NO_NIC        (-7)  // kernel: netless boot — dialing
                                         //  with no line is the error, the
                                         //  boot itself is a configuration
#define OS64_NET_ERR_NO_RESOURCES  (-8)  // kernel: out of memory, ephemeral
                                         //  ports, identifiers, or handles
#define OS64_NET_ERR_REFUSED       (-9)  // kernel: TCP peer answered RST —
                                         //  the machine is there, that door
                                         //  is not open
// kernel: nobody answered in time — a TCP handshake met silence, or an
// os64_read_for() deadline expired with nothing to show. An ALIAS since
// 2026-08-05: a timeout stopped being a network concept the day the console
// learned one (top's poll for 'q'), so the value's true name lives with the
// read contract in syscall_numbers.h and the dial table keeps this spelling
// for its own readability.
#define OS64_NET_ERR_TIMEOUT       OS64_ERR_TIMEOUT

// The RESOLVER's two verdicts (libos64/resolve.c, 2026-08-22 — the day a
// dial string learned to carry a name). Library codes like -3..-5: the
// kernel never sees a name. They are distinct from BAD_ADDRESS on purpose —
// "that is not an address" and "that is a perfectly good name nobody
// answers for" send a person to different places.
#define OS64_NET_ERR_NO_RESOLVER   (-11) // a name was given but there is no
                                         //  name server to ask: no
                                         //  `nameserver` in net.conf and the
                                         //  DHCP lease offered none (or the
                                         //  boot is static). /etc/hosts still
                                         //  works without one
#define OS64_NET_ERR_NO_SUCH_HOST  (-12) // the hosts files and the name
                                         //  server were all asked, and the
                                         //  answer was no (NXDOMAIN, or an
                                         //  answer with no A record)

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
//
// ── ICMP: "udp!" with no doors ──────────────────────────────────────────
// An ICMP handle (dial "icmp!10.0.2.2" — two segments, no port) sends and
// receives ECHO, the pair `ping` is made of:
//   write(h, buf, len)  — one echo request carrying `buf` as its payload
//   read(h, buf, len)   — blocks for the matching reply, returns the
//                         payload the peer echoed back to us
//
// Echo has no port numbers, so what identifies "my conversation" is the
// ICMP IDENTIFIER field — a 16-bit number whose entire job is to let one
// machine run several pings at once without mixing up the answers. That
// is a port in all but name, so os64 treats it as one: the kernel assigns
// it at dial time exactly the way it assigns an ephemeral port, and
// replies carrying anyone else's identifier never reach your handle.
// (This is also how ping stopped needing root on modern systems — an
// identifier the kernel owns is a raw socket nobody can abuse.)
//
// The SEQUENCE number is likewise the kernel's, incremented per write. A
// program that wants to match a specific reply — or measure a round trip —
// puts its own marker in the PAYLOAD and reads it back, which is exactly
// what every ping since 1983 has done with a timestamp.

#endif // OS64_ABI_NET_H
