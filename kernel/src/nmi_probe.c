// nmi_probe.c — ask a core that has stopped answering what it is doing.
//
// The doctrine, the safety model and the reason NMI is the only tool that can
// do this job all live in nmi_probe.h. Read that first; this file is the
// mechanism.
//
// Built 2026-08-11, for a Bosgame P5 that ran thirteen hours with core 2
// silently struck off the roster: top showed its idle thread frozen at
// 4397.0s and /proc/cores called it "parked", and every other instrument we
// owned reached that core through a maskable interrupt it was no longer
// taking. There was no way to ask it a question. Now there is.

#include <stddef.h>

#include "nmi_probe.h"
#include "smp.h"
#include "smp_core.h"
#include "scheduler.h"
#include "paging.h"
#include "driver/system/x86_64.h"   // rdtsc
#include "exception_report.h"       // exception_wire_lock — one narrator per report
#include "serial_logging.h"
#include "sprintf.h"
#include "video.h"
#include "CONFIG.h"

extern volatile bool mp_acctSettleAck[MAX_CPUS];
extern bool mp_schedulerEnabled[MAX_CPUS];
extern uint32_t read_apic_register(uintptr_t reg);
extern uint32_t apic_in_service_vector(void);
extern volatile uint64_t kTicksSinceStart;
extern uintptr_t kHHDMOffset;
extern uintptr_t kKernelPML4v;

// LAPIC register offsets we only ever READ. Named here rather than pulled from
// a driver header because this file must stay compilable on a path where as
// little as possible is trusted.
#define APIC_TPR_OFFSET        0x80
#define APIC_PPR_OFFSET        0xA0
#define APIC_LVT_TIMER_OFFSET  0x320

// LAPIC delivery mode 100b — the one that makes the target take vector 2 no
// matter what its IF, TPR or in-service state says. The vector field is
// ignored for NMI delivery; passing 0 says so.
#define IPI_DELIVERY_NMI       4

// Frames to walk, and the two independent give-up bounds os32's stack_trace
// established. A corrupted chain must END the walk, not decorate it forever.
#define PROBE_MAX_DEPTH        24
#define PROBE_STACK_WORDS      16

// One slot per core. Written ONLY by that core's own NMI handler, read by
// everyone else — see the header's note on why `valid` is published last.
static nmi_probe_snapshot_t kNmiSnapshot[MAX_CPUS];

// The request word. Set by the asker BEFORE the NMI goes out; the handler uses
// it to tell our probe apart from a genuine hardware NMI (parity, watchdog),
// which must still reach the panic path it has always reached. Without this,
// the probe would silently swallow the one class of NMI that means the machine
// is on fire.
static volatile uint8_t kNmiProbeRequest[MAX_CPUS];

// ───────────────────────────────────────────────────────────────────────────
// The handler half. Runs ON THE SICK CORE, on IST2, with NMIs blocked until
// our iretq. It snapshots and returns. It does not print, does not lock, and
// does not dereference anything it was not handed.
// ───────────────────────────────────────────────────────────────────────────
bool nmi_probe_capture(nmi_regs_t *regs, nmi_frame_t *frame)
{
	core_local_storage_t *cls = get_core_local_storage();

	// No CLS means an NMI arrived before this core had an identity — far too
	// early to be one of ours. Hand it to the panic path.
	if (cls == NULL || regs == NULL || frame == NULL) {
		return false;
	}

	uint32_t apic_id = (uint32_t)cls->apic_id;
	if (apic_id >= MAX_CPUS || !kNmiProbeRequest[apic_id]) {
		return false;   // genuine hardware NMI — not ours to eat
	}

	nmi_probe_snapshot_t *s = &kNmiSnapshot[apic_id];

	// Invalidate first: a reader must never see half of a NEW snapshot glued
	// to half of the last one.
	s->valid = 0;
	__asm__ volatile("" ::: "memory");

	s->apic_id = apic_id;
	s->tsc     = rdtsc();
	s->frame   = *frame;
	s->regs    = *regs;

	__asm__ volatile("mov %0, cr0" : "=r"(s->cr0));
	__asm__ volatile("mov %0, cr2" : "=r"(s->cr2));
	__asm__ volatile("mov %0, cr3" : "=r"(s->cr3));
	__asm__ volatile("mov %0, cr4" : "=r"(s->cr4));

	s->currentThread       = (uint64_t)cls->currentThread;
	s->currentTask         = (uint64_t)cls->task;
	s->acctLastDispatchTSC = cls->acctLastDispatchTSC;

	// The three bits that can each strike a core off the roster by themselves.
	s->inScheduler      = mp_inScheduler[apic_id] ? 1 : 0;
	s->settleAck        = mp_acctSettleAck[apic_id] ? 1 : 0;
	s->schedulerEnabled = mp_schedulerEnabled[apic_id] ? 1 : 0;
	s->lastIretqRIP     = mp_lastIretqRIP[apic_id];

	// The LAPIC's own opinion. Legal to ask only from here, because these
	// registers are core-local: the same MMIO address read on another core
	// answers about THAT core. This is the single most direct test of the
	// stuck-in-service-bit theory, and it costs four reads.
	s->apicInServiceVector = apic_in_service_vector();
	s->apicTPR             = read_apic_register(kMPApicBase + APIC_TPR_OFFSET);
	s->apicPPR             = read_apic_register(kMPApicBase + APIC_PPR_OFFSET);
	s->apicLVTTimer        = read_apic_register(kMPApicBase + APIC_LVT_TIMER_OFFSET);

	// Publish. Everything above must be visible before `valid` is, and must
	// not be sunk below it by the compiler.
	__asm__ volatile("" ::: "memory");
	s->valid = 1;

	// Consume the request LAST, so the asker's "done" test (valid && !request)
	// cannot observe an answer that is still being written.
	kNmiProbeRequest[apic_id] = 0;
	return true;
}

