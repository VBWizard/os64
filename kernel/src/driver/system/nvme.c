#include "nvme.h"
#include "kmalloc.h"
#include "paging.h"
#include "BasicRenderer.h"
#include "serial_logging.h"
#include "CONFIG.h"
#include "time.h"
#include "panic.h"
#include "math.h"
#include "memset.h"

extern uint64_t kDebugLevel;
int kNVMEControllerCount = 0;
uint16_t initialCMDValue;
uint32_t bar0InitialValue, bar1InitialValue;
uint64_t nvmeBaseAddressRemap = NVME_ABAR_OVERRIDE_ADDRESS;	

void log_nvme_debug_info(
    volatile nvme_controller_t* controller,         // Base NVMe registers address
    volatile nvme_submission_queue_entry_t* submission_queue, // Submission Queue pointer
    volatile nvme_completion_queue_entry_t* completion_queue, // Completion Queue pointer
    uint32_t sq_tail,                          // Submission Queue Tail index
    uint32_t cq_head,                          // Completion Queue Head index
    uint32_t queue_size,                        // Queue size
	uint32_t queueID
) {
    printd(DEBUG_NVME, "=== NVMe Debug Information ===\n");

	int a = queue_size;

    // Controller Status
    uint64_t cap = controller->registers->cap;
    uint32_t csts = controller->registers->csts;
    printd(DEBUG_NVME, "Controller CAP: 0x%016lx\n", cap);
    printd(DEBUG_NVME, "Controller CSTS: 0x%08x (RDY: %d, CFS: %d)\n",
           csts, csts & NVME_CSTS_RDY, (csts >> 1) & 1);

    // Submission Queue State
    printd(DEBUG_NVME, "Submission Queue Tail: %u\n", sq_tail);
    printd(DEBUG_NVME, "Submission Queue Entries:\n");
    for (uint32_t i = 0; i < 10; i++) {
        const nvme_submission_queue_entry_t* cmd = (void*)&submission_queue[i];
        printd(DEBUG_NVME, "%u: OPC=0x%02X CID=%u NSID=0x%X CDW10=0x%08X CWD11=0x%08x CWD12=0x%08x PRP1=0x%016lx\n",
               i, cmd->opc, cmd->cid, cmd->nsid, cmd->cdw10, cmd->cdw11, cmd->cdw12, cmd->prp1);
    }

    // Completion Queue State
    printd(DEBUG_NVME, "Completion Queue Head: %u\n", cq_head);
    printd(DEBUG_NVME, "Completion Queue Entries:\n");
    for (uint32_t i = 0; i < 10; i++) {
        const nvme_completion_queue_entry_t* entry = (void*)&completion_queue[i];
        printd(DEBUG_NVME, "  [%u]: SQHD=%u CID=%u Status=0x%04X\n",
               i, entry->sqhd, entry->cid, entry->status);
    }

	uint32_t dstrd = (cap >> 32) & 0xF; // Extract DSTRD from CAP
    // Doorbell Values
		uint32_t stride_in_bytes = 4 * (1 << controller->doorbellStride);

	volatile uint64_t submissionDoorbell = ((uintptr_t)controller->registers +
											DOORBELL_BASE_OFFSET +
											(queueID * 2 * stride_in_bytes));

	volatile uint64_t completionDoorbell = ((uintptr_t)controller->registers +
										DOORBELL_BASE_OFFSET +
										((queueID * 2 + 1) * stride_in_bytes));

	printd(DEBUG_NVME, "Submission Queue Doorbell: 0x%08X\n", submissionDoorbell);
    printd(DEBUG_NVME, "Completion Queue Doorbell: 0x%08X\n", completionDoorbell);

    printd(DEBUG_NVME, "=== End of NVMe Debug Information ===\n");
}

