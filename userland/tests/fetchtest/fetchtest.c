// fetchtest — NETWORK.md's Phase 4 milestone, in one ring-3 program:
// os64 fetches a real page from a real webserver on the real internet.
//
// The whole tower, end to end, from a program with no special privileges:
//
//   1. dial UDP to the resolver, ask "where is example.com?"      (Phase 3)
//   2. dial TCP to that address — three-way handshake, ring 3     (Phase 4)
//   3. write an HTTP request; read the reply until EOF            (a stream)
//   4. print the status line and the byte count
//
// Every layer under this is os64's: the bang-path parser, net_dial, the
// handle table, the TCP state machine, IPv4, ethernet, the virtio ring —
// and slirp is only NAT, not a helper. When this prints "HTTP/1.0 200 OK",
// os64 has genuinely spoken to a machine it has never met.
//
// This is a FIXTURE, not the utility: a real `fetch` (or `wget`, or a
// `curl`-shaped thing) with options, redirects, and output files is
// Chris's to write, per the standing labor division. This one exists to
// prove the stack and to hand the kernel a verdict.
//
// Exit codes (the ring3-fixture convention):
//   0x0FE7C400 success            0x0FE7C403 TCP dial failed
//   0x0FE7C401 DNS dial failed    0x0FE7C404 HTTP write failed
//   0x0FE7C402 no A record        0x0FE7C405 no/short response

#include "os64/os64.h"

#define FETCH_OK          0x0FE7C400
#define FETCH_BAD_DNS     0x0FE7C401
#define FETCH_NO_ADDRESS  0x0FE7C402
#define FETCH_BAD_DIAL    0x0FE7C403
#define FETCH_BAD_WRITE   0x0FE7C404
#define FETCH_BAD_READ    0x0FE7C405

#define HOSTNAME "example.com"

// Resolve a name to an IPv4 address (host order), 0 on failure. A tiny
// DNS client — the real resolver library lives in libos64's future, and
// this is the shape it will have: UDP handle, one question, one answer.
static uint32_t resolve(const char *ignored)
{
	(void)ignored;   // v1 asks about exactly one name (below), by design

	int64_t dns = os64_dial("udp!10.0.2.3!53");
	if (dns < 0)
		return 0;

	// The question, RFC 1035 wire format: id 0x6F34 ("o4"), recursion
	// desired, one question — 7"example" 3"com" 0, type A, class IN.
	uint8_t query[29] = {
		0x6F, 0x34,  0x01, 0x00,  0x00, 0x01,  0x00, 0x00,
		0x00, 0x00,  0x00, 0x00,
		7, 'e','x','a','m','p','l','e',  3, 'c','o','m',  0,
		0x00, 0x01,  0x00, 0x01,
	};
	if (os64_write((int32_t)dns, query, sizeof(query)) != (int64_t)sizeof(query))
	{
		os64_close((int32_t)dns);
		return 0;
	}

	uint8_t reply[512];
	int64_t n = os64_read((int32_t)dns, reply, sizeof(reply));
	os64_close((int32_t)dns);
	if (n < 12)
		return 0;

	uint16_t ancount = (uint16_t)((reply[6] << 8) | reply[7]);
	int64_t off = 12;
	while (off < n && reply[off] != 0)      // skip the echoed question
		off += reply[off] + 1;
	off += 1 + 4;

	for (uint16_t a = 0; a < ancount && off + 12 <= n; a++)
	{
		if ((reply[off] & 0xC0) == 0xC0)    // compressed name pointer
			off += 2;
		else
		{
			while (off < n && reply[off] != 0)
				off += reply[off] + 1;
			off += 1;
		}
		if (off + 10 > n)
			break;
		uint16_t rtype = (uint16_t)((reply[off] << 8) | reply[off + 1]);
		uint16_t rdlen = (uint16_t)((reply[off + 8] << 8) | reply[off + 9]);
		off += 10;
		if (rtype == 1 && rdlen == 4 && off + 4 <= n)
			return ((uint32_t)reply[off] << 24) | ((uint32_t)reply[off + 1] << 16) |
			       ((uint32_t)reply[off + 2] << 8) | (uint32_t)reply[off + 3];
		off += rdlen;                        // a CNAME on the way: keep walking
	}
	return 0;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	uint32_t ip = resolve(HOSTNAME);
	if (ip == 0)
	{
		os64_debug_log("fetchtest: DNS resolution failed\n");
		return FETCH_NO_ADDRESS;
	}

	// The dial string, composed at runtime — the ratified shape doing its
	// job: a resolved address becomes text becomes a struct becomes a
	// connection, and the kernel only ever sees the struct.
	char dialstr[40];
	os64_snprintf(dialstr, sizeof(dialstr), "tcp!%u.%u.%u.%u!80",
	              (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);

	int64_t h = os64_dial(dialstr);
	if (h < 0)
	{
		os64_debug_log("fetchtest: TCP dial failed\n");
		return FETCH_BAD_DIAL;
	}

	// HTTP/1.0 with an explicit close: the server sends the page and then
	// closes, so EOF marks the end of the body and we need no chunked
	// decoding or content-length arithmetic. (HTTP/1.0 is 1996 and still
	// the politest way to ask a stranger for one document.)
	static const char request[] =
		"GET / HTTP/1.0\r\n"
		"Host: " HOSTNAME "\r\n"
		"User-Agent: os64/1.0 (fetchtest)\r\n"
		"Connection: close\r\n"
		"\r\n";
	if (os64_write((int32_t)h, request, sizeof(request) - 1) != (int64_t)(sizeof(request) - 1))
	{
		os64_debug_log("fetchtest: HTTP request write failed\n");
		os64_close((int32_t)h);
		return FETCH_BAD_WRITE;
	}

	// Read the stream to EOF. This is the canonical os64 filter loop —
	// identical to the one `cat` uses on a file or a pipe, which is the
	// entire point of the handle model: read returns SHORT, and 0 is the
	// end (here, the peer's FIN after the last byte of the page).
	char status[128];
	uint32_t status_len = 0;
	uint64_t total = 0;
	char buf[1024];
	for (;;)
	{
		int64_t n = os64_read((int32_t)h, buf, sizeof(buf));
		if (n <= 0)
			break;
		// Capture the status line — everything up to the first CR.
		for (int64_t i = 0; i < n && status_len < sizeof(status) - 1; i++)
		{
			if (buf[i] == '\r' || buf[i] == '\n')
			{
				if (status_len > 0)
					status_len = (uint32_t)sizeof(status);   // sealed: stop capturing
				break;
			}
			status[status_len++] = buf[i];
		}
		if (status_len == sizeof(status))
			status_len = (uint32_t)(sizeof(status) - 1);     // unseal for printing
		total += (uint64_t)n;
	}
	os64_close((int32_t)h);

	if (total == 0)
	{
		os64_debug_log("fetchtest: no response bytes\n");
		return FETCH_BAD_READ;
	}
	status[status_len] = '\0';

	os64_printf("fetchtest: %u.%u.%u.%u said \"%s\" (%lu bytes of %s)\n",
	            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
	            status, total, HOSTNAME);

	os64_debug_log("fetchtest: success!\n");
	return FETCH_OK;
}
