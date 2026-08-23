// resolve.c — the hosts files, then one DNS question. Contract in
// os64/resolve.h; the lineage is there too.
//
// Everything here is deliberately small. A hosts line is three tokens; a
// DNS query is twelve bytes of header, the name as length-prefixed labels,
// and four bytes of type/class; the answer is walked once looking for the
// first A record. The parts of RFC 1035 this does NOT do (compression on
// the way OUT, multiple questions, TCP fallback for truncated answers,
// EDNS) are the parts no consumer has asked for.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "os64/resolve.h"
#include "os64/io.h"
#include "os64/str.h"
#include "os64/conf.h"
#include "os64/fmt.h"     // os64_snprintf — the dial string for the name server
#include "os64/dial.h"
#include "os64/net.h"
#include "os64/proc.h"    // os64_ticks — entropy enough for a query id

#define DNS_PORT          53
#define DNS_TRIES         2
#define DNS_TRY_MS        2500       // x2 = five seconds of patience, total
#define DNS_MAX_PACKET    512        // RFC 1035 §4.2.1: UDP answers fit in 512

// ── the quad ────────────────────────────────────────────────────────────────

bool os64_parse_ipv4(const char *s, const char *end, uint32_t *ip)
{
	uint32_t acc = 0;
	for (int octet = 0; octet < 4; octet++) {
		if (s >= end || *s < '0' || *s > '9')
			return false;
		uint32_t v = 0;
		while (s < end && *s >= '0' && *s <= '9') {
			v = v * 10 + (uint32_t)(*s - '0');
			if (v > 255)
				return false;
			s++;
		}
		acc = (acc << 8) | v;
		if (octet < 3) {
			if (s >= end || *s != '.')
				return false;
			s++;
		}
	}
	if (s != end)
		return false;
	*ip = acc;
	return true;
}

size_t os64_format_ipv4(uint32_t ip, char *buf, size_t cap)
{
	if (buf == NULL || cap < OS64_IPV4_STR_MAX)
		return 0;
	int32_t n = os64_snprintf(buf, cap, "%u.%u.%u.%u",
	                          (ip >> 24) & 255, (ip >> 16) & 255,
	                          (ip >> 8) & 255, ip & 255);
	return n > 0 ? (size_t)n : 0;
}

// Case-insensitive equality for names — DNS is case-insensitive (RFC 1035
// §2.3.3) and a hosts file that wasn't would be a trap.
static bool name_eq(const char *a, const char *a_end, const char *b)
{
	while (a < a_end && *b) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
		if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
		if (ca != cb)
			return false;
		a++; b++;
	}
	return a == a_end && *b == '\0';
}

// ── the hosts files ─────────────────────────────────────────────────────────
// `addr name [alias...]`, whitespace-separated, '#' to end of line. The
// first line whose names include `name` wins; the files are read in the
// order given, so the caller's ordering IS the precedence.

static bool hosts_lookup_file(const char *path, const char *name, uint32_t *ip)
{
	int64_t h = os64_open(path, "r");
	if (h < 0)
		return false;

	char line[512];
	bool found = false;
	while (!found && os64_readline((int32_t)h, line, sizeof(line)) == 1) {
		// chop the comment and the terminator
		char *p = line;
		while (*p && *p != '#' && *p != '\n' && *p != '\r')
			p++;
		*p = '\0';

		// token 1: the address
		p = line;
		while (*p == ' ' || *p == '\t') p++;
		char *tok = p;
		while (*p && *p != ' ' && *p != '\t') p++;
		if (tok == p)
			continue;                       // blank / comment-only
		uint32_t addr;
		if (!os64_parse_ipv4(tok, p, &addr))
			continue;                       // not an address: not our business

		// tokens 2..n: the names
		for (;;) {
			while (*p == ' ' || *p == '\t') p++;
			tok = p;
			while (*p && *p != ' ' && *p != '\t') p++;
			if (tok == p)
				break;
			if (name_eq(tok, p, name)) {
				*ip = addr;
				found = true;
				break;
			}
		}
	}
	os64_close((int32_t)h);
	return found;
}

