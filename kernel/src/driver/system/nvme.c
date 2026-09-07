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
#include "memcpy.h"
#include "vfs.h"
#include "ata.h"
#include "block_device.h"
#include "printd.h"
#include "strings.h"
#include "spinlock.h"
#include "smp.h"
#include "smp_core.h"
#include "kernel.h"
#include "driver/system/x86_64.h"
#include "watchpoint.h"   // WATCHDMA — the hardware aimed at the DMA buffer's PTE

extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;
extern bool kWatchDMA;   // WATCHDMA cmdline flag (kernel_commandline.c)
// The buffer whose page-table chain WATCHDMA watches: the first controller's
// write bounce buffer. Both controllers' buffers are HHDM aliases under the
// same PML4E and PDPTE, so watching one covers the shared upper levels, and
// four debug registers is the whole hardware budget anyway.
char *kNvmeWatchTarget = NULL;

// ── The DMA bounce buffers' tripwire (2026-08-14) ───────────────────────────
//
// THE FAULT THAT BOUGHT THIS. A #PF on the P5, kernel mode, error 0x3
// ("present, protection violation, write"), inside memcpy, storing to
// 0x00000000098a7000 — which was dmaWriteBuffer, back when kmalloc_dma handed
// out PHYSICAL addresses and identity-mapped them PRESENT|WRITE|PCD; that
// single mapping was the only reason the pointer was dereferenceable at all.
// (Since 2026-08-19 the pointer is an HHDM alias and the load-bearing mapping
// is the allocator's lazy-HHDM entry — a mapping with the same property this
// tripwire exists to defend: the buffer lives for the controller's whole
// life, nothing ever legitimately re-maps or downgrades it.) A page that
// reports present-but-not-writable is not a paging decision anybody made. It
// is somebody else's entries, or somebody else's write, landing in the page
// table that owns ours — the P5's was eventually caught doing exactly that
// (the status-table overrun, 2026-08-15), and the tripwire stays armed for
// the next one.
//
// The old symptom was a bare #PF inside memcpy, with the call chain to be
// reconstructed from register archaeology. The new symptom names the crime one
// instruction before it happens: which buffer, what its entry said at init,
// what it says now. Loud beats silent, and early beats post-mortem
// (SUCCESSION.md, rule 5).
//
// Cost is two page-table walks per I/O — four dependent loads each, in front of
// a transfer that is about to go to a disk. First page and last page, because
// the signature being hunted (a page-table page handed to a second owner)
// rewrites a whole region's worth of entries at once and either end catches it.
static uint64_t nvme_dma_pte(uintptr_t va)
{
	return (uint64_t)paging_walk_paging_table_keep_flags(
	    (pt_entry_t *)kKernelPML4v, va & ~(uintptr_t)0xFFF, true);
}

static void nvme_dma_tripwire_check(const char *which, uintptr_t base,
                                    size_t length, uint64_t atInit,
                                    uintptr_t ptPageAtInit)
{
	uintptr_t pages[2];
	pages[0] = base & ~(uintptr_t)0xFFF;
	pages[1] = (base + (length ? length - 1 : 0)) & ~(uintptr_t)0xFFF;

	for (int i = 0; i < 2; i++) {
		uint64_t pte = nvme_dma_pte(pages[i]);
		if ((pte & PAGE_PRESENT) && (pte & PAGE_WRITE))
			continue;                      // still present, still ours to write

		// 0xbadbadba is the walk's "no such mapping" sentinel, not an entry —
		// decoding its bits as flags prints nonsense (it reads as write=1),
		// so say which of the two answers this is before saying anything else.
		bool walked = (pte != 0xbadbadba);

		printf("\n");
		if (walked) {
			printf(">>> NVMe %s DMA buffer 0x%016lx: page 0x%016lx entry is 0x%016lx "
			       "(present=%u write=%u user=%u), was 0x%016lx at controller init <<<\n",
			       which, base, pages[i], pte,
			       (unsigned)((pte & PAGE_PRESENT) != 0),
			       (unsigned)((pte & PAGE_WRITE) != 0),
			       (unsigned)((pte & PAGE_USER) != 0), atInit);
		} else {
			printf(">>> NVMe %s DMA buffer 0x%016lx: page 0x%016lx HAS NO MAPPING AT ALL "
			       "(the walk died above the leaf); its entry was 0x%016lx at controller init <<<\n",
			       which, base, pages[i], atInit);
		}

		// The evidence, before the halt: every level, the leaf table's identity
		// and provenance, and whether that "page table" is really data now.
		paging_report_walk((pt_entry_t *)kKernelPML4v, pages[i], which);

		uintptr_t ptNow = paging_leaf_table_phys((pt_entry_t *)kKernelPML4v, pages[i]);
		printf(">>> leaf page table was at phys 0x%012lx at init, is at 0x%012lx now — %s <<<\n",
		       (uint64_t)ptPageAtInit, (uint64_t)ptNow,
		       (ptNow == ptPageAtInit) ? "SAME PAGE (its contents were rewritten in place)"
		                               : "DIFFERENT PAGE (the table itself was replaced or lost)");

		panic("NVMe %s DMA buffer 0x%016lx lost its mapping at page 0x%016lx. "
		      "The kernel's page tables were rewritten under a live "
		      "mapping — see the walk above.\n", which, base, pages[i]);
	}
}

int kNVMEControllerCount = 0;
uint16_t initialCMDValue;
uint32_t bar0InitialValue, bar1InitialValue;
uint64_t nvmeBaseAddressRemap = NVME_ABAR_OVERRIDE_ADDRESS;	
char* nvmeIdentifyInfo;

