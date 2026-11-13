#include "driver/system/pci.h"
#include "serial_logging.h"
#include "io.h"
#include "memcpy.h"
#include "kmalloc.h"
#include "strcpy.h"
#include "pci_lookup.h"
#include "memset.h"
#include "acpi.h"
#include "pci_lookup.h"
#include "paging.h"
#include "panic.h"
#include "BasicRenderer.h"   // printf — a table overflow is said on the glass

extern __uint128_t kDebugLevel;

uint16_t kPCIDeviceCount, kPCIBridgeCount, kPCIFunctionCount, kPCIBusCount;
// Entries a full table could not hold, reported once after the scan. A real
// machine's class-0x06 population alone (host bridges, data-fabric functions,
// GPP bridges) runs to dozens, and a table that writes past its end corrupts
// the heap silently, so every insert is bounded and the loss is said aloud.
static uint32_t sPCIDropped;
pci_bridge_t* kPCIBridgeHeaders;
pci_device_t* kPCIDeviceHeaders;
pci_device_t* kPCIDeviceFunctions;
uintptr_t kPCIBaseAddress=0;
uint8_t kPCIBusFirst, kPCIBusLast;
uint16_t bussesToScan[100];
uint16_t bussesToScanCount;

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
	printd(DEBUG_PCI | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "PCI COnfig: Reading address 0x%016lx\n", configAddress);
    volatile uint32_t* configSpace = (volatile uint32_t*)configAddress;

    uint32_t value = *configSpace;
    printd(DEBUG_PCI | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "PCI Config [Bus %u, Device %u, Function %u, Offset 0x%02x] = 0x%08x\n",
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
    printd(DEBUG_PCI | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "PCI Config [Bus %u, Device %u, Function %u, Offset 0x%02x] = 0x%08x\n",
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

bool getDeviceHeader(pci_device_t* node, uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t value;

    for (int cnt = 0; cnt < 16; cnt++) {
        value = readPCIRegister(bus, slot, func, cnt * 4);
        if (value == 0xFFFFFFFF)
            return false;

        switch (cnt) {
            case 0: // Vendor and Device ID
                node->vendor = value & 0xFFFF;
                node->device = value >> 16;
                break;
            case 1: // Command and Status
                node->command = value & 0xFFFF;
                node->status = (value >> 16) & 0xFFFF;
                break;
            case 2: // Class, Subclass, ProgIF, Revision
                node->class = value >> 24;
                node->subClass = (value >> 16) & 0xFF;
                node->prog = (value >> 8) & 0xFF;
                node->revisionID = value & 0xFF;   // silicon stepping — /sys reports it
                node->class_dword = value;
                break;
            case 3: // Header Type and Multifunction
                node->headerType = (value >> 16) & 0x7F;
                node->multiFunction = (value & 0x00800000) != 0;
                node->cacheSize = value & 0xFF;
                node->latencyTimer = (value >> 8) & 0xFF;
                break;
            case 4: case 5: case 6: case 7: case 8: case 9: // Base Address Registers
                node->baseAdd[cnt - 4] = value;
                break;
            case 11: // Subsystem Vendor and Device
                node->subvendor = value & 0xFFFF;
                node->subdevice = value >> 16;
                break;
            case 12: // Expansion ROM Base Address
                node->expROMAddress = value;
                break;
            case 13: // Capabilities Pointer
                node->caps = value & 0xFF;
                break;
            case 15: // Interrupts
                node->interrupt_line = value & 0xFF;
                node->interrupt_pin = (value >> 8) & 0xFF;
                break;
            default:
                break;
        }
    }

    node->busNo = bus;
    node->deviceNo = slot;
    node->funcNo = func;

    // Validate if the device is valid
    if ((node->vendor != 0xFFFF && node->vendor !=0x0 ) && (node->device != 0xFFFF && node->device != 0x0) && (node->class != 0xFF && node->class != 0x0))
        return true;
    else
        return false;
}

bool getBridgeHeader(pci_bridge_t* node, uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t value;

    printd(DEBUG_PCI_DISCOVERY | DEBUG_DETAILED,"Building bridge header for %u:%u:%u\n",bus,slot,func);
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
                node->revisionID=value & 0xFF;   // same fill as the device header
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
    printd(DEBUG_PCI_DISCOVERY | DEBUG_DETAILED,"\t* Found bridge on %02X:%02X:%02X,Cls#%02X pBus#%02X, sBus#%02X, suBus# %02X,MF=%u\n",bus, device, function, bridge.class, bridge.primaryBusNum, bridge.secondaryBusNum, bridge.subordinateBusNum, bridge.multiFunction);
    printd(DEBUG_PCI_DISCOVERY | DEBUG_DETAILED,"\t  deviceID: %04X, vendorID: %04X, class: %04X, subclass %04X\n", bridge.device, bridge.vendor, bridge.class, bridge.subClass);
	if (bussesToScanCount < sizeof(bussesToScan) / sizeof(bussesToScan[0]))
		bussesToScan[bussesToScanCount++]=bridge.secondaryBusNum;
	if (kPCIBridgeCount >= PCI_BRIDGE_SLOTS)
	{
		sPCIDropped++;
		return;
	}
    memcpy(&kPCIBridgeHeaders[kPCIBridgeCount++],&bridge,sizeof(pci_bridge_t));
    memcpy(&newNode,node,sizeof(pci_device_t));
}

void addDevice(pci_device_t* node)
{
    printd(DEBUG_PCI_DISCOVERY | DEBUG_DETAILED,"\t\t* Found device #%u on %02X:%02X:0, Ven# %04X Dev# %04X Cls# %02X, Sbcls 0x%04x MF=%u\n",kPCIDeviceCount, node->busNo, node->deviceNo, node->vendor, node->device, node->class, node->subClass, node->multiFunction);
    if (kPCIDeviceCount >= PCI_DEVICE_SLOTS)
    {
        sPCIDropped++;
        return;
    }
    memcpy(&kPCIDeviceHeaders[kPCIDeviceCount++],node,sizeof(pci_device_t));
}

void addFunction(pci_device_t* node)
{
    printd(DEBUG_PCI_DISCOVERY | DEBUG_DETAILED,"\t\t* Found function #%u on %02X:%02X:0, Ven# %04X Dev# %04X Cls# %02X, Sbcls 0x%04x MF=%u\n",kPCIFunctionCount, node->busNo, node->deviceNo, node->vendor, node->device, node->class, node->subClass, node->multiFunction);
    // sizeof(pci_device_t), not sizeof(pci_bridge_t): node and the array slot
    // are both pci_device_t. The bridge struct is packed and a different size,
    // so the old copy truncated the tail of every saved function header.
    if (kPCIFunctionCount >= PCI_FUNCTION_SLOTS)
    {
        sPCIDropped++;
        return;
    }
    memcpy(&kPCIDeviceFunctions[kPCIFunctionCount++],node,sizeof(pci_device_t));
}

void getDeviceName(uint32_t vendorID, uint32_t deviceID, char* deviceName)
{
		strncpy(deviceName, pci_get_device_by_vendor_device_id(vendorID, deviceID), 80);
}

char* getDeviceNameP(pci_device_t* node, char* buffer)
{
    getDeviceName(node->vendor, node->device, buffer);
    return buffer;
}

void getVendorLongName(uint32_t vendorId, char* vendorLongName)
{
		strncpy(vendorLongName, pci_get_vendor_by_id(vendorId),80);
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
	// kPCIFunctionCount, not kPCIDeviceCount: this loop walks the FUNCTIONS
	// array, and the two counts are unrelated. The old bound under-scanned
	// when functions outnumbered devices and read stale slots when devices
	// outnumbered functions (every allocation is zeroed, so the stale slots
	// were at least all-zero — matching only a 0xFF/0xFF wildcard probe).
	for (int idx = 0; idx < kPCIFunctionCount; idx++)
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
	printd(DEBUG_PCI, "\tBusses (count=%u)\n", kPCIBridgeCount);
	for (int bridge=0;bridge<kPCIBridgeCount;bridge++)
	{
		getDeviceName(kPCIBridgeHeaders[bridge].vendor, kPCIBridgeHeaders[bridge].device,(char*)&name);
		getVendorLongName(kPCIBridgeHeaders[bridge].vendor, (char*)vendor);
		if (kDebugLevel & DEBUG_DETAILED)
			printd(DEBUG_PCI, "\t\t%02x:%02x:%02x:%s(0x%04x)-%s(0x%04x)\n", kPCIBridgeHeaders[bridge].busNo, kPCIBridgeHeaders[bridge].deviceNo, kPCIBridgeHeaders[bridge].funcNo,
			vendor, kPCIBridgeHeaders[bridge].vendor, &name, kPCIBridgeHeaders[bridge].device);
		else
			printd(DEBUG_PCI, "\t\t%02x:%02x:%02x:%s-%s\n", kPCIBridgeHeaders[bridge].busNo, kPCIBridgeHeaders[bridge].deviceNo, kPCIBridgeHeaders[bridge].funcNo,
			vendor, &name);
	}
	printd(DEBUG_PCI, "\tDevices (count=%u)\n",kPCIDeviceCount);
	for (int dev=0;dev<kPCIDeviceCount;dev++)
	{
		getDeviceName(kPCIDeviceHeaders[dev].vendor, kPCIDeviceHeaders[dev].device,(char*)&name);
		getVendorLongName(kPCIDeviceHeaders[dev].vendor, (char*)vendor);
		if (kDebugLevel & DEBUG_DETAILED)
			printd(DEBUG_PCI, "\t\t0x%02x:0x%02x:0x%02x:%s(0x%04x)-%s(0x%04x),c=%u,sc=%u\n", kPCIDeviceHeaders[dev].busNo, kPCIDeviceHeaders[dev].deviceNo, kPCIDeviceHeaders[dev].funcNo,
			vendor, kPCIDeviceHeaders[dev].vendor, &name, kPCIDeviceHeaders[dev].device, kPCIDeviceHeaders[dev].class, kPCIDeviceHeaders[dev].subClass);
		else
			printd(DEBUG_PCI, "\t\t0x%02x:0x%02x:0x%02x:%s-%s,c=%u,sc=%u\n", kPCIDeviceHeaders[dev].busNo, kPCIDeviceHeaders[dev].deviceNo, kPCIDeviceHeaders[dev].funcNo,
			vendor, &name, kPCIDeviceHeaders[dev].class, kPCIDeviceHeaders[dev].subClass);
	}

	printd(DEBUG_PCI, "\tFunctions (count=%u)\n",kPCIFunctionCount);
	for (int func=0;func<kPCIFunctionCount;func++)
	{
		getDeviceName(kPCIDeviceFunctions[func].vendor, kPCIDeviceFunctions[func].device,(char*)&name);
		getVendorLongName(kPCIDeviceFunctions[func].vendor, (char*)vendor);
		printd(DEBUG_PCI, "\t\t0x%02x:0x%02x:0x%02x:%s(0x%04x)-%s(0x%04x),c=%u,sc=%u(F)\n", kPCIDeviceFunctions[func].busNo, kPCIDeviceFunctions[func].deviceNo, kPCIDeviceFunctions[func].funcNo,
		vendor, kPCIDeviceFunctions[func].vendor, &name, kPCIDeviceFunctions[func].device, kPCIDeviceFunctions[func].class, kPCIDeviceFunctions[func].subClass);
	}

	printd(DEBUG_PCI, "\n");
}

static void pci_caps_capture(void);

void init_PCI()
 {

	pci_device_t device, funcDevice;
	uint16_t prevBus=0;
	uint16_t currFunc;
	__uint128_t saved = kDebugLevel;
	kDebugLevel &= ~(DEBUG_PCI_DISCOVERY);
	kDebugLevel &= ~(DEBUG_PCI);
	kPCIDeviceCount=kPCIBridgeCount=kPCIFunctionCount=kPCIBusCount=0;

	kPCIDeviceHeaders = kmalloc(sizeof(pci_device_t) * PCI_DEVICE_SLOTS);
	kPCIBridgeHeaders = kmalloc(sizeof(pci_bridge_t) * PCI_BRIDGE_SLOTS);
	kPCIDeviceFunctions = kmalloc(sizeof(pci_device_t) * PCI_FUNCTION_SLOTS);

	// readPCIRegister reads configuration space through the MCFG's window,
	// with no port-0xCF8 fallback: os64 targets q35 and modern hardware on
	// purpose. Without the window every config read would land at
	// PCI_MMIO_OFFSET past address zero, so refuse by name instead.
	if (kPCIBaseAddress == 0)
		panic("PCI: the ACPI tables describe no PCI Express configuration space (no MCFG segment 0)\n");

	printd(DEBUG_PCI_DISCOVERY,"Iterating the PCI busses ...\n");
	
	for (uint16_t currBus=kPCIBusFirst;currBus<=kPCIBusLast;currBus++)
	{
		printd(DEBUG_PCI_DISCOVERY | DEBUG_DETAILED, "Scanning bus %u\n", currBus);
		for (uint16_t currSlot=0;currSlot<32;currSlot++)
		{
			//if device found print it
			currFunc=0;
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
				// Functions 1-7 exist only when function 0's header says the
				// device is multifunction (PCI spec, header type bit 7); a
				// single-function device may answer every function number
				// with function 0's registers. Each function that answers is
				// its own device, whatever its ID: two identical controllers
				// in one device (the 3900X's chipset USB at 07:00.1 and
				// 07:00.3) are two controllers, and a missing function in
				// between does not end the list.
				if (device.multiFunction)
				{
					for (currFunc=1;currFunc<8;currFunc++)
					{
						if (!getDeviceHeader(&funcDevice, currBus, currSlot, currFunc))
							continue;
						if (funcDevice.class == 0x06)
							addBridge(&funcDevice, currBus, currSlot, currFunc);
						else
							addFunction(&funcDevice);
					}
				}
			}
		}
	}
	
	kDebugLevel = saved;
	if (sPCIDropped)
	{
		printf("PCI: %u function(s) did not fit the tables and were not recorded\n", sPCIDropped);
		printd(DEBUG_BOOT, "PCI: %u function(s) did not fit the tables and were not recorded\n", sPCIDropped);
	}
	pci_caps_capture();
	pci_print_discovery_results();
 }

pci_config_space_t *pci_get_config_space(uint8_t bus, uint8_t device, uint8_t function) {
    // Calculate the address of the configuration space for the given B:D:F
    uintptr_t configSpaceAddress = PCI_CONFIG_ADDRESS(bus, device, function, 0x00);
    
    // Cast the address to a pointer to pci_config_space_t
    pci_config_space_t *configSpace = (pci_config_space_t *)configSpaceAddress;

    return configSpace;
}

// pci_caps_walk's `read`, over this machine's configuration space.
typedef struct { uint8_t bus, device, function; } pci_caps_bdf_t;

static uint32_t pci_caps_read_config(void *ctx, uint8_t offset)
{
	pci_caps_bdf_t *at = ctx;
	return readPCIRegister(at->bus, at->device, at->function, offset);
}

uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t cap_id)
{
	pci_caps_bdf_t at = { bus, device, function };
	pci_caps_t caps;
	pci_caps_walk(pci_caps_read_config, &at, &caps);
	return pci_caps_find(&caps, cap_id);
}

