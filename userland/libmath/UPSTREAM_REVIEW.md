# Upstream pin review — musl maths

Reviewed 2026-10-01 for the M1 port (MATH.md). This file records what was
checked about the imported bytes, the one upstream fix taken onto the pin,
every compiler exception and why it is safe, and how accurate the result is
against an independent oracle. The port's design and contract are MATH.md's;
this is the evidence behind it.

## The pin

musl **1.2.5**, from the release archive:

```text
https://musl.libc.org/releases/musl-1.2.5.tar.gz
sha256 a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
```

The digest was recomputed on download and matches the one MATH.md recorded
at audit time. Every imported file's digest is in `manifest.json`.

**musl 1.2.6 is now released, and the pin stays at 1.2.5.** Across the 70
imported files, 1.2.6 changes only two: a contributor's name in
`COPYRIGHT`, and `arch/x86_64/bits/stdint.h`, which folds into a generic
copy that defines the same limits on LP64. No maths routine changed, and in
particular 1.2.6 does **not** contain the `acosh` fix below. Moving the pin
would buy nothing and cost a re-review.

### Reproducing the import

```sh
tools/import_math.py --check musl-1.2.5.tar.gz   # upstream/ == archive + patches/
tools/import_math.py --write musl-1.2.5.tar.gz   # rebuild upstream/ and the manifest
```

The script takes its file list from `manifest.json` and never widens it:
what the port imports is a reviewed decision, not a directory walk. Patches
are applied in sorted order with `patch -p1` from inside `upstream/`.
`tools/test_math_audit.py` checks the tree against the manifest on every
run without needing the archive.

## What is imported

Paths under `upstream/` are musl's own.

- **31 public routines**, `src/math/<name>.c`, one per exported name except
  `sqrt`.
- **17 support files**: the trigonometric kernels and range reduction
  (`__sin`, `__cos`, `__tan`, `__rem_pio2`, `__rem_pio2_large`), the
  error-result builders (`__math_divzero`, `__math_invalid`, `__math_oflow`,
  `__math_uflow`, `__math_xflow`), `__expo2`, `rint`, `scalbn`, and the
  tables behind exp, log, log2 and pow with their headers.
- **The private header closure**, found by `-MM` over those sources with
  musl's own include order and nothing else visible (`-nostdinc`):
  `src/internal/libm.h`, `src/include/features.h`, `arch/generic/fp_arch.h`,
  and `include/` + `arch/x86_64/bits/` copies of `math.h`, `float.h`,
  `stdint.h`, `limits.h`, `endian.h`, `fenv.h` and `features.h`.
  `bits/alltypes.h` is generated at build time, as musl's build does, from
  the two vendored templates and the vendored `tools/mkalltypes.sed`.
- **`COPYRIGHT`**, whole.

None of these headers is on any consumer's include path. Consumers see
`include/math.h`, which is os64's.

**Not imported, on purpose:**

- `src/math/sqrt.c` and `sqrt_data.c`, musl's generic software square root.
  `port/sqrt.c` replaces them (below).
- musl's **x86-64 overrides**. musl's own build prefers
  `src/math/x86_64/{fabs,lrint,sqrt}.c` over the generic files. All three
  are one-instruction inline asm in AT&T operand order, which `-masm=intel`
  would misread. `sqrt` is replaced by the builtin adapter. `fabs` and
  `lrint` use the generic C, as MATH.md asks for `lrint`. The generic `fabs`
  compiles to a sign-bit clear (`btr`). The generic `lrint` compiles to
  `rint` followed by `cvttsd2si`. Both give the same results as the asm.

## Upstream fixes since the tag

Every commit touching `src/math` between the 1.2.5 release (2024-02-29) and
review was checked against the files this port builds. One lands in them:

| Patch | Upstream | What it fixes |
|---|---|---|
| 0001 | `3e80328d45` | `acosh` returned a **finite number instead of a NaN** for some x ≤ −2 (reported by Paul Zimmermann: `acosh(-0x1.8p15)` gave −3.7534…). The \|x\| ≥ 2 paths never checked the sign. |

