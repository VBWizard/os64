# The stray write into `mp_isrSavedRFlags[]`

**Status: SOLVED — fix applied 2026-08-03** (tempStack 32KB + a canary
strip at its floor, verified after every AP handshake; Chris ratified the
size-over-architecture route). The stray writer is
**AP bring-up C code overflowing the shared 1KB `tempStack`**
(`smp_core.c:73`), whose .bss placement sits a few hundred bytes ABOVE the
`mp_isrSaved*` arrays — deep frames punch out the bottom of the stack and
land in the per-core register arrays. See "2026-08-03: the culprit" at the
end. Everything below this line is the original investigation, kept because
its method was what cracked it.

**Status (historical): OPEN.** The mechanism is proven, the culprit is not.

If a tripwire fired and sent you here, read "What the tripwire means" at the
bottom first, then come back for the story.

---

## The symptom, and why it wore so many masks

For weeks os64 suffered an intermittent fault on an idle task — usually
`/idle2`, always early, roughly one boot in three. It was reported as a `#GP`
at `scheduler_continue_2+0x1c` (the `iretq`) with error code `0x1420`. Selector
index 644, in a GDT with 128 entries. Nobody could explain the selector,
because there was no selector.

On 2026-08-02, with the twelve new exception gates in place, the same bug
finally surfaced as a **`#DB` — Debug Exception, vector 1** on `/idle2`, and
that changed everything: `#DB` is a *hardware-explained* fault. `DR6` says why
it fired.

## The evidence, taken live from a frozen QEMU

QEMU was left halted at the panic and read through the monitor (see
"Method" below — everything here is reproducible).

**Per-CPU register state.** CPU#2 versus its healthy siblings:

|            | healthy cores | CPU#2 (`/idle2`) |
|------------|---------------|------------------|
| `RFL`      | `00003202`    | **`00043402`**   |
| `DR6`      | `ffff0ff0`    | **`ffff4ff0`**   |

`DR6` bit 14 is **BS — single-step trap**. The hardware is stating plainly
that `RFLAGS.TF` was set when the thread resumed. Not a breakpoint, not a wild
jump: the CPU was *asked* to single-step and did exactly one instruction.

**The faulting RIP.** `0xffffffff8003be21`, one byte past
`0xffffffff8003be20`, which disassembles to:

```
ffffffff8003be20 <task_idle_loop>:
  be20:  55              push rbp        <- resumed here
  be21:  48 89 e5        mov rbp,rsp     <- #DB fired here, one instruction later
  be24:  fb              sti
  be25:  f4              hlt
  be26:  eb fc           jmp be24
```

`be20` is the function's **first instruction**, so `/idle2` had never run. This
was its first ever dispatch. (`/idle0` and `/idle1`, which work, sit at `be26`
— parked mid-loop, as a running idle thread should be.)

**The thread's own saved state is CLEAN.** Read out of the guest at
`thread + 0x98`:

| thread | `regs.RIP` | `regs.CS` | `regs.RFLAGS` |
|--------|-----------|-----------|----------------|
| `/idle0` (0x21) | `ffffffff8003be26` | `0x28` | `0x3202` |
| `/idle1` (0x22) | `ffffffff8003be26` | `0x28` | `0x3202` |
| `/idle2` (0x23) | `ffffffff8003be20` | `0x28` | `0x0202` |
| `/idle3` (0x24) | `ffffffff8003be20` | `0x28` | `0x0202` |

No TF anywhere. **The corruption is not in the thread structures.**

**The per-core arrays are where it lives.** `mp_isrSavedRFlags`:

```
[0] = 0x0000000000003202   ok
[1] = 0x0000000000003202   ok
[2] = 0xffffffff8004f7d0   <-- a POINTER, not flags
[3] = 0x0000000000003202   ok
```

Decode `0x8004f7d0` as RFLAGS and it is the fault exactly: bit 8 **TF**, bit 9
IF, bit 10 DF, bits 12-13 IOPL=3, bit 18 AC. Strip TF and IF the way `#DB`
delivery does and you get **`0x43402`** — the live `RFL` on CPU#2, to the bit.

