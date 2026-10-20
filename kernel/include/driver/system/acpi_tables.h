// acpi_tables.h — the firmware's ACPI tables, walked and decoded.
//
// PURE: stdint, stdbool, stddef, nothing else, so tools/test_acpi_host.sh
// drives it under ASan with plain cc. The kernel half (acpi.c) owns every
// side effect — mapping, globals, the S5 descent — and hands this file a
// way to read memory.
//
// THE RULE THIS FILE EXISTS TO KEEP: a byte of firmware memory is read only
// after `map` has been asked for it. Firmware puts its tables wherever it
// likes, and each one is mapped by its own address and its own Length —
// first the 36-byte header (which is how the Length is learned), then the
// whole table. The host suite enforces the rule mechanically: its `map`
// returns a heap copy of exactly the bytes requested, so a read one byte
// past them is an ASan report rather than a lucky hit.
//
// The registry and every decoded fact land in one acpi_info_t, written by
// acpi_parse and never changed afterwards; /sys/acpi renders it.

#ifndef ACPI_TABLES_H
#define ACPI_TABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── The spec's structures ───────────────────────────────────────────────────
// Every one is packed and size-asserted: they are laid down by a spec, and an
// unpacked one shifts every field after its first misaligned member without a
// word (SUCCESSION.md has the evening that cost).

typedef struct
{
	char     signature[8];        // "RSD PTR "
	uint8_t  checksum;            // the first 20 bytes sum to zero
	char     oem_id[6];
	uint8_t  revision;            // 0 = ACPI 1.0 (RSDT only); 2+ adds the XSDT
	uint32_t rsdt_address;
	uint32_t length;              // 2+: the whole structure
	uint64_t xsdt_address;        // 2+
	uint8_t  extended_checksum;   // 2+: all `length` bytes sum to zero
	uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

_Static_assert(sizeof(acpi_rsdp_t) == 36, "RSDP is 36 bytes");

#define ACPI_RSDP_V1_LENGTH 20u

typedef struct
{
	char     signature[4];
	uint32_t length;              // the whole table, this header included
	uint8_t  revision;
	uint8_t  checksum;            // all `length` bytes sum to zero
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed)) acpi_header_t;

_Static_assert(sizeof(acpi_header_t) == 36, "SDT header is 36 bytes");

// The Generic Address Structure: TWELVE bytes. Unpacked it is sixteen, and
// because the FADT embeds these by value, every FADT field after ResetReg
// would drift four bytes per structure.
typedef struct
{
	uint8_t  space;               // ACPI_SPACE_*
	uint8_t  bit_width;
	uint8_t  bit_offset;
	uint8_t  access_size;
	uint64_t address;
} __attribute__((packed)) acpi_gas_t;

_Static_assert(sizeof(acpi_gas_t) == 12, "Generic Address Structure is 12 bytes");

#define ACPI_SPACE_MEMORY 0
#define ACPI_SPACE_IO     1
#define ACPI_SPACE_PCI    2

