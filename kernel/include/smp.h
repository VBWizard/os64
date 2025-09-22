#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "mpspec_def.h"
#include "thread.h"
#include "tss.h"
#include "task.h"

#define MAX_CPUS 24
#define APIC_EOI_OFFSET    0xB0
#define AP_STACK_BASE 0x500000
#define AP_STACK_SIZE 0x1000

typedef enum mpRecType
{
    CPU=0,
    BUS,
    IOAPIC,
    IOINTASS,
    LOCALINTASS
} eMPRecType;

typedef struct
{
    int apicID;
    //Virtual register base address
	uint64_t registerBase;
	//IO APIC address
	uint64_t ioAPICAddress;
	uint64_t apic_lvt_timer, apic_lvt_lint0, apic_lvt_lint1, apic_lvt_error, apic_tpr;
	uint64_t apic_id_reg, apic_svr, apic_eoi, apic_icr_low, apic_icr_high;
    uint64_t ticksPerSecond;
	//Put an address in this field and the CPU will jump out of park, to it
	void *goto_address;
} cpu_t;

typedef struct mpConfig
{
    union 
    {unsigned char rec[20];
    struct mpc_cpu cpu;
    struct mpc_bus bus;
    struct mpc_ioapic apic;
    struct mpc_intsrc irqSrc;
    struct mpc_lintsrc lintSrc;
    };
    eMPRecType recType;
    uintptr_t prevRecAddress;
    uintptr_t nextRecAddress;
    
} __attribute__((packed))mpConfig_t;

typedef struct 
{
	void *self;										// 0
	uint64_t apic_id;								// 8
	uint64_t threadID;								// 16
	uint64_t apicTicksPerSecond;					// 24
	uint64_t apicTimerCount;
	uintptr_t stackVirtualAddress, stackPhysicalAddress;
	thread_t *currentThread;
	bool coreAwoken, coreInitialized;
	tss_t *tss;
	uint64_t kernel_rsp0;
    task_t *task;
} core_local_storage_t;


extern cpu_t *kCPUInfo;
extern volatile uintptr_t kMPApicBase;
extern int kLocalAPICTimerSpeed[MAX_CPUS];
extern volatile core_local_storage_t* kCoreLocalStorage;
extern volatile uint8_t kMPCoreCount;
extern uintptr_t kMPICRLow;
extern uintptr_t kMPICRHigh;
extern uint64_t kMPIdReg;
extern uintptr_t kIOAPICAddress;
extern mpConfig_t* kMPConfigTable;
extern uint32_t kMPConfigTableCount;

int init_SMP(bool enableSMP);

#endif
