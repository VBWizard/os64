#ifndef NMI_PROBE_H
#define NMI_PROBE_H

#include <stdint.h>
#include <stdbool.h>
#include "smp.h"     // MAX_CPUS

// ── Asking a wedged core what it is doing ───────────────────────────────────
//
// WHY NMI, AND ONLY NMI. Every other way we have of reaching a core is a
// maskable interrupt — the acct-settle IPI (0x82), the schedule IPI (0x7A) —
// and every one of them is gated by exactly the machinery a wedged core is
// suspected of jamming: IF, the LAPIC priority logic (TPR, a stuck in-service
// bit), and our own mp_inScheduler software guard. Questioning the suspect
// through the thing it is accused of breaking answers nothing.
//
// A Non-Maskable Interrupt is gated by none of them. IF does not mask it, TPR
// does not hold it, an in-service ISR bit does not delay it, and it wakes a
// core out of `hlt` even with interrupts off. It is the one knock a core is
// architecturally obliged to answer.
//
// THE PROBE IS READ-ONLY, BY CONSTRUCTION. It changes no scheduler state,
// takes no lock, and returns the core to precisely what it was doing. The one
// honest caveat: `iretq` resumes at the instruction AFTER the `hlt`, not back
// inside it — in task_idle_loop that is a branch straight back to `sti; hlt`,
// and in a spin loop it resumes the spin. Either way a stuck core goes back to
// being stuck, which is the entire point: this instrument diagnoses, it never
// nudges. There is no telling what a wedged core was in the middle of, so
// "make it go back to work" is not a decision this code is entitled to make.
//
// THE DIVISION OF LABOUR, WHICH IS THE SAFETY MODEL:
//
//   The HANDLER, running on the sick core, SNAPSHOTS AND RETURNS. It does not
//   print — printd would take the logd queue locks from inside an NMI on a
//   core that may already hold them, converting a diagnosable wedge into a
//   deadlock. It does not walk the stack either: a #PF inside an NMI handler
//   `iretq`s on the way out and re-arms NMI mid-flight (the classic nested-NMI
//   trap), so the handler dereferences nothing it has not been handed.
//
//   The ASKER, running on a healthy core, INTERPRETS AND PRINTS. A bad frame
//   pointer costs it a "chain broke here" line instead of costing the machine
//   a double fault. Same shape scheduler.S already uses for mp_isrSaved*.
//
// One further hardware note worth knowing rather than discovering: from NMI
// delivery until our `iretq`, further NMIs are BLOCKED on that core. A genuine
// hardware NMI (parity, watchdog) arriving inside that ~50-instruction window
// is lost. Accepted deliberately — the window is tiny and the alternative is
// not having the instrument at all.

// The register block the stub pushes, low address first: `pushf` lands last so
// it sits lowest, then r15..rax ascending, because push walks DOWN. This is
// deliberately the SAME layout as fault_regs_t in simple_exceptions.c — the #PF
// stub established it and there is no reason for a second convention.
//
// CHANGE THIS STRUCT OR THE STUB'S PUSH ORDER AND YOU MUST CHANGE BOTH.
typedef struct {
	uint64_t rflags;
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} nmi_regs_t;

// The interrupt frame the CPU pushes for a no-error-code vector. Long mode
// always pushes all five, even ring0→ring0 (see the reference note in
// CLAUDE.md's lineage — this is the same fact that cost the scheduler its
// +40 arithmetic bug).
typedef struct {
	uint64_t rip, cs, rflags, rsp, ss;
} nmi_frame_t;