void nvme_print_version(uint32_t versionRegisterValue) {
    // Extract the major version (bits 31:16)
    uint16_t majorVersion = (versionRegisterValue >> 16) & 0xFFFF;

    // Extract the minor version (bits 15:8)
    uint8_t minorVersion = (versionRegisterValue >> 8) & 0xFF;

    // Extract the tertiary version (bits 7:0)
    uint8_t tertiaryVersion = versionRegisterValue & 0xFF;

    // Print the NVMe version
    printf("NVMe Version %d.%d.%d", majorVersion, minorVersion, tertiaryVersion);
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
		panic("MVME: CSTS did not signal ready after CC set to enabled\n");
		return false;
	}

    printd(DEBUG_NVME, "NVMe: controller reset completed and ready.\n");
	return true;
}

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

// Assume you have an nvme_controller_t* named controller

// Constants (these may differ depending on your implementation)
// Function to ring the doorbell for a given submission or completion queue
void nvme_ring_doorbell(nvme_controller_t* controller, uint16_t queueID, bool isSubmissionQueue, uint16_t newIndex) {
    // Calculate the doorbell register offset for the given queue ID

	uint32_t stride_in_bytes = 4 * (1 << controller->doorbellStride);

    if (isSubmissionQueue) {
        // Calculate the address for the submission doorbell register
        //volatile uint32_t* submissionDoorbell = (volatile uint32_t*)((uintptr_t)controller->registers + SUBMISSION_QUEUE_DOORBELL_OFFSET + doorbellOffset);
		volatile uint32_t* submissionDoorbell = (volatile uint32_t*)((uintptr_t)controller->registers +
											DOORBELL_BASE_OFFSET +
											(queueID * 2 * stride_in_bytes));

        // Write the new tail index to the submission doorbell
        *submissionDoorbell = newIndex;
	    __asm__ volatile("mfence" ::: "memory");
        printd(DEBUG_NVME, "NVME: Ringing submission queue doorbell for queue %u at %p, new tail index: %u\n", queueID, submissionDoorbell, newIndex);
    } else {
        // Calculate the address for the completion doorbell register
		volatile uint32_t* completionDoorbell = (volatile uint32_t*)((uintptr_t)controller->registers +
											DOORBELL_BASE_OFFSET +
											((queueID * 2 + 1) * stride_in_bytes));

        // Write the new head index to the completion doorbell
        *completionDoorbell = newIndex;
	    __asm__ volatile("mfence" ::: "memory");
        printd(DEBUG_NVME, "NVME: Ringing completion queue doorbell for queue %u at %p, new head index: %u\n", queueID, completionDoorbell, newIndex);
    }
}

void submit_command(nvme_controller_t* controller, nvme_submission_queue_entry_t* cmd, bool isAdminQueue) {

	printd(DEBUG_NVME,"NVME: submit_command: opc=0x%04x, nsid=0x%08x, cid=0x%08x, prp1=%p, cwd10=0x%08x, cwd11=0x%08x\n",
		cmd->opc, cmd->nsid, cmd->cid, cmd->prp1, cmd->cdw10, cmd->cdw11);

    // Add command to the submission queue
    nvme_submission_queue_entry_t* subQueue;
    uint16_t* tailIndexPtr;
    uint16_t maxQueueEntries;

    if (isAdminQueue) {
        subQueue = controller->admSubQueue;
        tailIndexPtr = &controller->admSubQueueTailIndex;
        maxQueueEntries = controller->queueDepth;
		printd(DEBUG_NVME, "NVME: submit_command: Using Admin sub queue @ 0x%016lx, tail = 0x%04x, queue depth = 0x%04x\n",
		subQueue, *tailIndexPtr, maxQueueEntries);
    } else {
        subQueue = controller->cmdSubQueue;  // Single command queue
        tailIndexPtr = &controller->cmdSubQueueTailIndex;
        maxQueueEntries = controller->queueDepth; // Single queue size
		printd(DEBUG_NVME, "NVME: submit_command: Using Cmd sub queue @ 0x%016lx, tail = 0x%04x, queue depth = 0x%04x\n",
		subQueue, *tailIndexPtr, maxQueueEntries);
    }

    // Add the command to the submission queue
    subQueue[*tailIndexPtr] = *cmd;

    // Increment tail index, wrapping if necessary
    *tailIndexPtr = (*tailIndexPtr + 1) % maxQueueEntries;
	printd(DEBUG_NVME, "NVME: submit_command: Incremented tail, now 0x%04x\n",*tailIndexPtr);

__asm__ volatile ("sfence" ::: "memory"); // Ensure memory writes are visible

    // Ring the doorbell to inform controller
    nvme_ring_doorbell(controller, isAdminQueue ? 0 : 1, true, *tailIndexPtr);
}

