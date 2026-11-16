// Host test for kernel/src/fpu_image.c — the register image's rules
// (AVX.md § The rules, R2-R5). Run by tools/test_fpu_image_host.sh under
// ASan and UBSan.
//
// The sanitizer is the part a wrong byte offset turns into a KERNEL #GP,
// so it is checked two ways: against a reference model written with whole
// 64-bit words (not the byte loop the kernel uses), over random images; and
// against the invariants xrstor64 itself demands, stated one by one.

#include "fpu_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		s_failures++; \
	} \
} while (0)

static uint64_t s_rng = 0x9E3779B97F4A7C15ULL;
static uint64_t next_random(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return s_rng;
}

static void fill_random(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t)next_random();
}

static uint64_t get_u64(const uint8_t *image, uint32_t offset)
{
	uint64_t v;
	memcpy(&v, image + offset, sizeof(v));   // the host is little-endian, like x86
	return v;
}

static uint32_t get_u32(const uint8_t *image, uint32_t offset)
{
	uint32_t v;
	memcpy(&v, image + offset, sizeof(v));
	return v;
}

static void put_u64(uint8_t *image, uint32_t offset, uint64_t v)
{
	memcpy(image + offset, &v, sizeof(v));
}

static void put_u32(uint8_t *image, uint32_t offset, uint32_t v)
{
	memcpy(image + offset, &v, sizeof(v));
}

// The reference: R4 as the design states it, in words.
static void model_sanitize(uint8_t *image, uint32_t mask)
{
	if (mask == 0)
		mask = 0xFFBFU;
	put_u64(image, 512, get_u64(image, 512) & 7ULL);
	for (uint32_t word = 520; word < 576; word += 8)
		put_u64(image, word, 0);
	put_u32(image, 24, get_u32(image, 24) & mask);
}

// What xrstor64 (standard form) and fxrstor64 need to not fault.
static void check_restorable(const uint8_t *image, uint32_t mask, const char *what)
{
	if (mask == 0)
		mask = 0xFFBFU;
	CHECK((get_u64(image, 512) & ~7ULL) == 0, "%s: XSTATE_BV 0x%llx names a component outside XCR0",
	      what, (unsigned long long)get_u64(image, 512));
	CHECK(get_u64(image, 520) == 0, "%s: XCOMP_BV is 0x%llx", what, (unsigned long long)get_u64(image, 520));
	for (uint32_t i = 528; i < 576; i++)
		CHECK(image[i] == 0, "%s: reserved header byte %u is 0x%02x", what, i, image[i]);
	CHECK((get_u32(image, 24) & ~mask) == 0, "%s: MXCSR 0x%08x has a bit outside the mask 0x%08x",
	      what, get_u32(image, 24), mask);
}

static void test_layout(void)
{
	CHECK(FPU_IMAGE_SIZE == 832, "the image is %d bytes", FPU_IMAGE_SIZE);
	CHECK(FPU_IMAGE_ALIGN == 64, "the alignment is %d", FPU_IMAGE_ALIGN);
	CHECK(FPU_IMAGE_HEADER_OFFSET == 512, "the header is at %d", FPU_IMAGE_HEADER_OFFSET);
	CHECK(FPU_IMAGE_AVX_OFFSET == 576 && FPU_IMAGE_AVX_SIZE == 256,
	      "AVX is at %d for %d", FPU_IMAGE_AVX_OFFSET, FPU_IMAGE_AVX_SIZE);
	CHECK(FPU_IMAGE_MXCSR_OFFSET == 24 && FPU_IMAGE_MXCSR_MASK_OFFSET == 28,
	      "MXCSR at %d, its mask at %d", FPU_IMAGE_MXCSR_OFFSET, FPU_IMAGE_MXCSR_MASK_OFFSET);
	CHECK(FPU_XCR0 == 7, "XCR0 is 0x%x", FPU_XCR0);
}

