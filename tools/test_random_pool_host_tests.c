// The tests, pasted AFTER random.c by the generator so the pool's statics
// are visible. See test_random_pool_host.c for the hooks and the list.

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(hw_mode_t rdseed, hw_mode_t rdrand, uint64_t cycle_step)
{
	memset(&kRandomStats, 0, sizeof(kRandomStats));
	memset(s_key, 0, sizeof(s_key));
	memset(s_fast, 0, sizeof(s_fast));
	s_generation = s_served_since_reseed = s_reseeded_at_tick = s_timing_seen = 0;
	s_lock = 0;
	g_rdseed_mode = rdseed; g_rdrand_mode = rdrand;
	g_cycle_step = cycle_step; g_cycle_noise = 9; g_flaky_calls = 0;
	kTicksSinceStart = 100;
}

static void test_seeding(void)
{
	reset(HW_OK, HW_OK, 7); random_init();
	CHECK(kRandomStats.seeded && strcmp(kRandomStats.seeded_by, "rdseed") == 0, "healthy rdseed seeds: %s", kRandomStats.seeded_by);
	CHECK(kRandomStats.rdseed_trusted && kRandomStats.rdrand_trusted, "both trusted");
	CHECK(kRandomStats.hw_words == 4 && kRandomStats.hw_retries == 0, "four words, no retries (%lu, %lu)", kRandomStats.hw_words, kRandomStats.hw_retries);

	reset(HW_STUCK, HW_OK, 7); random_init();
	CHECK(!kRandomStats.rdseed_trusted, "a constant rdseed is not trusted");
	CHECK(strcmp(kRandomStats.seeded_by, "rdrand") == 0, "rdrand seeds behind a stuck rdseed: %s", kRandomStats.seeded_by);

	reset(HW_FLAKY, HW_ABSENT, 7); random_init();
	CHECK(strcmp(kRandomStats.seeded_by, "rdseed") == 0 && kRandomStats.hw_words == 4, "a flaky rdseed seeds within the retry budget");
	CHECK(kRandomStats.hw_retries >= 8 && kRandomStats.hw_exhausted == 0, "retries counted (%lu), none exhausted", kRandomStats.hw_retries);

	reset(HW_DEAD, HW_DEAD, 7); random_init();
	CHECK(!kRandomStats.rdseed_trusted && !kRandomStats.rdrand_trusted, "instructions that never answer are not trusted");
	CHECK(strcmp(kRandomStats.seeded_by, "jitter") == 0, "the jitter loop seeds when nothing else can: %s", kRandomStats.seeded_by);
	CHECK(kRandomStats.jitter_samples >= RANDOM_JITTER_SAMPLES, "jitter samples %lu", kRandomStats.jitter_samples);

	// A clock that advances in perfectly even steps carries no information;
	// the stuck test's second difference is zero on every round, so the
	// loop keeps nothing, whatever the rate.
	reset(HW_DEAD, HW_DEAD, 7); g_cycle_noise = 0; random_init();
	CHECK(!kRandomStats.seeded && kRandomStats.jitter_samples == 0, "a linear cycle counter cannot seed (kept %lu)", kRandomStats.jitter_samples);

	reset(HW_ABSENT, HW_ABSENT, 0); random_init();
	CHECK(!kRandomStats.seeded && !kRandomStats.cpuid_rdseed, "a frozen cycle counter and no instruction: unseeded");
	// Interrupt timing finishes the job: events arrive, knet folds them.
	g_cycle_step = 3;
	// A fold takes a core's pool only once it holds a fold's worth, so the
	// threshold is crossed at the first fold AFTER the 1024th event lands,
	// never on a stray call in between.
	for (int i = 0; i < RANDOM_TIMING_SEED_EVENTS; i++)
	{
		random_add_timing(i % 2 ? RANDOM_SOURCE_TICK : RANDOM_SOURCE_NIC);
		if (i % 50 == 0) random_fold_fast_pools();
	}
	random_fold_fast_pools();
	CHECK(!kRandomStats.seeded, "a fold's worth short of the threshold is not seeded");
	for (int i = 0; i < RANDOM_FOLD_EVERY; i++) random_add_timing(RANDOM_SOURCE_TICK);
	random_fold_fast_pools();
	CHECK(kRandomStats.seeded && strcmp(kRandomStats.seeded_by, "timing") == 0, "timing seeds after the threshold: %s", kRandomStats.seeded_by);
	CHECK(kRandomStats.timing_events[RANDOM_SOURCE_TICK] + kRandomStats.timing_events[RANDOM_SOURCE_NIC] == RANDOM_TIMING_SEED_EVENTS + RANDOM_FOLD_EVERY, "events counted by source");
	CHECK(kRandomStats.folds > 0, "folds happened");

	// The no-NIC boot: the tick lands events, nothing ever calls
	// random_fold_fast_pools (there is no knet), and the first draw folds
	// them itself. random_seeded() alone never does — it is a question.
	reset(HW_ABSENT, HW_ABSENT, 0); random_init();
	g_cycle_step = 3;
	for (int i = 0; i < RANDOM_TIMING_SEED_EVENTS + RANDOM_FOLD_EVERY; i++) random_add_timing(RANDOM_SOURCE_TICK);
	CHECK(!random_seeded() && kRandomStats.folds == 0, "events alone fold nothing");
	uint8_t one;
	random_bytes(&one, 1);
	CHECK(kRandomStats.seeded && strcmp(kRandomStats.seeded_by, "timing") == 0 && kRandomStats.folds == 1, "a draw before seeding folds the fast pools: %s, folds %lu", kRandomStats.seeded_by, kRandomStats.folds);
	random_bytes(&one, 1);
	CHECK(kRandomStats.folds == 1, "a draw after seeding does not");
}

