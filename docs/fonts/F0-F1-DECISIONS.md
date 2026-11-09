# F0 replies to F1 — review candidate R2

2026-09-16. These are the font coordinator's dispositions of the six questions
in Opus's `docs/fonts/F1-REPORT.md`, including his corrections in
`F1-REPLY-R1.md`. They define the R2 candidate for Fable's design review; they
are not a declaration that Fable approved the design or that F1 passed its
implementation review.

Source worktrees on this machine:

- F0: `/home/yogi/src/os64/.worktrees/font-design`, branch `codex/font-design`.
- F1: `/home/yogi/src/os64/.worktrees/freetype-backend`, branch `opus/freetype-backend`.
- Both use base `3b82356413ab8f183bd6506febe8d06fea6e7de0`; work is local and
  uncommitted at this checkpoint. The [review brief](FABLE-REVIEW.md) describes
  the input snapshot and review boundary.

The [backend header](../../userland/libos64/include/os64/font_backend.h) remains
table revision 1 with the `_v1` getter. R2 is a documentation/review revision:
function signatures, structs, constants and enum values do not change. The
header gains explicit comments where R1's wording could be read ambiguously.
F1 should refresh its header copy from F0 when integrating; its implementation
should not be rewritten merely to match a new document revision label.

## 1. Glyph index zero and the missing-character marker

**Keep render(0) and pair_adjust involving 0 as BAD_ARGUMENT.** Lookup continues
to report MISSING for an unmapped scalar. No .notdef rendering entry is added.

R1's layout contract already selected a synthetic outlined marker after the
fallback list is exhausted. This gives F2 a visible result even when a font has
an empty or unsuitable .notdef image, and avoids making the final fallback's
geometry depend on the last failed font. In proportional runs the marker uses
8x16 bitmap metrics and an 8-pixel advance; grid mode preserves the selected
cell advance and clips to the cell. Placement identity 0 denotes this synthetic
image, not a glyph belonging to a backend face.

R1 put the zero-index sentence just before pair_adjust, which made its scope
easy to misread. R2 explicitly states the rule at render too. Existing fake
tests reject render(0); the updated tests also reject a zero kerning operand
and verify its output is cleared. Marker vectors retain logical/source bounds.
F2 is not blocked on adding .notdef support to F1.

## 2. GPOS pair support

**Accept the limited coverage and document zero as ambiguous.** For the selected
FreeType pin, PairPos formats 1 and 2 and extension lookups are supported, but
the accepted value records are first-glyph XAdvance alone (0x0004) and no
second-glyph value record (0). Unsupported layouts can yield OK with adjustment
zero. Callers cannot interpret that as proof that the font contains no kerning.

Keep upstream's legacy-kern preference and GPOS fallback; do not add the tables
together. General script/language-sensitive positioning and complex shaping
remain outside this interface's guarantee. F2 must not compensate with a private
GPOS reader. Its future shaping boundary can replace the source of positioned
glyphs without making editors understand OpenType tables.

Evidence: F1 `upstream/src/sfnt/ttgpos.c` validates these value formats;
`port/backend.c` delegates pair retrieval to FreeType. F1's independent fixture
reader explicitly adopts the same coverage restriction. That is acceptable for
testing this limited interface, but not evidence of general GPOS conformance.

## 3. Fractional kerning

**Confirm FT_KERNING_UNFITTED in both hint modes.** Return scaled 26.6 values
without whole-pixel grid fitting or the default mode's small-size reduction.
Preserve the precision the engine supplies; this does not remove the unavoidable
rounding involved in fixed-point scaling itself. Hint mode still affects glyph
outlines and advances; it does not switch pair adjustments to whole pixels.

F1's Source Sans 3 example demonstrates the reason: a -14/1000-em adjustment
at 32 pixels is roughly -0.448 pixels, which early pixel rounding erases.
F2 accumulates fractional positions and rounds at painting, using the same
logical positions for caret/hit/selection operations.

