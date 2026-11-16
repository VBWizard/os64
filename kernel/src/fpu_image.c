// The register image's rules — see fpu_image.h. Pure: no kernel headers,
// driven on the host by tools/test_fpu_image_host.sh.

#include "fpu_image.h"

static uint32_t image_u32(const uint8_t *image, uint32_t offset)
{
	const uint8_t *p = &image[offset];
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void image_set_u32(uint8_t *image, uint32_t offset, uint32_t value)
{
	uint8_t *p = &image[offset];
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

fpu_why_t fpu_mode_before_layout(const fpu_cpu_answers_t *a)
{
	if (!a->xsave)
		return FPU_WHY_NO_XSAVE;
	if (!a->avx)
		return FPU_WHY_NO_AVX;
	if (a->noavx)
		return FPU_WHY_NOAVX;
	return FPU_WHY_ON;
}

fpu_why_t fpu_mode_components(const fpu_cpu_answers_t *a)
{
	// Writing an XCR0 bit the CPU does not support is a #GP, so this is
	// asked before xsetbv, not after.
	if ((a->components & FPU_XCR0) != FPU_XCR0)
		return FPU_WHY_COMPONENTS;
	return FPU_WHY_ON;
}

fpu_why_t fpu_mode_layout(const fpu_cpu_answers_t *a)
{
	if (a->size != FPU_IMAGE_SIZE)
		return FPU_WHY_SIZE;
	if (a->avx_offset != FPU_IMAGE_AVX_OFFSET || a->avx_size != FPU_IMAGE_AVX_SIZE)
		return FPU_WHY_AVX_PLACE;
	return FPU_WHY_ON;
}

void fpu_image_init(uint8_t image[FPU_IMAGE_SIZE])
{
	for (uint32_t i = 0; i < FPU_IMAGE_SIZE; i++)
		image[i] = 0;
	// All x87 registers empty (the abridged tag word is zero), the control
	// word and MXCSR at reset. Building it does not disturb the live
	// registers of whichever thread is creating this one.
	image[FPU_IMAGE_FCW_OFFSET]     = (uint8_t)FPU_IMAGE_RESET_FCW;
	image[FPU_IMAGE_FCW_OFFSET + 1] = (uint8_t)(FPU_IMAGE_RESET_FCW >> 8);
	image_set_u32(image, FPU_IMAGE_MXCSR_OFFSET, FPU_IMAGE_RESET_MXCSR);
	image[FPU_IMAGE_HEADER_OFFSET] = (uint8_t)(FPU_XCR0_X87 | FPU_XCR0_SSE);
}

void fpu_image_sanitize(uint8_t image[FPU_IMAGE_SIZE], uint32_t mxcsr_mask)
{
	// XSTATE_BV is a little-endian 64-bit word whose only legal bits are
	// FPU_XCR0's, all in its first byte.
	image[FPU_IMAGE_HEADER_OFFSET] &= (uint8_t)FPU_XCR0;
	for (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 1; i < FPU_IMAGE_AVX_OFFSET; i++)
		image[i] = 0;
	if (mxcsr_mask == 0)
		mxcsr_mask = FPU_MXCSR_FALLBACK_MASK;
	image_set_u32(image, FPU_IMAGE_MXCSR_OFFSET,
	              image_u32(image, FPU_IMAGE_MXCSR_OFFSET) & mxcsr_mask);
}

uint32_t fpu_image_mxcsr_mask(const uint8_t image[FPU_IMAGE_SIZE])
{
	uint32_t mask = image_u32(image, FPU_IMAGE_MXCSR_MASK_OFFSET);
	return mask ? mask : FPU_MXCSR_FALLBACK_MASK;
}