void nvme_init_admin_queues(nvme_controller_t* controller)
{
    printd(DEBUG_NVME, "NVME: Initializing all queues to the max (0x%04x) entries (0x%016lx bytes for submission queues)\n", 
           controller->maxQueueEntries, controller->maxQueueEntries * sizeof(nvme_submission_queue_entry_t));

printf("  (1 ");
    // Set AQA (Admin Queue Attributes) register
    //Making 64 quies since that's the size of the uint64_t where we keep track of the phase bits
	if (controller->maxQueueEntries > 0x40)
	{
		controller->queueDepth = 0x40;
		printd(DEBUG_NVME, "NVME: Controller supports more than 64 queue entries (0x%04x), will use %u entries\n", controller->maxQueueEntries, controller->queueDepth);
	}
	else
	{
		controller->queueDepth = controller->maxQueueEntries;
		printd(DEBUG_NVME, "NVME: Controller doesn't supports 64 queue entries (0x%04x), will use %u entries\n", controller->maxQueueEntries, controller->queueDepth);
	}
	controller->registers->aqa = ((controller->queueDepth - 1) << 16) | (controller->queueDepth - 1);
	printd(DEBUG_NVME,"NVME: Setting the aqa register to 0x%08x to support %u queues\n",controller->registers->aqa, controller->queueDepth);

printf("2 ");
    // Calculate queue sizes
    size_t subQueueSize = controller->queueDepth * sizeof(nvme_submission_queue_entry_t);
    size_t compQueueSize = controller->queueDepth * sizeof(nvme_completion_queue_entry_t);
    controller->admSubQueue = kmalloc_dma(subQueueSize);
    if (!controller->admSubQueue) panic("Failed to allocate memory for admin submission queue\n");
printf("2 ");
    controller->cmdSubQueue = kmalloc_dma(subQueueSize);
//	kDebugLevel &= ~(DEBUG_PAGING);
    if (!controller->cmdSubQueue) panic("Failed to allocate memory for command submission queue\n");

printf("3 ");
    controller->admCompQueue = kmalloc_dma(compQueueSize);
    if (!controller->admCompQueue) panic("Failed to allocate memory for admin completion queue\n");

printf("4 ");
    controller->cmdCompQueue = kmalloc_dma(compQueueSize);
    if (!controller->cmdCompQueue) panic("Failed to allocate memory for command completion queue\n");

    // Ensure queue alignment based on CAP.DSTRD
    uint64_t cap = controller->registers->cap;
    uint32_t dstrd = (cap >> 32) & 0xF; // Doorbell Stride
    size_t alignment = (1 << (12 + dstrd)); // Alignment required (e.g., 16 KB for DSTRD=2)

    if ((uintptr_t)controller->admSubQueue % alignment != 0) {
        panic("Admin submission queue address 0x%016lx is not aligned to %lu bytes\n", 
              (uintptr_t)controller->admSubQueue, alignment);
    }
printf("5 ");
    if ((uintptr_t)controller->admCompQueue % alignment != 0) {
        panic("Admin completion queue address 0x%016lx is not aligned to %lu bytes\n", 
              (uintptr_t)controller->admCompQueue, alignment);
    }

printf("6 ");

    // Set ASQ (Admin Submission Queue) base address
    volatile uint32_t* asq_low = (volatile uint32_t*)&controller->registers->asq;
    volatile uint32_t* asq_high = asq_low + 1;
    *asq_low = (uint32_t)((uintptr_t)controller->admSubQueue & 0xFFFFFFFF);
    *asq_high = (uint32_t)((uintptr_t)controller->admSubQueue >> 32);

printf("7 ");
    wait(50); // Ensure write completion
printf("8 ");

    // Verify ASQ write
    uint64_t verify_asq = ((uint64_t)*asq_high << 32) | *asq_low;
    if (verify_asq != (uintptr_t)controller->admSubQueue) {
        panic("ASQ write failed. Read back value: 0x%016lx\n", verify_asq);
    } else {
        printd(DEBUG_NVME, "ASQ successfully set to: 0x%016lx\n", verify_asq);
    }

printf("9 ");
    // Set ACQ (Admin Completion Queue) base address
    volatile uint32_t* acq_low = (volatile uint32_t*)&controller->registers->acq;
    volatile uint32_t* acq_high = acq_low + 1;
    *acq_low = (uint32_t)((uintptr_t)controller->admCompQueue & 0xFFFFFFFF);
    *acq_high = (uint32_t)((uintptr_t)controller->admCompQueue >> 32);
	printd(DEBUG_NVME, "ACQ successfully set to: 0x%016lx\n", (uintptr_t)*acq_high << 32 | *acq_low);

printf("10)\n");

    // Initialize queue indices
    controller->cmdSubQueueTailIndex = 0;
    controller->admSubQueueTailIndex = 0;
	controller->admCompQueueHeadIndex = 0;
	controller->cmdCompQueueHeadIndex = 0;
	controller->admCompCurrentPhases = 0;
    printd(DEBUG_NVME, "NVME: Admin queues initialized successfully\n");
}

