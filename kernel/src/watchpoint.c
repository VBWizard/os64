// watchpoint.c — the four debug-address registers, mirrored across every core.
//
// The doctrine and the four hardware limits are in watchpoint.h; read that
// first. This file is the mechanism.
//
// THE SHAPE: a global table of four slots is the truth, and each CPU's debug
// registers are a CACHE of that truth. Arming writes the table, programs the
// calling core, and IPIs the others to re-read it. A core coming up late
// programs itself from the table on its way in (watchpoint_sync_this_core,
// called from the same two places pat_init_this_core is). There is no
// per-core state to keep coherent beyond "does DR7 match the table", which is
// why the sync is a whole-table rewrite rather than an incremental message:
// idempotent, order-independent, and impossible to get half-applied.

#include "watchpoint.h"
#include "CONFIG.h"
#include "printd.h"
#include "serial_logging.h"
#include "strings/sprintf.h"
#include "BasicRenderer.h"
#include "smp.h"        // kCPUInfo, kMPCoreCount
#include "smp_core.h"   // send_ipi and the IPI vector numbers
#include "driver/system/x86_64.h"   // read_apic_id
#include "spinlock.h"
#include "strings.h"

extern bool kSMPInitDone;
// smp_core.c owns this one and never published a header for it; the other
// IPI handlers reach it by local extern the same way (see inv_tlb_ISR).
extern void write_eoi(void);

// The WATCH= commandline spec, filled by kernel_commandline.c before
// watchpoint_init() runs. Empty means "no boot-time watchpoint".
char kWatchSpec[128] = {0};

typedef struct {
	uintptr_t      va;
	const char    *name;
	uint8_t        length;      // 1, 2, 4 or 8 bytes
	watch_kind_t   kind;
	watch_action_t action;
	bool           armed;
	uint64_t       hits;        // how many times this slot has tripped
} watchpoint_t;

static watchpoint_t kWatch[WATCHPOINT_SLOTS];
// Guards the TABLE, not the registers: each core writes its own DRs, and the
// only shared mutable state is these four structs.
static spinlock_t kWatchLock = 0;

// ── The architectural encodings ─────────────────────────────────────────────
//
// DR7, per slot n:  L(2n) G(2n+1) enable bits in 0..7,
//                   R/W in bits (16 + 4n), LEN in bits (18 + 4n).
// LEN is NOT the byte count: 1 byte = 0b00, 2 = 0b01, 8 = 0b10, 4 = 0b11.
// That 8-before-4 ordering is a genuine wart of the ISA (8-byte watchpoints
// were bolted on later), and getting it "obviously right" gives you a 4-byte
// watchpoint that silently misses half of a page table entry.
static inline uint64_t dr7_len_bits(uint8_t bytes)
{
	switch (bytes) {
		case 1:  return 0b00;
		case 2:  return 0b01;
		case 8:  return 0b10;   // <-- yes, really: 8 is 0b10 and 4 is 0b11
		case 4:  return 0b11;
		default: return 0b00;
	}
}

static inline uint64_t read_dr6(void) { uint64_t v; __asm__ volatile("mov %0, dr6" : "=r"(v)); return v; }
static inline void write_dr6(uint64_t v) { __asm__ volatile("mov dr6, %0" :: "r"(v)); }
static inline void write_dr7(uint64_t v) { __asm__ volatile("mov dr7, %0" :: "r"(v)); }

// INTEL SYNTAX, destination first: `mov dr0, %0` WRITES the register. The
// AT&T spelling means the exact opposite — the same trap that once wrote
// garbage into CR3 in exception_report.c and triple-faulted the machine.
static void write_dr_addr(int slot, uintptr_t va)
{
	switch (slot) {
		case 0: __asm__ volatile("mov dr0, %0" :: "r"(va)); break;
		case 1: __asm__ volatile("mov dr1, %0" :: "r"(va)); break;
		case 2: __asm__ volatile("mov dr2, %0" :: "r"(va)); break;
		case 3: __asm__ volatile("mov dr3, %0" :: "r"(va)); break;
		default: break;
	}
}

