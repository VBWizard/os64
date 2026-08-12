// Symbolized call-chain reporting for a faulting ring-3 task.
//
// Ported from os32's stack_trace.c (Chris, 2024) — see stack_trace.h for what
// was kept and why. The safety contract below is the whole design; everything
// else is bookkeeping.
//
//   1. NEVER FAULT WHILE REPORTING A FAULT. Every read of task memory goes
//      through read_task_u64(), which resolves the address through the TASK's
//      own page tables and gives up rather than dereferencing anything it
//      cannot prove is mapped. A trace that faults turns a diagnosable
//      segfault into a #DF, which is strictly worse than printing nothing.
//   2. NO FILESYSTEM, NO kmalloc, ON THIS PATH. Symbols were read at load
//      time (elf_loader.c). The one time this rule was learned the hard way,
//      the panic being reported WAS an unreadable filesystem — a trace that
//      reads the disk deadlocks on its own cause.
//   3. TWO INDEPENDENT GIVE-UP BOUNDS: depth, and consecutive unknowns. os32
//      had exactly this pair and it is right — a corrupted chain must end the
//      walk, not decorate it forever.
//   4. RE-VALIDATE EVERY FRAME, not just the first. The bug this instrument
//      was built to catch CORRUPTS STACKS, so the chain is presumed hostile.
//   5. THE KILL SWITCH IS CHECKED BEFORE THE FIRST DEREFERENCE (NOTRACE).

#include "stack_trace.h"
#include "elf_loader.h"
#include "paging.h"
#include "serial_logging.h"
#include "logging/log.h"   // log_sink_is_userland — which sinks this line needs
#include "sprintf.h"
#include "CONFIG.h"
#include "video.h"

extern uintptr_t kHHDMOffset;
extern bool kEnableStackTrace;

// Depth: deep enough for any real os64 call chain, shallow enough that a
// garbage chain cannot fill the screen. Unknowns: a couple of unnamed frames
// are normal (asm thunks, the launch stub); eight in a row means we are
// reading noise, not a stack.
#define TRACE_MAX_DEPTH     24
#define TRACE_MAX_UNKNOWN    8

// STT_FUNC, spelled from the raw st_info rather than a macro so this file
// depends on nothing but the struct layout.
#define TRACE_STT_FUNC 2

// Every sink, one formatted line. See the header for why the glass and the
// wire both matter.
//
// The DIRECT serial write was added 2026-08-11 with the rest of the
// exception-reporting ruling (simple_exceptions.c's FAULT_PRINT carries the
// full argument): a call chain is the single most valuable part of a fault
// report and it was reaching the wire only when no LOGD= daemon owned the log.
// The printd copy is added only when it lands somewhere other than this same
// wire, or the whole chain prints twice in the default no-LOGD case.
static void trace_emit(const char *line)
{
	printf("%s", line);
	serial_print_string(line);
	if (!log_printd_reaches_serial())
		printd(DEBUG_EXCEPTIONS, "%s", line);
}

/// @brief Read one qword from a task's address space, safely.
/// @return true only if the address was mapped and the value was read.
///
/// Resolves through the TASK's page tables (never the live CR3 — by the time a
/// fault report runs we may be on kKernelPML4) and reads through the HHDM
/// alias, which is valid under every CR3 for allocator-owned pages. The
/// 0xbadbadba sentinel is paging_walk_paging_table's "not mapped" answer.
static bool read_task_u64(task_t *task, uint64_t va, uint64_t *out)
{
	if (task == NULL || task->pml4v == 0 || out == NULL) {
		return false;
	}
	// A frame pointer must be 8-byte aligned and in the lower half. Rejecting
	// this early costs nothing and refuses obviously-corrupt values before
	// they reach the page walker.
	if ((va & 7) != 0 || va >= 0x0000800000000000ULL) {
		return false;
	}

	uintptr_t phys = paging_walk_paging_table((pt_entry_t *)task->pml4v, va);
	if (phys == 0 || phys == 0xbadbadba) {
		return false;
	}
	*out = *(volatile uint64_t *)((phys & ~0xFFFULL) | (va & 0xFFF) | kHHDMOffset);
	return true;
}

