#ifndef PCI_LOOKUP_H
#define PCI_LOOKUP_H

#include <stdint.h>

typedef struct {
    uint16_t venid;
    char vendor[64];
    uint16_t deviceNo;
    char devname[64];
    uint8_t devclass;
    uint8_t devsubclass;
} pci_device_id_t;


char* pci_get_vendor_by_id(uint16_t vendorId);
char* pci_get_device_by_vendor_device_id(uint16_t vendorId, uint16_t deviceId);

#endif