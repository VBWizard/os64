#include "driver/system/pci.h"
#include "serial_logging.h"
#include "io.h"
#include "memcpy.h"
#include "kmalloc.h"
#include "strcpy.h"
#include "driver/system/pci_c_header.h"

int kPCIDeviceCount, kPCIBridgeCount, kPCIFunctionCount, kPCIBusCount;
pci_bridge_t* kPCIBridgeHeaders;
pci_device_t* kPCIDeviceHeaders;
pci_device_t* kPCIDeviceFunctions;

 uint32_t pciConfigReadDWord (uint8_t bus, uint8_t slot,
                             uint8_t func, uint8_t offset)
 {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    uint32_t num;
    
    /* create configuration address as per Figure 1 */
    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));
 
    /* write out the address */
    outl (PCI_CONFIG_ADDRESS, address);
    /* read in the data */
    /* (offset & 2) * 8) = 0 will choose the first word of the 32 bits register */
    num = inl (PCI_CONFIG_DATA);
    return num;
 }

bool getDeviceHeader(pci_device_t* node, uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t value;

    //printd(DEBUG_PCI_DISCOVERY,"Building device header for %u:%u:%u\n",bus,slot,func);
    //Get the entire header
    for (int cnt=0;cnt<16;cnt++)
    {
        value=pciConfigReadDWord(bus, slot, func, cnt*4);
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
        value=pciConfigReadDWord(bus, slot, func, cnt*4);
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
    //int lastSubBridgeNum=0;
    getBridgeHeader(&bridge, bus, device, function);
    if (bridge.vendor==0xFFFF)
        return;
    //         bridge.busNo=bus;bridge.deviceNo=device;bridge.funcNo=function;
    printd(DEBUG_PCI_DISCOVERY,"\t\t\tFound bridge on %02X:%02X:%02X,Cls#%02X pBus#%02X, sBus#%02X, suBus# %02X,MF=%u\n",bus, device, function, bridge.class, bridge.primaryBusNum, bridge.secondaryBusNum, bridge.subordinateBusNum, bridge.multiFunction);
    printd(DEBUG_PCI_DISCOVERY,"\t\t\tdeviceID: %04X, vendorID: %04X, class: %04X, subclass %04X:%04X\n", bridge.device, bridge.vendor, bridge.class, bridge.subClass);
    memcpy(&kPCIBridgeHeaders[kPCIBridgeCount++],&bridge,sizeof(pci_bridge_t));
    memcpy(&newNode,node,sizeof(pci_device_t));
    //printd(DEBUG_PCI_DISCOVERY,"\t\t\tbridge entry created\n");
}

void addDevice(pci_device_t* node)
{
    printd(DEBUG_PCI_DISCOVERY,"\tFound device #%u on %02X:%02X:0, Ven# %04X Dev# %04X Cls# %02X MF=%u\n",kPCIDeviceCount, node->busNo, node->deviceNo, node->vendor, node->device, node->class, node->multiFunction);
    memcpy(&kPCIDeviceHeaders[kPCIDeviceCount++],node,sizeof(pci_device_t));
    //printd(DEBUG_PCI_DISCOVERY,"\tdevice entry created\n");
}

void addFunction(pci_device_t* node)
{
    printd(DEBUG_PCI_DISCOVERY,"\t\t\t\tFound function: deviceID: %04X, vendorID: %04X, class: %04X, subclass %04X:%04X\n", node->device, node->vendor, node->class, node->subClass);
    memcpy(&kPCIDeviceFunctions[kPCIFunctionCount++],node,sizeof(pci_bridge_t));
    //printd(DEBUG_PCI_DISCOVERY,"\t\t\t\tFunction entry created\n");
}

void getDeviceName(uint32_t vendorID, uint32_t deviceID, char* deviceName)
{
    for (int cnt=0; cnt<7000;cnt++)
        if (PciDevTable[cnt].VenId == vendorID && PciDevTable[cnt].DevId==deviceID)
        {
            strcpy(deviceName, PciDevTable[cnt].ChipDesc);
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
            strcpy(vendorLongName, PciVenTable[cnt].VenFull);
            return;
        }
    strcpy(vendorLongName,"Not Found");
}

void init_PCI()
 {

	pci_device_t device, funcDevice;
	uint16_t prevDev=0,prevBus=0;
	uint16_t currBus, currSlot, currFunc;

	kPCIDeviceCount=kPCIBridgeCount=kPCIFunctionCount=kPCIBusCount=0;

	kPCIDeviceHeaders = kmalloc(sizeof(pci_device_t) * PCI_DEVICE_SLOTS);
	kPCIBridgeHeaders = kmalloc(sizeof(pci_device_t) * PCI_BRIDGE_SLOTS);;
	kPCIDeviceFunctions = kmalloc(sizeof(pci_device_t) * PCI_FUNCTION_SLOTS);;

	printd(DEBUG_PCI_DISCOVERY,"Iterating the PCI busses ...\n");
	for (currBus=0;currBus<50;currBus++)
	{
		for (currSlot=0;currSlot<32;currSlot++)
		{
		//if device found print it
		currFunc=0;
		prevDev=0;
		//printd(DEBUG_PCI_DISCOVERY,"Current bus=%u, slot=%u\n",currBus,currSlot);
		if (getDeviceHeader(&device, currBus, currSlot, currFunc)==true)
		{
			if (prevBus!=currBus)
			{
				prevBus=currBus;
				kPCIBusCount++;
			}
			//printPCIHeader(&device);
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
				if (funcDevice.vendor!=0xFFFF)
					if (funcDevice.device != device.device && funcDevice.device != prevDev)
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
		printd(DEBUG_PCI, "\t\t0x%02x:0x%02x:0x%02x: %s (0x%04x) - %s (0x%04x)\n", kPCIDeviceHeaders[dev].busNo, kPCIDeviceHeaders[dev].deviceNo, kPCIDeviceHeaders[dev].funcNo,
		vendor, kPCIDeviceHeaders[dev].vendor, &name, kPCIDeviceHeaders[dev].device);
	}
	for (int func=0;func<kPCIFunctionCount;func++)
	{
		getDeviceName(kPCIDeviceFunctions[func].vendor, kPCIDeviceFunctions[func].device,(char*)&name);
		printd(DEBUG_PCI, "\t\t0x%02x:0x%02x:0x%02x: %s (0x%04x) - %s (0x%04x) (F)\n", kPCIDeviceFunctions[func].busNo, kPCIDeviceFunctions[func].deviceNo, kPCIDeviceFunctions[func].funcNo,
		vendor, kPCIDeviceFunctions[func].vendor, &name, kPCIDeviceFunctions[func].device);
	}

	printd(DEBUG_PCI, "\n");



 }