#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"	

#define NUM_BITS (sizeof(uint64_t) * 8) // Number of bits in a uint64_t
#define ITERATION_DELAY 100
#define DOORBELL_BASE_OFFSET 0x1000

#define PCI_COMMAND_OFFSET 0x04
#define PCI_BAR0_OFFSET 0x10
#define PCI_CSTS_OFFSET 0x1C
#define PCI_POWER_MGMT_AND_STTS 0x44
#define NVME_CSTS_RDY 1 // Ready bit mask
#define CSTS_READY_MASK 0x1
#define NVME_ABAR_REMAPPED_ADDRESS PCI_DEVICE_REMAP_BASE + 0xA2000000
#define NVME_ABAR_OVERRIDE_ADDRESS 0xA2000000 //Keep within the 32-bit space
#define NVME_REG_CAP         0x0000 // Controller Capabilities
#define NVME_REG_VERSION     0x0008 // Version
#define NVME_REG_INTMS       0x000c // Interrupt Mask Set
#define NVME_REG_INTMC       0x0010 // Interrupt Mask Clear
#define NVME_REG_CC          0x0014 // Controller Configuration
#define NVME_REG_CSTS        0x001c // Controller Status
#define NVME_REG_AQA         0x0024 // Admin Queue Attributes
#define NVME_REG_ASQ         0x0028 // Admin Submission Queue Base Address
#define NVME_REG_ACQ         0x0030 // Admin Completion Queue Base Address

#define DOORBELL_STRIDE (1 << dstrd) // DSTRD from CAP register
#define NVME_REG_SQ0TDBL 0x1000 // Doorbell register offset for SQ0 tail
#define NVME_REG_CQ0HDBL 0x1004
#define NVME_REG_SQ_TDBL(queue_id) (0x1000 + (4 * (queue_id)) * DOORBELL_STRIDE)
#define NVME_REG_CQ_HDBL(queue_id) (0x1000 + (4 * (queue_id) + 1) * DOORBELL_STRIDE)

#define NVME_ADMIN_OPC_IDENTIFY       0x06
#define NVME_ADMIN_OPC_GET_FEATURES   0x0a
#define NVME_ADMIN_OPC_SET_FEATURES   0x09

#define NVME_NVM_OPC_READ             0x02
#define NVME_NVM_OPC_WRITE            0x01

#define NVME_QUEUE_PRIO_URGENT        0
#define NVME_QUEUE_PRIO_HIGH          1
#define NVME_QUEUE_PRIO_MEDIUM        2
#define NVME_QUEUE_PRIO_LOW           3

#define NVME_SUBMISSION_QUEUE_SIZE    64
#define NVME_COMPLETION_QUEUE_SIZE    64

typedef enum {
    // Admin Commands
    NVME_ADMIN_DELETE_IO_SUBMISSION_QUEUE  = 0x00,
    NVME_ADMIN_CREATE_IO_SUBMISSION_QUEUE  = 0x01,
    NVME_ADMIN_GET_LOG_PAGE                = 0x02,
    NVME_ADMIN_DELETE_IO_COMPLETION_QUEUE  = 0x04,
    NVME_ADMIN_CREATE_IO_COMPLETION_QUEUE  = 0x05,
    NVME_ADMIN_IDENTIFY                    = 0x06,
    NVME_ADMIN_ABORT                       = 0x08,
    NVME_ADMIN_SET_FEATURES                = 0x09,
    NVME_ADMIN_GET_FEATURES                = 0x0A,
    NVME_ADMIN_ASYNC_EVENT_REQUEST         = 0x0C,
    NVME_ADMIN_NAMESPACE_MANAGEMENT        = 0x0D,
    NVME_ADMIN_FIRMWARE_ACTIVATE           = 0x10,
    NVME_ADMIN_FIRMWARE_IMAGE_DOWNLOAD     = 0x11,
    NVME_ADMIN_FORMAT_NVM                  = 0x80,
    NVME_ADMIN_SECURITY_SEND               = 0x81,
    NVME_ADMIN_SECURITY_RECEIVE            = 0x82
} nvme_admin_opcode_t;

