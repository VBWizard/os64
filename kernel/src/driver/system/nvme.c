#include "nvme.h"
#include "kmalloc.h"
#include "paging.h"
#include "BasicRenderer.h"
#include "serial_logging.h"
#include "CONFIG.h"
#include "time.h"
#include "panic.h"
#include "math.h"

extern uint64_t kDebugLevel;
int kNVMEControllerCount = 0;
uint16_t initialCMDValue;
uint32_t bar0InitialValue, bar1InitialValue;

uint64_t nvmeBaseAddressRemap = NVME_ABAR_OVERRIDE_ADDRESS;	

void nvme_print_version(uint32_t versionRegisterValue) {
    // Extract the major version (bits 31:16)
    uint16_t majorVersion = (versionRegisterValue >> 16) & 0xFFFF;

    // Extract the minor version (bits 15:8)
    uint8_t minorVersion = (versionRegisterValue >> 8) & 0xFF;

    // Extract the tertiary version (bits 7:0)
    uint8_t tertiaryVersion = versionRegisterValue & 0xFF;

    // Print the NVMe version
    printf("NVMe Version %d.%d.%d\n", majorVersion, minorVersion, tertiaryVersion);
    printd(DEBUG_NVME, "NVMe Version: %d.%d.%d\n", majorVersion, minorVersion, tertiaryVersion);
}

void nvme_enable_features(uint8_t bus, uint8_t device, uint8_t function)
{
    uint16_t cmd = readPCIRegister(bus, device, function, 0x04);
    cmd |= (1 << 1); // Enable Memory Space
    cmd |= (1 << 2); // Enable Bus Mastering
    writePCIRegister(bus, device, function, 0x04, cmd);
	wait(25);
}

bool nvme_reset_controller(nvme_controller_t* controller) {
volatile uint32_t *cc = (volatile uint32_t *)(controller->mmioAddress + 0x14);
uint32_t currentDelay = 0;
uint32_t intervalDelay = controller->defaultTimeout / 20;
	uint16_t pmcsr = readPCIRegister(controller->nvmePCIDevice->busNo, controller->nvmePCIDevice->deviceNo, controller->nvmePCIDevice->funcNo, PCI_POWER_MGMT_AND_STTS);
	if ((pmcsr & 0x3) != 0x0) {
		panic("Error: Controller is not in D0 power state\n");
	}

  // 1. Clear the EN bit in the CC register to initiate reset
    controller->registers->cc &= ~(0x1);  // Clear the EN bit (bit 0)

    // 2. Poll CSTS register until the RDY bit becomes 0 (controller acknowledges reset)
    while (controller->registers->csts & 0x1) {
        // RDY bit is 1, waiting for it to become 0
    }

	wait(100);

    // 3. Set the EN bit in the CC register to re-enable the controller
    controller->registers->cc |= 0x1;  // Set the EN bit (bit 0)
    __asm__ volatile ("sfence" ::: "memory"); // Serialize writes

    // 4. Poll CSTS register until the RDY bit becomes 1 (controller is ready)
	while (!(controller->registers->csts & 0x1) && currentDelay < controller->defaultTimeout) {
		// Polling with a timeout limit
		wait(intervalDelay);
		currentDelay+=intervalDelay;
	}

	if (currentDelay >= controller->defaultTimeout) {
		printd(DEBUG_NVME, "NVMe: Timeout while waiting for NVMe controller to reset.\n");
		return false;
	}

    printd(DEBUG_NVME, "NVMe: controller reset completed and ready.\n");
	return true;
}

#define ITERATION_DELAY 100
bool nvme_wait_for_ready_after_enabled(nvme_controller_t* controller, uint32_t maxDelayMS) {
    printd(DEBUG_NVME, "Waiting for CSTS ready\n");
	uint32_t maxIterations = maxDelayMS / ITERATION_DELAY;
    uint32_t iterations = 0;

    // Poll the CSTS register until the ready bit is set or our delay has elapsed
    while (((controller->registers->csts & CSTS_READY_MASK) == 0)) {
        if (++iterations > maxIterations)
		{
            // Timeout occurred
            printd(DEBUG_NVME, "NVME: Timeout while waiting for CSTS ready\n");
            return false;  
		}
        wait(ITERATION_DELAY);
    }

    printd(DEBUG_NVME, "NVME: CSTS is ready\n");
    return true;  // CSTS is ready
}

