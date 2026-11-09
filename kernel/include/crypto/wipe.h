#ifndef CRYPTO_WIPE_H
#define CRYPTO_WIPE_H

// wipe.h — zero a buffer in a way the optimizer may not remove.
//
// A plain "for (...) buf[i] = 0" on a local that is never read again is a
// dead store, and -O2 deletes dead stores; that is the compiler doing its
// job, and it is exactly wrong for a scratch buffer that held a pool key.
// The volatile stores below are observable by definition, so they stay,
// and the empty asm with a memory clobber keeps the compiler from
// reasoning about the buffer's contents past this point (the same shape
// as glibc's explicit_bzero and the kernel's memzero_explicit).
//
// NO KERNEL HEADERS: the crypto primitives and the pool are compiled by
// the host harness as well (blake2s.h says why).

#include <stdint.h>
#include <stddef.h>

static inline void crypto_wipe(void* p, size_t n)
{
	volatile uint8_t* v = (volatile uint8_t*)p;
	for (size_t i = 0; i < n; i++)
		v[i] = 0;
	__asm__ volatile("" : : "r"(p) : "memory");
}

#endif
