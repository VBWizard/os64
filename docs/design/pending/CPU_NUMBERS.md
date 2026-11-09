# CPU_NUMBERS.md — one number for a CPU, and the APIC ID kept at the hardware's edge

*Design 2026-10-19 (Opus), approved by Fable 2026-10-20 with the findings
in CPU_NUMBERS_REVIEW.md; built on `opus/cpu-numbers`. Ruling owner: Chris.
Where the build departs from this text, the review file or the code says
so; the one open acceptance item (a `nosmp` boot of the sparse topology)
is blocked on a separate, older bug booked in DEBTS (plain spinlocks waited
on with interrupts off).*

## 1. What broke

os64 has always used a CPU's **local APIC ID** as its number: per-CPU arrays
are indexed by it, the core-local storage (CLS) slot is chosen by it, the log
stamps it, `/proc` prints it, `PINCORE=` takes it. On every machine os64 had
met (QEMU, the P5), APIC IDs are dense, 0..N-1, so "the APIC ID" and "the
Nth CPU" were the same number and nothing could tell them apart.

**The AMD 3900X numbers its APICs sparsely.** Zen 2 gives each CCX four ID
slots and a 3900X CCX uses three, so its 24 hardware threads are APIC IDs
0-5, 8-13, 16-21, 24-29. Firmware also lists the CPUs in an order where the
first four are not 0, 1, 2, 3. Two boots on 2026-10-19 died two ways:

| Boot | What happened | Mechanism |
|---|---|---|
| all CPUs | hung after the pre-boot tests, no panic | APICs 24-29 index `MAX_CPUS` (24) arrays one-to-six slots past their end: `kCoreLocalStorage`, `kTSSPerCPU`, the `mp_isrSaved*` set, the scheduler's flags. Silent corruption of whatever follows each array |
| `MAXCORES=4` | `#PF` writing address 0 in `ap_wakeup_after_stack_switch` on APIC 4 | the wake loop reads the roster by APIC ID (`kCPUInfo[apic_id].goto_address`) and woke the wrong CPU; that CPU then read `kCPUInfo[4]` from a 4-entry array and wrote its TPR through a null pointer |

A third symptom was waiting behind them: the log rings are allocated for
Limine's dense CPU count but `printd` indexes them by `read_apic_id()`, and
`log_store_entry` panics on an index `>= MAX_CPUS` — so APIC 24 would have
panicked on its first log line.

**Reproduced in QEMU** (8.2): `-smp 24,sockets=1,dies=4,cores=3,threads=2`
lays the APIC IDs out exactly like a 3900X (die << 3 | core << 1 | thread),
and today's kernel hangs after "pre-boot tests: 35 passed", silently, the
way the real machine hung after its tests. That topology is this design's acceptance test.

## 2. The ruling

Chris, 2026-10-19: **a person sees logical CPU numbers, 0..N-1, always** —
"that logical mapping is mandatory for me. I don't care if physical core 6
actually exists." And **the word is "CPU"**, not "core": a hardware thread is
what the scheduler runs on, and that is what a CPU is.

## 3. The design: one number inside the kernel

**A CPU number (`cpu`) is dense, 0..`kCPUCount`-1, and it is the ONLY number
the kernel uses to mean "a CPU".** Every per-CPU array, the CLS slot, thread
pins and history, the log, `/proc`, `/sys/cpu/<n>`, `PINCORE=` — all CPU
numbers.

**The APIC ID becomes a hardware address**, appearing only where the hardware
wants one (section 6). It stays visible to people as a fact *about* a CPU
(`/sys/cpu/<n>/state` says `apic: 8`), never as the name of one.

Why one internal number rather than "APIC ID inside, logical outside" (the
option first floated): two numbers in circulation that are both a `uint32_t`
called something-about-a-CPU is precisely what produced this bug, and half
the existing code already got it wrong in each direction. With APIC IDs
kept internal, every array would need sizing for the APIC range (with holes),
every loop would need the roster indirection, and every user-facing surface
would need converting on the way out. With CPU numbers internal, the arrays
are already the right size, loops over `0..kCPUCount` are correct by
construction, and the conversion happens at a handful of hardware doors.

