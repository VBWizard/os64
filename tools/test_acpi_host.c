// test_acpi_host.c — drive kernel/src/driver/system/acpi_tables.c on the host.
//
// THE PROPERTY UNDER TEST is the one the first 3900X boot broke: the parser
// reads a byte of firmware memory only after asking `map` for it. Here `map`
// answers every request with a fresh heap block of EXACTLY the bytes asked
// for, so a read one byte past them — the old walker's whole bug, reading
// tables through a window mapped for some other table — is an ASan report
// instead of a lucky hit. The fake "physical memory" is a set of regions
// placed far apart, the way that machine's firmware placed them.
//
// Then the decoders are checked against tables built here, and the walk is
// fed thousands of damaged machines to prove no input reaches past `map`.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acpi_tables.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ── Fake physical memory ────────────────────────────────────────────────────

#define MAX_REGIONS 96
#define MAX_MAPS    4096

typedef struct { uint64_t phys; uint8_t *bytes; uint32_t length; } region_t;

typedef struct
{
	region_t regions[MAX_REGIONS];
	int      region_count;
	void    *maps[MAX_MAPS];      // every block handed out, freed at reset
	int      map_count;
	int      refusals;
} machine_t;

static machine_t M;

static void machine_reset(void)
{
	for (int i = 0; i < M.region_count; i++)
		free(M.regions[i].bytes);
	for (int i = 0; i < M.map_count; i++)
		free(M.maps[i]);
	memset(&M, 0, sizeof(M));
}

static uint8_t *region(uint64_t phys, uint32_t length)
{
	if (M.region_count >= MAX_REGIONS)
	{
		printf("harness: too many regions\n");
		exit(2);
	}
	region_t *r = &M.regions[M.region_count++];
	r->phys = phys;
	r->length = length;
	r->bytes = calloc(1, length);
	return r->bytes;
}

// Exactly the requested bytes, in a block of exactly that size — or NULL when
// they are not all inside one region, which is what an unmappable or absent
// address looks like to the parser.
static const void *fake_map(void *ctx, uint64_t phys, uint32_t length)
{
	(void)ctx;
	for (int i = 0; i < M.region_count; i++)
	{
		region_t *r = &M.regions[i];
		if (phys >= r->phys && phys - r->phys <= r->length && length <= r->length - (phys - r->phys))
		{
			if (M.map_count >= MAX_MAPS)
			{
				printf("harness: too many maps\n");
				exit(2);
			}
			void *copy = malloc(length);
			memcpy(copy, r->bytes + (phys - r->phys), length);
			M.maps[M.map_count++] = copy;
			return copy;
		}
	}
	M.refusals++;
	return NULL;
}

// ── Building tables ─────────────────────────────────────────────────────────

static void fix_checksum(uint8_t *table, uint32_t length, uint32_t at)
{
	table[at] = 0;
	uint8_t sum = 0;
	for (uint32_t i = 0; i < length; i++)
		sum = (uint8_t)(sum + table[i]);
	table[at] = (uint8_t)(0 - sum);
}

static uint8_t *table(uint64_t phys, const char *sig, uint32_t length, uint8_t revision)
{
	uint8_t *t = region(phys, length);
	acpi_header_t *h = (acpi_header_t *)t;
	memcpy(h->signature, sig, 4);
	h->length = length;
	h->revision = revision;
	memcpy(h->oem_id, "OS64  ", 6);
	memcpy(h->oem_table_id, "TESTBED ", 8);
	return t;
}

