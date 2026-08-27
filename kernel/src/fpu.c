#include "fpu.h"
#include "panic.h"
#include <cpuid.h>

#define CR0_MP (1UL << 1)
#define CR0_EM (1UL << 2)
#define CR0_TS (1UL << 3)
#define CR0_NE (1UL << 5)
#define CR4_OSFXSR (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)
#define FPU_DEFAULT_MXCSR 0x1F80U
#define FPU_MXCSR_FALLBACK_MASK 0xFFBFU
#define FXSAVE_MXCSR_OFFSET 24
#define FXSAVE_MXCSR_MASK_OFFSET 28

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

void fpu_init_this_cpu(void)
{
	uint32_t eax, ebx, ecx, edx;
	if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) ||
	    (edx & ((1U << 0) | (1U << 24) | (1U << 25) | (1U << 26))) !=
	        ((1U << 0) | (1U << 24) | (1U << 25) | (1U << 26)))
		panic("FPU: CPU lacks required x87/FXSAVE/SSE/SSE2 support\n");

	uint64_t cr0, cr4;
	__asm__ volatile("mov %0, cr0" : "=r"(cr0));
	cr0 |= CR0_MP | CR0_NE;
	cr0 &= ~(CR0_EM | CR0_TS);
	__asm__ volatile("mov cr0, %0" : : "r"(cr0) : "memory");
	__asm__ volatile("mov %0, cr4" : "=r"(cr4));
	cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
	__asm__ volatile("mov cr4, %0" : : "r"(cr4) : "memory");
	__asm__ volatile("fninit\n\tldmxcsr %0" : : "m"((const uint32_t){ FPU_DEFAULT_MXCSR }) : "memory");
}

void fpu_save(fpu_state_t *state)
{
	__asm__ volatile("fxsave %0" : "=m"(*state) : : "memory");
}

bool fpu_restore(fpu_state_t *state)
{
	uint32_t mask = fpu_state_u32(state, FXSAVE_MXCSR_MASK_OFFSET);
	if (mask == 0)
		mask = FPU_MXCSR_FALLBACK_MASK;
	fpu_state_set_u32(state, FXSAVE_MXCSR_OFFSET,
	                  fpu_state_u32(state, FXSAVE_MXCSR_OFFSET) & mask);
	__asm__ volatile("fxrstor %0" : : "m"(*state) : "memory");
	return true;
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
	uint32_t supported_mxcsr = fpu_state_u32(destination, FXSAVE_MXCSR_MASK_OFFSET);
	if (supported_mxcsr == 0)
		supported_mxcsr = FPU_MXCSR_FALLBACK_MASK;
	for (uint32_t i = 0; i < FPU_FXSAVE_SIZE; i++)
		destination->bytes[i] = source->bytes[i];
	// MXCSR_MASK is CPU capability data, not a user-controlled part of the
	// interrupted state. Keep the CPU-supplied mask before restoring a frame.
	fpu_state_set_u32(destination, FXSAVE_MXCSR_MASK_OFFSET, supported_mxcsr);
	return fpu_restore(destination);
}