void nvme_initialize_controller(nvme_controller_t* controller) {
    printd(DEBUG_NVME, "Configuring NVMe Controller Configuration (CC) register\n");

    uint32_t cc = 0;

    // Host's page size in bytes
    uint32_t host_page_size = 4096; // Replace with your system's actual page size

    // Calculate MPS value: MPS = log2(host_page_size) - 12
    uint8_t mps = (uint8_t)(log2(host_page_size) - 12);

    // Ensure MPS is within the supported range from CAP.MPSMIN to CAP.MPSMAX
    if (mps < controller->minPageSize || mps > controller->maxPageSize) {
        printd(DEBUG_NVME, "Error: Host page size is not supported by the controller\n");
        panic("Error: Host page size is not supported by the controller\n");
        return; // Handle failure appropriately
    }

    // Set the Memory Page Size (MPS)
    cc |= (mps << 7); // MPS is at bits [10:7] in the CC register

    // Set Command Set Selected (CSS) to NVM Command Set (typically 0)
    cc |= (0x0 << 4); // CSS is at bits [6:4]

    // Set Arbitration Mechanism Select (AMS) - round-robin (usually 0)
    cc |= (0x0 << 11); // AMS is at bits [13:11]

    // Set I/O Submission Queue Entry Size (IOSQES)
    cc |= (0x6 << 16); // IOSQES = 6 (2^6 = 64 bytes)

    // Set I/O Completion Queue Entry Size (IOCQES)
    cc |= (0x4 << 20); // IOCQES = 4 (2^4 = 16 bytes)

    controller->registers->cc = cc;

    // Ensure the write completes before proceeding
    __asm__ volatile ("sfence" ::: "memory"); // Serialize writes

    // Wait for the controller to become ready
    if (!nvme_reset_controller(controller)) {
        panic("Error: Controller did not become ready after enabling\n");
        return; // Handle failure appropriately
    }

    printd(DEBUG_NVME, "Controller successfully configured and ready\n");
}


void nvme_ring_doorbell(nvme_controller_t* controller, uint16_t queueID, uint16_t newTail) {
    // Doorbell base offset typically starts at 0x1000 for NVMe MMIO
    uintptr_t doorbellBaseOffset = 0x1000;

    // Calculate the doorbell offset for the given queueID
    uintptr_t doorbellOffset = doorbellBaseOffset + (queueID * (4 * (1 << controller->doorbellStride)));

    // Ring the doorbell by writing the new tail pointer
    volatile uint16_t* doorbell = (volatile uint16_t*)((uintptr_t)controller->registers + doorbellOffset);
    *doorbell = newTail;

    printd(DEBUG_NVME, "NVME: Ringing doorbell for queue ID %u with new tail index %u\n", queueID, newTail);
}

void submit_command(nvme_controller_t* controller, nvme_submission_queue_entry_t* cmd, uint16_t queueID) {
    // Add command to submission queue
    nvme_submission_queue_entry_t* subQueue = controller->cmdSubQueue;
    uint16_t tailIndex = controller->cmdSubQueueTailIndex;

    subQueue[tailIndex] = *cmd;  // Copy the command to the submission queue

    // Increment tail index, wrapping if necessary
    tailIndex = (tailIndex + 1) % controller->maxQueueEntries;
    controller->cmdSubQueueTailIndex = tailIndex;

    // Ring the doorbell to inform controller
    nvme_ring_doorbell(controller, queueID, tailIndex);
}

