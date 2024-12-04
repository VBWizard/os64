#ifndef PCI_H
#define	PCI_H

#include <stdbool.h> /* C doesn't have booleans by default. */
#include <stdint.h>
#include <stddef.h>

#define PCI_DEVICE_REMAP_BASE 0xFFFFFFFF00000000

#define PCI_CONFIG_SPACE_LIMIT 0x100 // Standard configuration space limit (256 bytes)
#define PCI_EXTENDED_CONFIG_SPACE_LIMIT 0x1000 // Extended configuration space limit (4 KB)

#define MMIO_LOWER_BOUND 0xA0000000  // Example lower bound for MMIO regions
#define MMIO_UPPER_BOUND 0xF0000000  // Updated upper bound for MMIO regions to better reflect typical MMIO ranges (below reserved system regions)

#define PCI_CONFIG_IO_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    	0xCFC
#define PCI_DEVICE_SLOTS 	0x100
#define PCI_BRIDGE_SLOTS 	0x10
#define PCI_FUNCTION_SLOTS 	0x100

//PCI Classes
#define PCI_CLASS_UNCLASSIFIED               0x00
#define PCI_CLASS_MASS_STORAGE_CONTROLLER    0x01
#define PCI_CLASS_NETWORK_CONTROLLER         0x02
#define PCI_CLASS_DISPLAY_CONTROLLER         0x03
#define PCI_CLASS_MULTIMEDIA_CONTROLLER      0x04
#define PCI_CLASS_MEMORY_CONTROLLER          0x05
#define PCI_CLASS_BRIDGE_DEVICE              0x06
#define PCI_CLASS_SIMPLE_COMMUNICATION       0x07
#define PCI_CLASS_BASE_SYSTEM_PERIPHERAL     0x08
#define PCI_CLASS_INPUT_DEVICE               0x09
#define PCI_CLASS_DOCKING_STATION            0x0A
#define PCI_CLASS_PROCESSOR                  0x0B
#define PCI_CLASS_SERIAL_BUS_CONTROLLER      0x0C
#define PCI_CLASS_WIRELESS_CONTROLLER        0x0D
#define PCI_CLASS_INTELLIGENT_IO_CONTROLLER  0x0E
#define PCI_CLASS_SATELLITE_COMM_CONTROLLER  0x0F
#define PCI_CLASS_ENCRYPTION_DECRYPTION      0x10
#define PCI_CLASS_DATA_ACQUISITION_SIGNAL    0x11
#define PCI_CLASS_PROCESSING_ACCELERATOR     0x12
#define PCI_CLASS_NON_ESSENTIAL_INSTRUMENTATION 0x13
#define PCI_CLASS_CO_PROCESSOR               0x40
#define PCI_CLASS_UNASSIGNED                 0xFF