uint8_t get_and_update_phase_bit(uint64_t* expected_phases, uint32_t index) {
    // Calculate the bit position within the uint64_t
    uint32_t bit_position = index % NUM_BITS;

    // Retrieve the current value of the bit
    uint8_t current_phase = (*expected_phases & (1ULL << bit_position)) >> bit_position;

    // Invert the phase value (1 for 0, 0 for 1) - this is the new current value
    uint8_t inverted_phase = !current_phase;

    // Save the new value back to the bit
    *expected_phases ^= (1ULL << bit_position);

    // Return the inverted phase value
    return inverted_phase;
}

/// @brief Wait for an admin or command queue entry to reflect completion (updates current phase, panics on timeout)
/// @param controller 
/// @param adminQueue 
/// @param entry 
/// @param cid 
/// @param entryIndex 
void nvme_wait_for_completion(nvme_controller_t* controller, bool adminQueue, volatile nvme_completion_queue_entry_t* entry, nvme_submission_queue_entry_t* command)
{
	uint32_t elapsed = 0;
	uint32_t delay = 10;
	int expectedPhase = 0;

	if (adminQueue)
		expectedPhase = get_and_update_phase_bit(&controller->admCompCurrentPhases, controller->admCompQueueHeadIndex);
	else
		expectedPhase = get_and_update_phase_bit(&controller->cmdCompCurrentPhases, controller->cmdCompQueueHeadIndex);

	while ((entry->cid != entry->cid || entry->status.phase_tag != expectedPhase) && elapsed < controller->defaultTimeout)
	{
		wait(delay);
		elapsed+=delay;
	}
	
	printd(DEBUG_NVME, "NVME:\tWaiting for completion of cid 0x%04x, before wait, status={status_code: 0x%02x, status_code_type: 0x%02x, more: 0x%u, phase: %u}\n", 	
						entry->cid, entry->status.status_code, entry->status.status_code_type, entry->status.more, entry->status.phase_tag);
	
	if (elapsed >= controller->defaultTimeout)
	{
		log_nvme_debug_info(controller, command, entry, controller->cmdSubQueueTailIndex, controller->cmdCompQueueHeadIndex, controller->queueDepth, adminQueue?0:1);
		panic("Timeout (%u seconds) waiting for command completion\n", controller->defaultTimeout);
	}
	printd(DEBUG_NVME, "\tNVME:\t After waiting for completion of cid 0x%04x, status={status_code: 0x%02x, status_code_type: 0x%02x, more: 0x%u, phase: %u}\n", 	
						entry->cid, entry->status.status_code, entry->status.status_code_type, entry->status.more, entry->status.phase_tag);
}