static void test_init(void)
{
	uint8_t image[FPU_IMAGE_SIZE];
	for (int round = 0; round < 64; round++)
	{
		fill_random(image, sizeof(image));      // every byte must be written
		fpu_image_init(image);
		CHECK(image[0] == 0x7F && image[1] == 0x03, "FCW is 0x%02x%02x", image[1], image[0]);
		CHECK(get_u32(image, 24) == 0x1F80, "MXCSR is 0x%08x", get_u32(image, 24));
		CHECK(get_u64(image, 512) == 3, "XSTATE_BV is 0x%llx, want x87|SSE",
		      (unsigned long long)get_u64(image, 512));
		for (uint32_t i = 2; i < FPU_IMAGE_SIZE; i++)
		{
			if (i >= 24 && i < 28)
				continue;
			if (i == 512)
				continue;
			CHECK(image[i] == 0, "reset image byte %u is 0x%02x", i, image[i]);
		}
		check_restorable(image, 0, "the reset image");
		// Already legal: sanitizing changes nothing.
		uint8_t copy[FPU_IMAGE_SIZE];
		memcpy(copy, image, sizeof(copy));
		fpu_image_sanitize(copy, 0xFFFF);
		CHECK(memcmp(copy, image, sizeof(copy)) == 0, "sanitizing the reset image changed it");
	}
}

// A random image as xsave64 would write it: any legacy bytes, a legal
// MXCSR, a legal header, any AVX bytes. Sanitizing must leave it alone.
static void test_legal_untouched(void)
{
	const uint32_t masks[] = { 0xFFFF, 0xFFBF, 0x2FFFF };
	for (int round = 0; round < 20000; round++)
	{
		uint32_t mask = masks[round % 3];
		uint8_t image[FPU_IMAGE_SIZE], copy[FPU_IMAGE_SIZE];
		fill_random(image, sizeof(image));
		put_u32(image, 24, get_u32(image, 24) & mask);
		memset(image + 512, 0, 64);
		image[512] = (uint8_t)(next_random() & 7);
		memcpy(copy, image, sizeof(copy));
		fpu_image_sanitize(image, mask);
		CHECK(memcmp(copy, image, sizeof(copy)) == 0, "round %d: a legal image was changed", round);
	}
}

// Any bytes at all, as ring 3 may hand them back in a signal frame.
static void test_hostile(void)
{
	const uint32_t masks[] = { 0xFFFF, 0xFFBF, 0x2FFFF, 0, 0x1 };
	for (int round = 0; round < 50000; round++)
	{
		uint32_t mask = masks[round % 5];
		uint8_t image[FPU_IMAGE_SIZE], expect[FPU_IMAGE_SIZE], before[FPU_IMAGE_SIZE];
		fill_random(image, sizeof(image));
		// Lean on the four forgeries fputest makes, one at a time.
		switch (round % 6)
		{
		case 0: put_u64(image, 512, ~0ULL); break;
		case 1: put_u64(image, 520, (1ULL << 63) | 7); break;
		case 2: memset(image + 512, 0, 64); image[575] = 0x5A; break;
		case 3: put_u32(image, 24, 0xFFFFFFFFU); break;
		default: break;
		}
		memcpy(before, image, sizeof(before));
		memcpy(expect, image, sizeof(expect));
		model_sanitize(expect, mask);
		fpu_image_sanitize(image, mask);
		CHECK(memcmp(expect, image, sizeof(expect)) == 0, "round %d: the sanitizer disagrees with the model", round);
		check_restorable(image, mask, "a sanitized hostile image");
		// The registers are the program's own business: every byte outside
		// MXCSR and the header comes back exactly as written.
		CHECK(memcmp(before, image, 24) == 0, "round %d: legacy bytes before MXCSR changed", round);
		CHECK(memcmp(before + 28, image + 28, 512 - 28) == 0, "round %d: legacy bytes after MXCSR changed", round);
		CHECK(memcmp(before + 576, image + 576, 256) == 0, "round %d: the AVX upper halves changed", round);
		CHECK((get_u32(before, 24) & (mask ? mask : 0xFFBFU)) == get_u32(image, 24),
		      "round %d: a legal MXCSR bit was cleared", round);
		CHECK((get_u64(before, 512) & 7) == get_u64(image, 512),
		      "round %d: a legal XSTATE_BV bit was cleared", round);
	}
}