// ───────────────────────────────────────────────────────────────────────────
// The asker half. Runs on a HEALTHY core. Everything from here down may fail,
// print a complaint and carry on — a wrong guess costs a line of output, not
// the machine.
// ───────────────────────────────────────────────────────────────────────────

static bool is_canonical(uint64_t address)
{
	uint64_t upper = address >> 47;
	return upper == 0 || upper == 0x1FFFF;
}

/// @brief Read one qword of KERNEL memory, safely, for the stack walk.
///
/// Deliberately the same shape as stack_trace.c's read_task_u64: resolve
/// through page tables and give up rather than dereference anything we cannot
/// prove is mapped. Rule 1 of that file — NEVER FAULT WHILE REPORTING — is
/// doubly true here, because the thing we are reporting on is a core that has
/// already stopped responding.
///
/// Kernel tables only, and that is a deliberate limit rather than an oversight:
/// a core wedged in RING 3 is a different (and much easier) problem, and the
/// report says so from CS rather than guessing through a lower-half VA.
static bool probe_read_u64(uint64_t va, uint64_t *out)
{
	if (out == NULL || (va & 7) != 0 || !is_canonical(va)) {
		return false;
	}
	uintptr_t phys = paging_walk_paging_table((pt_entry_t *)kKernelPML4v, va);
	if (phys == 0 || phys == 0xbadbadba) {
		return false;
	}
	*out = *(volatile uint64_t *)((phys & ~0xFFFULL) | (va & 0xFFF) | kHHDMOffset);
	return true;
}

// Both sinks, one line — printf reaches the glass a human is watching, printd
// reaches the wire a script can grep, and a diagnosis that exists in only one
// of those places is the failure mode this whole instrument was built to end.
//
// DEBUG_EXCEPTIONS, not DEBUG_SMP, and the reason is a bug this file already
// had: DEBUG_SMP is OFF in the default mask, so the first QEMU run swept all
// three APs perfectly and printed absolutely nothing to the wire. An
// instrument whose output depends on having guessed the right debug flag
// BEFORE the incident is not an instrument. DEBUG_EXCEPTIONS is on by default
// and is what stack_trace.c already uses for exactly this kind of report.
static void probe_emit(const char *line)
{
	printf("%s", line);
	printd(DEBUG_EXCEPTIONS, "%s", line);
}

/// @brief Name a kernel address. THE SEAM for kernel symbolization.
///
/// Returns NULL today, so every address renders as raw hex. The next slice
/// fills this in: main.c:132 already holds Limine's kernel-file response (we
/// use it only for the cmdline), and the linked kernel retains .symtab and
/// .strtab — so `scheduler_do+0x1a4` is a table walk over a buffer we already
/// have a pointer to, reusing sym_for_address()'s shape from stack_trace.c.
/// file:line needs a DWARF .debug_line state machine and is its own slice.
static const char *probe_sym_for_address(uint64_t addr, uint64_t *off)
{
	(void)addr;
	(void)off;
	return NULL;
}