typedef enum {
    NVME_OPCODE_FLUSH                = 0x00, // Flush
    NVME_OPCODE_WRITE                = 0x01, // Write
    NVME_OPCODE_READ                 = 0x02, // Read
    NVME_OPCODE_WRITE_UNCORRECTABLE  = 0x04, // Write Uncorrectable
    NVME_OPCODE_COMPARE              = 0x05, // Compare
    NVME_OPCODE_WRITE_ZEROES         = 0x08, // Write Zeroes
    NVME_OPCODE_DATASET_MANAGEMENT   = 0x09, // Dataset Management
    NVME_OPCODE_RESERVATION_REGISTER = 0x0D, // Reservation Register
    NVME_OPCODE_RESERVATION_REPORT   = 0x0E, // Reservation Report
    NVME_OPCODE_RESERVATION_ACQUIRE  = 0x11, // Reservation Acquire
    NVME_OPCODE_RESERVATION_RELEASE  = 0x15  // Reservation Release
} nvme_non_admin_opcodes_t;

#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint64_t nsze;     // Namespace Size (in LBAs)
    uint64_t ncap;     // Namespace Capacity (in LBAs)
    uint64_t nuse;     // Namespace Utilization (in LBAs)
    uint8_t  nsfeat;   // Namespace Features
    uint8_t  nlbaf;    // Number of LBA Formats (supported formats = nlbaf+1)
    uint8_t  flbas;    // Formatted LBA Size (bits: lower nibble = LBA format index, upper nibble = metadata placement)
    uint8_t  mc;       // Metadata Capabilities
    uint8_t  dpc;      // End-to-end Data Protection Capabilities
    uint8_t  dps;      // End-to-end Data Protection Type Settings
    uint8_t  nmic;     // Namespace Multipath IO and Sharing Capabilities
    uint8_t  rescap;   // Reservation Capabilities
    uint8_t  fpi;      // Format Progress Indicator
    uint8_t  dlfeat;   // Deallocate Logical Block Features
    uint16_t nawun;    // Namespace Atomic Write Unit Normal
    uint16_t nawupf;   // Namespace Atomic Write Unit Power Fail
    uint16_t nacwu;    // Namespace Atomic Compare & Write Unit
    uint16_t nabsn;    // Namespace Atomic Boundary Size Normal
    uint16_t nabo;     // Namespace Atomic Boundary Offset
    uint16_t nabspf;   // Namespace Atomic Boundary Size Power Fail
    uint16_t noiob;    // NVM Optimal IO Boundary
    uint8_t  nvmcap[16];// NVM Capacity (bytes)
    uint16_t npwg;     // Namespace Preferred Write Granularity
    uint16_t npwa;     // Namespace Preferred Write Alignment
    uint16_t npdg;     // Namespace Preferred Deallocate Granularity
    uint16_t npda;     // Namespace Preferred Deallocate Alignment
    uint16_t nows;     // Namespace Optimal Write Size
    uint8_t  reserved0[2];
    uint16_t anagrpid; // ANA Group Identifier
    uint8_t  reserved1[3];
    uint8_t  nsattr;   // Namespace Attributes
    uint8_t  nvmsetid; // NVM Set Identifier
    uint8_t  endgid;   // Endurance Group Identifier
    uint8_t  reserved2[42];
    uint8_t  eui64[8]; // Extended Unique Identifier

    // LBA Format Data Structures
    struct {
        uint16_t ms;   // Metadata Size
        uint8_t  lbads;// LBA Data Size (2^(lbads) bytes)
        uint8_t  rp;   // Relative Performance
    } lbaf[16];

    uint8_t  reserved3[192];
    uint8_t  vs[3712];  // Vendor Specific region
} nvme_identify_ns_t;
#pragma pack(pop)

