#ifndef MSR_H
#define MSR_H

#include <stdint.h>

#define EFER_MSR 0xC0000080
#define STAR_MSR 0xC0000081
#define LSTAR_MSR 0xC0000082
#define CSTAR_MSR 0xC0000083
#define SFMASK_MSR 0xC0000084

// EFER bits os64 cares about
#define EFER_SCE (1ULL << 0)   // SYSCALL/SYSRET enable — without it, `syscall` is #UD
#define EFER_NXE (1ULL << 11)  // No-Execute enable — makes PTE bit 63 mean NX
                               // instead of RESERVED (a stray NX bit under
                               // NXE=0 is a reserved-bit #PF on next access)

uint64_t rdmsr64(unsigned index);
void wrmsr64(unsigned index, uint64_t val);
void rdmsr32(unsigned index,uint32_t* loVal, uint32_t* hiVal);
void wrmsr32(unsigned index, uint32_t loVal, uint32_t hiVal);


#endif
