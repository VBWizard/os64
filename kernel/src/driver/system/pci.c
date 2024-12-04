#include "driver/system/pci.h"
#include "serial_logging.h"
#include "io.h"
#include "memcpy.h"
#include "kmalloc.h"
#include "strcpy.h"
#include "driver/system/pci_c_header.h"
#include "memset.h"
#include "acpi.h"

uint8_t kPCIDeviceCount, kPCIBridgeCount, kPCIFunctionCount, kPCIBusCount;
pci_bridge_t* kPCIBridgeHeaders;
pci_device_t* kPCIDeviceHeaders;
pci_device_t* kPCIDeviceFunctions;
uintptr_t kPCIBaseAddress=0;

#define PCI_CONFIG_SPACE_LIMIT 0x100 // Standard configuration space limit (256 bytes)
#define PCI_EXTENDED_CONFIG_SPACE_LIMIT 0x1000 // Extended configuration space limit (4 KB)

// Assuming we're using the standard 256-byte config space for now
uint32_t readPCIRegister(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    // Check for alignment (offset must be a multiple of 4)
    if (offset % 4 != 0) {
        printd(DEBUG_PCI, "Invalid offset: 0x%02x. Must be aligned to 4 bytes.\n", offset);
        return 0xFFFFFFFF; // Indicate error
    }

    // Check that the offset is within bounds
    if (offset >= PCI_CONFIG_SPACE_LIMIT) {
        printd(DEBUG_PCI, "Invalid offset: 0x%02x. Exceeds configuration space limit.\n", offset);
        return 0xFFFFFFFF; // Indicate error
    }

    uint64_t configAddress = kPCIBaseAddress + PCI_MMIO_OFFSET(bus, device, function, offset);
    volatile uint32_t* configSpace = (volatile uint32_t*)configAddress;

    uint32_t value = *configSpace;
    printd(DEBUG_PCI, "PCI Config [Bus %u, Device %u, Function %u, Offset 0x%02x] = 0x%08x\n",
           bus, device, function, offset, value);
    return value;
}

void writePCIRegister(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value) {
    // Check for alignment (offset must be a multiple of 4)
    if (offset % 4 != 0) {
        printd(DEBUG_PCI, "Invalid offset: 0x%02x. Must be aligned to 4 bytes.\n", offset);
        return;
    }

    // Check that the offset is within bounds
    if (offset >= PCI_CONFIG_SPACE_LIMIT) {
        printd(DEBUG_PCI, "Invalid offset: 0x%02x. Exceeds configuration space limit.\n", offset);
        return;
    }

    uint64_t configAddress = kPCIBaseAddress + PCI_MMIO_OFFSET(bus, device, function, offset);
    volatile uint32_t* configSpace = (volatile uint32_t*)configAddress;

    *configSpace = value;
    printd(DEBUG_PCI, "PCI Config [Bus %u, Device %u, Function %u, Offset 0x%02x] = 0x%08x\n",
           bus, device, function, offset, value);
}


uint32_t pciConfigReadDWord (uint8_t bus, uint8_t slot,
                             uint8_t func, uint8_t offset)
 {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    uint32_t num;
    
    /* create configuration address as per Figure 1 */
    address = ((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xfc) | (0x80000000));
 
    /* write out the address */
    outl (PCI_CONFIG_IO_ADDRESS, address);
    /* read in the data */
    /* (offset & 2) * 8) = 0 will choose the first word of the 32 bits register */
    num = inl (PCI_CONFIG_DATA);
    return num;
 }

bool getDeviceHeader(pci_device_t* node, uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t value;

    for (int cnt=0;cnt<16;cnt++)
    {
        //value=pciConfigReadDWord(bus, slot, func, cnt*4);
		value=readPCIRegister(bus, slot, func, cnt*4);
		if (value==0xFFFFFFFF)
            return false;
        switch(cnt)
        {
            case 0:
                node->vendor=value;
                node->device=value >> 16;
                break;
            case 1:
                node->command=value&0xFFFF;
                node->status=value>>16&0xFFFF;
                break;
            case 2:
                node->class=value >> 24;
                node->subClass=(value & 0x00FF0000) >> 16;
                node->prog=(value & 0x0000FF00) >> 8;
				node->class_dword = value;
                break;
            case 3:
                node->headerType=(value >> 16) & 0x7f;
                node->multiFunction=(value & 0x00800000)==0x00800000;
                node->cacheSize=value&0xFF;
                node->latencyTimer=value>>8;
                break;
            case 4: case 5: case 6: case 7: case 8: case 9:
                node->baseAdd[cnt+1-5]=value;
                break;
            case 11:
                node->subdevice=value >> 16;
                node->subvendor=value & 0xFFFF;
                break;
            case 12:
                node->expROMAddress=value;
                break;
            case 13:
                node->caps = value & 0xFF;
                break;
            case 15:
                node->interrupt_line=value&0xFF;
                node->interrupt_pin=(value >> 8)&0xFF;
                break;
			default:
				break;
        }
    }
    node->busNo=bus;
    node->deviceNo=slot;
    node->funcNo=func;
    return true;
}

