// test_cpu_roster_host.c — drive kernel/src/cpu_roster.c on the host.
//
// The roster is the one place an APIC ID becomes a CPU number, so every
// property the kernel leans on is checked here against machines built by
// hand: CPU 0 is the BSP, the rest ascend by APIC ID, the inverse table
// round-trips, a cap keeps the BSP, and IDs the xAPIC cannot address — or
// that appear twice — never get a number. Then random lists check the same
// properties on inputs nobody thought of.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_roster.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAX_CPUS 64

typedef struct
{
	uint32_t            slot[MAX_CPUS];
	uint8_t             inverse[CPU_ROSTER_APIC_IDS];
	cpu_roster_report_t report;
	bool                ok;
} roster_t;

static roster_t build(const uint32_t *ids, uint32_t n, uint32_t bsp, uint32_t cap, uint32_t max)
{
	roster_t r;
	memset(&r, 0xA5, sizeof(r));   // nothing may be read that was not written
	r.ok = cpu_roster_build(ids, n, bsp, cap, max, r.slot, r.inverse, &r.report);
	return r;
}

static uint32_t apic_of(const roster_t *r, const uint32_t *ids, uint32_t cpu)
{
	return ids[r->slot[cpu]];
}

// The properties every successful roster must have, whatever it was fed.
static void check_invariants(const roster_t *r, const uint32_t *ids, uint32_t n, uint32_t bsp,
                             uint32_t cap, uint32_t max, const char *what)
{
	if (!r->ok)
		return;
	const cpu_roster_report_t *rep = &r->report;
	CHECK(rep->count >= 1 && rep->count <= max, "%s: count %u outside 1..%u", what, rep->count, max);
	if (cap)
		CHECK(rep->count <= cap, "%s: count %u above cap %u", what, rep->count, cap);
	CHECK(apic_of(r, ids, 0) == bsp, "%s: CPU 0 is APIC %u, BSP is %u", what, apic_of(r, ids, 0), bsp);
	for (uint32_t c = 0; c < rep->count; c++)
	{
		CHECK(r->slot[c] < n, "%s: CPU %u slot %u past the list", what, c, r->slot[c]);
		uint32_t id = apic_of(r, ids, c);
		CHECK(id <= CPU_ROSTER_APIC_MAX, "%s: CPU %u has unaddressable APIC %u", what, c, id);
		CHECK(r->inverse[id] == c, "%s: round trip CPU %u -> APIC %u -> %u", what, c, id, r->inverse[id]);
		if (c >= 2)
			CHECK(apic_of(r, ids, c - 1) < id, "%s: CPU %u (APIC %u) not above CPU %u (APIC %u)",
			      what, c, id, c - 1, apic_of(r, ids, c - 1));
	}
	// Every inverse entry names a numbered CPU or none at all.
	uint32_t named = 0;
	for (uint32_t id = 0; id < CPU_ROSTER_APIC_IDS; id++)
	{
		if (r->inverse[id] == CPU_ROSTER_NO_CPU)
			continue;
		named++;
		CHECK(r->inverse[id] < rep->count && apic_of(r, ids, r->inverse[id]) == id,
		      "%s: inverse[%u] = %u is not that CPU", what, id, r->inverse[id]);
	}
	CHECK(named == rep->count, "%s: inverse names %u CPUs, roster has %u", what, named, rep->count);
	CHECK(rep->count + rep->parked + rep->too_wide + rep->duplicates == n,
	      "%s: %u numbered + %u parked + %u too wide + %u duplicates != %u listed",
	      what, rep->count, rep->parked, rep->too_wide, rep->duplicates, n);
	CHECK(rep->listed == n, "%s: listed %u, given %u", what, rep->listed, n);
	bool dense = bsp == 0;
	for (uint32_t c = 0; c < rep->count; c++)
		if (apic_of(r, ids, c) != c)
			dense = false;
	CHECK(rep->dense == dense, "%s: dense says %d, the roster says %d", what, rep->dense, dense);
}

// ── Machines ────────────────────────────────────────────────────────────────

