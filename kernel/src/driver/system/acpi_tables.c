// acpi_tables.c — see acpi_tables.h. Pure: no kernel or libc headers, and
// no firmware byte read except through the `map` it is handed.

#include "acpi_tables.h"

typedef struct
{
	acpi_info_t *info;
	acpi_map_fn  map;
	void        *ctx;
	bool         mcfg_seen;
} walk_t;

// Byte-wise: a root table's entries start at offset 36, so the XSDT's 64-bit
// pointers are never 8-aligned, and a cast would be undefined behaviour that
// the host's UBSan reports and x86 merely tolerates.
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static bool same4(const char *a, const char *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static uint8_t byte_sum(const uint8_t *p, uint32_t n)
{
	uint8_t sum = 0;
	for (uint32_t i = 0; i < n; i++)
		sum = (uint8_t)(sum + p[i]);
	return sum;
}

// Firmware pads its IDs with spaces or NULs; the padding is not the name.
static void copy_id(char *dst, const char *src, unsigned n)
{
	unsigned len = 0;
	for (unsigned i = 0; i < n && src[i] != '\0'; i++)
		len = i + 1;
	while (len > 0 && src[len - 1] == ' ')
		len--;
	for (unsigned i = 0; i < len; i++)
		dst[i] = src[i];
	dst[len] = '\0';
}

static void problem(walk_t *w, uint8_t code, const char *signature, uint64_t phys, uint64_t value)
{
	acpi_info_t *info = w->info;
	if (info->problem_count < ACPI_MAX_PROBLEMS)
	{
		acpi_problem_t *p = &info->problems[info->problem_count];
		p->code = code;
		for (unsigned i = 0; i < 4 && signature != NULL && signature[i] != '\0'; i++)
			p->signature[i] = signature[i];
		p->phys = phys;
		p->value = value;
	}
	info->problem_count++;
}

// Map a table by its own Length: the header first, because that is the only
// way to learn the Length, then all of it. `expect` (or NULL for "whatever
// this is") names the signature the caller was promised; a different one is
// reported as `wrong_signature`. On success *length is the table's Length
// and the result is readable for exactly that many bytes.
static const uint8_t *map_table(walk_t *w, uint64_t phys, const char *expect,
                                uint8_t wrong_signature, uint32_t *length)
{
	const acpi_header_t *h = w->map(w->ctx, phys, sizeof(acpi_header_t));
	if (h == NULL)
	{
		problem(w, ACPI_PROBLEM_UNMAPPED, expect, phys, sizeof(acpi_header_t));
		return NULL;
	}

	char sig[5] = { h->signature[0], h->signature[1], h->signature[2], h->signature[3], '\0' };
	for (int i = 0; i < 4; i++)
	{
		if ((uint8_t)sig[i] < 0x20 || (uint8_t)sig[i] > 0x7E)
		{
			problem(w, ACPI_PROBLEM_BAD_SIGNATURE, expect, phys, le32((const uint8_t *)h->signature));
			return NULL;
		}
	}
	if (expect != NULL && !same4(sig, expect))
	{
		problem(w, wrong_signature, sig, phys, 0);
		return NULL;
	}

	uint32_t len = h->length;
	if (len < sizeof(acpi_header_t) || len > ACPI_MAX_TABLE_LENGTH)
	{
		problem(w, ACPI_PROBLEM_BAD_LENGTH, sig, phys, len);
		return NULL;
	}

	const uint8_t *table = w->map(w->ctx, phys, len);
	if (table == NULL)
	{
		problem(w, ACPI_PROBLEM_UNMAPPED, sig, phys, len);
		return NULL;
	}
	*length = len;
	return table;
}

// A bad checksum is REPORTED, and the table is used anyway. Shipping firmware
// gets checksums wrong more often than it gets tables wrong, and refusing the
// MADT over one would cost the machine its interrupts; the signature and the
// Length have already been believed by the time a checksum can be computed.
static void register_table(walk_t *w, const uint8_t *table, uint32_t len, uint64_t phys, bool via_fadt)
{
	const acpi_header_t *h = (const acpi_header_t *)table;
	acpi_info_t *info = w->info;
	uint8_t sum = byte_sum(table, len);
	char sig[5] = { h->signature[0], h->signature[1], h->signature[2], h->signature[3], '\0' };

	if (sum != 0)
		problem(w, ACPI_PROBLEM_CHECKSUM, sig, phys, sum);

	if (info->table_count >= ACPI_MAX_TABLES)
	{
		problem(w, ACPI_PROBLEM_TOO_MANY, sig, phys, ACPI_LIST_TABLES);
		return;
	}
	acpi_table_t *t = &info->tables[info->table_count++];
	copy_id(t->signature, h->signature, 4);
	copy_id(t->oem_id, h->oem_id, 6);
	copy_id(t->oem_table_id, h->oem_table_id, 8);
	t->revision = h->revision;
	t->length = len;
	t->phys = phys;
	t->checksum_ok = (sum == 0);
	t->via_fadt = via_fadt;
}

// ── FADT ────────────────────────────────────────────────────────────────────

// A register can be described twice: the ACPI 1.0 32-bit port, and the 2.0+
// generic address. The generic one wins when it is present AND says SystemIO
// — a PM block in memory space is legal on paper and unseen in the wild, so
// one turning up gets the legacy port rather than an MMIO path nobody tested.
// Modern UEFI firmware often zeroes the legacy field and keeps the truth only
// in the generic one, which is why it is consulted first.
static uint16_t io_port(acpi_gas_t generic, uint32_t legacy)
{
	if (generic.address != 0 && generic.space == ACPI_SPACE_IO)
		return (uint16_t)generic.address;
	return (uint16_t)legacy;
}

#define ACPI_FADT_V1_LENGTH 116u

static void decode_fadt(walk_t *w, const uint8_t *table, uint32_t len, uint64_t phys)
{
	// Copy what the table HAS into a zeroed whole one: a field past the
	// table's own Length reads as absent instead of as whatever follows it.
	acpi_fadt_t f = { 0 };
	uint32_t n = len < sizeof(f) ? len : (uint32_t)sizeof(f);
	for (uint32_t i = 0; i < n; i++)
		((uint8_t *)&f)[i] = table[i];
	if (len < ACPI_FADT_V1_LENGTH)
		problem(w, ACPI_PROBLEM_SHORT_TABLE, "FACP", phys, len);

	acpi_fadt_info_t *out = &w->info->fadt;
	out->present = true;
	out->revision = f.h.revision;
	out->minor_version = f.minor_version;
	out->pm_profile = f.pm_profile;
	out->sci_interrupt = f.sci_interrupt;
	out->smi_command = f.smi_command;
	out->acpi_enable = f.acpi_enable;
	out->pm1a_event = io_port(f.x_pm1a_event_block, f.pm1a_event_block);
	out->pm1b_event = io_port(f.x_pm1b_event_block, f.pm1b_event_block);
	out->pm1a_control = io_port(f.x_pm1a_control_block, f.pm1a_control_block);
	out->pm1b_control = io_port(f.x_pm1b_control_block, f.pm1b_control_block);
	out->pm_timer = io_port(f.x_pm_timer_block, f.pm_timer_block);
	out->pm_timer_32bit = (f.flags & ACPI_FADT_TMR_VAL_EXT) != 0;
	out->gpe0 = io_port(f.x_gpe0_block, f.gpe0_block);
	out->gpe1 = io_port(f.x_gpe1_block, f.gpe1_block);
	out->gpe0_length = f.gpe0_length;
	out->gpe1_length = f.gpe1_length;
	out->century = f.century;
	out->boot_arch = f.boot_arch;
	out->flags = f.flags;
	out->reset_register = f.reset_register;
	out->reset_value = f.reset_value;
	out->sleep_control = f.sleep_control;
	out->hypervisor_vendor = f.hypervisor_vendor;

	// The DSDT's address is in the FADT twice. LEGACY FIRST, the 64-bit one
	// as the fallback — the opposite of io_port's order, because the risks
	// run the other way: the 32-bit field has been right on every machine
	// since 1996 and no firmware puts a DSDT above 4 GB, so when it is
	// non-zero it is the safer of two answers that agree, while a wrong
	// 64-bit one is a mapping of somewhere nobody meant. X_DSDT is for the
	// firmware that zeroes the legacy field.
	out->dsdt = f.dsdt != 0 ? f.dsdt : f.x_dsdt;
}

// ── MADT ────────────────────────────────────────────────────────────────────

static void count_cpu(walk_t *w, uint32_t flags, bool x2apic)
{
	acpi_madt_info_t *m = &w->info->madt;
	m->cpus++;
	if (x2apic)
		m->x2apic_cpus++;
	if (flags & ACPI_MADT_CPU_ENABLED)
		m->cpus_enabled++;
	else if (flags & ACPI_MADT_CPU_ONLINE_CAPABLE)
		m->cpus_online_capable++;
}

static void add_lapic_nmi(walk_t *w, uint32_t uid, uint16_t flags, uint8_t lint, uint64_t phys)
{
	acpi_madt_info_t *m = &w->info->madt;
	if (m->lapic_nmi_count >= ACPI_MAX_LAPIC_NMIS)
	{
		problem(w, ACPI_PROBLEM_TOO_MANY, "APIC", phys, ACPI_LIST_LAPIC_NMIS);
		return;
	}
	m->lapic_nmis[m->lapic_nmi_count++] = (acpi_lapic_nmi_t){ uid, flags, lint };
}

// The length a decoder needs before it may read an entry of this type; 2
// (just type and length) for a type nobody here reads.
static uint32_t madt_entry_need(uint8_t type)
{
	switch (type)
	{
		case ACPI_MADT_LAPIC:          return sizeof(acpi_madt_lapic_t);
		case ACPI_MADT_IOAPIC:         return sizeof(acpi_madt_ioapic_t);
		case ACPI_MADT_OVERRIDE:       return sizeof(acpi_madt_override_t);
		case ACPI_MADT_NMI_SOURCE:     return sizeof(acpi_madt_nmi_source_t);
		case ACPI_MADT_LAPIC_NMI:      return sizeof(acpi_madt_lapic_nmi_t);
		case ACPI_MADT_LAPIC_OVERRIDE: return sizeof(acpi_madt_lapic_override_t);
		case ACPI_MADT_X2APIC:         return sizeof(acpi_madt_x2apic_t);
		case ACPI_MADT_X2APIC_NMI:     return sizeof(acpi_madt_x2apic_nmi_t);
		default:                       return 2;
	}
}

static void decode_madt(walk_t *w, const uint8_t *table, uint32_t len, uint64_t phys)
{
	acpi_madt_info_t *m = &w->info->madt;
	if (len < sizeof(acpi_madt_t))
	{
		problem(w, ACPI_PROBLEM_SHORT_TABLE, "APIC", phys, len);
		return;
	}
	const acpi_madt_t *madt = (const acpi_madt_t *)table;
	m->present = true;
	m->lapic_address = madt->lapic_address;
	m->flags = madt->flags;

	// Every entry is [type, length, ...], and the length is the only way to
	// the next one. A length that cannot be right ends the walk: everything
	// after it would be read at a guessed offset.
	uint32_t at = sizeof(acpi_madt_t);
	while (at + 2 <= len)
	{
		const uint8_t *e = table + at;
		uint8_t type = e[0];
		uint8_t elen = e[1];
		if (elen < 2 || elen > len - at || elen < madt_entry_need(type))
		{
			problem(w, ACPI_PROBLEM_MADT_ENTRY, "APIC", phys, at);
			return;
		}

		switch (type)
		{
			case ACPI_MADT_LAPIC:
			{
				const acpi_madt_lapic_t *x = (const acpi_madt_lapic_t *)e;
				count_cpu(w, x->flags, false);
				break;
			}
			case ACPI_MADT_X2APIC:
			{
				const acpi_madt_x2apic_t *x = (const acpi_madt_x2apic_t *)e;
				count_cpu(w, x->flags, true);
				break;
			}
			case ACPI_MADT_IOAPIC:
			{
				const acpi_madt_ioapic_t *x = (const acpi_madt_ioapic_t *)e;
				if (m->ioapic_count >= ACPI_MAX_IOAPICS)
					problem(w, ACPI_PROBLEM_TOO_MANY, "APIC", phys, ACPI_LIST_IOAPICS);
				else
					m->ioapics[m->ioapic_count++] = (acpi_ioapic_t){ x->id, x->address, x->gsi_base };
				break;
			}
			case ACPI_MADT_OVERRIDE:
			{
				const acpi_madt_override_t *x = (const acpi_madt_override_t *)e;
				if (m->override_count >= ACPI_MAX_OVERRIDES)
					problem(w, ACPI_PROBLEM_TOO_MANY, "APIC", phys, ACPI_LIST_OVERRIDES);
				else
					m->overrides[m->override_count++] = (acpi_override_t){ x->bus, x->source, x->gsi, x->flags };
				break;
			}
			case ACPI_MADT_NMI_SOURCE:
				m->nmi_sources++;
				break;
			case ACPI_MADT_LAPIC_NMI:
			{
				// A one-byte UID of 0xFF means every processor; widened so it
				// reads the same as the x2APIC form's all-ones.
				const acpi_madt_lapic_nmi_t *x = (const acpi_madt_lapic_nmi_t *)e;
				add_lapic_nmi(w, x->uid == 0xFF ? 0xFFFFFFFFu : x->uid, x->flags, x->lint, phys);
				break;
			}
			case ACPI_MADT_X2APIC_NMI:
			{
				const acpi_madt_x2apic_nmi_t *x = (const acpi_madt_x2apic_nmi_t *)e;
				add_lapic_nmi(w, x->uid, x->flags, x->lint, phys);
				break;
			}
			case ACPI_MADT_LAPIC_OVERRIDE:
			{
				const acpi_madt_lapic_override_t *x = (const acpi_madt_lapic_override_t *)e;
				m->lapic_address = x->address;
				break;
			}
			default:
				m->unknown_entries++;
				break;
		}
		at += elen;
	}
}

// ── MCFG, HPET, BGRT ────────────────────────────────────────────────────────

static void decode_mcfg(walk_t *w, const uint8_t *table, uint32_t len, uint64_t phys)
{
	acpi_info_t *info = w->info;
	if (len < ACPI_MCFG_ENTRIES_OFFSET)
	{
		problem(w, ACPI_PROBLEM_SHORT_TABLE, "MCFG", phys, len);
		return;
	}
	uint32_t count = (len - ACPI_MCFG_ENTRIES_OFFSET) / sizeof(acpi_mcfg_entry_t);
	for (uint32_t i = 0; i < count; i++)
	{
		const acpi_mcfg_entry_t *e = (const acpi_mcfg_entry_t *)
			(table + ACPI_MCFG_ENTRIES_OFFSET + i * sizeof(acpi_mcfg_entry_t));
		if (info->mcfg_count >= ACPI_MAX_MCFG)
		{
			problem(w, ACPI_PROBLEM_TOO_MANY, "MCFG", phys, ACPI_LIST_MCFG);
			return;
		}
		info->mcfg[info->mcfg_count++] = (acpi_mcfg_t){ e->base, e->segment, e->start_bus, e->end_bus };
	}
}

static void decode_hpet(walk_t *w, const uint8_t *table, uint32_t len, uint64_t phys)
{
	if (len < sizeof(acpi_hpet_t))
	{
		problem(w, ACPI_PROBLEM_SHORT_TABLE, "HPET", phys, len);
		return;
	}
	const acpi_hpet_t *h = (const acpi_hpet_t *)table;
	acpi_hpet_info_t *out = &w->info->hpet;
	out->present = true;
	out->address = h->base.address;
	out->space = h->base.space;
	out->number = h->number;
	out->min_tick = h->min_tick;
	// The block ID is a copy of the HPET's capabilities register (low half):
	// comparators-minus-one in bits 8-12, the counter's width in bit 13,
	// legacy replacement in bit 15, the PCI vendor in the top sixteen.
	out->comparators = (uint8_t)(((h->block_id >> 8) & 0x1F) + 1);
	out->counter_64bit = (h->block_id & (1u << 13)) != 0;
	out->legacy_replacement = (h->block_id & (1u << 15)) != 0;
	out->vendor = (uint16_t)(h->block_id >> 16);
}

static void decode_bgrt(walk_t *w, const uint8_t *table, uint32_t len, uint64_t phys)
{
	if (len < sizeof(acpi_bgrt_t))
	{
		problem(w, ACPI_PROBLEM_SHORT_TABLE, "BGRT", phys, len);
		return;
	}
	const acpi_bgrt_t *b = (const acpi_bgrt_t *)table;
	acpi_bgrt_info_t *out = &w->info->bgrt;
	out->present = true;
	out->status = b->status;
	out->image_type = b->image_type;
	out->image_address = b->image_address;
	out->offset_x = b->offset_x;
	out->offset_y = b->offset_y;
}

// ── \_S5 ────────────────────────────────────────────────────────────────────
// \_S5 is an AML object in the DSDT, and evaluating AML properly means a name
// space, a parser and a method interpreter — a project, not a function. But
// \_S5 is always the same shape: a NAME holding a PACKAGE of small integers,
// nothing to evaluate. So find the name and read the package:
//
//   0x08              NameOp (optionally followed by '\' for root scope)
//   '_' 'S' '5' '_'   the name
//   0x12              PackageOp
//   <PkgLength>       1..4 bytes; the top two bits of the first byte say how
//                     many FOLLOW it
//   <NumElements>     one byte
//   <element>         0x0A <byte> for a small integer, or 0x00/0x01, which
//                     ARE the values zero and one with no payload
//
// The first two elements are SLP_TYPa and SLP_TYPb. THE COST IS NAMED: a
// machine that hides \_S5 behind something that must be executed has no S5
// here, and says so — acpi_poweroff reports it rather than pretending.

static bool s5_element(const uint8_t **p, const uint8_t *end, uint8_t *value)
{
	if (*p < end && **p == 0x0A)       // BytePrefix: the value follows
		(*p)++;
	if (*p >= end)
		return false;
	*value = **p;
	(*p)++;
	return true;
}

static void scan_s5(walk_t *w, const uint8_t *base, uint32_t len)
{
	const uint8_t *end = base + len;
	for (const uint8_t *s5 = base + sizeof(acpi_header_t); s5 + 5 <= end; s5++)
	{
		if (s5[0] != '_' || s5[1] != 'S' || s5[2] != '5' || s5[3] != '_')
			continue;
		// The four letters can sit in a table for other reasons — a
		// reference, a string. Only the declaration's shape is believed.
		// (The scan starts past the header, so s5[-2] is always in the table.)
		bool named = s5[-1] == 0x08 || (s5[-2] == 0x08 && s5[-1] == '\\');
		if (!named || s5[4] != 0x12)
			continue;

		const uint8_t *p = s5 + 5;
		if (p >= end)
			return;
		uint32_t skip = ((uint32_t)(*p & 0xC0) >> 6) + 2;   // PkgLength, then NumElements
		if (skip > (uint32_t)(end - p))
			return;
		p += skip;

		uint8_t a = 0, b = 0;
		if (!s5_element(&p, end, &a))
			return;
		s5_element(&p, end, &b);           // a package of one leaves TYPb zero
		w->info->s5.present = true;
		w->info->s5.slp_typ_a = a;
		w->info->s5.slp_typ_b = b;
		return;
	}
}

// ── The walk ────────────────────────────────────────────────────────────────

static void visit(walk_t *w, uint64_t phys)
{
	uint32_t len = 0;
	const uint8_t *table = map_table(w, phys, NULL, 0, &len);
	if (table == NULL)
		return;
	register_table(w, table, len, phys, false);

	// The first of each kind is the one decoded: a second FADT or MADT is
	// firmware disagreeing with itself, and the registry still lists it.
	acpi_info_t *info = w->info;
	const char *sig = ((const acpi_header_t *)table)->signature;
	if (same4(sig, "FACP") && !info->fadt.present)
		decode_fadt(w, table, len, phys);
	else if (same4(sig, "APIC") && !info->madt.present)
		decode_madt(w, table, len, phys);
	else if (same4(sig, "MCFG") && !w->mcfg_seen)
	{
		w->mcfg_seen = true;
		decode_mcfg(w, table, len, phys);
	}
	else if (same4(sig, "HPET") && !info->hpet.present)
		decode_hpet(w, table, len, phys);
	else if (same4(sig, "BGRT") && !info->bgrt.present)
		decode_bgrt(w, table, len, phys);
}

// Walk one root table; false if it could not be read at all.
static bool walk_root(walk_t *w, uint64_t phys, const char *signature, uint32_t entry_size)
{
	uint32_t len = 0;
	const uint8_t *root = map_table(w, phys, signature, ACPI_PROBLEM_ROOT_SIGNATURE, &len);
	if (root == NULL)
		return false;
	register_table(w, root, len, phys, false);

	acpi_info_t *info = w->info;
	copy_id(info->root_signature, signature, 4);
	info->root_phys = phys;
	info->root_entries = (len - (uint32_t)sizeof(acpi_header_t)) / entry_size;

	for (uint32_t i = 0; i < info->root_entries; i++)
	{
		const uint8_t *e = root + sizeof(acpi_header_t) + i * entry_size;
		uint64_t table = entry_size == 8 ? le64(e) : le32(e);
		if (table == 0)
			problem(w, ACPI_PROBLEM_NULL_ENTRY, signature, phys, i);
		else
			visit(w, table);
	}
	return true;
}

void acpi_parse(uint64_t rsdp_phys, acpi_map_fn map, void *ctx, acpi_info_t *out)
{
	*out = (acpi_info_t){ 0 };
	walk_t w = { .info = out, .map = map, .ctx = ctx };
	out->rsdp_phys = rsdp_phys;

	const acpi_rsdp_t *rsdp = map(ctx, rsdp_phys, ACPI_RSDP_V1_LENGTH);
	if (rsdp == NULL)
	{
		problem(&w, ACPI_PROBLEM_UNMAPPED, NULL, rsdp_phys, ACPI_RSDP_V1_LENGTH);
		problem(&w, ACPI_PROBLEM_NO_ROOT, NULL, rsdp_phys, 0);
		return;
	}
	const char *want = "RSD PTR ";
	for (int i = 0; i < 8; i++)
	{
		if (rsdp->signature[i] != want[i])
		{
			problem(&w, ACPI_PROBLEM_RSDP_SIGNATURE, NULL, rsdp_phys, 0);
			problem(&w, ACPI_PROBLEM_NO_ROOT, NULL, rsdp_phys, 0);
			return;
		}
	}
	if (byte_sum((const uint8_t *)rsdp, ACPI_RSDP_V1_LENGTH) != 0)
		problem(&w, ACPI_PROBLEM_RSDP_CHECKSUM, NULL, rsdp_phys, 1);
	out->rsdp_revision = rsdp->revision;
	copy_id(out->rsdp_oem_id, rsdp->oem_id, 6);
	uint64_t rsdt = rsdp->rsdt_address;

	// The XSDT when there is one, the RSDT when there is not — or when the
	// XSDT turns out not to be one, which is exactly when an older table
	// still pointing somewhere sane is worth having.
	bool walked = false;
	if (rsdp->revision >= 2)
	{
		const acpi_rsdp_t *full = map(ctx, rsdp_phys, sizeof(acpi_rsdp_t));
		if (full == NULL)
			problem(&w, ACPI_PROBLEM_UNMAPPED, NULL, rsdp_phys, sizeof(acpi_rsdp_t));
		else
		{
			if (byte_sum((const uint8_t *)full, sizeof(acpi_rsdp_t)) != 0)
				problem(&w, ACPI_PROBLEM_RSDP_CHECKSUM, NULL, rsdp_phys, 2);
			if (full->xsdt_address != 0)
				walked = walk_root(&w, full->xsdt_address, "XSDT", 8);
		}
	}
	if (!walked && rsdt != 0)
		walked = walk_root(&w, rsdt, "RSDT", 4);
	if (!walked)
	{
		problem(&w, ACPI_PROBLEM_NO_ROOT, NULL, rsdp_phys, 0);
		return;
	}

	if (out->fadt.present && out->fadt.dsdt != 0)
	{
		uint32_t len = 0;
		const uint8_t *dsdt = map_table(&w, out->fadt.dsdt, "DSDT", ACPI_PROBLEM_DSDT_SIGNATURE, &len);
		if (dsdt != NULL)
		{
			register_table(&w, dsdt, len, out->fadt.dsdt, true);
			scan_s5(&w, dsdt, len);
		}
	}
}

const acpi_table_t *acpi_find(const acpi_info_t *info, const char *signature)
{
	for (uint32_t i = 0; i < info->table_count; i++)
		if (same4(info->tables[i].signature, signature))
			return &info->tables[i];
	return NULL;
}

// ── Names ───────────────────────────────────────────────────────────────────

const char *acpi_problem_name(uint8_t code)
{
	static const char *const names[ACPI_PROBLEM_COUNT] = {
		[ACPI_PROBLEM_UNMAPPED]        = "could not be mapped",
		[ACPI_PROBLEM_RSDP_SIGNATURE]  = "RSDP signature is wrong",
		[ACPI_PROBLEM_RSDP_CHECKSUM]   = "RSDP checksum is wrong",
		[ACPI_PROBLEM_ROOT_SIGNATURE]  = "root table pointer leads to another table",
		[ACPI_PROBLEM_NO_ROOT]         = "no usable root table",
		[ACPI_PROBLEM_NULL_ENTRY]      = "root table entry is zero",
		[ACPI_PROBLEM_BAD_SIGNATURE]   = "signature is not text",
		[ACPI_PROBLEM_BAD_LENGTH]      = "length is not credible",
		[ACPI_PROBLEM_CHECKSUM]        = "checksum is wrong (table used anyway)",
		[ACPI_PROBLEM_TOO_MANY]        = "more entries than os64 keeps",
		[ACPI_PROBLEM_SHORT_TABLE]     = "shorter than its fixed part",
		[ACPI_PROBLEM_MADT_ENTRY]      = "malformed MADT entry ends the walk",
		[ACPI_PROBLEM_DSDT_SIGNATURE]  = "FADT's DSDT pointer leads to another table",
	};
	return code < ACPI_PROBLEM_COUNT && names[code] != NULL ? names[code] : "unknown";
}

const char *acpi_profile_name(uint8_t profile)
{
	static const char *const names[] = {
		"unspecified", "desktop", "mobile", "workstation", "enterprise-server",
		"soho-server", "appliance", "performance-server", "tablet",
	};
	return profile < sizeof(names) / sizeof(names[0]) ? names[profile] : "unknown";
}

const char *acpi_fadt_flag_name(unsigned bit)
{
	static const char *const names[] = {
		"wbinvd", "wbinvd-flush", "c1", "c2-mp", "power-button-not-fixed",
		"sleep-button-not-fixed", "rtc-not-fixed", "rtc-s4", "pm-timer-32bit",
		"docking", "reset-register", "sealed-case", "headless", "cpu-sw-slp",
		"pci-exp-wak", "use-platform-clock", "s4-rtc-sts-valid",
		"remote-power-on", "force-apic-cluster", "force-apic-physical",
		"hardware-reduced", "low-power-s0-idle", "persistent-cpu-caches",
	};
	return bit < sizeof(names) / sizeof(names[0]) ? names[bit] : NULL;
}

const char *acpi_boot_arch_flag_name(unsigned bit)
{
	static const char *const names[] = {
		"legacy-devices", "8042", "no-vga", "no-msi", "no-aspm", "no-cmos-rtc",
	};
	return bit < sizeof(names) / sizeof(names[0]) ? names[bit] : NULL;
}
