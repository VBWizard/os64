// The frame table on the host, under ASan and UBSan (test_frames_host.sh).
//
// Two kinds of proof. Hand cases pin the behaviour a sentence can name:
// coalescing on both sides, exact-size recycling, every tripwire refusing
// and changing nothing, region and bootstrap shapes, the TLSF bound. Then a
// DIFFERENTIAL run drives the table and a deliberately dumb reference model
// (one owner byte per frame, a list of live runs) with the same random
// operations. The two may choose different addresses, so each allocation is
// judged against the model's ownership rather than compared; accounting,
// liveness, the largest free run and refusals ARE compared, exactly, and
// frames_audit checks the whole table after every operation.
//
// Usage: test_frames_host [seed]

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frames.h"

static unsigned long checks, failures;
static uint64_t seed;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL (seed %llu) %s:%d: ", (unsigned long long)seed, __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		if (failures > 20) exit(1); \
	} \
} while (0)

static uint64_t rng_state;
static uint64_t rnd(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 2685821657736338717ULL;
}
static uint64_t below(uint64_t n) { return n ? rnd() % n : 0; }

// ── A table with its own storage ────────────────────────────────────────

typedef struct {
	frames_t f;
	frame_t *table;
} rig_t;

static void rig_new(rig_t *r, uint64_t nframes)
{
	r->table = malloc(frames_table_bytes(nframes));
	if (!r->table) { perror("malloc"); exit(2); }
	CHECK(frames_init(&r->f, r->table, nframes) == FRAMES_OK, "init");
}
static void rig_free(rig_t *r) { free(r->table); r->table = NULL; }

static void audit(rig_t *r, const char *when)
{
	uint64_t where = 0;
	const char *why = frames_audit(&r->f, &where);
	CHECK(why == NULL, "audit after %s: %s at %llu", when, why ? why : "", (unsigned long long)where);
	if (why) exit(1);
}

// ── The reference model ─────────────────────────────────────────────────

enum { M_RESERVED = 0, M_FREE = 1, M_RUN = 2, M_LEDGER = 3 };

typedef struct { uint64_t first, count; int kind; } live_t;

typedef struct {
	uint64_t n;
	uint8_t *owner;
	uint32_t *pins;
	live_t *live;
	size_t nlive, cap;
	uint64_t usable, free_frames, run_frames, ledger_frames;
} model_t;

static void model_new(model_t *m, uint64_t n)
{
	memset(m, 0, sizeof(*m));
	m->n = n;
	m->owner = calloc(n, 1);
	m->pins = calloc(n, sizeof(uint32_t));
	m->cap = 1024;
	m->live = malloc(m->cap * sizeof(live_t));
	if (!m->owner || !m->pins || !m->live) { perror("calloc"); exit(2); }
}
static void model_free(model_t *m) { free(m->owner); free(m->pins); free(m->live); }

static void model_add_free(model_t *m, uint64_t first, uint64_t count)
{
	if (first == 0 && count) { first = 1; count--; }
	for (uint64_t i = 0; i < count; i++)
		m->owner[first + i] = M_FREE;
	m->usable += count;
	m->free_frames += count;
}

static void model_take(model_t *m, uint64_t first, uint64_t count, int kind)
{
	for (uint64_t i = 0; i < count; i++)
		m->owner[first + i] = (uint8_t)kind;
	if (m->nlive == m->cap) {
		m->cap *= 2;
		m->live = realloc(m->live, m->cap * sizeof(live_t));
		if (!m->live) { perror("realloc"); exit(2); }
	}
	m->live[m->nlive++] = (live_t){first, count, kind};
	m->free_frames -= count;
	if (kind == M_RUN) m->run_frames += count; else m->ledger_frames += count;
}

static void model_release(model_t *m, size_t idx)
{
	live_t l = m->live[idx];
	for (uint64_t i = 0; i < l.count; i++)
		m->owner[l.first + i] = M_FREE;
	m->live[idx] = m->live[--m->nlive];
	m->free_frames += l.count;
	if (l.kind == M_RUN) m->run_frames -= l.count; else m->ledger_frames -= l.count;
}

static uint64_t model_largest_free(const model_t *m)
{
	uint64_t best = 0, run = 0;
	for (uint64_t i = 0; i < m->n; i++) {
		run = m->owner[i] == M_FREE ? run + 1 : 0;
		if (run > best) best = run;
	}
	return best;
}