// One row a recorded function: its capability list as enumeration read it.
typedef struct
{
	uint8_t    bus, device, function;
	pci_caps_t caps;
} pci_caps_row_t;

static pci_caps_row_t *sPCICaps;
static uint32_t        sPCICapsCount;

static void pci_caps_record(uint8_t bus, uint8_t device, uint8_t function)
{
	pci_caps_row_t *row = &sPCICaps[sPCICapsCount++];
	pci_caps_bdf_t at = { bus, device, function };
	row->bus = bus;  row->device = device;  row->function = function;
	pci_caps_walk(pci_caps_read_config, &at, &row->caps);
}

// Read every recorded function's capability list into the table
// pci_caps_of answers from. The lists are read here, while enumeration is
// reading configuration space anyway, so that a later question about a
// device touches no hardware.
static void pci_caps_capture(void)
{
	uint32_t total = (uint32_t)kPCIDeviceCount + kPCIFunctionCount + kPCIBridgeCount;
	if (total == 0)
		return;
	sPCICaps = kmalloc(sizeof(pci_caps_row_t) * total);
	for (int i = 0; i < kPCIDeviceCount; i++)
		pci_caps_record(kPCIDeviceHeaders[i].busNo, kPCIDeviceHeaders[i].deviceNo, kPCIDeviceHeaders[i].funcNo);
	for (int i = 0; i < kPCIFunctionCount; i++)
		pci_caps_record(kPCIDeviceFunctions[i].busNo, kPCIDeviceFunctions[i].deviceNo, kPCIDeviceFunctions[i].funcNo);
	for (int i = 0; i < kPCIBridgeCount; i++)
		pci_caps_record(kPCIBridgeHeaders[i].busNo, kPCIBridgeHeaders[i].deviceNo, kPCIBridgeHeaders[i].funcNo);
}