bool getBridgeHeader(pci_bridge_t* node, uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t value;

    printd(DEBUG_PCI_DISCOVERY,"Building bridge header for %u:%u:%u\n",bus,slot,func);
    //Get the entire header
    for (int cnt=0;cnt<14;cnt++)
    {
        //value=pciConfigReadDWord(bus, slot, func, cnt*4);
		value=readPCIRegister(bus, slot, func, cnt*4);
        if (value==0xFFFFFFFF)
            return false;
        switch(cnt)
        {
            case 0:
                node->vendor=value;
                node->device=value >> 16;
                break;
            case 1:
                node->command=value&0xFFFF;
                node->status=value>>16&0xFFFF;
                break;
            case 2:
                node->class=value >> 24;
                node->subClass=(value & 0x00FF0000) >> 16;
                node->prog=(value & 0x0000FF00) >> 8;
                break;
            case 3:
                node->headerType=(value >> 16) & 0x7f;
                node->multiFunction=(value & 0x00800000)==0x00800000;
                node->cacheSize=value&0xFF;
                node->latencyTimer=value>>8;
                break;
            case 4: 
                node->baseAdd[0]=value;
                break;
            case 5:
                node->baseAdd[1]=value;
                break;
            case 6:
                node->secLatencyTimer=(value>>24);
                node->subordinateBusNum=(value>>16) & 0xFF;
                node->secondaryBusNum=(value >> 8) & 0xFF;
                node->primaryBusNum=value & 0xFF;
                break;
            case 7:
                node->secStatus=(value>>16);
                node->ioLimit=(value>>8)&0xFF;
                node->ioBase=value&0xFF;
                break;
            case 8:
                node->memoryLimit=value>>16;
                node->memoryBase=value;
                break;
            case 9:
                node->prefMemLimit=value>>16;
                node->prefMemBase=value&0xFFFF;
                break;
            case 10:
                node->prefMemLimit|=value<<16;
                break;
            case 11:
                node->prefMemBase|=value<<16;
                break;
            case 12:
                node->ioLimit|=(value&0xFFFF0000)>>8;
                node->ioBase|=value&0xFFFF<<8;
                break;
            case 13:
                node->caps = value & 0xFF;
                break;
            case 14:
                node->expROMAddress=value;
                break;
            case 15:
                node->interrupt_line=value&0xFF;
                node->interrupt_pin=(value >> 8)&0xFF;
                node->bridgeControl=(value>>16)&0xFFFF;
                break;
			default:
				break;

        }
    }
    node->busNo=bus;
    node->deviceNo=slot;
    node->funcNo=func;
    return true;
}

void addBridge(pci_device_t* node, uint8_t bus, uint8_t device, uint8_t function)
{
    pci_device_t newNode;
    pci_bridge_t bridge;
	memset(&bridge, 0, sizeof(pci_bridge_t));
	bridge.vendor = 0xffff;
    getBridgeHeader(&bridge, bus, device, function);
    if (bridge.vendor==0xFFFF)
        return;
    printd(DEBUG_PCI_DISCOVERY,"\t* Found bridge on %02X:%02X:%02X,Cls#%02X pBus#%02X, sBus#%02X, suBus# %02X,MF=%u\n",bus, device, function, bridge.class, bridge.primaryBusNum, bridge.secondaryBusNum, bridge.subordinateBusNum, bridge.multiFunction);
    printd(DEBUG_PCI_DISCOVERY,"\t  deviceID: %04X, vendorID: %04X, class: %04X, subclass %04X\n", bridge.device, bridge.vendor, bridge.class, bridge.subClass);
    memcpy(&kPCIBridgeHeaders[kPCIBridgeCount++],&bridge,sizeof(pci_bridge_t));
    memcpy(&newNode,node,sizeof(pci_device_t));
}