### 3.1 A type the compiler polices

```c
typedef struct { uint32_t id; } apic_id_t;     // a local APIC's hardware ID
```

A struct, not a typedef of an integer: an `apic_id_t` cannot index an array,
cannot be compared with a CPU number, and cannot be passed where a `cpu` is
expected — each is a build error. The places that hold one are few and named
(section 6), so the wrapping costs little, and it turns this whole class of
bug from "silent corruption on somebody's new machine" into a compile error
on the author's. (`cpu` stays a plain `uint32_t`; it is the number everything
indexes with.)

**Question for review:** worth the friction? The alternative is naming
discipline alone (`cpu` vs `apic`), which is what failed here.

### 3.2 Who gets which number — the roster

`cpu_roster.c`, **pure** (no kernel headers, host-tested like `frames.c` and
`acpi_tables.c`): given Limine's list of LAPIC IDs, the BSP's LAPIC ID, and a
cap, it returns the order and the inverse table.

- **CPU 0 is the BSP**, always, by construction (Limine's `bsp_lapic_id`).
  Today's code assumes "core 0 is the BSP" (`init_SMP` maps the BSP's LAPIC
  for `core == 0`), and only Limine's list order made it true.
- **The rest in ascending APIC ID order.** Not Limine's order (which is MADT
  order: on AMD, every core's first thread, then every sibling). Ascending
  APIC order gives a topology-shaped numbering — on Zen, CPUs 2k and 2k+1 are
  SMT siblings — and on every dense machine it makes **CPU number == APIC ID
  exactly**, so on the P5 and in QEMU nothing anyone has typed changes meaning.
- **The cap** (`MAXCORES=` / `MAX_CPUS`) takes the first N of that order.
- Output: `kCPUs[cpu].apic` (`apic_id_t`) and `kApicToCPU[256]` (`uint8_t`,
  `0xFF` = no such CPU). xAPIC IDs are 8 bits; a CPU whose ID will not fit
  (x2APIC-only, > 254) is left off the roster and said so on the glass.

