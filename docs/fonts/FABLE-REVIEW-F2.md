# F2 implementation review — Fable

2026-09-17. Candidate: `codex/text-layout` at `32d8243`, worktree
`.worktrees/text-layout`, base `7c9e2d2` (frozen F0 `23bf6dd` + receipt
`0ab3cee` + reviewed F1 `f065c7c`). Reviewed against
[02-text-layout.md](02-text-layout.md), FONT_CONTRACTS.md (R3, frozen) and
[F2-REPORT.md](F2-REPORT.md). This is the implementation review the report
asks for. It is not a merge.

## What I read and ran

- Read in full: `text_internal.h`, `text_cache.c`, `text.c`, `text_decode.c`,
  `text_draw.c`, `text_bitmap.c`, the generated table's shape, the diffs to
  `draw.c`, `font_psf1.h`, both GNUmakefiles, `texttest.c`, the host
  harness's coverage map and its bitmap/grid/suppression tests, the
  evidence index and `elf.log`, and the F1 implementation review's
  disposition. Looked at `box-atlas.png` and `gui-specimen.png` as images.
- Ran: `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py --real`
  → `PASS: 7910 checks`, real engine, -O2. `git diff --check` clean.
  `readelf -d` on the built `libos64.so`: `NEEDED libfreetype.so`; `nm -D -u`
  shows NO undefined symbol from libfreetype, and no libos64 source names
  `os64_freetype_backend_v1`.
- Checked by reading, because the code depends on them: `os64_utf8_decode`
  returns 1 with U+FFFD for a bad lead or continuation and 0 only for an
  empty input, so the cluster scanner cannot stall; `os64_charset_glyph`
  returns a blank glyph for a `NONE` entry, which the box-range check in
  `resolve` guards by scalar before it can be trusted.
- Not run: the -O0 pass, a fresh guest boot, the A/B spawn batches. Their
  logs were read.

## Verdict

**F2 is accepted as an implementation of the frozen contract. Nothing is
required before merge: the one item I raised (F2-1) is ruled and closed, and
the remaining three are optional improvements.** No
correctness, lifetime, budget or caret finding survived reading the code
against the contract. Quinn's four named worries are answered below.

## The four worries

**Two-budget retry ownership.** Sound. The backend's allocator is
`allocate_raw`, which records why it refused (context cap → `LIMIT`,
callback NULL → `NO_MEMORY`) and NEVER evicts, so the backend is never
re-entered from its own allocation callback. Eviction lives only in
`text_alloc` and in the once-per-operation retry loops around
`face_open`/`lookup`/`pair_adjust`/`render`, all of which run after the
backend call has returned. `text_status` folds a callback refusal that was
really the context's cap back into `LIMIT`, so the engine's own cap and the
context's cap both read as `LIMIT` to the layout code, and a host allocator
failure stays `NO_MEMORY` with no retry. Backend bytes pass the context's
counter once; `engine_stats` is never added. The retry fires only when
eviction actually released bytes (`text_evict` returns the delta). This is
the R3 rule as written.

**Caret monotonicity.** Sound. A pair adjustment moves the boundary before
the right glyph only if the moved boundary stays strictly right of the
previous cluster's start (`add_glyph`, the `*pen+delta > carets[k-1].x`
test), otherwise it is suppressed; a zero-advance standalone glyph becomes
the marker before it can produce a zero-width interval; `next<=*pen` is
refused; a tab always lands strictly right of the pen because the stop is
the next multiple above it. Carets are therefore strictly increasing in
both byte offset and X, which is what the binary searches in `hit`, `fit`,
`caret` and `selection` rely on. The hit tie goes to the later offset by
construction of the search (equal distance keeps `lo`).

**Fallback and marker source spans.** Sound. A cluster's `[begin,end)` is
decided once by `text_decode` and copied to every placement it produces;
the base-plus-marker case makes two placements with one span and one caret
(`extra_marker`), the digit-then-marks case makes two clusters with a caret
between. The placement counts from the first pass and the writes in the
second pass are the same function of the same bytes, so the arrays are
exactly sized. Fallback resolution happens in `resolve` before the pen
moves, and the chosen image is pinned before it is placed.

**Procedural cell geometry.** Sound, and the atlas shows it: all 160 cells
present, double rails paired through corners, arcs and diagonals drawn,
blocks and shades filled, quadrants correct. The generated cell is keyed on
identity, index, width, height and top, so a real glyph index that happens
to equal a box scalar cannot collide (real glyphs are keyed with zero
dimensions). Grid painting clips to `[round(x), round(x+cell))` so cells
tile without seams, and the marker in grid mode keeps the cell advance.

## Findings

### F2-1 — RULED by Chris, 2026-09-17: the edge stays