// The FADT ("FACP"), through ACPI 6.5. A table is only as long as its own
// Length says: a revision-1 FADT ends at 116 and has no X_ fields at all,
// which is why acpi_parse copies min(Length, sizeof) into a zeroed one
// instead of reading the firmware's bytes in place.
typedef struct
{
	acpi_header_t h;
	uint32_t firmware_ctrl;       // the FACS
	uint32_t dsdt;
	uint8_t  reserved0;           // ACPI 1.0's INT_MODEL
	uint8_t  pm_profile;          // ACPI_PROFILE_*
	uint16_t sci_interrupt;
	uint32_t smi_command;         // port; 0 = the machine is always in ACPI mode
	uint8_t  acpi_enable;
	uint8_t  acpi_disable;
	uint8_t  s4bios_req;
	uint8_t  pstate_control;
	uint32_t pm1a_event_block;
	uint32_t pm1b_event_block;
	uint32_t pm1a_control_block;
	uint32_t pm1b_control_block;
	uint32_t pm2_control_block;
	uint32_t pm_timer_block;
	uint32_t gpe0_block;
	uint32_t gpe1_block;
	uint8_t  pm1_event_length;
	uint8_t  pm1_control_length;
	uint8_t  pm2_control_length;
	uint8_t  pm_timer_length;
	uint8_t  gpe0_length;
	uint8_t  gpe1_length;
	uint8_t  gpe1_base;
	uint8_t  cstate_control;
	uint16_t worst_c2_latency;
	uint16_t worst_c3_latency;
	uint16_t flush_size;
	uint16_t flush_stride;
	uint8_t  duty_offset;
	uint8_t  duty_width;
	uint8_t  day_alarm;           // CMOS index of the RTC's day-of-month alarm; 0 = none
	uint8_t  month_alarm;
	uint8_t  century;             // CMOS index of the RTC's century byte; 0 = none
	uint16_t boot_arch;           // IA-PC boot architecture flags: ACPI_BOOT_*
	uint8_t  reserved1;
	uint32_t flags;               // fixed feature flags: ACPI_FADT_*
	acpi_gas_t reset_register;
	uint8_t  reset_value;
	uint16_t arm_boot_arch;
	uint8_t  minor_version;       // the FADT's revision is h.revision.minor_version
	uint64_t x_firmware_ctrl;
	uint64_t x_dsdt;
	acpi_gas_t x_pm1a_event_block;
	acpi_gas_t x_pm1b_event_block;
	acpi_gas_t x_pm1a_control_block;
	acpi_gas_t x_pm1b_control_block;
	acpi_gas_t x_pm2_control_block;
	acpi_gas_t x_pm_timer_block;
	acpi_gas_t x_gpe0_block;
	acpi_gas_t x_gpe1_block;
	acpi_gas_t sleep_control;     // hardware-reduced ACPI's S-state door
	acpi_gas_t sleep_status;
	uint64_t hypervisor_vendor;
} __attribute__((packed)) acpi_fadt_t;

// The spec's own offsets, checked: a field in the wrong place is a build
// failure, not a wrong port number found at power-off.
_Static_assert(__builtin_offsetof(acpi_fadt_t, pm1a_control_block) == 64, "FADT PM1a_CNT_BLK");
_Static_assert(__builtin_offsetof(acpi_fadt_t, century) == 108, "FADT CENTURY");
_Static_assert(__builtin_offsetof(acpi_fadt_t, boot_arch) == 109, "FADT IAPC_BOOT_ARCH");
_Static_assert(__builtin_offsetof(acpi_fadt_t, flags) == 112, "FADT Flags");
_Static_assert(__builtin_offsetof(acpi_fadt_t, reset_register) == 116, "FADT RESET_REG");
_Static_assert(__builtin_offsetof(acpi_fadt_t, minor_version) == 131, "FADT Minor Version");
_Static_assert(__builtin_offsetof(acpi_fadt_t, x_dsdt) == 140, "FADT X_DSDT");
_Static_assert(__builtin_offsetof(acpi_fadt_t, x_pm1a_control_block) == 172, "FADT X_PM1a_CNT_BLK");
_Static_assert(__builtin_offsetof(acpi_fadt_t, x_pm_timer_block) == 208, "FADT X_PM_TMR_BLK");
_Static_assert(__builtin_offsetof(acpi_fadt_t, sleep_control) == 244, "FADT SLEEP_CONTROL_REG");
_Static_assert(sizeof(acpi_fadt_t) == 276, "FADT through ACPI 6.5 is 276 bytes");

// FADT Flags bits worth a name (the full list is the spec's table 5.10).
#define ACPI_FADT_WBINVD            (1u << 0)
#define ACPI_FADT_PWR_BUTTON        (1u << 4)    // SET = the power button is NOT a fixed feature
#define ACPI_FADT_SLP_BUTTON        (1u << 5)    // SET = the sleep button is NOT a fixed feature
#define ACPI_FADT_RTC_S4            (1u << 7)
#define ACPI_FADT_TMR_VAL_EXT       (1u << 8)    // the PM timer counts 32 bits, not 24
#define ACPI_FADT_RESET_REG_SUP     (1u << 10)
#define ACPI_FADT_HW_REDUCED        (1u << 20)
#define ACPI_FADT_LOW_POWER_S0      (1u << 21)

