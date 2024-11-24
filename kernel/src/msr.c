#include "msr.h"

uint64_t rdmsr64(unsigned index) {
    uint32_t low, high;

    __asm__ volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)     // Outputs: low in EAX, high in EDX
        : "c"(index)               // Input: index in ECX
    );

    return ((uint64_t)high << 32) | low;  // Combine high and low into a 64-bit result
}


 void rdmsr32(unsigned index,uint32_t* loVal, uint32_t* hiVal)
 {
     unsigned long long lTemp = rdmsr64(index);
     *hiVal = lTemp >> 32;
     *loVal = lTemp & 0xFFFFFFFF;
 }

void wrmsr64(unsigned index, uint64_t val) {
    uint32_t low = (uint32_t)(val & 0xFFFFFFFF);    // Extract low 32 bits
    uint32_t high = (uint32_t)(val >> 32);         // Extract high 32 bits

    __asm__ volatile (
        "wrmsr"
        :
        : "c"(index), "a"(low), "d"(high)          // Pass inputs
    );
}


void wrmsr32(unsigned index, uint32_t loVal, uint32_t hiVal)
{
    //NOTE: hiVal=EDX, loVal=EAX
    wrmsr64(index, ((uint64_t)hiVal << 32) | loVal);
 
}