void log_nvme_debug_info(
    volatile nvme_controller_t* controller,         // Base NVMe registers address
	bool admin,
    uint32_t sq_tail,                          // Submission Queue Tail index
    uint32_t cq_head,                          // Completion Queue Head index
	uint32_t queueID
) {
	__asm__ volatile("sfence" ::: "memory");
    printd(DEBUG_EXCEPTIONS, "=== NVMe Debug Information ===\n");
    // Controller Status
    uint64_t cap = controller->registers->cap;
    uint32_t csts = controller->registers->csts;
    printd(DEBUG_EXCEPTIONS, "Controller CAP: 0x%016lx\n", cap);
    printd(DEBUG_EXCEPTIONS, "Controller CSTS: 0x%08x (RDY: %d, CFS: %d)\n",
           csts, csts & NVME_CSTS_RDY, (csts >> 1) & 1);

    // Submission Queue State
    printd(DEBUG_EXCEPTIONS, "Submission Queue Tail: %u\n", sq_tail);
    printd(DEBUG_EXCEPTIONS, "Submission Queue Entries:\n");
    for (uint32_t i = (sq_tail - 5) % controller->maxQueueEntries; i < (sq_tail + 5) % controller->maxQueueEntries; i++) {
        const nvme_submission_queue_entry_t* cmd = (admin?(void*)&controller->admSubQueue[i]:(void*)&controller->cmdSubQueue[i]);
        printd(DEBUG_EXCEPTIONS, "%u: OPC=0x%02X CID=%u NSID=0x%X CDW10=0x%08X CWD11=0x%08x CWD12=0x%08x PRP1=0x%016lx\n",
               i, cmd->opc, cmd->cid, cmd->nsid, cmd->cdw10, cmd->cdw11, cmd->cdw12, cmd->prp1);
    }

    // Completion Queue State
    printd(DEBUG_EXCEPTIONS, "Completion Queue Head: %u\n", cq_head);
    printd(DEBUG_EXCEPTIONS, "Completion Queue Entries:\n");
    for (uint32_t i = (cq_head - 5) % controller->maxQueueEntries; i < (cq_head + 5) % controller->maxQueueEntries; i++) {
        const nvme_completion_queue_entry_t* entry = (admin?(void*)&controller->admCompQueue[i]:(void*)&controller->cmdCompQueue[i]);
        printd(DEBUG_EXCEPTIONS, "  [%u]: SQHD=%u CID=%u Status=0x%04X\n",
               i, entry->sqhd, entry->cid, entry->status);
    }

	//Remarked as unused: uint32_t dstrd = (cap >> 32) & 0xF; // Extract DSTRD from CAP
    // Doorbell Values
	uint32_t stride_in_bytes = 4 * (1 << controller->doorbellStride);

	volatile uint64_t submissionDoorbell = ((uintptr_t)controller->registers +
											DOORBELL_BASE_OFFSET +
											(queueID * 2 * stride_in_bytes));

	volatile uint64_t completionDoorbell = ((uintptr_t)controller->registers +
										DOORBELL_BASE_OFFSET +
										((queueID * 2 + 1) * stride_in_bytes));

	printd(DEBUG_EXCEPTIONS, "Submission Queue Doorbell: 0x%08X\n", submissionDoorbell);
    printd(DEBUG_EXCEPTIONS, "Completion Queue Doorbell: 0x%08X\n", completionDoorbell);

    printd(DEBUG_EXCEPTIONS, "=== End of NVMe Debug Information ===\n");
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

// Disable the controller and wait for CSTS.RDY to clear — and nothing more
// (nvme_reset_controller below is the disable-AND-re-enable dance; this is
// only its first half). Needed since 2026-08-21, the day os64 first booted
// FROM its NVMe disk: UEFI firmware that loaded the bootloader and kernel
// through this controller hands it over ENABLED, with the firmware's own
// admin queues live. The admin queue registers (AQA/ASQ/ACQ) are writable
// ONLY while CC.EN=0 (NVMe spec 3.1.5+) — program them into an enabled
// controller and the writes are dropped, the re-enable resurrects the
// firmware's stale queue addresses, and the first admin command answers
// with status 0x2 (Invalid Field), which is exactly the panic the OVMF
// rehearsal produced. Every pre-HD-boot path (SeaBIOS, or UEFI booting the
// ISO through AHCI) handed the controller over disabled, which is why this
// gap could hide for the driver's whole life.
static void nvme_disable_controller(nvme_controller_t* controller) {
    uint32_t currentDelay = 0;
    uint32_t intervalDelay = controller->defaultTimeout / 20;

    if (!(controller->registers->cc & 0x1) && !(controller->registers->csts & 0x1))
        return;   // already disabled and settled — the pre-HD-boot normal

    printd(DEBUG_NVME, "NVMe: controller handed over ENABLED (firmware booted from it) — disabling before admin queue setup\n");
    controller->registers->cc &= ~(0x1);
    while ((controller->registers->csts & 0x1) && currentDelay < controller->defaultTimeout) {
        wait(intervalDelay);
        currentDelay += intervalDelay;
    }
    if (currentDelay >= controller->defaultTimeout)
        panic("NVMe: CSTS RDY did not clear on pre-init disable\n");
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
    while ((controller->registers->csts & 0x1) && currentDelay < controller->defaultTimeout) {
        wait(intervalDelay);
        currentDelay += intervalDelay;
    }
    if (currentDelay >= controller->defaultTimeout) {
        printd(DEBUG_NVME, "NVMe: Timeout waiting for RDY to clear after reset.\n");
        panic("NVMe: CSTS RDY did not clear after CC.EN was cleared\n");
        return false;
    }
    currentDelay = 0;

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

    printd(DEBUG_NVME, "NVMe: controller reset completed, back online.\n");
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
    printd(DEBUG_NVME | DEBUG_DETAILED, "Configuring NVMe Controller Configuration (CC) register\n");

    uint32_t cc = 0;

    // Host's page size in bytes
    uint32_t host_page_size = 4096; // Replace with your system's actual page size

    // Calculate MPS value: MPS = log2(host_page_size) - 12
    uint8_t mps = (uint8_t)(log2(host_page_size) - 12);

    // Ensure MPS is within the supported range from CAP.MPSMIN to CAP.MPSMAX
    if (mps < controller->minPageSize || mps > controller->maxPageSize) {
        printd(DEBUG_NVME | DEBUG_DETAILED, "Error: Host page size is not supported by the controller\n");
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

    printd(DEBUG_NVME | DEBUG_DETAILED, "Controller successfully configured and ready\n");
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
        printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Ringing submission queue doorbell for queue %u at %p, new tail index: %u\n", queueID, submissionDoorbell, newIndex);
    } else {
        // Calculate the address for the completion doorbell register
		volatile uint32_t* completionDoorbell = (volatile uint32_t*)((uintptr_t)controller->registers +
											DOORBELL_BASE_OFFSET +
											((queueID * 2 + 1) * stride_in_bytes));

        // Write the new head index to the completion doorbell
        *completionDoorbell = newIndex;
	    __asm__ volatile("mfence" ::: "memory");
        printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Ringing completion queue doorbell for queue %u at %p, new head index: %u\n", queueID, completionDoorbell, newIndex);
    }
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

/// @brief Submit NVME command
/// @param controller 
/// @param cmd 
/// @param isAdminQueue 
void nvme_submit_command(nvme_controller_t* controller, nvme_submission_queue_entry_t* cmd, bool isAdminQueue) {

	printd(DEBUG_NVME | DEBUG_DETAILED,"NVME: submit_command: opc=0x%04x,nsid=0x%04x,cid=0x%04x,prp1=%p,prp2=%p,cwd10=0x%08x,cwd11=0x%08x,cdw12=0x%08x\n",
		cmd->opc, cmd->nsid, cmd->cid, cmd->prp1, cmd->prp2, cmd->cdw10, cmd->cdw11, cmd->cdw12);

    // Add command to the submission queue
    nvme_submission_queue_entry_t* subQueue;
    uint16_t* tailIndexPtr;
    uint16_t maxQueueEntries;

    if (isAdminQueue) {
        subQueue = controller->admSubQueue;
        tailIndexPtr = &controller->admSubQueueTailIndex;
        maxQueueEntries = controller->queueDepth;
    } else {
        subQueue = controller->cmdSubQueue;  // Single command queue
        tailIndexPtr = &controller->cmdSubQueueTailIndex;
        maxQueueEntries = controller->queueDepth; // Single queue size
    }

		printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: submit_command: Using %s sub queue @ 0x%016lx, tail = 0x%04x, queue depth = 0x%04x\n",
		isAdminQueue?"admin":"cmd", subQueue, *tailIndexPtr, maxQueueEntries);

    // Add the command to the submission queue
    subQueue[*tailIndexPtr] = *cmd;

    // Increment tail index, wrapping if necessary
    *tailIndexPtr = (*tailIndexPtr + 1) % maxQueueEntries;
	printd(DEBUG_NVME | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "NVME: submit_command: Incremented tail, now 0x%04x\n",*tailIndexPtr);

__asm__ volatile ("sfence" ::: "memory"); // Ensure memory writes are visible

    // Ring the doorbell to inform controller
    nvme_ring_doorbell(controller, isAdminQueue ? 0 : 1, true, *tailIndexPtr);
}

/// @brief Wait for an admin or command queue entry to reflect completion (updates current phase, panics on timeout, ignores completion errors)
/// @param controller 
/// @param adminQueue 
/// @param entry 
/// @param cid 
/// @param entryIndex 
void nvme_wait_for_completion(nvme_controller_t* controller, bool adminQueue, volatile nvme_completion_queue_entry_t* entry, nvme_submission_queue_entry_t* command)
{
	int expectedPhase = 0;

	if (adminQueue)
		expectedPhase = get_and_update_phase_bit(&controller->admCompCurrentPhases, controller->admCompQueueHeadIndex);
	else
		expectedPhase = get_and_update_phase_bit(&controller->cmdCompCurrentPhases, controller->cmdCompQueueHeadIndex);

	printd(DEBUG_NVME | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "NVME:\tWaiting for completion of cid 0x%04x, expectedPhase = %u, before wait, status={status_code: 0x%02x, status_code_type: 0x%02x, more: 0x%u, phase: %u}\n",
						entry->cid, expectedPhase, entry->status.status_code, entry->status.status_code_type, entry->status.more, entry->status.phase_tag);

	// Poll with PAUSE against a same-core rdtsc deadline — deliberately NOT
	// the tick-based wait(): I/O-queue waits run under the controller ioLock
	// with interrupts off, and if the waiting core is the BSP, IF=0 there
	// stops IRQ0 and kTicksSinceStart itself — a tick-based timeout would
	// never expire. rdtsc read on ONE core is monotonic, so it's safe here
	// (cross-core TSC comparison is what must be avoided on QEMU/WSL2).
	// During init_NVME, kCPUCyclesPerSecond hasn't been measured yet
	// (detect_cpu runs after storage init), so fall back to a generous
	// fixed cycle budget (~5s on a 4GHz part).
	uint64_t deadline_cycles = kCPUCyclesPerSecond
		? (uint64_t)controller->defaultTimeout * (kCPUCyclesPerSecond / 1000)
		: 20000000000ULL;
	uint64_t start = rdtsc();
	bool timed_out = false;
	bool waited = false;

	while (entry->cid != command->cid || entry->status.phase_tag != expectedPhase)
	{
		if (rdtsc() - start > deadline_cycles)
		{
			timed_out = true;
			break;
		}
		waited = true;
		__builtin_ia32_pause();
	}

	if (timed_out)
	{
		log_nvme_debug_info(controller, adminQueue, controller->cmdSubQueueTailIndex, controller->cmdCompQueueHeadIndex, adminQueue?0:1);
		panic("Timeout (%u ms) waiting for NVMe completion of cid 0x%04x\n", controller->defaultTimeout, command->cid);
	}
	if (waited)
		printd(DEBUG_NVME | DEBUG_DETAILED, "\tNVME:\t After waiting for completion of cid 0x%04x, status={status_code: 0x%02x, status_code_type: 0x%02x, more: 0x%u, phase: %u}\n",
							entry->cid, entry->status.status_code, entry->status.status_code_type, entry->status.more, entry->status.phase_tag);
}

// ---------------------------------------------------------------------------
// I/O-queue serialization. nvme_do_io is strictly one-command-at-a-time
// (submit → poll the CQ head slot → advance head), which is only correct if
// commands never interleave. They CAN: file-backed demand paging issues disk
// reads from the page-fault path (vma.c), concurrently with whatever thread
// was already reading — on another core, or on the SAME core via preemption
// while polling. Interleaved, both waiters capture the same CQ head slot; one
// consumes it, the other spins into the timeout panic (seen ~50% of VBox
// boots; QEMU's tickless entry masked it by squeezing threads onto the BSP).
//
// The lock is irqsave so the holder cannot be preempted: a bare spinlock
// could deadlock one core (holder preempted, fault-context IF=0 spinner
// waiting on it forever). Consequence: the completion poll above must never
// depend on ticks (see rdtsc rationale there).
// ---------------------------------------------------------------------------
static uint64_t nvme_io_lock(nvme_controller_t* controller)
{
	// Same-core re-entry tripwire: only this core ever writes its own id
	// into ioLockOwner, so reading our id back means WE hold the lock and
	// have re-entered — i.e. a page fault taken INSIDE the critical section
	// needed disk I/O. That must never happen (the DMA bounce buffers and
	// queues are HHDM/kmalloc-mapped); panic loudly instead of spinning
	// silently forever with interrupts off.
	int64_t me = kCLSInitialized ? (int64_t)get_core_local_storage()->apic_id : 0;
	if (controller->ioLockOwner == me)
		panic("NVMe: nested I/O on core %ld — disk read from a fault taken inside nvme_do_io?\n", me);

	uint64_t flags = spinlock_acquire_irqsave(&controller->ioLock);
	controller->ioLockOwner = me;
	return flags;
}

static void nvme_io_unlock(nvme_controller_t* controller, uint64_t flags)
{
	controller->ioLockOwner = -1;
	spinlock_release_irqrestore(&controller->ioLock, flags);
}

void nvme_init_admin_queues(nvme_controller_t* controller)
{
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Initializing all queues to the max (0x%04x) entries (0x%016lx bytes for submission queues)\n",
           controller->maxQueueEntries, controller->maxQueueEntries * sizeof(nvme_submission_queue_entry_t));

    // The registers this function programs (AQA/ASQ/ACQ) are writable only
    // while CC.EN=0 — a firmware that booted from this controller leaves it
    // enabled, so make the state true before writing (see
    // nvme_disable_controller's comment for the whole story).
    nvme_disable_controller(controller);

printf("  (1 ");
    // Set AQA (Admin Queue Attributes) register
    //Making 64 quies since that's the size of the uint64_t where we keep track of the phase bits
	if (controller->maxQueueEntries > 0x40)
	{
		controller->queueDepth = 0x40;
		printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Controller supports more than 64 queue entries (0x%04x), will use %u entries\n", controller->maxQueueEntries, controller->queueDepth);
	}
	else
	{
		controller->queueDepth = controller->maxQueueEntries;
		printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Controller doesn't supports 64 queue entries (0x%04x), will use %u entries\n", controller->maxQueueEntries, controller->queueDepth);
	}
	controller->registers->aqa = ((controller->queueDepth - 1) << 16) | (controller->queueDepth - 1);
	printd(DEBUG_NVME | DEBUG_DETAILED,"NVME: Setting the aqa register to 0x%08x to support %u queues\n",controller->registers->aqa, controller->queueDepth);

printf("2 ");
    // Calculate queue sizes
    size_t subQueueSize = controller->queueDepth * sizeof(nvme_submission_queue_entry_t);
    size_t compQueueSize = controller->queueDepth * sizeof(nvme_completion_queue_entry_t);
    // Each allocation yields TWO addresses now (kmalloc_dma's HHDM contract,
    // 2026-08-19): the struct keeps the VIRTUAL pointer the driver indexes
    // through, and the phys locals below feed the ASQ/ACQ registers — the
    // only consumers of the physical address, all in this function.
    uintptr_t admSubQueuePhys, cmdSubQueuePhys, admCompQueuePhys, cmdCompQueuePhys;
    controller->admSubQueue = kmalloc_dma(subQueueSize, &admSubQueuePhys);
    if (!controller->admSubQueue) panic("Failed to allocate memory for admin submission queue\n");
printf("2 ");
    controller->cmdSubQueue = kmalloc_dma(subQueueSize, &cmdSubQueuePhys);
//	kDebugLevel &= ~(DEBUG_PAGING);
    if (!controller->cmdSubQueue) panic("Failed to allocate memory for command submission queue\n");

printf("3 ");
    controller->admCompQueue = kmalloc_dma(compQueueSize, &admCompQueuePhys);
    if (!controller->admCompQueue) panic("Failed to allocate memory for admin completion queue\n");

printf("4 ");
    controller->cmdCompQueue = kmalloc_dma(compQueueSize, &cmdCompQueuePhys);
    if (!controller->cmdCompQueue) panic("Failed to allocate memory for command completion queue\n");
    (void)cmdSubQueuePhys; (void)cmdCompQueuePhys;   // the IO-queue pair is
    // re-created with fresh allocations in create_io_queues — these two
    // command-queue allocations are legacy warm-up the controller never sees.

    // Ensure queue alignment based on CAP.DSTRD
    uint64_t cap = controller->registers->cap;
    uint32_t dstrd = (cap >> 32) & 0xF; // Doorbell Stride
    size_t alignment = (1 << (12 + dstrd)); // Alignment required (e.g., 16 KB for DSTRD=2)

    // Alignment is a PHYSICAL requirement — it is the device that dereferences
    // this address. (The HHDM alias shares the low bits, but check the number
    // the hardware will actually be handed.)
    if (admSubQueuePhys % alignment != 0) {
        panic("Admin submission queue phys 0x%016lx is not aligned to %lu bytes\n",
              admSubQueuePhys, alignment);
    }
printf("5 ");
    if (admCompQueuePhys % alignment != 0) {
        panic("Admin completion queue phys 0x%016lx is not aligned to %lu bytes\n",
              admCompQueuePhys, alignment);
    }

printf("6 ");

    // Set ASQ (Admin Submission Queue) base address — the PHYSICAL address;
    // the VA in the struct is the kernel's business, never the device's.
    volatile uint32_t* asq_low = (volatile uint32_t*)&controller->registers->asq;
    volatile uint32_t* asq_high = asq_low + 1;
    *asq_low = (uint32_t)(admSubQueuePhys & 0xFFFFFFFF);
    *asq_high = (uint32_t)(admSubQueuePhys >> 32);

printf("7 ");
    wait(50); // Ensure write completion
printf("8 ");

    // Verify ASQ write
    uint64_t verify_asq = ((uint64_t)*asq_high << 32) | *asq_low;
    if (verify_asq != admSubQueuePhys) {
        panic("ASQ write failed. Read back value: 0x%016lx\n", verify_asq);
    } else {
        printd(DEBUG_NVME | DEBUG_DETAILED, "ASQ successfully set to: 0x%016lx\n", verify_asq);
    }

printf("9 ");
    // Set ACQ (Admin Completion Queue) base address
    volatile uint32_t* acq_low = (volatile uint32_t*)&controller->registers->acq;
    volatile uint32_t* acq_high = acq_low + 1;
    *acq_low = (uint32_t)(admCompQueuePhys & 0xFFFFFFFF);
    *acq_high = (uint32_t)(admCompQueuePhys >> 32);
	printd(DEBUG_NVME | DEBUG_DETAILED, "ACQ successfully set to: 0x%016lx\n", (uintptr_t)*acq_high << 32 | *acq_low);

printf("10)\n");

    // Initialize queue indices
    controller->cmdSubQueueTailIndex = 0;
    controller->admSubQueueTailIndex = 0;
	controller->admCompQueueHeadIndex = 0;
	controller->cmdCompQueueHeadIndex = 0;
	controller->admCompCurrentPhases = 0;
	controller->ioLock = 0;
	controller->ioLockOwner = -1;
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Admin queues initialized successfully\n");
}

