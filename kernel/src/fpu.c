// The floating-point register file as thread context — see fpu.h for the
// design (eager FXSAVE per switch; XSAVE/AVX deliberately off).

#include "fpu.h"
#include "panic.h"
#include "printd.h"
#include "sprintf.h"
#include "CONFIG.h"
#include "BasicRenderer.h"   // printf — the glass
#include <cpuid.h>

#define CR0_MP (1UL << 1)
#define CR0_EM (1UL << 2)
#define CR0_TS (1UL << 3)
#define CR0_NE (1UL << 5)
#define CR4_OSFXSR (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)

// CPUID.1:EDX — the long-mode baseline every x86-64 CPU carries.
#define CPUID1_EDX_FPU   (1U << 0)
#define CPUID1_EDX_FXSR  (1U << 24)
#define CPUID1_EDX_SSE   (1U << 25)
#define CPUID1_EDX_SSE2  (1U << 26)
// CPUID.1:ECX — what FXSAVE still covers (the SSE3..4.2 family adds
// instructions, not registers) and what it does not (XSAVE, AVX).
#define CPUID1_ECX_SSE3  (1U << 0)
#define CPUID1_ECX_SSSE3 (1U << 9)
#define CPUID1_ECX_SSE41 (1U << 19)
#define CPUID1_ECX_SSE42 (1U << 20)
#define CPUID1_ECX_XSAVE (1U << 26)
#define CPUID1_ECX_AVX   (1U << 28)
#define CPUID1_ECX_FMA3  (1U << 12)
#define CPUID1_ECX_F16C  (1U << 29)
#define CPUID1_EDX_MMX   (1U << 23)
// CPUID.7.0:EBX
#define CPUID7_EBX_AVX2  (1U << 5)
// CPUID.0x80000001:ECX — AMD's extended leaf; SSE4A lives only here
#define CPUID81_ECX_SSE4A (1U << 6)

// MXCSR reset value: every exception masked, round-to-nearest, flush-to-zero
// off. The x87 control word's twin is 0x037F (below, in fpu_state_init).
#define FPU_DEFAULT_MXCSR 0x1F80U
// The MXCSR bits every SSE CPU implements. FXSAVE writes the CPU's real
// mask into the image (offset 28); a ZERO there means "assume this" —
// Intel SDM vol. 1 §11.6.6.
#define FPU_MXCSR_FALLBACK_MASK 0xFFBFU
#define FXSAVE_MXCSR_OFFSET 24
#define FXSAVE_MXCSR_MASK_OFFSET 28

fpu_features_t kFPUFeatures;

