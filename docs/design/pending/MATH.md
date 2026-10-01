# Maths library port for os64

Status: recommended implementation and handoff, updated 2026-10-01. Chris approved
a reusable maths library as the numerical foundation for the standalone
JavaScript runtime. Use musl 1.2.5 binary64 maths, with generic C routines and
an SSE2 `sqrt` adapter. Opus is the proposed port owner; assignment and
implementation have not occurred through this document.

The working library name is `libmath.so`. The first deliverable covers the
32 functions required by the audited QuickJS configuration, with their complete
dependency set. It is not a claim to implement the entire C maths API. The
runtime and runner work is described in [JAVASCRIPT.md](JAVASCRIPT.md), with
ownership boundaries in [JAVASCRIPT_TASKS.md](JAVASCRIPT_TASKS.md).

Accepted changes from [MATH_REVIEW.md](MATH_REVIEW.md) are incorporated here.
Quinn's response there explains why focused floating-point state checks remain
and qualifies the proposed bit-exact host/guest comparison.

## Recommendation and alternatives

Use musl's maths implementation, vendored with a pinned source manifest,
preserved notices, private compatibility headers, and reviewable patches.
This is an import of numerical routines, not a port of musl libc or its Linux
system-call layer.

The choice is supported by a concrete dependency audit: 32 required functions
and 18 supporting C files compile with `x86_64-elf-gcc`. Combining those objects
with `x86_64-elf-ld -r` leaves no undefined symbols. The QuickJS research probe
also passes on Linux when linked against these cross-compiled maths objects
without `-lm`. This supports the portability recommendation; it does not
establish numerical conformance or execution on os64.

The repository already has a limited musl numerical import under
`userland/libpage/upstream/musl`, described in that directory's parent README.
It supplies private decimal-conversion support, not this public maths surface.
Do not refactor or replace those existing routines as part of the initial port.

OpenLibm 0.8.8 was also inspected. It is a credible standalone alternative with
its own public maths and floating-point environment headers. Its stock full
cross build stopped at a missing `assert.h` in Bessel-function sources. Those
functions are outside QuickJS's current requirements, so this is not evidence
that OpenLibm is unsuitable or that a selected subset cannot compile. musl is
preferred here because the required set has a demonstrated dependency closure
and its code fits the current baseline. This is a portability choice,
not a measured performance or accuracy comparison.

## Source pin and notices