void watchpoint_sync_this_core(void)
{
	// Build DR7 from scratch every time — see the header comment on why the
	// sync is a whole-table rewrite.
	//
	// Bit 10 is reserved-must-be-one. Bit 9 (GE, "global exact") asks the CPU
	// to report data breakpoints precisely rather than lazily; it is a no-op
	// on modern parts and correct on old ones, which is exactly the kind of
	// bit worth setting once and never thinking about again.
	uint64_t dr7 = (1ULL << 10) | (1ULL << 9);

	for (int i = 0; i < WATCHPOINT_SLOTS; i++) {
		if (!kWatch[i].armed) {
			write_dr_addr(i, 0);
			continue;
		}
		write_dr_addr(i, kWatch[i].va);

		// GLOBAL enable (bit 2i+1), never LOCAL (bit 2i): the local bits are
		// cleared by the CPU on a task switch through a TSS, and os64's
		// watchpoints are properties of the MACHINE, not of whoever happens
		// to be scheduled.
		dr7 |= (1ULL << (2 * i + 1));
		dr7 |= ((uint64_t)kWatch[i].kind          & 0x3) << (16 + 4 * i);
		dr7 |= (dr7_len_bits(kWatch[i].length) & 0x3) << (18 + 4 * i);
	}

	write_dr6(0);        // start this core with a clean hit-status register
	write_dr7(dr7);
}

void watchpoint_sync_ISR(void)
{
	watchpoint_sync_this_core();
	write_eoi();
}

// Push the table onto every OTHER core. Cheap and rare (arming is a human
// act), so it costs nothing to be thorough.
static void watchpoint_sync_all_cores(void)
{
	watchpoint_sync_this_core();

	if (!kSMPInitDone)
		return;   // nobody else is awake yet; they will sync on their way in

	uint32_t self = read_apic_id();
	for (int i = 0; i < (int)kMPCoreCount; i++) {
		uint32_t apic_id = (uint32_t)kCPUInfo[i].apicID;
		if (apic_id != self)
			send_ipi(apic_id, IPI_WATCHPOINT_SYNC_VECTOR, 0, 1, 0);
	}
}

int watchpoint_arm(uintptr_t va, uint8_t lengthBytes, watch_kind_t kind,
                   watch_action_t action, const char *name)
{
	if (lengthBytes != 1 && lengthBytes != 2 && lengthBytes != 4 && lengthBytes != 8) {
		printd(DEBUG_EXCEPTIONS, "watchpoint: length %u is not 1/2/4/8 — refused\n", lengthBytes);
		return -1;
	}
	// The hardware requires natural alignment, and silently misbehaves rather
	// than telling you — so we tell you.
	if (va & (uintptr_t)(lengthBytes - 1)) {
		printd(DEBUG_EXCEPTIONS, "watchpoint: 0x%016lx is not %u-byte aligned — refused\n",
		       va, lengthBytes);
		return -1;
	}
	if (kind == WATCH_EXEC && lengthBytes != 1) {
		printd(DEBUG_EXCEPTIONS, "watchpoint: execute watchpoints must have length 1 — refused\n");
		return -1;
	}
	if (kind == WATCH_IO) {
		printd(DEBUG_EXCEPTIONS, "watchpoint: I/O watchpoints need CR4.DE and are unsupported — refused\n");
		return -1;
	}

	uint64_t flags = spinlock_acquire_irqsave(&kWatchLock);

	int slot = -1;
	for (int i = 0; i < WATCHPOINT_SLOTS; i++) {
		if (!kWatch[i].armed) { slot = i; break; }
	}
	if (slot < 0) {
		spinlock_release_irqrestore(&kWatchLock, flags);
		printd(DEBUG_EXCEPTIONS, "watchpoint: all %d slots are in use — refused\n", WATCHPOINT_SLOTS);
		return -1;
	}

	kWatch[slot].va     = va;
	kWatch[slot].name   = (name != NULL) ? name : "(unnamed)";
	kWatch[slot].length = lengthBytes;
	kWatch[slot].kind   = kind;
	kWatch[slot].action = action;
	kWatch[slot].hits   = 0;
	kWatch[slot].armed  = true;

	spinlock_release_irqrestore(&kWatchLock, flags);

	watchpoint_sync_all_cores();

	// Announced on the wire AND the glass: arming a watchpoint is a deliberate
	// act with a machine-wide effect, and the boot that captured a corruption
	// should say in its own log that it was watching.
	printf("watchpoint %d armed: %s at 0x%016lx, %u byte%s, %s, %s\n",
	       slot, kWatch[slot].name, va, lengthBytes, lengthBytes == 1 ? "" : "s",
	       kind == WATCH_WRITE ? "on write" : (kind == WATCH_ACCESS ? "on read or write" : "on execute"),
	       action == WATCH_TRACE ? "trace (report and continue)" : "halt on first hit");
	printd(DEBUG_EXCEPTIONS, "watchpoint %d armed: %s at 0x%016lx (%u bytes)\n",
	       slot, kWatch[slot].name, va, lengthBytes);
	return slot;
}