The patch file is upstream's own `.patch`, byte for byte, with the commit
SHA in its header. It applies cleanly to 1.2.5.

**The port's own tests missed this until they were widened.** The corpus
drew `acosh` arguments from its domain only, and the negative specials
(−2, −2.5, …) happen to land on NaN. The corpus now spends half its random
`acosh` draws on [−2²⁶, −2] and carries the reported input. Without the
patch the MPFR oracle fails 26 vectors; with it, none fail.

The others touch only routines this port does not build: `fma` (a
negative-zero fix), `fmaf` (a subnormal fix, then a rewrite), `expl` on
x86, `powl` (a fix and a comment), `acoshf` and `logbl`.

## Compiler exceptions

`shared.mk` builds upstream files under userland's full
`-Wall -Wextra -Werror`, with three diagnosed exceptions scoped to upstream
compilations. os64's own files (`port/sqrt.c`, the header, the tests) keep
every warning.

**`-Wno-parentheses`**, on every upstream compilation. musl writes
precedence without redundant parentheses (`__x>>8&0xff00`) in
`include/endian.h`, which `libm.h` includes, and in expressions in `acos`,
`asin` and `atan2`. These are style warnings about code that is correct.

**`-Wno-unused-but-set-variable`**, on every upstream compilation. In
`libm.h`, `fp_force_eval` stores a value into a volatile local and never
reads it back. That store is the whole point: it forces the CPU to perform
the operation and raise its floating-point flags, which the optimizer would
otherwise discard. Removing the variable to silence the warning would
silently remove inexact/underflow signalling from every routine that uses
`FORCE_EVAL`.

**`-Wno-maybe-uninitialized`, on `__rem_pio2_large.c` alone.** GCC reports
that `fq` may be read uninitialized at `fw = fq[0]-fw` (line 417). The
control-flow argument that it cannot be:

1. The only caller here is `__rem_pio2`, with `prec = 1`, so
   `jk = init_jk[1] = 3`.
2. `fq[jz-i]` is written for every `i` from `jz` down to 0, which covers
   `fq[0..jz]` whenever `jz >= 0`. Every later read indexes within
   `fq[0..jz]`.
3. `jz` starts at `jk` and moves only in the "chop off zero terms" block,
   reached when `z == 0`. The recomputation check just before it has
   already established that some `iq[i]` with `jk <= i <= jz-1` is
   non-zero, or it would have recomputed instead. So the descent
   `while (iq[jz] == 0) jz--` stops at an index `>= jk = 3`.

So `jz >= 3` whenever `fq` is read, and `fq[0]` was written. GCC cannot
follow step 3 because it depends on the contents of `iq[]`. This is not a
defect, so it gets a flag, not a patch, and nothing is initialized.

## `sqrt`

`port/sqrt.c` returns `__builtin_sqrt(x)`, compiled with `-fno-math-errno`.
That is musl's own x86-64 `sqrt` (`src/math/x86_64/sqrt.c`, a single
`sqrtsd`), said without asm syntax. Without `-fno-math-errno`, GCC keeps a
call to `sqrt` for negative arguments so it can set `errno`, and here that
call would be to itself. The audit checks that the export disassembles to
exactly `sqrtsd; ret`, and that `__sqrt_data` is absent. The five routines
that take square roots (`acos`, `asin`, `acosh`, `asinh`, `hypot`) call this
export directly, because `-Bsymbolic-functions` binds intra-library calls at
link time.

`sqrtsd` is correctly rounded in every rounding mode. The oracle measures
exactly 0.5000 ULP worst case, and the state checks cover `sqrt(-1)`
raising invalid and `sqrt(4)` raising nothing.

## No `errno`

musl reports maths errors through the return value and the IEEE flags
(`math_errhandling == MATH_ERREXCEPT`). It never writes `errno`, and the
leaf link proves it: `libmath.so` has no undefined symbols at all. Error
results come from the `__math_*` builders, which compute them with real
floating-point operations so the CPU raises the flag. Checked on the host
for the claims `<math.h>` makes: `acos(2)` and `log(-1)` raise invalid,
`atanh(1)` raises divide-by-zero, `exp(1000)` raises overflow and inexact,
`exp(-1000)` underflow and inexact, and `lrint` of `1e300` or a NaN returns
`LONG_MIN` and raises invalid.

