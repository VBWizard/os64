# AVX for ring 3

*Design, 2026-11-16, Fable. Ruled the same day by Chris: os64 lets programs
use AVX. The consumer is the video arc (FFmpeg's and dav1d's fast paths),
and nothing argued for waiting. Evidence that the QEMU harness can prove
it: `~/os64-opus-kit/AVX_QEMU_PROBE_REPORT.md` (all six rows passed).
Retires the DEBTS row "No XSAVE, so no AVX".*

## What changes, in one paragraph

The kernel turns on the 256-bit registers for ring 3 and carries them as
thread context. The register image a thread owns grows from the 512-byte
FXSAVE image to the 832-byte XSAVE image, the context switch saves and
restores it with `xsave64`/`xrstor64`, and every signal frame carries the
larger image. The kernel itself stays `-mno-sse` and never touches the
registers. Userland stays built for SSE2: a program that wants AVX asks the
CPU first (CPUID, then `XGETBV`), and no syscall is involved.

## What is NOT in this design

- **No compiler flag changes anywhere.** Nothing is built `-mavx`. Ruled.
- **No AVX-512, no other state component.** XCR0 is written as exactly
  x87 + SSE + AVX (value 7), whatever else the CPU offers.
- **No skipping of unused state** (`xsaveopt`). See § The save instruction.
- **No use of AVX by anything.** libav's assembly and the demo are their
  own slices; this one only makes the registers safe to use.

## The rules

**R1. XCR0 is written, never inherited.** Firmware (UEFI) leaves XSAVE on
with whatever it liked in XCR0. Each CPU's bring-up sets `CR4.OSXSAVE` and
writes XCR0 = 7, then reads it back. It is the same rule the speculation
controls follow, for the same reason: what a second hardware thread arrives
with is not ours.

**R2. One mode per boot, decided on the BSP, the same on every CPU.**

| Mode | When | The switch uses | Ring 3 sees |
|---|---|---|---|
| `avx` | the CPU reports XSAVE and AVX, CPUID leaf 0xD agrees with the layout in R3, and `NOAVX` is absent | `xsave64` / `xrstor64`, mask 7 | CPUID's OSXSAVE bit set, `XGETBV` = 7 |
| `sse` | anything else | `fxsave64` / `fxrstor64`, as today | OSXSAVE clear; an AVX instruction is `#UD` (exit 206), as today |

An AP that cannot enter the BSP's mode panics by name. A machine whose
CPUs disagree about AVX is not one os64 runs on.

**R3. The image is 832 bytes, 64-byte aligned, and that is a constant.**
With exactly these three components the standard XSAVE format is fixed by
the architecture: the 512-byte legacy area, a 64-byte header, and the AVX
component at offset 576 for 256 bytes. `fpu_state_t` becomes that size and
alignment in both modes (`sse` mode uses its first 512 bytes). At boot the
BSP checks CPUID leaf 0xD against those three numbers; a CPU that answers
differently gets `sse` mode and a boot line saying why. This replaces the
DEBTS row's "variable-size image": a per-boot size would only be needed for
components this design refuses to enable.

**R4. The kernel never restores a header it did not write.** `xrstor64` is
a kernel `#GP` if the image's header names a component outside XCR0, has a
nonzero reserved byte, or carries a reserved MXCSR bit; a set compaction
word is a `#GP` on a CPU without XSAVEC and, on one with it, a request for
a layout the kernel never writes (R6). Signal frames live in user memory, so ring 3 can hand back any
bytes it likes. Therefore sanitizing lives INSIDE `fpu_restore`, where the
MXCSR mask already is, so that no caller can restore an unsanitized image:

- `XSTATE_BV` (header word 0) is ANDed with 7;
- header bytes 8 to 63 are zeroed;
- MXCSR is ANDed with the CPU's `MXCSR_MASK` (today's rule, kept).

Sanitize, never refuse: a program that scribbled on its own frame gets its
own scribble back as register values, which is its business. The invariant
is about the kernel not faulting.

**R5. A new thread starts from the architectural reset state.** The image
is the one `fpu_state_init` builds today, with `XSTATE_BV` = 3 (x87 and SSE
come from the image; AVX is marked absent, so the restore zeroes the upper
halves). No thread can see another's leftovers.

**R6. The standard format, in the frame too.** Not the compacted format
(`xsavec`/`xsaves`): it buys nothing with three components, the harness
does not offer it, and a signal frame in a documented, fixed layout is
worth more than a few bytes.