static uint64_t model_free_runs(const model_t *m)
{
	uint64_t runs = 0;
	for (uint64_t i = 0; i < m->n; i++)
		if (m->owner[i] == M_FREE && (i == 0 || m->owner[i - 1] != M_FREE))
			runs++;
	return runs;
}

static bool model_live(const model_t *m, uint64_t i)
{
	return m->owner[i] == M_RUN || (m->owner[i] == M_LEDGER && m->pins[i] > 0);
}

// Counters, free runs, largest free run, and liveness of every frame.
static void compare(rig_t *r, model_t *m, const char *when)
{
	CHECK(r->f.usable_frames == m->usable && r->f.free_frames == m->free_frames &&
	      r->f.run_frames == m->run_frames && r->f.ledger_frames == m->ledger_frames,
	      "%s: counters %llu/%llu/%llu/%llu vs model %llu/%llu/%llu/%llu", when,
	      (unsigned long long)r->f.usable_frames, (unsigned long long)r->f.free_frames,
	      (unsigned long long)r->f.run_frames, (unsigned long long)r->f.ledger_frames,
	      (unsigned long long)m->usable, (unsigned long long)m->free_frames,
	      (unsigned long long)m->run_frames, (unsigned long long)m->ledger_frames);
	CHECK(r->f.free_runs == model_free_runs(m), "%s: free runs %llu vs model %llu", when,
	      (unsigned long long)r->f.free_runs, (unsigned long long)model_free_runs(m));
	CHECK(frames_largest_free(&r->f) == model_largest_free(m), "%s: largest free %llu vs %llu",
	      when, (unsigned long long)frames_largest_free(&r->f),
	      (unsigned long long)model_largest_free(m));
	// Runs by length, counted from the model: its live runs, and its free
	// stretches (one free run each, since the table never leaves two free
	// runs side by side).
	uint64_t live_sized[FRAMES_SIZE_BUCKETS] = {0}, free_sized[FRAMES_SIZE_BUCKETS] = {0};
	for (size_t i = 0; i < m->nlive; i++)
		live_sized[frames_size_bucket(m->live[i].count)]++;
	for (uint64_t i = 0, run = 0; i <= m->n; i++) {
		if (i < m->n && m->owner[i] == M_FREE) { run++; continue; }
		if (run) free_sized[frames_size_bucket(run)]++;
		run = 0;
	}
	for (unsigned b = 0; b < FRAMES_SIZE_BUCKETS; b++)
		CHECK(r->f.live_runs_sized[b] == live_sized[b] && r->f.free_runs_sized[b] == free_sized[b],
		      "%s: runs of length >= %llu: live %llu/%llu free %llu/%llu", when,
		      (unsigned long long)frames_bucket_low(b),
		      (unsigned long long)r->f.live_runs_sized[b], (unsigned long long)live_sized[b],
		      (unsigned long long)r->f.free_runs_sized[b], (unsigned long long)free_sized[b]);

	// The runs the iterator reports tile the described memory exactly as the
	// model's owners do, kind by kind.
	uint64_t cursor = 0, first, count, by_kind[4] = {0, 0, 0, 0}, last_end = 0;
	frames_kind_t kind;
	while (frames_next_run(&r->f, &cursor, &first, &count, &kind)) {
		if (first < last_end || (int)kind == M_RESERVED || first + count > m->n) {
			CHECK(false, "%s: run iterator out of order at %llu", when, (unsigned long long)first);
			break;
		}
		for (uint64_t k = 0; k < count; k++)
			if (m->owner[first + k] != (int)kind) {
				CHECK(false, "%s: run [%llu,+%llu) of kind %d covers frame owned %d", when,
				      (unsigned long long)first, (unsigned long long)count, (int)kind,
				      m->owner[first + k]);
				break;
			}
		by_kind[kind] += count;
		last_end = first + count;
	}
	CHECK(by_kind[M_FREE] == m->free_frames && by_kind[M_RUN] == m->run_frames &&
	      by_kind[M_LEDGER] == m->ledger_frames, "%s: the runs iterated do not add up", when);
	for (uint64_t i = 0; i < m->n; i++) {
		if (frames_live(&r->f, i) != model_live(m, i)) {
			CHECK(false, "%s: liveness of frame %llu", when, (unsigned long long)i);
			break;
		}
		if ((int)frames_kind(&r->f, i) != m->owner[i]) {
			CHECK(false, "%s: kind of frame %llu is %d, model %d", when,
			      (unsigned long long)i, (int)frames_kind(&r->f, i), m->owner[i]);
			break;
		}
	}
	checks++;
}