static void test_dense(void)
{
	// QEMU's -smp 8 and the P5: dense IDs, BSP 0, in order. Nothing changes:
	// CPU n is APIC n.
	uint32_t ids[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	roster_t r = build(ids, 8, 0, 0, MAX_CPUS);
	CHECK(r.ok, "dense: refused");
	check_invariants(&r, ids, 8, 0, 0, MAX_CPUS, "dense");
	CHECK(r.report.count == 8 && r.report.dense, "dense: count %u dense %d", r.report.count, r.report.dense);
	for (uint32_t c = 0; c < 8; c++)
		CHECK(apic_of(&r, ids, c) == c, "dense: CPU %u is APIC %u", c, apic_of(&r, ids, c));
	CHECK(!r.report.bsp_not_lowest && r.report.parked == 0, "dense: report");
}

// The 3900X as its firmware lists it: every core's first thread, then every
// sibling (MADT order), APIC IDs die<<3 | core<<1 | thread with three cores
// per four-slot CCX.
static uint32_t zen2_3900x(uint32_t *ids)
{
	uint32_t n = 0;
	for (uint32_t thread = 0; thread < 2; thread++)
		for (uint32_t die = 0; die < 4; die++)
			for (uint32_t core = 0; core < 3; core++)
				ids[n++] = die << 3 | core << 1 | thread;
	return n;
}

static void test_3900x(void)
{
	uint32_t ids[32];
	uint32_t n = zen2_3900x(ids);
	roster_t r = build(ids, n, 0, 0, MAX_CPUS);
	CHECK(r.ok, "3900X: refused");
	check_invariants(&r, ids, n, 0, 0, MAX_CPUS, "3900X");
	CHECK(r.report.count == 24 && !r.report.dense, "3900X: count %u dense %d", r.report.count, r.report.dense);
	CHECK(r.report.apic_lowest == 0 && r.report.apic_highest == 29, "3900X: range %u-%u",
	      r.report.apic_lowest, r.report.apic_highest);
	// Siblings adjacent: CPUs 2k and 2k+1 are threads 0 and 1 of one core.
	for (uint32_t c = 0; c < 24; c += 2)
		CHECK((apic_of(&r, ids, c) | 1) == apic_of(&r, ids, c + 1) && !(apic_of(&r, ids, c) & 1),
		      "3900X: CPUs %u/%u are APIC %u/%u, not siblings", c, c + 1,
		      apic_of(&r, ids, c), apic_of(&r, ids, c + 1));
	// The two numbers the MAXCORES=4 death confused: APIC 4 is CPU 4, APIC 8 is CPU 6.
	CHECK(r.inverse[4] == 4 && r.inverse[8] == 6 && r.inverse[29] == 23, "3900X: inverse 4/8/29 = %u/%u/%u",
	      r.inverse[4], r.inverse[8], r.inverse[29]);
	CHECK(r.inverse[6] == CPU_ROSTER_NO_CPU && r.inverse[30] == CPU_ROSTER_NO_CPU, "3900X: holes are not CPUs");

	// MAXCPUS=4: the first four of the ORDER, which are one CCX's first two cores.
	roster_t c4 = build(ids, n, 0, 4, MAX_CPUS);
	check_invariants(&c4, ids, n, 0, 4, MAX_CPUS, "3900X cap 4");
	CHECK(c4.report.count == 4 && c4.report.parked == 20 && !c4.report.parked_by_max,
	      "3900X cap 4: count %u parked %u by_max %d", c4.report.count, c4.report.parked, c4.report.parked_by_max);
	for (uint32_t c = 0; c < 4; c++)
		CHECK(apic_of(&c4, ids, c) == c, "3900X cap 4: CPU %u is APIC %u", c, apic_of(&c4, ids, c));
	// The capped-off IDs must not be numbered: the wake loop and every lookup
	// would otherwise reach a CPU that is still parked in the bootloader.
	CHECK(c4.inverse[4] == CPU_ROSTER_NO_CPU, "3900X cap 4: APIC 4 numbered %u", c4.inverse[4]);

	// nosmp is a cap of one.
	roster_t c1 = build(ids, n, 0, 1, MAX_CPUS);
	check_invariants(&c1, ids, n, 0, 1, MAX_CPUS, "3900X nosmp");
	CHECK(c1.report.count == 1 && c1.report.dense, "3900X nosmp: count %u dense %d", c1.report.count, c1.report.dense);
}

static void test_bsp_not_lowest(void)
{
	// A firmware whose BSP is APIC 5 and lists it first.
	uint32_t ids[] = { 5, 0, 1, 2, 3, 4, 6, 7 };
	roster_t r = build(ids, 8, 5, 0, MAX_CPUS);
	CHECK(r.ok, "bsp 5: refused");
	check_invariants(&r, ids, 8, 5, 0, MAX_CPUS, "bsp 5");
	CHECK(r.report.bsp_not_lowest && !r.report.dense, "bsp 5: not_lowest %d dense %d",
	      r.report.bsp_not_lowest, r.report.dense);
	CHECK(apic_of(&r, ids, 1) == 0 && apic_of(&r, ids, 5) == 4 && apic_of(&r, ids, 6) == 6,
	      "bsp 5: order %u %u %u", apic_of(&r, ids, 1), apic_of(&r, ids, 5), apic_of(&r, ids, 6));

	// ...and one that lists the BSP somewhere other than first. A cap of one
	// must still be the BSP — position 0 of the list is NOT CPU 0.
	uint32_t late[] = { 2, 3, 0, 1 };
	roster_t c1 = build(late, 4, 0, 1, MAX_CPUS);
	check_invariants(&c1, late, 4, 0, 1, MAX_CPUS, "bsp listed third, nosmp");
	CHECK(c1.ok && c1.report.count == 1 && c1.slot[0] == 2, "bsp listed third: CPU 0 slot %u", c1.slot[0]);
}

static void test_refusals(void)
{
	// IDs the xAPIC cannot address, and a repeated one.
	uint32_t ids[] = { 0, 1, 255, 300, 1, 2 };
	roster_t r = build(ids, 6, 0, 0, MAX_CPUS);
	CHECK(r.ok, "refusals: refused");
	check_invariants(&r, ids, 6, 0, 0, MAX_CPUS, "refusals");
	CHECK(r.report.count == 3 && r.report.too_wide == 2 && r.report.duplicates == 1,
	      "refusals: count %u too_wide %u dup %u", r.report.count, r.report.too_wide, r.report.duplicates);
	CHECK(r.slot[1] == 1, "refusals: a duplicate keeps its FIRST entry (slot %u)", r.slot[1]);

	// The BSP missing, or unaddressable, is not a machine: refuse, write nothing.
	uint32_t nobsp[] = { 1, 2, 3 };
	roster_t m = build(nobsp, 3, 0, 0, MAX_CPUS);
	CHECK(!m.ok && m.inverse[1] == 0xA5, "no bsp: ok %d, inverse touched %u", m.ok, m.inverse[1]);
	uint32_t wide[] = { 255, 0 };
	CHECK(!build(wide, 2, 255, 0, MAX_CPUS).ok, "bsp 255 accepted");
	CHECK(!build(nobsp, 0, 0, 0, MAX_CPUS).ok, "empty list accepted");

	// A table size that would let the sentinel be a CPU number.
	uint32_t one[] = { 0 };
	CHECK(!build(one, 1, 0, 0, CPU_ROSTER_NO_CPU).ok, "max_cpus == sentinel accepted");
	CHECK(!build(one, 1, 0, 0, 0).ok, "max_cpus 0 accepted");
	CHECK(build(one, 1, 0, 0, CPU_ROSTER_NO_CPU - 1).ok, "max_cpus just below the sentinel refused");
}

static void test_more_than_max(void)
{
	// A Threadripper's worth and then some: 100 CPUs, tables of 64.
	uint32_t ids[100];
	for (uint32_t i = 0; i < 100; i++)
		ids[i] = 99 - i;          // listed backwards; BSP 0 is last
	roster_t r = build(ids, 100, 0, 0, MAX_CPUS);
	check_invariants(&r, ids, 100, 0, 0, MAX_CPUS, "100 cpus");
	CHECK(r.ok && r.report.count == 64 && r.report.parked == 36 && r.report.parked_by_max,
	      "100 cpus: count %u parked %u by_max %d", r.report.count, r.report.parked, r.report.parked_by_max);
	CHECK(r.report.dense && apic_of(&r, ids, 63) == 63, "100 cpus: CPU 63 is APIC %u", apic_of(&r, ids, 63));

	// A cap above the table is the table's limit, and says so.
	roster_t big = build(ids, 100, 0, 80, MAX_CPUS);
	CHECK(big.report.count == 64 && big.report.parked_by_max, "cap 80: count %u by_max %d",
	      big.report.count, big.report.parked_by_max);
	// A cap below it is the cap's.
	roster_t small = build(ids, 100, 0, 10, MAX_CPUS);
	CHECK(small.report.count == 10 && !small.report.parked_by_max, "cap 10: count %u by_max %d",
	      small.report.count, small.report.parked_by_max);
}

// ── Random machines ─────────────────────────────────────────────────────────

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)rng_state;
}

