#include <stddef.h>

#include "panic.h"
#include "BasicRenderer.h"
#include "serial_logging.h"
#include "sprintf.h"
#include "smp_core.h"
#include "task.h"
#include "stack_trace.h"   // symbolized ring-3 call chains (NOTRACE-gated)
#include "CONFIG.h"
#include "msr.h"        // rdmsr64 — GS_BASE in the fault report
#include "log.h"
#include "memory/paging.h"
#include "memory/memcpy.h"
#include "exceptions.h"
#include "exception_report.h"   // the unified path's context + reporter — the
                                // demand pager below serves BOTH paths, and
                                // asks exception_current_context() which one
                                // owns a fatal fault (see page_fault_panic)
#include "memory/vma.h"
#include "kmalloc.h"
#include "allocator.h"

// The per-core interrupt-frame save arrays, filled by the scheduler ISR
// prologue (scheduler.S) BEFORE it switches CR3 — so at panic time this is the
// RSP the interrupted code was actually standing on. Printed by
// exception_panic because a double fault is nearly always the stack, and the
// one thing the old panic never told you was which stack.
extern uint64_t mp_isrSavedRSP[];
#include "shared_object.h"

uint64_t gLastFaultRbp = 0;
uint64_t gLastFaultRsp = 0;
// Address of the 16 registers the #PF stub pushed (handler_errors.S). NULL
// until the first page fault, and only ever written by that stub.
uint64_t gLastFaultRegs = 0;

// The pushed block, low address first: pushf landed last so it sits lowest,
// then r15..rax ascending, because push walks DOWN. Change either this struct
// or the stub's push order and you must change both.
typedef struct {
	uint64_t rflags;
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} fault_regs_t;
uint64_t gLastFaultErrorCode = 0;
bool kTestingPageFaults = false;
uint64_t kTestingPageFaultResumeRip = 0;
volatile uint64_t kPageFaultCount;

static bool is_canonical_address(uint64_t address)
{
	uint64_t upper = address >> 47;
	return upper == 0 || upper == 0x1FFFF;
}

static bool address_is_mapped(uint64_t address)
{
	uintptr_t pte = paging_walk_paging_table_keep_flags((pt_entry_t*)kKernelPML4v, address, true);
	if (pte == 0xbadbadba) {
		return false;
	}
	return (pte & PAGE_PRESENT) != 0;
}

/// @brief Is this address readable RIGHT NOW, in whatever address space we are in?
///
/// address_is_mapped() asks kKernelPML4v, and for kernel pointers that is the
/// right question. It is the WRONG question for anything on a task's RSP0
/// stack — task_alloc_guarded_stack puts those at TASK-LOCAL lower-half VAs
/// that are deliberately absent from the kernel's tables (CLAUDE.md says so in
/// as many words). A fault frame lives on exactly such a stack, so asking the
/// kernel oracle about it answers "unmapped" while the CPU is standing on it
/// and pushing to it — which is precisely the wrong answer, and it cost this
/// function its first RSP line.
///
/// So: kernel tables first, then the CURRENT task's. Both are consulted
/// because a fault can arrive from either side of the boundary.
static bool fault_address_readable(uint64_t address)
{
	if (!is_canonical_address(address)) {
		return false;
	}
	if (address_is_mapped(address)) {
		return true;
	}
	core_local_storage_t *cls = get_core_local_storage();
	task_t *t = (cls != NULL) ? (task_t *)cls->task : NULL;
	if (t == NULL || t->pml4v == 0) {
		return false;
	}
	uintptr_t phys = paging_walk_paging_table((pt_entry_t *)t->pml4v, address);
	return phys != 0 && phys != 0xbadbadba;
}