## The image's lifecycle

| Where an image lives | Who wrote its header | Who may restore from it |
|---|---|---|
| `thread->fpuState`, thread off-CPU | `xsave64` at the last switch-out, or `fpu_state_init` at birth | the scheduler, loading that thread |
| `thread->fpuState`, thread on-CPU | stale: the live registers are the truth | nobody, until the next save |
| a signal frame on the user stack | copied from `thread->fpuState` right after a save (§5, §9) or while off-CPU (§10) | nobody directly |
| `sigreturn`'s kernel-side snapshot of that frame | ring 3, possibly forged | `fpu_restore`, after it sanitizes (R4), into `thread->fpuState` and the live registers |

Every arrow that ends in a restore passes through `fpu_restore`. That is
the whole of R4, and the reason the function is the one door.

## The save instruction: plain `xsave64` first

`xsaveopt64` can skip writing a component a thread never touched, which
would spare 256 of the 832 bytes for every thread that does not use AVX.
This design does not use it yet, for two reasons:

- **The harness cannot test it.** The probe found QEMU reports every
  enabled component as in use, always. The skipping path would ship
  unexercised.
- **It has a rule that is easy to break later.** The CPU also skips a
  component it believes unchanged since the last restore *from the same
  address*. That is sound only while the kernel never edits a saved image
  in place and then saves over it without a restore in between. Today's
  code obeys that, but it is a rule about every future edit to this file.

So: plain `xsave64`, correct and fully provable in QEMU. The cost is the
larger copy at every switch, to be measured on the P5 before and after
(a switch-heavy workload: two threads pinned to one CPU trading a pipe,
timed with `micros()`). If that number shows, `xsaveopt64` is a
small follow-up slice with the rule above written beside it.

## The signal frames

`signal_frame_t` is a 64-byte head followed by `fpu_state_t`, so the image
already begins at offset 64 and the head's offsets (the ones the stub in
`task_exit_asm.S` reads) do not move. What changes:

- the frame grows by 320 bytes (576 to 896; the full frame by 336, 688 to
  1024, because the image's 64-byte alignment rounds it up — those 16
  bytes are a named `tail` member so every byte copied to ring 3 is
  initialized);
- the three delivery paths copy the larger image, unchanged in shape;
- `sigreturn` copies the larger frame in, and its restore goes through R4.

**No ring-3 program reads the frame's layout** (libos64 and the ABI headers
do not name it), so existing binaries are unaffected and nothing is
rebuilt for it. The frame's address on the user stack stays 16-aligned:
the kernel never runs `xsave64` or `xrstor64` on user memory, only on
`thread->fpuState` and copies.

Kernel-stack use: `sigreturn` holds one full frame and one short frame as
locals, about 2 KiB after this change, on a 64 KiB kernel stack.

## Exec

A program image that replaces another must start with the reset state
(R5), upper halves included. The builder checks what exec does to
`thread->fpuState` today and, if it carries the old program's registers
into the new one, resets it there; that would be a leak between programs
that this slice makes 256 bits wider.

## What ring 3 gets

- **Discovery is the CPU's.** CPUID leaf 1 reports OSXSAVE once CR4 has
  it; `XGETBV` then reads XCR0. Both are unprivileged. FFmpeg, dav1d and
  compilers' own `__builtin_cpu_supports` already do exactly this.
- **Floating-point faults are unchanged.** An unmasked AVX exception is
  `#XM`, vector 19, exit 219, as for SSE.
- **AVX-512 is `#UD`** (exit 206) on any CPU that has it, because XCR0
  does not enable its state.

## What the machine says about it

- **The boot line** (`fpu_report`) says which mode is in force and why:
  `...; AVX AVX2 FMA3 enabled, XSAVE per thread (832 bytes)`, or
  `...; AVX present, off (NOAVX)`, or `no XSAVE`.
- **`/sys/cpu/<n>/probe`** gains XCR0 and CR4's OSXSAVE bit, read on that
  CPU, beside the MSRs it already shows.
- **`NOAVX`** (uppercase) on the cmdline forces `sse` mode for that boot.
  It is the flashlight, in the pattern of `NETPOLL` and `USBPOLL`: the old
  path stays reachable and tested. Like every token it does not travel to
  the P5, and it does not need to: the default there is `avx`.

