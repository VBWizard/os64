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
#include "driver/system/x86_64.h"   // rdtsc — the wire lock's barge timer
#include "paging.h"
#include "symbols.h"    // kernel .symtab names for the call chain
#include "serial_logging.h"
#include "logging/log.h"
#include "sprintf.h"
#include "video.h"
#include "CONFIG.h"
#include "task.h"
#include "thread.h"
#include "BasicRenderer.h"      // the glass: bust the lock, kill the throttle, home the cursor
#include "gui/compositor.h"     // gui_emergency_disable — same escape hatch panic() uses
#include "tty.h"                // tty_emergency_direct — ditto, one layer up
#include "io.h"                 // inb — the pager reads the 8042 by hand (IF=0, no IRQs)
#include "watchpoint.h"         // the #DB seam: which watchpoint fired, and does it resume

extern uintptr_t kKernelPML4v;
extern bool kLoggingInitialized;
extern uint64_t kCPUCyclesPerSecond;

// ── The wire lock ────────────────────────────────────────────────────────────
//
// Two cores faulted at once on 2026-08-11 (two tops, two APs) and their
// reports interleaved CHARACTER BY CHARACTER on COM1 — forty lines of two
// reports braided into confetti. The direct polled write remains the right
// transport for a dying report (no queues, no daemon dependencies); it just
// needs exactly one narrator at a time.
//
// Three properties, each one load-bearing, each one paid for the same night:
//
// - REENTRANT PER CORE. The same soak proved the reporter can itself fault
//   mid-report (a poisoned currentThread #GP'd in the excepting-task block).
//   The nested report must not self-deadlock on the lock its parent holds;
//   it re-enters and prints interleaved with its parent — which is correct,
//   because both are this core's one story, told in the order it happened.
//
// - BOUNDED ACQUIRE. A core that dies holding the lock (triple fault, wedge)
//   must not silence every other core forever. A full report at polled-serial
//   speed is ~300ms, so after ~3 seconds the holder is dead, not slow: the
//   waiter BARGES, says so, and speaks. Braided output over a corpse beats
//   silence.
//
// - RELEASED BEFORE EVERY HALT. A report ends, the wire frees — the cli/hlt
//   at the end of a fatal path must never be reached holding it.
#define WIRE_FREE 0xFFFFFFFFu
static volatile uint32_t kWireOwner = WIRE_FREE;
static volatile uint32_t kWireDepth = 0;

static uint32_t wire_self(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	// 0xFE: the pre-CLS early-boot pseudo-identity. One core, no contention.
	return (cls != NULL) ? (uint32_t)cls->apic_id : 0xFEu;
}

void exception_wire_lock(void)
{
	uint32_t self = wire_self();
	if (kWireOwner == self) {          // only self can have set this — safe test
		kWireDepth++;
		return;
	}

	uint64_t start = rdtsc();
	// 3 seconds of TSC; if calibration hasn't run yet (early boot), a plain
	// large cycle count lands in the same order of magnitude.
	uint64_t bound = kCPUCyclesPerSecond ? kCPUCyclesPerSecond * 3 : 10000000000UL;
	bool barged = false;

	while (!__sync_bool_compare_and_swap(&kWireOwner, WIRE_FREE, self)) {
		__builtin_ia32_pause();
		if (rdtsc() - start > bound) {
			// The holder is dead. Take the wire by force — and if two
			// waiters barge together the last write wins and both speak,
			// which is the degraded mode we accept over any silent one.
			kWireOwner = self;
			barged = true;
			break;
		}
	}
	kWireDepth = 1;

	if (barged) {
		printf("\n(wire lock barged after timeout — a prior reporter died holding it)\n");
		serial_print_string("\n(wire lock barged after timeout — a prior reporter died holding it)\n");
	}
}

void exception_wire_unlock(void)
{
	uint32_t self = wire_self();
	if (kWireOwner != self)
		return;                        // never held, or barged away — do no harm
	if (kWireDepth > 0)
		kWireDepth--;
	if (kWireDepth == 0)
		kWireOwner = WIRE_FREE;
}

