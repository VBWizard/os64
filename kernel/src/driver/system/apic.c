#include "apic.h"
#include "cpu.h"
#include "x86_64.h"

bool check_for_apic() {
   uint32_t eax=0, edx=0, notused=0;
   eax=1;
   cpuid(&eax, &notused, &notused, &edx);
   return edx & CPUID_FLAG_APIC;
}