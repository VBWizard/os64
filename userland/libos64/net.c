// net.c — os64_dial and the bang-path parser (the library half of ruling #1).
//
// Everything textual about networking lives in this file and below the
// syscall boundary nothing textual exists: the parser's entire job is to
// lower "udp!10.0.2.2!53" onto an os64_netdest_t and make the struct call.
// Strict on purpose — a dial string is user input, and the parser refuses
// garbage rather than guessing (exactly four octets, each 0-255; a port
// 1-65535; a protocol it knows). No allocation, no globals, no strtok.

#include <stdint.h>
#include <stdbool.h>
#include "os64/dial.h"
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
	if (dialstring == 0)
		return -1;

	// Segment 1: the protocol — the verb of the whole call, always
	// explicit. Matched by spelling, not by prefix: "u!..." is a typo,
	// not UDP.
	const char *s = dialstring;
	const char *bang = s;
	while (*bang && *bang != '!')
		bang++;
	if (*bang != '!')
		return -1;

	uint16_t protocol;
	if (bang - s == 3 && s[0] == 'u' && s[1] == 'd' && s[2] == 'p')
		protocol = OS64_NET_UDP;
	else if (bang - s == 3 && s[0] == 't' && s[1] == 'c' && s[2] == 'p')
		protocol = OS64_NET_TCP;   // parses today, kernel refuses until Phase 4
	else
		return -1;

	// Segment 2: the address — dotted quad only, until the resolver
	// library teaches this spot to read names.
	s = bang + 1;
	uint32_t ip = 0;
	for (int octet = 0; octet < 4; octet++)
	{
		const char *seg_end = s;
		char stop = (octet < 3) ? '.' : '!';
		while (*seg_end && *seg_end != stop)
			seg_end++;
		if (*seg_end != stop)
			return -1;

		uint32_t value;
		if (!parse_decimal_segment(s, seg_end, 255, &value))
			return -1;
		ip = (ip << 8) | value;   // host order, per ruling #2 — it reads
		                          // like an address in a debugger
		s = seg_end + 1;
	}

	// Segment 3: the service — numeric port until names ("!dns") earn a
	// consumer. Runs to end of string; port 0 is not a door.
	const char *end = s;
	while (*end)
		end++;
	uint32_t port;
	if (!parse_decimal_segment(s, end, 65535, &port) || port == 0)
		return -1;

	os64_netdest_t dest = {
		.ip = ip,
		.port = (uint16_t)port,
		.protocol = protocol,
	};
	return os64_net_dial(&dest);
}
