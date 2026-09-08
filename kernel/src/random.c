// random.c — the entropy pool. random.h is the contract, RANDOM.md the
// design record and its reasons. This file is compiled by the host too
// (tools/test_random_host.py strips the includes and supplies stubs), so
// everything hardware-shaped is behind the few functions marked HOOK.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <cpuid.h>

#include "random.h"
#include "crypto/blake2s.h"
#include "crypto/chacha20.h"
#include "crypto/wipe.h"
#include "spinlock.h"
#include "smp_core.h"
#include "kernel.h"
#include "driver/system/x86_64.h"
#include "serial_logging.h"
#include "CONFIG.h"

// ── Policy ──────────────────────────────────────────────────────────────

#define RANDOM_KEY_BYTES        32
#define RANDOM_HW_RETRIES       10      // Intel's DRNG guide: ten tries for RDRAND; the P5's RDSEED took eight
#define RANDOM_VARIATION_DRAWS  8       // Linux's boot check: eight equal draws means the instruction lies
#define RANDOM_RESEED_TICKS     (300 * TICKS_PER_SECOND)
#define RANDOM_RESEED_BYTES     (1u << 20)
#define RANDOM_FOLD_EVERY       64      // fast-pool events between folds, per core
#define RANDOM_TIMING_SEED_EVENTS 1024  // timing events that count as seeded when nothing else did
#define RANDOM_JITTER_SAMPLES   256     // samples past the stuck test the boot jitter loop wants
#define RANDOM_JITTER_ROUNDS    (1u << 16)  // and the most walks it may spend collecting them
#define RANDOM_JITTER_STEPS     256     // the one walk every round times
#define RANDOM_FAST_POOLS       256     // indexed by APIC id, one cache line each

// ── State ───────────────────────────────────────────────────────────────

random_stats_t kRandomStats;

static spinlock_t s_lock;
static uint8_t    s_key[RANDOM_KEY_BYTES];
static uint64_t   s_generation;            // generate calls; the ChaCha nonce, never repeated per key
static uint64_t   s_served_since_reseed;
static uint64_t   s_reseeded_at_tick;
static uint64_t   s_timing_seen;           // events folded so far (seeding threshold)
static bool       s_timing_seeded_announced;

// One per core, written from interrupt context with no lock. Padded to a
// cache line so two cores' pools never share one. `count` is the samples
// that passed the stuck test (random_add_timing) since the last fold —
// the number the seeding threshold counts; the three `last_*` fields are
// the test's history.
typedef struct
{
	uint64_t acc;
	uint64_t last_t, last_delta, last_delta2;
	uint32_t count;
	uint8_t  history;                 // samples seen, saturating at 3: the test needs that much past
	uint8_t  pad[64 - 4 * sizeof(uint64_t) - sizeof(uint32_t) - 1];
} __attribute__((aligned(64))) fast_pool_t;
_Static_assert(sizeof(fast_pool_t) == 64, "one fast pool is one cache line");
static fast_pool_t s_fast[RANDOM_FAST_POOLS];

// ── HOOKs: the hardware, replaceable by the host harness ────────────────

#ifndef RANDOM_HOST_HOOKS
static bool hw_rdseed(uint64_t* out)
{
	uint64_t v;
	uint8_t ok;
	__asm__ volatile("rdseed %0\n\tsetc %1" : "=r"(v), "=qm"(ok) : : "cc");
	*out = v;
	return ok != 0;
}

static bool hw_rdrand(uint64_t* out)
{
	uint64_t v;
	uint8_t ok;
	__asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(v), "=qm"(ok) : : "cc");
	*out = v;
	return ok != 0;
}

static void hw_probe_cpuid(bool* rdrand, bool* rdseed)
{
	unsigned a = 0, b = 0, c = 0, d = 0;
	*rdrand = __get_cpuid(1, &a, &b, &c, &d) && (c & bit_RDRND);
	a = b = c = d = 0;
	*rdseed = __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & bit_RDSEED);
}

static uint64_t hw_cycles(void) { return rdtsc(); }

// This core's interrupt flag, off and back: the producer's guard (see
// random_add_timing). Not a lock — nothing another core can wait on.
static uint64_t hw_irq_save(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq\n\tpop %0" : "=r"(flags) :: "memory");
	__asm__ volatile("cli" ::: "memory");
	return flags;
}
static void hw_irq_restore(uint64_t flags)
{
	if (flags & 0x200)
		__asm__ volatile("sti" ::: "memory");
}

