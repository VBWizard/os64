# FreeType audit and backend — handoff for Claude Opus 5

Frozen F0 contract: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Prepared 2026-09-16. Assignment F1 in the os64 font feature.

## Assignment and starting state

Audit and port a minimal FreeType engine into os64 userland, delivered as
`/lib/libfreetype.so` with an os64-owned interface, focused host tests, and a
real guest specimen. The source audit can start immediately. Production backend
implementation follows agreement on the narrow backend contract described below.
Use the recorded contract baseline in [F0-FREEZE.md](docs/fonts/F0-FREEZE.md).

**F0 update, R3:** the approved contract exists in
[FONT_CONTRACTS.md](FONT_CONTRACTS.md) and
[`font_backend.h`](userland/libos64/include/os64/font_backend.h). Review that
header against the audit rather than inventing a parallel API. It proposes a
callback-driven leaf engine, fixed-size face handles and independently owned
glyph masks. The getter is `os64_freetype_backend_v1`; the header location adds
no link dependency on libos64. The backend design is approved;
the separate implementation review remains required. Include these two
files when transferring this handoff to another checkout.

**F1 reconciliation:** [F0-F1-DECISIONS.md](docs/fonts/F0-F1-DECISIONS.md)
answers the six questions from the implemented F1 candidate. Table revision 1
and signatures are unchanged. F1 exists in its separate worktree; the staged
instructions below describe the original assignment, while the decisions file
and [review brief](docs/fonts/FABLE-REVIEW.md) describe the current review gate.

Chris is the project owner; Quinn coordinates the font feature and F0 contracts.
Return interface proposals and results through Chris or the shared repository;
no direct agent connection or access to the original conversation is required.