void nvme_admin_init_queues(nvme_controller_t* controller)
{
    printd(DEBUG_NVME, "NVME: Initializing all queues to the max (0x%04x) entries (0x%016lx bytes for submission queues)\n", 
           controller->maxQueueEntries, controller->maxQueueEntries * sizeof(nvme_submission_queue_entry_t));

    // Calculate queue sizes
    size_t subQueueSize = controller->maxQueueEntries * sizeof(nvme_submission_queue_entry_t);
    size_t compQueueSize = controller->maxQueueEntries * sizeof(nvme_completion_queue_entry_t);
printf("1 ");
	kDebugLevel |= DEBUG_PAGING;
    // Allocate memory for admin and command queues
    controller->adminSubQueue = kmalloc_dma(subQueueSize);
    if (!controller->adminSubQueue) panic("Failed to allocate memory for admin submission queue\n");
printf("2 ");
    controller->cmdSubQueue = kmalloc_dma(subQueueSize);
	kDebugLevel &= ~(DEBUG_PAGING);
printf("2.5 ");
    if (!controller->cmdSubQueue) panic("Failed to allocate memory for command submission queue\n");

printf("3 ");
    controller->adminCompQueue = kmalloc_dma(compQueueSize);
    if (!controller->adminCompQueue) panic("Failed to allocate memory for admin completion queue\n");

printf("4 ");
    controller->cmdCompQueue = kmalloc_dma(compQueueSize);
    if (!controller->cmdCompQueue) panic("Failed to allocate memory for command completion queue\n");

    // Ensure queue alignment based on CAP.DSTRD
    uint64_t cap = controller->registers->cap;
    uint32_t dstrd = (cap >> 32) & 0xF; // Doorbell Stride
    size_t alignment = (1 << (12 + dstrd)); // Alignment required (e.g., 16 KB for DSTRD=2)

    if ((uintptr_t)controller->adminSubQueue % alignment != 0) {
        panic("Admin submission queue address 0x%016lx is not aligned to %lu bytes\n", 
              (uintptr_t)controller->adminSubQueue, alignment);
    }
printf("5 ");
    if ((uintptr_t)controller->adminCompQueue % alignment != 0) {
        panic("Admin completion queue address 0x%016lx is not aligned to %lu bytes\n", 
              (uintptr_t)controller->adminCompQueue, alignment);
    }

    // Set AQA (Admin Queue Attributes) register
    controller->registers->aqa = ((controller->maxQueueEntries - 1) << 16) | (controller->maxQueueEntries - 1);
printf("6");

    // Set ASQ (Admin Submission Queue) base address
    volatile uint32_t* asq_low = (volatile uint32_t*)&controller->registers->asq;
    volatile uint32_t* asq_high = asq_low + 1;
    *asq_low = (uint32_t)((uintptr_t)controller->adminSubQueue & 0xFFFFFFFF);
    *asq_high = (uint32_t)((uintptr_t)controller->adminSubQueue >> 32);

printf("7 ");
    wait(50); // Ensure write completion
printf("8 ");

    // Verify ASQ write
    uint64_t verify_asq = ((uint64_t)*asq_high << 32) | *asq_low;
    if (verify_asq != (uintptr_t)controller->adminSubQueue) {
        panic("ASQ write failed. Read back value: 0x%016lx\n", verify_asq);
    } else {
        printd(DEBUG_NVME, "ASQ successfully set to: 0x%016lx\n", verify_asq);
    }

printf("9 ");
    // Set ACQ (Admin Completion Queue) base address
    volatile uint32_t* acq_low = (volatile uint32_t*)&controller->registers->acq;
    volatile uint32_t* acq_high = acq_low + 1;
    *acq_low = (uint32_t)((uintptr_t)controller->adminCompQueue & 0xFFFFFFFF);
    *acq_high = (uint32_t)((uintptr_t)controller->adminCompQueue >> 32);

printf("10 ");

    // Initialize queue indices
    controller->cmdSubQueueTailIndex = 0;
    controller->admSubQueueTailIndex = 0;

    printd(DEBUG_NVME, "NVME: Admin queues initialized successfully\n");
}

void nvme_extract_cap(nvme_controller_t* controller) {
    uint64_t cap = controller->registers->cap;

    controller->maxQueueEntries = (cap & 0xFFFF);
	controller->contiguousQueuesRequired = (cap >> 37) & 0x1;
	controller->minPageSize = (cap >> 48) & 0xF;
	controller->maxPageSize = (cap >> 52) & 0xF;
	controller->cmdSetSupported = (cap >> 24) & 0xF;
	controller->doorbellStride = (cap >> 32) & 0xF;
	controller->defaultTimeout = ((cap >> 24) & 0xFF);

    printd(DEBUG_NVME, "NVME: Maximum Queue Entries Supported: %u\n", controller->maxQueueEntries);
    printd(DEBUG_NVME, "NVME: Contiguous Queues Required: %u\n", controller->contiguousQueuesRequired);
    printd(DEBUG_NVME, "NVME: Minimum Memory Page Size: %lu bytes\n", 1UL << (12 + controller->minPageSize));
    printd(DEBUG_NVME, "NVME: Maximum Memory Page Size: %lu bytes\n", 1UL << (12 + controller->maxPageSize));
    printd(DEBUG_NVME, "NVME: Command Sets Supported: %u\n", controller->cmdSetSupported);
    printd(DEBUG_NVME, "NVME: Doorbell Stride: %u (distance between doorbells: %u bytes)\n", controller->doorbellStride, 4 * (1 << controller->doorbellStride));
    printd(DEBUG_NVME, "NVME: Timeout: %u * 500 ms = %u ms\n", controller->defaultTimeout, controller->defaultTimeout * 500);

	//If the timeout presented by the controller is too large, use our own value. (10 seconds)
	if (controller->defaultTimeout > 20)
	{
		controller->defaultTimeout = 20 * 500;
		printd(DEBUG_NVME, "NVME Controller timeout too long, setting to 20 (10 seconds)\n");
	}
	else
		//Set timeout to MS
		controller->defaultTimeout *= 500;
}