void addDevice(pci_device_t* node)
{
    printd(DEBUG_PCI_DISCOVERY,"\t\t* Found device #%u on %02X:%02X:0, Ven# %04X Dev# %04X Cls# %02X, Sbcls 0x%04x MF=%u\n",kPCIDeviceCount, node->busNo, node->deviceNo, node->vendor, node->device, node->class, node->subClass, node->multiFunction);
    memcpy(&kPCIDeviceHeaders[kPCIDeviceCount++],node,sizeof(pci_device_t));
}

void addFunction(pci_device_t* node)
{
    printd(DEBUG_PCI_DISCOVERY,"\t\t* Found function #%u on %02X:%02X:0, Ven# %04X Dev# %04X Cls# %02X, Sbcls 0x%04x MF=%u\n",kPCIFunctionCount, node->busNo, node->deviceNo, node->vendor, node->device, node->class, node->subClass, node->multiFunction);
    memcpy(&kPCIDeviceFunctions[kPCIFunctionCount++],node,sizeof(pci_bridge_t));
}

void getDeviceName(uint32_t vendorID, uint32_t deviceID, char* deviceName)
{
    for (int cnt=0; cnt<7000;cnt++)
        if (PciDevTable[cnt].VenId == vendorID && PciDevTable[cnt].DevId==deviceID)
        {
            strncpy(deviceName, PciDevTable[cnt].ChipDesc, 80);
            return;
        }
    strcpy(deviceName,"Not Found");
}

char* getDeviceNameP(pci_device_t* node, char* buffer)
{
    getDeviceName(node->vendor, node->device, buffer);
    return buffer;
}

void getVendorLongName(uint32_t vendorID, char* vendorLongName)
{
    for (unsigned cnt=0; cnt<PCI_VENTABLE_LEN;cnt++)
        if (PciVenTable[cnt].VenId == (uint16_t)vendorID)
        {
            strncpy(vendorLongName, PciVenTable[cnt].VenFull,80);
            return;
        }
    strcpy(vendorLongName,"Not Found");
}

 int pci_get_bridges(pci_bridge_t* bridges)
 {
	int bridge_count=0;

	for (int idx = 0; idx < kPCIBridgeCount; idx++)
	{
		memcpy(&bridges[bridge_count], &kPCIBridgeHeaders[idx], sizeof(pci_bridge_t));
		bridge_count++;
	}
	return bridge_count;
 }

/// @brief Returns a list of pci devices matching the passed class/subclass
/// @param device_class The class of the device you are looking for, pass 0xFF for all classes
/// @param device_subclass The subclass of the device(s) you are looking for, pass 0xFF for all subclasses
/// @param devices An allocated device array large enough to hold 100 devices
/// @return The count of devices added to the device array
int pci_get_device(uint32_t device_class, uint32_t device_subclass, pci_device_t* devices)
{
	int device_count = 0;
	for (int idx = 0; idx < kPCIDeviceCount; idx++)
	{
		if ((device_class == 0xFF || kPCIDeviceHeaders[idx].class == device_class) && 
			(device_subclass == 0xFF || kPCIDeviceHeaders[idx].subClass == device_subclass))
		{
			memcpy(&devices[device_count], &kPCIDeviceHeaders[idx], sizeof(pci_device_t));
			device_count++;
		}
	}
	for (int idx = 0; idx < kPCIDeviceCount; idx++)
	{
		if ((device_class == 0xFF || kPCIDeviceFunctions[idx].class == device_class) && 
			(device_subclass == 0xFF || kPCIDeviceFunctions[idx].subClass == device_subclass))
		{
			memcpy(&devices[device_count], &kPCIDeviceFunctions[idx], sizeof(pci_device_t));
			device_count++;
		}
	}
	return device_count;
}

