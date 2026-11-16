// The floating-point register file as thread context — see fpu.h for the
// design (eager save per switch, one mode per boot) and fpu_image.h for the
// image's rules.

#include "fpu.h"
#include "panic.h"
#include "printd.h"
#include "sprintf.h"
#include "CONFIG.h"
#include "smp_core.h"        // cpu_self — which CPU refused, in a panic
#include "BasicRenderer.h"   // printf — the glass
#include <cpuid.h>

#define CR0_MP (1UL << 1)
#define CR0_EM (1UL << 2)
#define CR0_TS (1UL << 3)
#define CR0_NE (1UL << 5)
#define CR4_OSFXSR (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)
#define CR4_OSXSAVE (1UL << 18)

// CPUID.1:EDX — the long-mode baseline every x86-64 CPU carries.
#define CPUID1_EDX_FPU   (1U << 0)
#define CPUID1_EDX_FXSR  (1U << 24)
#define CPUID1_EDX_SSE   (1U << 25)
#define CPUID1_EDX_SSE2  (1U << 26)
// CPUID.1:ECX — what FXSAVE covers (the SSE3..4.2 family adds instructions,
// not registers) and what needs XSAVE (AVX and the VEX-encoded rest).
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
// CPUID leaf 0xD: the XSAVE layout. Subleaf 0 gives the supported XCR0 bits
// (EAX) and the image size for the XCR0 in force (EBX); subleaf 2 gives the
// AVX component's size (EAX) and offset (EBX).
#define CPUID_XSAVE_LEAF 0xD

fpu_features_t kFPUFeatures;
bool kFPUNoAVX = false;
// The BSP is the first caller of fpu_init_this_cpu and the one that decides.
static bool s_fpu_mode_decided;

static uint64_t fpu_xgetbv0(void)
{
	uint32_t lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return ((uint64_t)hi << 32) | lo;
}

