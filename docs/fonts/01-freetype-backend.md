# F1 — FreeType source audit and backend

Read [the design](../../FONTS.md), [the work plan](../../FONTS_WORK_PLAN.md), and
AGENTS.md. This is the recommended initial independent assignment. Its source
audit can begin now; production adapters depend on F0's frozen backend interface.

The [filled-in Opus handoff](../../FREETYPE_HANDOFF.md) supplies the concrete base,
source-audit brief, repository paths, implementation checklist and evidence
requirements for this assignment.

Use the [F0 R3 contract](../../FONT_CONTRACTS.md) and
[`os64/font_backend.h`](../../userland/libos64/include/os64/font_backend.h) for
the interface checkpoint. The callback-driven header is in libos64's include
tree for consumers but imports no libos64 functions. Review the candidate with
the coordinator before freezing it; no production adapter is claimed here.

F1's candidate implementation now exists in `.worktrees/freetype-backend`.
[F0's dispositions](F0-F1-DECISIONS.md) answer its six contract questions;
the backend API signatures remain at table v1. The candidate still needs the
architecture gate and later implementation review before integration acceptance.

## Scope and ownership

Own proposed `userland/libfreetype/` upstream sources, private port, source list,
export definition, build fragment, source manifest and UPSTREAM_REVIEW.md;
applicable license records; and focused backend fixtures. Final paths come from
F0. Supply shared build/image changes to the coordinator rather than racing other
packages in root GNUmakefile or userland/GNUmakefile. Do not edit UI/editor code.

## Source-audit deliverable

Review the actual selected release and relevant upstream fixes. Record origin,
exact revision, integrity digest, included/excluded modules, compile options,
local changes and license obligations. Follow the source-selection discipline in
`userland/libtls/UPSTREAM_REVIEW.md`; do not present it as a complete security audit.

The feasibility candidate is FreeType tag VER-2-14-3. Official source:
https://github.com/freetype/freetype/tree/VER-2-14-3 . Its module declarations and
CUSTOMIZE/INSTALL.ANY documents are authoritative inputs. Include SFNT/TrueType/
CFF/smooth/autofit and required PS helpers; optional monochrome support follows
F0's render-mode decision. Excluded formats and optional compression/shaping
libraries must not become accidental dependencies.

The probe's external helper inventory was malloc/realloc/free, memchr/memcmp/
memcpy/memmove/memset, qsort, strcat/strcmp/strcpy/strlen/strncmp/strncpy/strrchr/
strstr. It used private GCC nonlocal-return builtins. Reproduce the dependency
check for the chosen configuration rather than copying that inventory as proof.
Propose adapters for genuinely missing operations with correct semantics.

## Backend implementation deliverable

Implement F0's memory-backed face creation, metrics, glyph lookup/raster output,
kerning and release operations. Hide upstream structs from consumers. Follow the
frozen callback/lifetime and dependency rules; do not invent an engine-owned file
loader or mutate the library dependency direction. Bound allocation and failure
paths. Keep upstream code separate from the private adapter so a future version
update has a reviewable diff. General public qsort or setjmp APIs are separate
from private helpers unless F0 explicitly assigns them here.

## Required evidence

- Strict cross-build, exported/undefined symbol inventory, DT_NEEDED and relocation
  inspection; verify no unwanted libc, stdio, compression or GUI dependencies.
- Host tests for TTF and OTF/CFF, multiple sizes, equal/unequal advances, empty
  glyphs, negative bearings, malformed/truncated fonts, char-map validator failure,
  allocation failures, lifetime and cleanup. Include optimized nonlocal-return
  failure-path tests if that mechanism is selected.
- Real os64 guest specimen loading both font formats, measuring and rendering
  them at multiple sizes. Report host and guest results separately; no P5 claim
  without a hardware run.
- Reproducible fixture sources and licenses. Font files are licensed separately
  from FreeType. Record measured code/data and allocation costs with exclusions.

Use [HANDOFF.md](HANDOFF.md). Return unsupported formats and remaining limitations
explicitly. Backend completion is not a claim that Scribe supports proportional
editing or that the Fonts settings product is complete.

## R2 review follow-up

Fable conditionally approved the backend design, subject to B1. R3 requires
LIMIT for engine-cap refusals at create/open/render and NO_MEMORY for callback
NULL; required module allocation failure must refuse engine creation. The F1
worktree contains the regression and correction; see F1-B1-REPORT.md there.

The implementation review remains separate. Include the B1 changes, a target
runtime tripwire for losing `-fno-tree-loop-distribute-patterns`, refusal of bare
CFF2 without an SFNT directory, and metadata fixtures for unpaired surrogates,
odd byte lengths, truncation and duplicate names. Existing host/guest passes do
not close those review tasks. Retain the current mask bound; the suggested
four-em bound was optional and needs its own contract change if adopted.