static const char *const kHostsPaths[] = { "/home/hosts", "/etc/hosts" };

// ── which name server ───────────────────────────────────────────────────────

static bool nameserver_conf_line(const char *key, const char *value, void *user)
{
	uint32_t *out = (uint32_t *)user;
	if (key != NULL && os64_streq(key, "nameserver")) {
		uint32_t ip;
		if (os64_parse_ipv4(value, value + os64_strlen(value), &ip))
			*out = ip;                      // last one wins, as everywhere
	}
	return true;                            // other keys belong to other readers
}

static const char *const kNetConfPaths[] = { "/home/net.conf", "/etc/net.conf" };

static bool nameserver_from_sys(uint32_t *ip)
{
	int64_t h = os64_open("/sys/net/dhcp", "r");
	if (h < 0)
		return false;
	char line[128];
	bool found = false;
	while (!found && os64_readline((int32_t)h, line, sizeof(line)) == 1) {
		// "dns: a.b.c.d\n" — the contract sysfs.c keeps for exactly this reader
		if (line[0] != 'd' || line[1] != 'n' || line[2] != 's' || line[3] != ':')
			continue;
		const char *s = line + 4;
		while (*s == ' ') s++;
		const char *e = s;
		while (*e && *e != '\n' && *e != '\r') e++;
		found = os64_parse_ipv4(s, e, ip);  // "none" parses false, correctly
	}
	os64_close((int32_t)h);
	return found;
}

static bool find_nameserver(uint32_t *ip)
{
	uint32_t fromConf = 0;
	os64_conf_read_first(kNetConfPaths, 2, nameserver_conf_line, &fromConf, NULL);
	if (fromConf != 0) {
		*ip = fromConf;
		return true;
	}
	return nameserver_from_sys(ip);
}

// ── one question, one answer ────────────────────────────────────────────────

static size_t put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return 2; }
static uint16_t get16(const uint8_t *p)    { return (uint16_t)((p[0] << 8) | p[1]); }

// Skip a (possibly compressed) name in an answer. Returns the new offset, or
// 0 if the packet ends first — a malformed answer is refused, not guessed.
static size_t skip_name(const uint8_t *pkt, size_t len, size_t off)
{
	for (;;) {
		if (off >= len)
			return 0;
		uint8_t l = pkt[off];
		if (l == 0)
			return off + 1;
		if ((l & 0xC0) == 0xC0)             // a pointer: two bytes, then done
			return (off + 2 <= len) ? off + 2 : 0;
		off += 1 + l;
	}
}

