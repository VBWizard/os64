#ifndef CRYPTO_CHACHA20_H
#define CRYPTO_CHACHA20_H

// chacha20.h — the ChaCha20 block function (RFC 8439 §2.3), the entropy
// pool's generator.
//
// The pool hands out bytes by running ChaCha20 as a stream from its
// 256-bit key and keeping the first 32 bytes of each run as the NEXT key
// (fast key erasure, random.c). What this file offers is exactly the block
// function and the keystream: no AEAD, no Poly1305, no nonce management —
// the pool's counter and nonce are its own business, and a cipher API here
// would be a promise nobody asked for. RFC 8439 §2.3.2 carries the block
// test vector and §2.4.2 the keystream one; tools/test_random_host.c
// checks both.
//
// NO KERNEL HEADERS here or in the .c (host-compiled).

#include <stdint.h>
#include <stddef.h>

#define CHACHA20_KEY_BYTES   32
#define CHACHA20_NONCE_BYTES 12
#define CHACHA20_BLOCK_BYTES 64

// One 64-byte block of keystream for (key, counter, nonce).
void chacha20_block(const uint8_t key[CHACHA20_KEY_BYTES], uint32_t counter,
                    const uint8_t nonce[CHACHA20_NONCE_BYTES],
                    uint8_t out[CHACHA20_BLOCK_BYTES]);

// `len` bytes of keystream starting at block `counter`. Writes whole
// blocks internally; the tail of the last block is discarded.
void chacha20_stream(const uint8_t key[CHACHA20_KEY_BYTES], uint32_t counter,
                     const uint8_t nonce[CHACHA20_NONCE_BYTES],
                     uint8_t* out, size_t len);

#endif