void nvme_init_cmd_queues(nvme_controller_t* controller)
{
   nvme_submission_queue_entry_t* cmd = kmalloc_aligned(sizeof(nvme_submission_queue_entry_t));

    // Step 1: Create I/O Completion Queue
    cmd->opc = NVME_ADMIN_CREATE_IO_COMPLETION_QUEUE;                  // CREATE IO COMPLETION QUEUE
	cmd->nsid = 0x0;
    cmd->cid = controller->adminCID++;
	uint32_t mallocSize = sizeof(nvme_completion_queue_entry_t) * controller->queueDepth;
    cmd->prp1 = (uintptr_t)kmalloc_dma(mallocSize);         // Physical address of CQ buffer
    cmd->cdw10 = 0x0000001 | ((controller->queueDepth - 1)<<16); // CQ ID = 1, Queue Size = QUEUE_DEPTH - 1
    cmd->cdw11 = 0x1;                 // Interrupts disabled, Physically Contiguous
    // Submit command to Admin SQ
    submit_command(controller, cmd, true);
	nvme_completion_queue_entry_t* completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completionEntry, cmd);
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);
	if (completionEntry->status.status_code != 0 || completionEntry->status.status_code_type != 0)
	{
		log_nvme_debug_info(controller, cmd, completionEntry, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, controller->maxQueueEntries, 0);
		panic("Queue completion status != 0!!! (0x%08x\n",completionEntry->status.status_code);
	}
	
	controller->cmdCompQueue = (volatile nvme_completion_queue_entry_t*)cmd->prp1;
    printd(DEBUG_NVME, "NVME: Command Completion Queue successfully created at 0x%016lx\n",cmd->prp1);

    // Step 2: Create I/O Submission Queue
    memset(cmd, 0, sizeof(nvme_submission_queue_entry_t));
    cmd->opc = NVME_ADMIN_CREATE_IO_SUBMISSION_QUEUE;                  // CREATE IO SUBMISSION QUEUE
	cmd->nsid = 0x0;
    cmd->cid =  controller->adminCID++;
	mallocSize = sizeof(nvme_submission_queue_entry_t) * controller->queueDepth;
    cmd->prp1 = (uintptr_t)kmalloc_dma(mallocSize);         // Physical address of SQ buffer
    cmd->cdw10 = ((controller->queueDepth - 1) << 16) | 1; //Queue Size = QUEUE_DEPTH - 1,  SQ ID = 1
    cmd->cdw11 = 0x00010001;          // Priority = 0 (high), PC=1

    submit_command(controller, cmd, true);

	completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completionEntry, cmd);
	if (completionEntry->status.status_code != 0 || completionEntry->status.status_code_type != 0)
	{
		log_nvme_debug_info(controller, cmd, completionEntry, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, controller->maxQueueEntries, 0);
		panic("Admin completion status != 0!!! (0x%08x\n",completionEntry->status.status_code);
	}
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);
	__asm__ volatile("mfence" ::: "memory");

	controller->cmdSubQueue = (void*)cmd->prp1;
    printd(DEBUG_NVME, "NVME: Command Submission Queue successfully created at 0x%016lx\n",cmd->prp1);
	printd(DEBUG_NVME, "NVME: kfreeing cmd @ 0x%016lx\n", cmd);
	kfree(cmd);
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

void nvme_parse_lba_format(uint8_t* namespace_buffer, uint8_t index) {
    // Offset 0x80: Start of LBA Format Table
    uint8_t* lba_format_table = namespace_buffer + 0x80;

    // Each LBA Format Descriptor is 4 bytes
    uint32_t* lba_format_descriptor = (uint32_t*)(lba_format_table + (index * 4));

    // Extract LBADS (bits 16-23)
    uint8_t lbads = (*lba_format_descriptor >> 16) & 0xFF;

    // Calculate block size
    uint64_t block_size = 1ULL << lbads;

    // Print the results
    printd(DEBUG_NVME, "Index: %u\n", index);
    printd(DEBUG_NVME, "LBADS: %u\n", lbads);
    printd(DEBUG_NVME, "Block Size: %lu bytes\n", block_size);
}