void print_BARs(pci_config_space_t* config, char* state)
{
	printd(DEBUG_NVME, "NVME: BARs at %s: 0=0x%08x, 1=0x%08x, 2=0x%08x, 3=0x%08x, 4=0x%08x, 5=0x%08x, ", 
		state, config->BAR[0], config->BAR[1], config->BAR[2], config->BAR[3], config->BAR[4], config->BAR[5]);
}

uint64_t nvme_get_Base_Memory_Address(pci_device_t* nvmeDevice, pci_config_space_t* config)
{
	//https://wiki.osdev.org/PCI#Base_Address_Registers:
	//Before attempting to read the information about the BAR, make sure to disable both I/O and memory decode in the command byte. You can restore the original value after completing the BAR info read. 
	//To determine the amount of address space needed by a PCI device, you must save the original value of the BAR, write a value of all 1's to the register, then read it back. 
	//The original value of the BAR should then be restored. 

	printd(DEBUG_NVME, "NVME: Initializing BARs\n");

	//Disable IO/Memory Decode
	initialCMDValue = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_COMMAND_OFFSET);
	uint32_t cmd = initialCMDValue & ~(0x3);  // Clear bits 0 (I/O) and 1 (Memory)
	writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_COMMAND_OFFSET, cmd);

	bar0InitialValue = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET);
	bar1InitialValue = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET + 4);

	print_BARs(config, "initial");
	for (int idx=0;idx<2;idx++)
	{
		config->BAR[idx]=0xffffffff;
		wait(10);
	}
	print_BARs(config, "mask retrieval");

    uint64_t finalBaseAddressMask = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET);
    uint32_t bar1Value = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET + 4);

	writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET, bar0InitialValue);
	wait(50);
	writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET + 4, bar1InitialValue);
	wait(50);
	//Re-enable IO and Memory Decode
	writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_COMMAND_OFFSET, initialCMDValue);
	wait(50);

	printd(DEBUG_NVME, "NVME: Found MMIO Base Address MASK at BAR index 0, value 0x%08x\n",finalBaseAddressMask);

	if (finalBaseAddressMask > 0 && (finalBaseAddressMask & 0x1) != 0x1)
	{
		printd(DEBUG_NVME, "NVME: MMIO Base Address is 64-bit, adjusting mask value with the value 0x%08x\n", bar1Value);
		finalBaseAddressMask |= ((uint64_t)bar1Value << 32);
	}

	printd(DEBUG_NVME, "NVME: Final mask value is 0x%016x\n",finalBaseAddressMask);

	return finalBaseAddressMask  &= 0xFFFFFFFFFFFFFFF0;

}

