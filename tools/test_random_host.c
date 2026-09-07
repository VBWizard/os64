// test_random_host.c — HOST-side proof of the entropy pool's primitives.
//
// BLAKE2s (kernel/src/crypto/blake2s.c) against Python's hashlib, over
// messages of many lengths, keyed and unkeyed, at three digest sizes, fed
// both whole and in one-byte updates; ChaCha20 (crypto/chacha20.c) against
// RFC 8439's §2.3.2 block vector and the `cryptography` package's keystream.
// The vectors arrive as a generated header (tools/test_random_host.py).
//
//   bash tools/test_random_host.sh
//
// The pool's own behaviour (random.c) is tested below the primitives once
// it exists; this file grows with it.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto/blake2s.h"
#include "crypto/chacha20.h"
#include VECTORS_HEADER

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_blake2s(void)
{
	for (size_t i = 0; i < BLAKE2S_CASES; i++)
	{
		uint8_t got[BLAKE2S_OUT_BYTES];
		// Whole message at once.
		blake2s(got, kBlake2sCases[i].outlen, kBlake2sCases[i].key, kBlake2sCases[i].keylen,
		        kBlake2sCases[i].msg, kBlake2sCases[i].msglen);
		CHECK(memcmp(got, kBlake2sCases[i].digest, kBlake2sCases[i].outlen) == 0,
		      "blake2s case %zu (len %zu key %zu out %zu) whole", i,
		      kBlake2sCases[i].msglen, kBlake2sCases[i].keylen, kBlake2sCases[i].outlen);
		// One byte per update: the block boundary logic, every offset.
		blake2s_state_t s;
		blake2s_init(&s, kBlake2sCases[i].outlen, kBlake2sCases[i].key, kBlake2sCases[i].keylen);
		for (size_t j = 0; j < kBlake2sCases[i].msglen; j++)
			blake2s_update(&s, kBlake2sCases[i].msg + j, 1);
		blake2s_final(&s, got);
		CHECK(memcmp(got, kBlake2sCases[i].digest, kBlake2sCases[i].outlen) == 0,
		      "blake2s case %zu bytewise", i);
		// And in 7-byte pieces, which straddle blocks unevenly.
		blake2s_init(&s, kBlake2sCases[i].outlen, kBlake2sCases[i].key, kBlake2sCases[i].keylen);
		for (size_t j = 0; j < kBlake2sCases[i].msglen; j += 7)
		{
			size_t take = kBlake2sCases[i].msglen - j < 7 ? kBlake2sCases[i].msglen - j : 7;
			blake2s_update(&s, kBlake2sCases[i].msg + j, take);
		}
		blake2s_final(&s, got);
		CHECK(memcmp(got, kBlake2sCases[i].digest, kBlake2sCases[i].outlen) == 0,
		      "blake2s case %zu in sevens", i);
	}
}

static void test_chacha20(void)
{
	for (size_t i = 0; i < CHACHA_CASES; i++)
	{
		uint8_t* got = malloc(kChachaCases[i].len);
		chacha20_stream(kChachaCases[i].key, kChachaCases[i].counter, kChachaCases[i].nonce,
		                got, kChachaCases[i].len);
		CHECK(memcmp(got, kChachaCases[i].stream, kChachaCases[i].len) == 0,
		      "chacha20 case %zu (counter %u len %zu)", i, kChachaCases[i].counter, kChachaCases[i].len);
		free(got);
	}
	// The block function alone, on the RFC's anchor, first 16 bytes spelled here
	// so a broken generator cannot hide behind a broken reference.
	uint8_t key[32];
	for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
	static const uint8_t nonce[12] = {0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0};
	static const uint8_t want[16] = {0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
	                                 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4};
	uint8_t block[64];
	chacha20_block(key, 1, nonce, block);
	CHECK(memcmp(block, want, 16) == 0, "RFC 8439 2.3.2 block");
}

int main(void)
{
	test_blake2s();
	test_chacha20();
	if (failures)
	{
		printf("random host: %d FAILURES\n", failures);
		return 1;
	}
	printf("random host: BLAKE2s (%d cases x3 feeds) and ChaCha20 (%d cases + RFC anchor) PASS\n",
	       (int)BLAKE2S_CASES, (int)CHACHA_CASES);
	return 0;
}
