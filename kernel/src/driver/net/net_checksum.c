// net_checksum.c — RFC 1071, the whole thing. See net_checksum.h for why
// this small file gets its own module: four protocols share it, and a
// checksum bug that exists in one place gets fixed in one place.

#include "driver/net/net_checksum.h"

uint32_t net_checksum_add(uint32_t sum, const void* data, uint32_t length)
{
	const uint8_t* b = (const uint8_t*)data;

	// Sum the buffer as big-endian 16-bit words. Accumulating in 32 bits
	// defers the carries; even a full frame of 0xFFFF words stays orders
	// of magnitude below 32-bit overflow, so accumulate-then-fold is
	// always enough arithmetic headroom.
	while (length >= 2)
	{
		sum += ((uint32_t)b[0] << 8) | b[1];
		b += 2;
		length -= 2;
	}
	if (length)   // odd trailing byte: high half of a word whose low byte is
	              // the virtual zero pad — legal only on the LAST chunk (.h)
		sum += (uint32_t)b[0] << 8;
	return sum;
}

uint16_t net_checksum_fold(uint32_t sum)
{
	// Fold the deferred carries back into 16 bits. Ones'-complement
	// addition is "add, then add the carry back in" — the loop runs at
	// most twice (the first fold can itself carry, the second cannot).
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);

	// Complement. (The wire convention has one wrinkle we inherit for
	// free: a computed 0x0000 is transmitted as 0xFFFF — same value in
	// ones'-complement arithmetic — because all-zeros means "no checksum"
	// in UDP. udp.c applies that substitution; IP and ICMP never need it.)
	return (uint16_t)~sum;
}

uint16_t net_checksum(const void* data, uint32_t length)
{
	return net_checksum_fold(net_checksum_add(0, data, length));
}
