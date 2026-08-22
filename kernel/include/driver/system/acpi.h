/* 
 * File:   acpi.h
 * Author: yogi
 *
 * Created on May 18, 2016, 5:11 PM
 */

#ifndef ACPI_H
#define	ACPI_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>   // acpi_poweroff's verdict

typedef struct RSDPDescriptor {
 char Signature[8];
 uint8_t Checksum;
 char OEMID[6];
 uint8_t Revision;
 uint32_t RsdtAddress;
} __attribute__ ((packed)) RSDPDescriptorOrig_t;

typedef struct RSDPDescriptor20 {
 RSDPDescriptorOrig_t firstPart;
 
 uint32_t Length;
 uint64_t XsdtAddress;
 uint8_t ExtendedChecksum;
 uint8_t reserved[3];
} __attribute__ ((packed)) acpiRSDPHeader_t;

typedef struct acpiSDTHeader {
  char Signature[4];
  uint32_t Length;
  uint8_t Revision;
  uint8_t Checksum;
  char OEMID[6];
  char OEMTableID[8];
  uint32_t OEMRevision;
  uint32_t CreatorID;
  uint32_t CreatorRevision;
} __attribute__ ((packed)) acpiSDTHeader_t;

typedef struct RSDT_s {
    acpiSDTHeader_t header;
    uint32_t PointerToOtherSDT[];
} __attribute__((packed)) acpiRSDT_t;

typedef struct XSDT_s {
    acpiSDTHeader_t header;
    uint64_t PointerToOtherSDT[]; // 64-bit pointers
} __attribute__((packed)) acpiXSDT_t;

enum eAddressSpace
{
    SystemMemory=0,
    SystemIO,
    PCIConfigSpace,
    EmbeddedController,
    SMBus,
    FunctionalFixedHardware = 0x7F
};

// ACPI's Generic Address Structure: TWELVE BYTES, and it must be packed to
// say so. Without the attribute the uint64_t aligns to 8 and the struct is
// SIXTEEN — and because the FADT embeds these by value, every field after the
// first one drifts four bytes, with each further GAS adding four more.
//
// This was wrong from the day the header was written and cost a full evening
// on 2026-08-21, because the damage is invisible until you read a late field:
//   - X_Dsdt (four bytes adrift) read as 0x0000200100000000 under QEMU —
//     thirty-two terabytes, mapped and dereferenced, hanging the boot;
//   - X_PM1aControlBlock (sixteen adrift, two GAS's worth) read as nothing on
//     the P5, whose UEFI firmware — like most modern firmware — zeroes the
//     legacy 32-bit PM1a_CNT field and puts the truth ONLY in the extended
//     one. "FADT names no PM1a control block" was this, not a machine
//     without ACPI soft-off.
// The static assert below is the part that matters going forward: a layout
// this silent should fail at BUILD time, not at power-off time.
typedef struct GenericAddressStructure_s
{
  uint8_t AddressSpace;
  uint8_t BitWidth;
  uint8_t BitOffset;
  uint8_t AccessSize;
  uint64_t Address;
} __attribute__((packed)) GenericAddressStructure_t;

_Static_assert(sizeof(GenericAddressStructure_t) == 12,
               "ACPI Generic Address Structure must be 12 bytes — an unpacked "
               "one silently shifts every FADT field after ResetReg");

typedef struct FADT_s
{
    acpiSDTHeader_t h;
    uint32_t FirmwareCtrl;
    uint32_t Dsdt;
 
    // field used in ACPI 1.0; no longer in use, for compatibility only
    uint8_t  Reserved;
 
    uint8_t  PreferredPowerManagementProfile;
    uint16_t SCI_Interrupt;
    uint32_t SMI_CommandPort;
    uint8_t  AcpiEnable;
    uint8_t  AcpiDisable;
    uint8_t  S4BIOS_REQ;
    uint8_t  PSTATE_Control;
    uint32_t PM1aEventBlock;
    uint32_t PM1bEventBlock;
    uint32_t PM1aControlBlock;
    uint32_t PM1bControlBlock;
    uint32_t PM2ControlBlock;
    uint32_t PMTimerBlock;
    uint32_t GPE0Block;
    uint32_t GPE1Block;
    uint8_t  PM1EventLength;
    uint8_t  PM1ControlLength;
    uint8_t  PM2ControlLength;
    uint8_t  PMTimerLength;
    uint8_t  GPE0Length;
    uint8_t  GPE1Length;
    uint8_t  GPE1Base;
    uint8_t  CStateControl;
    uint16_t WorstC2Latency;
    uint16_t WorstC3Latency;
    uint16_t FlushSize;
    uint16_t FlushStride;
    uint8_t  DutyOffset;
    uint8_t  DutyWidth;
    uint8_t  DayAlarm;
    uint8_t  MonthAlarm;
    uint8_t  Century;
 
    // reserved in ACPI 1.0; used since ACPI 2.0+
    uint16_t BootArchitectureFlags;
 
    uint8_t  Reserved2;
    uint32_t Flags;
 
    // 12 byte structure; see below for details
    GenericAddressStructure_t ResetReg;
 
    uint8_t  ResetValue;
    uint8_t  Reserved3[3];
 
    // 64bit pointers - Available on ACPI 2.0+
    uint64_t                X_FirmwareControl;
    uint64_t                X_Dsdt;
 
    GenericAddressStructure_t X_PM1aEventBlock;
    GenericAddressStructure_t X_PM1bEventBlock;
    GenericAddressStructure_t X_PM1aControlBlock;
    GenericAddressStructure_t X_PM1bControlBlock;
    GenericAddressStructure_t X_PM2ControlBlock;
    GenericAddressStructure_t X_PMTimerBlock;
    GenericAddressStructure_t X_GPE0Block;
    GenericAddressStructure_t X_GPE1Block;
} __attribute__((packed)) acpiFADT_t;

