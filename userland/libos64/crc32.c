// crc32.c — CRC-32/ISO-HDLC, the bitwise form.
//
// See crc32.h for why this variant and not another. This file is about how.
//
// NO 256-ENTRY TABLE, deliberately. libgzip makes CRC32 a bulk-data path now,
// alongside a decoder that is itself bit-oriented; changing both before an
// end-to-end measurement would guess which loop matters. The bitwise form
// keeps the whole algorithm visible in nine lines: no generated constants to
// audit and a reader can confirm the standard polynomial by looking rather
// than trusting. If a profile puts meaningful time here, a table can replace
// this loop behind the same interface and the host vectors prove it agrees.
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
