// exception_report.c — the ONE place an exception is captured and described.
//
// Doctrine, and the three bugs that motivated it, are in exception_report.h.
// Read that first; this file is the mechanism.
//
// A NOTE ON THE DUPLICATION IN HERE. The print macros and the two address
// sanity helpers are near-copies of the ones in simple_exceptions.c, and that
// is deliberate rather than lazy: while the EXCOLD fallback exists, the old
// reporter and this one must be unable to break each other. Sharing helpers
// would mean a mistake here could take down the very code path that exists to
// be fallen back on. The duplication dies with EXCOLD.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "exception_report.h"
#include "exceptions.h"
#include "smp.h"
#include "smp_core.h"
#include "msr.h"        // rdmsr64 — the GS_BASE sanity check
#include "paging.h"
#include "serial_logging.h"
#include "logging/log.h"
#include "sprintf.h"
#include "video.h"
#include "CONFIG.h"
#include "task.h"
#include "thread.h"

extern uintptr_t kKernelPML4v;
extern bool kLoggingInitialized;

// How much stack to show, and how deep to walk. Both bounded because a
// corrupted frame must produce a SHORT report, not an endless one.
#define EXC_STACK_WORDS   16
#define EXC_MAX_FRAMES    24

// ── Sinks ───────────────────────────────────────────────────────────────────
//
// An exception report goes to the wire ALWAYS (Chris, 2026-08-11: "it's cheap
// and it's permanent, and it's viewable outside of the session"). The printd
// copy is added only when it lands somewhere other than that same wire —
// otherwise the whole report prints twice in the default no-LOGD case.
//
// `dying` selects wire-only: a panicking core must not depend on logd being
// alive to drain a queue, and must not touch a queue lock it may itself hold.
#define EXC_EMIT(dying, fmt, ...) do { \
        printf(fmt, ##__VA_ARGS__); \
        char _exc_line[512]; \
        snprintf(_exc_line, sizeof(_exc_line), fmt, ##__VA_ARGS__); \
        serial_print_string(_exc_line); \
        if (!(dying) && !log_printd_reaches_serial()) \
            printd(DEBUG_EXCEPTIONS, "%s", _exc_line); \
    } while (0)

static bool exc_canonical(uint64_t address)
{
	uint64_t upper = address >> 47;
	return upper == 0 || upper == 0x1FFFF;
}

/// @brief Is this kernel address safe to dereference RIGHT NOW?
///
/// Rule one of any fault reporter: NEVER FAULT WHILE REPORTING A FAULT. A
/// report that faults turns a diagnosable exception into a #DF, which is
/// strictly worse than printing nothing. Every dereference below goes through
/// here, every time — not once at the top.
static bool exc_readable(uint64_t address)
{
	if (!exc_canonical(address) || (address & 7) != 0) {
		return false;
	}
	uintptr_t phys = paging_walk_paging_table((pt_entry_t *)kKernelPML4v, address);
	return phys != 0 && phys != 0xbadbadba;
}

const char *exception_vector_name(uint64_t vector)
{
	switch (vector) {
		case 0:  return "Divide Error (#DE)";
		case 1:  return "Debug (#DB)";
		case 2:  return "Non-Maskable Interrupt (NMI)";
		case 3:  return "Breakpoint (#BP)";
		case 4:  return "Overflow (#OF)";
		case 5:  return "Bound Range Exceeded (#BR)";
		case 6:  return "Invalid Opcode (#UD)";
		case 7:  return "Device Not Available (#NM)";
		case 8:  return "Double Fault (#DF)";
		case 9:  return "Coprocessor Segment Overrun";
		case 10: return "Invalid TSS (#TS)";
		case 11: return "Segment Not Present (#NP)";
		case 12: return "Stack-Segment Fault (#SS)";
		case 13: return "General Protection (#GP)";
		case 14: return "Page Fault (#PF)";
		case 16: return "x87 Floating-Point (#MF)";
		case 17: return "Alignment Check (#AC)";
		case 18: return "Machine Check (#MC)";
		case 19: return "SIMD Floating-Point (#XM)";
		case 20: return "Virtualization (#VE)";
		case 21: return "Control Protection (#CP)";
		case 28: return "Hypervisor Injection (#HV)";
		case 29: return "VMM Communication (#VC)";
		case 30: return "Security Exception (#SX)";
		default: return "Reserved/Unknown";
	}
}

/// @brief Decode a #PF error code. Only meaningful for vector 14.
static void exc_decode_pf(char *out, size_t len, uint64_t error_code)
{
	snprintf(out, len, "%s, %s, %s%s%s",
	         (error_code & 0x1)  ? "protection violation" : "page not present",
	         (error_code & 0x2)  ? "write" : "read",
	         (error_code & 0x4)  ? "user mode" : "kernel mode",
	         (error_code & 0x8)  ? ", reserved bit set" : "",
	         (error_code & 0x10) ? ", instruction fetch" : "");
}

/// @brief Walk the frame chain from the CAPTURED rbp — never a global.
///
/// Presumed hostile, every frame re-validated. The monotonicity test does most
/// of the work: stacks grow down, so each caller's frame must sit above its
/// callee's, and that single check ends every kind of broken chain there is.
static void exc_walk_chain(const exception_context_t *ctx, bool dying)
{
	EXC_EMIT(dying, "  Call chain (most recent first):\n");
	EXC_EMIT(dying, "   1) 0x%016lx   <-- faulted here\n", ctx->rip);

	// Ring 3 has no kernel chain to walk from here, and saying so beats
	// walking a lower-half frame pointer through the kernel's page tables.
	if ((ctx->cs & 3) != 0) {
		EXC_EMIT(dying, "   (ring 3 — user chain is stack_trace.c's job)\n");
		return;
	}

	uint64_t frame = ctx->rbp;
	for (int level = 2; level <= EXC_MAX_FRAMES; level++) {
		if (!exc_readable(frame) || !exc_readable(frame + 8)) {
			EXC_EMIT(dying, "   ... (frame 0x%016lx not readable — chain ends)\n", frame);
			return;
		}
		uint64_t saved_rbp = *(volatile uint64_t *)frame;
		uint64_t ret       = *(volatile uint64_t *)(frame + 8);

		if (ret == 0) {
			return;   // a clean chain terminates
		}
		EXC_EMIT(dying, "  %2d) 0x%016lx\n", level, ret);

		if (saved_rbp == 0) {
			return;   // the launch stub's floor — also clean
		}
		if (saved_rbp <= frame) {
			EXC_EMIT(dying, "   ... (frame pointer not monotonic — chain ends)\n");
			return;
		}
		frame = saved_rbp;
	}
	EXC_EMIT(dying, "   ... (depth limit reached)\n");
}

// The register block, from the CAPTURED context — this exception's own values,
// on this core, out of the frame the shared prologue built. Frequently the
// whole answer: CR2 says which address died, the registers say which pointer
// carried it there. Exported (not folded into exception_report) because the
// ring-3 segfault report needs exactly this block under its own headline.
void exception_report_registers(const exception_context_t *ctx, bool dying)
{
	EXC_EMIT(dying, ">>> RIP 0x%016lx  CS 0x%04lx  RFLAGS 0x%016lx (IF=%lu) <<<\n",
	         ctx->rip, ctx->cs, ctx->rflags, (ctx->rflags >> 9) & 1);
	EXC_EMIT(dying, ">>> RSP 0x%016lx  SS 0x%04lx  RBP 0x%016lx <<<\n",
	         ctx->rsp, ctx->ss, ctx->rbp);
	EXC_EMIT(dying, ">>> RAX 0x%016lx  RBX 0x%016lx  RCX 0x%016lx  RDX 0x%016lx <<<\n",
	         ctx->rax, ctx->rbx, ctx->rcx, ctx->rdx);
	EXC_EMIT(dying, ">>> RSI 0x%016lx  RDI 0x%016lx <<<\n", ctx->rsi, ctx->rdi);
	EXC_EMIT(dying, ">>> R8  0x%016lx  R9  0x%016lx  R10 0x%016lx  R11 0x%016lx <<<\n",
	         ctx->r8, ctx->r9, ctx->r10, ctx->r11);
	EXC_EMIT(dying, ">>> R12 0x%016lx  R13 0x%016lx  R14 0x%016lx  R15 0x%016lx <<<\n",
	         ctx->r12, ctx->r13, ctx->r14, ctx->r15);

	// GS, and ONLY GS, of the segment registers (Chris's ruling, 2026-08-10,
	// carried forward from the old reporter — a unification that silently
	// dropped it would be a regression wearing a cleanup's clothes). DS/ES/FS
	// are flat and base-ignored in long mode; CS and SS already printed above
	// with the frame. GS earns its line because get_core_local_storage() IS
	// `mov rax, [gs:0]` — a wrong GS makes every cls-> read return garbage,
	// which is the shape of a corruption family this OS spent weeks on. And
	// because os64 uses SWAPGS NOWHERE, the base has ONE correct answer at all
	// times, ring 0 and ring 3 alike: inside kCoreLocalStorage. So it is not
	// merely printed, it is CHECKED — if this line says WRONG, stop reading
	// the rest of the report and believe this line first.
	{
		uint64_t gs_base = rdmsr64(IA32_GS_BASE);
		uint64_t cls_lo = (uint64_t)&kCoreLocalStorage[0];
		uint64_t cls_hi = (uint64_t)&kCoreLocalStorage[MAX_CPUS];
		bool sane = (gs_base >= cls_lo && gs_base < cls_hi);
		EXC_EMIT(dying, ">>> GS_BASE 0x%016lx  (%s) <<<\n",
		         gs_base,
		         sane ? "in kCoreLocalStorage — ok"
		              : "*** OUTSIDE kCoreLocalStorage — GS IS WRONG ***");
	}
}

void exception_report(const exception_context_t *ctx, const char *why)
{
	core_local_storage_t *cls = get_core_local_storage();
	uint64_t cr0, cr2, cr3, cr4;
	char bits[96];

	// INTEL SYNTAX — destination first. `mov %0, cr3` READS cr3 into %0. The
	// AT&T spelling means the exact opposite and, shipped once on 2026-08-09,
	// wrote garbage INTO cr3 — zeroing the address space so the next
	// instruction fetch triple-faulted. A diagnostic that destroyed the
	// evidence it was added to collect.
	__asm__ volatile("mov %0, cr0" : "=r"(cr0));
	__asm__ volatile("mov %0, cr2" : "=r"(cr2));
	__asm__ volatile("mov %0, cr3" : "=r"(cr3));
	__asm__ volatile("mov %0, cr4" : "=r"(cr4));

	// Fatal for everything except a resolved #PF, and a resolved #PF never
	// reaches this function — so anything being reported here is dying.
	const bool dying = true;

	if (why != NULL) {
		EXC_EMIT(dying, "\n>>> EXCEPTION: %s — %s <<<\n",
		         exception_vector_name(ctx->vector), why);
	} else {
		EXC_EMIT(dying, "\n>>> EXCEPTION: %s <<<\n",
		         exception_vector_name(ctx->vector));
	}

	EXC_EMIT(dying, ">>> AP %lu (Thread 0x%08x)  vector %lu  error 0x%lx <<<\n",
	         cls ? cls->apic_id : 0, cls ? cls->threadID : 0,
	         ctx->vector, ctx->error_code);

	// The #PF error code decoded, in the SAME words the ring-3 segfault report
	// uses. One fault, one description, whichever side of the privilege
	// boundary it happened on.
	if (ctx->vector == 14) {
		exc_decode_pf(bits, sizeof(bits), ctx->error_code);
		EXC_EMIT(dying, ">>> Faulting address: 0x%016lx [%s] <<<\n", cr2, bits);
	}

	// CR2 IS LABELLED, NOT BARE. The CPU only updates it on a #PF; on a #GP it
	// still holds whatever the last page fault left, which reads as a
	// confident-looking number that means nothing. An unlabelled field that
	// lies costs more than the field that tells the truth.
	EXC_EMIT(dying, ">>> CR0 0x%016lx  CR2 0x%016lx%s <<<\n", cr0, cr2,
	         (ctx->vector == 14) ? " (the faulting address)"
	                             : " (STALE — only a #PF sets this)");
	EXC_EMIT(dying, ">>> CR3 0x%016lx  CR4 0x%016lx <<<\n", cr3, cr4);

	exception_report_registers(ctx, dying);

	if (cls != NULL && cls->currentThread != NULL) {
		task_t *task = (task_t *)cls->currentThread->ownerTask;
		if (task != NULL) {
			EXC_EMIT(dying, ">>> Excepting task: %s (id %lu) <<<\n",
			         task->exename[0] ? task->exename : "(unnamed)", task->taskID);
		}
	} else {
		EXC_EMIT(dying, ">>> No current task (core likely idle) <<<\n");
	}

	// A window of the interrupted stack. When the frame chain is garbage —
	// exactly what a corrupting bug produces — these words are often the only
	// evidence left.
	EXC_EMIT(dying, ">>> Stack at 0x%016lx: <<<\n", ctx->rsp);
	for (int i = 0; i < EXC_STACK_WORDS; i += 2) {
		uint64_t va = ctx->rsp + (uint64_t)i * 8;
		if (!exc_readable(va) || !exc_readable(va + 8)) {
			EXC_EMIT(dying, ">>>   +0x%02x: <unreadable — stopping> <<<\n", i * 8);
			break;
		}
		EXC_EMIT(dying, ">>>   +0x%02x: 0x%016lx  0x%016lx <<<\n", i * 8,
		         *(volatile uint64_t *)va, *(volatile uint64_t *)(va + 8));
	}

	exc_walk_chain(ctx, dying);

	// Best-effort drain of the log backlog, so the CONTEXT leading up to the
	// exception makes it out with us. Try-lock inside — if another core holds
	// the drain lock this does nothing, which is exactly why the report above
	// went to the wire directly. Inside the wire lock on purpose: the backlog
	// is part of this report's story.
	if (kLoggingInitialized) {
		logd_thread(false);
	}
}

// ── Dispatch ────────────────────────────────────────────────────────────────
//
// THE CURRENT CONTEXT, PER CORE. The demand pager (still in simple_exceptions.c
// while EXCOLD exists) is shared by both reporting paths, and duplicating a
// couple of hundred lines of paging policy so each path could own a copy would
// be far worse than this pointer: two demand pagers that must be kept in sync
// is a bug factory. So the pager asks "is a new-style context registered for my
// core?" and reports accordingly.
//
// Per CORE, never a single global — that was one of the three bugs this module
// exists to end (two cores faulting at once used to overwrite each other's
// capture, so the report could describe a fault on a different CPU).
static exception_context_t *kCurrentCtx[MAX_CPUS];

exception_context_t *exception_current_context(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	if (cls == NULL || cls->apic_id >= MAX_CPUS) {
		return NULL;
	}
	return kCurrentCtx[cls->apic_id];
}

void exception_dispatch(exception_context_t *ctx)
{
	core_local_storage_t *cls = get_core_local_storage();
	uint32_t core = (cls != NULL && cls->apic_id < MAX_CPUS) ? (uint32_t)cls->apic_id : 0;

	// Save-and-restore rather than set-and-clear, because exceptions NEST: a
	// #PF taken inside the pager re-enters here with its own context, and a
	// plain clear on its way out would unregister the OUTER fault's capture —
	// the exact clobber the old gLastFault* globals suffered. The contexts
	// themselves stack naturally (they live on the stack); this pointer must
	// stack with them.
	exception_context_t *prev = kCurrentCtx[core];
	kCurrentCtx[core] = ctx;

	if (ctx->vector == 14) {
		uint64_t cr2;
		__asm__ volatile("mov %0, cr2" : "=r"(cr2));

		// The demand pager. Returns for a fault it RESOLVED, which is the
		// common case and the reason this whole path is allowed to return;
		// for a fatal one it reports (through us, via the registered context)
		// and never comes back.
		handle_page_fault(cr2, ctx->error_code, ctx->rip);

		// The page-fault self-test steers the resume. This used to be done by
		// writing through a global pointer into the interrupt frame; now it is
		// simply a field, which is both clearer and per-core safe.
		if (kTestingPageFaults && kTestingPageFaultResumeRip != 0) {
			ctx->rip = kTestingPageFaultResumeRip;
		}

		kCurrentCtx[core] = prev;
		return;   // resolved — the prologue's iretq retries the instruction
	}

	// Everything else is fatal. One report, then stop: the scheduler's state
	// is not trustworthy after an unhandled exception, and an orderly shutdown
	// would ask that same machinery to walk itself to the door.
	exception_report(ctx, NULL);
	while (1) {
		__asm__ volatile("cli\nhlt\n");
	}
}

// The struct and the prologue are a contract. Break it and the register dump
// becomes confident nonsense — so break the BUILD instead.
_Static_assert(sizeof(exception_context_t) == 22 * 8,
               "exception_context_t must be exactly the 22 qwords exception_entry.S pushes");
_Static_assert(offsetof(exception_context_t, rax) == 14 * 8,
               "rax must be the last GPR pushed (highest address)");
_Static_assert(offsetof(exception_context_t, vector) == 15 * 8,
               "vector sits directly above the GPR block");
_Static_assert(offsetof(exception_context_t, rip) == 17 * 8,
               "the CPU frame starts directly above vector+error_code");