static void test_erasure_and_construction(void)
{
	reset(HW_OK, HW_OK, 7); random_init();
	uint8_t key_before[32]; memcpy(key_before, s_key, 32);
	uint64_t gen = s_generation;
	uint8_t out[100];
	random_bytes(out, sizeof(out));
	CHECK(memcmp(key_before, s_key, 32) != 0, "the key changed after a draw (fast key erasure)");
	// The construction, exactly: stream(key_before, nonce=gen) = new key || out.
	uint8_t nonce[12] = {0};
	for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)(gen >> (8 * i));
	uint8_t expect[32 + 100];
	chacha20_stream(key_before, 0, nonce, expect, sizeof(expect));
	CHECK(memcmp(expect, s_key, 32) == 0, "the next key is the stream's first 32 bytes");
	CHECK(memcmp(expect + 32, out, 100) == 0, "the output is the stream past them");
	// And the old key cannot regenerate the NEXT draw.
	uint8_t out2[100];
	random_bytes(out2, sizeof(out2));
	CHECK(memcmp(out, out2, 100) != 0, "consecutive draws differ");
	// A contribution changes the key.
	uint8_t k[32]; memcpy(k, s_key, 32);
	random_mix("hello", 5);
	CHECK(memcmp(k, s_key, 32) != 0 && kRandomStats.mixes == 1, "a mix changes the key");
	// Large draws span chunks and stay exact in length.
	static uint8_t big[5000];
	random_bytes(big, sizeof(big));
	CHECK(kRandomStats.bytes_served == 200 + 5000, "bytes served %lu", kRandomStats.bytes_served);
}

static void test_reseed_cadence(void)
{
	reset(HW_OK, HW_OK, 7); random_init();
	CHECK(kRandomStats.reseeds == 0, "no reseed at init");
	static uint8_t buf[4096];
	for (int i = 0; i < 257; i++) random_bytes(buf, sizeof(buf));   // past 1 MB
	CHECK(kRandomStats.reseeds == 1, "reseed after a megabyte (%lu)", kRandomStats.reseeds);
	uint64_t hw = kRandomStats.hw_words;
	kTicksSinceStart += RANDOM_RESEED_TICKS;
	random_bytes(buf, 1);
	CHECK(kRandomStats.reseeds == 2 && kRandomStats.hw_words == hw + 4, "reseed after the interval, from hardware");
	random_bytes(buf, 1);
	CHECK(kRandomStats.reseeds == 2, "and not again until the next");
}

int main(void)
{
	test_seeding();
	test_erasure_and_construction();
	test_reseed_cadence();
	if (failures) { printf("random pool host: %d FAILURES\n", failures); return 1; }
	printf("random pool host: seeding (rdseed/rdrand/jitter/timing), erasure + construction, reseed cadence PASS\n");
	return 0;
}
