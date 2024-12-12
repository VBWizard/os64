#include "pci_lookup.h"
#include <stddef.h>

pci_device_id_t *kPCIIdsData;
uint32_t kPCIIdsCount;

char* pci_get_vendor_by_id(uint16_t vendorId)
{
	for (uint32_t idx=0;idx<kPCIIdsCount;idx++)
		if (kPCIIdsData[idx].venid == vendorId)
			return kPCIIdsData[idx].vendor;
	return "Not identified";
}

char* pci_get_device_by_vendor_device_id(uint16_t vendorId, uint16_t deviceId)
{
	for (uint32_t idx=0;idx<kPCIIdsCount;idx++)
		if (kPCIIdsData[idx].venid == vendorId && kPCIIdsData[idx].deviceNo == deviceId)
			return kPCIIdsData[idx].devname;
	return "Not identified";
}