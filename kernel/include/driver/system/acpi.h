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

typedef struct GenericAddressStructure_s
{
  uint8_t AddressSpace;
  uint8_t BitWidth;
  uint8_t BitOffset;
  uint8_t AccessSize;
  uint64_t Address;
} GenericAddressStructure_t;

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

#endif	/* ACPI_H */
