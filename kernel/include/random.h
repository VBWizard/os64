#ifndef RANDOM_H
#define RANDOM_H

// random.h — the entropy pool: the kernel's one source of numbers nobody
// can predict. RANDOM.md is the design record; random.c the pool.
//
// The shape, in one paragraph: a 256-bit key, BLAKE2s to fold entropy in,
// ChaCha20 to generate with FAST KEY ERASURE (each run of the stream keeps
// its first 32 bytes as the next key, so a captured key cannot reproduce
// what was already served). Seeded at boot from RDSEED (RDRAND behind it,
// a jitter loop behind both), stirred for the rest of the session by the
// arrival times of interrupts, reseeded every few minutes or megabyte.
//
// Customers: /dev/random (devfs.c) for programs — BearSSL's seed is the one
// that demanded it — and, in the kernel, the TCP initial sequence number,
// the ephemeral port draw and DHCP's transaction id.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Where a timing sample came from, for the eyes and for the mix.
typedef enum
{
	RANDOM_SOURCE_TICK = 0,   // the timer interrupt, once per tick on the BSP
	RANDOM_SOURCE_NIC,        // a network card's interrupt top half
	RANDOM_SOURCE_DRAIN,      // knet's drain loop, once per wake that moved frames
	RANDOM_SOURCE_COUNT
} random_source_t;

// Boot: probe the instructions, run the variation check, seed. Runs in
// kernel_init before anything dials. Prints one line saying how the pool
// was seeded, or that it was not.
void random_init(void);

// The kernel's verb. Never blocks and never fails: a caller before the pool
// is seeded gets what the pool has, which still beats the tick counter.
// Callers that must not proceed unseeded ask random_seeded() first
// (/dev/random's reader is the one that waits).
void random_bytes(void* buf, size_t n);
uint32_t random_u32(void);
uint64_t random_u64(void);

// Has a hardware source contributed 256 bits, or the jitter loop or the
// timing source crossed its threshold? /sys/random says which.
bool random_seeded(void);

// The interrupt-side verb: a few instructions, no lock, safe from any
// context. Reads the cycle counter and folds it into THIS core's fast
// pool; the pool takes the fast pools in under its lock from thread
// context (random_fold_fast_pools, called by knet and by every reseed).
void random_add_timing(random_source_t source);
void random_fold_fast_pools(void);

// A program's contribution through /dev/random: folded in, credited with
// nothing — the kernel cannot know what the bytes are worth.
void random_mix(const void* buf, size_t n);

// The eyes, for /sys/random. Counters are best-effort (the timing ones are
// bumped from interrupt context without a lock).
typedef struct
{
	bool     cpuid_rdrand, cpuid_rdseed;      // advertised
	bool     rdrand_trusted, rdseed_trusted;  // and passed the variation check
	bool     seeded;
	char     seeded_by[24];                   // "rdseed", "rdrand", "jitter", "timing", "-"
	uint64_t hw_words;                        // 64-bit words a hardware instruction gave
	uint64_t hw_retries;                      // attempts that returned CF=0
	uint64_t hw_exhausted;                    // words given up on after the retry budget
	uint64_t jitter_samples;                  // boot jitter loop: distinct deltas kept
	uint64_t timing_events[RANDOM_SOURCE_COUNT];
	uint64_t folds;                           // fast pools folded into the key
	uint64_t reseeds;
	uint64_t bytes_served;
	uint64_t mixes;                           // /dev/random writes
	uint64_t reads_refused;                   // /dev/random reads that arrived before seeding
} random_stats_t;
extern random_stats_t kRandomStats;

#endif