void nvme_init_cmd_queues(nvme_controller_t* controller)
{
   nvme_submission_queue_entry_t* cmd = kmalloc_aligned(sizeof(nvme_submission_queue_entry_t));

    // Step 1: Create I/O Completion Queue
    cmd->opc = NVME_ADMIN_CREATE_IO_COMPLETION_QUEUE;                  // CREATE IO COMPLETION QUEUE
	cmd->nsid = 0x0;
    cmd->cid = controller->adminCID++;
	uint32_t mallocSize = sizeof(nvme_completion_queue_entry_t) * controller->queueDepth;
    // One allocation, two addresses: the device gets the PHYS in prp1, the
    // driver keeps the VA for the struct pointer below. (These used to be the
    // same number — the identity-map era; see kmalloc_dma.)
    uintptr_t ioCompQueuePhys;
    void *ioCompQueueVa = kmalloc_dma(mallocSize, &ioCompQueuePhys);
    cmd->prp1 = ioCompQueuePhys;                            // Physical address of CQ buffer
    cmd->cdw10 = controller->cmdQID | ((controller->queueDepth - 1)<<16); // CQ ID = 1, Queue Size = QUEUE_DEPTH - 1
    cmd->cdw11 = 0x1;                 // Interrupts disabled, Physically Contiguous
    // Submit command to Admin SQ
    nvme_submit_command(controller, cmd, true);
	nvme_completion_queue_entry_t* completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completionEntry, cmd);
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);
	if (completionEntry->status.status_code != 0 || completionEntry->status.status_code_type != 0)
	{
		log_nvme_debug_info(controller, true, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, 0);
		panic("Queue completion status != 0!!! (0x%08x\n",completionEntry->status.status_code);
	}
	
	controller->cmdCompQueue = (volatile nvme_completion_queue_entry_t*)ioCompQueueVa;
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Command Completion Queue successfully created at phys 0x%016lx\n",cmd->prp1);

    // Step 2: Create I/O Submission Queue
    memset(cmd, 0, sizeof(nvme_submission_queue_entry_t));
    cmd->opc = NVME_ADMIN_CREATE_IO_SUBMISSION_QUEUE;                  // CREATE IO SUBMISSION QUEUE
	cmd->nsid = 0x0;
    cmd->cid =  controller->adminCID++;
	mallocSize = sizeof(nvme_submission_queue_entry_t) * controller->queueDepth;
    uintptr_t ioSubQueuePhys;
    void *ioSubQueueVa = kmalloc_dma(mallocSize, &ioSubQueuePhys);
    cmd->prp1 = ioSubQueuePhys;                             // Physical address of SQ buffer
    cmd->cdw10 = ((controller->queueDepth - 1) << 16) | 1; //Queue Size = QUEUE_DEPTH - 1,  SQ ID = 1
    cmd->cdw11 = 0x00010001;          // Priority = 0 (high), PC=1

    nvme_submit_command(controller, cmd, true);

	completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completionEntry, cmd);
	if (completionEntry->status.status_code != 0 || completionEntry->status.status_code_type != 0)
	{
		log_nvme_debug_info(controller, true, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, 0);
		panic("Admin completion status != 0!!! (0x%08x\n",completionEntry->status.status_code);
	}
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);
	__asm__ volatile("mfence" ::: "memory");

	controller->cmdSubQueue = ioSubQueueVa;
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Command Submission Queue successfully created at phys 0x%016lx\n",cmd->prp1);
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

    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Maximum Queue Entries Supported: %u\n", controller->maxQueueEntries);
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Contiguous Queues Required: %u\n", controller->contiguousQueuesRequired);
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Minimum Memory Page Size: %lu bytes\n", 1UL << (12 + controller->minPageSize));
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Maximum Memory Page Size: %lu bytes\n", 1UL << (12 + controller->maxPageSize));
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Command Sets Supported: %u\n", controller->cmdSetSupported);
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Doorbell Stride: %u (distance between doorbells: %u bytes)\n", controller->doorbellStride, 4 * (1 << controller->doorbellStride));
    printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Timeout: %u * 500 ms = %u ms\n", controller->defaultTimeout, controller->defaultTimeout * 500);

	//If the timeout presented by the controller is too large, use our own value. (10 seconds)
	if (controller->defaultTimeout > 20)
	{
		controller->defaultTimeout = 20 * 500;
		printd(DEBUG_NVME | DEBUG_DETAILED, "NVME Controller timeout too long, setting to 20 (10 seconds)\n");
	}
	else
		//Set timeout to MS
		controller->defaultTimeout *= 500;
}

