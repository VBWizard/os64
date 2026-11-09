// dialtest — the Phase 3 milestone fixture: ring 3 places a real call.
//
// This program is the constitution's "userland fixture round-trips
// datagrams with the host" (NETWORK.md Phase 3), and it exercises the
// ENTIRE tower in one conversation: os64_dial's bang-path parser →
// the net_dial syscall → a HANDLE_NET_UDP → write (one datagram out the
// virtio ring) → a blocking read that parks this task on the SIGSLEEP
// machinery until the answer climbs back up eth→ipv4→udp→conn queue →
// wake sweep → here.
//
// The peer is slirp's DNS forwarder (10.0.2.3, the third address of the
// NAT convention), and the payload is a genuine DNS question — "A records
// for example.com?" — hand-rolled, because DNS is a USERLAND protocol by
// constitutional ruling (the kernel moves datagrams, never parses names).
// Any syntactically-sane answer passes: we are testing the phone line,
// not the phone book (the resolver behind slirp may answer, SERVFAIL, or
// NXDOMAIN — all of those are the wire WORKING).
//
// Exit codes are the ring3-fixture convention: one magic success value,
// distinct step codes for the autopsy.
//   0x0D1A1600  success ("DIAL" + 00)      0x0D1A1603  write failed
//   0x0D1A1601  dial string refused        0x0D1A1604  read failed/short
//   0x0D1A1602  (reserved)                 0x0D1A1605  reply id mismatch
//                                          0x0D1A1606  reply not a response
// NOT linked with the test harness — a normal /bin app; the kernel test
// spawns it by path and reads the exit code.

#include "os64/os64.h"

#define DIALTEST_OK           0x0D1A1600
#define DIALTEST_BAD_DIAL     0x0D1A1601
#define DIALTEST_BAD_WRITE    0x0D1A1603
#define DIALTEST_BAD_READ     0x0D1A1604
#define DIALTEST_BAD_ID       0x0D1A1605
#define DIALTEST_NOT_RESPONSE 0x0D1A1606

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	int64_t h = os64_dial("udp!10.0.2.3!53");
	if (h < 0)
    {
        os64_debug_log("testdial: os64_dial() failed\n");
        return DIALTEST_BAD_DIAL;
    }

	// A minimal DNS query, RFC 1035 wire format. DNS is big-endian on the
	// wire like everything from 1983 — and note this is APPLICATION bytes:
	// the host-order ABI rule governs syscall structs, not what a program
	// chooses to put in its own datagrams. Apps that speak wire protocols
	// own their own wire encoding; that's what "DNS is userland" means.
	//
	//   header: id=0x6F34 ("o4"), flags=0x0100 (RD: recurse please),
	//           1 question, 0 answers/authority/additional
	//   question: 7"example"3"com"0, type A (1), class IN (1)
	uint8_t query[29] = {
		0x6F, 0x34,  0x01, 0x00,  0x00, 0x01,  0x00, 0x00,
		0x00, 0x00,  0x00, 0x00,
		7, 'e','x','a','m','p','l','e',  3, 'c','o','m',  0,
		0x00, 0x01,  0x00, 0x01,
	};

	if (os64_write((int32_t)h, query, sizeof(query)) != (int64_t)sizeof(query))
    {
        os64_debug_log("testdial: os64_write() failed\n");
        return DIALTEST_BAD_WRITE;
    }
	// One read = one datagram (the whole answer, however big, in one call).
	// This BLOCKS — the task genuinely parks at zero CPU until the reply
	// crosses two NATs and comes home. That park is half of what this
	// fixture exists to prove.
	uint8_t reply[512];   // classic UDP DNS fits in 512 (RFC 1035 §2.3.4)
	int64_t n = os64_read((int32_t)h, reply, sizeof(reply));
	os64_close((int32_t)h);

	if (n < 12)                                   // smaller than a DNS header
    {
        os64_debug_log("testdial: os64_read() failed\n");
        return DIALTEST_BAD_READ;
    }
	if (reply[0] != 0x6F || reply[1] != 0x34)     // answers OUR question?
    {
        os64_debug_log("testdial: read() - bad response\n");
        return DIALTEST_BAD_ID;
    }
	if ((reply[2] & 0x80) == 0)                   // QR bit: this IS a response?
    {
        os64_debug_log("testdial: read() - not response\n");
        return DIALTEST_NOT_RESPONSE;
    }

	// Read the LETTER, not just the envelope (Chris's amendment, 2026-08-01,
	// upon catching this fixture grading a reply it never opened): one line
	// on the boot display saying what the DNS server actually SAID. The
	// answer section is walked for the first A record — skipping the echoed
	// question, and skipping answer NAMEs which arrive either as inline
	// labels or as a 0xC0xx compression pointer (RFC 1035 §4.1.4: two bytes
	// that point back into the packet — 1983 fighting for every byte of a
	// 512-byte budget). CNAME records may precede the A; we walk past them.
	uint16_t ancount = (uint16_t)((reply[6] << 8) | reply[7]);
	uint8_t  rcode   = (uint8_t)(reply[3] & 0x0F);
	if (ancount == 0)
	{
		// The wire worked; the phone book had a bad day. Say which:
		// rcode 3 = no such name, 2 = server failure, 0 = "fine but empty".
		os64_printf("dialtest: DNS answered with NO records for example.com (rcode %u)\n", (uint32_t)rcode);
		return DIALTEST_OK;   // the fixture tests the phone LINE, not the book
	}

	int64_t off = 12;
	while (off < n && reply[off] != 0)            // skip the echoed question name
		off += reply[off] + 1;
	off += 1 + 4;                                 // its terminator + qtype/qclass

	uint32_t addr = 0, ttl = 0;
	for (uint16_t a = 0; a < ancount && off + 12 <= n; a++)
	{
		if ((reply[off] & 0xC0) == 0xC0)          // compressed name: 2-byte pointer
			off += 2;
		else                                      // inline name: labels to the zero
		{
			while (off < n && reply[off] != 0)
				off += reply[off] + 1;
			off += 1;
		}
		if (off + 10 > n)
			break;
		uint16_t rtype = (uint16_t)((reply[off] << 8) | reply[off + 1]);
		uint32_t rttl  = ((uint32_t)reply[off + 4] << 24) | ((uint32_t)reply[off + 5] << 16) |
		                 ((uint32_t)reply[off + 6] << 8)  |  (uint32_t)reply[off + 7];
		uint16_t rdlen = (uint16_t)((reply[off + 8] << 8) | reply[off + 9]);
		off += 10;
		if (rtype == 1 && rdlen == 4 && off + 4 <= n)   // an A record: the answer
		{
			addr = ((uint32_t)reply[off] << 24) | ((uint32_t)reply[off + 1] << 16) |
			       ((uint32_t)reply[off + 2] << 8) | (uint32_t)reply[off + 3];
			ttl = rttl;
			break;
		}
		off += rdlen;                             // a CNAME (or stranger): walk on
	}

	if (addr != 0)
		os64_printf("dialtest: DNS says example.com = %u.%u.%u.%u (%u answer%s, ttl %us)\n",
		            (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
		            (addr >> 8) & 0xFF, addr & 0xFF,
		            (uint32_t)ancount, ancount == 1 ? "" : "s", ttl);
	else
		os64_printf("dialtest: DNS sent %u answer%s but no A record (a CNAME maze?)\n",
		            (uint32_t)ancount, ancount == 1 ? "" : "s");

    os64_debug_log("testdial: success!\n");
    return DIALTEST_OK;
}
