// net.c — os64_dial and the bang-path parser (the library half of ruling #1).
//
// Everything textual about networking lives in this file and below the
// syscall boundary nothing textual exists: the parser's entire job is to
// lower "udp!10.0.2.2!53" onto an os64_netdest_t and make the struct call.
// Strict on purpose — a dial string is user input, and the parser refuses
// garbage rather than guessing (exactly four octets, each 0-255; a port
// 1-65535; a protocol it knows). No allocation, no globals, no strtok.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "os64/dial.h"
#include "os64/resolve.h"         // names become addresses here, not in the kernel
#include "os64/syscall.h"          // the raw stubs
#include "os64/syscall_numbers.h"

int64_t os64_net_dial(const os64_netdest_t *dest)
{
	return (int64_t)os64_syscall1(SYSCALL_NET_DIAL, (uint64_t)dest);
}

// Parse an unsigned decimal out of [s, bang-or-end), bounded by `max`.
// Returns false on empty/non-digit/overflow — refusal, never guessing.
static bool parse_decimal_segment(const char *s, const char *end,
                                  uint32_t max, uint32_t *out)
{
	if (s >= end)
		return false;
	uint32_t value = 0;
	while (s < end)
	{
		if (*s < '0' || *s > '9')
			return false;
		value = value * 10 + (uint32_t)(*s - '0');
		if (value > max)
			return false;
		s++;
	}
	*out = value;
	return true;
}

int64_t os64_dial(const char *dialstring)
{
	// Refusals are SPECIFIC (os64/net.h error table): a parser that answers
	// every question with -1 forces the caller into a debugger to learn
	// which segment it fumbled. The code names the segment.
	if (dialstring == 0)
		return OS64_NET_ERR_BAD_STRING;

	// Segment 1: the protocol — the verb of the whole call, always
	// explicit. Matched by spelling, not by prefix: "u!..." is a typo,
	// not UDP.
	const char *s = dialstring;
	const char *bang = s;
	while (*bang && *bang != '!')
		bang++;
	if (*bang != '!')
		return OS64_NET_ERR_BAD_STRING;

	uint16_t protocol;
	if (bang - s == 3 && s[0] == 'u' && s[1] == 'd' && s[2] == 'p')
		protocol = OS64_NET_UDP;
	else if (bang - s == 3 && s[0] == 't' && s[1] == 'c' && s[2] == 'p')
		protocol = OS64_NET_TCP;
	else if (bang - s == 4 && s[0] == 'i' && s[1] == 'c' && s[2] == 'm' && s[3] == 'p')
		protocol = OS64_NET_ICMP;
	else
		return OS64_NET_ERR_BAD_STRING;

	// ICMP dial strings have TWO segments, not three ("icmp!10.0.2.2"),
	// because echo has no ports — the kernel owns the identifier that
	// stands in for one (see os64/net.h). Plan 9 would have called this
	// segment a "service"; echo simply doesn't have one, and inventing a
	// placeholder to keep the shape uniform would be ceremony.
	bool no_service = (protocol == OS64_NET_ICMP);

	// Segment 2: the address — a dotted quad, or since 2026-08-22 a NAME,
	// which the resolver (os64/resolve.h) turns into one: the hosts files
	// first, then a DNS question. The segment runs to the '!' before the
	// service, or to the end of the string when there is no service (ICMP).
	// Nothing textual crosses the syscall boundary either way — the kernel
	// receives an address, as it always has; this is Plan 9's dial() going
	// through cs, in one process instead of two.
	s = bang + 1;
	const char *seg_end = s;
	while (*seg_end && *seg_end != '!')
		seg_end++;
	if (no_service && *seg_end != '\0')
		return OS64_NET_ERR_BAD_SERVICE;   // "icmp!1.2.3.4!something" —
		                                   // a service echo doesn't have
	if (!no_service && *seg_end != '!')
		return OS64_NET_ERR_BAD_SERVICE;   // "tcp!host" — the door is missing
	if (seg_end == s || seg_end - s > OS64_RESOLVE_NAME_MAX)
		return OS64_NET_ERR_BAD_ADDRESS;

	uint32_t ip = 0;               // host order, per ruling #2 — it reads
	                               // like an address in a debugger
	if (!os64_parse_ipv4(s, seg_end, &ip))
	{
		char name[OS64_RESOLVE_NAME_MAX + 1];
		size_t nlen = (size_t)(seg_end - s);
		for (size_t i = 0; i < nlen; i++)
			name[i] = s[i];
		name[nlen] = '\0';
		int64_t rr = os64_resolve(name, &ip);
		if (rr < 0)
			return rr;             // NO_SUCH_HOST / NO_RESOLVER / TIMEOUT — its own words
	}
	s = *seg_end ? seg_end + 1 : seg_end;

	// Segment 3: the service — numeric port until names ("!dns") earn a
	// consumer. Runs to end of string; port 0 is not a door. Absent
	// entirely for ICMP, whose destination is a machine, not a door.
	uint32_t port = 0;
	if (!no_service)
	{
		const char *end = s;
		while (*end)
			end++;
		if (!parse_decimal_segment(s, end, 65535, &port) || port == 0)
			return OS64_NET_ERR_BAD_SERVICE;   // port 0 is not a door
	}

	os64_netdest_t dest = {
		.ip = ip,
		.port = (uint16_t)port,
		.protocol = protocol,
	};
	return os64_net_dial(&dest);
}