void print_BARs(pci_config_space_t* config, char* state)
{
	printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: BARs at %s: 0=0x%08x, 1=0x%08x, 2=0x%08x, 3=0x%08x, 4=0x%08x, 5=0x%08x, ", 
		state, config->BAR[0], config->BAR[1], config->BAR[2], config->BAR[3], config->BAR[4], config->BAR[5]);
}

uint64_t nvme_get_Base_Memory_Address(pci_device_t* nvmeDevice, pci_config_space_t* config)
{
	//https://wiki.osdev.org/PCI#Base_Address_Registers:
	//Before attempting to read the information about the BAR, make sure to disable both I/O and memory decode in the command byte. You can restore the original value after completing the BAR info read. 
	//To determine the amount of address space needed by a PCI device, you must save the original value of the BAR, write a value of all 1's to the register, then read it back. 
	//The original value of the BAR should then be restored. 

	printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Initializing BARs\n");

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

	printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Found MMIO Base Address MASK at BAR index 0, value 0x%08x\n",finalBaseAddressMask);

	if (finalBaseAddressMask > 0 && (finalBaseAddressMask & 0x1) != 0x1)
	{
		printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: MMIO Base Address is 64-bit, adjusting mask value with the value 0x%08x\n", bar1Value);
		finalBaseAddressMask |= ((uint64_t)bar1Value << 32);
	}

	printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Final mask value is 0x%016lx\n",finalBaseAddressMask);

	return finalBaseAddressMask  &= 0xFFFFFFFFFFFFFFF0;

}

uint32_t nvme_parse_lba_format(uint8_t* namespace_buffer, uint8_t index) {
    // Offset 0x80: Start of LBA Format Table
    uint8_t* lba_format_table = namespace_buffer + 0x80;

    // Each LBA Format Descriptor is 4 bytes
    uint32_t* lba_format_descriptor = (uint32_t*)(lba_format_table + (index * 4));

    // Extract LBADS (bits 16-23)
    uint8_t lbads = (*lba_format_descriptor >> 16) & 0xFF;

    // Calculate block size
    uint64_t block_size = 1ULL << lbads;

    // Print the results
    printd(DEBUG_NVME | DEBUG_DETAILED, "Index: %u\n", index);
    printd(DEBUG_NVME | DEBUG_DETAILED, "LBADS: %u\n", lbads);
    printd(DEBUG_NVME | DEBUG_DETAILED, "Block Size: %lu bytes\n", block_size);

	return block_size;
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
	nvme_submit_command(controller, command, true);
	// Wait for completion
	nvme_completion_queue_entry_t* completion = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completion, command);

	// Parse response
	uint16_t max_submission_queues = (completion->cmd_specific & 0xFFFF) + 1;
	uint16_t max_completion_queues = ((completion->cmd_specific >> 16) & 0xFFFF) + 1;

	printd(DEBUG_NVME | DEBUG_DETAILED, "Max Submission Queues: %u, Max Completion Queues: %u\n", 
		max_submission_queues, max_completion_queues);
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);
	kfree(command);
}