static int64_t dns_query(uint32_t server, const char *name, uint32_t *ip)
{
	// Build the question. Labels are the dotted pieces, length-prefixed,
	// terminated by a zero label; each piece must be 1..63 bytes.
	uint8_t q[DNS_MAX_PACKET];
	size_t n = 12;
	const char *p = name;
	while (*p) {
		const char *dot = p;
		while (*dot && *dot != '.') dot++;
		size_t l = (size_t)(dot - p);
		if (l == 0 || l > 63 || n + 1 + l + 5 > sizeof(q))
			return OS64_NET_ERR_NO_SUCH_HOST;   // not a name DNS could hold
		q[n++] = (uint8_t)l;
		for (size_t i = 0; i < l; i++) q[n++] = (uint8_t)p[i];
		p = *dot ? dot + 1 : dot;
	}
	q[n++] = 0;
	n += put16(q + n, 1);                   // QTYPE A
	n += put16(q + n, 1);                   // QCLASS IN

	os64_ticks_t t;
	os64_ticks(&t);
	uint16_t id = (uint16_t)(t.ticks ^ (t.ticks >> 16) ^ 0x6f73);   // 'os'
	put16(q + 0, id);
	put16(q + 2, 0x0100);                   // RD: please recurse for us
	put16(q + 4, 1);                        // one question
	put16(q + 6, 0); put16(q + 8, 0); put16(q + 10, 0);

	char quad[OS64_IPV4_STR_MAX];
	os64_format_ipv4(server, quad, sizeof(quad));
	char dial[40];
	os64_snprintf(dial, sizeof(dial), "udp!%s!%d", quad, DNS_PORT);
	int64_t h = os64_dial(dial);
	if (h < 0)
		return h;                           // the dial table's own verdict

	int64_t rc = OS64_NET_ERR_TIMEOUT;
	for (int attempt = 0; attempt < DNS_TRIES && rc == OS64_NET_ERR_TIMEOUT; attempt++) {
		if (os64_write((int32_t)h, q, n) != (int64_t)n) {
			rc = OS64_NET_ERR_NO_RESOURCES;
			break;
		}
		uint8_t a[DNS_MAX_PACKET];
		int64_t got = os64_read_for((int32_t)h, a, sizeof(a), DNS_TRY_MS);
		if (got == OS64_ERR_TIMEOUT)
			continue;                       // ask again
		if (got < 12) {
			rc = OS64_NET_ERR_NO_SUCH_HOST; // garbage is not an answer
			break;
		}
		size_t len = (size_t)got;
		if (get16(a) != id || !(a[2] & 0x80))
			continue;                       // not ours, or not an answer: keep waiting
		uint8_t rcode = a[3] & 0x0F;
		if (rcode != 0) {
			rc = OS64_NET_ERR_NO_SUCH_HOST; // 3 = NXDOMAIN; anything else, same to us
			break;
		}
		uint16_t qd = get16(a + 4), an = get16(a + 6);
		size_t off = 12;
		for (uint16_t i = 0; i < qd && off; i++) {
			off = skip_name(a, len, off);
			if (off) off += 4;              // QTYPE, QCLASS
		}
		rc = OS64_NET_ERR_NO_SUCH_HOST;     // until an A record says otherwise
		for (uint16_t i = 0; i < an && off && off + 10 <= len; i++) {
			off = skip_name(a, len, off);
			if (!off || off + 10 > len)
				break;
			uint16_t type = get16(a + off), cls = get16(a + off + 2);
			uint16_t rdlen = get16(a + off + 8);
			off += 10;
			if (off + rdlen > len)
				break;
			if (type == 1 && cls == 1 && rdlen == 4) {
				*ip = ((uint32_t)a[off] << 24) | ((uint32_t)a[off + 1] << 16) |
				      ((uint32_t)a[off + 2] << 8) | a[off + 3];
				rc = 0;
				break;
			}
			off += rdlen;                   // a CNAME on the way to the A, usually
		}
		break;
	}
	os64_close((int32_t)h);
	return rc;
}

// ── the public verb ─────────────────────────────────────────────────────────

int64_t os64_resolve(const char *name, uint32_t *ip)
{
	if (name == NULL || ip == NULL || *name == '\0')
		return OS64_NET_ERR_BAD_ADDRESS;
	size_t len = os64_strlen(name);
	if (len > OS64_RESOLVE_NAME_MAX)
		return OS64_NET_ERR_BAD_ADDRESS;

	// An address is its own answer.
	if (os64_parse_ipv4(name, name + len, ip))
		return 0;

	// The room first: /home/hosts, then /etc/hosts, merged.
	for (size_t i = 0; i < sizeof(kHostsPaths) / sizeof(kHostsPaths[0]); i++)
		if (hosts_lookup_file(kHostsPaths[i], name, ip))
			return 0;

	// Then the world.
	uint32_t server;
	if (!find_nameserver(&server))
		return OS64_NET_ERR_NO_RESOLVER;
	return dns_query(server, name, ip);
}