bool watchpoint_disarm(int slot)
{
	if (slot < 0 || slot >= WATCHPOINT_SLOTS)
		return false;

	uint64_t flags = spinlock_acquire_irqsave(&kWatchLock);
	bool was = kWatch[slot].armed;
	kWatch[slot].armed = false;
	spinlock_release_irqrestore(&kWatchLock, flags);

	if (was)
		watchpoint_sync_all_cores();
	return was;
}

void watchpoint_disarm_all(void)
{
	uint64_t flags = spinlock_acquire_irqsave(&kWatchLock);
	for (int i = 0; i < WATCHPOINT_SLOTS; i++)
		kWatch[i].armed = false;
	spinlock_release_irqrestore(&kWatchLock, flags);
	watchpoint_sync_all_cores();
}

void watchpoint_dump(void)
{
	printf("Watchpoints (%d hardware slots):\n", WATCHPOINT_SLOTS);
	for (int i = 0; i < WATCHPOINT_SLOTS; i++) {
		if (!kWatch[i].armed) {
			printf("  %d: (free)\n", i);
			continue;
		}
		printf("  %d: %-28s 0x%016lx  %u bytes  %s  %s  hits=%lu\n",
		       i, kWatch[i].name, kWatch[i].va, kWatch[i].length,
		       kWatch[i].kind == WATCH_WRITE ? "write " :
		           (kWatch[i].kind == WATCH_ACCESS ? "rd/wr " : "exec  "),
		       kWatch[i].action == WATCH_TRACE ? "trace" : "halt ",
		       kWatch[i].hits);
	}
}

