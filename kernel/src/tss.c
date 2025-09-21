#include <stddef.h>

#include "tss.h"
#include "gdt.h"
#include "CONFIG.h"
#include "smp.h"
#include "smp_core.h"

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
    set_gdt_entry(kGDT, descriptor, base, limit, GDT_ACCESS_TSS, 0x00, 1);
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
