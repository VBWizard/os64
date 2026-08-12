#ifndef EXCEPTION_REPORT_H
#define EXCEPTION_REPORT_H

#include <stdint.h>
#include <stdbool.h>

// ── One capture, one report, for every exception ────────────────────────────
//
// THE PROBLEM THIS REPLACES (2026-08-11). Each exception vector had grown its
// own idea of what to save and what to print. #PF pushed a register block and
// stashed pointers to it in three FILE-SCOPE globals; every other vector saved
// nothing at all and then printed those same globals anyway — so a #GP reported
// the last page fault's registers under a label claiming they were its own. The
// `gLastFaultRegs == 0` guard that was supposed to catch that never fired once,
// because demand paging means a page fault has always just happened.
//
// And the globals were MACHINE-wide, not per-core: two cores faulting at the
// same moment overwrote each other's capture, so on an SMP box the report could
// describe a fault on a different CPU. That one had never been noticed at all.
//
// The cure is structural rather than careful: there is now exactly ONE prologue
// (exception_entry.S), it builds this context ON THE STACK — which makes it
// inherently per-core and re-entrant, with no globals to race — and there is
// exactly ONE function that renders it. A vector cannot report differently from
// its neighbours because there is only one piece of code that reports.
//
// Chris's ruling, and it is the right one: "unify all of the exceptions so that
// they use the same code to capture the registers, stack, stack trace, etc. and
// then the same method to display all of the dump info."

// The layout is a CONTRACT with exception_entry.S, low address first, because
// that is the order the pushes land in. Reading it top to bottom is reading the
// stack upward from the pointer the stub hands us.
//
// CHANGE THIS STRUCT OR THAT PROLOGUE AND YOU MUST CHANGE BOTH. The static
// asserts at the bottom of exception_report.c are there to make a mismatch a
// compile error rather than a garbage register dump.
typedef struct {
	// Pushed by the common prologue, rax first so it lands highest.
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

	// Pushed by the per-vector stub. The error code is a DUMMY ZERO for the
	// vectors the CPU does not push one for — normalizing there is what lets
	// every vector share one layout, and it is the same trick Linux uses.
	uint64_t vector;
	uint64_t error_code;

	// Pushed by the CPU. Long mode always pushes all five, even ring0→ring0.
	uint64_t rip, cs, rflags, rsp, ss;
} exception_context_t;

// The one entry point every vector calls. Returns only when the exception was
// RESOLVED (a demand-paging #PF, the page-fault self-test); for everything else
// it reports and halts, so the caller's iretq is simply never reached.
//
// It may also EDIT the context: writing ctx->rip changes where the iretq
// resumes, which is how the page-fault self-test lands on its recovery
// instruction. That was previously a separate global-and-hope arrangement.
void exception_dispatch(exception_context_t *ctx);

// Render the standard report — banner, core and thread, faulting instruction,
// error code, CR2/CR3, the interrupted stack pointer, every general register,
// a bounded window of the stack, and the call chain. `why` is the qualifier a
// vector wants to add ("no VMA covers this address"), or NULL for none.
//
// This is THE display path. If a new exception ever wants to say something the
// others do not, it says it in `why`, not in a private printf.
void exception_report(const exception_context_t *ctx, const char *why);

// Human name for a vector ("Page Fault (#PF)").
const char *exception_vector_name(uint64_t vector);

// Just the register block — RIP/CS/RFLAGS, RSP/SS/RBP, all sixteen GPRs, and
// the GS_BASE sanity check — rendered from a captured context. Split out so
// the ring-3 segfault report (user_fault_kill, which kills the task and keeps
// the OS) can show the same registers without adopting the fatal banner.
// `dying` has the usual meaning: wire-only, touch no log queue.
void exception_report_registers(const exception_context_t *ctx, bool dying);

// The context the shared prologue registered for THIS core, or NULL when the
// running exception arrived through the old (EXCOLD) stubs — which is exactly
// the question the shared demand pager asks to decide which reporter owns a
// fatal fault. Set on dispatch entry, cleared when a resolved fault returns.
exception_context_t *exception_current_context(void);

#endif // EXCEPTION_REPORT_H