static void seal(uint8_t *t)
{
	fix_checksum(t, ((acpi_header_t *)t)->length, 9);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void rsdp(uint64_t phys, uint8_t revision, uint32_t rsdt, uint64_t xsdt)
{
	uint8_t *r = region(phys, sizeof(acpi_rsdp_t));
	acpi_rsdp_t *p = (acpi_rsdp_t *)r;
	memcpy(p->signature, "RSD PTR ", 8);
	memcpy(p->oem_id, "OS64  ", 6);
	p->revision = revision;
	p->rsdt_address = rsdt;
	p->length = sizeof(acpi_rsdp_t);
	p->xsdt_address = xsdt;
	fix_checksum(r, ACPI_RSDP_V1_LENGTH, 8);
	fix_checksum(r, sizeof(acpi_rsdp_t), 32);
}

static void root(uint64_t phys, const char *sig, const uint64_t *entries, int count)
{
	uint32_t size = sig[0] == 'X' ? 8 : 4;
	uint8_t *t = table(phys, sig, (uint32_t)(sizeof(acpi_header_t) + size * count), 1);
	for (int i = 0; i < count; i++)
	{
		if (size == 8)
			put64(t + sizeof(acpi_header_t) + 8 * i, entries[i]);
		else
			put32(t + sizeof(acpi_header_t) + 4 * i, (uint32_t)entries[i]);
	}
	seal(t);
}

static uint8_t *fadt(uint64_t phys, uint32_t length, uint32_t dsdt, uint64_t x_dsdt)
{
	uint8_t *t = table(phys, "FACP", length, length >= 244 ? 6 : 1);
	acpi_fadt_t f;
	memset(&f, 0, sizeof(f));
	memcpy(&f, t, sizeof(acpi_header_t));
	f.dsdt = dsdt;
	f.x_dsdt = x_dsdt;
	f.pm_profile = 1;
	f.sci_interrupt = 9;
	f.smi_command = 0xB2;
	f.acpi_enable = 0xA0;
	f.pm1a_event_block = 0x400;
	f.pm1a_control_block = 0x404;
	f.pm_timer_block = 0x408;
	f.gpe0_block = 0x420;
	f.gpe0_length = 8;
	f.century = 0x32;
	f.boot_arch = ACPI_BOOT_8042 | ACPI_BOOT_LEGACY_DEVICES;
	f.flags = ACPI_FADT_WBINVD | ACPI_FADT_TMR_VAL_EXT | ACPI_FADT_RESET_REG_SUP;
	f.reset_register = (acpi_gas_t){ ACPI_SPACE_IO, 8, 0, 1, 0xCF9 };
	f.reset_value = 0x06;
	f.minor_version = 4;
	// The generic PM1a control block says something different from the
	// legacy port, so a test can tell which one won.
	f.x_pm1a_control_block = (acpi_gas_t){ ACPI_SPACE_IO, 16, 0, 2, 0x804 };
	memcpy(t, &f, length < sizeof(f) ? length : sizeof(f));
	seal(t);
	return t;
}

// A DSDT carrying `Name (\_S5, Package () { 5, 0 })` after some noise —
// including a decoy "_S5_" that is NOT a declaration.
static void dsdt(uint64_t phys, bool with_s5)
{
	static const uint8_t body[] = {
		0x10, 0x05, '_', 'S', '5', '_', 0x00,           // a decoy: not after a NameOp
		0x08, '\\', '_', 'S', '5', '_',                  // Name (\_S5,
		0x12, 0x06, 0x04,                                //   Package (4) {
		0x0A, 0x05, 0x00, 0x00, 0x00,                    //     5, 0, 0, 0 })
	};
	uint32_t length = (uint32_t)(sizeof(acpi_header_t) + sizeof(body));
	uint8_t *t = table(phys, "DSDT", length, 2);
	memcpy(t + sizeof(acpi_header_t), body, sizeof(body));
	if (!with_s5)
		t[sizeof(acpi_header_t) + 7] = 0x10;            // the NameOp becomes something else
	seal(t);
}

static void madt(uint64_t phys, bool ryzen_order)
{
	uint8_t e[256];
	uint32_t n = 0;
	for (uint8_t cpu = 0; cpu < 4; cpu++)
	{
		acpi_madt_lapic_t l = { ACPI_MADT_LAPIC, 8, cpu, cpu, cpu < 3 ? ACPI_MADT_CPU_ENABLED : ACPI_MADT_CPU_ONLINE_CAPABLE };
		memcpy(e + n, &l, sizeof(l)); n += sizeof(l);
	}
	// A Ryzen lists the processor's IOAPIC (GSI 24 up) before the chipset's.
	acpi_madt_ioapic_t gnb = { ACPI_MADT_IOAPIC, 12, 0x22, 0, 0xFEC01000, 24 };
	acpi_madt_ioapic_t fch = { ACPI_MADT_IOAPIC, 12, 0x21, 0, 0xFEC00000, 0 };
	memcpy(e + n, ryzen_order ? &gnb : &fch, 12); n += 12;
	memcpy(e + n, ryzen_order ? &fch : &gnb, 12); n += 12;
	acpi_madt_override_t pit = { ACPI_MADT_OVERRIDE, 10, 0, 0, 2, 0 };
	acpi_madt_override_t sci = { ACPI_MADT_OVERRIDE, 10, 0, 9, 9, 0x000F };
	memcpy(e + n, &pit, 10); n += 10;
	memcpy(e + n, &sci, 10); n += 10;
	acpi_madt_lapic_nmi_t nmi = { ACPI_MADT_LAPIC_NMI, 6, 0xFF, 0x0005, 1 };
	memcpy(e + n, &nmi, 6); n += 6;
	acpi_madt_x2apic_t x2 = { ACPI_MADT_X2APIC, 16, 0, 300, ACPI_MADT_CPU_ENABLED, 300 };
	memcpy(e + n, &x2, 16); n += 16;
	e[n++] = 0x7F; e[n++] = 4; e[n++] = 0; e[n++] = 0;   // a type nobody here knows

	uint8_t *t = table(phys, "APIC", (uint32_t)(sizeof(acpi_madt_t) + n), 5);
	acpi_madt_t *m = (acpi_madt_t *)t;
	m->lapic_address = 0xFEE00000;
	m->flags = 1;
	memcpy(t + sizeof(acpi_madt_t), e, n);
	seal(t);
}

static void mcfg(uint64_t phys)
{
	uint8_t *t = table(phys, "MCFG", ACPI_MCFG_ENTRIES_OFFSET + 2 * sizeof(acpi_mcfg_entry_t), 1);
	acpi_mcfg_entry_t a = { 0xE0000000, 0, 0, 255, 0 };
	acpi_mcfg_entry_t b = { 0xF0000000, 1, 0, 63, 0 };
	memcpy(t + ACPI_MCFG_ENTRIES_OFFSET, &a, 16);
	memcpy(t + ACPI_MCFG_ENTRIES_OFFSET + 16, &b, 16);
	seal(t);
}

static void hpet(uint64_t phys)
{
	uint8_t *t = table(phys, "HPET", sizeof(acpi_hpet_t), 1);
	acpi_hpet_t *h = (acpi_hpet_t *)t;
	h->block_id = (0x1022u << 16) | (1u << 15) | (1u << 13) | (2u << 8);   // AMD, legacy, 64-bit, 3 comparators
	h->base = (acpi_gas_t){ ACPI_SPACE_MEMORY, 64, 0, 0, 0xFED00000 };
	h->min_tick = 0x37EE;
	seal(t);
}

// ── The machine that crashed the old walker ─────────────────────────────────
// The photo's numbers: the XSDT at 0xca455728 and a table at 0xca349000, a
// megabyte BELOW it. The rest are scattered further still, and one sits
// above 4 GB because the XSDT's pointers are 64-bit for a reason.

static void build_ryzen(void)
{
	rsdp(0x000F0000, 2, 0x0BEEF000, 0xCA455728);
	uint64_t entries[] = { 0xCA349000, 0xCA6F0000, 0x0C0000000ULL, 0x1CA455000ULL, 0x0D0001234 };
	root(0xCA455728, "XSDT", entries, 5);
	fadt(0xCA349000, 276, 0xCA300000, 0);
	madt(0xCA6F0000, true);
	mcfg(0x0C0000000ULL);
	hpet(0x1CA455000ULL);
	uint8_t *ssdt = table(0x0D0001234, "SSDT", 64, 2);
	seal(ssdt);
	dsdt(0xCA300000, true);
}

static void test_ryzen(void)
{
	machine_reset();
	build_ryzen();
	acpi_info_t info;
	acpi_parse(0x000F0000, fake_map, NULL, &info);

	CHECK(info.problem_count == 0, "a clean machine has no problems (got %u, first code %u)",
	      info.problem_count, info.problems[0].code);
	CHECK(strcmp(info.root_signature, "XSDT") == 0, "revision 2 walks the XSDT, got '%s'", info.root_signature);
	CHECK(info.table_count == 7, "XSDT + 5 entries + DSDT = 7 tables, got %u", info.table_count);
	CHECK(acpi_find(&info, "DSDT") && acpi_find(&info, "DSDT")->via_fadt, "the DSDT is reached through the FADT");
	CHECK(strcmp(info.tables[1].oem_id, "OS64") == 0, "OEM padding is trimmed, got '%s'", info.tables[1].oem_id);
	CHECK(strcmp(info.tables[1].oem_table_id, "TESTBED") == 0, "table ID padding is trimmed");

	CHECK(info.fadt.present && info.fadt.revision == 6 && info.fadt.minor_version == 4, "FADT 6.4");
	CHECK(info.fadt.pm1a_control == 0x804, "the generic PM1a block wins over the legacy port, got 0x%x", info.fadt.pm1a_control);
	CHECK(info.fadt.pm1a_event == 0x400, "with no generic block the legacy port stands");
	CHECK(info.fadt.pm_timer == 0x408 && info.fadt.pm_timer_32bit, "PM timer, 32-bit");
	CHECK(info.fadt.century == 0x32 && info.fadt.sci_interrupt == 9, "century and SCI");
	CHECK(info.fadt.reset_register.address == 0xCF9 && info.fadt.reset_value == 6, "reset register");
	CHECK(info.fadt.dsdt == 0xCA300000, "DSDT address");

	CHECK(info.madt.present && info.madt.lapic_address == 0xFEE00000, "MADT");
	CHECK(info.madt.cpus == 5 && info.madt.cpus_enabled == 4 && info.madt.cpus_online_capable == 1 && info.madt.x2apic_cpus == 1,
	      "cpus listed/enabled/online/x2apic = %u/%u/%u/%u", info.madt.cpus, info.madt.cpus_enabled,
	      info.madt.cpus_online_capable, info.madt.x2apic_cpus);
	CHECK(info.madt.ioapic_count == 2 && info.madt.ioapics[0].gsi_base == 24 && info.madt.ioapics[1].gsi_base == 0,
	      "both IOAPICs, in firmware order");
	CHECK(info.madt.override_count == 2 && info.madt.overrides[0].source == 0 && info.madt.overrides[0].gsi == 2,
	      "the PIT override");
	CHECK(info.madt.overrides[1].flags == 0x000F, "override flags kept");
	CHECK(info.madt.lapic_nmi_count == 1 && info.madt.lapic_nmis[0].uid == 0xFFFFFFFFu && info.madt.lapic_nmis[0].lint == 1,
	      "an 0xFF LAPIC NMI UID widens to all-ones");
	CHECK(info.madt.unknown_entries == 1, "an unknown entry is counted and stepped over");

	CHECK(info.mcfg_count == 2 && info.mcfg[0].base == 0xE0000000 && info.mcfg[0].end_bus == 255 && info.mcfg[1].segment == 1,
	      "both MCFG segments");
	CHECK(info.hpet.present && info.hpet.address == 0xFED00000 && info.hpet.comparators == 3 && info.hpet.counter_64bit
	      && info.hpet.legacy_replacement && info.hpet.vendor == 0x1022, "HPET above 4 GB decoded");
	CHECK(info.s5.present && info.s5.slp_typ_a == 5 && info.s5.slp_typ_b == 0,
	      "\\_S5 found past a decoy, got present=%d a=%u", info.s5.present, info.s5.slp_typ_a);
	CHECK(M.refusals == 0, "nothing asked for outside the machine's memory (%d refusals)", M.refusals);
}

// ACPI 1.0: the RSDT, 32-bit pointers, and a 116-byte FADT with no X_ fields.
// The FADT's copy-what-is-there rule is what keeps that last part from
// reading past the table; ASan is what proves it does.
static void test_rsdt_and_short_fadt(void)
{
	machine_reset();
	rsdp(0xE0000, 0, 0x7FFE0000, 0);
	uint64_t entries[] = { 0x7FF00000, 0x7FE00000 };
	root(0x7FFE0000, "RSDT", entries, 2);
	fadt(0x7FF00000, 116, 0x7FD00000, 0);
	madt(0x7FE00000, false);
	dsdt(0x7FD00000, false);

	acpi_info_t info;
	acpi_parse(0xE0000, fake_map, NULL, &info);
	CHECK(strcmp(info.root_signature, "RSDT") == 0, "revision 0 walks the RSDT");
	CHECK(info.fadt.present && info.fadt.revision == 1, "a version-1 FADT");
	CHECK(info.fadt.pm1a_control == 0x404, "no X_ fields: the legacy port, got 0x%x", info.fadt.pm1a_control);
	CHECK(info.fadt.minor_version == 0 && info.fadt.sleep_control.address == 0, "fields past the table read as absent");
	CHECK(!info.s5.present, "a DSDT whose _S5_ is not declared has no S5");
	CHECK(info.problem_count == 0, "and none of that is a problem (%u)", info.problem_count);
}

// The FADT names its DSDT only in the 64-bit field — modern firmware's habit.
static void test_x_dsdt_only(void)
{
	machine_reset();
	rsdp(0xE0000, 2, 0, 0x100000000ULL);
	uint64_t entries[] = { 0x200000000ULL };
	root(0x100000000ULL, "XSDT", entries, 1);
	fadt(0x200000000ULL, 276, 0, 0x300000000ULL);
	dsdt(0x300000000ULL, true);

	acpi_info_t info;
	acpi_parse(0xE0000, fake_map, NULL, &info);
	CHECK(info.fadt.dsdt == 0x300000000ULL && info.s5.present, "X_DSDT is followed when the legacy field is zero");
}

// A broken XSDT is not the end: the RSDT beside it is walked instead.
static void test_xsdt_falls_back(void)
{
	machine_reset();
	rsdp(0xE0000, 2, 0x7FFE0000, 0x7FFF0000);
	uint8_t *bad = table(0x7FFF0000, "XSDX", 36, 1);
	seal(bad);
	uint64_t entries[] = { 0x7FE00000 };
	root(0x7FFE0000, "RSDT", entries, 1);
	madt(0x7FE00000, false);

	acpi_info_t info;
	acpi_parse(0xE0000, fake_map, NULL, &info);
	CHECK(strcmp(info.root_signature, "RSDT") == 0 && info.madt.present, "fell back to the RSDT");
	CHECK(info.problem_count == 1 && info.problems[0].code == ACPI_PROBLEM_ROOT_SIGNATURE,
	      "and said why (%u problems, first %u)", info.problem_count, info.problems[0].code);
}

// Every way a root entry can be wrong: zero, unmapped, a signature that is
// not text, a Length too small and too large, a bad checksum. Each is a
// problem with its own code; the bad checksum is still USED; and the walk
// reaches the good table at the end every time.
static void test_bad_entries(void)
{
	machine_reset();
	rsdp(0xE0000, 2, 0, 0x10000000);
	uint64_t entries[] = { 0, 0x66600000, 0x20000000, 0x30000000, 0x40000000, 0x50000000, 0x60000000 };
	root(0x10000000, "XSDT", entries, 7);
	uint8_t *garbage = table(0x20000000, "\x01\x02\x03\x04", 36, 1);
	(void)garbage;
	uint8_t *tiny = table(0x30000000, "SSDT", 36, 1);
	((acpi_header_t *)tiny)->length = 12;
	uint8_t *huge = table(0x40000000, "SSDT", 36, 1);
	((acpi_header_t *)huge)->length = 0xFFFFFFF0u;
	hpet(0x50000000);
	M.regions[M.region_count - 1].bytes[9] ^= 0x55;     // break the HPET's checksum
	madt(0x60000000, false);

	acpi_info_t info;
	acpi_parse(0xE0000, fake_map, NULL, &info);
	uint8_t want[] = { ACPI_PROBLEM_NULL_ENTRY, ACPI_PROBLEM_UNMAPPED, ACPI_PROBLEM_BAD_SIGNATURE,
	                   ACPI_PROBLEM_BAD_LENGTH, ACPI_PROBLEM_BAD_LENGTH, ACPI_PROBLEM_CHECKSUM };
	CHECK(info.problem_count == sizeof(want), "six problems, got %u", info.problem_count);
	for (unsigned i = 0; i < sizeof(want) && i < info.problem_count; i++)
		CHECK(info.problems[i].code == want[i], "problem %u is %u (%s), want %u", i, info.problems[i].code,
		      acpi_problem_name(info.problems[i].code), want[i]);
	CHECK(info.hpet.present && !acpi_find(&info, "HPET")->checksum_ok, "a bad checksum is reported AND used");
	CHECK(info.madt.present, "the walk reached the last entry");
}

// MADT entries that lie about their length end the MADT walk — what came
// before them is kept — and never cost a read past the table.
static void test_madt_damage(void)
{
	static const uint8_t cases[][2] = {
		{ ACPI_MADT_IOAPIC, 0 },      // length zero: would loop forever
		{ ACPI_MADT_IOAPIC, 4 },      // shorter than an IOAPIC entry
		{ ACPI_MADT_LAPIC, 200 },     // runs off the end of the table
		{ ACPI_MADT_LAPIC, 1 },       // shorter than the type and length themselves
	};
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		machine_reset();
		rsdp(0xE0000, 0, 0x1000, 0);
		uint64_t entries[] = { 0x2000 };
		root(0x1000, "RSDT", entries, 1);
		uint8_t *t = table(0x2000, "APIC", sizeof(acpi_madt_t) + 8 + 4, 1);
		acpi_madt_lapic_t l = { ACPI_MADT_LAPIC, 8, 0, 0, ACPI_MADT_CPU_ENABLED };
		memcpy(t + sizeof(acpi_madt_t), &l, 8);
		t[sizeof(acpi_madt_t) + 8] = cases[c][0];
		t[sizeof(acpi_madt_t) + 9] = cases[c][1];
		seal(t);

		acpi_info_t info;
		acpi_parse(0xE0000, fake_map, NULL, &info);
		CHECK(info.madt.cpus == 1, "case %u: the entry before the damage is kept", c);
		CHECK(info.problem_count == 1 && info.problems[0].code == ACPI_PROBLEM_MADT_ENTRY
		      && info.problems[0].value == sizeof(acpi_madt_t) + 8,
		      "case %u: the damage is named at its offset", c);
	}
}

