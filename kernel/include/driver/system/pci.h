#ifndef PCI_H
#define	PCI_H

#include <stdbool.h> /* C doesn't have booleans by default. */
#include <stdint.h>
#include <stddef.h>

#define PCI_CONFIG_ADDRESS 	0xCF8
#define PCI_CONFIG_DATA    	0xCFC
#define PCI_DEVICE_SLOTS 	0x100
#define PCI_BRIDGE_SLOTS 	0x10
#define PCI_FUNCTION_SLOTS 	0x100

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

extern int kPCIDeviceCount, kPCIBridgeCount, kPCIFunctionCount, kPCIBusCount;
extern pci_device_t* kPCIDeviceHeaders;
extern pci_bridge_t* kPCIBridgeHeaders;
extern pci_device_t* kPCIDeviceFunctions;

void init_PCI();

#endif
