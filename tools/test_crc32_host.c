// test_crc32_host.c — HOST-side unit test for libos64's CRC32.
//
// A checksum is the one piece of code whose bugs are invisible until the
// worst possible moment: it computes SOMETHING for every input, and a wrong
// variant looks perfectly healthy right up until the far end disagrees and
// every transfer starts failing for no visible reason. So it gets checked
// against PUBLISHED vectors rather than against itself — the whole point is
// agreeing with the rest of the world, and only the rest of the world's
// numbers can prove that.
//
// The canonical one is "123456789" -> 0xCBF43926, which every CRC catalogue
// lists for exactly this purpose. The others come from the same catalogues
// and from zlib, which is what the Python valet on the far end will use.
//
// Build & run (one line):
//   gcc -g -Wall -Wextra -I userland/libos64/include userland/libos64/crc32.c tools/test_crc32_host.c -o /tmp/os64_crc32_test && /tmp/os64_crc32_test

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "os64/crc32.h"

static int g_failures = 0;

static void expect(const char *what, uint32_t got, uint32_t want)
{
	if (got != want)
	{
		printf("FAIL %s: got 0x%08x want 0x%08x\n", what, got, want);
		g_failures++;
	}
}

static void expect_str(const char *s, uint32_t want)
{
	expect(s, os64_crc32(s, strlen(s)), want);
}

int main(void)
{
	// ── The published vectors ───────────────────────────────────────────
	// If any of these fails, the variant is wrong — not the arithmetic.
	expect_str("", 0x00000000u);
	expect_str("a", 0xE8B7BE43u);
	expect_str("abc", 0x352441C2u);
	expect_str("message digest", 0x20159D7Fu);
	expect_str("abcdefghijklmnopqrstuvwxyz", 0x4C2750BDu);
	// THE check value every CRC catalogue publishes so implementations can
	// prove they chose the same flavour. This one line is most of the test.
	expect_str("123456789", 0xCBF43926u);
	expect_str("The quick brown fox jumps over the lazy dog", 0x414FA339u);

	// ── Streaming must equal one-shot ───────────────────────────────────
	// This is the property os64get actually depends on: it checksums a file
	// as it arrives, in whatever sized pieces the network hands over, and
	// the answer has to match what the server computed over the whole thing
	// at once. Split at every possible boundary and demand agreement.
	{
		const char *msg = "The quick brown fox jumps over the lazy dog";
		size_t len = strlen(msg);
		uint32_t whole = os64_crc32(msg, len);

		for (size_t split = 0; split <= len; split++)
		{
			uint32_t s = os64_crc32_begin();
			s = os64_crc32_update(s, msg, split);
			s = os64_crc32_update(s, msg + split, len - split);
			if (os64_crc32_end(s) != whole)
			{
				printf("FAIL streaming split at %zu: 0x%08x != 0x%08x\n",
				       split, os64_crc32_end(s), whole);
				g_failures++;
			}
		}
	}

	// Many tiny chunks — the shape a slow wire actually produces.
	{
		const char *msg = "123456789";
		uint32_t s = os64_crc32_begin();
		for (size_t i = 0; i < 9; i++)
			s = os64_crc32_update(s, msg + i, 1);
		expect("byte-at-a-time 123456789", os64_crc32_end(s), 0xCBF43926u);
	}

	// A zero-length update in the middle must change nothing — a network
	// read CAN return a zero-length piece, and a checksum that flinched at
	// one would corrupt a transfer that was otherwise perfect.
	{
		const char *msg = "abc";
		uint32_t s = os64_crc32_begin();
		s = os64_crc32_update(s, msg, 1);
		s = os64_crc32_update(s, msg, 0);
		s = os64_crc32_update(s, msg + 1, 2);
		expect("zero-length update is a no-op", os64_crc32_end(s), 0x352441C2u);
	}

	// ── It must actually NOTICE damage ──────────────────────────────────
	// The reason the whole thing exists. Flip one bit in a payload the size
	// of a real binary and demand a different answer, at every byte
	// position — a checksum that agrees with a corrupted copy is worse than
	// no checksum, because it converts "the transfer failed" into "the
	// wrong file was installed".
	{
		size_t len = 4096;
		unsigned char *buf = malloc(len);
		for (size_t i = 0; i < len; i++)
			buf[i] = (unsigned char)(i * 31u + 7u);
		uint32_t good = os64_crc32(buf, len);

		int missed = 0;
		for (size_t i = 0; i < len; i++)
		{
			buf[i] ^= 0x01;                       // one flipped bit
			if (os64_crc32(buf, len) == good)
				missed++;
			buf[i] ^= 0x01;                       // put it back
		}
		if (missed != 0)
		{
			printf("FAIL single-bit detection: %d of %zu flips went unnoticed\n",
			       missed, len);
			g_failures++;
		}
		// And truncation, the other half of what a transfer gets wrong.
		if (os64_crc32(buf, len - 1) == good)
		{
			printf("FAIL truncation detection: short buffer matched\n");
			g_failures++;
		}
		free(buf);
	}

	if (g_failures == 0)
	{
		printf("crc32 tests: all passed\n");
		return 0;
	}
	printf("crc32 tests: %d FAILURE(S)\n", g_failures);
	return 1;
}
