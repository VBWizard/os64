// chacha20.c — RFC 8439 §2.3's block function, as written there: the
// "expand 32-byte k" constants, the key in words 4..11, the counter in
// word 12, the nonce in 13..15, twenty rounds of the quarter round in the
// column/diagonal pattern, then the initial state added back. chacha20.h
// says what this is for and what it deliberately is not.

#include "crypto/chacha20.h"

static inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

static inline uint32_t load32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// §2.1
#define QR(a, b, c, d)                          \
	do {                                        \
		a += b; d ^= a; d = rotl32(d, 16);      \
		c += d; b ^= c; b = rotl32(b, 12);      \
		a += b; d ^= a; d = rotl32(d, 8);       \
		c += d; b ^= c; b = rotl32(b, 7);       \
	} while (0)

void chacha20_block(const uint8_t key[CHACHA20_KEY_BYTES], uint32_t counter,
                    const uint8_t nonce[CHACHA20_NONCE_BYTES],
                    uint8_t out[CHACHA20_BLOCK_BYTES])
{
	uint32_t s[16], x[16];
	s[0] = 0x61707865u; s[1] = 0x3320646Eu; s[2] = 0x79622D32u; s[3] = 0x6B206574u;
	for (int i = 0; i < 8; i++)
		s[4 + i] = load32(key + 4 * i);
	s[12] = counter;
	for (int i = 0; i < 3; i++)
		s[13 + i] = load32(nonce + 4 * i);

	for (int i = 0; i < 16; i++)
		x[i] = s[i];
	for (int r = 0; r < 10; r++)         // 20 rounds: a column round and a diagonal round, ten times
	{
		QR(x[0], x[4], x[8],  x[12]);
		QR(x[1], x[5], x[9],  x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8],  x[13]);
		QR(x[3], x[4], x[9],  x[14]);
	}
	for (int i = 0; i < 16; i++)
		store32(out + 4 * i, x[i] + s[i]);
}

void chacha20_stream(const uint8_t key[CHACHA20_KEY_BYTES], uint32_t counter,
                     const uint8_t nonce[CHACHA20_NONCE_BYTES],
                     uint8_t* out, size_t len)
{
	uint8_t block[CHACHA20_BLOCK_BYTES];
	while (len > 0)
	{
		chacha20_block(key, counter++, nonce, block);
		size_t take = len < CHACHA20_BLOCK_BYTES ? len : CHACHA20_BLOCK_BYTES;
		for (size_t i = 0; i < take; i++)
			out[i] = block[i];
		out += take;
		len -= take;
	}
	for (size_t i = 0; i < CHACHA20_BLOCK_BYTES; i++)
		block[i] = 0;
}
