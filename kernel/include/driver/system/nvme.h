#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"	

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
#define NVME_REG_SQ_TDBL(queue_id) (0x1000 + (2 * (queue_id)) * DOORBELL_STRIDE)
#define NVME_REG_CQ_HDBL(queue_id) (0x1000 + (2 * (queue_id) + 1) * DOORBELL_STRIDE)

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

typedef struct {
    uint64_t cap;         // 0x0000: Controller Capabilities
    uint32_t version;          // 0x0008: Version
    uint32_t intms;       // 0x000C: Interrupt Mask Set
    uint32_t intmc;       // 0x0010: Interrupt Mask Clear
    uint32_t cc;          // 0x0014: Controller Configuration
    uint32_t reserved1;   // 0x0018: Reserved
    uint32_t csts;        // 0x001C: Controller Status
    uint32_t nssr;        // 0x0020: NVM Subsystem Reset
    uint32_t aqa;         // 0x0024: Admin Queue Attributes
    uint64_t asq;         // 0x0028: Admin Submission Queue Base Address
    uint64_t acq;         // 0x0030: Admin Completion Queue Base Address
    // ... Additional registers as needed
} volatile nvme_controller_regs_t;

typedef struct {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
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
    uint32_t sqhd;             // Submission Queue Head Pointer
    uint32_t sqid;             // Submission Queue Identifier
    uint16_t cid;              // Command Identifier
    uint16_t status;           // Status field, includes Phase Tag (P)
} __attribute__((packed)) nvme_completion_queue_entry_t;


typedef struct {
	volatile nvme_controller_regs_t* registers;
	uint64_t mmioSize;
	uintptr_t mmioAddress;
	uint16_t maxQueueEntries;
	bool contiguousQueuesRequired;
	uint8_t minPageSize, maxPageSize;
	uint8_t cmdSetSupported;
	uint8_t doorbellStride;
	uint32_t defaultTimeout;
	nvme_submission_queue_entry_t* adminSubQueue;
	nvme_completion_queue_entry_t* adminCompQueue;
	nvme_submission_queue_entry_t* cmdSubQueue;
	nvme_submission_queue_entry_t* cmdCompQueue;
    uint16_t admSubQueueTailIndex;  // Tail index for the command submission queue
    uint16_t cmdSubQueueTailIndex;  // Tail index for the command submission queue
	uint16_t admCompQueueHeadIndex;
	uint16_t cmdCompQueueHeadIndex;
	pci_device_t* nvmePCIDevice;
	uint64_t acq_depth;
 	uint8_t expectedPhaseTag;        // Phase tag for Completion Queue wrap-around
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
void nvme_send_command(nvme_command_t *cmd);
int nvme_read(uint32_t nsid, uint64_t lba, uint16_t nblocks, void *buffer);

#endif // NVME_H