static void fpu_xsetbv0(uint64_t value)
{
	__asm__ volatile("xsetbv" : : "c"(0), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}

static uint64_t fpu_read_cr4(void)
{
	uint64_t cr4;
	__asm__ volatile("mov %0, cr4" : "=r"(cr4));
	return cr4;
}

static void fpu_write_cr4(uint64_t cr4)
{
	__asm__ volatile("mov cr4, %0" : : "r"(cr4) : "memory");
}

static const char *fpu_why_text(fpu_why_t why)
{
	switch (why)
	{
	case FPU_WHY_ON:         return "on";
	case FPU_WHY_NO_XSAVE:   return "no XSAVE";
	case FPU_WHY_NO_AVX:     return "no AVX";
	case FPU_WHY_NOAVX:      return "NOAVX";
	case FPU_WHY_COMPONENTS: return "CPUID leaf 0xD lacks x87, SSE or AVX";
	case FPU_WHY_SIZE:       return "CPUID leaf 0xD reports another image size";
	case FPU_WHY_AVX_PLACE:  return "CPUID leaf 0xD places AVX elsewhere";
	}
	return "?";
}

// Try to put THIS CPU into `avx` mode: the first refusals, then the
// components (writing an unsupported XCR0 bit is a #GP), then CR4.OSXSAVE
// and XCR0 = x87|SSE|AVX, then the layout the CPU reports for that XCR0.
// On FPU_WHY_ON the CPU is left in `avx` mode; on anything else OSXSAVE is
// cleared again, which is `sse` mode whatever XCR0 now holds.
static fpu_why_t fpu_try_avx(fpu_cpu_answers_t *a)
{
	uint32_t eax, ebx, ecx, edx;
	fpu_why_t why = fpu_mode_before_layout(a);
	if (why != FPU_WHY_ON)
		return why;
	a->components = __get_cpuid_count(CPUID_XSAVE_LEAF, 0, &eax, &ebx, &ecx, &edx) ? eax : 0;
	why = fpu_mode_components(a);
	if (why != FPU_WHY_ON)
		return why;
	fpu_write_cr4(fpu_read_cr4() | CR4_OSXSAVE);
	fpu_xsetbv0(FPU_XCR0);
	if (__get_cpuid_count(CPUID_XSAVE_LEAF, 0, &eax, &ebx, &ecx, &edx))
		a->size = ebx;
	if (__get_cpuid_count(CPUID_XSAVE_LEAF, 2, &eax, &ebx, &ecx, &edx))
	{
		a->avx_size = eax;
		a->avx_offset = ebx;
	}
	why = fpu_mode_layout(a);
	if (why != FPU_WHY_ON)
		fpu_write_cr4(fpu_read_cr4() & ~CR4_OSXSAVE);
	return why;
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
	// OSXMMEXCPT routes unmasked SSE and AVX exceptions to #XM (vector 19)
	// instead of #UD. OSXSAVE is decided below and WRITTEN either way, never
	// left as the firmware had it: UEFI runs with XSAVE on and XCR0 as it
	// liked, and a CPU left with OSXSAVE set in `sse` mode would let ring 3
	// execute AVX while the switch saves only the FXSAVE image — the upper
	// halves leaking between threads.
	uint64_t cr0;
	__asm__ volatile("mov %0, cr0" : "=r"(cr0));
	cr0 |= CR0_MP | CR0_NE;
	cr0 &= ~(CR0_EM | CR0_TS);
	__asm__ volatile("mov cr0, %0" : : "r"(cr0) : "memory");
	fpu_write_cr4((fpu_read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT) & ~CR4_OSXSAVE);

	// The mode (R2): the BSP decides it from its own answers; every later
	// CPU must enter it. A machine whose CPUs disagree about AVX is not one
	// os64 runs on, so an AP that cannot follow the BSP into `avx` stops the
	// boot by name rather than run the one CPU whose switch would leak.
	fpu_cpu_answers_t answers = {
		.xsave = kFPUFeatures.xsave, .avx = kFPUFeatures.avx, .noavx = kFPUNoAVX,
	};
	if (!s_fpu_mode_decided)
	{
		fpu_why_t why = fpu_try_avx(&answers);
		kFPUFeatures.mode = why == FPU_WHY_ON ? FPU_MODE_AVX : FPU_MODE_SSE;
		kFPUFeatures.why = why;
		kFPUFeatures.answers = answers;
		s_fpu_mode_decided = true;
	}
	else if (kFPUFeatures.mode == FPU_MODE_AVX)
	{
		fpu_why_t why = fpu_try_avx(&answers);
		if (why != FPU_WHY_ON)
			panic("FPU: CPU %u cannot enter the boot CPU's AVX mode (%s)\n",
			      cpu_self(), fpu_why_text(why));
	}
	// R1, read back: XCR0 is what we wrote, on this CPU.
	if (kFPUFeatures.mode == FPU_MODE_AVX && fpu_xgetbv0() != FPU_XCR0)
		panic("FPU: CPU %u kept XCR0 0x%lx after writing 0x%x\n",
		      cpu_self(), fpu_xgetbv0(), FPU_XCR0);

	__asm__ volatile("fninit\n\tldmxcsr %0" : : "m"((const uint32_t){ FPU_IMAGE_RESET_MXCSR }) : "memory");

	// Ask the hardware which MXCSR bits it implements — the only way to learn
	// it is to save and read the field back, and it must come after the mode
	// is entered, because fpu_save uses the mode's instruction. Static, not a
	// local: the early-boot stack is not guaranteed aligned and a misaligned
	// save is a #GP. Cores may race into it; the one field read back is CPU
	// capability data, identical from every core.
	static fpu_state_t probe;
	fpu_save(&probe);
	kFPUFeatures.mxcsr_mask = fpu_image_mxcsr_mask(probe.bytes);
}

void fpu_report(void)
{
	// Glass AND log: printf reaches only the framebuffer, printd only the
	// serial pipeline. A boot line about whether the FPU is on belongs in
	// both places — it is the first thing to check when a float program dies.
	const fpu_features_t *f = &kFPUFeatures;
	char vex[40];
	snprintf(vex, sizeof(vex), "AVX%s%s%s",
	         f->avx2 ? " AVX2" : "", f->fma3 ? " FMA3" : "", f->f16c ? " F16C" : "");
	char line[256];
	int n = snprintf(line, sizeof(line), "FPU: x87%s SSE SSE2%s%s%s%s%s",
	                 f->mmx   ? " MMX"    : "",
	                 f->sse3  ? " SSE3"   : "",
	                 f->ssse3 ? " SSSE3"  : "",
	                 f->sse41 ? " SSE4.1" : "",
	                 f->sse42 ? " SSE4.2" : "",
	                 f->sse4a ? " SSE4A"  : "");
	if (f->mode == FPU_MODE_AVX)
		snprintf(line + n, sizeof(line) - n,
		         " %s enabled, XSAVE per thread (%u bytes, XCR0 0x%x, MXCSR mask 0x%04X)\n",
		         vex, FPU_IMAGE_SIZE, FPU_XCR0, f->mxcsr_mask);
	else
	{
		n += snprintf(line + n, sizeof(line) - n,
		              " enabled, FXSAVE per thread (MXCSR mask 0x%04X); ", f->mxcsr_mask);
		const fpu_cpu_answers_t *a = &f->answers;
		switch (f->why)
		{
		case FPU_WHY_NO_XSAVE:
			snprintf(line + n, sizeof(line) - n, "no XSAVE\n");
			break;
		case FPU_WHY_NO_AVX:
			snprintf(line + n, sizeof(line) - n, "XSAVE present, no AVX\n");
			break;
		case FPU_WHY_NOAVX:
			snprintf(line + n, sizeof(line) - n, "%s present, off (NOAVX)\n", vex);
			break;
		case FPU_WHY_COMPONENTS:
			snprintf(line + n, sizeof(line) - n,
			         "%s present, off (CPUID leaf 0xD offers components 0x%x, not 0x%x)\n",
			         vex, a->components, FPU_XCR0);
			break;
		case FPU_WHY_SIZE:
			snprintf(line + n, sizeof(line) - n,
			         "%s present, off (CPUID leaf 0xD: a %u-byte image, not %u)\n",
			         vex, a->size, FPU_IMAGE_SIZE);
			break;
		case FPU_WHY_AVX_PLACE:
			snprintf(line + n, sizeof(line) - n,
			         "%s present, off (CPUID leaf 0xD: AVX at %u for %u, not %u for %u)\n",
			         vex, a->avx_offset, a->avx_size, FPU_IMAGE_AVX_OFFSET, FPU_IMAGE_AVX_SIZE);
			break;
		case FPU_WHY_ON:   // `sse` mode is never FPU_WHY_ON; say so if it is
			snprintf(line + n, sizeof(line) - n, "AVX off for no recorded reason\n");
			break;
		}
	}
	printf("%s", line);
	printd(DEBUG_BOOT, "%s", line);
}

void fpu_save(fpu_state_t *state)
{
	// The REX.W forms: FIP and FDP are saved as full 64-bit addresses. The
	// plain forms keep only their low 32 bits (plus selectors that mean
	// nothing in long mode), which truncates the x87 exception pointers for
	// any code above 4GB — the shared-library window is at 0x7F0000000000.
	// "+m": neither instruction writes every byte (XSAVE writes only
	// XSTATE_BV of the header), so the bytes it skips are kept, not undefined.
	if (kFPUFeatures.mode == FPU_MODE_AVX)
		__asm__ volatile("xsave64 %0" : "+m"(*state) : "a"(FPU_XCR0), "d"(0) : "memory");
	else
		__asm__ volatile("fxsave64 %0" : "+m"(*state) : : "memory");
}

void fpu_restore(fpu_state_t *state)
{
	// R4: whoever wrote this image, nothing in it may fault the restore.
	// For an image the kernel saved this changes nothing; for one a signal
	// frame brought back it is the whole defence.
	fpu_image_sanitize(state->bytes, kFPUFeatures.mxcsr_mask);
	if (kFPUFeatures.mode == FPU_MODE_AVX)
		__asm__ volatile("xrstor64 %0" : : "m"(*state), "a"(FPU_XCR0), "d"(0) : "memory");
	else
		__asm__ volatile("fxrstor64 %0" : : "m"(*state) : "memory");
}

void fpu_state_init(fpu_state_t *state)
{
	fpu_image_init(state->bytes);
}

bool fpu_state_from_user(fpu_state_t *destination, const fpu_state_t *source)
{
	for (uint32_t i = 0; i < FPU_IMAGE_SIZE; i++)
		destination->bytes[i] = source->bytes[i];
	fpu_restore(destination);
	return true;
}
