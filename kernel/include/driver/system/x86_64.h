#ifndef x86_64_H
#define x86_64_H
#include "stdint.h"

	void cpuid(uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);
	void write_apic_register(uintptr_t reg, uint32_t value);
	uint32_t read_apic_register(uintptr_t reg);
	uint32_t read_apic_id();
	uint64_t rdtsc();
	uint64_t getCR3();
	int tscGetCyclesPerSecond();
#endif