static uint32_t hw_core_index(void)
{
	core_local_storage_t* cls = try_get_core_local_storage();
	return cls ? ((uint32_t)cls->apic_id & (RANDOM_FAST_POOLS - 1)) : 0u;
}
#endif

// ── The key ─────────────────────────────────────────────────────────────
// Every change to the key goes through one door: new key = BLAKE2s(old key
// || what arrived). Caller holds s_lock.

static void key_fold(const void* material, size_t len)
{
	blake2s_state_t h;
	blake2s_init(&h, RANDOM_KEY_BYTES, NULL, 0);
	blake2s_update(&h, s_key, RANDOM_KEY_BYTES);
	blake2s_update(&h, material, len);
	blake2s_final(&h, s_key);
}

// Draw `words` 64-bit values from an instruction, retrying each up to the
// budget; returns how many it got. Counts every retry and every give-up.
static size_t hw_draw(bool (*instr)(uint64_t*), uint64_t* out, size_t words)
{
	size_t got = 0;
	for (size_t i = 0; i < words; i++)
	{
		bool ok = false;
		for (int tries = 0; tries < RANDOM_HW_RETRIES && !ok; tries++)
		{
			ok = instr(&out[got]);
			if (!ok)
				kRandomStats.hw_retries++;
		}
		if (ok)
			got++;
		else
			kRandomStats.hw_exhausted++;
	}
	kRandomStats.hw_words += got;
	return got;
}

// The variation check: eight draws that all agree mean the instruction is
// answering with a constant (the Zen 2 post-resume bug returned all ones),
// and a constant is worse than nothing because it LOOKS like a source.
// Each draw gets the retry budget, because "not ready" (CF=0) is an
// answer RDSEED is allowed to give and the P5's gives it seven times in a
// row; a source that is merely slow must not be branded a liar. The
// retries are counted; the words are not, since this is a check and not
// a seed.
static bool hw_varies(bool (*instr)(uint64_t*))
{
	uint64_t first = 0, v = 0;
	bool have_first = false;
	for (int i = 0; i < RANDOM_VARIATION_DRAWS; i++)
	{
		bool ok = false;
		for (int tries = 0; tries < RANDOM_HW_RETRIES && !ok; tries++)
		{
			ok = instr(&v);
			if (!ok)
				kRandomStats.hw_retries++;
		}
		if (!ok)
			continue;
		if (!have_first) { first = v; have_first = true; }
		else if (v != first)
			return true;
	}
	return false;
}

static void set_seeded(const char* by)
{
	kRandomStats.seeded = true;
	int i = 0;
	for (; by[i] && i < 23; i++) kRandomStats.seeded_by[i] = by[i];
	kRandomStats.seeded_by[i] = '\0';
}

// Fold every core's fast pool into the key. Caller holds s_lock, which
// the producers never take, so THE SNAPSHOT IS ORDERED, NOT LOCKED: take
// `count` first with an ACQUIRE exchange, then read `acc`. Every sample
// the exchange counted was published with a release store AFTER its
// timestamp went into `acc`, so the `acc` read that follows holds all of
// them — a count is never credited for a timestamp the key has not seen.
// A sample landing after the exchange is counted at the next fold; its
// timestamp may already be in this read, and `acc` is cumulative, so it
// is folded again then, which is harmless.
static void fold_fast_pools_locked(void)
{
	uint64_t material[RANDOM_FAST_POOLS + 1];
	size_t n = 0;
	for (uint32_t i = 0; i < RANDOM_FAST_POOLS; i++)
	{
		uint32_t count = __atomic_exchange_n(&s_fast[i].count, 0, __ATOMIC_ACQUIRE);
		if (count == 0)
			continue;
		material[n++] = __atomic_load_n(&s_fast[i].acc, __ATOMIC_ACQUIRE);
		s_timing_seen += count;
	}
	if (n == 0)
		return;
	material[n++] = hw_cycles();
	key_fold(material, n * sizeof(uint64_t));
	crypto_wipe(material, sizeof(material));
	kRandomStats.folds++;
	if (!kRandomStats.seeded && s_timing_seen >= RANDOM_TIMING_SEED_EVENTS)
		set_seeded("timing");
}

