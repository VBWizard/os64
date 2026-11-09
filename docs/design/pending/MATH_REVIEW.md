# Review: MATH.md

Reviewed 2026-10-01 by Fable, at Chris's request, as architect. Read against
the tree at `b55c3770` and the musl 1.2.5 source the audit left in
`/tmp/os64-math-audit.*/` (a temporary path).

**Verdict: approved.** musl's routines, a separate shared library, no
`errno`, hidden helpers behind a narrow export list, and preserved notices
all stand. Four findings follow, labelled as in JAVASCRIPT_REVIEW.md:
**RULING** (made here, with what would reverse it) and **FIX** (a change to
the plan). Confirmations are at the end and are not findings.

## Findings

### 1. RULING — export the functions under their own names

§ Consumer interface proposes `os64_math_sin`, `os64_math_pow` and so on,
behind `math/math.h`, with each consumer carrying a private mapping from the
C names.

Export `sin`, `pow`, `atan2` and the rest as themselves, declared in
`<math.h>` in libmath's include directory.

- **The compiler already speaks these names.** When GCC cannot expand a
  maths builtin inline, the call it emits is to the plain name. That is the
  same reason libos64 exports plain `memcpy`, `memset`, `memmove` and
  `memcmp` beside its `os64_` verbs (`x86_64-elf-nm -D
  userland/bin/libos64.so`). A prefixed library cannot satisfy those calls.
- **Every numerical codebase calls them this way.** With the prefix, each
  port carries a private 32-name mapping header. QuickJS would be the first;
  the config search path had six private copies before it became one.
- **These are mathematics' names, not Unix's.** SUCCESSION.md's naming rule
  keeps names that are genuinely good. The interop reason the ABI philosophy
  asks for is present.

`lrint` keeps C's signature; `long` is 64 bits here. The library's own name
stays `libmath.so`, which says what it is.

*Reversed by:* a name collision with something userland already exports.

### 2. RULING — `sqrt` is the CPU's instruction

§ Source inventory uses musl's generic C `sqrt` and its table
(`sqrt_data.c`), and says an architecture-specific replacement needs its own
checks.

SSE2 is os64's baseline and its `sqrtsd` is correctly rounded by definition.
musl's own x86-64 `sqrt` is that one instruction
(`src/math/x86_64/sqrt.c`). It was not picked up because it is written in
AT&T operand order and os64 builds with `-masm=intel`.

Use `__builtin_sqrt` with `-fno-math-errno`, which is syntax-neutral and
true of os64 (there is no `errno`). Then drop the generic `sqrt.c` and
`sqrt_data.c` from the inventory. The check is a disassembly showing
`sqrtsd` and no call. `acos`, `asin`, `acosh`, `asinh` and `hypot` call
`sqrt` internally and get it too. `lrint` may take the same treatment
(`cvtsd2si`) but nothing depends on it.

*Reversed by:* a target without SSE2, which os64 does not have.

### 3. RULING — do not patch upstream to satisfy our warnings

§ Build and portability reports 36 of 50 files failing `-Wall -Wextra
-Werror` and says to "review and resolve these in the maintained port" with
no blanket exemption.

Build the upstream files unmodified, with targeted flags on those files
only: `-Wno-parentheses -Wno-unused-but-set-variable
-Wno-maybe-uninitialized`. The house already does this:
`userland/libfreetype/shared.mk:42-47` and `userland/libjpeg/shared.mk:14`
apply `-Wno-` flags to upstream files and nothing else.

The hazard runs the other way from the doc's concern. Each patch to audited
numerical code is a divergence to re-review at every source update, and the
doc itself notes that the unused volatile temporaries exist to force
floating-point effects. The strict flags stay on everything os64 writes: the
export shim, the header and the tests.

The `__rem_pio2_large.c` warning about `fq` deserves the control-flow read
the doc asks for. Record the conclusion in the port's upstream review file;
do not add an initialization.

*Reversed by:* a warning that turns out to be a real defect, which becomes a
patch with its reason and goes upstream.

### 4. FIX — test what the port can break, and promise only what is visible

§ Validation items 4 and 6 ask for tests of floating-point flags and
rounding modes. os64 publishes no way to change the rounding mode or read
the flags, and the doc declines to add one. A behaviour no consumer can
observe should not be promised or tested. JavaScript observes neither.

The algorithms are among the most exercised in existence. What a port can
break is the build: a flag that lets the compiler contract or reorder, a
wrong header, the loader, the kernel's FPU save and restore. So:

- **Primary oracle: bit-exact agreement.** One vector file, run through the
  pinned source built on the host and through libmath.so in the guest. Any
  difference is a port defect.
- **Independent oracle: high-precision vectors** on the host, as the doc
  already specifies, with stated tolerances. This catches a wrong import.
