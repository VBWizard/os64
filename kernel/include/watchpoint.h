#pragma once
//
// watchpoint.h — the hardware's four debug-address registers, made usable.
//
// WHAT THIS IS FOR. "Who scribbled this memory?" is the single most expensive
// question this OS asks, and it has been asked repeatedly: the CLS corruption
// hunt, the idle-task RFLAGS stray write, the scribbled console text, and the
// P5's page-table corruption that finally bought this file. Every one of those
// was chased with tripwires — a check placed AFTER the fact, narrowing the
// window by guesswork until the culprit had nowhere left to hide. That works,
// and it is slow.
//
// x86 has had the answer since the 386: four debug-address registers that trap
// the INSTRUCTION performing an access to an address you name. A watchpoint
// turns "something corrupts this eventually" into a call chain, on the first
// occurrence, with the machine still standing.
//
// WHAT IT COSTS AT REST: nothing. An unarmed DR7 means the CPU checks nothing.
//
// THE FOUR LIMITS, stated up front because each one will eventually surprise
// somebody (they are hardware, not policy):
//
//   1. FOUR. The CPU has four address registers. That is the budget, forever.
//   2. LINEAR ADDRESSES, NOT PHYSICAL. A watchpoint matches the VA it was
//      given. The same physical bytes reached through a DIFFERENT mapping
//      (the HHDM alias vs the identity alias, say) will NOT trip it. Watch
//      every alias that matters, or accept the blind spot knowingly.
//   3. PER CORE. DR0-3/DR6/DR7 are per-CPU registers. This module keeps a
//      global table and mirrors it onto every core (at bring-up, and by IPI
//      for cores already running), because a watchpoint armed on one core is
//      a watchpoint that misses seven eighths of an eight-core machine.
//   4. THE CPU IS NOT THE ONLY WRITER. A device DMA-ing into memory writes no
//      instruction and trips nothing. A watchpoint that stays silent while the
//      memory still changes is therefore EVIDENCE, not a failure: it says the
//      write did not come from a CPU. That null result is often the answer.
//
// A NOTE ON WHEN THE TRAP ARRIVES: data watchpoints are TRAPS, not faults —
// they are delivered after the storing instruction retires. The reported RIP
// is therefore the instruction AFTER the culprit. The call chain is unaffected,
// and the store you want is the one just above the reported RIP.
//
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The hardware's budget, and the reason this number is not a tunable.
#define WATCHPOINT_SLOTS 4

// What kind of access trips it. Values are the architectural R/W encoding, so
// they go into DR7 unchanged — no translation table to get wrong.
typedef enum {
	WATCH_EXEC   = 0,   // instruction fetch at this address (length MUST be 1)
	WATCH_WRITE  = 1,   // data writes only — the usual choice for a scribble hunt
	WATCH_IO     = 2,   // I/O port access; needs CR4.DE, unsupported here
	WATCH_ACCESS = 3,   // data reads OR writes ("who even LOOKS at this?")
} watch_kind_t;

// What happens when it trips.
typedef enum {
	// Report and stop the machine. The default, and the right one for a
	// corruption you only expect to happen once: the report is the point, and
	// a dead machine cannot overwrite its own evidence.
	WATCH_HALT  = 0,
	// Report and CONTINUE. Turns a watchpoint into a logger: every writer of
	// an address announces itself with a full call chain and the system keeps
	// running. Expensive if the address is written often — a report is
	// hundreds of polled-serial milliseconds — so aim it at something rare.
	WATCH_TRACE = 1,
} watch_action_t;

/// @brief Arm a watchpoint on `va`. Returns the slot (0-3), or -1 if there is
///        no free slot, the length is not 1/2/4/8, or the address is not
///        naturally aligned to the length (the hardware requires this).
/// `name` is borrowed, not copied — pass a string literal or something that
/// outlives the watchpoint, because the report prints it at trap time.
int watchpoint_arm(uintptr_t va, uint8_t lengthBytes, watch_kind_t kind,
                   watch_action_t action, const char *name);

/// @brief Release a slot (and stop watching), on every core.
bool watchpoint_disarm(int slot);
void watchpoint_disarm_all(void);

/// @brief Program THIS core's debug registers from the global table. Called at
///        BSP init and by every AP as it comes up — same shape and the same
///        reason as pat_init_this_core().
void watchpoint_sync_this_core(void);

/// @brief Parse and apply the WATCH= kernel commandline spec (see watchpoint.c
///        for the grammar). Called once, after the commandline is parsed.
void watchpoint_init(void);

/// @brief Print the table (armed slots, addresses, kinds, hit counts).
void watchpoint_dump(void);

/// @brief The #DB seam. If this debug exception was one of ours, fill `out`
///        with a human sentence naming the watchpoint and return true; set
///        *outContinue to true when the slot's action is WATCH_TRACE. Also
///        acknowledges the hit (clears DR6) so the next one reports honestly.
bool watchpoint_describe_hit(char *out, size_t len, bool *outContinue);

/// @brief The IPI target: "re-read the global table onto your own registers".
void watchpoint_sync_ISR(void);