// Reseed: fresh hardware output if the instruction is trusted, the fast
// pools, and the cycle counter, all through the one door.
static void reseed_locked(void)
{
	uint64_t hw[4];
	size_t got = 0;
	if (kRandomStats.rdseed_trusted)
		got = hw_draw(hw_rdseed, hw, 4);
	if (got < 4 && kRandomStats.rdrand_trusted)
		got += hw_draw(hw_rdrand, hw + got, 4 - got);
	if (got)
		key_fold(hw, got * sizeof(uint64_t));
	crypto_wipe(hw, sizeof(hw));
	fold_fast_pools_locked();
	uint64_t now = hw_cycles();
	key_fold(&now, sizeof(now));
	s_served_since_reseed = 0;
	s_reseeded_at_tick = kTicksSinceStart;
	kRandomStats.reseeds++;
}

// ── Seeding at boot ─────────────────────────────────────────────────────

// The fallback when no instruction is trusted: time THE SAME memory walk,
// round after round, with the cycle counter. The walk is fixed — one
// length, one start, one stride — so the work timed never varies and
// whatever the deltas do is the machine's doing: cache and bus noise on
// bare metal without RDSEED (nothing this house owns), the host's
// scheduling on a hypervisor. A sample is kept only when it passes the
// STUCK TEST from Müller's Jitter RNG (the kernel crypto API's
// jitterentropy): the delta, the delta of deltas and the delta of THOSE
// must all be non-zero. That rejects a constant delta and one drifting at
// a constant rate — a cache warming up, a counter advancing in fixed
// steps — which are the only shapes a clock with no noise in it can
// produce. It is a health test, not a measurement: it cannot tell noise
// from a clock that lies in a pattern, and nothing can. Bounded by
// ROUNDS, not ticks: it runs under the pool's lock with interrupts off,
// so the tick cannot be its clock. If the budget runs out first the pool
// stays unseeded and the timing source finishes the job as interrupts
// arrive.
static bool jitter_seed_locked(void)
{
	static volatile uint8_t arena[4096];
	uint64_t last_delta = 0, last_delta2 = 0;
	uint64_t samples[32];
	size_t n = 0, kept = 0;
	for (uint32_t round = 0; round < RANDOM_JITTER_ROUNDS && kept < RANDOM_JITTER_SAMPLES; round++)
	{
		uint64_t before = hw_cycles();
		uint32_t at = 0;
		for (uint32_t i = 0; i < RANDOM_JITTER_STEPS; i++)
		{
			arena[at] = (uint8_t)(arena[at] + 1);
			at = (at * 1103515245u + 12345u) & 4095;
		}
		uint64_t delta = hw_cycles() - before;
		uint64_t delta2 = delta - last_delta;
		uint64_t delta3 = delta2 - last_delta2;
		// The first two rounds have no history to test against.
		bool stuck = round < 2 || delta == 0 || delta2 == 0 || delta3 == 0;
		last_delta = delta;
		last_delta2 = delta2;
		if (stuck)
			continue;
		samples[n++] = delta;
		kept++;
		if (n == 32)
		{
			key_fold(samples, sizeof(samples));
			n = 0;
		}
	}
	if (n)
		key_fold(samples, n * sizeof(uint64_t));
	crypto_wipe(samples, sizeof(samples));
	crypto_wipe(&last_delta, sizeof(last_delta));      // the test's history is two samples too
	crypto_wipe(&last_delta2, sizeof(last_delta2));
	kRandomStats.jitter_samples += kept;
	return kept >= RANDOM_JITTER_SAMPLES;
}