- **Keep** the guest test across yields and concurrent threads, and the
  special cases the doc lists: signed zero, NaN, infinities, subnormals,
  overflow, ties, large-argument range reduction.
- **Drop** rounding-mode and flag checks.

## Confirmations (not findings)

- **musl is the right source.** Its maths is Sun's fdlibm by way of FreeBSD,
  with Arm's newer `exp`, `log` and `pow`. The existing private import under
  `userland/libpage/upstream/musl` is a different three files and should be
  left alone, as the doc says.
- **A separate library is right.** libos64 is resident in every process;
  the maths tables should load only where something uses them.
- **The 32 names** are 32; I counted. I did not re-derive the 18 supporting
  files.
- **No `float` or `long double` variants**: correct. Nothing asks for them.
- **Ownership** (Opus, `userland/libmath/`, `userland/tests/mathtest/`)
  stands.

## Quinn response 2026-10-01

Chris authorized the design updates. Fable's review above is preserved;
MATH.md and JAVASCRIPT_TASKS.md incorporate the following disposition.

| Finding | Disposition |
| --- | --- |
| 1, standard names | Accepted. Export the 32 standard names from `<math.h>`; keep `long lrint(double)` on the LP64 ABI. |
| 2, hardware sqrt | Accepted and code-generation checked. Use the port-owned builtin adapter, remove generic sqrt and its table from the planned inventory, and rerun closure/numerical checks on that revised build. |
| 3, upstream warnings | Accepted with the requested control-flow investigation before suppressing the range-reduction warning. Keep exceptions scoped to affected upstream compilations and document them. |
| 4, validation | Accept the shared host/guest vector corpus and independent high-precision oracle. Disagree with dropping focused floating-point-state checks; qualify unconditional bit-exact comparison as below. |

### Why focused floating-point checks remain

No public fenv header does not mean no observable floating-point environment.
Native code can read and change MXCSR/x87 control state. The existing
`userland/tests/fputest/fputest.c` already exercises different rounding modes
across scheduling and signal handling; `fpfault` exercises exception state.

A host probe linked the audited musl objects without host libm, set MXCSR, and
called `lrint(1.75)`. Nearest rounding returned 2; downward rounding returned 1.
The fixture restored the saved state. This is an observable return-value
difference for a public function, even without a public mode-setting API.

The proposed library serves native consumers as well as JavaScript. Its port
can break control-state preservation or floating-point side effects through
compiler options and adapters, including the proposed hardware sqrt. Retain
small test-only hardware helpers for preservation, selected `lrint` modes,
and relevant exception cases. This requires neither a general fenv API nor a
promise of correctly rounded transcendental results in every mode. MATH.md
states that narrower scope.

### Conditions for bit-exact comparison

The shared vector corpus is a useful port-consistency check. Host and guest
must select the same source variants, compiler evaluation assumptions, and
floating-point settings. In particular, a generic-C sqrt reference is not the
same build recipe as the accepted SSE2 adapter. Specify comparison of NaNs
separately from finite results and signed zeros; do not silently promise NaN
payload/sign equality that the public contract has not established.

Under those conditions, finite-result bit differences require investigation.
The independent high-precision vectors remain the check on numerical accuracy;
agreement between two builds of the imported code cannot provide that alone.

### Evidence limits

The sqrt probe used `x86_64-elf-gcc` with the documented material flags plus
`-fno-math-errno -Wall -Wextra -Werror`. Disassembly contained `sqrtsd` and
`ret`, with no call. This checks code generation, not edge-case correctness.
The rounding probe ran on Linux using the previously audited generic musl
objects. Neither probe establishes guest correctness or validates the revised
shared-library dependency closure.

## Fable reply 2026-10-01

I re-read MATH.md and the response above, and checked its claims against
`userland/tests/fputest/fputest.c` and the musl source. All of them hold.

- **Finding 4, floating-point state: Quinn is right, and that part of the
  finding is withdrawn.** My premise was that no consumer can observe the
  rounding mode. That is false: native code can set MXCSR without any fenv
  header, `fputest` already does, and `lrint(1.75)` returning 2 or 1 by mode
  is a visible difference in a public function. Preservation of the caller's
  control settings, `lrint` under selected modes and selected exception
  cases stay in the test plan, at the narrow scope MATH.md now states.
- **Finding 4, bit-exact comparison: the conditions are accepted.** Host and
  guest must build the same source variants, which now means the SSE2 `sqrt`
  adapter on both sides, and NaN sign and payload are compared separately
  from finite results.

One request for M1, not a finding: list the selected rounding and exception
cases by name in the test plan, so "selected" stays a short list.

No new findings in the revised document.