void nvme_parse_model_name(char nvme_device_name[40], char* deviceName)
{

	for (int cnt=0;cnt<39;cnt++)
		deviceName[cnt] = nvme_device_name[cnt];
	
	deviceName[39]='\0';

	strtrim(deviceName);

}

uint64_t calculate_mdts(uint8_t mdts) {
    uint64_t max_size = 1; // Start with 2^0
    uint8_t shift = 12 + mdts; // Add the base shift (12 for 4 KB)

    // Perform left-shift iteratively to calculate 2^shift
    while (shift > 0) {
        max_size *= 2; // Multiply by 2 for each bit
        shift--;
    }

    return max_size; // Return the computed size in bytes
}

void nvme_identify(nvme_controller_t* controller)
{
	nvme_submission_queue_entry_t* command = kmalloc(sizeof(nvme_submission_queue_entry_t));

	uintptr_t identifyPhys;
	nvmeIdentifyInfo = kmalloc_dma(PAGE_SIZE, &identifyPhys);

	//List of Active Namespace IDs:
	command->opc = NVME_ADMIN_IDENTIFY;
	command->nsid = 0x0;
	command->prp1 = identifyPhys;
	command->cid = controller->adminCID++;
	command->cdw10 = 2; // number of namespaces
	nvme_submit_command(controller, command, true);

	nvme_completion_queue_entry_t* completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	
	// Wait for completion
	nvme_wait_for_completion(controller, true, completionEntry, command);

	if (completionEntry->status.status_code != 0)
	{
		log_nvme_debug_info(controller, true, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, 0);
		panic("Admin completion status != 0!!! (0x%08x\n",completionEntry->status.status_code);
	}
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);

	// Read the RESULT through the kernel's pointer, not through prp1: prp1 is
	// the device's (physical) address now, and dereferencing it as a VA was
	// only ever legal in the identity-map era.
	controller->nsid = *(uint32_t*)nvmeIdentifyInfo;
	printd(DEBUG_NVME | DEBUG_DETAILED, "Number of namespaces: 0x%08x\n", controller->nsid);
	kfree(nvmeIdentifyInfo);
	nvmeIdentifyInfo = NULL;

	uintptr_t bufferPhys;
	char* buffer = kmalloc_dma(PAGE_SIZE, &bufferPhys);

	//Identify Namespace Data Structure:
	command->nsid = controller->nsid;
	command->prp1 = bufferPhys;
	command->cid = controller->adminCID++;
	command->cdw10 = 0; // Identify Namespace Data Structure
	nvme_submit_command(controller, command, true);

	// Wait for completion
	completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completionEntry,  command);
	if (completionEntry->status.status_code != 0)
	{
		log_nvme_debug_info(controller, true, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, 0);
		panic("Admin completion status != 0!!! (0x%08x\n",completionEntry->status);
	}
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);

	nvme_namespace_data_t* idData = (nvme_namespace_data_t*)buffer;
	printd(DEBUG_NVME | DEBUG_DETAILED, "Namespace Size: %lu logical blocks\n", idData->namespaceSize);
	printd(DEBUG_NVME | DEBUG_DETAILED, "Namespace Capacity: %lu logical blocks\n", idData->namespaceCapacity);
	printd(DEBUG_NVME | DEBUG_DETAILED, "Namespace Utilization: %lu logical blocks\n", idData->namespaceUtilization);
	printd(DEBUG_NVME | DEBUG_DETAILED, "Namespace Features: 0x%02X\n", idData->namespaceFeatures);
	printd(DEBUG_NVME | DEBUG_DETAILED, "Number of LBA Formats: %u\n", idData->numOfLBAFormats + 1); // 0-based index
	printd(DEBUG_NVME | DEBUG_DETAILED, "Active LBA Format: %u\n", idData->formattedLBASize & 0x0F);
	printd(DEBUG_NVME | DEBUG_DETAILED, "Formatted LBA Size: %u\n", idData->formattedLBASize);
	printd(DEBUG_NVME | DEBUG_DETAILED, "NVM Capacity (bytes): ");

	printd(DEBUG_NVME | DEBUG_DETAILED, "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ",
		idData->nvmcap[0], idData->nvmcap[1], idData->nvmcap[2], idData->nvmcap[3], idData->nvmcap[4], idData->nvmcap[5], idData->nvmcap[6], idData->nvmcap[7], idData->nvmcap[8], 
		idData->nvmcap[9], idData->nvmcap[10], idData->nvmcap[11], idData->nvmcap[12], idData->nvmcap[13], idData->nvmcap[14], idData->nvmcap[15]);

	controller->blockSize = nvme_parse_lba_format((uint8_t*)idData, idData->formattedLBASize & 0x0F);
	
	//Identify Controller Data Structure:
	// leave command->prp1 as it was, we can re-use it
	command->nsid=0x0;
	command->cid = controller->adminCID++;
	command->cdw10 = 1; // Identify Controller Data Structure
	nvme_submit_command(controller, command, true);

	// Wait for completion
	completionEntry = &controller->admCompQueue[controller->admCompQueueHeadIndex];
	nvme_wait_for_completion(controller, true, completionEntry,  command);
	if (completionEntry->status.status_code != 0)
	{
		log_nvme_debug_info(controller, true, controller->admSubQueueTailIndex, controller->admCompQueueHeadIndex, 0);
		panic("Admin completion status != 0!!! (0x%08x\n",completionEntry->status);
	}
	nvme_ring_doorbell(controller, 0, false, ++controller->admCompQueueHeadIndex);

	nvme_identify_controller_t* cData = (nvme_identify_controller_t*)buffer;
	nvme_parse_model_name(cData->mn, controller->deviceName);
	controller->maxBytesPerTransfer = calculate_mdts(cData->mdts);
	printd(DEBUG_NVME, "NVME: Identified max bytes per NVME transfer: 0x%08x bytes\n", controller->maxBytesPerTransfer);
	//kDebugLevel |= DEBUG_KMALLOC | DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED;
	// Both addresses persist in the struct (e1000's shape): the VA is what
	// every memcpy and tripwire walk uses; the PHYS is what each I/O writes
	// into prp1/prp2. They stopped being the same number on 2026-08-19.
	controller->dmaReadBuffer = kmalloc_dma(controller->maxBytesPerTransfer,
	                                        &controller->dmaReadBufferPhys);
	controller->dmaWriteBuffer = kmalloc_dma(controller->maxBytesPerTransfer,
	                                         &controller->dmaWriteBufferPhys);

	// Arm the tripwire: photograph both buffers' page-table entries while they
	// are known good, so every later I/O has something truthful to compare
	// against and a report can say "was X, is Y" instead of only "is Y".
	controller->dmaReadPteAtInit  = nvme_dma_pte((uintptr_t)controller->dmaReadBuffer);
	controller->dmaWritePteAtInit = nvme_dma_pte((uintptr_t)controller->dmaWriteBuffer);
	controller->dmaReadPtPageAtInit  = paging_leaf_table_phys((pt_entry_t *)kKernelPML4v,
	                                       (uintptr_t)controller->dmaReadBuffer);
	controller->dmaWritePtPageAtInit = paging_leaf_table_phys((pt_entry_t *)kKernelPML4v,
	                                       (uintptr_t)controller->dmaWriteBuffer);

	// And hand the same two pages to the paging sentinels, so the damage is
	// noticed at the next task lifecycle boundary rather than waiting for the
	// next disk I/O to walk into it. Same evidence, hours earlier.
	paging_sentinel_add((uintptr_t)controller->dmaWriteBuffer, "NVMe write DMA buffer");
	paging_sentinel_add((uintptr_t)controller->dmaReadBuffer,  "NVMe read DMA buffer");

	// WATCHDMA remembers its target here but ARMS LATE, from kernel_init's
	// post-boot block (nvme_watch_dma_chain below). Arming at this point would
	// catch BOOT doing lawful work: ktask maps its argv at TASK_ARGV_VIRT
	// through the kernel's OWN PML4, which legitimately ORs PAGE_USER into
	// PML4[0] (0x…23 -> 0x…27). A halting watchpoint on that lawful write is a
	// triple fault during boot — which is exactly how this comment got written.
	if (kWatchDMA && kNvmeWatchTarget == NULL)
		kNvmeWatchTarget = controller->dmaWriteBuffer;
	// The BEFORE picture, so a later failure has something to be compared to by
	// a human reading a photograph of a dead machine. Gated: it is eight lines
	// per controller, and it is exactly the eight lines you want on the boot
	// that precedes the crash.
	if (kDebugLevel & DEBUG_NVME)
		paging_report_walk((pt_entry_t *)kKernelPML4v,
		                   (uintptr_t)controller->dmaWriteBuffer,
		                   "write DMA buffer (armed)");
	// (A probe that lived here on 2026-08-14 walked the same page's HHDM alias
	// to ask whether the two aliases share tables — if they did, the
	// allocator's unmap-on-free would take the identity mapping down with it.
	// They do NOT: identity walks PML4[0]→0x34e000→0x34f000→0x3f8000 while the
	// HHDM alias walks PML4[256]→0x34b000→0x34c000→0x3f7000, distinct at every
	// level. Recorded here so nobody re-runs that experiment. Note in passing
	// that the identity alias carries PCD and the HHDM alias does not — one
	// physical page, two memory types, which x86 frowns on and we get away
	// with because the DMA buffer is only ever touched through the identity VA.)
	printd(DEBUG_NVME, "NVME: DMA bounce buffers armed — read 0x%016lx (pte 0x%016lx), write 0x%016lx (pte 0x%016lx)\n",
	       (uintptr_t)controller->dmaReadBuffer, controller->dmaReadPteAtInit,
	       (uintptr_t)controller->dmaWriteBuffer, controller->dmaWritePteAtInit);

	printd(DEBUG_NVME, "NVME: Device found, model: %s, max bytes per PRP = %u\n", controller->deviceName, controller->maxBytesPerTransfer);

	kfree(buffer);
	kfree(command);
}