## Accuracy

`tools/test_math_host.sh` runs 11,696 vectors through MPFR at 256 bits. The
bound for each function is upstream's own stated worst case where its
source states one. Otherwise it is fdlibm's faithful-rounding standard of
1 ULP.

| Function | Max measured (ULP) | Bound | Source of the bound |
|---|---|---|---|
| sin, cos, tan | 0.518, 0.641, 0.586 | 1 | fdlibm, faithful |
| asin, acos, atan | 0.654, 0.668, 0.586 | 1 | fdlibm, faithful |
| atan2 | 0.927 | 1 | fdlibm, faithful |
| sinh | **1.056** | 2 | none stated; built from `expm1` exactly as `tanh` is |
| cosh | 0.877 | 2 | as `sinh` |
| tanh | 1.277 | 2 | `tanh.c`: "up to 2ulp error in [0.1,0.2554]" |
| asinh | 1.250 | 1.6 | `asinh.c`: "up to 1.6ulp error in [0.125,0.5]" |
| acosh | 0.971 | 2 | `acosh.c`: "up to 2ulp error in [1,1.125]" |
| atanh | 0.933 | 1.7 | `atanh.c`: "up to 1.7ulp error" |
| exp | 0.500 | 0.511 | `exp_data.c`: "0.511 without fma" |
| expm1, log1p | 0.651, 0.592 | 1 | fdlibm, "1 ulp" |
| log | 0.500 | 0.54 | `log.c`: 0.5 + 4.13/N with N = 128, i.e. 0.532, plus a polynomial term it does not quantify |
| log10 | 0.544 | 1 | fdlibm, faithful |
| log2 | 0.498 | 0.55 | `log2.c`: "0.550 ULP without fma" |
| pow | 0.500 | 0.54 | `pow.c`: "Worst-case error: 0.54 ULP" |
| cbrt | 0.618 | 0.667 | `cbrt.c`: "error < 0.667 ulps" |
| hypot | 0.479 | 1 | fdlibm, faithful |
| sqrt | 0.500 | 0.5 | correctly rounded (IEEE 754) |
| ceil, floor, trunc, round, lrint, fabs, fmax, fmin, fmod | 0 | 0 | exact by construction |

`sinh` is not faithfully rounded: musl states no bound for it, and it
measures above 1 ULP. It is held to its sibling `tanh`'s stated 2 ULP.

Special values are judged by kind, not distance. A NaN must be a NaN, an
infinity the same infinity, a zero answer must carry the exact answer's
sign, and an overflow to infinity is right only past round-to-nearest's
threshold. The oracle was mutation-tested before it was trusted: a value
1 ULP past faithful, a NaN for a number, a wrong infinity, a lost overflow
and a wrong zero sign each fail it.

The measured maxima come from a corpus of 32 × 256 random draws plus
special values, so they are lower bounds on the true worst case. The
bounds come from upstream's error analysis, not from these measurements.

## Licence

musl's `COPYRIGHT` is imported unmodified, and every source keeps its own
notice. The root notice is musl's MIT licence plus the permissive terms of
the contributed maths code (Sun's fdlibm, FreeBSD, Arm's optimized
routines). It ships whole, under a one-line heading, as
`/etc/licenses/libmath.txt` on both the ext2 root and the FAT lifeboat
(`license/libmath-LICENSE`). The audit checks that the COPYRIGHT bytes are
in that file.

## What was not done

- **No `float` or `long double` routines.** Nothing asks for them
  (MATH_REVIEW.md, confirmations).
- **No public fenv API.** The state checks reach MXCSR with test-only
  helpers (`userland/tests/mathtest/state.h`).
- **`lrint` was not given `cvtsd2si`.** Generic C, as MATH.md asks.
  Substituting the instruction is its own review.
- **No P5 run.** Evidence is QEMU (TCG) plus the host. MATH.md promises
  nothing about the P5.
