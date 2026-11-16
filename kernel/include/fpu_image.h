#ifndef FPU_IMAGE_H
#define FPU_IMAGE_H

// The floating-point register IMAGE's rules, pure: no kernel headers, so
// tools/test_fpu_image_host.sh drives them with plain cc under ASan.
// docs/design/pending/AVX.md § The rules is the design (R2-R5 here).
//
// The image is the standard (non-compacted) XSAVE format with exactly three
// components — x87, SSE, AVX — and with exactly those three the architecture
// fixes the layout: the 512-byte legacy area FXSAVE also writes, a 64-byte
// header, and the AVX upper halves at 576 for 256 bytes. It is a constant,
// the same size in both modes: `sse` mode uses the first 512 bytes.

#include <stdbool.h>
#include <stdint.h>

#define FPU_IMAGE_SIZE              832
#define FPU_IMAGE_ALIGN             64    // XSAVE's alignment; FXSAVE needs 16
#define FPU_IMAGE_LEGACY_SIZE       512   // the FXSAVE image
#define FPU_IMAGE_HEADER_OFFSET     512   // XSTATE_BV, then XCOMP_BV, then reserved
#define FPU_IMAGE_HEADER_SIZE       64
#define FPU_IMAGE_AVX_OFFSET        576   // YMM0..15 bits 255:128, 16 bytes each
#define FPU_IMAGE_AVX_SIZE          256
#define FPU_IMAGE_FCW_OFFSET        0
#define FPU_IMAGE_MXCSR_OFFSET      24
#define FPU_IMAGE_MXCSR_MASK_OFFSET 28

_Static_assert(FPU_IMAGE_HEADER_OFFSET == FPU_IMAGE_LEGACY_SIZE,
               "the XSAVE header follows the legacy area");
_Static_assert(FPU_IMAGE_AVX_OFFSET == FPU_IMAGE_HEADER_OFFSET + FPU_IMAGE_HEADER_SIZE,
               "the AVX component follows the header in the standard format");
_Static_assert(FPU_IMAGE_AVX_OFFSET + FPU_IMAGE_AVX_SIZE == FPU_IMAGE_SIZE,
               "the image ends where the AVX component does");
_Static_assert(FPU_IMAGE_SIZE % FPU_IMAGE_ALIGN == 0,
               "an array of images keeps every one aligned");

// XCR0 and XSTATE_BV share these bits. os64 enables exactly these three,
// whatever else the CPU offers (R1): AVX-512's state stays off, so its
// instructions are #UD.
#define FPU_XCR0_X87  0x1U
#define FPU_XCR0_SSE  0x2U
#define FPU_XCR0_AVX  0x4U
#define FPU_XCR0      (FPU_XCR0_X87 | FPU_XCR0_SSE | FPU_XCR0_AVX)

// The architectural reset values: every exception masked, round to nearest,
// extended precision for x87.
#define FPU_IMAGE_RESET_FCW         0x037FU
#define FPU_IMAGE_RESET_MXCSR       0x1F80U
// The MXCSR bits every SSE CPU implements. An image's MXCSR_MASK field of
// ZERO means "assume this" (Intel SDM vol. 1 § 11.6.6).
#define FPU_MXCSR_FALLBACK_MASK     0xFFBFU

typedef enum
{
	FPU_MODE_SSE = 0,   // fxsave64/fxrstor64, CR4.OSXSAVE clear, AVX is #UD
	FPU_MODE_AVX = 1,   // xsave64/xrstor64 with mask FPU_XCR0, XCR0 = FPU_XCR0
} fpu_mode_t;

// Why the boot is in the mode it is in. Everything but FPU_WHY_ON is a
// reason for `sse` mode, and fpu_report prints it.
typedef enum
{
	FPU_WHY_ON = 0,
	FPU_WHY_NO_XSAVE,       // CPUID.1:ECX.XSAVE clear
	FPU_WHY_NO_AVX,         // XSAVE, but CPUID.1:ECX.AVX clear
	FPU_WHY_NOAVX,          // the NOAVX cmdline token
	FPU_WHY_COMPONENTS,     // CPUID.0xD.0:EAX does not offer x87, SSE and AVX
	FPU_WHY_SIZE,           // CPUID.0xD.0:EBX with XCR0 = 7 is not FPU_IMAGE_SIZE
	FPU_WHY_AVX_PLACE,      // CPUID.0xD.2 does not put AVX at 576 for 256
} fpu_why_t;

// What the CPU answered. The layout fields are read only after the CPU has
// passed fpu_mode_before_layout and XCR0 has been written (the size CPUID
// reports is the size for the components enabled NOW).
typedef struct
{
	bool xsave, avx, noavx;
	uint32_t components;    // CPUID.0xD.0:EAX — which XCR0 bits the CPU supports
	uint32_t size;          // CPUID.0xD.0:EBX — the image size for XCR0 as written
	uint32_t avx_size;      // CPUID.0xD.2:EAX
	uint32_t avx_offset;    // CPUID.0xD.2:EBX
} fpu_cpu_answers_t;

// The first refusals: whether XSAVE is worth turning on at all. FPU_WHY_ON
// means go on to write CR4.OSXSAVE and read leaf 0xD's components.
fpu_why_t fpu_mode_before_layout(const fpu_cpu_answers_t *a);
// Whether the CPU offers the three components, so XCR0 = 7 may be written.
// FPU_WHY_ON means write it, then read the sizes.
fpu_why_t fpu_mode_components(const fpu_cpu_answers_t *a);
// Whether the layout the CPU reports is R3's. FPU_WHY_ON means `avx` mode.
fpu_why_t fpu_mode_layout(const fpu_cpu_answers_t *a);

// The reset image (R5): x87 and SSE as the architecture resets them, read
// from the image (XSTATE_BV = x87|SSE); AVX marked absent, so a restore
// zeroes every upper half and no thread inherits another's.
void fpu_image_init(uint8_t image[FPU_IMAGE_SIZE]);

// R4: make an image safe for the kernel to restore, whoever wrote it. A
// header naming a component outside XCR0, a nonzero reserved header byte
// and a reserved MXCSR bit are each a KERNEL #GP on xrstor64 (the last also
// on fxrstor64). A compaction word is a #GP on a CPU without XSAVEC and, on
// one with it, asks xrstor64 to read a layout the kernel never writes (the
// standard form is R6's). Sanitize, never refuse: XSTATE_BV
// is ANDed with FPU_XCR0, header bytes 8-63 are zeroed, and MXCSR is ANDed
// with `mxcsr_mask` (the CPU's; 0 means FPU_MXCSR_FALLBACK_MASK). Nothing
// else changes, so an image that was already legal is restored as it was.
void fpu_image_sanitize(uint8_t image[FPU_IMAGE_SIZE], uint32_t mxcsr_mask);

// The MXCSR bits the CPU implements, as an FXSAVE or XSAVE of it reported
// them in this image, or the fallback when it reported none.
uint32_t fpu_image_mxcsr_mask(const uint8_t image[FPU_IMAGE_SIZE]);

#endif