const pci_caps_t *pci_caps_of(uint8_t bus, uint8_t device, uint8_t function)
{
	for (uint32_t i = 0; i < sPCICapsCount; i++)
		if (sPCICaps[i].bus == bus && sPCICaps[i].device == device && sPCICaps[i].function == function)
			return &sPCICaps[i].caps;
	return NULL;
}

// ── Message-signalled interrupts (pci.h has the contract) ───────────────────

#define MSI_CTRL_ENABLE      (1u << 16)   // Message Control is the capability dword's high half
#define MSI_CTRL_MME_MASK    (7u << 20)   // multiple message enable: 0 = one vector
#define MSI_CTRL_64BIT       (1u << 23)
#define MSIX_CTRL_ENABLE     (1u << 31)
#define MSIX_CTRL_FMASK      (1u << 30)   // function mask: every entry masked while set
#define MSIX_ENTRY_MASKED    1u           // an entry's Vector Control, bit 0
#define MSI_ADDRESS_BASE     0xFEE00000u  // the LAPIC's message window; bits 12..19 name an APIC ID

static void pci_intx_disable(uint8_t bus, uint8_t device, uint8_t function)
{
	// A LIVE read-modify-write of the command word: enumeration's copy may
	// be stale by the time a driver gets here.
	uint32_t live = readPCIRegister(bus, device, function, 4) & 0xFFFF;
	writePCIRegister(bus, device, function, 4, live | PCI_COMMAND_INTX_DISABLE);
}