// IA-PC boot architecture flags.
#define ACPI_BOOT_LEGACY_DEVICES    (1u << 0)
#define ACPI_BOOT_8042              (1u << 1)
#define ACPI_BOOT_NO_VGA            (1u << 2)
#define ACPI_BOOT_NO_MSI            (1u << 3)
#define ACPI_BOOT_NO_ASPM           (1u << 4)
#define ACPI_BOOT_NO_CMOS_RTC       (1u << 5)

typedef struct
{
	acpi_header_t h;
	uint32_t lapic_address;
	uint32_t flags;               // bit 0: the machine also has 8259 PICs
} __attribute__((packed)) acpi_madt_t;

_Static_assert(sizeof(acpi_madt_t) == 44, "MADT fixed part is 44 bytes");

#define ACPI_MADT_LAPIC            0
#define ACPI_MADT_IOAPIC           1
#define ACPI_MADT_OVERRIDE         2
#define ACPI_MADT_NMI_SOURCE       3
#define ACPI_MADT_LAPIC_NMI        4
#define ACPI_MADT_LAPIC_OVERRIDE   5
#define ACPI_MADT_X2APIC           9
#define ACPI_MADT_X2APIC_NMI       10

typedef struct { uint8_t type, length, uid, apic_id; uint32_t flags; } __attribute__((packed)) acpi_madt_lapic_t;
typedef struct { uint8_t type, length, id, reserved; uint32_t address, gsi_base; } __attribute__((packed)) acpi_madt_ioapic_t;
typedef struct { uint8_t type, length, bus, source; uint32_t gsi; uint16_t flags; } __attribute__((packed)) acpi_madt_override_t;
typedef struct { uint8_t type, length; uint16_t flags; uint32_t gsi; } __attribute__((packed)) acpi_madt_nmi_source_t;
typedef struct { uint8_t type, length, uid; uint16_t flags; uint8_t lint; } __attribute__((packed)) acpi_madt_lapic_nmi_t;
typedef struct { uint8_t type, length; uint16_t reserved; uint64_t address; } __attribute__((packed)) acpi_madt_lapic_override_t;
typedef struct { uint8_t type, length; uint16_t reserved; uint32_t x2apic_id, flags, uid; } __attribute__((packed)) acpi_madt_x2apic_t;
typedef struct { uint8_t type, length; uint16_t flags; uint32_t uid; uint8_t lint, reserved[3]; } __attribute__((packed)) acpi_madt_x2apic_nmi_t;

_Static_assert(sizeof(acpi_madt_lapic_t) == 8, "MADT LAPIC");
_Static_assert(sizeof(acpi_madt_ioapic_t) == 12, "MADT IOAPIC");
_Static_assert(sizeof(acpi_madt_override_t) == 10, "MADT interrupt source override");
_Static_assert(sizeof(acpi_madt_nmi_source_t) == 8, "MADT NMI source");
_Static_assert(sizeof(acpi_madt_lapic_nmi_t) == 6, "MADT LAPIC NMI");
_Static_assert(sizeof(acpi_madt_lapic_override_t) == 12, "MADT LAPIC address override");
_Static_assert(sizeof(acpi_madt_x2apic_t) == 16, "MADT x2APIC");
_Static_assert(sizeof(acpi_madt_x2apic_nmi_t) == 12, "MADT x2APIC NMI");

#define ACPI_MADT_CPU_ENABLED         (1u << 0)
#define ACPI_MADT_CPU_ONLINE_CAPABLE  (1u << 1)

typedef struct
{
	uint64_t base;                // ECAM address of BUS ZERO, even when start_bus is not
	uint16_t segment;
	uint8_t  start_bus;
	uint8_t  end_bus;
	uint32_t reserved;
} __attribute__((packed)) acpi_mcfg_entry_t;

_Static_assert(sizeof(acpi_mcfg_entry_t) == 16, "MCFG allocation is 16 bytes");

#define ACPI_MCFG_ENTRIES_OFFSET 44u   // header + 8 reserved bytes

typedef struct
{
	acpi_header_t h;
	uint32_t   block_id;          // the HPET's own capabilities register, low half
	acpi_gas_t base;
	uint8_t    number;
	uint16_t   min_tick;
	uint8_t    page_protection;
} __attribute__((packed)) acpi_hpet_t;

_Static_assert(sizeof(acpi_hpet_t) == 56, "HPET table is 56 bytes");

