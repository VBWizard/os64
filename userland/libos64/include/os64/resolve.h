#ifndef OS64_RESOLVE_H
#define OS64_RESOLVE_H

// os64/resolve.h — a name becomes an address. The library half of the
// network, one layer above dial's bang-path parser and the reason that
// parser can now read "tcp!yogi!6464" (2026-08-22).
//
// TWO ANSWERS, IN ORDER, because they are two different problems that
// happen to share a question:
//
//   1. THE HOSTS FILES — `addr name [alias...]`, '#' comments. This is
//      SRI-NIC's HOSTS.TXT (1974), the file every ARPANET host FTP'd
//      nightly until the list outgrew the method; it predates DNS by a
//      decade and is still the right answer for the three machines in the
//      room. /home/hosts is read first (YOUR names — the build PC's is
//      yours, not the system's), then /etc/hosts; the two MERGE, first
//      match wins, so a /home line can override a system one by repeating
//      it (Chris's ruling: merge, not first-file-wins).
//
//   2. DNS — one UDP question to one server, A records only, five seconds
//      of patience across two tries, no cache, no search list, no IPv6.
//      Sized to the demand (Mockapetris, 1983, had a bigger one). The
//      server is `nameserver = a.b.c.d` in /home/net.conf or /etc/net.conf,
//      else the one the DHCP lease offered (option 6, read back out of
//      /sys/net/dhcp — a file, not a syscall: the kernel keeps the number
//      and this code has opinions about it).
//
// IN THE LIBRARY, NOT A SERVICE — and not only because os64 has no listen
// yet. A resolver is a client; it asks and waits. The day this becomes a
// daemon is the day CACHING across processes earns it (nscd, Plan 9's
// cs/dns), and the seam is exactly here: os64_dial calls os64_resolve, and
// whether that resolves in-process or over a pipe is invisible to every
// caller.

#include <stdint.h>
#include <stdbool.h>

// 0 and *ip (host order, like every address in the ABI) on success. A
// dotted quad resolves to itself. Negative on failure, from the dial table
// (os64/net.h): OS64_NET_ERR_NO_SUCH_HOST, OS64_NET_ERR_NO_RESOLVER,
// OS64_NET_ERR_TIMEOUT, or whatever dialing the name server itself said.
int64_t os64_resolve(const char *name, uint32_t *ip);

// The quad parser on its own: "10.0.2.2" -> 0x0a000202. False for
// anything else (a name, an empty string, 256 in an octet).
bool os64_parse_ipv4(const char *s, const char *end, uint32_t *ip);

// And its inverse: 0x0a000202 -> "10.0.2.2". Writes at most cap-1 bytes
// plus a NUL and returns the length written (0 if cap is too small —
// OS64_IPV4_STR_MAX always fits). ping's header line was the first customer
// (Chris, 2026-08-22): once a name can stand where an address did, the
// address it turned into is worth printing beside it, the way every ping
// since 1983 has.
#define OS64_IPV4_STR_MAX 16   // "255.255.255.255" + NUL
size_t os64_format_ipv4(uint32_t ip, char *buf, size_t cap);

// The longest name this resolver will carry — DNS's own limit.
#define OS64_RESOLVE_NAME_MAX 253

#endif