pci_msi_result_t pci_msi_enable(uint8_t bus, uint8_t device, uint8_t function,
                                uint8_t vector, uint8_t apic_id)
{
	pci_msi_result_t r = { 0 };
	r.cap = pci_find_capability(bus, device, function, PCI_CAP_MSI);
	if (r.cap == 0)
		return r;

	uint32_t ctrl = readPCIRegister(bus, device, function, r.cap);
	r.control_before = (uint16_t)(ctrl >> 16);
	r.is64 = (ctrl & MSI_CTRL_64BIT) != 0;
	uint32_t address = MSI_ADDRESS_BASE | ((uint32_t)apic_id << 12);
	uint8_t data_at = r.cap + (r.is64 ? 12 : 8);

	writePCIRegister(bus, device, function, r.cap + 4, address);
	if (r.is64)
		writePCIRegister(bus, device, function, r.cap + 8, 0);
	writePCIRegister(bus, device, function, data_at, vector);
	writePCIRegister(bus, device, function, r.cap, (ctrl & ~MSI_CTRL_MME_MASK) | MSI_CTRL_ENABLE);
	pci_intx_disable(bus, device, function);

	uint32_t ctrl_back = readPCIRegister(bus, device, function, r.cap);
	r.control_after = (uint16_t)(ctrl_back >> 16);
	r.address = readPCIRegister(bus, device, function, r.cap + 4);
	r.data = readPCIRegister(bus, device, function, data_at) & 0xFFFF;
	r.took = (ctrl_back & MSI_CTRL_ENABLE) != 0 && r.address == address && r.data == vector;
	return r;
}