**Lifecycle:** built ONCE, by the BSP, in `kernel_main` before logging
init (the log rings are per-CPU and sized from it), from Limine data only.
Read-only from then on; no lock, ever. Nothing is ever removed (no hot-plug;
"online-capable" CPUs in the MADT are not on Limine's list).

### 3.3 The CLS slot, and how a CPU learns its own number

`kCoreLocalStorage[cpu]`. CLS gains `cpu`; `apic_id` stays (as `apic_id_t`)
for the code that addresses hardware *from* a CPU.

An AP learns who it is exactly once: `ap_wakeup_entry` reads its LAPIC ID
register, looks it up in `kApicToCPU`, and from then on has `cpu` in a
register until `init_core_local_storage(cpu)` publishes GS. An APIC ID not in
the table there is a **panic by name** — it would mean Limine started a CPU
the roster refused, which is a bug, not a machine.

After that, **"which CPU am I" is `gs:[cls.cpu]`**, everywhere — never the
LAPIC ID register (an MMIO read, slower, and the wrong number). The exception
and panic paths keep a fallback for a fault before GS is set: read the
register, translate through `kApicToCPU`, print both.

## 4. MAX_CPUS

Stays what it is — a bound on CPU *numbers*, i.e. a count — and grows from
24 to **64**: a 3950X is 32 threads, a Threadripper 3970X 64, and 24 left the
3900X with no headroom at all. The cost is small and static:

| Array (per CPU) | today (24) | at 64 |
|---|---|---|
| `kNmiSnapshot` | 11.7 KB | 31 KB |
| `kCoreLocalStorage` | 4.6 KB | 12 KB |
| `kTSSPerCPU` | 2.5 KB | 6.7 KB |
| `mp_isrSaved*` (31 arrays) | 6 KB | 16 KB |
| everything else | small | small |

plus `GDT_ENTRIES` 128 → 192, because each CPU's TSS descriptor takes two
GDT slots (64 × 2 + the fixed entries). Log rings stay 16 MB each but are
allocated per CPU **on the roster**, not per Limine entry, so `MAXCORES=4`
now also saves the memory it implies.

A machine with more CPUs than `MAX_CPUS` brings up the first 64 of the
roster's order and prints how many it left parked — the same treatment
`MAXCORES` gets today.

## 5. What people see

| Surface | Today | After |
|---|---|---|
| `PINCORE=<n>` | an APIC ID | `PINCPU=<n>`, a CPU number (section 5.1) |
| `/proc/<pid>/...` `core` / `affinity` lines | APIC IDs, labelled `core` | CPU numbers, labelled `cpu` / `affinity` |
| `/sys/cpu/<n>/` | `n` happens to be the APIC ID | `n` is the CPU number; `state` gains `apic: <id>` |
| log `%c`, classic format `AP%c` | the APIC ID (`read_apic_id()`) | the CPU number from CLS. The `AP` label stays (section 5.1) |
| exception reports `>>> AP 4 ...` | the APIC ID | `>>> AP 2 (APIC 4) ...` — the log's label, and on a panic both numbers earn their place |
| coreprobe `apic` lines | APIC ID | CPU number, APIC beside it |
| boot line `SMP: ... (24 core(s) initialized)` | | `24 CPUs`, and the APIC range when it is not dense (`APIC IDs 0-29, sparse`) |

`top`, `htop` and `libos64/monitor.c` read `/sys/cpu/<n>` and `/proc`; with
dense numbers their loops become correct for free. The `core` → `cpu` label
change in `/proc` breaks their parsing, so it lands with the matching
userland edit in the same PR — they are Chris's programs, so that edit is
shown to him rather than slipped in.

### 5.1 Rulings on the words (Chris, 2026-10-19)

- **"CPU" is the word** for what the scheduler runs on — one hardware thread.
  "Core" is kept for the physical core, the thing two SMT threads share, so
  that both words stay available and each means one thing. Boot lines,
  `/proc`, `/sys` and messages say CPU.
- **`MAXCORES=` becomes `MAXCPUS=`, `PINCORE=` becomes `PINCPU=`.** The old
  spellings stay accepted as aliases — `MAXCORES` sits in the P5's own
  `limine.conf`, and boot tokens do not travel with a refresh — and an alias
  prints one line saying what it is now called, so it fades rather than
  living forever. `PINCORE` is then free to mean, someday, "either thread of
  this core".
- **The log keeps `AP`.** Classic format stays `%k (0x%T) AP%c: %m`; only the
  number after it changes (it becomes the CPU number). Kept for sentiment,
  and ruled as such — it is the Intel MP spec's "application processor", so
  `AP0` names the bootstrap processor with the word for the others, and that
  is known and fine.

## 6. The hardware doors (the only places an `apic_id_t` lives)

| Door | Converts |
|---|---|
| `send_ipi(cpu, ...)` | `kCPUs[cpu].apic` into the ICR destination field |
| IOAPIC redirection destination (`ioapic_route_irq`, the input-IRQ and PIT routing in `kernel.c` / `apic.c`) | a CPU number in, `kCPUs[cpu].apic` out |
| RTL8125 MSI address destination (`r8125.c`) | CPU 0's APIC ID |
| `ap_wakeup_entry` | LAPIC ID register → `kApicToCPU` (section 3.3) |
| Limine's SMP response and the MADT (`cpu_roster.c`, `acpi_tables.c`) | where APIC IDs come from |
| exception/panic fallback before GS | register → table, both printed |
| `/sys/cpu/<n>/state`, `/sys/acpi` | APIC IDs shown as facts |

Everything else in the kernel — the 219 `apic_id`-shaped sites today, of which
the audit below is the map — becomes `cpu`.

## 7. The audit (what the PR touches)

- **Per-CPU arrays** (all `MAX_CPUS`-sized): `scheduler.c`'s 30-odd
  (`mp_isrSaved*`, `mp_inScheduler`, `mp_schedulerEnabled`, `kIdleTicks`, …),
  `tss.c` (`kTSSPerCPU`, `kTSSSelector`, the IST/NMI stack tops),
  `nmi_probe.c`, `smp.c` (`kCoreLocalStorage`, `kLocalAPICTimerSpeed`),
  `smp_core.c` (`kDiag*`, `kSpecCtrl*`), `kernel.c` (`kIdleTasks`),
  `log.c` (`core_log_buffers`). Their *size* is right already; their *index*
  becomes `cpu`.
- **`scheduler.S`**: one load, `gs:[CLS_APIC_ID_OFFSET]` → `gs:[CLS_CPU_OFFSET]`
  (asm-offsets generates it). Every `mp_isrSaved*` store indexes off that
  register.
- **`->apic_id` field uses**: scheduler.c 21, smp_core.c 12, nmi_probe.c 5,
  kworker.c 4, task.c, log.c, knet.c 2 each, and single sites in syscall.c,
  random.c, compositor.c, nvme.c, simple_exceptions.c, doorbell.c.
- **`read_apic_id()` callers** (15): logging, exception reports, watchpoint
  sync, the settle/IPI paths in smp_core.c — all become `cls->cpu` except the
  pre-GS fallback.
- **Loops `0..kMPCoreCount`** (23): those that index an array directly become
  correct; those that already walk `kCPUInfo[i].apicID` lose the indirection.
- **The roster**: `kCPUInfo[]` (today indexed by Limine position) becomes
  `kCPUs[cpu]`; its `apicID` field becomes `apic_id_t apic`.
- **Thread fields**: `mp_apic` (the pin) and `lastRunApicID` (the history)
  become CPU numbers, renamed to say so; `THREAD_NO_AFFINITY` is unchanged.
  `task_env_pin` validates `cpu < kCPUCount`.
- **`kMPCoreCount`** → `kCPUCount`. `BOOTSTRAP_PROCESSOR_ID` → CPU 0.

**Renaming policy** (the stdint precedent — no sweeps): every name this PR
must touch anyway takes the CPU vocabulary (`kCPUCount`, `cpu`, `kCPUs`).
Names it does not otherwise touch — `core_local_storage_t`, `cls`, the
`mp_` prefix — keep theirs. User-visible words become "CPU" throughout.

## 8. Verification

1. **Host:** `tools/test_cpu_roster_host.sh` — the roster on dense lists, the
   3900X list in MADT order, a BSP that is not the lowest ID, duplicates, an
   ID of 255, a cap, more CPUs than `MAX_CPUS`.
2. **QEMU, sparse** (`make run-sparse`, `dies=4,cores=3,threads=2`): hung
   before this change; must boot to a shell with all tests green, `MAXCPUS=4`
   and the `MAXCORES=4` alias included. `/sys/cpu` lists 0..23; `state` shows
   the gapped APIC IDs; `pincputest` passes with CPU numbers.
3. **QEMU, dense** (`-smp 8`, BIOS and UEFI): unchanged behaviour; CPU number
   equals APIC ID everywhere it is printed.
4. **A tripwire test:** a preboot check that every CPU's `cls->cpu` matches its
   slot and that `kCPUs[cls->cpu].apic` matches its LAPIC register.
5. **The P5**, then **the 3900X** — Chris.

Review: Codex, one round minimum — this is the SMP core (bring-up, IPIs, the
scheduler's per-CPU state).

## 9. Out of scope

- **x2APIC mode** (IDs > 255). os64 runs the LAPIC in xAPIC mode; such CPUs
  are left off the roster, loudly. A machine that needs them is a project.
- **NUMA / topology awareness** (which CPUs share a CCX or a cache). The
  ascending-APIC numbering keeps siblings adjacent, which is a convenience,
  not a scheduler policy.
- **Hot-plug**, and the MADT's online-capable CPUs.

## 10. Open questions

1. **`apic_id_t` as a struct** (3.1): yes, or naming discipline?
2. **Ascending-APIC numbering** (3.2) vs Limine's MADT order. Ascending keeps
   dense machines' numbers unchanged and siblings adjacent; MADT order is what
   Linux does (its `cpu0..N` follow the firmware's listing).
3. ~~Token names~~ and 4. ~~the log's `AP`~~ — ruled, section 5.1.