// Everything one core can truthfully say about itself in an NMI handler.
// Filled ONLY by that core, read by everyone else — so every field is written
// once, before done is published, and never touched again.
typedef struct {
	// Identity and freshness.
	uint32_t apic_id;
	uint64_t tsc;                  // when the snapshot was taken

	// Where it was and how it got there.
	nmi_frame_t frame;
	nmi_regs_t  regs;

	// Control registers. cr3 says which address space; cr2 is only meaningful
	// if the core was mid-#PF, which is itself worth knowing.
	uint64_t cr0, cr2, cr3, cr4;

	// Who the scheduler believes this core is running.
	uint64_t currentThread, currentTask;
	uint64_t acctLastDispatchTSC;  // frozen == this core stopped keeping books

	// THE STATE WE HAVE ALWAYS MAINTAINED AND COULD NEVER SEE. Any one of
	// these can strike a core off the roster on its own:
	//   inScheduler   — set: the nudge loop skips this core (scheduler.c:231),
	//                   send_ipi drops its schedule IPI (smp_core.c:143), AND
	//                   acct_settle_ISR refuses to settle (smp_core.c:641).
	//                   One bit, three independent disappearances.
	//   settleAck     — the liveness beacon mpAcctSettleAll already clears and
	//                   sets and nothing has ever read.
	//   lastIretqRIP  — where this core last returned to from a scheduler pass.
	uint8_t  inScheduler, settleAck, schedulerEnabled;
	uint64_t lastIretqRIP;

	// The LAPIC's own opinion, read while we are standing on the core — the
	// only moment it is legal to ask. A vector in service with nothing
	// progressing IS the diagnosis: an interrupt that never got its EOI blocks
	// everything at or below its priority class, forever.
	uint32_t apicInServiceVector;  // highest ISR bit, 0 = nothing in service
	uint32_t apicTPR, apicPPR, apicLVTTimer;

	// Published LAST. A reader that sees this true is guaranteed to see every
	// field above it, because the write is release-ordered by the compiler
	// barrier in the capture path.
	volatile uint8_t valid;
} nmi_probe_snapshot_t;

// Called by the NMI stub (handler_errors.S). Returns true if this NMI was a
// probe request we issued — the stub then resumes the core untouched. Returns
// false for a genuine hardware NMI, which falls through to the panic path that
// has always handled vector 2.
bool nmi_probe_capture(nmi_regs_t *regs, nmi_frame_t *frame);

// Fire an NMI at one core and wait — WITH INTERRUPTS ENABLED — for its answer.
// Returns false on timeout, which is not a failure but a verdict: a core that
// will not answer an NMI is off the bus or in SMM (the verdict line prints
// itself). On success the answer sits in the snapshot slot — RENDERING IS THE
// CALLER'S JOB (nmi_probe_render / nmi_probe_report_wire below), because the
// three callers want three different things: the boot sweep wants one summary
// line, /sys/cpu/<n>/probe wants the full text in a file, and a live wedge
// hunt wants it on the wire.
bool nmi_probe_core(uint32_t apic_id, uint32_t timeout_ticks);

// Probe every core but this one and print a ONE-LINE liveness summary per
// core (glass + wire). The full dumps stay in the snapshot slots, readable
// afterward at /sys/cpu/<n>/probe — a boot self-test's job is to prove the
// mechanism works, not to scroll eight cores' register files past the
// bootloader logo.
void nmi_probe_sweep(void);

// The last snapshot taken for a core, or NULL if it has never answered.
const nmi_probe_snapshot_t *nmi_probe_last(uint32_t apic_id);

// Render a snapshot as text, one line at a time, through a caller-supplied
// emitter — THE formatting lives here and only here, so the wire report and
// the /sys/cpu/<n>/probe file can never drift apart. The emitter gets each
// finished line (NUL-terminated, newline included) plus the caller's context.
// Takes no lock: locking is sink policy, and only sinks know their sinks.
typedef void (*nmi_probe_line_fn)(void *ctx, const char *line);
void nmi_probe_render(const nmi_probe_snapshot_t *s, nmi_probe_line_fn emit, void *ctx);

// Render a core's last snapshot to the WIRE (printd → logd file + COM1),
// under the exception wire lock. This is the crash-insurance copy the /sys
// probe trigger writes the moment an answer lands: if the machine dies before
// anyone reads the file, the answer still exists somewhere permanent.
void nmi_probe_report_wire(uint32_t apic_id);

#endif // NMI_PROBE_H
