#ifndef OS64_CRC32_H
#define OS64_CRC32_H

// crc32.h — the checksum that tells a good transfer from a plausible one.
//
// WHY THIS IS IN THE LIBRARY AND NOT IN os64get. A length check catches a
// transfer that stopped early, which is the failure everybody imagines. It
// says nothing at all about a transfer that arrived complete and WRONG —
// a flipped bit on the wire, a truncated write, a server that sent the
// file it had rather than the file you asked for. `os64 refresh` exists to
// replace the binaries this machine runs, so "complete" is not a high
// enough bar: the whole point of checking is to refuse to install
// something subtly damaged, and only a checksum can tell you that.
//
// This is CRC-32/ISO-HDLC — the one in Ethernet frames, PNG chunks, gzip,
// zip, and very nearly everything else that says "CRC32" without further
// qualification. The reflected polynomial 0xEDB88320 is 0x04C11DB7 read
// backwards, which is what "reflected" means here: the algorithm shifts
// right instead of left, so the polynomial's bits get written in the
// mirror. Choosing the SAME variant everyone else chose is the entire
// value proposition — the Python valet on the other end computes this with
// `zlib.crc32` and never has to be told which flavour we meant.
//
// It is NOT cryptographic. CRC32 detects accidents — noise, truncation, a
// dropped block — and is trivial to forge on purpose. That is the right
// tool for an isolated two-node build segment and the wrong one for a
// hostile network, and the day os64 fetches from somewhere it does not
// control, this needs to become a real hash rather than be trusted harder.

#include <stdint.h>
#include <stddef.h>

// One-shot: the CRC32 of a buffer.
uint32_t os64_crc32(const void *data, size_t length);

// Streaming, for data that arrives in pieces — a socket transfer and a gzip
// decoder both checksum bytes they never hold in memory all at once.
//
// Start with os64_crc32_begin(), feed each chunk in order, finish with
// os64_crc32_end(). The value carried in between is NOT the CRC of what has
// been fed so far — it is the running state, pre-inversion — which is
// exactly why it has its own accessor instead of being read directly. Every
// CRC32 bug in the world is somebody inverting once, twice, or never.
//
//     uint32_t s = os64_crc32_begin();
//     while ((n = read(...)) > 0) s = os64_crc32_update(s, buf, n);
//     uint32_t crc = os64_crc32_end(s);
uint32_t os64_crc32_begin(void);
uint32_t os64_crc32_update(uint32_t state, const void *data, size_t length);
uint32_t os64_crc32_end(uint32_t state);

#endif // OS64_CRC32_H