bool watchpoint_describe_hit(char *out, size_t len, bool *outContinue)
{
	uint64_t dr6 = read_dr6();
	uint64_t hitBits = dr6 & 0xF;      // B0..B3: which address register matched

	if (outContinue)
		*outContinue = false;

	if (hitBits == 0) {
		// A #DB with no breakpoint bit set is a single-step or a debug trap
		// nobody here armed — not ours to describe.
		return false;
	}

	// DR6 is STICKY: the CPU sets bits and never clears them. Clear it now, or
	// the next debug exception inherits this one's bits and lies about which
	// watchpoint fired.
	write_dr6(0);

	// Under kWatchLock: a concurrent arm/disarm on another core rewrites
	// kWatch[] fields, and a description assembled from a half-rewritten slot
	// names the wrong watchpoint in the one sentence the report leads with
	// (2026-08-15 review find; the lock is cheap and this path is already an
	// exception).
	uint64_t lockFlags = spinlock_acquire_irqsave(&kWatchLock);

	int slot = -1;
	for (int i = 0; i < WATCHPOINT_SLOTS; i++) {
		if (hitBits & (1ULL << i)) { slot = i; break; }
	}
	if (slot < 0 || !kWatch[slot].armed) {
		spinlock_release_irqrestore(&kWatchLock, lockFlags);
		snprintf(out, len, "debug exception, DR6=0x%016lx, but no armed watchpoint claims it", dr6);
		return true;
	}

	kWatch[slot].hits++;
	if (outContinue)
		*outContinue = (kWatch[slot].action == WATCH_TRACE);

	// The sentence the report leads with. It names the watchpoint, what was
	// done to it, and reminds the reader that RIP is one instruction PAST the
	// store — the single most misread thing about a data watchpoint.
	snprintf(out, len,
	         "WATCHPOINT %d HIT — %s at 0x%016lx was %s (hit #%lu). "
	         "RIP below is the instruction AFTER the access",
	         slot, kWatch[slot].name, kWatch[slot].va,
	         kWatch[slot].kind == WATCH_WRITE ? "WRITTEN" :
	             (kWatch[slot].kind == WATCH_ACCESS ? "READ or WRITTEN" : "EXECUTED"),
	         kWatch[slot].hits);
	spinlock_release_irqrestore(&kWatchLock, lockFlags);
	return true;
}

// ── The commandline spec ────────────────────────────────────────────────────
//
//   WATCH=<hexaddr>[:len[:kind[:action]]]
//     len    1 | 2 | 4 | 8            (default 8)
//     kind   w = write, a = access, x = execute   (default w)
//     action h = halt, t = trace                  (default h)
//
//   WATCH=ffff8000003f8fd0            watch 8 bytes for writes, halt on hit
//   WATCH=ffff8000003f8fd0:8:w:t      the same, but report and keep running
//
// Deliberately a STRING rather than four separate options: a watchpoint is one
// idea, and four flags that only make sense together are four ways to typo it.
static bool watch_parse_hex(const char *s, const char **end, uintptr_t *out)
{
	uintptr_t v = 0;
	bool any = false;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while (*s) {
		char c = *s;
		uintptr_t d;
		if (c >= '0' && c <= '9')      d = (uintptr_t)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (uintptr_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (uintptr_t)(c - 'A' + 10);
		else break;
		v = (v << 4) | d;
		any = true;
		s++;
	}
	*end = s;
	*out = v;
	return any;
}

void watchpoint_init(void)
{
	// Always start from a known state: a stale DR7 (kexec, a warm reset, a
	// hypervisor's idea of initial register state) would otherwise trap on
	// whatever the previous occupant cared about.
	watchpoint_sync_this_core();

	if (kWatchSpec[0] == '\0')
		return;

	const char *p = kWatchSpec;
	uintptr_t va = 0;
	if (!watch_parse_hex(p, &p, &va)) {
		printf("WATCH=%s: not a hexadecimal address — ignored\n", kWatchSpec);
		return;
	}

	uint8_t len = 8;
	watch_kind_t kind = WATCH_WRITE;
	watch_action_t action = WATCH_HALT;

	if (*p == ':') {
		p++;
		if (*p >= '1' && *p <= '8') { len = (uint8_t)(*p - '0'); p++; }
	}
	if (*p == ':') {
		p++;
		if (*p == 'w') kind = WATCH_WRITE;
		else if (*p == 'a') kind = WATCH_ACCESS;
		else if (*p == 'x') { kind = WATCH_EXEC; len = 1; }
		if (*p) p++;
	}
	if (*p == ':') {
		p++;
		if (*p == 't') action = WATCH_TRACE;
		else if (*p == 'h') action = WATCH_HALT;
	}

	watchpoint_arm(va, len, kind, action, "WATCH= (commandline)");
}