// The offsets that matter, checked against the spec's own numbers. These are
// the two the S5 path reads, and both of them were wrong for years behind an
// unpacked GenericAddressStructure_t (see its comment).
_Static_assert(__builtin_offsetof(acpiFADT_t, PM1aControlBlock) == 64,
               "FADT: PM1a_CNT_BLK belongs at offset 64");
_Static_assert(__builtin_offsetof(acpiFADT_t, X_Dsdt) == 140,
               "FADT: X_DSDT belongs at offset 140");
_Static_assert(__builtin_offsetof(acpiFADT_t, X_PM1aControlBlock) == 172,
               "FADT: X_PM1a_CNT_BLK belongs at offset 172");

// Simplified MCFG Table Entry
typedef struct {
    uint64_t base_address;        // PCI configuration space base address
    uint16_t segment_group;       // PCI Segment Group Number
    uint8_t start_bus_number;     // Start Bus Number
    uint8_t end_bus_number;       // End Bus Number
    uint32_t reserved;            // Reserved (must be zero)
} __attribute__((packed)) acpi_mcfg_entry_t;


// Simplified MCFG Table Header (44 bytes + entries)
typedef struct {
    uint8_t signature[4];         // "MCFG"
    uint32_t length;              // Total table length
    uint8_t revision;             // Revision number
    uint8_t checksum;             // Checksum of entire table
    uint8_t oem_id[6];            // OEM ID
    uint8_t oem_table_id[8];      // OEM Table ID
    uint32_t oem_revision;        // OEM Revision number
    uint32_t creator_id;          // Creator ID
    uint32_t creator_revision;    // Creator Revision number
    uint64_t reserved;            // Reserved (8 bytes)
} __attribute__((packed)) acpi_mcfg_table_t;

typedef struct {
    uint8_t  type;    // = 1 for IO APIC
    uint8_t  length;  // length of this structure, e.g. 12 bytes
    uint8_t  ioapic_id;
    uint8_t  reserved; // or interrupt offset depending on specification revision
    uint32_t ioapic_addr;  // Physical address of the IO APIC
    uint32_t global_sys_int_base; // First GSI handled by this IO APIC
} __attribute__((packed)) IO_APIC_Entry;

// MADT type 2: Interrupt Source Override. Declares that an ISA IRQ is NOT
// wired to the identically-numbered IOAPIC input. The classic, near-universal
// example is the PIT: ISA IRQ 0 -> GSI 2. QEMU happens to also expose the MP
// tables in a way our lookup understood, but VirtualBox (and real hardware)
// only guarantee THIS — ignoring it routes IRQ0 to a masked IOAPIC pin and
// the system's tick heartbeat silently stops (July 2026 VBox bring-up bug).
typedef struct {
    uint8_t  type;    // = 2 for Interrupt Source Override
    uint8_t  length;  // 10
    uint8_t  bus;     // always 0 (ISA)
    uint8_t  source;  // ISA IRQ number being overridden
    uint32_t gsi;     // the global system interrupt it is actually wired to
    uint16_t flags;   // MPS INTI flags: bits 0-1 polarity, bits 2-3 trigger mode
} __attribute__((packed)) Interrupt_Source_Override_Entry;

// Per-ISA-IRQ routing gleaned from the MADT overrides above: kISAIrqToGSI[n]
// is the GSI for ISA IRQ n, or -1 if no override exists (identity mapping per
// the ACPI spec). kISAIrqOverrideFlags[n] holds the matching MPS INTI flags.
extern int16_t kISAIrqToGSI[16];
extern uint16_t kISAIrqOverrideFlags[16];

typedef struct {
    char Signature[4];      // Table signature (e.g., "MCFG", "FACP")
    uint32_t Length;        // Length of the table, including the header
    uint8_t Revision;       // Revision of the structure
    uint8_t Checksum;       // Checksum of the entire table
    char OEMID[6];          // OEM Identifier
    char OEMTableID[8];     // OEM Table Identifier
    uint32_t OEMRevision;   // OEM Revision
    uint32_t CreatorID;     // Creator ID
    uint32_t CreatorRevision; // Creator Revision
} __attribute__((packed)) acpi_table_header_t;

extern uintptr_t kPCIBaseAddress;
void acpiFindTables();

// ── S5: the machine actually going dark (2026-08-21) ────────────────────────
// The hypervisor poweroff ports (0x604, 0x4004) are decoded by QEMU and VBox
// and by nothing else, so on real iron the descent could only ever park with
// the 1995 liturgy on screen. This is the real mechanism: ACPI's soft-off.
//
// Parsed AT BOOT, fired AT SHUTDOWN, deliberately — the boot moment is when
// the ACPI tables are known-mapped and the machine is healthy, and the
// shutdown moment is the worst possible time to be walking firmware tables
// for the first time. acpi_prepare_s5() caches four numbers; acpi_poweroff()
// spends them.
//
// It is NOT an AML interpreter: \_S5 is found by scanning the DSDT for the
// name and reading the small package that follows. The full interpreter is a
// someday project (Chris, 2026-08-21: "when we need more of the DSDT").
void acpi_prepare_s5(void);

// Ask the machine to power off. Returns FALSE if it could not even try (no
// FADT, no \_S5, no PM1a control block) — the caller then falls back. On
// success it does not return, because the machine is off.
bool acpi_poweroff(void);

#endif	/* ACPI_H */
