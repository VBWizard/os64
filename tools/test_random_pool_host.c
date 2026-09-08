// test_random_pool_host.c — the pool's behaviour (kernel/src/random.c) on
// the host, with the hardware replaced by hooks this file controls. The
// generator (tools/test_random_host.py --pool) strips random.c's includes
// and pastes it below the stubs, so the pool's statics are in scope here:
// the tests read s_key directly, which is the only way to prove fast key
// erasure rather than merely observe different output.
//
// What is proven: seeding by RDSEED, by RDRAND when RDSEED is stuck, by
// the jitter loop when both are absent, and by interrupt timing when the
// jitter loop cannot; the jitter loop refusing a frozen clock AND a
// perfectly linear one; the variation check refusing a constant
// instruction; the retry budget counting; a draw folding the fast pools
// when nothing else has (the no-NIC boot); fast key erasure and the exact
// ChaCha20 construction; the reseed cadence by bytes and by ticks; a
// contribution changing the key.

// ── Hooks: what the "hardware" does in each test ────────────────────────
// HW_FLAKY answers one call in three, HW_SLOW one in eight — the P5's
// RDSEED, which needs the whole retry budget for a single word.
typedef enum { HW_ABSENT, HW_OK, HW_STUCK, HW_FLAKY, HW_SLOW, HW_DEAD } hw_mode_t;
static hw_mode_t g_rdseed_mode = HW_OK, g_rdrand_mode = HW_OK;
static uint64_t g_hw_next = 0x1234567890ABCDEFull;
static uint32_t g_flaky_calls;
static uint64_t g_cycles = 1000;
static uint64_t g_cycle_step = 7;      // 0 = a cycle counter that never moves
static uint64_t g_cycle_noise = 9;     // 0 = a clock with no jitter: a perfectly linear counter
static uint64_t g_noise_state = 0x243F6A8885A308D3ull;
static uint32_t g_core = 0;

static bool hw_answer(hw_mode_t mode, uint64_t* out)
{
	switch (mode)
	{
		case HW_ABSENT:
		case HW_DEAD:  return false;
		case HW_STUCK: *out = 0xFFFFFFFFFFFFFFFFull; return true;
		case HW_FLAKY: if (++g_flaky_calls % 3 != 0) return false; /* fall through */
		case HW_SLOW:  if (mode == HW_SLOW && ++g_flaky_calls % 8 != 0) return false; /* fall through */
		case HW_OK:    g_hw_next = g_hw_next * 6364136223846793005ull + 1442695040888963407ull;
		               *out = g_hw_next; return true;
	}
	return false;
}
static bool hw_rdseed(uint64_t* out) { return hw_answer(g_rdseed_mode, out); }
static bool hw_rdrand(uint64_t* out) { return hw_answer(g_rdrand_mode, out); }
static void hw_probe_cpuid(bool* rdrand, bool* rdseed)
{
	*rdrand = g_rdrand_mode != HW_ABSENT;
	*rdseed = g_rdseed_mode != HW_ABSENT;
}
static uint64_t hw_cycles(void)
{
	// A model of a real clock: a fixed rate plus noise from the harness's
	// own generator. The jitter loop times a FIXED walk, so the noise is
	// the only thing its stuck test can pass on — and with the noise at
	// zero the counter is a straight line, which the test must refuse.
	if (g_cycle_step)
	{
		g_noise_state = g_noise_state * 6364136223846793005ull + 1442695040888963407ull;
		g_cycles += g_cycle_step + (g_cycle_noise ? (g_noise_state >> 33) % g_cycle_noise : 0);
	}
	return g_cycles;
}
static uint32_t hw_core_index(void) { return g_core; }

#define RANDOM_HOST_HOOKS 1
