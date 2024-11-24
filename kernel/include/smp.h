#ifndef SMP_H
#define SMP_H

#include <stdint.h>

typedef struct
{
    int apicID;
    //Virtual register base address
	uint64_t registerBase;
    uint64_t ticksPerSecond;
	//Put an address in this field and the CPU will jump out of park, to it
	void *goto_address;
} cpu_t;

extern cpu_t *kCPUInfo;

void init_SMP();

#endif