// An allocation the table answered, judged against the model: inside usable
// memory, every frame free a moment ago. A refusal is judged too: the model
// must have no free stretch that long.
static bool judge_alloc(rig_t *r, model_t *m, uint64_t got, uint64_t count, int kind)
{
	if (got == FRAMES_NONE) {
		CHECK(model_largest_free(m) < count, "refused %llu frames with a free run of %llu",
		      (unsigned long long)count, (unsigned long long)model_largest_free(m));
		return false;
	}
	CHECK(got != 0 && got + count <= m->n, "allocation [%llu,+%llu) out of bounds",
	      (unsigned long long)got, (unsigned long long)count);
	for (uint64_t i = 0; i < count; i++)
		if (m->owner[got + i] != M_FREE) {
			CHECK(false, "allocation [%llu,+%llu) overlaps frame %llu owned %d",
			      (unsigned long long)got, (unsigned long long)count,
			      (unsigned long long)(got + i), m->owner[got + i]);
			return false;
		}
	model_take(m, got, count, kind);
	(void)r;
	return true;
}

// ── Hand cases ──────────────────────────────────────────────────────────

static void case_basics(void)
{
	rig_t r;
	rig_new(&r, 1000);
	CHECK(frames_add_region(&r.f, 100, 50) == FRAMES_OK, "region");
	audit(&r, "region");
	CHECK(r.f.free_runs == 1 && frames_largest_free(&r.f) == 50, "one run of 50");

	uint64_t a = frames_alloc(&r.f, 1, FRAMES_RUN);
	uint64_t b = frames_alloc(&r.f, 3, FRAMES_RUN);
	uint64_t c = frames_alloc(&r.f, 1, FRAMES_RUN);
	CHECK(a == 100 && b == 101 && c == 104, "carved from the front, in order: %llu %llu %llu",
	      (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
	audit(&r, "three allocations");
	CHECK(frames_live(&r.f, 102) && frames_live(&r.f, 103) && !frames_live(&r.f, 105),
	      "interior frames of a run are live, the free rest is not");

	// Exact-size recycling: freed between two live neighbours, the run of 3
	// is on the 3-list, and the next request for 3 gets exactly it back.
	CHECK(frames_free(&r.f, b, NULL) == FRAMES_OK, "free b");
	audit(&r, "free b");
	CHECK(frames_alloc(&r.f, 3, FRAMES_RUN) == b, "a repeating size gets its own hole back");

	// Coalescing both ways: free a, then c, then b sits between free runs.
	CHECK(frames_free(&r.f, a, NULL) == FRAMES_OK, "free a");
	CHECK(frames_free(&r.f, c, NULL) == FRAMES_OK, "free c");
	audit(&r, "free a, c");
	uint64_t n = 0;
	CHECK(frames_free(&r.f, b, &n) == FRAMES_OK && n == 3, "free b again, length reported");
	audit(&r, "free b between free runs");
	CHECK(r.f.free_runs == 1 && frames_largest_free(&r.f) == 50 && r.f.free_frames == 50,
	      "everything merged back into one run of 50");
	rig_free(&r);
}

static void case_tripwires(void)
{
	rig_t r;
	rig_new(&r, 200);
	frames_add_region(&r.f, 10, 100);
	uint64_t a = frames_alloc(&r.f, 5, FRAMES_RUN);
	uint64_t l = frames_alloc(&r.f, 4, FRAMES_LEDGER);
	frame_t before[200];
	memcpy(before, r.table, sizeof(before));
	frames_t fbefore = r.f;
#define UNCHANGED(what) CHECK(memcmp(before, r.table, sizeof(before)) == 0 && \
	memcmp(&fbefore, &r.f, sizeof(fbefore)) == 0, "a refused %s changed something", what)

	frames_kind_t k = FRAMES_RESERVED;
	uint64_t n = 0;
	CHECK(frames_check_start(&r.f, a, &k, &n) == FRAMES_OK && k == FRAMES_RUN && n == 5,
	      "check_start names a run's kind and length");
	CHECK(frames_check_start(&r.f, l, &k, &n) == FRAMES_OK && k == FRAMES_LEDGER && n == 4,
	      "and a ledger run's");
	CHECK(frames_check_start(&r.f, a + 1, NULL, NULL) == FRAMES_NOT_A_START &&
	      frames_check_start(&r.f, 50, NULL, NULL) == FRAMES_NOT_ALLOCATED,
	      "check_start refuses what free refuses");
	UNCHANGED("check_start");
	CHECK(frames_free(&r.f, a + 2, NULL) == FRAMES_NOT_A_START, "interior free refused");
	UNCHANGED("interior free");
	CHECK(frames_free(&r.f, a + 4, NULL) == FRAMES_NOT_A_START, "tail free refused");
	UNCHANGED("tail free");
	CHECK(frames_run_start(&r.f, a + 4) == a, "the run an interior free landed in can be named");
	CHECK(frames_free(&r.f, 50, NULL) == FRAMES_NOT_ALLOCATED, "free of a free frame refused");
	UNCHANGED("free of a free frame");
	CHECK(frames_free(&r.f, 5, NULL) == FRAMES_NOT_ALLOCATED, "free of reserved refused");
	UNCHANGED("free of reserved");
	CHECK(frames_free(&r.f, 0, NULL) == FRAMES_NOT_ALLOCATED, "free of frame 0 refused");
	CHECK(frames_free(&r.f, 500, NULL) == FRAMES_OUT_OF_RANGE, "free past the table refused");
	UNCHANGED("out of range free");

	// Pins: only on ledger frames, never below zero, all or nothing.
	CHECK(frames_ledger_pin(&r.f, a, 1, 1) == FRAMES_NOT_LEDGER, "pin on a RUN frame refused");
	CHECK(frames_ledger_pin(&r.f, l, 1, -1) == FRAMES_PIN_RANGE, "unpin below zero refused");
	CHECK(frames_ledger_pin(&r.f, l + 2, 4, 1) == FRAMES_NOT_LEDGER,
	      "a pin range running off the ledger run is refused");
	UNCHANGED("refused pins");
	CHECK(!frames_live(&r.f, l + 1), "an unpinned ledger frame is not live");
	CHECK(frames_ledger_pin(&r.f, l + 1, 2, 1) == FRAMES_OK, "pin two ledger frames");
	CHECK(frames_live(&r.f, l + 1) && frames_live(&r.f, l + 2) && !frames_live(&r.f, l + 3),
	      "pinned ledger frames are live, the rest are not");
	audit(&r, "pins");
	memcpy(before, r.table, sizeof(before));
	fbefore = r.f;
	CHECK(frames_free(&r.f, l, NULL) == FRAMES_LEDGER_PINNED, "a pinned ledger run cannot go back");
	UNCHANGED("pinned ledger free");
	CHECK(frames_ledger_pin(&r.f, l + 1, 2, -1) == FRAMES_OK, "unpin");
	CHECK(frames_free(&r.f, l, NULL) == FRAMES_OK, "an unpinned ledger run goes back");
	CHECK(frames_free(&r.f, l, NULL) == FRAMES_NOT_ALLOCATED, "a double free is refused");
	audit(&r, "ledger round trip");

	CHECK(frames_alloc(&r.f, 0, FRAMES_RUN) == FRAMES_NONE, "zero frames refused");
	CHECK(frames_alloc(&r.f, 1, FRAMES_FREE) == FRAMES_NONE, "allocating as FREE refused");
	CHECK(frames_alloc(&r.f, 1000, FRAMES_RUN) == FRAMES_NONE, "more than there is refused");
	audit(&r, "refused allocations");
	rig_free(&r);
}

static void case_regions(void)
{
	rig_t r;
	rig_new(&r, 4096);
	// Frame 0 is clipped, adjacent regions join, holes stay holes.
	CHECK(frames_add_region(&r.f, 0, 100) == FRAMES_OK, "region at 0");
	CHECK(r.f.table[0] == 0 && r.f.usable_frames == 99, "frame 0 stays reserved");
	CHECK(frames_add_region(&r.f, 100, 50) == FRAMES_OK, "adjacent region");
	audit(&r, "adjacent regions");
	CHECK(r.f.free_runs == 1 && frames_largest_free(&r.f) == 149, "adjacent regions are one run");
	CHECK(frames_add_region(&r.f, 200, 10) == FRAMES_OK, "region after a hole");
	CHECK(r.f.free_runs == 2, "a hole keeps runs apart");
	CHECK(frames_add_region(&r.f, 140, 20) == FRAMES_NOT_RESERVED, "overlapping region refused");
	CHECK(frames_add_region(&r.f, 4090, 10) == FRAMES_OUT_OF_RANGE, "region off the table refused");
	CHECK(frames_add_region(&r.f, 4086, 10) == FRAMES_OK, "region at the very end");
	audit(&r, "regions");

	// Bootstrap: the table's own frames sit in the middle of a region,
	// described as allocated, with the rest of the region around them.
	CHECK(frames_add_allocated(&r.f, 1000, 8, FRAMES_RUN) == FRAMES_OK, "bootstrap run");
	CHECK(frames_add_region(&r.f, 900, 100) == FRAMES_OK && frames_add_region(&r.f, 1008, 92) == FRAMES_OK,
	      "the region either side");
	audit(&r, "bootstrap");
	CHECK(frames_live(&r.f, 1003) && r.f.run_frames == 8, "the table's frames are live");
	CHECK(frames_add_allocated(&r.f, 0, 1, FRAMES_RUN) == FRAMES_BAD_ARGUMENT, "frame 0 never allocated");
	CHECK(frames_add_allocated(&r.f, 1000, 1, FRAMES_RUN) == FRAMES_NOT_RESERVED,
	      "described memory cannot be described again");
	rig_free(&r);
}

// Many separated 70-frame holes and one big run: a request for 66 or 100
// is served by bit scans from the big run, walking no list. Only when the
// big run is gone does the request walk its own sub-bin, once, on the way to
// an answer.
static void case_tlsf_bound(void)
{
	rig_t r;
	rig_new(&r, 100000);
	frames_add_region(&r.f, 1, 99999);
	uint64_t holes[200];
	for (int i = 0; i < 200; i++) {
		holes[i] = frames_alloc(&r.f, 70, FRAMES_RUN);
		frames_alloc(&r.f, 1, FRAMES_RUN);            // a separator that stays
	}
	for (int i = 0; i < 200; i++)
		frames_free(&r.f, holes[i], NULL);
	audit(&r, "holes");
	uint64_t walks = r.f.fallback_walks;
	uint64_t big = frames_alloc(&r.f, 100, FRAMES_RUN);
	CHECK(big != FRAMES_NONE && r.f.fallback_walks == walks, "100 frames with no walk");
	CHECK(frames_alloc(&r.f, 66, FRAMES_RUN) != FRAMES_NONE && r.f.fallback_walks == walks,
	      "66 frames with no walk: the round-up finds the big run");
	// Use up the big run, leaving only the 70-frame holes (one now taken).
	uint64_t rest = frames_largest_free(&r.f);
	CHECK(rest > 70, "the big run remains");
	// Asking for exactly the longest run rounds past every list, so that
	// request is the one that walks its own sub-bin.
	frames_alloc(&r.f, rest, FRAMES_RUN);
	audit(&r, "big run used up");
	walks = r.f.fallback_walks;
	uint64_t examined = r.f.fallback_examined;
	uint64_t got = frames_alloc(&r.f, 66, FRAMES_RUN);
	CHECK(got != FRAMES_NONE && r.f.fallback_walks == walks + 1 &&
	      r.f.fallback_examined == examined + 1,
	      "with only 70-frame holes left, 66 walks its own sub-bin once and takes the first");
	CHECK(frames_alloc(&r.f, 100, FRAMES_RUN) == FRAMES_NONE, "100 is refused: nothing that long");
	audit(&r, "tlsf");
	rig_free(&r);
}

// The P5 shape: 150,000 live single frames (Yonder's 66,000 and more),
// freed in random order, the table back to one run.
static void case_p5_shape(void)
{
	const uint64_t n = 200000;
	rig_t r;
	rig_new(&r, n);
	frames_add_region(&r.f, 1, n - 1);
	uint64_t *got = malloc(150000 * sizeof(uint64_t));
	for (int i = 0; i < 150000; i++) {
		got[i] = frames_alloc(&r.f, 1, FRAMES_RUN);
		if (got[i] == FRAMES_NONE) { CHECK(false, "ran out at %d", i); break; }
	}
	audit(&r, "150,000 live single frames");
	CHECK(r.f.run_frames == 150000, "150,000 frames live");
	for (int i = 149999; i > 0; i--) {
		int j = (int)below((uint64_t)i + 1);
		uint64_t t = got[i]; got[i] = got[j]; got[j] = t;
	}
	for (int i = 0; i < 150000; i++)
		CHECK(frames_free(&r.f, got[i], NULL) == FRAMES_OK || (i = 150000, false), "free %d", i);
	audit(&r, "all freed");
	CHECK(r.f.free_runs == 1 && r.f.free_frames == n - 1, "back to one run of everything");
	free(got);
	rig_free(&r);
}

// Churn of repeating sizes cannot bloat the lists: free runs are always
// separated by live runs, so there are never more free runs than live runs
// plus regions. (The August disease was free holes outnumbering everything.)
static void case_churn_bound(void)
{
	const uint64_t n = 50000;
	rig_t r;
	rig_new(&r, n);
	frames_add_region(&r.f, 1, n - 1);
	static const uint64_t sizes[] = {1, 3, 7, 20, 1, 1, 2};
	uint64_t live[2000];
	size_t nlive = 0;
	for (int op = 0; op < 400000; op++) {
		if (nlive < 2000 && (nlive == 0 || below(2))) {
			uint64_t g = frames_alloc(&r.f, sizes[below(7)], FRAMES_RUN);
			if (g != FRAMES_NONE) live[nlive++] = g;
		} else {
			size_t i = below(nlive);
			frames_free(&r.f, live[i], NULL);
			live[i] = live[--nlive];
		}
		if (r.f.free_runs > nlive + 1) {
			CHECK(false, "free runs %llu with %zu live", (unsigned long long)r.f.free_runs, nlive);
			break;
		}
	}
	checks++;
	audit(&r, "churn");
	rig_free(&r);
}

// ── The differential run ────────────────────────────────────────────────

static uint64_t pick_count(void)
{
	uint64_t roll = below(100);
	if (roll < 50) return 1;
	if (roll < 75) return 1 + below(64);
	if (roll < 93) return 65 + below(500);
	return 1 + below(5000);
}

static void differential(uint64_t n, int nregions, long ops, long audit_every)
{
	rig_t r;
	model_t m;
	rig_new(&r, n);
	model_new(&m, n);

	// Random regions with holes between them, one possibly at frame 0.
	uint64_t at = below(3) == 0 ? 0 : 1 + below(50);
	for (int i = 0; i < nregions && at < n; i++) {
		uint64_t len = 1 + below(n / (uint64_t)nregions);
		if (at + len > n) len = n - at;
		CHECK(frames_add_region(&r.f, at, len) == FRAMES_OK, "region %d", i);
		model_add_free(&m, at, len);
		at += len + below(40);
	}
	audit(&r, "regions");
	compare(&r, &m, "regions");

	for (long op = 0; op < ops; op++) {
		uint64_t roll = below(100);
		if (roll < 45 || m.nlive == 0) {
			int kind = below(10) == 0 ? M_LEDGER : M_RUN;
			uint64_t count = pick_count();
			uint64_t got = frames_alloc(&r.f, count, (frames_kind_t)kind);
			judge_alloc(&r, &m, got, count, kind);
		} else if (roll < 85) {
			size_t i = below(m.nlive);
			live_t l = m.live[i];
			if (l.kind == M_LEDGER) {
				// The ledger unpins everything before handing a run back.
				for (uint64_t k = 0; k < l.count; k++)
					while (m.pins[l.first + k] > 0) {
						CHECK(frames_ledger_pin(&r.f, l.first + k, 1, -1) == FRAMES_OK, "unpin");
						m.pins[l.first + k]--;
					}
			}
			uint64_t freed = 0;
			CHECK(frames_free(&r.f, l.first, &freed) == FRAMES_OK && freed == l.count,
			      "free [%llu,+%llu)", (unsigned long long)l.first, (unsigned long long)l.count);
			model_release(&m, i);
		} else if (roll < 93) {
			// A ledger carve or free: pin or unpin a stretch of a ledger run.
			size_t i = below(m.nlive);
			live_t l = m.live[i];
			uint64_t from = l.first + below(l.count), len = 1 + below(l.first + l.count - from);
			int delta = below(2) ? 1 : -1;
			bool ok = l.kind == M_LEDGER;
			for (uint64_t k = 0; ok && k < len; k++)
				if (delta < 0 && m.pins[from + k] == 0) ok = false;
			frames_status_t s = frames_ledger_pin(&r.f, from, len, delta);
			CHECK((s == FRAMES_OK) == ok, "pin [%llu,+%llu) %+d answered %d, model %d",
			      (unsigned long long)from, (unsigned long long)len, delta, (int)s, (int)ok);
			if (s == FRAMES_OK)
				for (uint64_t k = 0; k < len; k++)
					m.pins[from + k] = (uint32_t)((int)m.pins[from + k] + delta);
		} else {
			// A stray free somewhere random must be refused unless it names
			// the start of a live run, and refused frees change nothing.
			uint64_t frame = below(n + 10);
			bool starts = false;
			bool pinned = false;
			size_t idx = 0;
			for (size_t i = 0; i < m.nlive; i++)
				if (m.live[i].first == frame) { starts = true; idx = i; }
			if (starts && m.live[idx].kind == M_LEDGER)
				for (uint64_t k = 0; k < m.live[idx].count; k++)
					if (m.pins[frame + k]) pinned = true;
			frames_t fb = r.f;
			frames_status_t s = frames_free(&r.f, frame, NULL);
			if (starts && !pinned) {
				CHECK(s == FRAMES_OK, "a real free refused (%d)", (int)s);
				model_release(&m, idx);
			} else {
				CHECK(s != FRAMES_OK, "a stray free at %llu was accepted", (unsigned long long)frame);
				CHECK(memcmp(&fb, &r.f, sizeof(fb)) == 0, "a refused free changed the counters");
			}
		}
		if (audit_every && op % audit_every == 0) {
			audit(&r, "differential op");
			compare(&r, &m, "differential op");
		}
	}
	audit(&r, "differential end");
	compare(&r, &m, "differential end");

	// Drain: free everything, and the table is one run per run of regions.
	while (m.nlive) {
		live_t l = m.live[0];
		for (uint64_t k = 0; k < l.count; k++)
			while (m.pins[l.first + k] > 0) {
				frames_ledger_pin(&r.f, l.first + k, 1, -1);
				m.pins[l.first + k]--;
			}
		CHECK(frames_free(&r.f, l.first, NULL) == FRAMES_OK, "drain free");
		model_release(&m, 0);
	}
	audit(&r, "drained");
	compare(&r, &m, "drained");
	CHECK(r.f.free_frames == r.f.usable_frames, "everything free again");
	model_free(&m);
	rig_free(&r);
}

int main(int argc, char **argv)
{
	seed = argc > 1 ? strtoull(argv[1], NULL, 0) : 0x0F5A3E5ULL;
	rng_state = seed ? seed : 1;

	case_basics();
	case_tripwires();
	case_regions();
	case_tlsf_bound();
	case_p5_shape();
	case_churn_bound();
	// A failed hand case names the broken rule; a differential run over the
	// same broken table only cascades, and can hang (an unpin that never
	// reaches zero), turning a verdict into a timeout. Stop here instead.
	if (failures) {
		printf("frames host: %lu checks, %lu failed in the hand cases (seed %llu)\n", checks,
		       failures, (unsigned long long)seed);
		return 1;
	}
	// Small tables audited after every operation, then a larger one audited
	// periodically with far more operations. FRAMES_QUICK (the mutant
	// harness) keeps the every-operation audits and drops the long run.
	bool quick = getenv("FRAMES_QUICK") != NULL;
	differential(3000, 3, quick ? 20000 : 60000, 1);
	differential(20000, 6, quick ? 20000 : 60000, 1);
	if (!quick)
		differential(1 << 20, 12, 1500000, 50000);

	printf("frames host: %lu checks, %lu failed (seed %llu)\n", checks, failures,
	       (unsigned long long)seed);
	return failures ? 1 : 0;
}
