#include <stddef.h>

#include "tss.h"
#include "gdt.h"
#include "CONFIG.h"
#include "smp.h"
#include "smp_core.h"
#include "memory/kmalloc.h"   // kmalloc_aligned — the per-core IST stack
#include "serial_logging.h"   // printd

// One page per core for the double-fault stack. A #DF handler's job is to
// print and stop, not to recurse — if it ever needs more than 4KB, the
// handler is doing too much.
#define IST_STACK_SIZE PAGE_SIZE

// Pre-computed stack TOPS, one per core, filled once by the BSP. Kept as a
// plain array (not allocated per core inside tss_initialize_cpu) because
// that function runs on an AP's 1 KB bootstrap stack — see the note at its
// use. Zero means "no IST stack for this core", which the CPU treats as a
// gate with no stack switch: exactly as fragile as before, never worse.
static uint64_t kIstStackTop[MAX_CPUS];

// ── Why exception handlers need a stack of their own ────────────────────────
//
// Every handler used to run on whatever RSP the faulting code had. That is
// fine right up until the fault IS the stack — and then it is fatal by
// construction: the CPU cannot push an exception frame, so #PF becomes #DF
// becomes triple fault, the machine vanishes, and nothing is ever printed.
//
// The Interrupt Stack Table is the hardware's answer: a gate that names an
// IST slot makes the CPU switch to THAT stack on delivery, no matter how
// broken RSP is. #DF is its textbook customer (Linux puts #DF, NMI and #MC
// there for the same reason). Per core, deliberately — two cores double-
// faulting at once must not land on the same page.
//
// Called ONCE by the BSP, after the allocator is up and on a real kernel
// stack. kmalloc_aligned returns a zeroed, page-aligned HHDM address, which
// lives in the shared upper half and therefore resolves under every task's
// CR3 — a property a fault stack absolutely requires.
void tss_init_ist_stacks(void)
{
	for (uint32_t i = 0; i < MAX_CPUS; i++)
	{
		void *p = kmalloc_aligned(IST_STACK_SIZE);
		// Stacks grow DOWN: point at the top, 16-byte aligned.
		kIstStackTop[i] = p ? (((uint64_t)p + IST_STACK_SIZE - 16) & ~0xFULL) : 0;
	}
	printd(DEBUG_BOOT, "tss: allocated %u double-fault (IST1) stacks, CPU0's at 0x%016lx\n",
	       (uint32_t)MAX_CPUS, kIstStackTop[0]);
}

extern uintptr_t kKernelStack;

static tss_t kTSSPerCPU[MAX_CPUS];
static uint16_t kTSSSelector[MAX_CPUS];

static inline int tss_descriptor_index(uint32_t cpu_index)
{
    return GDT_FIRST_TSS_ENTRY + (cpu_index * 2);
}

static void tss_install_descriptor(uint32_t cpu_index, tss_t *tss)
{
    uint64_t base = (uint64_t)tss;
    uint32_t limit = sizeof(tss_t) - 1;
    int descriptor = tss_descriptor_index(cpu_index);
    // GDT_S_SYSTEM_SEGMENT: a 64-bit TSS descriptor is 16 bytes — the upper
    // half of the base lands in GDT entry descriptor+1 (reserved for it by
    // tss_descriptor_index's 2-entry stride).
    set_gdt_entry(kGDT, descriptor, base, limit, GDT_ACCESS_TSS, 0x00, GDT_S_SYSTEM_SEGMENT);
    kTSSSelector[cpu_index] = (uint16_t)(descriptor << 3);
}

void tss_initialize_cpu(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS)
    {
        return;
    }

    tss_t *tss = &kTSSPerCPU[cpu_index];
    *tss = (tss_t){0};
    tss->iomap_base = sizeof(tss_t);
    tss->rsp0 = kKernelStack + KERNEL_STACK_SIZE - 8;

    // IST1: this core's double-fault stack, ALREADY ALLOCATED (see
    // tss_init_ist_stacks). Assignment only — no allocation, no logging,
    // nothing that needs a stack. THIS FUNCTION RUNS ON AN AP'S 1 KB
    // BOOTSTRAP STACK (smp_core.c hands it `tempStack + 1024 - 8` and calls
    // straight into here), which is why the first version of this code —
    // a kmalloc_aligned plus a printd whose format buffer alone is 2 KB —
    // smashed straight past the bottom of that stack and scribbled on
    // kernel memory. It corrupted neighbouring globals so effectively that
    // it produced garbage scheduler stacks and wild segment selectors on
    // unrelated cores, which is a very convincing impression of a
    // mysterious pre-existing bug. Chris caught it in one move: stash the
    // uncommitted work, 5 clean boots; restore it, 3 for 3 broken.
    //
    // The rule this leaves: anything running before init_core_local_storage
    // on an AP gets a 1 KB budget and no library calls.
    tss->ist1 = kIstStackTop[cpu_index];

    tss_install_descriptor(cpu_index, tss);

    tss_set_rsp0(cpu_index, tss->rsp0);

    asm volatile ("ltr %0" :: "r"(kTSSSelector[cpu_index]) : "memory");
}

void tss_set_rsp0(uint32_t cpu_index, uint64_t rsp0)
{
    if (cpu_index >= MAX_CPUS)
    {
        return;
    }

    kTSSPerCPU[cpu_index].rsp0 = rsp0;

    volatile core_local_storage_t *cls_base = kCoreLocalStorage;
    if (cls_base)
    {
        core_local_storage_t *cls = get_core_local_storage_for_core(cpu_index);
        if (cls)
        {
            cls->kernel_rsp0 = rsp0;
        }
    }
}

tss_t* tss_get_for_cpu(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS)
    {
        return NULL;
    }

    return &kTSSPerCPU[cpu_index];
}

uint16_t tss_selector_for_cpu(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS)
    {
        return 0;
    }

    return kTSSSelector[cpu_index];
}