void random_init(void)
{
	uint64_t f = spinlock_acquire_irqsave(&s_lock);
	hw_probe_cpuid(&kRandomStats.cpuid_rdrand, &kRandomStats.cpuid_rdseed);
	kRandomStats.rdseed_trusted = kRandomStats.cpuid_rdseed && hw_varies(hw_rdseed);
	kRandomStats.rdrand_trusted = kRandomStats.cpuid_rdrand && hw_varies(hw_rdrand);
	kRandomStats.seeded_by[0] = '-';

	uint64_t hw[4];
	size_t got = 0;
	if (kRandomStats.rdseed_trusted)
		got = hw_draw(hw_rdseed, hw, 4);
	if (got == 4)
		set_seeded("rdseed");
	else if (kRandomStats.rdrand_trusted)
	{
		got += hw_draw(hw_rdrand, hw + got, 4 - got);
		if (got == 4)
			set_seeded("rdrand");
	}
	if (got)
		key_fold(hw, got * sizeof(uint64_t));
	crypto_wipe(hw, sizeof(hw));
	if (!kRandomStats.seeded && jitter_seed_locked())
		set_seeded("jitter");
	uint64_t now = hw_cycles();
	key_fold(&now, sizeof(now));
	s_reseeded_at_tick = kTicksSinceStart;
	spinlock_release_irqrestore(&s_lock, f);

	printf("random: rdrand %s, rdseed %s — pool %s%s\n",
	       !kRandomStats.cpuid_rdrand ? "absent" : kRandomStats.rdrand_trusted ? "ok" : "FAILED",
	       !kRandomStats.cpuid_rdseed ? "absent" : kRandomStats.rdseed_trusted ? "ok" : "FAILED",
	       kRandomStats.seeded ? "seeded by " : "UNSEEDED — waiting on interrupt timing",
	       kRandomStats.seeded ? kRandomStats.seeded_by : "");
	printd(DEBUG_BOOT, "random: rdrand %s, rdseed %s — pool %s%s (hw words %lu, retries %lu, jitter samples %lu)\n",
	       !kRandomStats.cpuid_rdrand ? "absent" : kRandomStats.rdrand_trusted ? "ok" : "FAILED",
	       !kRandomStats.cpuid_rdseed ? "absent" : kRandomStats.rdseed_trusted ? "ok" : "FAILED",
	       kRandomStats.seeded ? "seeded by " : "UNSEEDED",
	       kRandomStats.seeded ? kRandomStats.seeded_by : "",
	       kRandomStats.hw_words, kRandomStats.hw_retries, kRandomStats.jitter_samples);
}

// ── Generating ──────────────────────────────────────────────────────────

bool random_seeded(void) { return kRandomStats.seeded; }

// A draw is served in chunks of at most RANDOM_CHUNK_BYTES, and THE LOCK
// IS TAKEN PER CHUNK, for the OTHER cores: it is an irqsave lock (the ISN
// is drawn under TCP's), and a core waiting on it spins with its own
// interrupts off, so a long draw must not hold it for its whole length.
// It does NOT bound THIS core's interrupts-off time on the syscall path —
// call_in_kernel_context keeps IF clear across the whole file operation,
// whatever this loop releases — which is why /dev/random serves at most a
// page per read (devfs.c) and why kernel callers draw a few bytes. Each
// chunk is its own generation with its own nonce, so two callers
// interleaving at chunk boundaries take distinct stream. The reseed
// cadence is checked at every chunk for the same reason: a request that
// starts under the byte threshold must not carry itself past it on one
// key.
#define RANDOM_CHUNK_BYTES 1024

void random_bytes(void* buf, size_t n)
{
	uint8_t* out = buf;
	while (n > 0)
	{
		uint64_t f = spinlock_acquire_irqsave(&s_lock);
		if (s_served_since_reseed >= RANDOM_RESEED_BYTES ||
		    kTicksSinceStart - s_reseeded_at_tick >= RANDOM_RESEED_TICKS)
			reseed_locked();
		// Unseeded, take whatever the interrupts have left in the fast
		// pools before answering: on a machine with no NIC there is no
		// knet to fold them, and the tick alone crosses the threshold in
		// seconds if something folds it. Seeded, only the cadence above
		// folds here.
		else if (!kRandomStats.seeded)
			fold_fast_pools_locked();
		if (!s_timing_seeded_announced && kRandomStats.seeded && kRandomStats.seeded_by[0] == 't')
		{
			s_timing_seeded_announced = true;
			printd(DEBUG_BOOT, "random: pool seeded by interrupt timing after %lu samples past the stuck test\n", s_timing_seen);
		}
		// FAST KEY ERASURE: run the stream from the current key; the first
		// 32 bytes of it become the next key and are never handed out, the
		// rest is the answer. A key that leaks after this call cannot
		// reproduce it. One chunk per nonce; the nonce is the generation
		// count and the key changes every chunk, so no (key, nonce) repeats.
		uint8_t block[RANDOM_KEY_BYTES + RANDOM_CHUNK_BYTES];
		size_t take = n < RANDOM_CHUNK_BYTES ? n : RANDOM_CHUNK_BYTES;
		uint8_t nonce[CHACHA20_NONCE_BYTES] = {0};
		for (int i = 0; i < 8; i++)
			nonce[i] = (uint8_t)(s_generation >> (8 * i));
		s_generation++;
		chacha20_stream(s_key, 0, nonce, block, RANDOM_KEY_BYTES + take);
		for (int i = 0; i < RANDOM_KEY_BYTES; i++)
			s_key[i] = block[i];
		for (size_t i = 0; i < take; i++)
			out[i] = block[RANDOM_KEY_BYTES + i];
		crypto_wipe(block, RANDOM_KEY_BYTES + take);
		s_served_since_reseed += take;
		kRandomStats.bytes_served += take;
		spinlock_release_irqrestore(&s_lock, f);
		out += take;
		n -= take;
	}
}