//PCI Classes + Subclasses
// Unclassified (0x00)
#define PCI_CLASS_SUBCLASS_UNCLASSIFIED_NON_VGA       0x0000 // Non-VGA Unclassified Device
#define PCI_CLASS_SUBCLASS_UNCLASSIFIED_VGA           0x0001 // VGA-Compatible Unclassified Device
// Mass Storage Controller (0x01)
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_SCSI          0x0100 // SCSI Bus Controller
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_IDE           0x0101 // IDE Controller
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_FLOPPY        0x0102 // Floppy Disk Controller
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_IPI           0x0103 // IPI Bus Controller
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_RAID          0x0104 // RAID Controller
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_ATA           0x0105 // ATA Controller with Single DMA
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_SATA          0x0106 // Serial ATA Controller
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_SAS           0x0107 // SAS Controller
#define PCI_CLASS_SUBCLASS_MASS_STORAGE_OTHER         0x0180 // Mass Storage Controller (Other)
// Network Controller (0x02)
#define PCI_CLASS_SUBCLASS_NETWORK_ETHERNET           0x0200 // Ethernet Controller
#define PCI_CLASS_SUBCLASS_NETWORK_TOKEN_RING         0x0201 // Token Ring Controller
#define PCI_CLASS_SUBCLASS_NETWORK_FDDI               0x0202 // FDDI Controller
#define PCI_CLASS_SUBCLASS_NETWORK_ATM                0x0203 // ATM Controller
#define PCI_CLASS_SUBCLASS_NETWORK_ISDN               0x0204 // ISDN Controller
#define PCI_CLASS_SUBCLASS_NETWORK_WORLDFIP           0x0205 // WorldFIP Controller
#define PCI_CLASS_SUBCLASS_NETWORK_PICMG              0x0206 // PICMG Controller
#define PCI_CLASS_SUBCLASS_NETWORK_OTHER              0x0280 // Network Controller (Other)
// Display Controller (0x03)
#define PCI_CLASS_SUBCLASS_DISPLAY_VGA                0x0300 // VGA-Compatible Controller
#define PCI_CLASS_SUBCLASS_DISPLAY_XGA                0x0301 // XGA Controller
#define PCI_CLASS_SUBCLASS_DISPLAY_3D                 0x0302 // 3D Controller
#define PCI_CLASS_SUBCLASS_DISPLAY_OTHER              0x0380 // Display Controller (Other)
// Multimedia Controller (0x04)
#define PCI_CLASS_SUBCLASS_MULTIMEDIA_VIDEO           0x0400 // Video Device
#define PCI_CLASS_SUBCLASS_MULTIMEDIA_AUDIO           0x0401 // Audio Device
#define PCI_CLASS_SUBCLASS_MULTIMEDIA_TELEPHONY       0x0402 // Computer Telephony Device
#define PCI_CLASS_SUBCLASS_MULTIMEDIA_OTHER           0x0480 // Multimedia Controller (Other)
// Memory Controller (0x05)
#define PCI_CLASS_SUBCLASS_MEMORY_RAM                 0x0500 // RAM Controller
#define PCI_CLASS_SUBCLASS_MEMORY_FLASH               0x0501 // Flash Controller
#define PCI_CLASS_SUBCLASS_MEMORY_OTHER               0x0580 // Memory Controller (Other)
// Bridge Device (0x06)
#define PCI_CLASS_SUBCLASS_BRIDGE_HOST                0x0600 // Host Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_ISA                 0x0601 // ISA Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_EISA                0x0602 // EISA Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_MCA                 0x0603 // MicroChannel Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_PCI_TO_PCI          0x0604 // PCI-to-PCI Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_PCMCIA              0x0605 // PCMCIA Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_NUBUS               0x0606 // NuBus Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_CARDBUS             0x0607 // CardBus Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_RACEWAY             0x0608 // RACEway Bridge
#define PCI_CLASS_SUBCLASS_BRIDGE_OTHER               0x0680 // Bridge Device (Other)
// Simple Communication Controller (0x07)
#define PCI_CLASS_SUBCLASS_COMMUNICATION_SERIAL       0x0700 // Serial Controller
#define PCI_CLASS_SUBCLASS_COMMUNICATION_PARALLEL     0x0701 // Parallel Controller
#define PCI_CLASS_SUBCLASS_COMMUNICATION_MULTIPORT    0x0702 // Multiport Serial Controller
#define PCI_CLASS_SUBCLASS_COMMUNICATION_MODEM        0x0703 // Modem
#define PCI_CLASS_SUBCLASS_COMMUNICATION_OTHER        0x0780 // Communication Controller (Other)
// Base System Peripheral (0x08)
#define PCI_CLASS_SUBCLASS_SYSTEM_PIC                 0x0800 // PIC (Programmable Interrupt Controller)
#define PCI_CLASS_SUBCLASS_SYSTEM_DMA                 0x0801 // DMA Controller
#define PCI_CLASS_SUBCLASS_SYSTEM_TIMER               0x0802 // Timer
#define PCI_CLASS_SUBCLASS_SYSTEM_RTC                 0x0803 // RTC (Real Time Clock)
#define PCI_CLASS_SUBCLASS_SYSTEM_PCI_HOTPLUG         0x0804 // PCI Hot-Plug Controller
#define PCI_CLASS_SUBCLASS_SYSTEM_OTHER               0x0880 // System Peripheral (Other)
// Input Device (0x09)
#define PCI_CLASS_SUBCLASS_INPUT_KEYBOARD             0x0900 // Keyboard Controller
#define PCI_CLASS_SUBCLASS_INPUT_PEN                  0x0901 // Pen Controller
#define PCI_CLASS_SUBCLASS_INPUT_MOUSE                0x0902 // Mouse Controller
#define PCI_CLASS_SUBCLASS_INPUT_SCANNER              0x0903 // Scanner Controller
#define PCI_CLASS_SUBCLASS_INPUT_GAMEPORT             0x0904 // Gameport Controller
#define PCI_CLASS_SUBCLASS_INPUT_OTHER                0x0980 // Input Device (Other)
// Docking Station (0x0A)
#define PCI_CLASS_SUBCLASS_DOCKING_GENERIC            0x0A00 // Generic Docking Station
#define PCI_CLASS_SUBCLASS_DOCKING_OTHER              0x0A80 // Docking Station (Other)
// Processor (0x0B)
#define PCI_CLASS_SUBCLASS_PROCESSOR_386              0x0B00 // 386 Processor
#define PCI_CLASS_SUBCLASS_PROCESSOR_486              0x0B01 // 486 Processor
#define PCI_CLASS_SUBCLASS_PROCESSOR_PENTIUM          0x0B02 // Pentium Processor
#define PCI_CLASS_SUBCLASS_PROCESSOR_ALPHA            0x0B10 // Alpha Processor
#define PCI_CLASS_SUBCLASS_PROCESSOR_POWERPC          0x0B20 // PowerPC Processor
#define PCI_CLASS_SUBCLASS_PROCESSOR_MIPS             0x0B30 // MIPS Processor
#define PCI_CLASS_SUBCLASS_PROCESSOR_CO_PROCESSOR     0x0B40 // Co-Processor
// Serial Bus Controller (0x0C)
#define PCI_CLASS_SUBCLASS_SERIAL_FIREWIRE            0x0C00 // FireWire (IEEE 1394) Controller
#define PCI_CLASS_SUBCLASS_SERIAL_ACCESS_BUS          0x0C01 // ACCESS.bus Controller
#define PCI_CLASS_SUBCLASS_SERIAL_SSA                 0x0C02 // SSA Controller
#define PCI_CLASS_SUBCLASS_SERIAL_USB                 0x0C03 // USB Controller
#define PCI_CLASS_SUBCLASS_SERIAL_FIBRE_CHANNEL       0x0C04 // Fibre Channel Controller
#define PCI_CLASS_SUBCLASS_SERIAL_SMBUS               0x0C05 // SMBus Controller
#define PCI_CLASS_SUBCLASS_SERIAL_OTHER               0x0C80 // Serial Bus Controller (Other)
// Wireless Controller (0x0D)
#define PCI_CLASS_SUBCLASS_WIRELESS_IRDA              0x0D00 // IRDA Controller
#define PCI_CLASS_SUBCLASS_WIRELESS_CONSUMER_IR       0x0D01 // Consumer IR Controller
#define PCI_CLASS_SUBCLASS_WIRELESS_RF                0x0D10 // RF Controller
#define PCI_CLASS_SUBCLASS_WIRELESS_BLUETOOTH         0x0D11 // Bluetooth Controller
#define PCI_CLASS_SUBCLASS_WIRELESS_BROADBAND         0x0D12 // Broadband Controller
#define PCI_CLASS_SUBCLASS_WIRELESS_OTHER             0x0D80 // Wireless Controller (Other)
// Intelligent I/O Controller (0x0E)
#define PCI_CLASS_SUBCLASS_INTELLIGENT_IO_I2O         0x0E00 // I2O Controller
// Satellite Communication Controller (0x0F)
#define PCI_CLASS_SUBCLASS_SATELLITE_TV               0x0F00 // TV Controller
#define PCI_CLASS_SUBCLASS_SATELLITE_AUDIO            0x0F01 // Audio Controller
#define PCI_CLASS_SUBCLASS_SATELLITE_VOICE            0x0F03 // Voice Controller
#define PCI_CLASS_SUBCLASS_SATELLITE_DATA             0x0F04 // Data Controller
// Encryption/Decryption Controller (0x10)
#define PCI_CLASS_SUBCLASS_ENCRYPTION_NETWORK         0x1000 // Network Encryption/Decryption
#define PCI_CLASS_SUBCLASS_ENCRYPTION_ENTERTAINMENT   0x1001 // Entertainment Encryption/Decryption
#define PCI_CLASS_SUBCLASS_ENCRYPTION_OTHER           0x1080 // Encryption/Decryption (Other)
// Data Acquisition and Signal Processing Controller (0x11)
#define PCI_CLASS_SUBCLASS_DATA_ACQUISITION_DPIO      0x1100 // Data Acquisition and Processing I/O
#define PCI_CLASS_SUBCLASS_DATA_ACQUISITION_OTHER     0x1180 // Data Acquisition (Other)