**Only that one slot.** `mp_isrSavedRIP[2]` = `ffffffff8003be20` and
`mp_isrSavedCS[2]` = `0x28`, both correct and both matching `/idle2`'s thread
struct. So `scheduler_load_thread` ran and wrote all three slots properly, and
something overwrote **one qword** afterward. This is a targeted stray write,
NOT a smear through the array — which rules out the obvious candidate, since
`tempStack` (the AP's 1KB bootstrap stack) sits at `0xffffffff800b22a0`, above
these arrays, and an overrun off its bottom would damage a contiguous range.

**What the stray value points at.** `0xffffffff8004f7d0` is in `.rodata`.
Dumping it yields `AJA Video`, `Kona 3G`, `Corvid 3G`, `Kona LHe+` — the **PCI
device-name lookup table** (`pci_lookup.c`). Whatever wrote that slot was
holding a pointer into the PCI name table at the time.

## The conclusion

**Every sighting of this bug is one thing: an 8-byte stray write into
`mp_isrSavedRFlags[core]` between `scheduler_load_thread` and the `iretq`.**

Which bit of the garbage happens to be wrong decides the symptom:

- garbage in the **flags** -> `#DB` (TF), or a thread running with DF/AC set
- garbage in a **selector slot** -> the `#GP` at `iretq` we chased for weeks
- error code `0x1420` was never a selector. It was debris.

`/idle` is the usual victim only because idle threads are dispatched more often
than anything else in the system. There is nothing special about them.

## Why the existing TF tripwire never fired

`scheduler_load_thread` (scheduler.c) checks `thread->regs.RFLAGS & 0x100` and
clears TF before dispatch. It is guarding **the wrong side of the copy**: the
thread struct is clean (proven above), and the corruption lands in the per-core
array *after* the value is copied across. The tripwire has been watching a
value that was never the problem.

## The fix that isn't a fix yet

The next step is NOT a repair — the writer is still unnamed. It is to move the
check to where the value is actually consumed: **`scheduler.S`, immediately
before the `iretq`**, at the point that already reads the slot to `or rbx,
0x3000`. Any value with TF set, or with bits outside the legal RFLAGS mask,
gets reported (core, value) and sanitized to a safe `0x202 | 0x3000`.

That converts a random boot-killer into a logged line naming the moment — and
crucially it will fire on *whatever* core is hit next, not only on `/idle2`,
which is what narrows the suspect list.

## Method (so this is repeatable)

QEMU's monitor is on telnet 127.0.0.1:55555 (`QEMU_BASE_FLAGS`). With the guest
halted at a panic, the whole machine is still readable:

```
info registers -a          # every CPU: RIP, RFL, DR0-7. DR6 explains any #DB.
x/8gx <kernel virtual>     # read guest memory through the current CPU's mapping
xp /64c <physical>         # read physical memory as characters
```

Kernel globals are at their link addresses (`x86_64-elf-nm os64_kernel`), and
the higher half is mapped under every CR3, so any CPU can read them.

Struct offsets come from the kernel's own DWARF, without a running target:

```
gdb -q -batch -ex "ptype /o struct s_thread" kernel/bin/os64_kernel
```

That is how the table above was built: `kIdleTasks` -> `task_t*` -> `+144`
(`threads`) -> `+32` (`regs`) -> `+120/128/168` (RIP/CS/RFLAGS).

## What the tripwire means

If you are reading this because a line like

```
RFLAGS TRIPWIRE: core N had mp_isrSavedRFlags = 0x................
```

appeared in the log: the stray write just happened, and the value is a clue to
who wrote it. Resolve it. A `.rodata` pointer names a table; a `.text` pointer
names a function; a stack address names a frame. The 2026-08-02 sighting was a
pointer into the PCI device-name table. Two data points on different boots
would likely be enough to name the culprit outright.

---

## 2026-08-03: the culprit — tempStack underflow during AP bring-up

The predicted second data point arrived (double sighting: `/idle3` #GP
EC=0x400 and `/idle6` #GP EC=0xf2c8, both at the iretq, same boot) and the
guest was kept alive for forensics. What the frozen machine gave up:

**The droppings.** `mp_isrSavedCS[3]` = `0x400`, `CS[4]` = `0x0`,
`CS[6]` = `0xffffffff8004f2c8`, `RIP[6]` = `0xffffffff8004f1a8` — and both
pointers resolve to **printd format strings in .rodata** ("\tSetting page
table entry at 0x%016lx, index 0x%04x, ..." — paging.c's per-PTE DETAILED
logger). The 2026-08-02 value was a PCI *name string* — also .rodata, also
something that gets handed to printd. The corruption always looks like
printd's argument neighborhood because it IS printd's argument neighborhood.

**The fingerprint field.** `MAX_CPUS` is 24 but only 8 cores exist, so
slots 8–23 of every array are written by NO legitimate code — yet they held
droppings that persist forever (nothing overwrites a ghost slot):
`RFlags[10]` = the same paging format-string pointer, then alternating
16-byte pairs `{0x200, 0xc000000000000000}` marching through
`RFlags[11..14]` and `RIP[8..11]`. That alternation is a **varargs register
save area**: six GPR slots (level, fmt, args) followed by eight XMM spills
all holding the same stale 128-bit value. Someone ran a varargs prologue
with RSP pointing into these arrays.

**The confession.** `tempStack` (the AP bootstrap stack, `smp_core.c:73`,
**1,024 bytes, ONE shared global**) links at `0xffffffff800b3420` — a few
hundred bytes ABOVE the victim corridor. Its residue still contained a
coherent saved-RBP chain with return addresses resolving to:

```
ap_wakeup_entry (smp_core.c:245)        <- the AP's C entry, ON tempStack
  kmalloc_aligned -> kmalloc_common
  paging_map_pages (paging.c:348)
    paging_map_page (paging.c:216/230)
    allocator_unlock (allocator.c:34)
```

with physical page addresses and PTE flag values (0x023, 0x027, 0x1000) as
frame arguments. The measured high-water of that chain alone is ~600 bytes;
one DETAILED printd from the deep end (varargs save area ≈176B + sprintf
frame + 256B message buffer + log_store_entry) busts 1KB, and the frames
keep descending — out the bottom of tempStack and straight through
`mp_lastIretqRIP`, `R15..R8`, `CS`, `RIP`, `RFlags`. Which qword gets hit
depends on that boot's exact frame layout; which SLOT is active decides the
mask (#GP from a selector slot, #DB from TF in a flags slot, #UD from a
poisoned RIP slot).

**Why it was intermittent, and why it got worse.** The overflow depth
tracks logging volume during AP bring-up. Quiet boots stay inside 1KB;
DEBUG_DETAILED eras (the PCI-name era on 8/2, the paging-PTE era during
the logd DETAILED experiments) go deep. The bug's frequency followed the
logging configuration all along — "one boot in three" was never chance,
it was verbosity.

**The fix (ratified by Chris, applied 2026-08-03):** `tempStack` grew to
32KB (`TEMP_STACK_SIZE`, smp_core.c) — safe to share because
`ap_wake_up_aps` serializes bring-up through the `coreInitialized`
handshake — with a 64-byte canary strip (`TEMP_STACK_CANARY`, "CANARY!!"
in a hex dump) at the stack's floor, armed before the first wake and
verified after EVERY AP's handshake; a smashed strip is a panic naming
this document, because the .bss below may already be dirty. The per-AP
real-stack alternative (BSP pre-allocates, ~384KB for MAX_CPUS) was
declined as architecture the problem doesn't need. The RFLAGS tripwire in
scheduler.S stays regardless — defense-in-depth that names any FUTURE
stray writer by value, built the same day this was caught.
