# F0 contract fixtures

See [FONT_CONTRACTS.md](../../FONT_CONTRACTS.md) for R3 semantics and review status.
This directory contains a fake engine and hand-authored layout acceptance vectors,
not a font parser or the F2 layout implementation. It is not linked into os64.

Run from the repository root:

```sh
python3 tools/test_font_contracts.py --output /tmp/os64-font-contracts-r3
```

Requires Python 3, a host C11 compiler with ASan/UBSan, and `x86_64-elf-gcc`.
`CC` and `CROSS_CC` override those compilers. The runner prints the host compile
command, exercises the fixture, exports metrics to JSON, checks the golden
vectors and strictly cross-compiles the backend/headers. Artifacts remain in
the supplied output directory; omit it to use a temporary directory.

If the environment runs programs under ptrace, LeakSanitizer cannot operate.
For that environment use `ASAN_OPTIONS=detect_leaks=0` with the same command;
ASan and UBSan stay enabled. Allocation callbacks independently count live bytes
and assert return to zero, including the injected-failure sweep. Report LSan as
unrun, not passing.

`fake_backend.c` implements backend table v1 with single-byte test fonts: P for
proportional, M for mono, L for proportional with W missing. Metrics intentionally
do not scale with requested size; tests exercise geometry/ownership, not an
outline scaler. Masks use deterministic varying coverage, not letter shapes.
There are no third-party font assets or Unicode data tables in this fixture.

`contract_test.c` exercises independent engines, retained masks after face close,
empty glyphs, negative bearings, zero advances, lookup/kerning errors, allocation
refusal at each allocation site, memory/face caps and balanced cleanup. It can
export the fake backend's metrics using `--dump`.

`fixtures.json` holds manually specified source spans, resolved glyph choices,
logical/ink positions, carets, hits, fits and selections. `check_fixtures.py`
checks their arithmetic against the C backend's actual metrics, including
kerning and fallback. It trusts the specified cluster decisions; it does not
implement or establish correct UTF-8 decoding/cluster segmentation. F2 must run
the actual implementation against these inputs/expected outputs and extend them
with generated-profile, overflow, clipping and failure tests.

R2 adds a -29/64-pixel e/W pair and two vectors that retain and accumulate its
fractional adjustment. The numbers are synthetic contract inputs; F1's real
font expectations are derived by its independent reader. No .notdef request is
needed for the synthetic missing-marker vectors; render(0) remains an error.

Recorded local result, 2026-09-16: host fixture tests passed at `-O2` with
ASan/UBSan and LSan disabled because of ptrace; 14 acceptance vectors passed
arithmetic checks; strict freestanding target compilation passed. No FreeType,
production layout, UI migration, QEMU or hardware execution is claimed.
Host compiler: Ubuntu GCC 13.3.0; cross compiler: x86_64-elf GCC 14.2.0.
The successful command used `ASAN_OPTIONS=detect_leaks=0` and output directory
`/tmp/os64-font-contracts-r2`. Each candidate header was compiled independently.
The R2 backend header's preprocessed declarations match F1's R1 copy exactly
under the freestanding target preprocessor. A deliberate mutation that rounds
the -29/64 pair to zero fails the new subpixel vector, confirming that vector
detects the precision loss it is intended to expose.

R3 fixture schema revision 2 stores each cluster as
`[byte_start, byte_end, [[face, glyph], ...]]`. Placements in a multi-glyph
cluster share that complete source span; the cluster contributes one end caret.
The q/acute and stacked-accent cases retain the base plus one marker; the
digit/acute case has a separate mark cluster. A primary-row clipping vector
checks row selection/ink arithmetic separately from run metrics.

R3 validation: 17 vectors, backend allocation/cap recovery, and independent
strict target header/fake-backend compilation pass. ASan/UBSan enabled, LSan
disabled as above; `/tmp/os64-font-contracts-r3`. The historical R2 result above
describes its original 14 vectors. These are still arithmetic checks, not F2
implementation or guest evidence.