## The harness

- The makefile's default CPU model gains the AVX features (the probe's
  `QEMU_CPU` variable, default changed), on the precedent of `+rdrand`:
  the harness should look like the machines os64 runs on. The bare model
  stays one variable away and is how `sse` mode on a CPU without XSAVE is
  tested.
- `enforce` is not added to the default; it is for a probe.

## Proof

**Pure code, host-tested.** The image's rules (R3's layout constants, R4's
sanitizer, R5's initial image) go in a file with no kernel headers, with a
host test and mutants, in the pattern of `psf2.c` and `ansi.c`. The
sanitizer is the part a wrong byte offset would turn into a kernel `#GP`.

**Fixtures, in `avx` mode:**

| Fixture | What it proves |
|---|---|
| `fputest`, widened | the upper halves of all sixteen YMM registers survive preemption, migration between CPUs, and a signal handler that wipes every register, on each of the three delivery paths |
| `fputest`, new step | two threads pinned to ONE CPU, each holding its own pattern in the upper halves for some seconds: neither ever sees the other's (the leak the probe demonstrated) |
| `fputest`, new step | a handler forges its frame's image four ways (a component bit outside XCR0, the compaction word set, a reserved header byte, a reserved MXCSR bit) and returns: the program continues, the machine is alive |
| `/tests/avxprobe`, kept from the probe as a fixture | CPUID and `XGETBV` say yes, and 256-bit float, integer and FMA operations give right answers on CPU 0 and on another CPU |
| `fpfault ud`, re-aimed | it asks the CPU first: with AVX on it executes an AVX-512 instruction, with AVX off the `vzeroupper` it uses today; either way it must die with 206 |
| a new program started by exec or spawn | its upper halves are zero |

**The same suite in `sse` mode**, twice: with `NOAVX` on the AVX model, and
on the bare model. AVX steps skip by asking the CPU; everything else must
match.

**Boots:** the default 8 CPUs, `make run-sparse` (per-CPU steps get missed
on the sparse layout), and `testrun` against a baseline in each mode.

**On the P5** (Chris): `testrun`, the boot line, the switch cost before and
after, and the demo once it exists.

## Slices

| Slice | What | Review |
|---|---|---|
| **A** | everything above, in one PR. Enabling without the wide save is the leak, so there is no safe half | ring boundary and context switch: Codex, after one cold read |
| **B** | the demo (below) | userland and GUI: reviewed here |
| **C** | `xsaveopt64`, only if the P5's switch cost shows | Codex |

Slice A also retires the DEBTS row, re-trues `fpu.h`'s header comment, the
comment in `fpu_init_this_cpu` about clearing OSXSAVE, and CLAUDE.md
§ Floating Point ("XSAVE/AVX deliberately off"), in the same commit.
`tools/stale_refs.sh` for `FPU_FXSAVE_SIZE` if it is renamed.

The probe branch (`opus/avx-probe`) is reference only. Slice A starts from
`userland`; it may take `avxprobe.c` and the `QEMU_CPU` variable from it
and nothing of the `AVXPROBE` token.

## Slice A, as built (2026-11-16, Opus)

Built as designed. Where the code says it differently, or the design left
the choice open:

- **The pure file** is `kernel/src/fpu_image.c` / `fpu_image.h`: the layout
  constants, `fpu_image_init` (R5), `fpu_image_sanitize` (R4) and the mode
  decision as three steps, because the CPU must be asked in that order
  (`fpu_mode_before_layout`, then `fpu_mode_components` before XCR0 may be
  written, then `fpu_mode_layout` with the sizes CPUID reports for the XCR0
  just written). `tools/test_fpu_image_host.sh` checks the sanitizer against
  a word-wise model over random and forged images and against what
  `xrstor64` demands; `tools/test_fpu_image_mutants.py` breaks each rule.
- **The sanitizer masks MXCSR with the CPU's mask** (`kFPUFeatures.mxcsr_mask`,
  probed once per CPU at bring-up), not with the image's own `MXCSR_MASK`
  field. `fpu_state_from_user` therefore no longer re-plants that field
  before restoring: nothing reads it on the way back.
- **Exec**: os64 has none. A program starts as a new task whose thread
  `createThread` builds from `fpu_state_init`, and so does every thread a
  program makes; both start from R5's image. `avxprobe` proves it from ring 3:
  it dirties all sixteen upper halves, then spawns itself pinned to CPU 0 and
  CPU 1, and each child checks it was born with them zero.