static uint32_t fpu_state_u32(const fpu_state_t *state, uint32_t offset)
{
	const uint8_t *p = &state->bytes[offset];
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fpu_state_set_u32(fpu_state_t *state, uint32_t offset, uint32_t value)
{
	uint8_t *p = &state->bytes[offset];
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

// The mask FXSAVE reported, or the architectural fallback when it reported
// none. Restoring an MXCSR with a reserved bit set is a #GP — in the KERNEL,
// on an fxrstor — so every restore masks first.
static uint32_t fpu_state_mxcsr_mask(const fpu_state_t *state)
{
	uint32_t mask = fpu_state_u32(state, FXSAVE_MXCSR_MASK_OFFSET);
	return mask ? mask : FPU_MXCSR_FALLBACK_MASK;
}

void fpu_init_this_cpu(void)
{
	uint32_t eax, ebx, ecx, edx;
	const uint32_t baseline = CPUID1_EDX_FPU | CPUID1_EDX_FXSR | CPUID1_EDX_SSE | CPUID1_EDX_SSE2;
	if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || (edx & baseline) != baseline)
		panic("FPU: CPU lacks required x87/FXSAVE/SSE/SSE2 support\n");

	// Every core fills this; every core gets the same answers on any machine
	// this OS runs on, so the last writer is as good as the first.
	kFPUFeatures.sse3  = (ecx & CPUID1_ECX_SSE3)  != 0;
	kFPUFeatures.ssse3 = (ecx & CPUID1_ECX_SSSE3) != 0;
	kFPUFeatures.sse41 = (ecx & CPUID1_ECX_SSE41) != 0;
	kFPUFeatures.sse42 = (ecx & CPUID1_ECX_SSE42) != 0;
	kFPUFeatures.mmx   = (edx & CPUID1_EDX_MMX)   != 0;
	kFPUFeatures.xsave = (ecx & CPUID1_ECX_XSAVE) != 0;
	kFPUFeatures.avx   = (ecx & CPUID1_ECX_AVX)   != 0;
	kFPUFeatures.fma3  = (ecx & CPUID1_ECX_FMA3)  != 0;
	kFPUFeatures.f16c  = (ecx & CPUID1_ECX_F16C)  != 0;
	uint32_t eax7, ebx7, ecx7, edx7;
	kFPUFeatures.avx2 = __get_cpuid_count(7, 0, &eax7, &ebx7, &ecx7, &edx7) &&
	                    (ebx7 & CPUID7_EBX_AVX2) != 0;
	uint32_t eax81, ebx81, ecx81, edx81;
	kFPUFeatures.sse4a = __get_cpuid(0x80000001, &eax81, &ebx81, &ecx81, &edx81) &&
	                     (ecx81 & CPUID81_ECX_SSE4A) != 0;

	// CR0: MP so WAIT/FWAIT honours TS (which we never set — but the bit
	// pair is defined together); NE so x87 errors arrive as #MF (vector 16)
	// rather than the PC/AT's IRQ13 detour; EM and TS clear so nothing traps.
	// CR4: OSFXSR licenses FXSAVE/FXRSTOR and the SSE instructions themselves;
	// OSXMMEXCPT routes unmasked SSE exceptions to #XM (vector 19) instead of
	// #UD. OSXSAVE stays clear ON PURPOSE (fpu.h).
	uint64_t cr0, cr4;
	__asm__ volatile("mov %0, cr0" : "=r"(cr0));
	cr0 |= CR0_MP | CR0_NE;
	cr0 &= ~(CR0_EM | CR0_TS);
	__asm__ volatile("mov cr0, %0" : : "r"(cr0) : "memory");
	__asm__ volatile("mov %0, cr4" : "=r"(cr4));
	cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
	__asm__ volatile("mov cr4, %0" : : "r"(cr4) : "memory");
	__asm__ volatile("fninit\n\tldmxcsr %0" : : "m"((const uint32_t){ FPU_DEFAULT_MXCSR }) : "memory");

	// Ask the hardware which MXCSR bits it implements — the only way to learn
	// it is to FXSAVE and read the field back. Static, not a local: the
	// early-boot stack is not guaranteed 16-aligned and a misaligned FXSAVE
	// is a #GP. Cores may race into it; the one field read back is CPU
	// capability data, identical from every core.
	static fpu_state_t probe;
	fpu_save(&probe);
	kFPUFeatures.mxcsr_mask = fpu_state_mxcsr_mask(&probe);
}

void fpu_report(void)
{
	// Glass AND log: printf reaches only the framebuffer, printd only the
	// serial pipeline. A boot line about whether the FPU is on belongs in
	// both places — it is the first thing to check when a float program dies.
	char line[200];
	snprintf(line, sizeof(line),
	         "FPU: x87%s SSE SSE2%s%s%s%s%s enabled, FXSAVE per thread (MXCSR mask 0x%04X); %s%s%s%s%s%s\n",
	         kFPUFeatures.mmx   ? " MMX"    : "",
	         kFPUFeatures.sse3  ? " SSE3"   : "",
	         kFPUFeatures.ssse3 ? " SSSE3"  : "",
	         kFPUFeatures.sse41 ? " SSE4.1" : "",
	         kFPUFeatures.sse42 ? " SSE4.2" : "",
	         kFPUFeatures.sse4a ? " SSE4A"  : "",
	         kFPUFeatures.mxcsr_mask,
	         kFPUFeatures.xsave ? "XSAVE" : "no XSAVE",
	         kFPUFeatures.avx   ? " AVX"   : "",
	         kFPUFeatures.avx2  ? " AVX2"  : "",
	         kFPUFeatures.fma3  ? " FMA3"  : "",
	         kFPUFeatures.f16c  ? " F16C"  : "",
	         kFPUFeatures.xsave ? " present, not enabled" : "");
	printf("%s", line);
	printd(DEBUG_BOOT, "%s", line);
}

void fpu_save(fpu_state_t *state)
{
	// The REX.W form: FIP and FDP are saved as full 64-bit addresses. The
	// plain form keeps only their low 32 bits (plus selectors that mean
	// nothing in long mode), which truncates the x87 exception pointers for
	// any code above 4GB — the shared-library window is at 0x7F0000000000.
	__asm__ volatile("fxsave64 %0" : "=m"(*state) : : "memory");
}

void fpu_restore(fpu_state_t *state)
{
	fpu_state_set_u32(state, FXSAVE_MXCSR_OFFSET,
	                  fpu_state_u32(state, FXSAVE_MXCSR_OFFSET) & fpu_state_mxcsr_mask(state));
	__asm__ volatile("fxrstor64 %0" : : "m"(*state) : "memory");
}

void fpu_state_init(fpu_state_t *state)
{
	for (uint32_t i = 0; i < FPU_FXSAVE_SIZE; i++)
		state->bytes[i] = 0;
	// The architectural reset image: all x87 registers empty, the x87 control
	// word masks exceptions and rounds to nearest, and MXCSR has its SSE reset
	// value. Constructing it does not disturb the state of the thread running
	// task creation.
	state->bytes[0] = 0x7F;
	state->bytes[1] = 0x03;
	fpu_state_set_u32(state, FXSAVE_MXCSR_OFFSET, FPU_DEFAULT_MXCSR);
}

bool fpu_state_from_user(fpu_state_t *destination, const fpu_state_t *source)
{
	uint32_t supported_mxcsr = fpu_state_mxcsr_mask(destination);
	for (uint32_t i = 0; i < FPU_FXSAVE_SIZE; i++)
		destination->bytes[i] = source->bytes[i];
	// MXCSR_MASK is CPU capability data, not a user-controlled part of the
	// interrupted state. Keep the CPU-supplied mask before restoring a frame.
	fpu_state_set_u32(destination, FXSAVE_MXCSR_MASK_OFFSET, supported_mxcsr);
	fpu_restore(destination);
	return true;
}