// Build a PRP list covering prpCount data pages starting at startAddress.
// Each list page holds 512 entries. If the list spans more than one page the
// last entry on each intermediate page is a chain pointer to the next list
// page, per the NVMe spec.  Returns the address of the first list page.
static uintptr_t setup_prp_list(uintptr_t startAddress, uint32_t prpCount)
{
    const uint32_t entries_per_page = PAGE_SIZE / sizeof(uintptr_t); // 512

    // Two worlds per list page, kept explicitly apart: the kernel FILLS the
    // page through its HHDM pointer, the ENTRIES it fills in (data addresses
    // and chain pointers alike) are physical, because the device is the only
    // reader of the list's contents. startAddress is physical on arrival —
    // the caller passes the bounce buffer's phys, and PRP arithmetic never
    // leaves that world.
    uintptr_t firstListPhys;
    uintptr_t* currentPage = kmalloc_dma(PAGE_SIZE, &firstListPhys);
    uint32_t remaining = prpCount;

    while (remaining > 0) {
        if (remaining <= entries_per_page) {
            // Last (or only) list page: fill entirely with data addresses.
            for (uint32_t i = 0; i < remaining; i++) {
                currentPage[i] = startAddress;
                startAddress += PAGE_SIZE;
            }
            remaining = 0;
        } else {
            // More list pages needed: 511 data entries, then a chain pointer.
            for (uint32_t i = 0; i < entries_per_page - 1; i++) {
                currentPage[i] = startAddress;
                startAddress += PAGE_SIZE;
            }
            remaining -= entries_per_page - 1;
            uintptr_t nextPagePhys;
            uintptr_t* nextPage = kmalloc_dma(PAGE_SIZE, &nextPagePhys);
            currentPage[entries_per_page - 1] = nextPagePhys;   // the DEVICE walks this
            currentPage = nextPage;                             // the KERNEL walks this
        }
    }

    return firstListPhys;
}

// Walk and free every page in a chained PRP list.  prpCount must match the
// value passed to setup_prp_list so we know where the chain ends.
static void free_prp_list(uintptr_t listPage, uint32_t prpCount)
{
    const uint32_t entries_per_page = PAGE_SIZE / sizeof(uintptr_t);
    uint32_t remaining = prpCount;

    // listPage arrives PHYSICAL (it is cmd->prp2, the device's copy), and so
    // is every chain pointer stored in the pages — so each hop converts to
    // the HHDM alias to read the chain and to hand kfree an honest pointer.
    // (The identity era let this function conflate the two; its kfree also
    // leaked an identity MAPPING per page per I/O, forever. No more.)
    while (remaining > entries_per_page) {
        uintptr_t* page = (uintptr_t*)(listPage + kHHDMOffset);
        uintptr_t nextPagePhys = page[entries_per_page - 1]; // chain pointer (phys)
        kfree(page);
        listPage = nextPagePhys;
        remaining -= entries_per_page - 1;
    }
    kfree((void*)(listPage + kHHDMOffset)); // last (or only) page
}

// ── Command-stream telemetry (2026-08-06 — the queue-depth court's
// discovery phase). Before teaching the driver to keep multiple commands
// in flight, MEASURE what the filesystems actually hand it: if writes
// arrive one sector at a time (the 33KB/s copy's suspected shape), the fix
// belongs a layer up before queue depth buys anything; if they arrive in
// runs, pipelining pays immediately. Buckets are command sizes in BLOCKS.
// A summary prints via printd(DEBUG_NVME) every 8192 commands — silent on
// a normal boot, a histogram on a diagnostic one.
typedef struct {
	uint64_t cmds, blocks;
	uint64_t sz1, sz2_8, sz9_127, sz128p;
} nvme_iostat_t;
nvme_iostat_t kNvmeReadStats = {0};
nvme_iostat_t kNvmeWriteStats = {0};

static void nvme_iostat_note(bool isWrite, uint32_t blockCount)
{
	nvme_iostat_t *s = isWrite ? &kNvmeWriteStats : &kNvmeReadStats;
	s->cmds++;
	s->blocks += blockCount;
	if (blockCount <= 1)        s->sz1++;
	else if (blockCount <= 8)   s->sz2_8++;
	else if (blockCount <= 127) s->sz9_127++;
	else                        s->sz128p++;

	uint64_t total = kNvmeReadStats.cmds + kNvmeWriteStats.cmds;
	if ((total & 8191) == 0)
		printd(DEBUG_NVME,
		       "NVME iostat: R %lu cmds/%lu blks [1:%lu 2-8:%lu 9-127:%lu 128+:%lu] "
		       "W %lu cmds/%lu blks [1:%lu 2-8:%lu 9-127:%lu 128+:%lu]\n",
		       kNvmeReadStats.cmds, kNvmeReadStats.blocks,
		       kNvmeReadStats.sz1, kNvmeReadStats.sz2_8,
		       kNvmeReadStats.sz9_127, kNvmeReadStats.sz128p,
		       kNvmeWriteStats.cmds, kNvmeWriteStats.blocks,
		       kNvmeWriteStats.sz1, kNvmeWriteStats.sz2_8,
		       kNvmeWriteStats.sz9_127, kNvmeWriteStats.sz128p);
}