void exception_wire_abandon(void)
{
	// The terminal release: this narrator is about to cli/hlt forever, and a
	// plain unlock can't free a NESTED hold (panic from inside a locked
	// report leaves depth > 1). Story over — free the wire unconditionally
	// so the next core speaks immediately instead of barging past a corpse.
	if (kWireOwner == wire_self()) {
		kWireDepth = 0;
		kWireOwner = WIRE_FREE;
	}
}

// How much stack to show, and how deep to walk. Both bounded because a
// corrupted frame must produce a SHORT report, not an endless one.
#define EXC_STACK_WORDS   16
#define EXC_MAX_FRAMES    24

// ── Taking the glass (2026-08-14, from a photograph of the P5) ──────────────
//
// THE BUG THIS ENDS. page_fault_panic() reports through exception_report() and
// then cli/hlt's on the spot — it never calls panic(), so it never ran panic()'s
// takeover sequence: GUI sink detached, renderer lock busted, terminals forced
// direct. Two of those three merely protect against a dead lock holder. The
// third is load-bearing every single time, and nobody noticed until Chris
// photographed a #PF report on the P5 that stopped at the bottom pixel row of
// the panel with its call chain missing.
//
// renderer_bust_lock() also DISABLES THE BLIT THROTTLE. The throttle
// (BasicRenderer.c, 2026-08-04) holds the glass to ~30Hz: an ordinary putc
// writes VRAM directly while the glass is clean, but a SCROLL writes the shadow
// only and defers the blit until kTicksSinceStart - s_lastBlitTick >= 3. A
// fatal report runs with interrupts OFF — the fault arrived through an
// interrupt gate — so kTicksSinceStart is FROZEN and that condition can never
// become true again. Every line the report prints before the screen fills is
// visible; every line from the first scroll onward lands in a shadow buffer no
// eye will ever see. The report was complete. The glass was never told.
//
// So a dying report takes the machine's output devices exactly the way panic()
// does, and then homes the cursor: starting at row 0 gives the report the whole
// panel instead of the handful of rows left under a running top(1), which on a
// normal screen means it never has to scroll at all.
// Set by the dispatcher around a report that is going to RESUME (a TRACE-mode
// watchpoint) — PER CORE, because the first cut was a single global and a
// concurrent fatal report on another core could read it true and skip its own
// takeover (2026-08-15 review find). Everything in a dying report's takeover
// is destructive FOREVER — GUI sink detached, blit throttle killed, terminals
// routed direct, locks force-released while the (deliberately un-frozen)
// other cores may hold them — so a report the machine survives must do NONE
// of it. A resuming report goes to the serial wire and the log only, never
// the glass: partly because the takeover can't be undone, and partly because
// printf through the LIVE console path can deadlock on a tty or renderer
// lock the interrupted core itself holds. Trace mode is a logger by charter
// (WATCHPOINTS.md); the wire is where a logger's output belongs.
static bool kExcResumingCore[MAX_CPUS];

// Whether EXC_EMIT mirrors to the framebuffer. A static, but serialized by
// the wire lock (one narrator at a time): exception_report clears it for a
// resuming report and restores it before releasing the wire, so the direct
// exception_report_registers callers (ring-3 segfault reports) always see it
// true.
static bool kExcEmitToGlass = true;

static void exc_take_the_glass(void)
{
	gui_emergency_disable();   // the compositor never stands between us and the screen
	renderer_bust_lock();      // busts the lock AND kills the throttle: scrolls blit now
	tty_emergency_direct();    // terminals route straight to the legacy console
	clear(&kRenderer, 0xff000080, true);   // the blue of video.c, cursor home
}