void pci_print_discovery_results()
{
	char name[255] = {0};
	char vendor[255] = {0};

	printd(DEBUG_PCI, "PCI Discovery\n");
	printd(DEBUG_PCI, "\tBusses\n");
	for (int bridge=0;bridge<kPCIBridgeCount;bridge++)
	{
		getDeviceName(kPCIBridgeHeaders[bridge].vendor, kPCIBridgeHeaders[bridge].device,(char*)&name);
		getVendorLongName(kPCIBridgeHeaders[bridge].vendor, (char*)vendor);
		printd(DEBUG_PCI, "\t\t0x%02x:0x%02x:0x%02x: %s (0x%04x) - %s (0x%04x)\n", kPCIBridgeHeaders[bridge].busNo, kPCIBridgeHeaders[bridge].deviceNo, kPCIBridgeHeaders[bridge].funcNo,
		vendor, kPCIBridgeHeaders[bridge].vendor, &name, kPCIBridgeHeaders[bridge].device);
	}
	printd(DEBUG_PCI, "\tDevices\n");
	for (int dev=0;dev<kPCIDeviceCount;dev++)
	{
		getDeviceName(kPCIDeviceHeaders[dev].vendor, kPCIDeviceHeaders[dev].device,(char*)&name);
		getVendorLongName(kPCIDeviceHeaders[dev].vendor, (char*)vendor);
		printd(DEBUG_PCI, "\t\t0x%02x:0x%02x:0x%02x: %s (0x%04x) - %s (0x%04x), c 0x%02x, sc 0x%02x  (class dword=0x%08x)\n", kPCIDeviceHeaders[dev].busNo, kPCIDeviceHeaders[dev].deviceNo, kPCIDeviceHeaders[dev].funcNo,
		vendor, kPCIDeviceHeaders[dev].vendor, &name, kPCIDeviceHeaders[dev].device, kPCIDeviceHeaders[dev].class, kPCIDeviceHeaders[dev].subClass, kPCIDeviceHeaders[dev].class_dword);
	}
	for (int func=0;func<kPCIFunctionCount;func++)
	{
		getDeviceName(kPCIDeviceFunctions[func].vendor, kPCIDeviceFunctions[func].device,(char*)&name);
		getVendorLongName(kPCIDeviceFunctions[func].vendor, (char*)vendor);
		printd(DEBUG_PCI, "\t\t0x%02x:0x%02x:0x%02x: %s (0x%04x) - %s (0x%04x), c 0x%02x, sc 0x%02x  (class dword=0x%08x)(F)\n", kPCIDeviceFunctions[func].busNo, kPCIDeviceFunctions[func].deviceNo, kPCIDeviceFunctions[func].funcNo,
		vendor, kPCIDeviceFunctions[func].vendor, &name, kPCIDeviceFunctions[func].device, kPCIDeviceFunctions[func].class, kPCIDeviceFunctions[func].subClass, kPCIDeviceFunctions[func].class_dword);
	}

	printd(DEBUG_PCI, "\n");
}

void init_PCI()
 {

	pci_device_t device, funcDevice;
	uint16_t prevDev=0,prevBus=0;
	uint16_t currFunc;

	kPCIDeviceCount=kPCIBridgeCount=kPCIFunctionCount=kPCIBusCount=0;

	kPCIDeviceHeaders = kmalloc(sizeof(pci_device_t) * PCI_DEVICE_SLOTS);
	kPCIBridgeHeaders = kmalloc(sizeof(pci_device_t) * PCI_BRIDGE_SLOTS);
	kPCIDeviceFunctions = kmalloc(sizeof(pci_device_t) * PCI_FUNCTION_SLOTS);

	printd(DEBUG_PCI_DISCOVERY,"Iterating the PCI busses ...\n");
	for (uint16_t currBus=0;currBus<255;currBus++)
	{
		for (uint16_t currSlot=0;currSlot<32;currSlot++)
		{
			//if device found print it
			currFunc=0;
			prevDev=0;
			if (getDeviceHeader(&device, currBus, currSlot, currFunc)==true)
			{
				if (prevBus!=currBus)
				{
					prevBus=currBus;
					kPCIBusCount++;
				}
				if (device.class==0x06)
				{
					addBridge(&device, currBus, currSlot, currFunc);
				}
				else
					addDevice(&device);
				//if found device is multi-function, iterate all of the functions
				for (currFunc=0;currFunc<8;currFunc++)
				{
					getDeviceHeader(&funcDevice, currBus, currSlot, currFunc);
					if (funcDevice.vendor!=0xFFFF && funcDevice.device != device.device && funcDevice.device != prevDev)
					{
						prevDev=funcDevice.device;
						if (funcDevice.class == 0x06)
							addBridge(&funcDevice, currBus, currSlot, currFunc);
						else
							addFunction(&funcDevice);
					}
				}
			}
		}
	}
	
 }

pci_config_space_t *pci_get_config_space(uint8_t bus, uint8_t device, uint8_t function) {
    // Calculate the address of the configuration space for the given B:D:F
    uintptr_t configSpaceAddress = PCI_CONFIG_ADDRESS(bus, device, function, 0x00);
    
    // Cast the address to a pointer to pci_config_space_t
    pci_config_space_t *configSpace = (pci_config_space_t *)configSpaceAddress;

    return configSpace;
}