void nvme_init_device(pci_device_t* nvmeDevice)
{
	bool barIs64Bit = false;
	uint64_t baseMemoryAddressMask = 0;
	uint64_t baseMemoryAddress = 0;

	nvme_enable_features(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);

	printd(DEBUG_NVME, "NVME: Retrieving PCI config for device at %u:%u:%u\n",nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);
	pci_config_space_t *config = pci_get_config_space(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);

	baseMemoryAddressMask = nvme_get_Base_Memory_Address(nvmeDevice, config);

	uint16_t bar0_size = (~baseMemoryAddressMask) + 1;

	baseMemoryAddress = ((uint64_t)bar0InitialValue | ((uint64_t)bar1InitialValue << 32));

	if (baseMemoryAddress < kAvailableMemory && bar0InitialValue > 0xA0000000 )
	{
		baseMemoryAddress = ((uint64_t)bar0InitialValue | ((uint64_t)bar1InitialValue << 32)) & ~(0xf);
		printd(DEBUG_NVME, "NVME: Initial base memory address is valid.  We'll use it! (0x%016lx)\n", baseMemoryAddress);
	}
	else
	{
		uint64_t temp = nvmeBaseAddressRemap;
		nvmeBaseAddressRemap += bar0_size;
		printd(DEBUG_NVME, "NVME: Initial base memory address (0x%016lx) is outside physical memory.  Using 0x%016x instead\n",baseMemoryAddress,temp);
		baseMemoryAddress = temp;
		printd(DEBUG_NVME, "Initializing base address 0x%08x to Bar[0], and 0x0 to BAR[1]\n",baseMemoryAddress);
		writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET, baseMemoryAddress & 0xFFFFFFFF);
		wait(50);
		writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET + 4, 0);
		wait(50);
		config = pci_get_config_space(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);
	}

	print_BARs(config, "post config");

	nvme_controller_t* controller = kmalloc(sizeof(nvme_controller_t));
	printd(DEBUG_NVME, "Allocated controller_t at 0x%08x\n",controller);
	controller->nvmePCIDevice = nvmeDevice;
	controller->mmioAddress = baseMemoryAddress;
	controller->registers = (volatile nvme_controller_regs_t*)controller->mmioAddress;
	controller->mmioSize = bar0_size;

	printd(DEBUG_NVME,"NVME: Updating paging for MMIO Base Address, identity mapped at 0x%016lx\n", controller->mmioAddress);
	paging_map_pages((pt_entry_t*)kKernelPML4v, controller->mmioAddress, controller->mmioAddress, bar0_size / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);

	nvme_print_version(controller->registers->version);
	nvme_extract_cap(controller);
	nvme_admin_init_queues(controller);
	nvme_initialize_controller(controller);
	kNVMEControllerCount++;

	// 		uint32_t cmd_status = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x06 & ~0x3); // Align offset
	// 		uint16_t status = (cmd_status >> 16) & 0xFFFF; // Extract upper 16 bits
	// 		if (status & (1 << 0)) { // Check for error conditions
	// 			printf("Device reported an error\n");
	// 			return;
	// 		}

	// 		uint16_t cmd = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x04) & 0xFFFF;
	// 		if (!(cmd & (1 << 1))) {
	// 			printf("Memory Space not enabled!\n");
	// 		}
	// 		if (!(cmd & (1 << 2))) {
	// 			printf("Bus Mastering not enabled!\n");
	// 		}
	// 		uint32_t bar0 = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x10 ) & 0xFFFFFFF0;
	// 		uint32_t bar1 = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x10 + 4);
	// 		uint64_t mmio_base = ((uint64_t)bar1 << 32) | bar0;
	// 		printf("BAR[0]: 0x%08x, BAR[1]: 0x%08x, MMIO Base: 0x%016lx\n", bar0, bar1, mmio_base);
	// 		cmd_status = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x04); // Read 32-bit Command and Status
	// 		cmd_status |= (1 << 20); // Write 1 to clear bit 4 in the Status Register (bit 20 in 32-bit combined register)
	// 		writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x04, cmd_status); // Write back the 32-bit value
	// 		cmd_status = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x04); // Read 32-bit Command and Status

	// 		cmd = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x04) & 0xFFFF;
	// 		cmd &= ~(1 << 2); // Clear bit 2 to disable Bus Mastering
	// 		writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x04, cmd);
	// 		cmd_status = readPCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, 0x04); // Read 32-bit Command and Status
	// paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)&controller->registers->cc);

}

void init_NVME()
{

	for (int idx = 0; idx < kPCIDeviceCount; idx++)
		if (kPCIDeviceHeaders[idx].class == 0x1 && kPCIDeviceHeaders[idx].subClass == 0x8)
			nvme_init_device(&kPCIDeviceHeaders[idx]);

	for (int idx = 0; idx < kPCIFunctionCount; idx++)
		if (kPCIDeviceFunctions[idx].class == 0x1 && kPCIDeviceFunctions[idx].subClass == 0x8)
			nvme_init_device(&kPCIDeviceFunctions[idx]);

}