/// @brief Name the function containing a runtime address, from the task's .symtab.
/// @param off Receives the offset of addr into the symbol (the "+0x1c" part).
static const char *sym_for_address(task_t *task, uint64_t addr, uint64_t *off)
{
	elf_image_t *img = (elf_image_t *)task->elf;
	if (img == NULL || img->tracesyms == NULL || img->tracestr == NULL) {
		return NULL;
	}

	// Symbol values are image-relative; loadBias is where the image actually
	// landed. For os64's static, non-PIE programs the bias is 0 and these are
	// already absolute — but subtracting unconditionally is what keeps this
	// correct on the day PIE lands (see the debug-bias plan).
	if (addr < task->loadBias) {
		return NULL;
	}
	uint64_t rel = addr - task->loadBias;

	for (size_t i = 0; i < img->tracesym_count; i++) {
		Elf64_Sym *s = &img->tracesyms[i];
		if ((s->st_info & 0xF) != TRACE_STT_FUNC || s->st_size == 0) {
			continue;
		}
		if (rel >= s->st_value && rel < s->st_value + s->st_size) {
			if (s->st_name >= img->tracestr_size) {
				return NULL;   // malformed index — refuse rather than run off
			}
			*off = rel - s->st_value;
			return img->tracestr + s->st_name;
		}
	}
	return NULL;
}

void stack_trace_user(task_t *task, uint64_t rip, uint64_t rbp)
{
	char line[160];

	// Rule 5: the switch is honored before anything is touched.
	if (!kEnableStackTrace) {
		return;
	}
	if (task == NULL) {
		return;
	}

	elf_image_t *img = (elf_image_t *)task->elf;
	if (img == NULL || img->tracesyms == NULL) {
		// Stripped, or loaded while NOTRACE was set. Say so once — silence
		// here would read as "no call chain", which is a different claim.
		trace_emit("  (no symbol table for this image — call chain unavailable)\n");
		return;
	}

	trace_emit("  Call chain (most recent first):\n");

	// The faulting address first, tagged — os32 did this and it is right: the
	// eye should land on where it died before where it came from.
	uint64_t off = 0;
	const char *name = sym_for_address(task, rip, &off);
	if (name != NULL) {
		sprintf(line, "   1) %s+0x%lx  (0x%016lx)   <-- faulted here\n", name, off, rip);
	} else {
		sprintf(line, "   1) 0x%016lx  <no name>   <-- faulted here\n", rip);
	}
	trace_emit(line);

	int level = 2;
	int unknowns = 0;
	uint64_t frame = rbp;

	while (level <= TRACE_MAX_DEPTH && unknowns <= TRACE_MAX_UNKNOWN) {
		uint64_t saved_rbp = 0, ret = 0;

		// Rule 1 + rule 4: both reads validated, every single frame.
		if (!read_task_u64(task, frame, &saved_rbp) ||
		    !read_task_u64(task, frame + 8, &ret)) {
			break;
		}
		if (ret == 0) {
			break;   // a clean chain terminates here (the launch stub's floor)
		}

		off = 0;
		name = sym_for_address(task, ret, &off);
		if (name != NULL) {
			sprintf(line, "  %2d) %s+0x%lx  (0x%016lx)\n", level, name, off, ret);
			unknowns = 0;
		} else {
			sprintf(line, "  %2d) 0x%016lx  <no name>\n", level, ret);
			unknowns++;
		}
		trace_emit(line);

		// MONOTONICITY: stacks grow DOWN, so each caller's frame must sit at a
		// HIGHER address than the callee's. This one test ends every kind of
		// broken chain there is — a self-referential frame, a cycle, a slot
		// overwritten with a small integer — and it is why a corrupted stack
		// produces a short trace instead of an infinite one.
		if (saved_rbp <= frame) {
			break;
		}
		frame = saved_rbp;
		level++;
	}

	if (level > TRACE_MAX_DEPTH) {
		trace_emit("  ... (depth limit reached)\n");
	} else if (unknowns > TRACE_MAX_UNKNOWN) {
		trace_emit("  ... (too many unnamed frames — chain is not trustworthy)\n");
	}
}