// The firmware's boot logo, left in memory for the OS to keep on screen.
typedef struct
{
	acpi_header_t h;
	uint16_t version;
	uint8_t  status;              // bit 0: displayed; bits 1-2: rotation
	uint8_t  image_type;          // 0 = BMP
	uint64_t image_address;
	uint32_t offset_x;
	uint32_t offset_y;
} __attribute__((packed)) acpi_bgrt_t;

_Static_assert(sizeof(acpi_bgrt_t) == 56, "BGRT is 56 bytes");

// ── What the walk produces ──────────────────────────────────────────────────

// The decoded lists are fixed-size: a machine with more than these is
// recorded as a problem (ACPI_PROBLEM_TOO_MANY), never silently truncated.
#define ACPI_MAX_TABLES      64
#define ACPI_MAX_IOAPICS      8
#define ACPI_MAX_OVERRIDES   24
#define ACPI_MAX_LAPIC_NMIS   8
#define ACPI_MAX_MCFG         8
#define ACPI_MAX_PROBLEMS    16

// Larger than any table firmware ships (a big server DSDT is ~1 MiB); a
// Length past it is garbage, and mapping it would be how garbage costs.
#define ACPI_MAX_TABLE_LENGTH (4u * 1024u * 1024u)

typedef struct
{
	char     signature[5];
	char     oem_id[7];
	char     oem_table_id[9];
	uint8_t  revision;
	uint32_t length;
	uint64_t phys;
	bool     checksum_ok;
	bool     via_fadt;            // reached through the FADT, not the root table
} acpi_table_t;

typedef struct { uint8_t id; uint32_t address; uint32_t gsi_base; } acpi_ioapic_t;
typedef struct { uint8_t bus; uint8_t source; uint32_t gsi; uint16_t flags; } acpi_override_t;
typedef struct { uint32_t uid; uint16_t flags; uint8_t lint; } acpi_lapic_nmi_t;   // uid ~0 = every CPU
typedef struct { uint64_t base; uint16_t segment; uint8_t start_bus; uint8_t end_bus; } acpi_mcfg_t;

typedef enum
{
	ACPI_PROBLEM_UNMAPPED = 1,    // `map` refused the range (value = length)
	ACPI_PROBLEM_RSDP_SIGNATURE,
	ACPI_PROBLEM_RSDP_CHECKSUM,   // value = which checksum: 1 = first 20 bytes, 2 = extended
	ACPI_PROBLEM_ROOT_SIGNATURE,  // the RSDP's XSDT/RSDT pointer leads elsewhere
	ACPI_PROBLEM_NO_ROOT,         // neither root table is usable
	ACPI_PROBLEM_NULL_ENTRY,      // a root entry of zero (value = index)
	ACPI_PROBLEM_BAD_SIGNATURE,   // not four printable characters
	ACPI_PROBLEM_BAD_LENGTH,      // value = the Length claimed
	ACPI_PROBLEM_CHECKSUM,        // value = the sum; the table is still used
	ACPI_PROBLEM_TOO_MANY,        // a fixed list is full (value = which: ACPI_LIST_*)
	ACPI_PROBLEM_SHORT_TABLE,     // shorter than its own fixed part (value = length)
	ACPI_PROBLEM_MADT_ENTRY,      // malformed entry (value = its offset in the MADT)
	ACPI_PROBLEM_DSDT_SIGNATURE,  // the FADT's DSDT pointer leads elsewhere
	ACPI_PROBLEM_COUNT
} acpi_problem_code_t;

enum { ACPI_LIST_TABLES = 1, ACPI_LIST_IOAPICS, ACPI_LIST_OVERRIDES, ACPI_LIST_LAPIC_NMIS, ACPI_LIST_MCFG };

typedef struct
{
	uint8_t  code;                // acpi_problem_code_t
	char     signature[5];        // the table it concerns, "" if none
	uint64_t phys;
	uint64_t value;
} acpi_problem_t;

