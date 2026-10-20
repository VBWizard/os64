# CPU_NUMBERS_REVIEW.md — Fable's review of CPU_NUMBERS.md

*2026-10-20. Reviewer: Fable 5.1. Design: Opus, 2026-10-19. The design is
APPROVED with the rulings and findings below; Opus codes it, Fable reviews
the code against the design + this file, then Codex one round.*

The design is right in its one big call: ONE number inside the kernel, the
APIC ID demoted to a hardware address at named doors. That is the fix for
the class, not the symptom. The findings are about the edges.

## The two open questions — ruled

**Q1. `apic_id_t` as a struct: YES.** Naming discipline is what failed here,
and this project's doctrine is tripwires over silence; a compile error on
the author's machine beats silent corruption on Chris's new one. Keep it
minimal: one field `id`, no arithmetic, no implicit conversion. The ONE
array an `apic_id_t` may index is `kApicToCPU[a.id]` — that is the
translation table, which is the whole point of it existing. Nothing else.

**Q2. Ascending APIC order: YES, BSP first.** The argument is stronger than
the doc makes it: an APIC ID is not an arbitrary label, it is a STRUCTURED
number — CPUID leaf 0x8000001E (AMD) / 0xB (Intel) define it as thread bit,
core bits, die/CCX bits, package bits, low to high. Sorting it IS a topology
sort, by the hardware's own definition, on every firmware. MADT order is a
BIOS convention that changes between vendors and firmware versions; it would
also have made the P5's CPU 1 stop meaning APIC 1 the day it landed, and
the whole SPEC_CTRL odd-thread finding is written in that vocabulary.
Ascending keeps dense machines' numbers IDENTICAL to today's, which is the
continuity Chris typed PINCORE values against.
- Keep "CPU 0 = BSP" ahead of the sort. On every PC we know the BSP is APIC
  0, so the two rules agree. On one where they don't, siblings-adjacent
  breaks for the first pair — print ONE boot line saying so (`BSP is APIC
  5; numbering not topological`). Loud, not silent (R3 below is the same
  shape).

## Findings

**R1. The log stamp before the BSP's CLS exists.** The design says `%c`
becomes `cls->cpu`. The BSP's CLS is published in `init_SMP`
(`smp_core.c:809`), long after the banner, `logging_queueing_init`, paging
and the allocator have all logged. Before that, GS base is 0 and a CLS read
is a wild pointer. Rule: a kernel flag set the moment the BSP publishes its
GS; before the flag, the stamp is CPU 0 — which is TRUE, because nothing
else runs yet. Do NOT fall back to `read_apic_id()` there: that register is
the door being closed. APs never log before their CLS
(`ap_wakeup_after_stack_switch` prints after `init_core_local_storage`), so
the flag is a BSP-only question. Say that in its comment.

**R2. How an AP learns its number.** Register-then-table is fine and is the
shape the code already has (`ap_wakeup_entry`, `smp_core.c:309`). Two
requirements:
- The "panic by name" on a miss must run BEFORE the first consumer indexes
  with the result — today that is `tss_initialize_cpu(temp_apic_id)` four
  lines later. A 0xFF through that call is the MAXCORES=4 death again with
  a different array.
- `temp_apic_id`, `stackVirtualAddress`, `tempStack` are GLOBALS shared by
  every AP, safe only because the BSP wakes APs ONE AT A TIME. Renaming
  them to `cpu` vocabulary is required; losing the comment that says why a
  shared global is sound here is not allowed. (If no such comment exists,
  this PR writes it.)
- Worth knowing, not required: Limine hands the AP its own
  `limine_smp_info*` in RDI, whose `extra_argument` the BSP can fill with
  the CPU number before it writes `goto_address`. The AP is then TOLD who it
  is by the one that woke it, and the register read plus table lookup
  disappear. If Opus takes this door, `ap_wakeup_entry` must capture RDI
  before its inline asm clobbers anything.

**R3. The roster owns each CPU's doorbell.** The `MAXCORES=4` death was the
wake loop writing `kCPUInfo[apic_id].goto_address` — the wrong CPU's
doorbell. The roster entry carries `limine_smp_info*` (or the
`goto_address` pointer) for ITS CPU, filled in the same pass that assigns
the number, so the wake loop is `kCPUs[cpu].goto_address` and can only wake
the CPU it named. Same for `apic_tpr`/`apic_svr`: those are per-CPU MMIO
pointers that the AP writes about ITSELF, so they belong in CLS or are
derived from `kMPApicBase` on the running CPU — not looked up by number at
all.

**R4. The sparse topology lives in the REPO, not the scratchpad.** The
`-smp 24,sockets=1,dies=4,cores=3,threads=2` recipe is the acceptance test
for this PR and for every SMP PR after it; the memory note already says
scratch scripts died with /tmp once. Add a root `GNUmakefile` target
(`make run-sparse`, or a `QEMUSMP=` variable the default `run` honours) and
a line in CLAUDE.md's QEMU section. Verification step 2 names that target.

