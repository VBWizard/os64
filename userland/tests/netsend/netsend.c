// netsend — push bytes UP a TCP connection, and say how long it took.
//
// Everything else in the tree downloads: os64get, whois, the browser rungs
// are all a small request followed by a large reply, so the segments os64
// itself must get across the wire are a handful. This is the workload the
// SEND side's debts are measured against — the retransmit timer (only OUR
// lost segments are OUR timer's problem; a download's losses are the
// peer's), the stop-and-wait slot, one day a send window. Its partner is
// tools/tcpsink.py on the host, which verifies every byte against the same
// pattern, so "arrived" and "arrived right" are one answer.
//
//   netsend HOST PORT BYTES        e.g. netsend 10.0.2.2 7200 100000
//
// Exit 0 with a line naming bytes and milliseconds; a badge code otherwise.
#include "os64/os64.h"

#define NETSEND_USAGE      0x5E4D0001
#define NETSEND_BAD_DIAL   0x5E4D0002
#define NETSEND_BAD_WRITE  0x5E4D0003
#define NETSEND_BAD_CLOCK  0x5E4D0004

// The pattern: a 32-bit LCG (Numerical Recipes' constants), one byte from
// the middle of each state. tcpsink.py generates the same stream.
#define NETSEND_SEED 0x5EEDu

int main(int argc, char **argv)
{
	if (argc != 4)
	{
		os64_hprintf(OS64_STDERR, "usage: netsend HOST PORT BYTES\n");
		return NETSEND_USAGE;
	}
	int64_t port  = os64_atoi(argv[2]);
	int64_t total = os64_atoi(argv[3]);
	if (port <= 0 || port > 65535 || total <= 0)
	{
		os64_hprintf(OS64_STDERR, "netsend: PORT is 1..65535 and BYTES is positive\n");
		return NETSEND_USAGE;
	}

	char dialstring[128];
	os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%ld", argv[1], (long)port);
	int64_t conn = os64_dial(dialstring);
	if (conn < 0)
	{
		os64_hprintf(OS64_STDERR, "netsend: cannot reach %s:%ld — %s\n",
		             argv[1], (long)port, os64_dial_reason(conn));
		return NETSEND_BAD_DIAL;
	}

	os64_ticks_t start;
	if (os64_ticks(&start) < 0)
		return NETSEND_BAD_CLOCK;

	static uint8_t chunk[8192];
	uint32_t x = NETSEND_SEED;
	int64_t sent = 0;
	while (sent < total)
	{
		int64_t n = total - sent;
		if (n > (int64_t)sizeof(chunk))
			n = (int64_t)sizeof(chunk);
		for (int64_t i = 0; i < n; i++)
		{
			x = x * 1103515245u + 12345u;
			chunk[i] = (uint8_t)(x >> 16);
		}
		int64_t off = 0;
		while (off < n)
		{
			int64_t w = os64_write((int32_t)conn, chunk + off, (size_t)(n - off));
			if (w <= 0)
			{
				os64_hprintf(OS64_STDERR, "netsend: write failed after %ld bytes (%ld)\n",
				             (long)(sent + off), (long)w);
				os64_close((int32_t)conn);
				return NETSEND_BAD_WRITE;
			}
			off += w;
		}
		sent += n;
	}
	os64_close((int32_t)conn);

	os64_ticks_t end;
	if (os64_ticks(&end) < 0)
		return NETSEND_BAD_CLOCK;
	uint64_t ms = (end.ticks - start.ticks) * 1000u / start.per_second;
	os64_printf("netsend: %ld bytes to %s:%ld in %lu ms\n",
	            (long)sent, argv[1], (long)port, (unsigned long)ms);
	return 0;
}
