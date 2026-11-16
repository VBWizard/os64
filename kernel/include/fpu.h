#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>
#include "fpu_image.h"

// ── THE FLOATING-POINT REGISTER FILE IS PART OF A THREAD'S CONTEXT ──────────
//
// x87, MMX, the sixteen XMM registers, MXCSR and — in `avx` mode — the upper
// halves of the sixteen YMM registers are saved and restored with every
// context switch (scheduler_store_thread / scheduler_load_thread) and
// snapshotted into every signal frame, as one 832-byte image (fpu_image.h).
// EAGER, not lazy: no CR0.TS juggling, no #NM trap on first use. The copy is
// unconditional; the trap approach costs more than it saves once userland is
// compiled with SSE for ordinary code (it is — -msse2 is the x86-64
// baseline, and even a varargs printf prologue touches XMM).
//
// The invariant everything rests on: the LIVE register file always belongs
// to the thread the core is running. The kernel itself is built -mno-sse
// and never touches it, so nothing needs saving on the way into an
// interrupt or syscall — only on the way to another thread.
//
// ONE MODE PER BOOT, decided on the BSP and entered by every CPU
// (docs/design/pending/AVX.md § The rules, R2):
//
//   avx  the CPU has XSAVE and AVX, CPUID leaf 0xD agrees with the fixed
//        layout, and NOAVX is absent. CR4.OSXSAVE is set and XCR0 is
//        written as exactly x87|SSE|AVX on every CPU — never inherited from
//        the firmware, and never AVX-512's state, so its instructions stay
//        #UD. The switch uses xsave64/xrstor64. Ring 3 learns it from CPUID
//        (OSXSAVE) and XGETBV, with no syscall.
//   sse  anything else. CR4.OSXSAVE is CLEARED (UEFI leaves it on), the
//        switch uses fxsave64/fxrstor64 on the image's first 512 bytes, and
//        an AVX instruction is #UD at ring 3. Enabling AVX without the wide
//        save would leak the upper halves between threads.
//
// EVERY RESTORE GOES THROUGH fpu_restore, which sanitizes the image first
// (fpu_image_sanitize, R4): a signal frame is user memory, and a header or
// MXCSR the kernel did not write is otherwise a kernel #GP.

typedef struct
{
	uint8_t bytes[FPU_IMAGE_SIZE] __attribute__((aligned(FPU_IMAGE_ALIGN)));
} fpu_state_t;

_Static_assert(_Alignof(fpu_state_t) == FPU_IMAGE_ALIGN,
		"XSAVE requires every FPU state image to be 64-byte aligned");
_Static_assert(sizeof(fpu_state_t) == FPU_IMAGE_SIZE,
		"fpu_state_t is exactly the image");

// What CPUID said and what was switched on — filled by the BSP's
// fpu_init_this_cpu, reported by fpu_report.
typedef struct
{
	bool mmx, sse3, ssse3, sse41, sse42, sse4a;   // present AND usable in both modes
	bool xsave, avx, avx2, fma3, f16c;            // present; usable at ring 3 only in `avx` mode
	uint32_t mxcsr_mask;              // the MXCSR bits this CPU implements
	fpu_mode_t mode;                  // the boot's mode (R2), the same on every CPU
	fpu_why_t why;                    // FPU_WHY_ON, or why `sse` mode
	fpu_cpu_answers_t answers;        // what the BSP's CPUID said, for the boot line
} fpu_features_t;

extern fpu_features_t kFPUFeatures;
// Set by the NOAVX cmdline token: `sse` mode on a CPU that could do `avx`.
extern bool kFPUNoAVX;

// Per CPU: verify the baseline, set CR0/CR4 (and XCR0 in `avx` mode),
// reset the register file. The BSP — the first caller — decides the mode;
// every later caller must enter it or panics by name. Panics if x87/FXSR/
// SSE/SSE2 are missing, which cannot happen on a CPU that reached long
// mode, so that panic is a tripwire, not a branch.
void fpu_init_this_cpu(void);
// The boot line, glass and log both: what is enabled, and what is present
// but off and why. Call once, on the BSP, once the glass exists.
void fpu_report(void);
void fpu_state_init(fpu_state_t *state);
void fpu_save(fpu_state_t *state);
// Sanitizes `state` in place, then loads it into the live registers.
void fpu_restore(fpu_state_t *state);
// A signal frame's image coming home: copied into `destination` (the
// thread's own) and restored through fpu_restore's sanitizer.
bool fpu_state_from_user(fpu_state_t *destination, const fpu_state_t *source);

#endif
