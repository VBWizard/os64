# F2 — Text runs, bitmap compatibility, cache and drawing

Read [the design](../../FONTS.md), [the work plan](../../FONTS_WORK_PLAN.md), and
AGENTS.md. Start against F0's frozen run/backend contracts and fake backend.
Real-engine integration follows F1; the fake backend is not final runtime evidence.

Contract headers are `os64/font_backend.h`, `os64/text.h`, and
`os64/text_draw.h`, under `userland/libos64/include/`.
[F0 R3](../../FONT_CONTRACTS.md) specifies their semantics. Implementations belong
in new `userland/libos64/text*.c` files; the fake backend and acceptance vectors
are in `tools/fonts/`. Use the exact baseline from [F0-FREEZE.md](F0-FREEZE.md).

## Scope and ownership

Own new text/font implementation files selected by F0, glyph/run cache internals,
the bitmap backend, run measurement and hit-testing, and font-aware additions
to `userland/libos64/draw.c`. Shared public headers remain coordinated through F0.
Do not modify ui_text.c, Scribe, gterm, or persistent configuration schema here.

Read existing draw.c, font_psf1.h, str.c's UTF-8 helpers, and the relevant drawing
and charset ABI headers. Preserve current byte/charset API behavior while consumers
are migrated explicitly. Existing Latin-1/CP437 bytes must not silently acquire
UTF-8 interpretation because a new drawing implementation landed.

## Deliverable

Produce the positioned runs defined by F0 and use them for width, fit, caret
positions, hit-testing, selection spans, and drawing. Decode source offsets
according to the explicit text policy. Resolve fallback before measurement;
retain the chosen face/glyph and source-range mappings for drawing and input.
Distinguish logical advance from ink bounds and preserve fractional positioning.

Own a bounded cache keyed by immutable face identity, glyph index, size and
render configuration. Define and test cache retention/eviction, stale-font
replacement, and mutable engine ownership exactly as the contract specifies.
A cache miss or allocation failure must not masquerade as an empty successful run.

Draw grayscale glyph masks with clipping and correct baseline/bearing positions.
Paint selection/background independently so overhangs survive. Provide the
monospace-cell mode needed by F3 without enabling proportional advances there.
Font-family/role resolution and reading fonts.conf belong to F5; accept resolved
instances or the agreed provider interface rather than growing a second resolver.

## Required evidence

Use fixed expected geometry, not a self-consistency test that compares one
function against itself. Cover bitmap compatibility, i versus W, a kerned pair,
spaces, zero-advance glyphs, negative ink bearings, tab origin and stops, accented
clusters, source offsets, empty runs, limits/overflow, and missing glyphs.
Check selection/caret pixel positions, insertion-point ties, fallback baselines,
cache eviction while glyphs are retained, and configuration-generation changes.

Check clipped drawing at surface edges and allocation-failure cleanup. With F1,
render real fonts in a guest and compare measured placement with the actual
output. Record strict build and focused host/guest commands in the
[handoff report](HANDOFF.md). Do not claim Unicode behavior beyond F0's profile.

## R3 review rulings to implement

- Engine-cap refusal is LIMIT; callback refusal is NO_MEMORY. Count backend
  storage once through the context allocator. Follow FONT_CONTRACTS' two-budget,
  bounded eviction/retry rule and test both thresholds with pinned runs.
- NORMAL advances may be integral while pair adjustments remain fractional.
  Quantize paint and carets alike; use mathematical floor for negative tab
  differences. Test overflow before allocating placement/caret arrays.
- Run metrics describe content. Consumers choose stable primary-face row pitch
  and clip taller content; do not fold row policy into run measurement.
- Unsupported Latin accents draw base plus one marker with the full source span
  on both placements, and no interior caret. Digits/punctuation followed by marks
  have a separate isolated-mark cluster. Generate/test the complete pinned W1
  composition table; fixture segmentation alone does not establish decoding.
- For selected-face misses in U+2500..259F grid mode, generate box/block/shade
  coverage at cell dimensions, reaching joining edges. This precedes F3; the
  five existing 8x16 procedural bitmaps are not a scalable implementation.
- Measure trivial-program spawn latency before/after the libfreetype dependency.
  Preserve the leaf's `--no-undefined`; do not introduce loader changes.

Engine calls, including child getters/releases, are serialized by the context
owner. The per-engine allocation-failure record relies on that contract and
needs no separate lock. Concurrent calls on one engine violate the ownership
rule; sharing an engine across threads requires serialization of the complete
operation, not a lock around that field alone.