typedef struct
{
	bool       present;
	uint8_t    revision;
	uint8_t    minor_version;
	uint8_t    pm_profile;
	uint16_t   sci_interrupt;
	uint32_t   smi_command;
	uint8_t    acpi_enable;
	uint16_t   pm1a_event, pm1b_event;       // ports; 0 = absent
	uint16_t   pm1a_control, pm1b_control;
	uint16_t   pm_timer;
	bool       pm_timer_32bit;
	uint16_t   gpe0, gpe1;
	uint8_t    gpe0_length, gpe1_length;
	uint8_t    century;
	uint16_t   boot_arch;
	uint32_t   flags;
	acpi_gas_t reset_register;
	uint8_t    reset_value;
	acpi_gas_t sleep_control;
	uint64_t   dsdt;                          // the address actually followed
	uint64_t   hypervisor_vendor;
} acpi_fadt_info_t;

typedef struct
{
	bool             present;
	uint64_t         lapic_address;            // with any 64-bit override applied
	uint32_t         flags;
	uint32_t         cpus;                      // LAPIC + x2APIC entries
	uint32_t         cpus_enabled;
	uint32_t         cpus_online_capable;       // disabled now, may be brought online
	uint32_t         x2apic_cpus;
	acpi_ioapic_t    ioapics[ACPI_MAX_IOAPICS];
	uint32_t         ioapic_count;
	acpi_override_t  overrides[ACPI_MAX_OVERRIDES];
	uint32_t         override_count;
	acpi_lapic_nmi_t lapic_nmis[ACPI_MAX_LAPIC_NMIS];
	uint32_t         lapic_nmi_count;
	uint32_t         nmi_sources;
	uint32_t         unknown_entries;
} acpi_madt_info_t;

typedef struct
{
	bool     present;
	uint64_t address;
	uint8_t  space;
	uint8_t  number;
	uint16_t min_tick;
	uint8_t  comparators;
	bool     counter_64bit;
	bool     legacy_replacement;
	uint16_t vendor;
} acpi_hpet_info_t;

typedef struct
{
	bool     present;
	uint8_t  status;
	uint8_t  image_type;
	uint64_t image_address;
	uint32_t offset_x, offset_y;
} acpi_bgrt_info_t;

typedef struct
{
	bool    present;
	uint8_t slp_typ_a;        // PM1a_CNT's SLP_TYP field for S5, unshifted
	uint8_t slp_typ_b;
} acpi_s5_info_t;

typedef struct
{
	// The RSDP and the root table.
	uint64_t rsdp_phys;
	uint8_t  rsdp_revision;
	char     rsdp_oem_id[7];
	char     root_signature[5];   // "XSDT", "RSDT", or "" when neither worked
	uint64_t root_phys;
	uint32_t root_entries;

	acpi_table_t tables[ACPI_MAX_TABLES];
	uint32_t     table_count;

	acpi_fadt_info_t fadt;

	acpi_madt_info_t madt;

	acpi_mcfg_t mcfg[ACPI_MAX_MCFG];
	uint32_t    mcfg_count;

	acpi_hpet_info_t hpet;

	acpi_bgrt_info_t bgrt;

	acpi_s5_info_t s5;

	acpi_problem_t problems[ACPI_MAX_PROBLEMS];
	uint32_t       problem_count;   // can exceed ACPI_MAX_PROBLEMS: the overflow is counted, not kept
} acpi_info_t;

// Return a pointer through which [phys, phys + length) may be read, valid
// until acpi_parse returns — or NULL to refuse, which acpi_parse records as
// ACPI_PROBLEM_UNMAPPED and walks on.
typedef const void *(*acpi_map_fn)(void *ctx, uint64_t phys, uint32_t length);

// Walk everything reachable from the RSDP at `rsdp_phys` into `out` (which
// it zeroes first). It never fails as a whole: whatever could be read is in
// `out`, and whatever could not is in out->problems.
void acpi_parse(uint64_t rsdp_phys, acpi_map_fn map, void *ctx, acpi_info_t *out);

// The first registered table with this signature, or NULL.
const acpi_table_t *acpi_find(const acpi_info_t *info, const char *signature);

// Names for /sys/acpi and the log. A code or profile always gets a string
// ("unknown" when it is not one); a flag bit nobody has named gets NULL.
const char *acpi_problem_name(uint8_t code);
const char *acpi_profile_name(uint8_t profile);
const char *acpi_fadt_flag_name(unsigned bit);
const char *acpi_boot_arch_flag_name(unsigned bit);

#endif // ACPI_TABLES_H