static void nvme_do_io(nvme_controller_t* controller, uint64_t LBA, size_t length, void* buffer, bool isWrite) {
    if (controller->maxBytesPerTransfer == 0)
        panic("nvme_do_io: controller->maxBytesPerTransfer = 0\n");

    size_t remaining = length;
    uintptr_t userBufferOffset = (uintptr_t)buffer;
    uint64_t currentLBA = LBA;

    while (remaining > 0) {
        size_t transferLength = remaining > controller->maxBytesPerTransfer ? controller->maxBytesPerTransfer : remaining;
        uint32_t blockCount = transferLength / controller->blockSize;
        if (transferLength % controller->blockSize)
            blockCount++;

        uint32_t prpCount = transferLength / PAGE_SIZE;
        if (transferLength % PAGE_SIZE)
            prpCount++;

        nvme_iostat_note(isWrite, blockCount);

        char* dmaBuffer = isWrite ? controller->dmaWriteBuffer : controller->dmaReadBuffer;

        // Before ANY use of it — the write path is about to memcpy INTO this
        // buffer, and the read path is about to let the controller DMA into it,
        // which is the worse of the two to get wrong: a device writing a page
        // we no longer own corrupts in silence, where the memcpy at least
        // faults. Both directions ask the same question.
        nvme_dma_tripwire_check(isWrite ? "write" : "read", (uintptr_t)dmaBuffer,
                                transferLength,
                                isWrite ? controller->dmaWritePteAtInit
                                        : controller->dmaReadPteAtInit,
                                isWrite ? controller->dmaWritePtPageAtInit
                                        : controller->dmaReadPtPageAtInit);

        // The command lives on the STACK (2026-08-04, the paging-pool
        // exhaustion hunt): nvme_submit_command COPIES the struct into the
        // DMA-visible submission ring (subQueue[tail] = *cmd), so the heap
        // allocation this used to do was pure ceremony — and expensive
        // ceremony: one page-ALIGNED kmalloc/kfree per disk I/O, ~1,100/sec
        // under a logd-on-ext2 soak, was the single largest driver of the
        // allocator's address march (each aligned carve tours fresh
        // territory whose lazy-HHDM mapping draws paging-pool pages that
        // never come back). The memset stands in for kmalloc's
        // zero-on-alloc, which was load-bearing for the fields not set
        // below (cdw13-15, flags, metadata pointer).
        nvme_submission_queue_entry_t cmdOnStack;
        nvme_submission_queue_entry_t* cmd = &cmdOnStack;
        memset(cmd, 0, sizeof(cmdOnStack));
        cmd->opc  = isWrite ? NVME_OPCODE_WRITE : NVME_OPCODE_READ;
        cmd->nsid = controller->nsid;
        // The device gets the PHYSICAL buffer address; dmaBuffer (the VA) is
        // for the memcpys below. PRP arithmetic stays pure phys — page 2 of
        // the buffer is phys+4096 no matter where the kernel sees it.
        cmd->prp1 = isWrite ? controller->dmaWriteBufferPhys
                            : controller->dmaReadBufferPhys;

        if (prpCount == 2)
            cmd->prp2 = cmd->prp1 + PAGE_SIZE;
        else if (prpCount > 2)
            cmd->prp2 = setup_prp_list(cmd->prp1 + PAGE_SIZE, prpCount - 1);

        cmd->cdw10 = currentLBA & 0xffffffff;
        cmd->cdw11 = currentLBA >> 32;
        cmd->cdw12 = blockCount - 1;

        // ---- Critical section: everything that touches the shared queues,
        // the CID counter, or the shared DMA bounce buffers. See the ioLock
        // comment block above nvme_io_lock for why this exists.
        uint64_t lockFlags = nvme_io_lock(controller);

        // CID wraps safely: the lock guarantees one command in flight, so no
        // two commands ever share a CID slot.
        cmd->cid  = controller->cmdCID++;

        if (isWrite) {
            printd(DEBUG_NVME | DEBUG_DETAILED, "Copying data from user buffer: DMA Buffer=0x%016lx, User Buffer Offset=0x%016lx, Length=%lu\n",
                (uintptr_t)dmaBuffer, userBufferOffset, transferLength);
            memcpy(dmaBuffer, (void*)userBufferOffset, transferLength);
        }

        printd(DEBUG_NVME | DEBUG_DETAILED, "Submitting NVMe %s: LBA=0x%016lx, Blocks=%u, DMA Buffer=0x%016lx\n",
            isWrite ? "write" : "read", currentLBA, blockCount, (uintptr_t)dmaBuffer);
        nvme_submit_command(controller, cmd, false);

        volatile nvme_completion_queue_entry_t* completionEntry =
            (volatile nvme_completion_queue_entry_t*)&controller->cmdCompQueue[controller->cmdCompQueueHeadIndex];
        printd(DEBUG_NVME | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "NVME: Current %s completion queue head index = %u\n",
            isWrite ? "write" : "read", controller->cmdCompQueueHeadIndex);
        nvme_wait_for_completion(controller, false, completionEntry, cmd);

        if (completionEntry->status.status_code || completionEntry->status.status_code_type) {
            log_nvme_debug_info(controller, false, controller->cmdSubQueueTailIndex, controller->cmdCompQueueHeadIndex, 1);
            panic("NVMe %s error. System log contains more information.", isWrite ? "write" : "read");
        }

        controller->cmdCompQueueHeadIndex = (controller->cmdCompQueueHeadIndex + 1) % controller->queueDepth;
        nvme_ring_doorbell(controller, 1, false, controller->cmdCompQueueHeadIndex);

        if (!isWrite) {
            printd(DEBUG_NVME | DEBUG_DETAILED, "Copying data to user buffer: DMA Buffer=0x%016lx, User Buffer Offset=0x%016lx, Length=%lu\n",
                (uintptr_t)dmaBuffer, userBufferOffset, transferLength);
            memcpy((void*)userBufferOffset, dmaBuffer, transferLength);
        }

        nvme_io_unlock(controller, lockFlags);
        // ---- End critical section.

        if (prpCount > 2)
            free_prp_list(cmd->prp2, prpCount - 1);

        userBufferOffset += transferLength;
        currentLBA += blockCount;
        remaining -= transferLength;
    }
}

#ifdef DISK_WRITING_ENABLED
void nvme_write_disk(nvme_controller_t* controller, uint64_t LBA, size_t length, void* buffer) {
    nvme_do_io(controller, LBA, length, buffer, true);
}
#endif

// ── FLUSH CACHE (the shutdown slice, 2026-08-08) ─────────────────────────────
// Commit the DRIVE's volatile write cache to media. The filesystems are
// write-through and the block cache is update-in-place, but both of those
// chains end at the drive's own RAM — Flush is the only command that ends at
// the NAND. Same critical-section dance as nvme_do_io, minus the entire data
// phase: NSID + opcode IS the whole command (NVMe 1.x §6.8 — no PRPs, no
// LBAs; the drive flushes everything for that namespace).
static void nvme_flush_controller(nvme_controller_t* controller)
{
    nvme_submission_queue_entry_t cmdOnStack;
    nvme_submission_queue_entry_t* cmd = &cmdOnStack;
    memset(cmd, 0, sizeof(cmdOnStack));
    cmd->opc  = NVME_OPCODE_FLUSH;
    cmd->nsid = controller->nsid;

    uint64_t lockFlags = nvme_io_lock(controller);
    cmd->cid = controller->cmdCID++;
    nvme_submit_command(controller, cmd, false);

    volatile nvme_completion_queue_entry_t* completionEntry =
        (volatile nvme_completion_queue_entry_t*)&controller->cmdCompQueue[controller->cmdCompQueueHeadIndex];
    nvme_wait_for_completion(controller, false, completionEntry, cmd);

    // A flush error at shutdown is REPORTED, never panicked: the descent
    // must reach "safe to turn off" regardless — a panic here would strand
    // the operator less safe than the flush failure did.
    if (completionEntry->status.status_code || completionEntry->status.status_code_type)
        printd(DEBUG_NVME, "NVME: FLUSH failed (sct=%u sc=%u) — drive cache state unknown\n",
               completionEntry->status.status_code_type, completionEntry->status.status_code);

    controller->cmdCompQueueHeadIndex = (controller->cmdCompQueueHeadIndex + 1) % controller->queueDepth;
    nvme_ring_doorbell(controller, 1, false, controller->cmdCompQueueHeadIndex);
    nvme_io_unlock(controller, lockFlags);
}

// Every NVMe device in the block table gets the order. (AHCI's ATA FLUSH
// CACHE 0xE7 twin rides the AHCI-write DEBTS row — no write path, no cache
// to flush, no customer: the P5 has no SATA controller at all.)
void nvme_flush_all(void)
{
    for (int i = 0; i < kBlockDeviceInfoCount; i++)
        if (kBlockDeviceInfo[i].bus == BUS_NVME && kBlockDeviceInfo[i].block_extra_info != NULL)
            nvme_flush_controller((nvme_controller_t*)kBlockDeviceInfo[i].block_extra_info);
}

void nvme_read_disk(nvme_controller_t* controller, uint64_t LBA, size_t length, void* buffer) {
    nvme_do_io(controller, LBA, length, buffer, false);
}

size_t nvme_vfs_read_disk(block_device_info_t* device, uint64_t sector, void* buffer, uint64_t sector_count)
{
	nvme_controller_t* controller = device->block_extra_info;
	nvme_read_disk(controller, sector, sector_count * controller->blockSize, buffer);
	return 0;
}

