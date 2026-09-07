#ifndef CRYPTO_BLAKE2S_H
#define CRYPTO_BLAKE2S_H

// blake2s.h — BLAKE2s (RFC 7693), the entropy pool's mixing function.
//
// Why this hash and not another: the pool folds entropy of unknown quality
// into a fixed-size key, and the fold must be one-way and collision
// resistant or a bad source could steer the key. BLAKE2s is what Linux's
// pool has used since 5.17, it is ~200 lines with no tables beyond a
// 16-entry permutation, and RFC 7693 carries test vectors — so the host
// harness diffs this file against Python's hashlib before the kernel ever
// runs it (tools/test_random_host.c). 32-bit words on purpose: the pool
// is not a throughput customer, and BLAKE2s is the variant whose reference
// Python exposes without a dependency.
//
// NO KERNEL HEADERS in this file or its .c: the host test compiles them
// with the host's gcc, the r8125_ring.c pattern.

#include <stdint.h>
#include <stddef.h>

#define BLAKE2S_BLOCK_BYTES 64
#define BLAKE2S_OUT_BYTES   32   // the only output size the pool uses
#define BLAKE2S_KEY_MAX     32

typedef struct
{
	uint32_t h[8];
	uint32_t t[2];                    // bytes hashed so far, 64-bit counter
	uint8_t  buf[BLAKE2S_BLOCK_BYTES];
	size_t   buflen;
	size_t   outlen;
} blake2s_state_t;

// Unkeyed or keyed (key may be NULL with keylen 0). outlen 1..32.
void blake2s_init(blake2s_state_t* s, size_t outlen, const void* key, size_t keylen);
void blake2s_update(blake2s_state_t* s, const void* in, size_t inlen);
void blake2s_final(blake2s_state_t* s, void* out);

// One-shot convenience: the whole message at once.
void blake2s(void* out, size_t outlen, const void* key, size_t keylen,
             const void* in, size_t inlen);

#endif