**R5. The inverse table's sentinel.** `kApicToCPU` as `uint8_t` with `0xFF`
is right for `MAX_CPUS` 64. Put `_Static_assert(MAX_CPUS < 0xFF)` beside it
so the sentinel can never become a CPU number if someone raises the bound
again.

**R6. The tripwire test (verification 4) checks the round trip.** For every
`c < kCPUCount`: `kApicToCPU[kCPUs[c].apic.id] == c`, no two CPUs share an
APIC, and the running CPU's `cls->cpu` slot holds the LAPIC register's ID.
Run it preboot on the BSP and once per AP as it comes up (the AP half is
one compare in `ap_wakeup_after_stack_switch`).

**R7. The `PINCORE` alias changes the VALUE's meaning**, not just the name:
an APIC ID became a CPU number. Identical on every dense machine, different
on the 3900X. The alias notice for the ENV var is the `DEBUG_TASK` line
`task_env_pin` already prints — it runs at every spawn under a pinned shell,
so it cannot be a glass line. The cmdline alias (`MAXCORES`) prints one boot
line, as the doc says.

**R8. Log rings per roster CPU (section 4).** Right, and it makes a cap save
real memory. One consequence to state in `log.c`: a capped-off CPU is parked
in Limine's loop and never runs kernel code, so it never logs; if that ever
changes (a future hot-add), the ring must exist before the CPU does.

**R9. Review bar.** Codex one round minimum, agreed — bring-up, IPIs and the
scheduler's per-CPU state are exactly what an outside round earns its cost
on. Fable reads the code against this file and the design first.

## Not findings (so nobody re-litigates them)

- x2APIC / IDs > 254 out of scope: correct. In xAPIC mode such a CPU cannot
  even be addressed through the ICR's 8-bit destination.
- `MAX_CPUS` 64 and `GDT_ENTRIES` 192: the arithmetic checks (64 × 2 TSS
  slots + the fixed entries).
- Renaming policy (no sweeps; names the PR must touch take the CPU
  vocabulary): matches the stdint precedent.
- `core_local_storage_t` / `cls` keep their names: yes, and "core" stays
  the word for the physical core per Chris's ruling, so the type name is
  not even wrong.

## Addendum (2026-10-20): `nosmp` on the 3900X

Chris reported a `nosmp` boot failed there too, and asked whether bring-up
talks to CPUs beyond the cap. It does not: `init_SMP` sets the count to 1
(`nosmp`) or the cap, sizes `kCPUInfo` by it, and the wake loop, idle-task
creation and SPEC_CTRL broadcast all iterate that count. The `MAXCORES=4`
death was the wrong-doorbell bug (design §1), not a touch beyond the cap.

Two things the design must therefore say outright, and one hygiene item:

**A1. The capped contract.** `nosmp` and `MAXCPUS=N` take the FIRST N of the
roster's sorted order, BSP first. Today "core 0" is Limine's POSITION 0,
whose APIC ID is whatever the firmware listed first, and the BSP's idle
task is pinned to that ID (`kernel.c:495`). On a firmware that lists APIC 2
first, `nosmp` pins the only idle task to a parked CPU and the BSP has
nothing to run. §3.2 fixes this by construction (`bsp_lapic_id`); state the
capped case as a consequence so nobody reintroduces position 0.

**A2. The MP-table parser's bound.** `parse_mp_table` writes `mc->count`
records into `kMPConfigTable`, allocated for `MAX_CPUS` entries
(`smp.c:268`, `:118`). The record count is processors + buses + IOAPICs +
every interrupt assignment, which exceeds 24 on any real table. Modern UEFI
boards ship no `_MP_` table, so it is probably moot on the 3900X — but
allocate by `mc->count` (or bound the loop) while this PR is in the file.

**A3. Verification step 2** gains a `nosmp` boot and a `MAXCPUS=4` boot on
the sparse topology, each to a shell with tests green.

**Not a cause in the kernel:** the token is lowercase `nosmp`; `NOSMP`
matches nothing, silently, and boots the all-CPU configuration.

## A DEBTS row for this PR to carry (Chris, 2026-10-20: "a debt line, nothing more")

The PR touches `log.c` (rings per roster CPU), so it carries this row under
DEBTS § Quick wins, text as follows:

> **Log rings are 16 MB per CPU whatever the boot asked for.** They were
> sized for boots with several flood bits set (DEBUG_SCHEDULER, DEBUG_PAGING,
> the DETAILED pair), which nobody runs any more; a default boot on the P5
> spends 192 MB of its 328 MB on them, a 3900X 384 MB. They are already
> kmalloc'd (`logging_queueing_init`, right after the kernel stack) with
> `kDebugLevel` known, so the size can follow the level at birth: small with
> no flood bit, 16 MB with one. A ring that must GROW later (a level raised
> at runtime, if that ever becomes possible) is kworker's job at a high-water
> mark — printd never allocates. Only the 64-entry pre-allocator ring is BSS,
> and it stays. Low priority; booked 2026-10-20.