uint32_t random_u32(void) { uint32_t v; random_bytes(&v, sizeof(v)); return v; }
uint64_t random_u64(void) { uint64_t v; random_bytes(&v, sizeof(v)); return v; }

// ── Stirring ────────────────────────────────────────────────────────────

// THE PRODUCER'S GUARD. A core's pool has one writer at a time only if
// this core's interrupts are off while it writes: the tick and the NIC
// ISRs are producers, and so is knet's drain from THREAD context on the
// same core — an ISR landing between its read of `acc` and its write
// would have its sample overwritten and its count lost. The flag is
// saved and restored (free where it was already off, which is every ISR),
// and it is taken BEFORE the core index is read, so a thread cannot
// migrate between choosing a pool and writing it. `count` is published
// with a RELEASE atomic add after `acc` is written: the release is what
// lets the fold on another core take a consistent snapshot without a
// lock (fold_fast_pools_locked), and the atomic is what keeps that fold's
// exchange-to-zero from landing between this core's read and its write —
// a plain increment would resurrect a count the fold had already credited.
void random_add_timing(random_source_t source)
{
	uint64_t flags = hw_irq_save();
	fast_pool_t* p = &s_fast[hw_core_index()];
	uint64_t t = hw_cycles();
	// Rotate-and-xor: the low bits of the cycle count are the noise, the
	// rotate keeps successive samples from cancelling, the source tag keeps
	// two sources' identical timestamps distinct.
	p->acc = ((p->acc << 7) | (p->acc >> 57)) ^ t ^ ((uint64_t)source << 56);
	// The same stuck test the boot jitter loop applies (jitter_seed_locked
	// says why): an arrival whose delta, or delta of deltas, or delta of
	// THOSE is zero is what a noiseless clock produces, and it is mixed in
	// (harmless) but not COUNTED toward seeding. Per core, so no lock.
	uint64_t delta = t - p->last_t, delta2 = delta - p->last_delta, delta3 = delta2 - p->last_delta2;
	bool stuck = p->history < 3 || delta == 0 || delta2 == 0 || delta3 == 0;
	p->last_t = t;
	p->last_delta = delta;
	p->last_delta2 = delta2;
	if (p->history < 3)
		p->history++;
	if (stuck)
		__atomic_add_fetch(&kRandomStats.timing_rejected, 1, __ATOMIC_RELAXED);
	else
		__atomic_add_fetch(&p->count, 1, __ATOMIC_RELEASE);
	if (source < RANDOM_SOURCE_COUNT)
		__atomic_add_fetch(&kRandomStats.timing_events[source], 1, __ATOMIC_RELAXED);
	hw_irq_restore(flags);
}

void random_fold_fast_pools(void)
{
	// Only when some core has accumulated a fold's worth: knet calls this
	// on every wake and /dev/random on every read it would refuse, and
	// taking the lock for nothing is the cost to avoid.
	bool due = false;
	for (uint32_t i = 0; i < RANDOM_FAST_POOLS && !due; i++)
		due = s_fast[i].count >= RANDOM_FOLD_EVERY;
	if (!due)
		return;
	uint64_t f = spinlock_acquire_irqsave(&s_lock);
	fold_fast_pools_locked();
	spinlock_release_irqrestore(&s_lock, f);
}

void random_mix(const void* buf, size_t n)
{
	uint64_t f = spinlock_acquire_irqsave(&s_lock);
	key_fold(buf, n);
	kRandomStats.mixes++;
	spinlock_release_irqrestore(&s_lock, f);
}