#define PCI_MMIO_OFFSET(bus, device, function, offset) \
    ((uint64_t)(bus) << 20 | (uint64_t)(device) << 15 | (uint64_t)(function) << 12 | (offset & ~3))

#define PCI_CONFIG_ADDRESS(bus, device, function, offset) \
    ((uintptr_t)kPCIBaseAddress + ((bus) << 20) + ((device) << 15) + ((function) << 12) + (offset))

typedef struct {
    uint16_t vendorID;               // Offset 0x00: Vendor ID (identifies the manufacturer of the device)
    uint16_t deviceID;               // Offset 0x02: Device ID (identifies the specific device)
    uint16_t command;                // Offset 0x04: Command register (controls the device's capabilities)
    uint16_t status;                 // Offset 0x06: Status register (provides status information about the device)
    uint8_t revisionID;              // Offset 0x08: Revision ID (specifies the revision of the device)
    uint8_t progIF;                  // Offset 0x09: Programming Interface (defines specific device programming model)
    uint8_t subclass;                // Offset 0x0A: Subclass code (specifies the device's function within its class)
    uint8_t classCode;               // Offset 0x0B: Class code (specifies the type of device, e.g., mass storage)
    uint8_t cacheLineSize;           // Offset 0x0C: Cache line size (cache line size in 32-bit words)
    uint8_t latencyTimer;            // Offset 0x0D: Latency timer (specifies latency requirements for the device)
    uint8_t headerType;              // Offset 0x0E: Header type (specifies the layout of the configuration space)
    uint8_t BIST;                    // Offset 0x0F: Built-in Self-Test (BIST) capability/status
    uint32_t BAR[6];                 // Offsets 0x10 - 0x27: Base Address Registers (BARs) for mapping device memory or I/O regions
    uint32_t cardbusCISPtr;          // Offset 0x28: CardBus CIS Pointer (only used for CardBus devices)
    uint16_t subsystemVendorID;      // Offset 0x2C: Subsystem Vendor ID (identifies the manufacturer of the subsystem)
    uint16_t subsystemID;            // Offset 0x2E: Subsystem ID (identifies the specific subsystem)
    uint32_t expansionROMBaseAddr;   // Offset 0x30: Expansion ROM Base Address (memory address for expansion ROM)
    uint8_t capabilitiesPtr;         // Offset 0x34: Capabilities Pointer (points to linked list of capabilities)
    uint8_t reserved1[3];            // Offset 0x35 - 0x37: Reserved for future use
    uint32_t reserved2;              // Offset 0x38: Reserved for future use
    uint8_t interruptLine;           // Offset 0x3C: Interrupt Line (specifies which interrupt line the device is using)
    uint8_t interruptPin;            // Offset 0x3D: Interrupt Pin (specifies which interrupt pin the device uses)
    uint8_t minGrant;                // Offset 0x3E: Minimum Grant (minimum amount of time the device needs for PCI bus access)
    uint8_t maxLatency;              // Offset 0x3F: Maximum Latency (maximum time the device can wait before needing PCI bus access)
} __attribute__((packed)) pci_config_space_t;