typedef struct {
    uint64_t cap;         // 0x0000: Controller Capabilities
    uint32_t version;     // 0x0008: Version
    uint32_t intms;       // 0x000C: Interrupt Mask Set
    uint32_t intmc;       // 0x0010: Interrupt Mask Clear
    uint32_t cc;          // 0x0014: Controller Configuration
    uint32_t reserved1;   // 0x0018: Reserved
    uint32_t csts;        // 0x001C: Controller Status
    uint32_t nssr;        // 0x0020: NVM Subsystem Reset
    uint32_t aqa;         // 0x0024: Admin Queue Attributes
    uint64_t asq;         // 0x0028: Admin Submission Queue Base Address
    uint64_t acq;         // 0x0030: Admin Completion Queue Base Address
    uint32_t cmbloc;      // 0x0038: Controller Memory Buffer Location
    uint32_t cmbsz;       // 0x003C: Controller Memory Buffer Size
    uint32_t bpinfo;      // 0x0040: Boot Partition Information
    uint32_t bprsel;      // 0x0044: Boot Partition Read Select
    uint64_t bpmbl;       // 0x0048: Boot Partition Memory Buffer Location
    uint64_t reserved2[15]; // 0x0050–0x007F: Reserved
    uint64_t pmrcap;      // 0x0080: Persistent Memory Region Capabilities
    uint64_t pmrctl;      // 0x0088: Persistent Memory Region Control
    uint64_t pmrsts;      // 0x0090: Persistent Memory Region Status
    uint64_t pmrebs;      // 0x0098: Persistent Memory Region Elasticity Buffer Size
    uint64_t pmrmscl;     // 0x00A0: Persistent Memory Region Memory Space Control Lower
    uint64_t pmrmscu;     // 0x00A8: Persistent Memory Region Memory Space Control Upper
    uint64_t reserved3[22]; // 0x00B0–0x00FF: Reserved
    uint32_t sq0tls;      // 0x0100: Submission Queue 0 Tail Doorbell
    uint32_t cq0hls;      // 0x0104: Completion Queue 0 Head Doorbell
    // Doorbells (variable stride based on CAP.DSTRD, typically follows CQ0HLS)
} volatile nvme_controller_regs_t;

typedef struct {
    uint8_t opc;         // Opcode (0x05 for Create I/O Completion Queue)
    uint8_t flags;       // Flags
    uint16_t cid;        // Command ID
    uint32_t nsid;       // Namespace ID (unused for this command)
    uint64_t rsvd2;      // Reserved
    uint64_t prp1;       // PRP Entry 1 (physical address of CQ buffer)
    uint64_t prp2;       // PRP Entry 2 (if needed)
    uint16_t cqid;       // Completion Queue ID
    uint16_t qsize;      // Queue size (entries - 1)
    uint16_t cq_flags;   // CQ flags (e.g., IRQ enable)
    uint16_t irq_vector; // IRQ vector (optional, if using interrupts)
} __attribute__((packed)) nvme_command_t;

typedef struct {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} __attribute__((packed)) nvme_completion_t;

typedef struct {
    uint64_t submission_queue;
    uint64_t completion_queue;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t phase;
} __attribute__((packed)) nvme_queue_t;

typedef struct {
    uint8_t opc;               // Opcode (command type)
    uint8_t fuse;              // Fused operation
    uint16_t cid;              // Command identifier
    uint32_t nsid;             // Namespace identifier
    uint64_t reserved1;        // Reserved
    uint64_t mptr;             // Metadata pointer (optional)
    uint64_t prp1;             // Physical Region Page 1 (data pointer)
    uint64_t prp2;             // Physical Region Page 2 (data pointer)
    uint32_t cdw10;            // Command-specific fields
    uint32_t cdw11;            // Command-specific fields
    uint32_t cdw12;            // Command-specific fields
    uint32_t cdw13;            // Command-specific fields
    uint32_t cdw14;            // Command-specific fields
    uint32_t cdw15;            // Command-specific fields
} __attribute__((packed)) nvme_submission_queue_entry_t;

typedef struct {
    uint16_t phase_tag : 1;  // Bit 0: Phase Tag (P)
    uint16_t status_code : 7; // Bits 1-7: Status Code (SC)
    uint16_t status_code_type : 3; // Bits 8-10: Status Code Type (SCT)
    uint16_t reserved : 3;   // Bits 11-13: Reserved
    uint16_t more : 1;       // Bit 14: More (M)
    uint16_t do_not_retry : 1; // Bit 15: Do Not Retry (DNR)
} __attribute__((packed)) nvme_status_t;

