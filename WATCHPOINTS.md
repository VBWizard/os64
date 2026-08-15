# WATCHPOINTS.md — asking the CPU who scribbled your memory

*Born 2026-08-14, out of the P5's page-table corruption. The mechanism is in
`kernel/src/watchpoint.c`; the doctrine and the four hardware limits are in
`kernel/include/watchpoint.h`. This file is the map: what exists, how to use
it, and what it cannot do.*

## Why this exists

"Something is corrupting this memory" is the most expensive question this OS
asks, and it keeps asking it: the CLS corruption hunt, the idle-task RFLAGS
stray write, the scribbled console text, the pipe double-destroy, and finally
an NVMe DMA buffer's page table entry that changed twice on real hardware with
two different wrong values.

Every one of those was chased with **tripwires** — a check placed *after* the
fact, narrowing the window by guesswork until the culprit had nowhere left to
hide. That technique works and this OS is good at it. It is also slow, and it
scales with how often the corruption happens, which is exactly the wrong thing
to depend on when it happens once an hour.

x86 has had the other answer since the 386: four registers that trap the
**instruction** performing an access to an address you name. A watchpoint turns
"something corrupts this eventually" into a symbolized call chain, on the first
occurrence, with the machine still standing.

## The three instruments, weakest to strongest

os64 now has a ladder. Reach for the cheapest rung that answers the question.

| Instrument | Where | Tells you |
|---|---|---|
| **Tripwire** (e.g. the NVMe DMA check) | at a consumer, before use | "it was already broken by the time I looked" |
| **Mapping sentinel** (`paging_sentinel_*`) | at phase boundaries | *which phase* broke it (burial? compaction? task build?) |
| **Watchpoint** (this file) | the CPU itself | *which instruction* broke it, and its call chain |

The sentinel is the middle rung and it is not redundant: it costs a couple of
page-table walks at events that already happen, and it works on addresses no
watchpoint can cover (there are only four of those, forever).

## Using it

**From the kernel commandline** — the usual way:

```
WATCH=<hexaddr>[:len[:kind[:action]]]
    len     1 | 2 | 4 | 8                       (default 8)
    kind    w = write, a = access, x = execute  (default w)
    action  h = halt, t = trace                 (default h)

WATCH=ffff8000003f8fd8           8 bytes, on write, stop the machine on the first hit
WATCH=ffff8000003f8fd8:8:w:t     the same, but report and KEEP RUNNING
```

**From C** — when the address is not known until runtime:

```c
int slot = watchpoint_arm(addr, 8, WATCH_WRITE, WATCH_HALT, "what this is");
...
watchpoint_disarm(slot);
```

`WATCHDMA` is the first real consumer: it resolves each NVMe controller's
write-DMA-buffer *page table entry* (`paging_pte_address`) and watches those
eight bytes. There is a boot entry for it in `limine.conf`.

**TRACE vs HALT.** Halt is right when the corruption happens once and the
report *is* the answer — a dead machine cannot overwrite its own evidence.
Trace turns a watchpoint into a logger: every writer announces itself with a
full call chain and the system carries on. Trace is expensive (a report is
hundreds of polled-serial milliseconds), so aim it at something rare.

## Reading a hit

A hit is an ordinary fatal-exception report with a headline:

```
>>> EXCEPTION: Debug (#DB) — WATCHPOINT 0 HIT — NVMe write DMA buffer's PTE at
    0xffff8000003f8fd8 was WRITTEN (hit #1). RIP below is the instruction AFTER
    the access <<<
```

…followed by every register, the excepting task, the stack window, and the
symbolized call chain. **RIP is one instruction past the store** — data
watchpoints are traps, delivered after the access retires. The call chain is
unaffected; the store you want is the one just above the reported RIP.

## What it cannot do (all four are hardware, not policy)

1. **Four.** That is the budget, forever.
2. **Linear addresses, not physical.** The same bytes reached through a
   different mapping will not trip it. Watch every alias that matters — or know
   which one you are blind to. (`paging_pte_address` returns the HHDM alias,
   which for pool-drawn page tables is the only one that exists; that was
   *checked*, not assumed.)
3. **Per core.** DR0-3/DR7 are per-CPU. This module keeps a global table and
   mirrors it onto every core — at bring-up (`watchpoint_sync_this_core`, called
   beside `pat_init_this_core`) and by IPI (`IPI_WATCHPOINT_SYNC_VECTOR`) for
   cores already running. Get this wrong and you watch one eighth of the
   machine, which is worse than not watching at all because it looks like proof.
4. **The CPU is not the only writer.** A device DMA-ing into memory executes no
   instruction and trips nothing. **A silent watchpoint over memory that still
   changes is EVIDENCE, not failure**: it says the write did not come from a
   CPU. On a bug about a DMA buffer, that null result may be the whole answer.

## Keeping it honest

`TESTWATCH` (fifth member of the TESTPANIC family) arms a watchpoint on a bait
variable and stores to it twice — once in TRACE mode, once in HALT mode. It
proves DR programming, the trap reaching the unified `#DB` path, slot
attribution, the named report, and above all that a traced hit actually
**resumes**. Like every other member of that family it exists because this code
only runs when something has already gone wrong, which is precisely the kind of
code that rots unnoticed.
