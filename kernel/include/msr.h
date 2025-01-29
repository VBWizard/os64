#ifndef MSR_H
#define MSR_H

#include <stdint.h>

#define STAR_MSR 0xC0000081
#define LSTAR_MSR 0xC0000082
#define CSTAR_MSR 0xC0000083
#define SFMASK_MSR 0xC0000084

uint64_t rdmsr64(unsigned index);
void wrmsr64(unsigned index, uint64_t val);
void rdmsr32(unsigned index,uint32_t* loVal, uint32_t* hiVal);
void wrmsr32(unsigned index, uint32_t loVal, uint32_t hiVal);


#endif