typedef struct {
    uint32_t cmd_specific;
	uint32_t reserved;
	uint16_t sqhd;             // Submission Queue Head Pointer
    uint16_t sqid;             // Submission Queue Identifier
	uint16_t cid;              // Command Identifier
    nvme_status_t status;           // Status field, includes Phase Tag (P)
} __attribute__((packed)) nvme_completion_queue_entry_t;


typedef struct {
	volatile nvme_controller_regs_t* registers;
	uint32_t nsid;
	uint64_t mmioSize;
	uintptr_t mmioAddress;
	uint16_t maxQueueEntries;
	bool contiguousQueuesRequired;
	uint8_t minPageSize, maxPageSize;
	uint8_t cmdSetSupported;
	uint8_t doorbellStride;
	uint32_t defaultTimeout;
	nvme_submission_queue_entry_t* admSubQueue;
	nvme_completion_queue_entry_t* admCompQueue;
	nvme_submission_queue_entry_t* cmdSubQueue;
	volatile nvme_completion_queue_entry_t* cmdCompQueue;
    uint16_t admSubQueueTailIndex;  // Tail index for the command submission queue
    uint16_t cmdSubQueueTailIndex;  // Tail index for the command submission queue
	uint16_t admCompQueueHeadIndex;
	uint16_t cmdCompQueueHeadIndex;
	pci_device_t* nvmePCIDevice;
	uint64_t acq_depth;
 	uint8_t expectedPhaseTag;        // Phase tag for Completion Queue wrap-around
	uint64_t admCompCurrentPhases;
	uint64_t cmdCompCurrentPhases;
	uint16_t queueDepth;
	uint16_t adminCID;
	uint16_t cmdCID;
	uint32_t blockSize;
 } nvme_controller_t;

#include <stdint.h>

typedef struct {
    uint64_t namespaceSize;         // 0x00: Namespace Size (in logical blocks)
    uint64_t namespaceCapacity;         // 0x08: Namespace Capacity (in logical blocks)
    uint64_t namespaceUtilization;         // 0x10: Namespace Utilization (in logical blocks)
    uint8_t namespaceFeatures;        // 0x18: Namespace Features
    uint8_t numOfLBAFormats;         // 0x19: Number of LBA Formats
    uint8_t formattedLBASize;         // 0x1A: Formatted LBA Size
    uint8_t metadataCaps;            // 0x1B: Metadata Capabilities
    uint8_t dataProtCaps;           // 0x1C: End-to-End Data Protection Capabilities
    uint8_t dataProtTypeSettings;           // 0x1D: End-to-End Data Protection Type Settings
    uint8_t nsMPIONSSharingCaps;          // 0x1E: Namespace Multi-path I/O and Namespace Sharing Capabilities
    uint8_t resCap;        // 0x1F: Reservation Capabilities
    uint8_t formatProgressInd;           // 0x20: Format Progress Indicator
    uint8_t deallocateLBFeatures;        // 0x21: Deallocate Logical Block Features
    uint16_t nsAtomicWriteUnitNormal;        // 0x22: Namespace Atomic Write Unit Normal
    uint16_t nsAtomicWriteUnitPowerFail;       // 0x24: Namespace Atomic Write Unit Power-Fail
    uint16_t nsAtomicCompWriteUnit;        // 0x26: Namespace Atomic Compare & Write Unit
    uint16_t nsAtomicBoundarySizeNormal;        // 0x28: Namespace Atomic Boundary Size Normal
    uint16_t nabspf;       // 0x2A: Namespace Atomic Boundary Size Power-Fail
    uint16_t noiob;        // 0x2C: Namespace Optimal I/O Boundary
    uint8_t nvmcap[16];    // 0x2E: NVM Capacity (in bytes)
    uint8_t reserved1[40]; // 0x3E: Reserved
    uint64_t anagrpid;     // 0x60: ANA Group Identifier
    uint8_t reserved2[88]; // 0x68: Reserved
    uint8_t vs[3712];      // 0x100: Vendor-Specific Data
} nvme_namespace_data_t;

void init_NVME();
void nvme_read_disk();
void nvme_write_disk(nvme_controller_t* controller, uint64_t LBA, size_t length, void* buffer);

#endif // NVME_H

