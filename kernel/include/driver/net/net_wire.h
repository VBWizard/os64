#ifndef NET_WIRE_H
#define NET_WIRE_H

// net_wire.h — the byte-order boundary, in one place.
//
// Network byte order is BIG-endian ("the octet closest to the left margin
// ships first" — RFC 791, Appendix B) because the ARPAnet world of 1981 was
// built on big-endian iron: PDP-10s, IBM 360s, 68000s. x86 is little-endian,
// so every multi-byte field that crosses the wire gets swapped — HERE, and
// only here. This header is the concrete half of NETWORK.md's pending ruling
// #2 ("the kernel owns the wire"): applications will never see network byte
// order, htons() will never exist in libos64, and the entire swap surface of
// the operating system is the four helpers below.
//
// The helpers read and write wire fields BYTE AT A TIME on purpose — no
// casting a frame offset to uint16_t* and dereferencing. Packet headers land
// wherever the layers above dictate (an IPv4 address inside an ethernet
// frame sits at offset 26 or 30 — aligned to nothing), and x86's tolerance
// for misaligned loads is a habit we refuse to teach in a learner's OS: the
// same code on ARM would fault, and -fsanitize=alignment would agree with
// ARM. Byte loads compile to the same few instructions anyway.

#include <stdint.h>

// Read a big-endian 16/32-bit wire field into a host-order value.
static inline uint16_t net_read16(const void* p)
{
	const uint8_t* b = (const uint8_t*)p;
	return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}
static inline uint32_t net_read32(const void* p)
{
	const uint8_t* b = (const uint8_t*)p;
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

// Write a host-order value as a big-endian wire field.
static inline void net_write16(void* p, uint16_t v)
{
	uint8_t* b = (uint8_t*)p;
	b[0] = (uint8_t)(v >> 8);
	b[1] = (uint8_t)v;
}
static inline void net_write32(void* p, uint32_t v)
{
	uint8_t* b = (uint8_t*)p;
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}

// IPv4 addresses live in the kernel as HOST-ORDER uint32_t: 10.0.2.15 is
// 0x0A00020F — it reads left-to-right in a debugger, compares numerically,
// and subnet masks work with ordinary arithmetic. An address becomes
// big-endian only by passing through net_write32 at the packet boundary.
// This macro spells one from dotted parts so configuration defaults read
// like addresses instead of hex riddles.
#define NET_IPV4(a, b, c, d) \
	(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

// printd/printf helper: expands one host-order address into the four
// arguments a "%u.%u.%u.%u" format expects. A macro, not a to-string
// function, so log call sites need no scratch buffers.
#define NET_IPV4_OCTETS(ip) \
	(uint32_t)((ip) >> 24) & 0xFF, (uint32_t)((ip) >> 16) & 0xFF, \
	(uint32_t)((ip) >> 8) & 0xFF, (uint32_t)(ip) & 0xFF

#endif // NET_WIRE_H