| Item | Starting value |
|---|---|
| Repository | <https://github.com/VBWizard/os64.git> |
| Audited os64 base | `3b82356413ab8f183bd6506febe8d06fea6e7de0` (merged PR #109) |
| Local design branch | `codex/font-design` |
| Local design directory | `/home/yogi/src/os64/.worktrees/font-design` |
| Actual implementation branch/worktree | `opus/freetype-backend`, `.worktrees/freetype-backend` |
| Frozen backend contract commit | `23bf6dddfd1077bf844c661d8762a9b52e3a68f9`; [freeze record](docs/fonts/F0-FREEZE.md) |
| Other implementation prerequisites | No F2 layout or F3/F4 consumer implementation required for the standalone backend |
| Delivery requested | Local, reviewable implementation, audit, tests, and completion report; publication/merge is not part of this assignment |

The design files were uncommitted and unpublished when this handoff was written.
Checking out the base or fetching the design branch alone does not provide them.
On this workstation, read them at the directory above. For another workstation,
share this file plus `FONTS.md`, `FONTS_WORK_PLAN.md`, `FONT_CONTRACTS.md`,
`userland/libos64/include/os64/font_backend.h`, and `docs/fonts/` explicitly.
The technical brief below is self-contained; those documents provide the wider
feature context and ownership map. Obtain the frozen contract separately when
it exists. Do not silently substitute a different base or overwrite existing work.

Read repository `AGENTS.md` before editing. Work in an isolated checkout. Record
the actual base and toolchain versions. Keep comments accurate alongside code,
search sibling paths for the same defect, and investigate recent changes first
when a new regression appears. No kernel/loader/syscall changes are in scope.

## Product context and boundaries

The accepted product is installable standard outline TTF/OTF fonts, independent
interface/terminal/document choices, scalable monospace terminal text, and
proportional or monospace Scribe text. The first language scope is UTF-8 Western
text and common symbols, including deliberate handling of accented text; complex
script shaping is later. Scribe must preserve original file bytes. The existing
bitmap font remains available as a fallback.

F1 implements face parsing, Unicode scalar-to-glyph lookup, metrics, raster masks,
and pair kerning. UTF-8 decoding, combining-cluster placement/editing, fallback
selection, positioned runs, glyph caching, line layout, drawing into UI surfaces,
font discovery/configuration and consumers belong to other packages. FreeType
support for a character does not establish correct editing or shaping support.

Do not migrate gterm/Scribe/widgets, change `fonts.conf` or appearance schemas,
add a Unicode PTY, implement window decorations, or introduce HarfBuzz here.
The specimen may use existing GUI/surface APIs to demonstrate masks, without
adding production text policy to libos64. It is a backend test client.

## Repository map and file ownership

Read these current examples; copy their applicable conventions, not dependencies
that conflict with the font design:

| Existing path | Purpose |
|---|---|
| `userland/libtls/UPSTREAM_REVIEW.md` | Source pin/fix review and honest limits of audit claims |
| `userland/libtls/port/runtime.c` | Private freestanding runtime example |
| `userland/libjpeg/shared.mk`, `sources.mk`, `exports.map` | Selected upstream sources, PIC shared build, hidden exports |
| `userland/libjpeg/port/config.h`, `port/decode.c`, `port/compat/` | Private runtime mappings and GCC nonlocal-return precedent |
| `tools/test_jpeg_host.py`, `tools/test_jpeg_host.c` | Production-source host sanitizer harness pattern |
| `userland/tests/jpegtest/jpegtest.c` | Guest fixture and reporting pattern |
| `userland/GNUmakefile`, `userland/link/lib.ld`, `userland/tools/app_bases.py` | Compiler/linker policy, library slots, app/test discovery |
| `GNUmakefile` | Root image/library installation and QEMU launch rules |
| `userland/libos64/include/os64/mem.h`, `str.h`, `slurp.h`, `draw.h` | Allocator/helpers, bounded file loading and specimen drawing |
| `LIBOS64.md`, `LIBDRAW.md` | Runtime and surface context |

Proposed owned additions: `userland/libfreetype/` (upstream, private port,
include, sources.mk, shared.mk, exports.map, source manifest, UPSTREAM_REVIEW.md,
README/test instructions), `tools/test_freetype_host.*`,
`userland/tests/fonttest/`, and named FreeType/font fixture license files.
Confirm final interface paths with F0 before creating shared headers. Use a
fixture directory under your owned tree and document its source/licensing.

Quinn owns integration of shared build/image manifests and common interfaces.
Provide a patch for `userland/GNUmakefile` and root `GNUmakefile`, with the library
slot list, build/include/dependency entries, guest fixture linkage, installation,
and debugger symbol entries as applicable. Apply/test that patch in your isolated
worktree; identify it separately in the report so integration does not race other
packages. Do not edit another implementer's worktree. No production libos64
dependency on FreeType is necessary for the standalone F1 specimen.

## Stage A — source selection and audit

Start with the feasibility candidate [FreeType VER-2-14-3](https://github.com/freetype/freetype/tree/VER-2-14-3).
Recheck the selected release against upstream fixes at the time of your audit;
do not equate a previously compiled tag with a reviewed production pin. Read
`docs/CHANGES`, `docs/CUSTOMIZE`, `docs/INSTALL.ANY`, `README.git`, `modules.cfg`,
and the selected source and configuration headers. Prefer an official release
archive or a reproducible clone/import procedure that includes needed generated
files; the earlier direct-compile probe did not test upstream's full build system.

Record the source URL, full resolved commit, tag/release, acquired archive digest
when applicable, imported-file inventory, build options, local changes, and
relevant fixes considered. Map fixes to enabled parsers/renderers and cite the
upstream evidence. Describe exclusions and residual risks. This is a source and
port review, not a claim of exhaustive vulnerability discovery.

Use the BSD-style FreeType License option, retain upstream notices and the
required distribution credit, and inspect notices for the actual included files.
The authoritative [license page](https://freetype.org/license.html) and the pinned
`LICENSE.TXT` / `docs/FTL.TXT` describe that option. Fixture fonts carry independent
licenses; record their origins, hashes, redistribution rights, and any subset
or modification steps. Do not rely on fonts installed on your development host.

Initial selected drivers/modules: base support, SFNT, TrueType, CFF, smooth
grayscale rendering, autofit, and CFF's required `psaux`, `pshinter`, `psnames`.
Exclude standalone Type 1/CID/PFR/Windows FON/BDF/PCF drivers, upstream cache,
compression wrappers and external compression/shaping/image dependencies.
Disable system file streams, environment-controlled properties, SVG and legacy
Mac container support. Audit embedded bitmap/color/variation/collection behavior
explicitly; a `.ttf` or `.otf` extension is not a guarantee of supported contents.
Monochrome raster output is not a product requirement for F1; the probe included
its module. Resolve whether to retain it at the interface checkpoint.

Reproduce undefined-symbol analysis. Check helper semantics and compiler-emitted
dependencies instead of assuming existing os64 helpers are libc-compatible.
For example `os64_streq` is boolean, not strcmp's three-way comparison. Keep
private helpers private. A general public qsort/setjmp/longjmp API is outside
this assignment unless the coordinator explicitly changes the shared contract.

### Prior feasibility evidence — leads to reproduce, not acceptance evidence

The 2026-09-16 probe cross-compiled 17 upstream translation units at `-O2` using
`x86_64-elf-gcc`, PIC, freestanding, no red zone, Intel assembly syntax, and strict
warnings. It did not run in os64. A Linux host harness linked the cross-built
objects and rendered DejaVu Sans/Sans Mono TTF and Nimbus Sans Regular OTF/CFF
at 16, 24, and 32 pixels. No sanitizer or comprehensive malformed-font result
was established by that probe.

The source archive URL was
`https://api.github.com/repos/freetype/freetype/tarball/VER-2-14-3`, with observed
SHA-256 `1cc149d9dce64e02f92713a777588d0551a8334d63c3d3e73e955269dc57a89a`.
That identifies the acquired bytes, not all future archives generated for this
tag. Resolve the full source revision yourself; do not use an archive-directory
abbreviation as the production provenance record.

Source units, relative to upstream `src/`:

```text
base/ftsystem.c base/ftinit.c base/ftdebug.c base/ftbase.c
base/ftbbox.c base/ftglyph.c base/ftbitmap.c base/ftmm.c
truetype/truetype.c cff/cff.c sfnt/sfnt.c
psaux/psaux.c pshinter/pshinter.c psnames/psnames.c
autofit/autofit.c smooth/smooth.c raster/raster.c
```

This is the probe list, not a mandate to retain unused helper entry points.
Its configuration disabled stream support, environment properties, zlib, legacy
Mac support, SVG, and upstream assembly; it enabled `TT_CONFIG_OPTION_GPOS_KERNING`.
HarfBuzz, PNG and Brotli were not enabled. Its custom `ftstdlib.h` used private
GCC nonlocal-return builtins; an explicit cast was needed for upstream's volatile
jump-buffer use. Review the compiler rules and optimized failure paths before
adopting that mechanism. Do not expose it as a conforming public C setjmp ABI.

The unresolved runtime inventory was:

```text
malloc realloc free
memchr memcmp memcpy memmove memset
qsort strcat strcmp strcpy strlen strncmp strncpy strrchr strstr
```

Combined object sections measured 484431 text/read-only bytes, 14712 data bytes,
and zero BSS, totaling 499143 bytes. This excludes the runtime shim, final ELF,
font buffers and caches. Peak tracked engine allocations for the three single
face trials were 33769 / 35796 / 44164 bytes, excluding font buffers/cache;
each returned to zero. These are sample costs, not resource limits.
At 16 pixels, i/W advances were respectively 4/16, 10/10 and 4/15 pixels.
New fixture versions/configuration may produce different values: establish
expected results for your pinned inputs rather than hardcoding these samples.

## Interface checkpoint — required before the production adapter

Return Stage A's review and any necessary changes to the F0 R3 backend header
to Quinn through Chris. Quinn/F0 owns final shared header and library-direction
decisions. Include
example create/open/lookup/render/release flows and tests that exercise ownership
and errors. Receive or agree an exact contract revision, record it in your README
and report, then implement against it. Fable's feature design review is tracked
by the coordinator. This is an interface dependency, not repeated approval for
routine private implementation choices.

Do not wait on the whole editor/configuration design: request the narrow engine
subset. While it is pending, finish the source audit, source/config import,
isolated cross-compile, helper tests, licensed fixtures and failure reproducers.
Experimental harnesses may use FreeType directly; do not publish a speculative
backend ABI as frozen or make consumers depend on it.

The following is the proposed contract checklist, not invented C signatures:

| Operation/topic | Required contract decision |
|---|---|
| Engine create/destroy | Opaque handle, allocator callbacks/context lifetime, accounting and failure semantics; no global current allocator |
| Face open/close | Memory pointer plus bounded length, face index policy, immutable bytes retained by caller until close; no internal file I/O |
| Font instance | Pixel size limits, render/hinting policy, size-state ownership, lifetime relative to face/engine |
| Metadata and metrics | Copy/borrow rules for names, glyph count, fixed-width metadata, ascender/descender/line metrics and signed units |
| Glyph lookup | Unicode scalar input (not UTF-8), explicit missing-glyph result distinct from invalid input/engine error |
| Advance and ink | Signed fixed-point representation, baseline/axis conventions and overflow handling; preserve fractional advances |
| Raster mask | Grayscale coverage, width/height/stride, signed bearings, empty-glyph success, bitmap ownership and validity duration |
| Pair kerning | Same face/size, units/mode, absent-pair success returning zero, enabled kern/GPOS coverage without shaping claims |
| Errors | Invalid/unsupported font, invalid argument, resource limit, allocation failure, upstream failure; cleared output on failure |
| Resource bounds | File/face/size/glyph dimensions, checked products, engine allocation budget and accounting; named limits with rationale |
| Concurrency | Independent contexts supported; explicitly define serialization/reentrancy of each mutable face/engine |

Preferred dependency graph: callbacks and private helpers keep libfreetype a
leaf; libos64 can later consume it without libfreetype importing libos64 back.
Today draw/UI are compiled into libos64. Copying JPEG's libos64 dependency would
create a cycle when fonts integrate. Confirm the leaf design at this checkpoint.
The standalone test application may link both libraries and supply callbacks.

Choose whether returned glyph pixels are copied/owned or borrowed until the next
operation, and make the release/lifetime rule testable. FreeType's mutable slot
must not masquerade as a retained cache entry. Specify normalization of negative
pitch and pixel formats, allocation rollback, repeated destruction policy, and
the realloc-failure rule that keeps the old allocation valid. Decide explicit
behavior for unsupported collections, variation axes and color-only glyphs.

## Stage B — implement and validate the backend

Keep imported source distinct from port/config code and record patches. Route
allocations through the agreed callbacks (investigate `FT_New_Library` plus
module initialization); do not let default `ftsystem.c` allocation introduce a
hidden libc dependency. Use memory faces and Unicode charmap selection according
to the agreed supported-font policy. Preserve memory-buffer lifetimes. Do not
mistake selecting a charmap for validating an entire font up front.

Implement the agreed operations without application types or FreeType structures
in the consumer header. Restrict exported symbols; record the exact export list.
Retain grayscale output and consistent metrics; leave compositing/cache/fallback
policy to F2. Test cleanup from partially created engine/face/size/glyph objects.
Bound allocations and check integer conversions before calling upstream code.
Allocation bounds alone are not a CPU-time bound; report any remaining work-limit
limitations honestly and use timeouts for malformed-input test runs.

Follow the repository's actual compiler/link conventions: strict warnings,
freestanding PIC, no red zone, `-masm=intel`, explicit SysV hash, the shared
library linker script and assigned library slot. Do not pick an arbitrary load
address or add a loader workaround. Inspect `DT_NEEDED`, undefined/exported
symbols, relocations and LOAD permissions against the agreed graph and current
loader support. Confirm no host libc/stdio/GUI/compression/shaping dependency
slips into the backend. `--no-undefined` should enforce the intended final link.

Add reproducible host and guest commands to the backend README. The following
existing commands establish the build/check conventions; new test targets/scripts
must be supplied by this assignment, not assumed to exist:

```sh
make -C userland
x86_64-elf-readelf -dW userland/bin/libfreetype.so
x86_64-elf-readelf -rW userland/bin/libfreetype.so
x86_64-elf-readelf -lW userland/bin/libfreetype.so
x86_64-elf-nm -D --defined-only userland/bin/libfreetype.so
x86_64-elf-nm -D -u userland/bin/libfreetype.so
x86_64-elf-size userland/bin/libfreetype.so
git diff --check
tools/stale_refs.sh
```

The ELF commands apply after adding/building the new library. Do not use
`NOWERROR=1` as acceptance evidence. Record host compiler and cross compiler
separately. Review every applicable stale-reference report; untracked additions
also need direct whitespace/comment checking before delivery.

### Acceptance matrix

| Area | Required evidence |
|---|---|
| Import | Exact provenance, included/excluded modules, options, patches, licenses, reproducible source manifest |
| Functional host | Pinned mono/proportional TTF and OTF/CFF; 16/24/32 pixel sizes; lookup, metrics, space/empty glyph, overhang/negative bearing, kerning present/absent, missing glyph |
| Lifetime | Font-buffer retention, output-mask lifetime, size changes, interleaved independent contexts, repeated open/close and balanced allocations |
| Failure | Truncated/malformed fonts; missing/invalid charmap; invalid scalar/size/index; dimension/size overflow; allocation denial at each reachable allocation point with usable surviving state |
| Nonlocal returns | If used, a reproducible charmap-validator failure that actually enters the jump path, at optimized settings; subsequent operations and cleanup remain correct |
| Sanitizers | Production adapter and selected upstream sources under ASan/UBSan at optimization, with harness flags and any exclusions documented |
| Cross build | Strict target build/link; reviewed exports, undefined symbols, dependencies, relocation types, segment permissions and library-slot fit |
| Guest | Actual os64 fonttest loads both formats and renders mono/proportional samples at several sizes; logs assertions and visible specimen; invalid-font recovery leaves the process usable |
| Cost | Final ELF/section sizes, allocation peaks and cleanup accounting with fixture/size details and exclusions stated |

Use independent expected assertions where practical; two calls to the same
wrapper agreeing do not independently establish correct geometry. Pin fixture
hashes and relevant hinting/render settings for pixel or metric comparisons.
Grayscale blending in the specimen must be clipped and must handle bearings;
its display path should not erase overlapping glyph masks with opaque cells.

For QEMU, use the repository root build/run configuration (`make`, `make run`)
after integrating the owned build patch and arranging fixture installation.
Use isolated disk images/ports if another session is running. Inspect the actual
boot/root image and verify it contains the newly built library, test binary and
font fixtures before recording success. Run `/tests/fonttest` once implemented;
document its actual arguments and output. Save a screenshot and serial/test log
paths in the report. Compilation or a host-generated specimen is not guest
evidence. Hardware/P5 validation is not required for F1; make no hardware claim.

## Completion report and handback

Deliver the import/port, audit, frozen contract reference, tests, fixture/license
records, shared build patch, and reproduction README together. Separate source
selection/import from adapter and test changes in the review presentation. If
commits are made, identify them exactly; otherwise list changed files and provide
the diff/artifacts. Do not push, open a PR or merge solely on this handoff.

Use this report structure:

```text
F1 status: audit complete / contract pending / backend complete / other
os64 base and actual branch/worktree:
FreeType tag + full commit + archive/import digest:
Frozen backend contract revision and any approved deviations:
Files/commits delivered; separate shared build patch:
Enabled formats/modules/render modes and intentional exclusions:
Host tests: exact commands, compiler/options, pass/fail, artifact paths
Cross build: exact command, export/dependency/relocation findings
QEMU: boot/root evidence, test command/results, screenshot/log paths
Hardware: not run, unless actually performed
Measured code/data/allocation costs and exclusions:
Unrun checks, known limitations, remaining integration work:
Publication status:
```

F1 is complete when the agreed backend is built, exercised under host failure
tests and in the guest, and reproducible by the coordinator. If the contract is
still pending, deliver the completed independent audit/preparation with a precise
proposal; label backend implementation as pending. F1 completion does not claim
proportional Scribe, UTF-8 editing, font settings or full product readiness.

## Reference links

- [Feature design](FONTS.md), [delivery plan](FONTS_WORK_PLAN.md),
  [F0 contracts](docs/fonts/00-contracts.md), [F1 packet](docs/fonts/01-freetype-backend.md).
- [FreeType customization](https://github.com/freetype/freetype/blob/VER-2-14-3/docs/CUSTOMIZE)
  and [module list](https://github.com/freetype/freetype/blob/VER-2-14-3/modules.cfg).
- [Memory management](https://freetype.org/freetype2/docs/reference/ft2-system_interface.html),
  [module initialization](https://freetype.org/freetype2/docs/reference/ft2-module_management.html),
  [glyph retrieval/kerning](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html).
- [GCC nonlocal-return rules](https://gcc.gnu.org/onlinedocs/gcc/Nonlocal-Gotos.html).

Use the selected release's source/docs when moving beyond the feasibility pin;
current online documentation may describe a different version.

Current architecture verdict: [Fable R3 review](docs/fonts/FABLE-REVIEW-R3.md) approves backend,
layout and configuration contracts with no design findings remaining. The
[freeze record](docs/fonts/F0-FREEZE.md) identifies the baseline for downstream assignments.
F1 implementation review and guest validation remain separate gates.
