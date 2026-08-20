// crc32.c — CRC-32/ISO-HDLC, the bitwise form.
//
// See crc32.h for why this variant and not another. This file is about how.
//
// NO 256-ENTRY TABLE, deliberately. The table version is roughly eight
// times faster and is what you would reach for in a hot path — but this
// runs once per transferred file, on data that just crossed a wire at a few
// hundred kilobytes a second, so the checksum is not remotely the slow part.
// What the bitwise form buys instead is that the whole algorithm is visible
// in nine lines: no generated constants to get subtly wrong, nothing to
// regenerate if the polynomial ever changes, and a reader can confirm it is
// the standard CRC32 by looking rather than by trusting. If a consumer ever
// appears that checksums megabytes in a loop, add the table THEN, behind
// this same interface, with the host test as the proof it still agrees.
//
// THE THREE PLACES CRC32 IMPLEMENTATIONS GO WRONG, all of them here:
//   1. the initial value is 0xFFFFFFFF, not 0
//   2. the final value is inverted, and exactly once
//   3. the polynomial is REFLECTED (0xEDB88320) because the algorithm
//      shifts right; using 0x04C11DB7 with a right shift computes a
//      perfectly deterministic number that nobody else in the world agrees
//      with, which is the worst possible outcome — it looks like it works
//      until the other end disagrees
//
// tools/test_crc32_host.c checks all of it against the published vectors,
// including the canonical "123456789" -> 0xCBF43926 that every CRC catalogue
// lists precisely so implementations can prove they picked the same variant.

#include "os64/crc32.h"

// 0xEDB88320 is 0x04C11DB7 bit-reversed — see the header.
#define OS64_CRC32_POLY 0xEDB88320u

uint32_t os64_crc32_begin(void)
{
	return 0xFFFFFFFFu;
}

uint32_t os64_crc32_update(uint32_t state, const void *data, size_t length)
{
	const unsigned char *p = (const unsigned char *)data;

	for (size_t i = 0; i < length; i++)
	{
		state ^= p[i];
		// Eight bits, one at a time: shift the register right, and XOR the
		// polynomial back in whenever the bit shifted OUT was set. That is
		// the whole of CRC — long division over GF(2), where subtraction
		// and addition are both XOR.
		for (int bit = 0; bit < 8; bit++)
			state = (state >> 1) ^ (OS64_CRC32_POLY & (uint32_t)(-(int32_t)(state & 1)));
	}
	return state;
}

uint32_t os64_crc32_end(uint32_t state)
{
	return state ^ 0xFFFFFFFFu;
}

uint32_t os64_crc32(const void *data, size_t length)
{
	return os64_crc32_end(os64_crc32_update(os64_crc32_begin(), data, length));
}