static void test_no_rsdp(void)
{
	machine_reset();
	acpi_info_t info;
	acpi_parse(0xE0000, fake_map, NULL, &info);
	CHECK(info.problem_count == 2 && info.problems[1].code == ACPI_PROBLEM_NO_ROOT, "no RSDP, no root");

	machine_reset();
	uint8_t *r = region(0xE0000, 36);
	memcpy(r, "RSD PTX ", 8);
	acpi_parse(0xE0000, fake_map, NULL, &info);
	CHECK(info.problems[0].code == ACPI_PROBLEM_RSDP_SIGNATURE, "a wrong RSDP signature is named");
}

static void test_too_many_tables(void)
{
	machine_reset();
	enum { N = ACPI_MAX_TABLES + 5 };
	uint64_t entries[N];
	for (int i = 0; i < N; i++)
	{
		entries[i] = 0x100000 + (uint64_t)i * 0x1000;
		seal(table(entries[i], "SSDT", 36, 1));
	}
	rsdp(0xE0000, 0, 0x1000, 0);
	root(0x1000, "RSDT", entries, N);
	acpi_info_t info;
	acpi_parse(0xE0000, fake_map, NULL, &info);
	CHECK(info.table_count == ACPI_MAX_TABLES, "the registry fills and stops");
	CHECK(info.problem_count == 6, "the overflow is counted, one per table (RSDT + %d), got %u", N, info.problem_count);
	CHECK(info.problems[0].code == ACPI_PROBLEM_TOO_MANY && info.problems[0].value == ACPI_LIST_TABLES, "and named");
}