pci_msix_result_t pci_msix_enable_one(uint8_t bus, uint8_t device, uint8_t function,
                                      const uint32_t bars[6], uint8_t vector, uint8_t apic_id)
{
	pci_msix_result_t r = { 0 };
	r.cap = pci_find_capability(bus, device, function, PCI_CAP_MSIX);
	if (r.cap == 0)
		return r;

	uint32_t ctrl = readPCIRegister(bus, device, function, r.cap);
	uint32_t where = readPCIRegister(bus, device, function, r.cap + 4);
	r.entries = (uint16_t)(((ctrl >> 16) & 0x7FF) + 1);
	r.bar = (uint8_t)(where & 7);
	r.offset = where & ~7u;
	if (r.bar > 5 || (bars[r.bar] & 1)) {
		r.refused = "its table is not in a memory BAR";
		return r;
	}
	uint64_t base = bars[r.bar] & ~0xFull;
	if ((bars[r.bar] & 0x6) == 0x4) {
		if (r.bar == 5) {
			r.refused = "its table's BAR is 64-bit with no upper half";
			return r;
		}
		base |= (uint64_t)bars[r.bar + 1] << 32;
	}
	if (base == 0) {
		r.refused = "its table's BAR is unassigned";
		return r;
	}

	// The table at its upper-half alias. A page the driver has already
	// mapped there (the table sharing a BAR with its registers) is mapped
	// again to the same frame, which changes nothing.
	uint64_t table = base + r.offset;
	uint64_t first = table & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t bytes = (table - first) + (uint64_t)r.entries * 16;
	paging_map_pages((pt_entry_t *)kKernelPML4v, kHHDMOffset + first, first,
	                 (bytes + PAGE_SIZE - 1) / PAGE_SIZE,
	                 PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	volatile uint32_t *entry = (volatile uint32_t *)(kHHDMOffset + table);

	// Enabled with everything masked while the table is written: enabling
	// is what takes the function off INTx, and the mask is what keeps a
	// half-written entry from sending anything.
	uint32_t held = (ctrl & 0xFFFF) | MSIX_CTRL_ENABLE | MSIX_CTRL_FMASK;
	writePCIRegister(bus, device, function, r.cap, held);
	uint32_t address = MSI_ADDRESS_BASE | ((uint32_t)apic_id << 12);
	// Vector Control's other bits are reserved and kept as read.
	for (uint32_t i = 0; i < r.entries; i++)
		entry[i * 4 + 3] |= MSIX_ENTRY_MASKED;
	entry[0] = address;
	entry[1] = 0;
	entry[2] = vector;
	entry[3] &= ~MSIX_ENTRY_MASKED;
	writePCIRegister(bus, device, function, r.cap, held & ~MSIX_CTRL_FMASK);
	pci_intx_disable(bus, device, function);

	uint32_t ctrl_back = readPCIRegister(bus, device, function, r.cap);
	r.control_after = (uint16_t)(ctrl_back >> 16);
	r.address = entry[0];
	r.data = entry[2];
	r.took = (ctrl_back & MSIX_CTRL_ENABLE) != 0 && (ctrl_back & MSIX_CTRL_FMASK) == 0 &&
	         r.address == address && r.data == vector && (entry[3] & MSIX_ENTRY_MASKED) == 0;
	return r;
}