F0 adds a fake e/W pair of -29/64 pixels and two golden vectors. One preserves
the subpixel adjustment; the other repeats it, ending at 1734/64 pixels instead
of the 1792/64 that independently rounding both adjustments to zero would give.
The fixture is deliberately synthetic; it does not claim the real e/W pair
has those values in an installed font. F1's pinned real-font tests remain the
independent evidence for the FreeType adapter.

## 4. Hinting configuration

**Keep the TrueType bytecode interpreter disabled for this initial profile.**
NORMAL uses the autofitter for ordinary TrueType outlines and the native Adobe
hinter for CFF outlines. NONE disables both native hinting and autohinting.
Do not force CFF through the autofitter for cosmetic uniformity.

The accepted tradeoff is a smaller enabled parser/interpreter surface in return
for giving up designer-supplied TrueType hinting. It is not a claim that the
formats share a hinter or render identical pixels. F5 must judge the actual
selected files at useful sizes; font family labels and filename extensions do
not establish identical outlines or rendering paths. Reconsider the interpreter
if a small-size visual comparison warrants it, with the relevant upstream fixes
and failure-path tests included in that change.

The report's corrected explanation is the one retained by R2. Historical claims
about which hinter older desktop systems shipped are not needed for this decision.

## 5. Variable-font detection

**Retain explicit fvar/CFF2 table rejection.** Disabling variation support is not
equivalent to rejecting variable input; upstream capability flags can disappear
with that build option. The adapter must not silently open a variable font at
its default instance behind a cache identity that has no variation coordinates.

This approves the detection strategy, not every error path in its implementation.
F1 implementation review must verify a present table is recognized even with
variation support disabled, malformed table lookup does not become accidental
acceptance, failures clean up, and static TTF/CFF1 fixtures remain usable.
Collection refusal remains a separate contract rule. Supporting collections or
variable axes later requires an explicit interface/configuration change.

## 6. UTF-8 display names

**Accept reading Unicode name records directly and a documented ASCII fallback.**
FreeType's already-flattened family string cannot recover accents discarded
earlier. The direct UTF-16BE conversion is justified by the metadata contract.

Make the implemented precedence explicit: typographic IDs 16/17 outrank legacy
IDs 1/2; within an ID, Microsoft Unicode outranks Unicode-platform records;
within Microsoft, en-US 0x0409 is preferred, and within Unicode-platform records,
language 0 is preferred. Equal candidates retain table order. The language-0
preference is a deterministic default, not a claim of English detection. Family
and style are selected independently. This matches F1's scoring order, which
gives platform preference more weight than its language preference.

Decode malformed Unicode units as U+FFFD, terminate at NUL and truncate at a
UTF-8 scalar boundary with the existing flag. Without a usable Unicode entry,
accept the engine's lossy ASCII copy; empty names permit a filename label.
F5 may show disambiguating filenames for duplicate family/style labels. It must
not derive paths or cache identities from display names. Full localization and
lossless decoding of every legacy name encoding are outside this profile.

These are selection/representation rules, not proof that the converter already
handles every malformed-name case. The implementation review still needs to
check surrogate, odd-length, truncation and duplicate-name fixtures.

## Small notes and remaining gates

Retain the 64-face limit, 64 MiB default backend cap and line-height floor of
ascent+descent. F1's sample face costs are useful sizing evidence, not a
worst-case resource guarantee; metadata metrics may legitimately be adjusted to
the documented line-height floor. No warning is required for that normalization.

No production function signature or additional backend capability is required
by these six dispositions. They also do not waive implementation discrepancies
that a later review may find. In particular, passing fixture tests does not
prove malformed-input coverage or bound CPU time.

Next gates: Fable's design verdict (backend separately from layout/configuration),
resolution of design findings, a recorded freeze revision, then implementation
review and integration. Existing F1 work is a reviewable implementation against
the candidate, not evidence that the freeze gate already occurred.