size_t nvme_vfs_write_disk(block_device_info_t* device, uint64_t sector, void* buffer, size_t length)
{
	// Stray-write tripwire (block_device.c): a write that isn't fully inside
	// a writable (FAT) partition panics HERE, with the culprit on the stack —
	// earned by the 2026-07-26 root-inode zero-fill. `length` is a sector
	// count at this layer (the byte conversion happens just below).
	block_verify_write_allowed(device, sector, length);

	nvme_controller_t* controller = device->block_extra_info;
	nvme_write_disk(controller, sector, length * controller->blockSize, buffer);
	// 0 = success, matching nvme_vfs_read_disk. This was `void` until the FAT
	// glue started propagating write results (2026-07-18) — the bops cast hid
	// the mismatch, so "success" was whatever RAX happened to hold, and the
	// root-fs disk test failed on the first honest look at it. An NVMe error
	// never reaches this line anyway: nvme_do_io panics on device errors.
	return 0;
}

void init_vfs_block_device(nvme_controller_t* controller, enum eATADeviceType deviceType)
{
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_extra_info = (void*)controller;
	kBlockDeviceInfo[kBlockDeviceInfoCount].ATADeviceType = deviceType;
	kBlockDeviceInfo[kBlockDeviceInfoCount].bus = BUS_NVME;
	kBlockDeviceInfo[kBlockDeviceInfoCount].DeviceAvailable = true;
	kBlockDeviceInfo[kBlockDeviceInfoCount].dmaSupported = true;
	kBlockDeviceInfo[kBlockDeviceInfoCount].driveNo = kBlockDeviceInfoCount;
	kBlockDeviceInfo[kBlockDeviceInfoCount].major = 0x6;
	kBlockDeviceInfo[kBlockDeviceInfoCount].sectorSize = controller->blockSize;
	strncpy(kBlockDeviceInfo[kBlockDeviceInfoCount].ATADeviceModel, controller->deviceName, 40);

	block_device_t* blockDev = kmalloc(sizeof(block_device_t));
	blockDev->name = controller->deviceName;
	block_operations_t* bops = kmalloc(sizeof(block_operations_t));
	bops->read = (void*)nvme_vfs_read_disk;
	bops->write = (void*)nvme_vfs_write_disk;
	blockDev->ops = bops;
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_device = blockDev;
	//add_block_device(controller, &kBlockDeviceInfo[kBlockDeviceInfoCount]);
	kBlockDeviceInfoCount++;

}


void nvme_init_device(pci_device_t* nvmeDevice)
{
	uint64_t baseMemoryAddressMask = 0;
	uint64_t baseMemoryAddress = 0;

	nvme_enable_features(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);

	printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Retrieving PCI config for device at %u:%u:%u\n",nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);
	pci_config_space_t *config = pci_get_config_space(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);

	baseMemoryAddressMask = nvme_get_Base_Memory_Address(nvmeDevice, config);

	uint16_t bar0_size = (~baseMemoryAddressMask) + 1;

	baseMemoryAddress = ((uint64_t)(bar0InitialValue & 0xfffffff0) | ((uint64_t)bar1InitialValue << 32));

	if (baseMemoryAddress < kAvailableMemory && bar0InitialValue > 0xA0000000 )
	{
		baseMemoryAddress = ((uint64_t)bar0InitialValue | ((uint64_t)bar1InitialValue << 32)) & ~(0xf);
		printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Initial base memory address is valid.  We'll use it! (0x%016lx)\n", baseMemoryAddress);
	}
	else
	{
		uint64_t temp = nvmeBaseAddressRemap;
		nvmeBaseAddressRemap += bar0_size;
		printd(DEBUG_NVME | DEBUG_DETAILED, "NVME: Initial base memory address (0x%016lx) is outside physical memory.  Using 0x%016lx instead\n",baseMemoryAddress,temp);
		baseMemoryAddress = temp;
		printd(DEBUG_NVME | DEBUG_DETAILED, "Initializing base address 0x%08x to Bar[0], and 0x0 to BAR[1]\n",baseMemoryAddress);
		writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET, baseMemoryAddress & 0xFFFFFFFF);
		wait(50);
		writePCIRegister(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo, PCI_BAR0_OFFSET + 4, 0);
		wait(50);
		config = pci_get_config_space(nvmeDevice->busNo, nvmeDevice->deviceNo, nvmeDevice->funcNo);
	}

	print_BARs(config, "post config");

	nvme_controller_t* controller = kmalloc(sizeof(nvme_controller_t));
	printd(DEBUG_NVME | DEBUG_DETAILED, "Allocated controller_t at 0x%016lx\n",(uintptr_t)controller);
	controller->nvmePCIDevice = nvmeDevice;
	controller->mmioAddress = baseMemoryAddress;
	controller->mmioSize = bar0_size;
	controller->adminCID = controller->cmdCID = 0;
	controller->cmdQID = 1;

	// The register window lives at its HHDM alias — an UPPER-half VA, which
	// every task's PML4 shares with the kernel — for the same reason the NIC,
	// xHCI and virtio windows do: a doorbell is rung from whatever page
	// tables happen to be live, and the ELF loader opens a program (ext2
	// resolving its path, reading its inode) under the SPAWNING task's CR3.
	// A lower-half identity mapping exists in kKernelPML4 alone, so under any
	// other CR3 the first doorbell after a block-cache miss is a kernel #PF —
	// which is how the P5 died on 2026-09-06 (NOCACHE, then a 176MB write
	// evicting the root's inode lines, then a spawn from the late-tests
	// thread). PAGE_PCD: device registers are never cached.
	paging_map_pages((pt_entry_t*)kKernelPML4v, kHHDMOffset + controller->mmioAddress,
	                 controller->mmioAddress, bar0_size / PAGE_SIZE,
	                 PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	controller->registers = (volatile nvme_controller_regs_t*)(kHHDMOffset + controller->mmioAddress);
	printd(DEBUG_NVME | DEBUG_DETAILED,"NVME: MMIO window phys 0x%016lx mapped at its HHDM alias %p\n",
	       controller->mmioAddress, controller->registers);

	nvme_print_version(controller->registers->version);
	nvme_extract_cap(controller);
	nvme_init_admin_queues(controller);
	nvme_initialize_controller(controller);
	nvme_identify(controller);
	nvme_set_features(controller);
	nvme_init_cmd_queues(controller);
	printd(DEBUG_NVME,"Performing a test read ... \n");
	printd(DEBUG_NVME | DEBUG_DETAILED, "Initializing a buffer of 0x%08x bytes for the test read\n");
	char* buffer = kmalloc(controller->blockSize);
	printd(DEBUG_NVME | DEBUG_DETAILED, "Calling nvme_read_disk\n");
	nvme_read_disk(controller, 0, controller->blockSize, buffer);
	init_vfs_block_device(controller, ATA_DEVICE_TYPE_NVME_HD);
	kfree(buffer);
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

// WATCHDMA's second half: arm a hardware watchpoint on EVERY level of the page
// table chain that maps the write DMA bounce buffer. Called from kernel_init
// AFTER the post-boot tests, because boot itself legitimately rewrites the
// upper levels (ktask's argv mapping ORs PAGE_USER into PML4[0]) and a halting
// watchpoint armed before that stops the machine for a lawful change.
//
// After boot those entries are stable: paging_map_page only writes an entry
// when the value actually CHANGES (2026-08-14), so every remaining write to
// one of these eight-byte slots is, by construction, something new.
void nvme_watch_dma_chain(void)
{
	if (!kWatchDMA || kNvmeWatchTarget == NULL)
		return;

	static const char *levelName[4] = {
		"DMA buffer's PML4 entry", "DMA buffer's PDPT entry",
		"DMA buffer's PD entry",   "DMA buffer's PT entry" };

	uintptr_t entries[4];
	int levels = paging_walk_entry_addresses((pt_entry_t *)kKernelPML4v,
	                                         (uintptr_t)kNvmeWatchTarget, entries);
	printf("WATCHDMA: watching the whole chain for DMA buffer 0x%016lx\n",
	       (uintptr_t)kNvmeWatchTarget);
	for (int i = 0; i < levels; i++)
		if (entries[i] != 0)
			watchpoint_arm(entries[i], 8, WATCH_WRITE, WATCH_HALT, levelName[i]);
}
