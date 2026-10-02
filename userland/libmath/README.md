# libmath — os64's maths library

`/lib/libmath.so` is the C maths library at binary64: `sin`, `pow`, `atan2`
and the rest, by their C names, declared in `<math.h>`. The routines are
musl 1.2.5's — fdlibm by way of FreeBSD, with Arm's optimized `exp`, `log`,
`log2` and `pow` — built unmodified apart from musl's own later fixes
(`patches/`), and `sqrt` is the CPU's `sqrtsd`. MATH.md is the design and
the contract; UPSTREAM_REVIEW.md is the evidence.

**It is a LEAF.** No `DT_NEEDED`, no undefined symbols, no dynamic
relocations, no system calls: arithmetic needs nothing from anyone, and the
link's `--no-undefined` keeps it that way. A program links it by naming
`$(LIBMATH_SO)` among its extra libraries in `userland/GNUmakefile`, as
`mathtest` does.

## What is where

| | |
|---|---|
| `include/math.h` | the public header: the 32 functions, the classification macros, and the floating-point contract |
| `upstream/` | the pinned musl files, at musl's own paths |
| `patches/` | musl's own fixes since the release, applied in order |
| `manifest.json` | the archive pin, every file's digest, every patch's digest |
| `port/sqrt.c` | `sqrt` as the CPU's instruction |
| `exports.map` | the 32 names; everything else stays local |
| `sources.mk`, `shared.mk` | what is built, and the flags that decide the numbers |
| `UPSTREAM_REVIEW.md` | the pin, the fix taken, every warning exception, the accuracy table |

## The contract, briefly

- C semantics, C names: `round` takes ties away from zero, `atan2` takes y
  first, `lrint` returns a 64-bit `long` and rounds in the current mode.
- Errors are the result plus the IEEE flags in MXCSR. There is no `errno`.
- The caller's floating-point control settings are never changed.
- Accuracy is upstream's: within 1 ULP for most functions, within 2 for the
  hyperbolic ones, about half an ULP for the Arm-derived four, and the exact
  functions are exact (UPSTREAM_REVIEW.md § Accuracy has each bound).
- Consumers must not use `-ffast-math`, which assumes NaN and infinity away.

## Checking it

```sh
make -C userland                         # builds libmath.so and /tests/mathtest
tools/test_math_host.sh                  # host vs cross build, MPFR oracle, state checks
python3 tools/test_math_audit.py         # pin, exports, leaf, sqrtsd, notice
tools/import_math.py --check musl-1.2.5.tar.gz   # upstream/ is archive + patches/
```

and in the guest, `mathtest` or `testrun mathtest`: it recomputes the whole
corpus through `libmath.so` and compares each block's digest with the host's
(`userland/tests/mathtest/expected.h`), runs the floating-point state checks,
and repeats both across four threads. When a digest disagrees it prints that
block's vectors; `tools/test_math_host.sh dump <fn> <block>` prints the
host's, line for line.

After any change to the corpus, the flags or the sources, regenerate the
expected digests with `tools/test_math_host.sh --regen`, which refuses to
write them unless the MPFR oracle passed first.