Audited source:
[musl 1.2.5 release archive](https://musl.libc.org/releases/musl-1.2.5.tar.gz).
Downloaded archive SHA-256:

```text
a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
```

This identifies the audited bytes, not an independently verified signature or
a claim that this is the newest upstream revision. Review subsequent relevant
maths fixes before release. Any chosen source update must revise the manifest
and rerun the dependency and numerical checks.

Retain musl's root `COPYRIGHT` and individual source notices. The root notice
describes musl's MIT licence and the permissive licences of contributed maths
code, including Sun, BSD-derived, and Arm work. Preserve the actual notices
instead of replacing them with a blanket project licence. Package the port's
licence notice using os64's existing image conventions.

## Consumer interface

The following is the proposed engineering contract for the maths and runtime
owners to confirm before their implementation branches depend on it:

- Public header: `math.h` under `userland/libmath/include`, used as `<math.h>`.
- Shared object: `libmath.so`.
- Public function names: the standard C names listed below. These also satisfy
  compiler-emitted maths calls without a mapping in each consumer.
- Unary functions take one `double` and return `double`, except `lrint`.
- `atan2`, `fmax`, `fmin`, `fmod`, `hypot`, and `pow` take two `double`
  arguments and return `double`. Preserve the conventional argument order;
  in particular, `atan2` takes y followed by x.
- `lrint(double)` returns `long`, which is 64 bits on os64's LP64 ABI. Verify
  the conversion and its exceptional behaviour rather than casting a guessed
  result to a different public type.

Required names:

```text
acos acosh asin asinh atan atan2 atanh cbrt ceil cos cosh exp expm1
fabs floor fmax fmin fmod hypot log log10 log1p log2 lrint pow round
sin sinh sqrt tan tanh trunc
```

The maths port owns these declarations, numerical implementations, documented
semantics, and the constants/classification macros required by consumers.
QuickJS includes this header without a 32-name rename map. Keep upstream
helper symbols hidden; do not install the rest of musl's declarations as an
os64 API. Private upstream headers remain separate from the public include path.

Use C maths semantics. In particular, C `round` and JavaScript `Math.round`
have different tie rules; QuickJS owns that JavaScript distinction. Preserve
signed zero and the specified NaN/infinity cases. Do not add a process-global
`errno` mechanism to satisfy this API. The audited maths set has no unresolved
errno dependency; error values and floating-point effects must be documented.

The baseline execution environment uses masked floating-point exceptions,
round-to-nearest, and gradual underflow. os64 initializes and saves x87/SSE
state per thread. `lrint` is sensitive to the rounding mode. The port must
preserve the calling thread's control settings, and its tests must cover the
promised rounding behaviour. Publishing a general fenv API is a separate
decision; do not claim one from the presence of private upstream headers.

Validation retains focused checks for control-state preservation, `lrint`
rounding, and selected exception effects. Native consumers and test fixtures
can access MXCSR and x87 state without a public fenv API; os64's existing
`userland/tests/fputest/` does so. This does not promise complete numerical
conformance in every rounding mode. Record the supported environment and
build assumptions, and restore any state changed by a fixture.

The initial floating-point-state test set is limited to these named cases:

| Case | Required observation |
| --- | --- |
| `lrint` directed rounding | For inputs `1.75` and `-1.75`, nearest returns `2, -2`; downward `1, -2`; upward `2, -1`; toward-zero `1, -1`. |
| `lrint` ties to even | In nearest mode, `2.5` and `-2.5` return `2` and `-2`. |
| `lrint` inexact | With cleared status flags in nearest mode, `lrint(1.75)` raises inexact; `lrint(2.0)` does not. |
| `sqrt` invalid | With masked exceptions and cleared status flags, `sqrt(-1.0)` returns NaN and raises invalid. |
| `log` pole | With masked exceptions and cleared status flags, `log(0.0)` returns negative infinity and raises divide-by-zero. |
| Exact `sqrt` control | With cleared status flags in nearest mode, `sqrt(4.0)` returns `2.0` without raising exception flags. |

Check that these calls preserve MXCSR and x87 control settings, distinguishing
control bits from sticky exception status. Fixtures save and restore both
environments, set the relevant rounding modes explicitly, and call the actual
library exports with compiler folding/builtin substitution prevented. These
cases bound the initial state checks; the numerical corpus below still covers
the wider input domain. Expand this list only for a documented contract or
regression need.

## Source inventory

The initial audit used `src/math/<name>.c` for each of the 32 required names
plus 18 support files. The maintained port replaces generic `sqrt.c` with the
adapter below and omits `sqrt_data.c`. Its starting inventory is therefore
31 public-function C files, these 17 support files, and the adapter. Recheck
dependency closure and exports on the maintained build.

```text
src/math/__cos.c
src/math/__expo2.c
src/math/__math_divzero.c
src/math/__math_invalid.c
src/math/__math_oflow.c
src/math/__math_uflow.c
src/math/__math_xflow.c
src/math/__rem_pio2.c
src/math/__rem_pio2_large.c
src/math/__sin.c
src/math/__tan.c
src/math/exp_data.c
src/math/log2_data.c
src/math/log_data.c
src/math/pow_data.c
src/math/rint.c
src/math/scalbn.c
```

Retain the associated data headers, `src/internal/libm.h`, required private
header support, and their source provenance. Header dependency generation in
the maintained build should establish the precise retained header inventory.
The audit used musl's headers and a generated `bits/alltypes.h`, with include
paths for `arch/x86_64`, `arch/generic`, generated headers, `src/include`,
`src/internal`, and `include`. Those are research inputs, not permission to
install musl's libc headers into os64's public include path.

Implement the exported `sqrt` in a small port-owned adapter returning
`__builtin_sqrt(x)`, compiled with `-fno-math-errno`. SSE2 is the target
baseline. This expresses musl's x86-64 `sqrtsd` approach without its AT&T inline
assembly conflicting with os64's `-masm=intel`. Preserve the source lineage
and explain the adapter in `UPSTREAM_REVIEW.md`. Check disassembly for
`sqrtsd` and no recursive call to `sqrt`, plus numerical and exception cases.
Calls from the other routines resolve to this export; the build must not
retain the generic implementation or its unused table.

The 2026-10-01 follow-up cross compile produced `sqrtsd` followed by `ret`
with the material flags below plus `-fno-math-errno -Wall -Wextra -Werror`.
That checks code generation only; the 50-file dependency and QuickJS probes
above used generic `sqrt`, so their results do not validate the revised port.
Keep generic `lrint` initially; any instruction substitution gets separate
review and tests.

## Build and portability requirements

Build as freestanding PIC code for os64's System V x86-64 ABI. Use the existing
library linker conventions and a narrow export list. No allocation, I/O,
threading, dynamic loading, or system calls were required by the audited set.
Verify that the maintained library preserves that boundary.

Material flags used for the successful source-dependency probe were:

```text
-O2 -ffreestanding -fPIC -fno-builtin -mno-red-zone -msse2
-mno-avx -mno-fma -fno-stack-protector -fno-fast-math
-ffp-contract=off -fexcess-precision=standard -masm=intel
```

These are probe flags, not the final build recipe. Use the repository's other
applicable flags and strict warnings, with the reviewed upstream exceptions
below and `-fno-math-errno` for the `sqrt` adapter. Avoid `-march=native`,
AVX/FMA assumptions, fast-math, and compiler rewrites that discard signed zero or floating-point
effects. Preserve the numerical evaluation order required by upstream.

The unchanged sources and headers do not pass the repository's strict warning
policy: a separate `-Wall -Wextra -Werror` audit passed 14 of the 50 files and
failed 36. The distinct diagnostics were:

- Parentheses in private endian helpers and expressions in `acos`, `asin`,
  and `atan2`.
- Set-but-unused volatile temporaries in `libm.h` evaluation helpers.
- A potentially uninitialized `fq` value reported in `__rem_pio2_large.c`.

Keep upstream files unmodified for understood warning-only issues. Use
`-Wno-parentheses` and `-Wno-unused-but-set-variable` on affected upstream
compilations, following the scoped exceptions in the FreeType and JPEG ports.
Volatile evaluation helpers preserve floating-point effects; do not remove
their operations to silence a warning. Inspect the range-reduction control
flow before enabling `-Wno-maybe-uninitialized` on that upstream file, and
record the conclusion in `UPSTREAM_REVIEW.md`. Do not add an unexplained
initialization. If the warning exposes a real defect, retain a reasoned patch
and report it upstream. os64-owned adapters, public headers, and tests keep
the full strict warning policy; do not disable warnings for the library as a
whole. Account for warnings originating in included private headers when
choosing each affected compilation's flags.

The Linux probe link emitted a missing GNU-stack-note warning for the bare-metal
objects. The maintained shared library uses os64's existing no-executable-stack
linker policy. A host harness must also declare its intended stack policy.

## Validation and completion criteria

1. Retained upstream sources have a manifest, checksums, notices, and explicit
   patches or adapters. The source set and public exports are reviewable.
2. The production library passes the cross build with strict warnings and
   documented upstream-only exceptions. Undefined-symbol, export, disassembly,
   and shared-dependency checks demonstrate the intended boundary.
3. Host tests execute the port's implementation rather than resolving maths
   calls accidentally to host libm. Disable compiler builtins where necessary
   and verify symbol binding. Include tests for each public function.
4. Test zero signs, infinities, quiet NaNs, domain and pole cases, subnormals,
   overflow/underflow, rounding ties, integer-conversion boundaries, and large
   arguments that exercise trigonometric range reduction. Check preservation
   of control settings, `lrint` under selected rounding modes, and relevant
   exception flags with small test-only hardware helpers. Restore the caller's
   state. No public fenv API is required for these tests.
5. Use independent reference results with documented precision and tolerances
   for non-exact functions, such as generated high-precision vectors. Agreement
   with one host libm is a comparison, not a proof of correct rounding.
6. Run one vector corpus through a host build of the pinned source and the
   guest shared library. Compare finite results bit-exactly, including zero
   signs, when both builds use the same selected implementations, evaluation
   assumptions, and floating-point settings. Define NaN classification and
   any promised payload/sign handling separately. Investigate mismatches;
   agreement verifies port consistency, not independent numerical accuracy.
   The host reference must use the selected SSE2 `sqrt` path as well.
7. A guest maths test exercises the numerical and failure contracts and
   verifies representative results across yields and concurrent threads.
   Confirm the loaded binary and report actual QEMU evidence. No P5 success
   is implied by these requirements.
8. JavaScript integration links the same maths library and runs its guest
   numeric fixtures. This final consumer check is shared with the runtime
   owner and follows the independently usable maths deliverable.

The port owner owns the maths library and its tests. Common build and image
registration are coordinated as described in JAVASCRIPT_TASKS.md. QuickJS,
the runner, libpage's existing numeric code, Yonder, and kernel changes are
outside this port's implementation scope.

A 2026-10-01 host probe linked the audited musl objects without host libm and
called `lrint(1.75)` after setting MXCSR. It returned 2 under round-to-nearest
and 1 under downward rounding, then restored the saved state. This demonstrates
observable rounding dependence, not guest validation or a complete fenv audit.

## References

- [musl maths sources](https://git.musl-libc.org/cgit/musl/tree/src/math).
- [musl source and notices](https://git.musl-libc.org/cgit/musl/tree/).
- [OpenLibm project](https://openlibm.org/),
  [0.8.8 release](https://github.com/JuliaMath/openlibm/releases/tag/v0.8.8), and
  [licence inventory](https://github.com/JuliaMath/openlibm/blob/v0.8.8/LICENSE.md).