void nvme_set_features(nvme_controller_t* controller)
{
	nvme_submission_queue_entry_t* command = kmalloc(sizeof(nvme_submission_queue_entry_t));

	command->opc = NVME_ADMIN_SET_FEATURES;
	command->nsid = 0x0;
	command->cid = controller->adminCID++;
	command->cdw10 = 0x07; // Feature Identifier: Number of Queue
	// Request 4 Submission Queues and 4 Completion Queues
	command->cdw11 = ((4 - 1) << 16) | (4 - 1);
	submit_command(controller, command, true);
	// Wait for completion
	nvme_completion_queue_entry_t* completion = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completion, command);

	// Parse response
	uint16_t max_submission_queues = (completion->cmd_specific & 0xFFFF) + 1;
	uint16_t max_completion_queues = ((completion->cmd_specific >> 16) & 0xFFFF) + 1;

	printd(DEBUG_NVME, "Max Submission Queues: %u, Max Completion Queues: %u\n", 
		max_submission_queues, max_completion_queues);
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);
	kfree(command);
}

void nvme_identify(nvme_controller_t* controller)
{
	uint64_t elapsed = 0;
	nvme_submission_queue_entry_t* command = kmalloc(sizeof(nvme_submission_queue_entry_t));

	command->opc = NVME_ADMIN_IDENTIFY;
	command->nsid = 0x0;
	command->prp1 = (uint64_t)kmalloc_dma(PAGE_SIZE);
	command->cid = controller->adminCID++;
	command->cdw10 = 2; // number of namespaces
	submit_command(controller, command, true);

	nvme_completion_queue_entry_t* completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	
	// Wait for completion
	nvme_wait_for_completion(controller, true, completionEntry, command);

	if (completionEntry->status.status_code != 0)
	{
		log_nvme_debug_info(controller, command, completionEntry, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, controller->maxQueueEntries, 0);
		panic("Admin completion status != 0!!! (0x%08x\n",completionEntry->status.status_code);
	}
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);

	controller->nsid = *(uint32_t*)command->prp1;
	printd(DEBUG_NVME, "Number of namespaces: 0x%08x\n", *(uint32_t*)command->prp1);

	command->opc = NVME_ADMIN_IDENTIFY;
	command->nsid = controller->nsid;
	// leave command->prp1 as it was, we can re-use it
	command->cid = controller->adminCID++;
	command->cdw10 = 0; // Identify Namespace Data Structure
	submit_command(controller, command, true);

	// Wait for completion
	completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completionEntry,  command);
	
	if (completionEntry->status.status_code != 0)
	{
		log_nvme_debug_info(controller, command, completionEntry, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, controller->maxQueueEntries, 0);
		panic("Admin completion status != 0!!! (0x%08x\n",completionEntry->status);
	}
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);

	nvme_namespace_data_t* idData = (nvme_namespace_data_t*)command->prp1;
	printd(DEBUG_NVME, "Namespace Size: %lu logical blocks\n", idData->namespaceSize);
	printd(DEBUG_NVME, "Namespace Capacity: %lu logical blocks\n", idData->namespaceCapacity);
	printd(DEBUG_NVME, "Namespace Utilization: %lu logical blocks\n", idData->namespaceUtilization);
	printd(DEBUG_NVME, "Namespace Features: 0x%02X\n", idData->namespaceFeatures);
	printd(DEBUG_NVME, "Number of LBA Formats: %u\n", idData->numOfLBAFormats + 1); // 0-based index
	printd(DEBUG_NVME, "Active LBA Format: %u\n", idData->formattedLBASize & 0x0F);
	printd(DEBUG_NVME, "Formatted LBA Size: %u\n", idData->formattedLBASize);
	printd(DEBUG_NVME, "NVM Capacity (bytes): ");

	printd(DEBUG_NVME, "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ",
		idData->nvmcap[0], idData->nvmcap[1], idData->nvmcap[2], idData->nvmcap[3], idData->nvmcap[4], idData->nvmcap[5], idData->nvmcap[6], idData->nvmcap[7], idData->nvmcap[8], 
		idData->nvmcap[9], idData->nvmcap[10], idData->nvmcap[11], idData->nvmcap[12], idData->nvmcap[13], idData->nvmcap[14], idData->nvmcap[15]);

	nvme_parse_lba_format((uint8_t*)idData, idData->formattedLBASize & 0x0F);
	
	kfree(command);
}

