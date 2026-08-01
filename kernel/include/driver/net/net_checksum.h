#ifndef NET_CHECKSUM_H
#define NET_CHECKSUM_H

// net_checksum.h — the internet checksum (RFC 1071), written ONCE.
//
// IPv4, ICMP, UDP, and TCP all use this same algorithm: the ones'-complement
// sum of the data taken as 16-bit big-endian words, complemented. It dates
// to the original 1981 protocol suite and was chosen for properties that
// mattered on a PDP-11: computable one word at a time in either byte order,
// incrementally updatable when a router decrements TTL (RFC 1141 is entirely
// about that trick), and just strong enough to catch the bit-rot of 1970s
// serial lines. It is NOT a CRC — reordered 16-bit words sum identically —
// but it's the contract, so it's what we compute. One module, four
// protocols, zero copies of the fold logic (NETWORK.md Phase 2 charter).

#include <stdint.h>

// Compute the internet checksum of a buffer. Returns the HOST-ORDER value
// ready to be placed with net_write16 into a header whose checksum field
// was ZERO during computation.
//
// Verification uses the algorithm's lovely self-property: summing a buffer
// that already CONTAINS its own checksum yields 0xFFFF, so this function
// returns 0 over a valid packet — "checksum ok" is `net_checksum(p, n) == 0`,
// no field-zeroing dance required on the receive side.
//
// An odd trailing byte is padded with a virtual zero (RFC 1071 §1) — the
// pad is arithmetic only, never transmitted.
//
uint16_t net_checksum(const void* data, uint32_t length);

// The accumulate API (arrived with UDP, exactly as promised): UDP and TCP
// checksum a PSEUDO-HEADER — src/dst address, protocol, length, fields
// borrowed from the IP layer — ahead of their real bytes. The pseudo-header
// was 1980's anti-misdelivery belt-and-suspenders: if a damaged IP header
// steered the datagram to the wrong host, the checksum (which baked in the
// intended addresses) would fail there. Sum the pieces with _add, seal with
// _fold:
//
//   uint32_t sum = net_checksum_add(0, pseudo, 12);
//   sum = net_checksum_add(sum, datagram, len);
//   uint16_t ck = net_checksum_fold(sum);
//
// RULE: an ODD-length chunk is only legal as the LAST chunk (the virtual
// zero pad ends the sum — RFC 1071's word alignment would otherwise shift
// every following byte). All wire pseudo-headers are even, so this never
// bites in practice; stated so it can't bite in silence either.
uint32_t net_checksum_add(uint32_t sum, const void* data, uint32_t length);
uint16_t net_checksum_fold(uint32_t sum);

#endif // NET_CHECKSUM_H
