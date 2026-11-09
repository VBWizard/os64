// blake2s.c — BLAKE2s per RFC 7693, straight from the specification's
// pseudocode. The mixing function G, the ten-round compression with the
// sigma permutation, the parameter block folded into the IV, and the
// finalisation flag: nothing here is original, and that is the point.
// blake2s.h says why; tools/test_random_host.c proves it against hashlib.

#include "crypto/blake2s.h"
#include "crypto/wipe.h"

static const uint32_t kIV[8] = {
	0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
	0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

// RFC 7693 §2.7: the message word schedule, one row per round.
static const uint8_t kSigma[10][16] = {
	{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
	{ 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
	{  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
	{  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
	{  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
	{ 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
	{ 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
	{  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
	{ 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
};

static inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static inline uint32_t load32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// §3.1 G: mix two message words into four state words.
#define G(r, i, a, b, c, d)                              \
	do {                                                 \
		a = a + b + m[kSigma[r][2 * (i)]];               \
		d = rotr32(d ^ a, 16);                           \
		c = c + d;                                       \
		b = rotr32(b ^ c, 12);                           \
		a = a + b + m[kSigma[r][2 * (i) + 1]];           \
		d = rotr32(d ^ a, 8);                            \
		c = c + d;                                       \
		b = rotr32(b ^ c, 7);                            \
	} while (0)

// §3.2 F: compress one 64-byte block. `last` sets the finalisation flag.
static void blake2s_compress(blake2s_state_t* s, const uint8_t block[BLAKE2S_BLOCK_BYTES], int last)
{
	uint32_t m[16], v[16];
	for (int i = 0; i < 16; i++)
		m[i] = load32(block + 4 * i);
	for (int i = 0; i < 8; i++)
	{
		v[i] = s->h[i];
		v[i + 8] = kIV[i];
	}
	v[12] ^= s->t[0];
	v[13] ^= s->t[1];
	if (last)
		v[14] = ~v[14];

	for (int r = 0; r < 10; r++)
	{
		G(r, 0, v[0], v[4], v[8],  v[12]);
		G(r, 1, v[1], v[5], v[9],  v[13]);
		G(r, 2, v[2], v[6], v[10], v[14]);
		G(r, 3, v[3], v[7], v[11], v[15]);
		G(r, 4, v[0], v[5], v[10], v[15]);
		G(r, 5, v[1], v[6], v[11], v[12]);
		G(r, 6, v[2], v[7], v[8],  v[13]);
		G(r, 7, v[3], v[4], v[9],  v[14]);
	}
	for (int i = 0; i < 8; i++)
		s->h[i] ^= v[i] ^ v[i + 8];
	// In the pool's fold the first block's m[0..7] is the old key, and at
	// -O0 both arrays live in this frame.
	crypto_wipe(m, sizeof(m));
	crypto_wipe(v, sizeof(v));
}

void blake2s_init(blake2s_state_t* s, size_t outlen, const void* key, size_t keylen)
{
	if (outlen == 0 || outlen > BLAKE2S_OUT_BYTES)
		outlen = BLAKE2S_OUT_BYTES;
	if (keylen > BLAKE2S_KEY_MAX)
		keylen = BLAKE2S_KEY_MAX;
	for (int i = 0; i < 8; i++)
		s->h[i] = kIV[i];
	// §2.5 parameter block, sequential mode: digest length, key length,
	// fanout 1, depth 1 — folded into h[0] as one little-endian word.
	s->h[0] ^= 0x01010000u ^ ((uint32_t)keylen << 8) ^ (uint32_t)outlen;
	s->t[0] = 0;
	s->t[1] = 0;
	s->buflen = 0;
	s->outlen = outlen;
	for (size_t i = 0; i < BLAKE2S_BLOCK_BYTES; i++)
		s->buf[i] = 0;
	if (keylen)
	{
		// A key is the first block, zero-padded to 64 bytes.
		const uint8_t* k = key;
		for (size_t i = 0; i < keylen; i++)
			s->buf[i] = k[i];
		s->buflen = BLAKE2S_BLOCK_BYTES;
	}
}

static void blake2s_count(blake2s_state_t* s, uint32_t inc)
{
	s->t[0] += inc;
	if (s->t[0] < inc)
		s->t[1]++;
}

void blake2s_update(blake2s_state_t* s, const void* in, size_t inlen)
{
	const uint8_t* p = in;
	while (inlen > 0)
	{
		// A full buffer is compressed only when MORE input follows: the
		// last block must reach final() with the flag set (§3.3).
		if (s->buflen == BLAKE2S_BLOCK_BYTES)
		{
			blake2s_count(s, BLAKE2S_BLOCK_BYTES);
			blake2s_compress(s, s->buf, 0);
			s->buflen = 0;
		}
		size_t take = BLAKE2S_BLOCK_BYTES - s->buflen;
		if (take > inlen)
			take = inlen;
		for (size_t i = 0; i < take; i++)
			s->buf[s->buflen + i] = p[i];
		s->buflen += take;
		p += take;
		inlen -= take;
	}
}

void blake2s_final(blake2s_state_t* s, void* out)
{
	blake2s_count(s, (uint32_t)s->buflen);
	for (size_t i = s->buflen; i < BLAKE2S_BLOCK_BYTES; i++)
		s->buf[i] = 0;
	blake2s_compress(s, s->buf, 1);
	uint8_t full[BLAKE2S_OUT_BYTES];
	for (int i = 0; i < 8; i++)
		store32(full + 4 * i, s->h[i]);
	uint8_t* o = out;
	for (size_t i = 0; i < s->outlen; i++)
		o[i] = full[i];
	// The state is spent, and the digest scratch is the pool's next key
	// when the caller is key_fold: wipe both in a way -O2 cannot drop.
	crypto_wipe(full, sizeof(full));
	crypto_wipe(s->buf, sizeof(s->buf));
	crypto_wipe(s->h, sizeof(s->h));
}

void blake2s(void* out, size_t outlen, const void* key, size_t keylen,
             const void* in, size_t inlen)
{
	blake2s_state_t s;
	blake2s_init(&s, outlen, key, keylen);
	blake2s_update(&s, in, inlen);
	blake2s_final(&s, out);
}