// ── Damaged machines ────────────────────────────────────────────────────────
// The Ryzen layout, then random bytes overwritten — anywhere, including
// pointers and Lengths — thousands of times. Nothing is asserted about the
// result except that the walk finishes without a sanitizer report: every
// read past what `map` handed out would be one.

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint32_t next(void)
{
	rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
	return (uint32_t)rng;
}

static void test_damage(int rounds)
{
	for (int round = 0; round < rounds; round++)
	{
		machine_reset();
		build_ryzen();
		int hits = 1 + (int)(next() % 6);
		for (int h = 0; h < hits; h++)
		{
			region_t *r = &M.regions[next() % (unsigned)M.region_count];
			uint32_t at = next() % r->length;
			switch (next() % 4)
			{
				case 0: r->bytes[at] = (uint8_t)next(); break;
				case 1: r->bytes[at] ^= (uint8_t)(1u << (next() % 8)); break;
				case 2: r->bytes[at] = 0; break;
				default: r->bytes[at] = 0xFF; break;
			}
		}
		acpi_info_t info;
		acpi_parse(0x000F0000, fake_map, NULL, &info);
		if (info.table_count > ACPI_MAX_TABLES || info.madt.ioapic_count > ACPI_MAX_IOAPICS)
			CHECK(0, "round %d: a list overran its array", round);
	}
}