The `libos64 -> libfreetype` edge is kept as committed. It comes back in F5
regardless, so removing and re-adding it is effort with no return; the
measurement below stays on record and the makefile comment stands. The
analysis is preserved for whoever asks why an unreferenced library is in
the closure. Nothing further is required of F2 on this point.

#### The finding as filed: the edge has no consumer yet

- **Observed:** `userland/GNUmakefile` links libos64 with `--no-as-needed
  $(LIBFREETYPE_SO)`; the built `libos64.so` records `NEEDED
  libfreetype.so` and imports nothing from it. The only program that calls
  the getter is `texttest`, and it links libfreetype itself. The edge is
  carried purely by `--no-as-needed`, which is the flag whose job is to keep
  an unreferenced library in the closure.
- **Cost:** every process on the system now opens and identity-checks one
  more library at spawn. The report's own A/B (six batches of fifty
  spawn/exit/reap cycles, 100 Hz timer) shows a 4 ms median difference on
  a 13 ms cycle, honestly labelled too noisy to attribute. That is still
  the only measured effect this slice has on `cat`.
- **Why it matters here:** the house rule is consumer-driven growth, and
  the makefile comment's reason ("so ordinary consumers share the same
  dependency closure") describes a benefit that arrives only when a libos64
  function calls the getter, which is F5's provider. Adding the edge now
  pays the cost a slice early and, because nothing references it, a later
  `-as-needed` cleanup would silently drop it.
- **Correction:** remove `$(LIBFREETYPE_SO)` from libos64's link and its
  prerequisite list in F2, keep the measurement on record, and reintroduce
  the edge in the same commit as the first in-library caller, where a
  symbol reference makes it self-justifying. If Chris would rather have the
  closure settle now so F5 changes no process's startup, keep it and say so
  in the makefile comment; either is coherent, and it is his doctrine to
  apply. When the measurement is repeated, use a finer clock than the tick
  (the kernel exposes the TSC rate since #105) and pin the batches to one
  core; the current spread hides a 4 ms effect.

### F2-2 — optional: eviction is all-or-nothing, and fires for limits it cannot cure

`text_evict` discards every unpinned image on any `LIMIT`, including the
64-face ceiling and an oversized request that no eviction could satisfy
(a 1 MiB-glyph line's 48 MiB placement array on a small context). The
result is correct, and the retry rule is met, but one refused run empties
the cache for every other line on screen. Evicting oldest-first until the
request fits, and skipping eviction when the refusal was a count limit,
would keep the neighbours' glyphs. Not a contract matter.

### F2-3 — optional: a composed accent missing from every font loses its base

`text_decode` substitutes the precomposed scalar for `e`+U+0301, and if no
font carries it, `resolve` exhausts fallback and the whole three-byte
cluster becomes one marker, so the `e` is not drawn. The frozen text
permits this ("a missing composed glyph follows normal fallback/marker
rules"). Chris's base-plus-marker ruling was about keeping the letter
readable, and falling back to that form when the composition is missing
everywhere would honour its spirit at no contract cost. Product taste; his
call, not a defect.

### F2-4 — optional: two linear scans per scalar

`latin()` walks 190 entries and the composition lookup 161 for every
UTF-8 cluster, and the CP437 reverse lookup in `resolve` walks 256 bytes per
glyph. Correct and bounded; a 1 MiB line spends a few hundred million
compares. A sorted table with a binary search, or a 256-entry reverse map
built once, is a five-minute change when a profile asks for it.

## Notes for the consumers (not findings)

- **F5 appends the builtin instance.** The layout engine tries only the
  fonts in the run's list; the bitmap fallback the configuration section
  calls "implicit after configured faces" is the provider's to append
  (`os64_text_font_bitmap`, last, within the eight). The marker is the only
  fallback F2 supplies on its own.
- **F3, Latin-1 bytes 0x80..0x9F under an outline face.** In grid mode with
  `OS64_TEXT_LATIN1` those bytes become C1 scalars, which no outline font
  carries, so they reach whatever fallback is last. The console draws the
  face's glyph at that index; only the bitmap instance at the end of the
  list reproduces that. Same remedy as above.
- **F4, row pitch.** Run metrics include the marker's 8x16 box in
  proportional mode, exactly as frozen; the row is the consumer's.
- **The F1 implementation review** was Quinn's, not mine as R3 assumed. I
  read its eight corrections and its disposition; they are the kind of
  findings a real review produces (a BMP map ranked over a full-repertoire
  one, a one-face TTC slipping past the count check, a latent negative-pitch
  read). I accept it as the F1 record without having re-read the whole port.

## What this review does not establish

No guest run of my own, no P5, no -O0 rerun. The host suite is the oracle
for geometry and the guest specimen for the real engine on the real
allocator; both were produced by the author and one of them reproduced
here. Merge, and the F3/F4/F5 slices that consume this, remain separate.

— Fable 5.1
