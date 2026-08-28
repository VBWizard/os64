#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>

// ── THE FLOATING-POINT REGISTER FILE IS PART OF A THREAD'S CONTEXT ──────────
//
// x87, MMX, the sixteen XMM registers and MXCSR are saved and restored with
// every context switch (scheduler_store_thread / scheduler_load_thread) and
// snapshotted into every signal frame, as one 512-byte FXSAVE image. EAGER,
// not lazy: no CR0.TS juggling, no #NM trap on first use. The copy is cheap
// and unconditional; the trap approach costs more than it saves once
// userland is compiled with SSE for ordinary code (it is — -msse2 is the
// x86-64 baseline, and even a varargs printf prologue touches XMM).
//
// The invariant everything rests on: the LIVE register file always belongs
// to the thread the core is running. The kernel itself is built -mno-sse
// and never touches it, so nothing needs saving on the way into an
// interrupt or syscall — only on the way to another thread.
//
// Deliberately NOT enabled: XSAVE, and with it AVX and everything above
// SSE4.2. CR4.OSXSAVE stays clear, so an AVX instruction is #UD at ring 3
// (and a ring-3 #UD kills the program, not the machine). That is the
// consistent, safe state — the upper halves of the YMM registers cannot be
// in play if nothing can execute an instruction that reaches them. Booked
// in DEBTS; the day a consumer wants AVX, this file grows XSAVE with the
// variable-size image XGETBV reports, and the thread_t/signal_frame fields
// stop being a fixed 512 bytes.

#define FPU_FXSAVE_SIZE 512

typedef struct
{
	uint8_t bytes[FPU_FXSAVE_SIZE] __attribute__((aligned(16)));
} fpu_state_t;

_Static_assert(_Alignof(fpu_state_t) == 16,
		"FXSAVE requires every FPU state image to be 16-byte aligned");

// What CPUID said and what was switched on — filled by the BSP's
// fpu_init_this_cpu, reported by fpu_report, published for /sys later.
typedef struct
{
	bool mmx, sse3, ssse3, sse41, sse42, sse4a;   // present AND usable (FXSAVE covers them)
	bool xsave, avx, avx2, fma3, f16c;            // present, deliberately NOT enabled (VEX-encoded: #UD without OSXSAVE)
	uint32_t mxcsr_mask;              // the MXCSR bits this CPU implements
} fpu_features_t;

extern fpu_features_t kFPUFeatures;

// Per core: verify the baseline, set CR0/CR4, reset the register file.
// Panics if x87/FXSR/SSE/SSE2 are missing — which cannot happen on a CPU
// that reached long mode, so the panic is a tripwire, not a branch.
void fpu_init_this_cpu(void);
// The boot line, glass and log both: what is enabled, what is present but
// off. Call once, on the BSP, once the glass exists (after init_video).
void fpu_report(void);
void fpu_state_init(fpu_state_t *state);
void fpu_save(fpu_state_t *state);
void fpu_restore(fpu_state_t *state);
bool fpu_state_from_user(fpu_state_t *destination, const fpu_state_t *source);

#endif