static void test_random(uint32_t rounds)
{
	for (uint32_t round = 0; round < rounds; round++)
	{
		uint32_t n = 1 + rng() % 300;
		uint32_t ids[300];
		for (uint32_t i = 0; i < n; i++)
			ids[i] = (rng() % 8 == 0) ? rng() % 1024 : rng() % 256;
		uint32_t bsp = ids[rng() % n];
		uint32_t cap = (rng() % 3 == 0) ? rng() % 80 : 0;
		uint32_t max = 1 + rng() % (CPU_ROSTER_NO_CPU - 1);
		if (max > MAX_CPUS)
			max = MAX_CPUS;
		roster_t r = build(ids, n, bsp, cap, max);
		CHECK(r.ok == (bsp <= CPU_ROSTER_APIC_MAX), "random %u: ok %d for bsp %u", round, r.ok, bsp);
		check_invariants(&r, ids, n, bsp, cap, max, "random");
		if (failures > 20)
			return;
	}
}

int main(int argc, char **argv)
{
	uint32_t rounds = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 20000;
	test_dense();
	test_3900x();
	test_bsp_not_lowest();
	test_refusals();
	test_more_than_max();
	test_random(rounds);
	if (failures)
	{
		printf("cpu roster: %d FAILED\n", failures);
		return 1;
	}
	printf("cpu roster: all checks passed (%u random machines)\n", rounds);
	return 0;
}