static void test_mxcsr_mask(void)
{
	uint8_t image[FPU_IMAGE_SIZE];
	memset(image, 0, sizeof(image));
	CHECK(fpu_image_mxcsr_mask(image) == 0xFFBF, "a zero mask field reads as 0x%x", fpu_image_mxcsr_mask(image));
	put_u32(image, 28, 0xFFFF);
	CHECK(fpu_image_mxcsr_mask(image) == 0xFFFF, "the mask field reads as 0x%x", fpu_image_mxcsr_mask(image));
	put_u32(image, 28, 0x2FFFF);
	CHECK(fpu_image_mxcsr_mask(image) == 0x2FFFF, "the mask field reads as 0x%x", fpu_image_mxcsr_mask(image));
	put_u32(image, 24, 0x12345678);   // MXCSR itself is not the mask
	CHECK(fpu_image_mxcsr_mask(image) == 0x2FFFF, "MXCSR leaked into the mask: 0x%x", fpu_image_mxcsr_mask(image));
	// The field is read whole, all four bytes, whatever a CPU put there.
	put_u32(image, 28, 0x8100FFFFU);
	CHECK(fpu_image_mxcsr_mask(image) == 0x8100FFFFU, "the mask field's top byte was lost: 0x%x",
	      fpu_image_mxcsr_mask(image));
	// And the sanitizer keeps every MXCSR bit the mask allows, top byte included.
	put_u32(image, 24, 0xFFFFFFFFU);
	fpu_image_sanitize(image, 0x8100FFFFU);
	CHECK(get_u32(image, 24) == 0x8100FFFFU, "MXCSR under a mask of 0x8100FFFF is 0x%x", get_u32(image, 24));
}

static fpu_cpu_answers_t good(void)
{
	fpu_cpu_answers_t a = { .xsave = true, .avx = true, .noavx = false,
	                        .components = 7, .size = 832, .avx_size = 256, .avx_offset = 576 };
	return a;
}

static void test_mode(void)
{
	fpu_cpu_answers_t a = good();
	CHECK(fpu_mode_before_layout(&a) == FPU_WHY_ON, "a capable CPU refused before the layout");
	CHECK(fpu_mode_components(&a) == FPU_WHY_ON, "a capable CPU refused at the components");
	CHECK(fpu_mode_layout(&a) == FPU_WHY_ON, "a capable CPU refused at the layout");

	// A CPU that also offers AVX-512's three components and MPX's two: it
	// still gets exactly x87|SSE|AVX, so this is still yes.
	a.components = 0xFF;
	CHECK(fpu_mode_components(&a) == FPU_WHY_ON, "more components than we enable refused");

	a = good(); a.xsave = false;
	CHECK(fpu_mode_before_layout(&a) == FPU_WHY_NO_XSAVE, "no XSAVE was not refused as such");
	a = good(); a.xsave = false; a.avx = false;
	CHECK(fpu_mode_before_layout(&a) == FPU_WHY_NO_XSAVE, "no XSAVE and no AVX was not 'no XSAVE'");
	a = good(); a.avx = false;
	CHECK(fpu_mode_before_layout(&a) == FPU_WHY_NO_AVX, "no AVX was not refused as such");
	a = good(); a.noavx = true;
	CHECK(fpu_mode_before_layout(&a) == FPU_WHY_NOAVX, "NOAVX was not obeyed");
	a = good(); a.avx = false; a.noavx = true;
	CHECK(fpu_mode_before_layout(&a) == FPU_WHY_NO_AVX, "a CPU without AVX blamed NOAVX");

	for (uint32_t bit = 0; bit < 3; bit++)
	{
		a = good(); a.components = 7 & ~(1U << bit);
		CHECK(fpu_mode_components(&a) == FPU_WHY_COMPONENTS, "component bit %u missing was not refused", bit);
	}
	a = good(); a.components = 0;
	CHECK(fpu_mode_components(&a) == FPU_WHY_COMPONENTS, "no leaf 0xD was not refused");

	a = good(); a.size = 1088;
	CHECK(fpu_mode_layout(&a) == FPU_WHY_SIZE, "a size of 1088 was not refused");
	a = good(); a.size = 576;
	CHECK(fpu_mode_layout(&a) == FPU_WHY_SIZE, "a size of 576 was not refused");
	a = good(); a.avx_offset = 512;
	CHECK(fpu_mode_layout(&a) == FPU_WHY_AVX_PLACE, "AVX at 512 was not refused");
	a = good(); a.avx_size = 128;
	CHECK(fpu_mode_layout(&a) == FPU_WHY_AVX_PLACE, "AVX of 128 bytes was not refused");
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	test_layout();
	test_init();
	test_legal_untouched();
	test_hostile();
	test_mxcsr_mask();
	test_mode();
	if (s_failures)
	{
		fprintf(stderr, "fpu_image: %d check(s) FAILED\n", s_failures);
		return 1;
	}
	printf("fpu_image: every check passed\n");
	return 0;
}
