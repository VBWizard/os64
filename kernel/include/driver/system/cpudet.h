#ifndef CPUDET_H
#define CPUDET_H

#include <stdint.h>

typedef struct
    {
        union {
          struct {
            uint8_t fpu: 1;
            uint8_t vme: 1;
            uint8_t de: 1;
            uint8_t pse: 1;
            uint8_t tsc: 1;
            uint8_t msr: 1;
            uint8_t pae: 1;
            uint8_t mce: 1;
            uint8_t cx8: 1;
            uint8_t apic: 1;
            uint8_t reserved1: 1;
            uint8_t sep: 1;
            uint8_t mtrr: 1;
            uint8_t pge: 1;
            uint8_t mca: 1;
            uint8_t cmov: 1;
            uint8_t pat: 1;
            uint8_t pse36: 1;
            uint8_t psn: 1;
            uint8_t clfsh: 1;
            uint8_t reserved2: 1;
            uint8_t ds: 1;
            uint8_t acpi: 1;
            uint8_t mmx: 1;
            uint8_t fxsr: 1;
            uint8_t sse: 1;
            uint8_t sse2: 1;
            uint8_t ss: 1;
            uint8_t htt: 1;
            uint8_t tm: 1;
            uint8_t ia64: 1;
            uint8_t pbe: 1;
          };
          uint32_t cpuid_features_edx_reg;
        } cpuid_feature_bits;

        union {
          struct {
            uint32_t sse3: 1;
            uint32_t pclmulqdq: 1;
            uint32_t dtes64: 1;
            uint32_t monitor: 1;
            uint32_t dscpl: 1;
            uint32_t vmx: 1;
            uint32_t smx: 1;
            uint32_t est: 1;
            uint32_t tm2: 1;
            uint32_t ssse3: 1;
            uint32_t cntxid: 1;
            uint32_t sdbg: 1;
            uint32_t fma: 1;
            uint32_t cx16: 1;
            uint32_t xtpr: 1;
            uint32_t pdcm: 1;
            uint32_t reserved1: 1;
            uint32_t pcid: 1;
            uint32_t dca: 1;
            uint32_t sse41: 1;
            uint32_t sse42: 1;
            uint32_t x2apic: 1;
            uint32_t movbe: 1;
            uint32_t popcnt: 1;
            uint32_t tscdadline: 1;
            uint32_t aes: 1;
            uint32_t xsave: 1;
            uint32_t osxsave: 1;
            uint32_t avx: 1;
            uint32_t f16c: 1;
            uint32_t rdrnd: 1;
            uint32_t hypervisor: 1;      
          };
          uint32_t cpuid_feature_bits_ecx_reg;
        } cpuid_feature_bits_2;

        union {
          struct {
            uint8_t fsgsbase: 1;
            uint8_t ia32tscadjust: 1;
            uint8_t sgx: 1;
            uint8_t bmi1: 1;
            uint8_t hle: 1;
            uint8_t avx2: 1;
            uint8_t reserved1: 1;
            uint8_t smep: 1;
            uint8_t bmi2: 1;
            uint8_t erms: 1;
            uint8_t invpcid: 1;
            uint8_t rtm: 1;
            uint8_t pqm: 1;
            uint8_t fpucsdsdeprecated: 1;
            uint8_t mpx: 1;
            uint8_t pqe: 1;
            uint8_t avx512f: 1;
            uint8_t avx512dq: 1;
            uint8_t rdseed: 1;
            uint8_t adx: 1;
            uint8_t smap: 1;
            uint8_t avx512ifma: 1;
            uint8_t pcommit: 1;
            uint8_t clflushopt: 1;
            uint8_t clwb: 1;
            uint8_t intelproctrace: 1;
            uint8_t avx512pf: 1;
            uint8_t avx512er: 1;
            uint8_t avx512cd: 1;
            uint8_t sha: 1;
            uint8_t avx512bw: 1;
            uint8_t avx512vl: 1;
          };
          uint32_t cpuid_extended_feature_bits_ebx_reg;
        } cpuid_extended_feature_bits_3;
    } cpuid_features_t;

typedef struct
{
    int family, model, stepping, type, brand, extended_family;
    char vendor[50], model_name[50], detected_processor_name[50], type_name[50], family_name[50], brand_name[50];
    
} cpuinfo_t;

int detect_cpu(void);
extern cpuinfo_t kcpuInfo;

#endif