// ── Where a fault report goes ───────────────────────────────────────────────
//
// These two macros live ABOVE every reporter in this file deliberately: they
// used to sit halfway down it, which is how dump_stack_trace and
// log_page_fault_bits — the two most diagnostic parts of a kernel page-fault
// report — ended up as bare printf calls that reached the framebuffer and
// NOTHING else. Not the wire, not the log. Anything above them physically
// could not use them.
//
// Emit a line to BOTH the screen and the serial port.  Exceptions must be
// diagnosable from either — a #GP that only ever appears on the framebuffer
// is invisible to headless/CI runs.  Serial output goes DIRECTLY through
// serial_print_string, bypassing the printd ring buffer entirely: this core
// is about to cli/hlt, so anything left in the buffer depends on another
// core's logd/kworker still being alive to drain it (and logd_thread(false)
// is only a try-lock — it can silently drain nothing).  A panic path must
// not have dependencies.
#define EXCEPTION_PRINT(fmt, ...) do { \
        printf(fmt, ##__VA_ARGS__); \
        char _exc_line[512]; \
        snprintf(_exc_line, sizeof(_exc_line), fmt, ##__VA_ARGS__); \
        serial_print_string(_exc_line); \
    } while (0)

// `direct` now means exactly ONE thing: THIS CORE IS DYING. A panic must not
// depend on logd being alive to drain a queue, so it writes polled serial and
// nothing else. Everything else — a ring-3 segfault, a page-fault diagnosis, a
// stack trace — goes to ALL THREE sinks: the glass, the wire, and (only when a
// userland daemon owns it) the LOGD= file.
//
// THE RULING THAT CHANGED THIS (Chris, 2026-08-11): a fault report belongs on
// the wire unconditionally — "it's cheap and it's permanent, and it's viewable
// outside of the session." He hit the old behaviour the hard way the same day:
// an exception in processSignals whose only surviving line was a RIP, because
// the LOGD= file received only the printd lines while the decoded bits and the
// stack trace went to the framebuffer of an emulator he then closed. A report
// you have to survive long enough to go read is not a report.
//
// The old split's stated reason was real, but it was solved backwards. Mixing
// direct and queued lines in ONE report scrambled it, because direct lines land
// instantly and queued ones land whenever logd next runs — so the register dump
// could print before its own headline. The cure is not to queue everything; it
// is to send every line to every sink IN THE SAME ORDER, after which each sink
// independently holds a correctly-ordered, complete report.
//
// The printd copy is added only when it lands somewhere OTHER than this same
// wire — see log_printd_reaches_serial(). Without that test the whole report
// prints twice on COM1 in the no-LOGD case, which is the default case.
#define FAULT_PRINT(direct, fmt, ...) do { \
        if (direct) { EXCEPTION_PRINT(fmt, ##__VA_ARGS__); } \
        else { \
            printf(fmt, ##__VA_ARGS__); \
            char _flt_line[512]; \
            snprintf(_flt_line, sizeof(_flt_line), fmt, ##__VA_ARGS__); \
            serial_print_string(_flt_line); \
            if (!log_printd_reaches_serial()) \
                printd(DEBUG_EXCEPTIONS, "%s", _flt_line); \
        } \
    } while (0)

//NOTE: Won't work with userland RIPs. Will need to modify to accept the CR3 for non-kernel processes once we have a userland
//
// `direct` has the same meaning as everywhere else in this file: this core is
// dying, so write the wire and do not touch a log queue. It became a parameter
// on 2026-08-11 when this walk moved INSIDE exception_panic — a report that is
// half direct and half queued arrives out of order, which is the whole reason
// FAULT_PRINT takes the flag at all.
void dump_stack_trace(uint64_t rip, bool direct)
{
	FAULT_PRINT(direct, "Stack trace (most recent call first):\n");
	FAULT_PRINT(direct, "  [0] RIP=0x%016lx\n", rip);
	FAULT_PRINT(direct, "  Captured RSP=0x%016lx RBP=0x%016lx\n", gLastFaultRsp, gLastFaultRbp);

	if (gLastFaultErrorCode & (1ull << 2)) {
		FAULT_PRINT(direct, "  <fault originated from user mode>\n");
	}

	uint64_t rbp = gLastFaultRbp;
	if (rbp == 0) {
		// TRUE, and now meaningful rather than dead code: only the #PF stub
		// captures RBP, and every other stub CLEARS it on entry (see
		// handler_errors.S). Before that, a #GP printed the last page fault's
		// frame pointer and walked a stack that had nothing to do with it.
		FAULT_PRINT(direct, "  <no frame pointer captured for this exception>\n");
		return;
	}

	if (!is_canonical_address(rbp)) {
		FAULT_PRINT(direct, "  <frame pointer 0x%016lx non-canonical>\n", rbp);
		return;
	}

	if (!address_is_mapped(rbp) || !address_is_mapped(rbp + sizeof(uint64_t))) {
		FAULT_PRINT(direct, "  <frame pointer 0x%016lx unmapped>\n", rbp);
		return;
	}

	const uint32_t max_frames = 16;
	for (uint32_t frame = 1; frame < max_frames; frame++) {
		uint64_t *frame_ptr = (uint64_t*)rbp;
		uint64_t next_rbp = frame_ptr[0];
		uint64_t return_address = frame_ptr[1];

		if (!is_canonical_address(return_address)) {
			FAULT_PRINT(direct, "  [%u] <non-canonical return address 0x%016lx>\n", frame, return_address);
			break;
		}

		FAULT_PRINT(direct, "  [%u] RIP=0x%016lx\n", frame, return_address);

		if (next_rbp == 0) {
			break;
		}
		if (next_rbp <= rbp) {
			FAULT_PRINT(direct, "  <next frame pointer 0x%016lx not higher than current 0x%016lx>\n", next_rbp, rbp);
			break;
		}
		if (!is_canonical_address(next_rbp)) {
			FAULT_PRINT(direct, "  <next frame pointer 0x%016lx non-canonical>\n", next_rbp);
			break;
		}
		if (!address_is_mapped(next_rbp) || !address_is_mapped(next_rbp + sizeof(uint64_t))) {
			FAULT_PRINT(direct, "  <next frame pointer 0x%016lx unmapped>\n", next_rbp);
			break;
		}

		rbp = next_rbp;
	}
}

// The interrupted register set, and optionally the stack under it.
//
// Registers are UNCONDITIONAL: they are free (already pushed), they are always
// safe (the block is on the stack we are standing on), and they answer
// questions no other field can — which register held the bad pointer, and
// whether it looks like data, a small integer, or copied program text. That
// last one is not hypothetical: this OS has twice been debugged by noticing a
// pointer whose bytes decoded as x86 instructions.
//
// The STACK DUMP is gated behind DEBUG_DETAILED, and Chris called that right.
// It is bounded and every line is mapped-checked, so it cannot fault — but it
// is long, and a wall of qwords is exactly the noise that makes people stop
// reading crash reports. Ask for it when you want it.
void dump_fault_registers(bool direct)
{
	if (gLastFaultRegs == 0) {
		return;   // no fault has been through the capturing stub yet
	}
	const fault_regs_t *r = (const fault_regs_t *)gLastFaultRegs;

	FAULT_PRINT(direct, ">>> RAX 0x%016lx  RBX 0x%016lx  RCX 0x%016lx  RDX 0x%016lx <<<\n",
	                r->rax, r->rbx, r->rcx, r->rdx);
	FAULT_PRINT(direct, ">>> RSI 0x%016lx  RDI 0x%016lx  RBP 0x%016lx <<<\n",
	                r->rsi, r->rdi, r->rbp);

	// RSP, CS, SS and the TRUE RFLAGS — none of which are in the pushed block.
	//
	// RSP is not pushable (push saves everything except the thing doing the
	// pushing), so it has to come from the CPU's own fault frame, which
	// gLastFaultRsp points at: [+0]=error code, +8=RIP, +16=CS, +24=RFLAGS,
	// +32=RSP, +40=SS. That layout is guaranteed because LONG MODE ALWAYS
	// PUSHES ALL FIVE QWORDS — even ring0->ring0, unlike 32-bit — which is a
	// fact this codebase paid for twice (see the interrupt-frame rule in
	// CLAUDE.md; reading RSP by arithmetic instead of from the +32 FIELD is
	// how you get a byte-shifted value, because the CPU aligns RSP to 16
	// before pushing).
	//
	// And the RFLAGS here is the INTERRUPTED code's, which the pushf in the
	// stub is not: the stub does `cli` first, so its copy always reads IF=0
	// no matter what the faulting code was running with. A flags register that
	// lies about interrupt state is worse than no flags register — printing
	// the frame's copy is the whole point.
	if (gLastFaultRsp != 0 && fault_address_readable(gLastFaultRsp) &&
	    fault_address_readable(gLastFaultRsp + 40)) {
		const volatile uint64_t *f = (const volatile uint64_t *)gLastFaultRsp;
		FAULT_PRINT(direct, ">>> RSP 0x%016lx  CS 0x%04lx  SS 0x%04lx  RFLAGS 0x%08lx (interrupted) <<<\n",
		                f[4], f[2] & 0xFFFF, f[5] & 0xFFFF, f[3]);
	} else {
		FAULT_PRINT(direct, ">>> RSP <fault frame at 0x%016lx unreadable> <<<\n", gLastFaultRsp);
	}

	// GS, and ONLY GS, of the segment registers (Chris's call, 2026-08-10).
	//
	// DS/ES/FS are flat and base-ignored in long mode — their selectors say
	// nothing and would be four columns of noise. CS and SS earn their place
	// above because the RPL bits name the ring. GS earns its place for a reason
	// specific to this kernel: get_core_local_storage() IS `mov rax, [gs:0]`,
	// so GS is the core-local pointer, and a wrong GS makes every single cls->
	// read return garbage. That is not a hypothetical failure mode here — it is
	// the shape of the corruption family this OS spent weeks on.
	//
	// The SELECTOR is the useless half; in long mode the base comes from
	// IA32_GS_BASE, so that MSR is what gets printed. And because os64 uses
	// SWAPGS NOWHERE (verified — the base is written once per core at bring-up
	// in smp_core.c and never swapped), this value has one correct answer at
	// all times, in ring 0 and ring 3 alike: it must point into the
	// kCoreLocalStorage array. So we do not merely print it, we CHECK it — a
	// number a reader has to validate by hand is a number that gets skimmed
	// past. If this ever says WRONG, stop reading the rest of the report and
	// believe this line first.
	{
		uint64_t gs_base = rdmsr64(IA32_GS_BASE);
		uint64_t cls_lo = (uint64_t)&kCoreLocalStorage[0];
		uint64_t cls_hi = (uint64_t)&kCoreLocalStorage[MAX_CPUS];
		bool sane = (gs_base >= cls_lo && gs_base < cls_hi);
		FAULT_PRINT(direct, ">>> GS_BASE 0x%016lx  (%s) <<<\n",
		                gs_base,
		                sane ? "in kCoreLocalStorage — ok"
		                     : "*** OUTSIDE kCoreLocalStorage — GS IS WRONG ***");
	}
	FAULT_PRINT(direct, ">>> R8  0x%016lx  R9  0x%016lx  R10 0x%016lx  R11 0x%016lx <<<\n",
	                r->r8, r->r9, r->r10, r->r11);
	FAULT_PRINT(direct, ">>> R12 0x%016lx  R13 0x%016lx  R14 0x%016lx  R15 0x%016lx <<<\n",
	                r->r12, r->r13, r->r14, r->r15);

	if ((kDebugLevel & DEBUG_DETAILED) != DEBUG_DETAILED) {
		return;
	}

	// Sixteen qwords of stack, two per line, each PROVEN mapped before it is
	// read. Same rule as the ring-3 walker: a diagnostic that faults turns a
	// readable panic into a #DF, which is a strictly worse day.
	uint64_t sp = gLastFaultRsp;
	if (sp == 0 || !is_canonical_address(sp)) {
		return;
	}
	FAULT_PRINT(direct, ">>> Stack at 0x%016lx: <<<\n", sp);
	for (int i = 0; i < 16; i += 2) {
		uint64_t a = sp + (uint64_t)i * 8;
		if (!fault_address_readable(a) || !fault_address_readable(a + 8)) {
			FAULT_PRINT(direct, ">>>   +0x%02x: <unmapped — stopping> <<<\n", i * 8);
			break;
		}
		FAULT_PRINT(direct, ">>>   +0x%02x: 0x%016lx  0x%016lx <<<\n",
		                i * 8, *(volatile uint64_t *)a, *(volatile uint64_t *)(a + 8));
	}
}


void exception_panic(const char* message, uint64_t rip, uint64_t error_code) {
    core_local_storage_t* core = get_core_local_storage();

    // One narrator per report (exception_report.h) — without this, two cores
    // faulting together braid their reports character-by-character on COM1,
    // which is how the 2026-08-11 soak turned two clean reports into confetti.
    exception_wire_lock();

    EXCEPTION_PRINT("\n>>> EXCEPTION PANIC: %s <<<                      \n", message);
    EXCEPTION_PRINT(">>> AP %lu (Thread 0x%08x) <<<                        \n", core->apic_id, core->threadID);
    EXCEPTION_PRINT(">>> Faulting instruction: 0x%016lx <<<             \n", rip);

    if (error_code != 0xFFFFFFFFFFFFFFFF) {
        EXCEPTION_PRINT(">>> Error Code: 0x%lx <<<                          \n", error_code);
    }

    // CR2 + CR3 + the interrupted RSP, ALWAYS, for every exception (added
    // 2026-08-09). Not decoration — these three are what turn an unexplainable
    // #DF into a diagnosis, and their absence cost a full day.
    //
    // The story: a reproducible double fault against /bin/hog printed a task
    // name and a faulting instruction, and nothing else. Everything that
    // actually mattered had to be dug out of the QEMU monitor by hand — CR2
    // said 0x1023bf98, CR3 said kKernelPML4 was live, and the two together
    // said "this core faulted touching a task-local stack from inside the
    // kernel address space", which IS the bug, in one line. The kernel knew
    // all three at panic time and simply never said so.
    //
    // CR2 is the address that faulted (meaningful for #PF and, as above, for
    // the first fault behind a #DF). CR3 names the address space it faulted
    // IN — the pair is what makes "unmapped" mean something. RSP is the third
    // leg: a #DF is nearly always the stack, so print the stack.
    // INTEL SYNTAX — destination first. This file compiles under -masm=intel
    // like the rest of the kernel, so `mov %0, cr3` READS cr3 into %0. The
    // AT&T spelling (`mov %%cr3, %0`) means the exact OPPOSITE here: it WRITES
    // %0 into cr3. I shipped that inverted version for about an hour on
    // 2026-08-09, and it stored a garbage register into CR2 and then into CR3
    // — zeroing the address space, so the next instruction FETCH page-faulted,
    // which page-faulted again delivering it, and the machine triple-faulted.
    // A diagnostic that destroyed the evidence it was added to collect. The
    // idiom to copy lives in handle.c:129 and syscall.c:228.
    uint64_t cr2_val, cr3_val;
    __asm__ volatile("mov %0, cr2" : "=r"(cr2_val));
    __asm__ volatile("mov %0, cr3" : "=r"(cr3_val));
    // CR2 IS LABELLED, NOT BARE — it is the page-fault address register, and
    // the CPU only updates it on a #PF. On a #GP (or #UD, or #DF) it still
    // holds whatever the LAST page fault left there, which reads as a
    // confident-looking number that means nothing. Chris distrusted exactly
    // that on the first #GP this printed, and was right to; an unlabelled
    // field that lies costs more than the field that tells the truth (the
    // DEBUG_TASKSWITCH doctrine, applied to a panic line). Kept rather than
    // suppressed because for the #PF case it is THE datum.
    EXCEPTION_PRINT(">>> CR2: 0x%016lx (page-fault address; STALE unless this is a #PF)  CR3: 0x%016lx <<<\n",
                    cr2_val, cr3_val);
    EXCEPTION_PRINT(">>> Interrupted RSP: 0x%016lx <<<      \n",
                    mp_isrSavedRSP[core->apic_id]);

    // The register set the fault interrupted. Free (already pushed by the
    // stub), always safe, and frequently the whole answer — CR2 says WHAT
    // address died, the registers say WHICH POINTER carried it there.
    dump_fault_registers(true);   // panic: direct, no daemon dependency

    // The call chain, for EVERY exception rather than only #PF (2026-08-11,
    // Chris: "all exception paths should print the same thing to the same
    // places"). It used to be called by hand from the three fatal page-fault
    // sites, which is why a #GP report had no chain at all and a #PF report
    // printed one BEFORE its own headline.
    //
    // Honest for every vector because of the companion change in
    // handler_errors.S: only the #PF stub captures a frame pointer, and every
    // other stub now CLEARS it, so this prints a real walk after a #PF and
    // "<no frame pointer captured for this exception>" after anything else —
    // instead of confidently walking the last page fault's stack.
    dump_stack_trace(rip, true);

    if (core->currentThread) {
		task_t *task = (task_t*)core->currentThread->ownerTask;

        EXCEPTION_PRINT(">>> Excepting Task: %s <<<                         \n", task->path);
    } else {
        EXCEPTION_PRINT(">>> No current task (core likely idle) <<<         \n");
    }

	// Best-effort drain of the printd ring buffer too, so the log context
	// LEADING UP to the exception makes it out with us.  Try-lock inside —
	// if another core holds the drain lock this does nothing, which is why
	// the exception report itself went directly to serial above.
	if (kLoggingInitialized) {
		logd_thread(false);
	}

	// Free the wire BEFORE halting — the next core's report must not have to
	// barge past this one's corpse.
	exception_wire_unlock();

	while (1) { __asm__ volatile ("cli\nhlt\n"); }
}

void handle_divide_by_zero(uint64_t rip) {
    exception_panic("Divide by zero (#DE) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_invalid_opcode(uint64_t rip) {
    exception_panic("Invalid opcode (#UD) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

// A double fault means the CPU could not deliver some FIRST exception —
// nearly always because the stack it had to push onto was unusable. So the
// interesting evidence is not "a #DF happened", it is WHAT RSP and RFLAGS
// were when it did: an RSP outside any stack region, or RFLAGS carrying
// bits no kernel thread should have (TF, NT, IOPL≠0, AC), says the thread
// was dispatched with a corrupt register frame rather than that it did
// something wrong.
void handle_double_fault_frame(uint64_t rip, uint64_t rsp, uint64_t rflags)
{
	core_local_storage_t *core = get_core_local_storage();
	// EXCEPTION_PRINT, not FAULT_PRINT: a #DF is as dying as it gets. The wire
	// copy is the only one that can be trusted to survive, and touching a log
	// queue here risks a lock this core may already hold.
	EXCEPTION_PRINT("\n>>> DOUBLE FAULT on AP %lu <<<\n", core ? core->apic_id : 0);
	EXCEPTION_PRINT(">>> RIP=0x%016lx RSP=0x%016lx RFLAGS=0x%016lx <<<\n", rip, rsp, rflags);
	// Name the usual suspects rather than making a reader decode bits.
	if (rflags & 0x100)   EXCEPTION_PRINT(">>>   RFLAGS.TF set — single-step on a kernel thread <<<\n");
	if (rflags & 0x4000)  EXCEPTION_PRINT(">>>   RFLAGS.NT set <<<\n");
	if (rflags & 0x3000)  EXCEPTION_PRINT(">>>   RFLAGS.IOPL != 0 <<<\n");
	if (rflags & 0x40000) EXCEPTION_PRINT(">>>   RFLAGS.AC set <<<\n");
	exception_panic("Double Fault (#DF) — the first fault could not be delivered", rip, 0);
}

void handle_double_fault(uint64_t rip) {
    exception_panic("Double fault (#DF) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_general_protection_fault(uint64_t error_code, uint64_t rip) {
    exception_panic("General Protection Fault (#GP) occurred!", rip, error_code);
}

// Every CPU exception that had no gate until 2026-08-01. They share one
// handler because the point isn't to RECOVER from them — it's to say which
// one happened. An unpopulated gate makes the CPU raise #GP instead, whose
// error code names the IDT slot it couldn't deliver; that is how a debug
// exception spent an afternoon impersonating a protection fault in the
// idle loop. A named panic costs nothing and answers the first question.
static const char *exception_name(uint64_t vector)
{
    switch (vector)
    {
        case 1:  return "Debug Exception (#DB) — single-step or breakpoint";
        case 2:  return "Non-Maskable Interrupt (NMI)";
        case 3:  return "Breakpoint (#BP) — an int3 executed";
        case 4:  return "Overflow (#OF)";
        case 5:  return "BOUND Range Exceeded (#BR)";
        case 7:  return "Device Not Available (#NM) — FPU/SSE used before CR0 setup";
        case 10: return "Invalid TSS (#TS)";
        case 11: return "Segment Not Present (#NP)";
        case 12: return "Stack-Segment Fault (#SS)";
        case 16: return "x87 Floating-Point Error (#MF)";
        case 17: return "Alignment Check (#AC)";
        case 19: return "SIMD Floating-Point Exception (#XM)";
        default: return "Unexpected CPU exception";
    }
}

void handle_unexpected_exception(uint64_t vector, uint64_t error_code, uint64_t rip)
{
    // The vector number goes in the banner too: a reader who doesn't have
    // the Intel manual open still gets something greppable.
    char msg[160];
    snprintf(msg, sizeof(msg), "%s [vector %lu]", exception_name(vector), vector);
    exception_panic(msg, rip, error_code);
}


/// @brief Decode a #PF error code into the bracketed form both fault reports use.
///
/// ONE spelling of these five bits, shared by the ring-3 segfault report
/// (user_fault_kill) and the ring-0 panic (page_fault_panic). They used to be
/// two: a bracketed one-liner for segfaults and a multi-line bullet list for
/// panics, so the same fault described itself two different ways depending on
/// which side of the privilege boundary it happened on.
static void page_fault_decode(char *out, size_t len, uint64_t error_code)
{
	snprintf(out, len, "%s, %s, %s%s%s",
	         (error_code & 0x1) ? "protection violation" : "page not present",
	         (error_code & 0x2) ? "write" : "read",
	         (error_code & 0x4) ? "user mode" : "kernel mode",
	         (error_code & 0x8) ? ", reserved bit set" : "",
	         (error_code & 0x10) ? ", instruction fetch" : "");
}

/// @brief The single door every FATAL page fault leaves by.
///
/// Before this existed the eight fatal exits in handle_page_fault each called
/// panic() directly, which meant a kernel #PF got a bespoke, thinner report
/// than any other exception: no register dump (the bitter irony being that the
/// #PF stub is the ONLY one that captures registers), no AP/thread line, no
/// CR3, no interrupted RSP. Chris caught it on a live #PF during a VT soak:
/// "in the end ... its a #PF. Can we make this error consistent with others?"
///
/// So it is a #PF first and a reason second — the vector names the event, the
/// `why` qualifies it, and exception_panic prints exactly what #GP, #UD, #DE
/// and #MC print.
static void __attribute__((noreturn)) page_fault_panic(const char *why, uint64_t cr2,
                                                       uint64_t error_code, uint64_t rip)
{
	// THE SEAM BETWEEN THE TWO REPORTING PATHS (2026-08-11). The demand pager
	// is shared — duplicating ~250 lines of paging policy so each path could
	// own a copy would be a bug factory — so this door asks which path the
	// fault ARRIVED through. The unified prologue (exception_entry.S)
	// registered a per-core context on entry; the old EXCOLD stubs never do.
	// A registered context means the full capture exists: report from it.
	exception_context_t *ctx = exception_current_context();
	if (ctx != NULL) {
		// exception_report prints the faulting address and the decoded error
		// bits itself for vector 14 — `why` carries only the pager's verdict.
		exception_report(ctx, why);
		while (1) { __asm__ volatile("cli\nhlt\n"); }
	}

	char bits[96];
	char msg[256];

	page_fault_decode(bits, sizeof(bits), error_code);
	snprintf(msg, sizeof(msg), "Page fault (#PF) — %s: address 0x%016lx [%s]",
	         why, cr2, bits);

	exception_panic(msg, rip, error_code);
	__builtin_unreachable();
}

// (Here lay log_page_fault_bits, which printed the same five error-code bits as
// a multi-line bullet list — "  Write (bit 1)" — while the ring-3 segfault
// report printed them as a bracketed one-liner. Same fault, two descriptions,
// chosen by which side of the privilege boundary it happened on. Retired
// 2026-08-11 in favour of page_fault_decode, which both reports now share.)

// A user-mode fault the demand pager can't resolve is the APP's bug, not the
// kernel's: kill the task, keep the OS.  This is the segmentation fault, and
// the exit code is 139 by the oldest convention in Unix — 128 + 11, signal 11
// being SIGSEGV's number since the Seventh Edition signal table (os64 doesn't
// deliver signals for this yet, but the exit code keeps the lineage so shell
// scripts and muscle memory read it correctly).
//
// Safe from #PF context: interrupts are already masked (the stub cli'd), we
// are on the CPU-switched kernel interrupt stack, and task_exit() is built
// for exactly this situation — it re-points RSP at that stack's top, switches
// to kKernelPML4, and schedules away, never returning to the faulting frame.
static void __attribute__((noreturn)) user_fault_kill(task_t *task, const char *why,
    uint64_t cr2, uint64_t error_code, uint64_t rip)
{
	// One narrator per report (exception_report.h). Ring-3 kills are the most
	// COMMON concurrent reporters — two tops segfaulting on two cores is
	// exactly what braided the 2026-08-11 soak log — and this whole report,
	// headline through call chain, must land on the wire as one story.
	exception_wire_lock();

	// The headline, on EVERY sink. DEBUG_EXCEPTIONS is permanently on (see
	// CONFIG.h), so the printd is not a hedge — it is the copy a script greps.
	// The direct serial write is the copy that survives the session.
	FAULT_PRINT(false, "\nSegmentation fault: %s (task %lu, %s)\n",
	       task->exename[0] ? task->exename : "(unnamed)", task->taskID, why);

	// The error code decoded, because "error=0x7" is a number and "write to a
	// present page from user mode" is a diagnosis. These five bits answer the
	// first three questions anyone asks: was it a read or a write, was the
	// page there at all, and was it us or the kernel.
	{
		char bits[96];
		page_fault_decode(bits, sizeof(bits), error_code);
		FAULT_PRINT(false, "  address 0x%016lx  RIP 0x%016lx  error 0x%lx [%s]\n",
		            cr2, rip, error_code, bits);
	}

	// The registers the program died holding — same argument as the kernel
	// panic path: CR2 names the address, the registers name the pointer.
	// Same seam as page_fault_panic: a registered context means the unified
	// prologue captured this exception's own registers — use them, and hand
	// the walker the frame pointer from the same instant. Otherwise fall back
	// to the globals the old #PF stub maintains.
	exception_context_t *ctx = exception_current_context();
	if (ctx != NULL) {
		exception_report_registers(ctx, false);  // the OS survives this — ordinary log, in order

		// SEGFAULT FORENSICS (born as the hog -n 6 stampede hunt's TEMP DIAG,
		// 2026-08-14; promoted to permanent at the 2026-08-15 review — three
		// lines per segfault, and exactly the three that discriminate the
		// hard theories). A fault frame that contradicts itself — push-rbp
		// at RIP but a read fault at an unrelated address — means frame and
		// CR2 are not the same instant. What settles it: the CR3 actually
		// loaded vs the task the core believes it runs, which thread the
		// scheduler thinks is here, and whether the frame's own RSP/RIP/CR2
		// translate under this task's tables. If CR3 belongs to someone
		// else, it is a resume/migration mix-up wearing a segfault's clothes.
		{
			uint64_t liveCr3;
			__asm__ volatile("mov %0, cr3" : "=r"(liveCr3));
			core_local_storage_t *dcls = get_core_local_storage();
			thread_t *dthr = (dcls != NULL) ? dcls->currentThread : NULL;
			uintptr_t rspPhys = task->pml4v ? paging_walk_paging_table((pt_entry_t *)task->pml4v, ctx->rsp) : 0;
			uintptr_t ripPhys = task->pml4v ? paging_walk_paging_table((pt_entry_t *)task->pml4v, ctx->rip) : 0;
			uintptr_t cr2Phys = task->pml4v ? paging_walk_paging_table((pt_entry_t *)task->pml4v, cr2) : 0;
			FAULT_PRINT(false, "  forensics: live CR3 0x%016lx  task->pml4 0x%016lx  cls->task %lu  thread 0x%08x\n",
			            liveCr3, (uint64_t)(uintptr_t)task->pml4, task->taskID,
			            dthr ? dthr->threadID : 0);
			FAULT_PRINT(false, "  forensics: walk(task): rsp->0x%016lx  rip->0x%016lx  cr2->0x%016lx  [0xbadbadba = unmapped]\n",
			            rspPhys, ripPhys, cr2Phys);
			// The smoking gun for the scribbled-text theory: the bytes the
			// CPU actually executed, read through the task's own walk + HHDM.
			// If RIP symbolizes to a known function but these bytes aren't
			// that function's prologue, the text frame was overwritten at
			// runtime (freed-and-reallocated, or a stray write) — the fault
			// frame was honest and the on-disk disassembly was the lie.
			if (ripPhys != 0 && ripPhys != 0xbadbadba)
			{
				uint8_t *code = (uint8_t *)(((ripPhys & ~(uintptr_t)0xFFF) | kHHDMOffset)
				                            + (ctx->rip & 0xFFF));
				FAULT_PRINT(false, "  forensics: bytes at RIP: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
				            code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
				            code[8], code[9], code[10], code[11], code[12], code[13], code[14], code[15]);
			}
		}

		// And the call chain. NOTRACE disables everything inside.
		stack_trace_user(task, rip, ctx->rbp);
	} else {
		dump_fault_registers(false);  // the OS survives this — ordinary log, in order

		// And the call chain. gLastFaultRbp is captured by the #PF stub before it
		// calls us (handler_errors.S), which is the only reason a trace is possible
		// this far from the fault. NOTRACE disables everything below.
		stack_trace_user(task, rip, gLastFaultRbp);
	}

	// Report told — free the wire BEFORE task_exit, which never returns.
	exception_wire_unlock();

	task->retVal = 139;   // 128 + SIGSEGV(11)
	task_exit();
	__builtin_unreachable();
}

void handle_page_fault(uint64_t cr2, uint64_t error_code, uint64_t rip)
{
    if (kTestingPageFaults)
    {
        if (gLastFaultRsp != 0 && kTestingPageFaultResumeRip != 0)
        {
            uint64_t *stack = (uint64_t *)gLastFaultRsp;
            // Error code at stack[0], return RIP at stack[1]
            stack[1] = kTestingPageFaultResumeRip;
        }
        printd(DEBUG_EXCEPTIONS, "\tPage fault occurred during test mode, returning without halt.\n");
        // Clear the CR2
        __asm__ __volatile__(
            "xor rax, rax\n\t"
            "mov cr2, rax\n\t");
        return;
    }

    // DEBUG_DEMAND_PAGING, not EXCEPTIONS: most faults that reach this line
    // are the demand pager being asked to do its job, and EXCEPTIONS is
    // always-on — routine paging traffic was drowning the default log. Every
    // FATAL path below re-announces RIP/CR2/error on DEBUG_EXCEPTIONS (plus
    // decoded bits and a stack trace) before it panics, so demoting this
    // line loses nothing when a fault is actually news.
    printd(DEBUG_DEMAND_PAGING, "PAGE FAULT at RIP=0x%016lx, CR2=0x%016lx, ERROR=0x%lx\n", rip, cr2, error_code);

    // Guard against faults BEFORE per-core state exists (early boot: CLS not
    // allocated and/or GS base not programmed). Without this, [gs:0] returns
    // junk, ->task is junk, and vma_lookup faults on the junk pointer — the
    // handler then re-enters itself until the stack dies in a triple fault,
    // taking the diagnosable panic below with it.
    task_t *task = kCLSInitialized ? get_core_local_storage()->task : NULL;
    if (!task)
    {
        page_fault_panic("no task context (early boot?)", cr2, error_code, rip);
    }
    vma_t *vma = vma_lookup(task, cr2);
    if (!vma)
    {
        // Error bit 2 = the faulting access came from ring 3: the app chased
        // a wild pointer.  Its problem, not ours — segfault the task.  (The
        // kernel-mode paths below stay panics: a ring-0 no-VMA fault is a
        // kernel bug, and the syscall copy helpers pre-validate user ranges
        // precisely so a bad user pointer can never fault down here in ring 0.)
        if (error_code & 0x4)
            user_fault_kill(task, "access to unmapped address", cr2, error_code, rip);
        // A fault in the HHDM range is the lazy-HHDM tripwire firing (see
        // paging.h): physical memory is only HHDM-mapped while allocated, so
        // this is a use-after-free, a wild physical-address dereference, or
        // memory that never came from the allocator (e.g. MMIO that needs an
        // explicit mapping). Say so, rather than the generic no-VMA message.
        if (kHHDMMaintenanceEnabled && cr2 >= kHHDMOffset && cr2 < kHHDMOffset + 0x1000000000000UL)
        {
            char hhdm[128];
            snprintf(hhdm, sizeof(hhdm),
                     "HHDM access to unallocated physical 0x%016lx — use-after-free or wild pointer?",
                     cr2 - kHHDMOffset);
            page_fault_panic(hhdm, cr2, error_code, rip);
        }
        page_fault_panic("no VMA covers this address", cr2, error_code, rip);
    }

    // Per-fault detail: rides DETAILED so the base demand-paging channel
    // stays a readable two-lines-per-fault (announce + resolution).
    printd(DEBUG_DEMAND_PAGING | DEBUG_DETAILED, "Found VMA: 0x%016lx - 0x%016lx (prot=0x%x, cow=%d)\n", vma->start, vma->end, vma->prot, vma->cow);

    // Calculate aligned fault address
    uintptr_t aligned = cr2 & ~(PAGE_SIZE - 1);

    // Classify the fault from the error code:
    //   bit 0 (P): 0 = page not present, 1 = page present (protection violation)
    //   bit 1 (W): 0 = read fault,        1 = write fault
    bool page_was_present = (error_code & 0x1) != 0;
    bool was_write        = (error_code & 0x2) != 0;

    if (page_was_present && was_write && vma->cow)
    {
        // Copy-on-Write fault: the page is present but mapped read-only because
        // it is (or was) shared with another task.  Allocate a private copy,
        // duplicate the content, then remap writable so the faulting store can retry.
        uintptr_t old_phys = paging_walk_paging_table((pt_entry_t *)task->pml4v, aligned);
        if (!old_phys || old_phys == 0xbadbadba)
            page_fault_panic("CoW fault — page table walk did not find the original page",
                             cr2, error_code, rip);

        // kmalloc_aligned guarantees the page is accessible via HHDM in kKernelPML4.
        // allocate_memory_aligned() does not make that guarantee.
        void *new_virt = kmalloc_aligned(PAGE_SIZE);
        if (!new_virt)
            page_fault_panic("CoW fault — failed to allocate replacement page",
                             cr2, error_code, rip);
        uintptr_t new_phys = (uintptr_t)new_virt - kHHDMOffset;

        // Copy the old page's content via HHDM — the source physical page is
        // accessible at (old_phys | kHHDMOffset); the dest is new_virt directly.
        memcpy(new_virt,
               (void *)(old_phys | kHHDMOffset),
               PAGE_SIZE);

        // Remap the virtual address to the new private page, now writable.
        uint64_t cow_flags = PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
        paging_map_page((pt_entry_t *)task->pml4v, aligned, new_phys, cow_flags);

        // paging_map_page does not flush the TLB on map (only on unmap), so we
        // must invalidate this entry explicitly or the CPU retries against the
        // stale read-only TLB entry and faults again immediately.
        __asm__ volatile("invlpg [%0]" :: "r"(aligned) : "memory");

        printd(DEBUG_DEMAND_PAGING, "CoW: 0x%016lx privatised (old phys 0x%016lx -> new phys 0x%016lx)\n",
               aligned, old_phys, new_phys);
        kPageFaultCount++;
        return;
    }

    if (page_was_present)
    {
        // Ring-3 protection violation on a non-CoW page (write to read-only
        // data, jump into no-exec, etc.): the app's bug — segfault the task.
        // Ring-0 violations fall through to the diagnosing panic below.
        if (error_code & 0x4)
            user_fault_kill(task, "protection violation", cr2, error_code, rip);

        // Page is present but the access was denied and this VMA is not CoW.
        // This is a genuine protection violation, not a recoverable fault.
        // Decode the error bits into the panic message rather than assuming
        // write-to-read-only: a USER instruction fetch through intermediate
        // tables lacking PAGE_USER lands here too (that exact fault, error
        // 0x15, is how ring-3 bring-up found the paging_map_page U/S bug),
        // and a wrong message sends the reader hunting in the wrong place.
        page_fault_panic("protection violation on a present non-CoW page",
                         cr2, error_code, rip);
    }

    // Demand page fault: page is not present yet.
    if (vma->flags & MAP_SHARED_LIBRARY)
    {
        // This VMA belongs to a dynamically-linked image (library or main
        // executable) — vma->file is a shared_object_t*, not a vfs_file_t*.
        // Resolution goes through the per-image page cache instead of a
        // plain per-VMA file read: whichever task touches a given page
        // first reads it from the file and applies that page's
        // relocations; every task after gets the same physical page. The
        // symbols those relocations reference resolve against the OBJECT'S
        // own dependency scope (so + so->deps, inside shared_object.c) —
        // deliberately not this task's view, since the resolved page is
        // cached and shared with every other task that maps this object.
        // Never PAGE_WRITE here even for a writable segment — vma->cow
        // (checked above) governs the write path separately, through the
        // existing, unmodified CoW branch.
        shared_object_t *so = (shared_object_t *)vma->file;
        size_t page_idx = (aligned - so->load_bias) / PAGE_SIZE;

        uintptr_t phys = shared_object_resolve_page(so, page_idx);
        if (!phys)
            page_fault_panic("failed to resolve a shared-object page",
                             cr2, error_code, rip);

        paging_map_page((pt_entry_t *)task->pml4v, aligned, phys, PAGE_PRESENT | PAGE_USER);
        kPageFaultCount++;
        return;
    }

    // Ordinary demand page fault: page is not present yet.  Resolve via the
    // VMA's backing (static executables, anonymous memory).
    uintptr_t phys = vma_resolve_backing_page(vma, cr2);
    if (!phys)
        page_fault_panic("failed to resolve the page", cr2, error_code, rip);

    // Map the page into task's address space
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (vma->prot & PROT_WRITE)
        flags |= PAGE_WRITE;

    paging_map_page((pt_entry_t *)task->pml4v, aligned, phys, flags);

    printd(DEBUG_DEMAND_PAGING, "Mapped page at 0x%016lx with flags 0x%lx\n", aligned, flags);

    kPageFaultCount++;
}


void handle_machine_check(uint64_t rip) {
    exception_panic("Machine Check (#MC) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}