static void test_names(void)
{
	CHECK(strcmp(acpi_problem_name(0), "unknown") == 0 && strcmp(acpi_problem_name(200), "unknown") == 0, "unknown codes");
	for (uint8_t c = 1; c < ACPI_PROBLEM_COUNT; c++)
		CHECK(strcmp(acpi_problem_name(c), "unknown") != 0, "problem %u has a name", c);
	CHECK(strcmp(acpi_profile_name(1), "desktop") == 0 && strcmp(acpi_profile_name(99), "unknown") == 0, "profiles");
	CHECK(strcmp(acpi_fadt_flag_name(20), "hardware-reduced") == 0 && acpi_fadt_flag_name(31) == NULL, "FADT flags");
	CHECK(strcmp(acpi_boot_arch_flag_name(1), "8042") == 0 && acpi_boot_arch_flag_name(9) == NULL, "boot arch flags");
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 20000;
	test_ryzen();
	test_rsdt_and_short_fadt();
	test_x_dsdt_only();
	test_xsdt_falls_back();
	test_bad_entries();
	test_madt_damage();
	test_no_rsdp();
	test_too_many_tables();
	test_names();
	test_damage(rounds);
	machine_reset();
	if (failures)
	{
		printf("test_acpi_host: %d failures\n", failures);
		return 1;
	}
	printf("test_acpi_host: all checks passed (%d damaged machines walked)\n", rounds);
	return 0;
}