- **`fputest`** gained the third delivery path (§9: a fault on page zero,
  the SIGSEGV handler stepping the frame's RIP past it), the upper halves of
  all sixteen YMM registers on every thread and path, the one-CPU step
  (`fputest --pinned`, spawned with `PINCPU=1`), and the four forgeries. Its
  summary lines also reach the serial wire.
- **A preboot kernel test** (`fpu_image_on_this_cpu`) checks on the BSP that
  YMM0's upper half lands at 576, that the reset image zeroes the upper
  halves, and that an image carrying all four forgeries restores, in both
  modes.
- **`QEMU_CPU`** defaults to `qemu64,+rdrand,+rdseed,+ssse3,+sse4.1,
  +sse4.2,+popcnt,+xsave,+xsaveopt,+avx,+avx2,+fma,+bmi1,+bmi2` (the probe's
  model without `enforce`).

**Evidence, QEMU 8.2.2 TCG** (TESTRUN entry; the baseline before this slice
was 70 / 0 / 6, built-in 37/0, 34/0, 8/0):

| Boot | Boot line says | Built-in | testrun |
|---|---|---|---|
| AVX model, 8 CPUs (twice) | `AVX AVX2 FMA3 enabled, XSAVE per thread (832 bytes, XCR0 0x7, ...)` | 38/0, 34/0, 8/0 | 71 / 0 / 6 |
| AVX model + `NOAVX` | `...; AVX AVX2 FMA3 present, off (NOAVX)` | 38/0, 34/0, 8/0 | 70 / 0 / 7 (`avxprobe` SKIP) |
| bare model | `...; no XSAVE` | 38/0, 34/0, 8/0 | 70 / 0 / 7 (`avxprobe` SKIP) |
| AVX model, sparse 24 CPUs | `avx` as above | 38/0, 34/0, 8/0 | 71 / 0 / 6 |

The six SKIPs common to all four are the baseline's six. `fpfault ud` dies
with 206 in all four (AVX-512's `vpxord` in `avx` mode, `vzeroupper` in
`sse`).

**The fixtures catch the leak.** A hand mutant kept `avx` mode but saved and
restored only the FXSAVE image (the probe's state): testrun 69 / 2 / 6, with
`fputest` failing `0xF0DE000F` (an upper half came back wrong) and
`avxprobe`'s child on CPU 1 born holding its parent's fill pattern in
YMM15. The host suite's mutants: 32 of 32 caught.

## The demo (slice B)

**`gmandel`**, in `/bin` with the demos that exist to be enjoyed: two
windows zooming into the same point of the Mandelbrot set, one computing
four pixels at a time with SSE2 and one eight at a time with AVX2, each
showing its frame rate. The AVX window asks the CPU first and says so if
the answer is no. It doubles as a standing proof: both run while the
desktop switches threads around them, and a lost upper half shows as
garbage in the picture.

**As built** (`userland/apps/gmandel/gmandel.c`): one program, two threads,
a window each, reading one tour clock so both show the same view at the
same moment. The kernels are GCC vector extensions (16- and 32-byte float
vectors; the AVX one in a `target("avx2,fma")` function with the complex
multiply as an FMA), because the freestanding compiler's `immintrin.h`
needs a C library. Single precision is what makes the lanes four and
eight, and a float runs out of digits about 2^14 into the zoom, so the
tour turns at 2^13. Built `-O2` (`OPTIMIZED_APPS`): at `-O0` both kernels
spill every vector to the stack and the race measures the spills. The AVX
window's status line gives its speed against the SSE2 one. In QEMU TCG
the AVX window is the SLOWER one (TCG emulates 256-bit operations
expensively), so the real race is the P5's; QEMU proves the pictures, the
keys, the bare model's refusal, and that paused, the two engines draw the
same view.

## For Chris

1. **Plain `xsave64` first, `xsaveopt64` only if the P5's number asks.**
   My lean: yes, for the two reasons in § The save instruction.
2. **A `NOAVX` token.** My lean: yes; it costs a table row and keeps the
   old path honest.
3. **The makefile's default CPU model gains AVX.** My lean: yes. The probe
   saw `testrun` take 5 to 7 % longer under it, from one sample each.
4. **One PR for slice A.** My lean: yes; I do not see a safe way to cut it.