void nvme_LBA_read_sectors(nvme_controller_t* controller)
{
	nvme_submission_queue_entry_t* cmd = kmalloc_aligned(sizeof(nvme_submission_queue_entry_t));
	cmd->opc = NVME_OPCODE_READ;
	cmd->nsid = controller->nsid;
    cmd->cid = controller->cmdCID++;
	cmd->prp1 = (uintptr_t)kmalloc_dma(PAGE_SIZE);
	memset((void*)cmd->prp1, 0xbb, PAGE_SIZE);
	cmd->cdw10 = 0x0;
	cmd->cdw11 = 0x0;
	cmd->cdw12 = 0x0;
    submit_command(controller, cmd, false);
}

void nvme_init_device(pci_device_t* nvmeDevice)
{
	uint64_t baseMemoryAddressMask = 0;
	uint64_t baseMemoryAddress = 0;

	nvme_enable_features(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);

	printd(DEBUG_NVME, "NVME: Retrieving PCI config for device at %u:%u:%u\n",nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);
	pci_config_space_t *config = pci_get_config_space(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);

	baseMemoryAddressMask = nvme_get_Base_Memory_Address(nvmeDevice, config);

	uint16_t bar0_size = (~baseMemoryAddressMask) + 1;

	baseMemoryAddress = ((uint64_t)(bar0InitialValue & 0xfffffff0) | ((uint64_t)bar1InitialValue << 32));

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
	controller->adminCID = controller->cmdCID = 0;

	printd(DEBUG_NVME,"NVME: Updating paging for MMIO Base Address, identity mapped at 0x%016lx\n", controller->mmioAddress);
	paging_map_pages((pt_entry_t*)kKernelPML4v, controller->mmioAddress, controller->mmioAddress, bar0_size / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);

	nvme_print_version(controller->registers->version);
	nvme_extract_cap(controller);
	nvme_init_admin_queues(controller);
	nvme_initialize_controller(controller);
	nvme_identify(controller);
	nvme_set_features(controller);
	nvme_init_cmd_queues(controller);

	printd(DEBUG_NVME,"Completing a test read ... \n");
	nvme_submission_queue_entry_t* cmd = kmalloc_aligned(sizeof(nvme_submission_queue_entry_t));
	printd(DEBUG_NVME,"Allocated nvme command @ 0x%016lx\n", cmd);
	cmd->opc = NVME_OPCODE_READ;
	cmd->nsid = controller->nsid;
    cmd->cid = controller->cmdCID++;
	cmd->prp1 = (uintptr_t)kmalloc_dma(PAGE_SIZE);
	printd(DEBUG_NVME,"allocated prp1 @ 0x%016lx\n",cmd->prp1);
	memset((void*)cmd->prp1, 0xbb, PAGE_SIZE);
	cmd->cdw10 = 0x0;
	cmd->cdw11 = 0x0;
	cmd->cdw12 = 0x0;
	printd(DEBUG_NVME,"Submitting read command\n");
    submit_command(controller, cmd, false);
	volatile nvme_completion_queue_entry_t* completionEntry = (volatile nvme_completion_queue_entry_t*)&controller->cmdCompQueue[controller->cmdCompQueueHeadIndex];
	nvme_wait_for_completion(controller, false, (volatile nvme_completion_queue_entry_t*)completionEntry, cmd);
	if (completionEntry->cid!=cmd->cid || completionEntry->status.status_code != 0 || completionEntry->status.phase_tag!=1)
	{
		nvme_submission_queue_entry_t* cmd2 = kmalloc_aligned(sizeof(nvme_submission_queue_entry_t));
		cmd2->opc=0x2;
		cmd2->nsid=0xffffffff;
		cmd2->cid=controller->adminCID++;
		cmd2->cdw10=0x000F0001;
		cmd2->prp1=cmd->prp1;
		memset((void*)cmd2->prp1, 0xAA, 4096);  // Fill with a known pattern
		submit_command(controller, cmd2, true);
		nvme_completion_queue_entry_t* completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
		nvme_wait_for_completion(controller, true, completionEntry, cmd2);
		nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);

	}


	kfree(cmd);

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