static void probe_print_frame_line(int level, uint64_t addr, const char *tag)
{
	char line[160];
	uint64_t off = 0;
	const char *name = probe_sym_for_address(addr, &off);

	if (name != NULL) {
		sprintf(line, "  %2d) %s+0x%lx  (0x%016lx)%s\n", level, name, off, addr, tag);
	} else {
		sprintf(line, "  %2d) 0x%016lx  <no name>%s\n", level, addr, tag);
	}
	probe_emit(line);
}

/// @brief Walk the RBP chain of a stopped core, from the asker's side.
///
/// Presumed hostile, every frame re-validated — rule 4 of stack_trace.c, and
/// the reason a corrupted stack yields a SHORT trace rather than an infinite
/// one. The monotonicity test does most of the work: stacks grow down, so each
/// caller's frame must sit ABOVE its callee's.
static void probe_walk_stack(const nmi_probe_snapshot_t *s)
{
	char line[160];

	// Ring 3 has no kernel chain to walk, and saying so is a real answer.
	if ((s->frame.cs & 3) != 0) {
		probe_emit("  (core was in ring 3 — no kernel call chain; see CS/RIP above)\n");
		return;
	}

	probe_emit("  Call chain (most recent first):\n");
	probe_print_frame_line(1, s->frame.rip, "   <-- stopped here");

	int level = 2;
	uint64_t frame = s->regs.rbp;

	while (level <= PROBE_MAX_DEPTH) {
		uint64_t saved_rbp = 0, ret = 0;

		if (!probe_read_u64(frame, &saved_rbp) ||
		    !probe_read_u64(frame + 8, &ret)) {
			probe_emit("  ... (chain broke here — frame not readable)\n");
			break;
		}
		if (ret == 0) {
			break;   // a clean chain terminates
		}
		probe_print_frame_line(level, ret, "");

		// A ZERO saved RBP is the floor, not a fault: the thread launch stub
		// pushes it deliberately so the chain has somewhere to stop. The first
		// QEMU run reported "chain broke here" on every healthy idle core for
		// exactly this reason — a correct walk that libelled itself.
		if (saved_rbp == 0) {
			break;
		}
		if (saved_rbp <= frame) {
			probe_emit("  ... (chain broke here — frame pointer not monotonic)\n");
			break;
		}
		frame = saved_rbp;
		level++;
	}

	// The raw stack window. When the RBP chain is garbage — which is exactly
	// the case a corrupting bug produces — these words are often the only
	// evidence left, and they cost sixteen validated reads.
	probe_emit("  Stack words at RSP:\n");
	for (int i = 0; i < PROBE_STACK_WORDS; i += 2) {
		uint64_t a = 0, b = 0;
		uint64_t va = s->frame.rsp + (uint64_t)i * 8;
		bool oka = probe_read_u64(va, &a);
		bool okb = probe_read_u64(va + 8, &b);
		if (!oka && !okb) {
			probe_emit("    (stack not readable from here)\n");
			break;
		}
		sprintf(line, "    +0x%02x  %016lx  %016lx\n", i * 8,
		        oka ? a : 0UL, okb ? b : 0UL);
		probe_emit(line);
	}
}