typedef struct
{
    uint8_t busNo, deviceNo, funcNo;
    uint8_t headerType;
    bool multiFunction;
    uint16_t vendor, device;       /* Vendor and device ID or PCI_ANY_ID*/
    uint16_t subvendor, subdevice; /* Subsystem ID's or PCI_ANY_ID */
    uint32_t class, subClass;    /* (class,subclass,prog-if) triplet */
    uint8_t prog;
    unsigned long driver_data; /* Data private to the driver */
    uint32_t baseAdd[6];
    uint32_t expROMAddress;
    uint8_t caps, interrupt_line, interrupt_pin,BIST, latencyTimer,cacheSize,progIF,revisionID;
    uint16_t bridgeControl, memoryBase,memoryLimit,status,command,secStatus;
	uint32_t class_dword;
} pci_device_t;

typedef struct
{
    uint8_t busNo, deviceNo, funcNo;
    uint8_t headerType;
    bool multiFunction;
    uint16_t vendor, device;       /* Vendor and device ID or PCI_ANY_ID*/
    uint32_t class, subClass;    /* (class,subclass,prog-if) triplet */
    uint8_t prog;
    uint32_t baseAdd[2],expROMAddress,ioLimit, ioBase;
    uint8_t secondaryBusNum, primaryBusNum, subordinateBusNum;
    uint8_t caps, interrupt_line, interrupt_pin,BIST, latencyTimer,cacheSize,progIF,revisionID,secLatencyTimer;
    uint16_t bridgeControl, memoryBase,memoryLimit,status,command,secStatus;
    uint64_t prefMemLimit, prefMemBase;
} __attribute__((packed)) pci_bridge_t;

extern uint8_t kPCIDeviceCount, kPCIBridgeCount, kPCIFunctionCount, kPCIBusCount;
extern pci_device_t* kPCIDeviceHeaders;
extern pci_bridge_t* kPCIBridgeHeaders;
extern pci_device_t* kPCIDeviceFunctions;
extern uintptr_t kPCIBaseAddress;

char* getDeviceNameP(pci_device_t* node, char* buffer);
uint32_t readPCIRegister(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
void writePCIRegister(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value);
pci_config_space_t *pci_get_config_space(uint8_t bus, uint8_t device, uint8_t function);

void init_PCI();

#endif