// ── The pager ───────────────────────────────────────────────────────────────
//
// Insurance for the reports that still overflow: 24 frames of call chain plus
// 8 lines of stack window plus the registers can outrun a small panel, and the
// whole point of the work above is that a human gets to READ this.
//
// Paced by hand because nothing else is running: IF=0 means no keyboard IRQ and
// no timer tick, so the wait polls the 8042's two ports directly (the same pair
// keyboard.c's handler reads) and races them against the TSC. A key advances
// immediately; if the keyboard is USB and the firmware isn't emulating an 8042,
// the clock advances it anyway, so the report can never strand itself waiting
// for a keystroke that no hardware will deliver.
#define EXC_PAGE_SECONDS 10

static uint32_t kExcPagerRow;    // lines emitted since the last pause
static uint32_t kExcPagerRows;   // panel height in text rows; 0 = pager off

static void exc_pager_begin(void)
{
	kExcPagerRow = 0;
	kExcPagerRows = renderer_rows();
}

// One emitted line. Every EXC_EMIT in this file prints exactly one line, so
// counting calls counts rows; a future multi-line emit would page early, which
// is harmless (a short page), never late (a lost page).
static void exc_pager_line(void)
{
	if (kExcPagerRows == 0 || ++kExcPagerRow < kExcPagerRows - 1)
		return;
	kExcPagerRow = 0;

	printf("-- more -- (press a key, or wait %u seconds) ", EXC_PAGE_SECONDS);

	uint64_t start = rdtsc();
	uint64_t bound = kCPUCyclesPerSecond
	                     ? kCPUCyclesPerSecond * EXC_PAGE_SECONDS
	                     : 3000000000UL * EXC_PAGE_SECONDS;   // uncalibrated: same order
	while (rdtsc() - start < bound) {
		if (inb(0x64) & 0x01) {     // 8042 output buffer full: a key is waiting
			(void)inb(0x60);        // consume the scancode so it can't queue up
			break;
		}
		__builtin_ia32_pause();
	}

	// Wipe the prompt so the next page starts on a clean line ('\r' is honored
	// by the renderer; the spaces are the erase, same trick husk's backspace uses).
	printf("\r                                                             \r");
}

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
        if (kExcEmitToGlass) { \
            printf(fmt, ##__VA_ARGS__); \
            if (dying) exc_pager_line(); \
        } \
        char _exc_line[512]; \
        snprintf(_exc_line, sizeof(_exc_line), fmt, ##__VA_ARGS__); \
        serial_print_string(_exc_line); \
        if (!(dying) && !log_printd_reaches_serial()) \
            printd(DEBUG_EXCEPTIONS, "%s", _exc_line); \
    } while (0)

// Kernel-half only, not merely canonical (tightened 2026-08-13 alongside the
// probe_read_u64 alias fix in nmi_probe.c — see the essay there). The check
// gates a walk of kKernelPML4 followed by a dereference through the FAULT-TIME
// CR3, and those two only describe the same memory in the shared upper half.
// A garbage RBP pointing at a lower-half VA could pass the walk (the vestigial
// low identity window is mapped in kKernelPML4) and then read whatever the
// faulting task maps there — or nothing. Ring-0 chains never legitimately
// leave the upper half, so refusing the lower half loses no real frames.
static bool exc_kernel_half(uint64_t address)
{
	return (address >> 47) == 0x1FFFF;
}

/// @brief Is this kernel address safe to dereference RIGHT NOW?
///
/// Rule one of any fault reporter: NEVER FAULT WHILE REPORTING A FAULT. A
/// report that faults turns a diagnosable exception into a #DF, which is
/// strictly worse than printing nothing. Every dereference below goes through
/// here, every time — not once at the top.
static bool exc_readable(uint64_t address)
{
	if (!exc_kernel_half(address) || (address & 7) != 0) {
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

/// @brief One chain line, named when the kernel's symbol table knows the
/// address. symbols_for_address is lock-free and allocation-free (symbols.h's
/// contract), so it is legal here even while dying; NULL — a user address, a
/// stripped kernel, symbols_init never ran — falls back to the exact hex line
/// this reporter printed before symbols existed.
static void exc_emit_frame(bool dying, int level, uint64_t addr, const char *tag)
{
	uint64_t off = 0;
	const char *name = symbols_for_address(addr, &off);
	if (name != NULL)
		EXC_EMIT(dying, "  %2d) %s+0x%lx  (0x%016lx)%s\n", level, name, off, addr, tag);
	else
		EXC_EMIT(dying, "  %2d) 0x%016lx  <no name>%s\n", level, addr, tag);
}

/// @brief Walk the frame chain from the CAPTURED rbp — never a global.
///
/// Presumed hostile, every frame re-validated. The monotonicity test does most
/// of the work: stacks grow down, so each caller's frame must sit above its
/// callee's, and that single check ends every kind of broken chain there is.
static void exc_walk_chain(const exception_context_t *ctx, bool dying)
{
	EXC_EMIT(dying, "  Call chain (most recent first):\n");
	exc_emit_frame(dying, 1, ctx->rip, "   <-- faulted here");

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
		exc_emit_frame(dying, level, ret, "");

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
	// Reentrant, so this costs nothing under exception_report's own lock and
	// buys atomicity when user_fault_kill calls us standalone.
	exception_wire_lock();
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
	exception_wire_unlock();
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

	// One narrator per report — see the wire lock's header comment.
	exception_wire_lock();

	// Dying — or resuming? A TRACE-mode watchpoint hit is the one report the
	// machine survives (dispatcher sets the per-core flag around the call).
	// Resuming means: no takeover (every part of it is forever), no glass
	// (printf through the live console path can deadlock on a lock the
	// interrupted core itself holds), no pager (nothing may park a running
	// core at IF=0 for ten seconds a page). Serial wire always; and with
	// `dying` false, EXC_EMIT's printd copy carries the report into the log
	// file when logd holds the sink. Read under the wire lock; per-core, so
	// a concurrent fatal report on another core is unaffected.
	uint32_t excCore = (cls != NULL && cls->apic_id < MAX_CPUS)
	                       ? (uint32_t)cls->apic_id : 0;
	const bool resuming = kExcResumingCore[excCore];
	const bool dying = !resuming;
	kExcEmitToGlass = !resuming;

	if (!resuming) {
		// AFTER the wire lock, so exactly one dying core wipes the panel and
		// the second one's report braids onto the first's screen instead of
		// erasing it.
		exc_take_the_glass();
		exc_pager_begin();
	}

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

	// CONVICTED OF FAULTING MID-REPORT (2026-08-11 soak): this block read
	// cls->currentThread->ownerTask through a POISONED currentThread and
	// #GP'd — the nested-report machinery saved the evidence, but rule one
	// is NEVER FAULT WHILE REPORTING. Every pointer on this path is now
	// treated as hostile, and an unreadable one is REPORTED rather than
	// dereferenced: a poisoned thread pointer is not a nuisance to step
	// around, it is a finding about the fault — often the headline finding.
	// (exc_readable wants 8-aligned addresses; the &~7 masks check the
	// containing qword, which is the same page.)
	{
		thread_t *thread = (cls != NULL) ? cls->currentThread : NULL;
		if (thread == NULL) {
			EXC_EMIT(dying, ">>> No current task (core likely idle) <<<\n");
		} else if (!exc_readable((uint64_t)&thread->ownerTask & ~7UL)) {
			EXC_EMIT(dying, ">>> Excepting task: UNREADABLE — currentThread 0x%016lx is POISONED <<<\n",
			         (uint64_t)thread);
		} else {
			task_t *task = (task_t *)thread->ownerTask;
			if (task == NULL) {
				EXC_EMIT(dying, ">>> Excepting task: (thread 0x%016lx has no owner task) <<<\n",
				         (uint64_t)thread);
			} else if (!exc_readable((uint64_t)&task->taskID & ~7UL) ||
			           !exc_readable((uint64_t)task->exename & ~7UL)) {
				EXC_EMIT(dying, ">>> Excepting task: UNREADABLE — ownerTask 0x%016lx is POISONED (thread 0x%016lx) <<<\n",
				         (uint64_t)task, (uint64_t)thread);
			} else {
				// exename copied BOUNDED, each byte's page proven readable,
				// NUL forced — a garbage name must yield a short string,
				// never a strlen walking off the mapped world inside %s.
				char name[32];
				size_t n = 0;
				while (n < sizeof(name) - 1) {
					uint64_t a = (uint64_t)&task->exename[n];
					if (!exc_readable(a & ~7UL))
						break;
					char c = task->exename[n];
					if (c == '\0')
						break;
					name[n++] = c;
				}
				name[n] = '\0';
				EXC_EMIT(dying, ">>> Excepting task: %s (id %lu) <<<\n",
				         n ? name : "(unnamed)", task->taskID);
			}
		}
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

	// Restore the glass mirror for the next narrator (a resuming report
	// turned it off; direct exception_report_registers callers rely on it).
	kExcEmitToGlass = true;

	// The story ends here — free the wire BEFORE the caller halts, or the
	// next core's report spends three seconds waiting to barge past a corpse.
	exception_wire_unlock();
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

	// A debug exception MIGHT be one of ours (watchpoint.c). If it is, the
	// report leads with which watchpoint fired and what was done to it, and a
	// TRACE-mode watchpoint RESUMES afterwards — the only non-#PF vector that
	// is allowed to return, and only because a data watchpoint is a trap: the
	// access already completed, so there is nothing to retry and nothing
	// broken to return to. (2026-08-14, the P5 page-table hunt.)
	if (ctx->vector == 1) {
		char why[192];
		bool keepRunning = false;
		if (watchpoint_describe_hit(why, sizeof(why), &keepRunning)) {
			// A TRACE hit is about to RESUME, so it must neither freeze the
			// machine nor touch the glass at all — the takeover is forever,
			// and printing through the live console can deadlock on a lock
			// this very core was holding when the trap fired. The per-core
			// flag routes its report wire-and-log-only (exception_report).
			kExcResumingCore[core] = keepRunning;
			if (!keepRunning)
				mpFreezeOtherCores();
			exception_report(ctx, why);
			kExcResumingCore[core] = false;
			if (keepRunning) {
				kCurrentCtx[core] = prev;
				return;
			}
			// Halt mode: the story is told, and telling it twice (by falling
			// into the generic report below) would only bury the headline.
			while (1) {
				__asm__ volatile("cli\nhlt\n");
			}
		}
		// Not one of ours — a stray debug trap. Fall through and report it
		// generically rather than swallowing it.
	}

	// A fault RING 3 raised is the program's bug, not the kernel's: a divide
	// by zero, an AVX instruction with XSAVE off (#UD), a wild segment (#GP),
	// an x87 or SSE exception the program unmasked (#MF/#XM) — every one of
	// them kills the task and keeps the OS, the same way a segfault has since
	// the fault-isolation work. The kernel's own state is intact: the CPU
	// switched to the interrupt stack on the way in, nothing in ring 0 was
	// mid-flight. Two are NOT the program's to answer for: #DF and #MC are
	// hardware or kernel trouble whatever CS says, and fall through to the
	// fatal path. (#PF and #DB were dispatched above.)
	if ((ctx->cs & 3) == 3 && ctx->vector != 8 && ctx->vector != 18) {
		user_exception_kill(ctx);   // returns only if there is no task to kill
	}

	// Everything else is fatal. One report, then stop: the scheduler's state
	// is not trustworthy after an unhandled exception, and an orderly shutdown
	// would ask that same machinery to walk itself to the door.
	//
	// The other cores stop FIRST (2026-08-14, Chris: "if I leave top up the
	// screen gets wiped every second"). Only the reporting core halts by
	// itself; the rest keep scheduling, so a shell repainting elsewhere can
	// scribble over the report before it is read. On hardware with no serial
	// capture that report is the only record there will ever be.
	mpFreezeOtherCores();
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