static void probe_report(const nmi_probe_snapshot_t *s)
{
	char line[200];
	const nmi_regs_t *r = &s->regs;

	// One narrator per report (exception_report.h): a probe dump racing a
	// fault report on another core must not braid with it. Per-REPORT, not
	// per-sweep — a sweep can spend seconds in timeouts, and holding the wire
	// that long would push a dying core's report into the barge path.
	exception_wire_lock();

	sprintf(line, "core %u: RIP=%016lx CS=%04lx RFLAGS=%016lx (IF=%lu) RSP=%016lx SS=%04lx\n",
	        s->apic_id, s->frame.rip, s->frame.cs, s->frame.rflags,
	        (s->frame.rflags >> 9) & 1, s->frame.rsp, s->frame.ss);
	probe_emit(line);

	sprintf(line, "  RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n",
	        r->rax, r->rbx, r->rcx, r->rdx);
	probe_emit(line);
	sprintf(line, "  RSI=%016lx RDI=%016lx RBP=%016lx\n", r->rsi, r->rdi, r->rbp);
	probe_emit(line);
	sprintf(line, "  R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n",
	        r->r8, r->r9, r->r10, r->r11);
	probe_emit(line);
	sprintf(line, "  R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n",
	        r->r12, r->r13, r->r14, r->r15);
	probe_emit(line);
	sprintf(line, "  CR0=%016lx CR2=%016lx CR3=%016lx CR4=%016lx\n",
	        s->cr0, s->cr2, s->cr3, s->cr4);
	probe_emit(line);

	// The scheduler's opinion of this core, and the three bits that can each
	// remove it from service on their own.
	sprintf(line, "  sched: inScheduler=%u enabled=%u settleAck=%u lastIretqRIP=%016lx\n",
	        s->inScheduler, s->schedulerEnabled, s->settleAck, s->lastIretqRIP);
	probe_emit(line);
	sprintf(line, "  thread=%016lx task=%016lx lastDispatchTSC=%lu (now %lu)\n",
	        s->currentThread, s->currentTask, s->acctLastDispatchTSC, s->tsc);
	probe_emit(line);

	// The LAPIC. An in-service vector here with nothing progressing is the
	// whole answer: an interrupt that never got its EOI blocks everything at
	// or below its priority class until the core is reset.
	sprintf(line, "  lapic: inService=0x%02x TPR=0x%02x PPR=0x%02x LVTtimer=0x%08x\n",
	        s->apicInServiceVector, s->apicTPR, s->apicPPR, s->apicLVTTimer);
	probe_emit(line);

	probe_walk_stack(s);

	exception_wire_unlock();
}

bool nmi_probe_core(uint32_t apic_id, uint32_t timeout_ticks)
{
	char line[160];

	if (apic_id >= MAX_CPUS || !kSMPInitDone) {
		return false;
	}

	core_local_storage_t *cls = get_core_local_storage();
	if (cls != NULL && (uint32_t)cls->apic_id == apic_id) {
		probe_emit("nmi_probe: refusing to probe the core doing the asking\n");
		return false;
	}

	kNmiSnapshot[apic_id].valid = 0;
	__asm__ volatile("" ::: "memory");
	kNmiProbeRequest[apic_id] = 1;

	send_ipi(apic_id, 0, IPI_DELIVERY_NMI, 1, 0);

	// NEVER WAIT FOR AN INTERRUPT-DELIVERED ANSWER WITH INTERRUPTS OFF —
	// mpAcctSettleAll paid for that lesson on 2026-08-10 with a two-core
	// mutual deadlock, and the rule is general. An NMI is not maskable, so
	// the TARGET will answer regardless; but we still bound the wait on
	// wall-clock rather than a spin count, because a spin count means
	// something different on every machine and under every emulator.
	uint64_t deadline = kTicksSinceStart + (timeout_ticks ? timeout_ticks : 10);
	while (!kNmiSnapshot[apic_id].valid && kTicksSinceStart < deadline) {
		__builtin_ia32_pause();
	}

	if (!kNmiSnapshot[apic_id].valid) {
		// Not a failure of the instrument — a finding, and the most damning
		// one available. A core that will not answer an NMI is not merely
		// stuck in software: it is off the bus, held in SMM, or gone.
		kNmiProbeRequest[apic_id] = 0;
		sprintf(line, "core %u: DID NOT ANSWER AN NMI within %lu ticks — off the bus, in SMM, or dead\n",
		        apic_id, (uint64_t)(timeout_ticks ? timeout_ticks : 10));
		probe_emit(line);
		return false;
	}

	probe_report(&kNmiSnapshot[apic_id]);
	return true;
}

void nmi_probe_sweep(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	uint32_t self = cls ? (uint32_t)cls->apic_id : BOOTSTRAP_PROCESSOR_ID;
	char line[120];

	if (!kSMPInitDone) {
		probe_emit("nmi_probe: SMP is not up — nothing to sweep\n");
		return;
	}

	sprintf(line, "── NMI core sweep (asked from core %u) ──────────────────────\n", self);
	probe_emit(line);

	for (int i = 0; i < kMPCoreCount; i++) {
		uint32_t apic_id = kCPUInfo[i].apicID;
		if (apic_id == self || apic_id >= MAX_CPUS) {
			continue;
		}
		nmi_probe_core(apic_id, 10);
	}
	probe_emit("── end of sweep ────────────────────────────────────────────\n");
}

const nmi_probe_snapshot_t *nmi_probe_last(uint32_t apic_id)
{
	if (apic_id >= MAX_CPUS || !kNmiSnapshot[apic_id].valid) {
		return NULL;
	}
	return &kNmiSnapshot[apic_id];
}
