# F4 — Font-aware widgets and Scribe editing

Frozen F0 contract: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Read [the design](../../FONTS.md), [the work plan](../../FONTS_WORK_PLAN.md), and
AGENTS.md. Start after F0's Unicode/editor/measurement contract and an integrated
F1/F2 baseline. Use F0's deterministic layout fixtures as independent oracles.

The shared provider/replacement boundary is [FONT_PROVIDER.md](../../FONT_PROVIDER.md),
implemented by `os64/font_provider.h` and `os64/font_adopt.h`. Use its role sets
and prepare/barrier/commit/abort sequence. The assignment receipt identifies the
exact F2.5 base; the earlier F2 checkpoint alone does not contain these APIs.

## Scope and ownership

Own libos64's ui.c, ui_controls.c, ui_list.c, ui_text.c and related internal
widget helpers, plus Scribe's font-related integration and focused tests.
Other application layout changes need a named ownership extension from the
coordinator. Shared header changes go through F0. F5 owns the font settings UI
and theme/configuration publication code. Keep textfields and textviews under
one owner; they share editing semantics and ui_text.c.

Read the current textfield/textview structures and document vtable in ui.h,
Scribe's horizontal-scroll and widest-line calculations, and existing text tests.
The buffer vtable and plain-text storage behavior are assets to preserve.

## Deliverable

Replace fixed-width estimates with F2's measured runs in widget painting and
layout. Derive row height, insets and control minima from font metrics. Reuse
shared drawing, fit and hit-test APIs rather than local sums of glyph widths.

For editing, store document positions in original source-byte offsets and use
the agreed legal caret/cluster boundaries. Support mouse placement, drag
selection, keyboard motion and deletion, tabs, caret visibility, pixel horizontal
scrolling, and remembered pixel X during vertical navigation. Use the same run
for visible text, cursor and selection. Retain source bytes on load/save; display
fallback must not rewrite the buffer. Honor F0's invalid-input and accented-text
policy. Do not silently expand into soft wrapping, rich text or full shaping.

Update Scribe's maximum-line extent and horizontal scrollbar together. Avoid
repeated full-document layout on each keypress; record the cache/invalidation
strategy and behavior when a formerly widest line shrinks. Preserve its help-view
vtable swap, find/selection behavior, and ordinary editing regressions.

Implement the consumer-side relayout/font-replacement operation F5 will call.
Retain valid focus, document contents and selection on successful changes and
usable old state on failure. A larger font at 1024x768 needs an intentional
layout solution; do not hide essential controls to make a test fit.

## Required evidence

Exercise differing advances, kerning, overhangs, blank lines, trailing spaces,
tabs, accents in both encoded forms, malformed UTF-8, fallback, clipping, and
allocation limits. Verify click/caret/source-offset mappings against expected
fixtures. Test selection deletion, navigation across short/long lines, scrolling,
font switches with unsaved edits, and byte-identical save for unchanged files.

Run existing editor/widget tests plus focused regressions, strict builds, and
QEMU interaction with proportional and monospace faces at multiple sizes.
Separate text-layout correctness from language features the support profile
excludes. Deliver through [HANDOFF.md](HANDOFF.md), with remaining layout consumers
listed rather than a claim that every application has migrated.

## R3 row and oversized-line contract

Derive row pitch/baseline from the primary role face, including for empty rows.
Keep it fixed when a line contains a taller fallback or missing marker. Clip
paint to the row; use run selection X with row Y. Cover a 12-pixel primary row
containing a 16-pixel marker: neighbours, vertical navigation, scrollbar extent
and click-to-row must keep their geometry.

A line exceeding run limits remains editable through a bounded source window
around the caret. Choose window endpoints at W1 cluster boundaries and retain
its document byte origin/revision to translate run-relative carets and selections.
Show explicit non-document “more” indicators at omitted ends; they are neither
saved bytes nor editable glyphs. Moving to an edge shifts the window, preserving
the document caret and rebuilding the run. Selections may span omitted bytes;
delete/copy/save use document offsets and the full buffer, not the window.

Bound window bytes and placements before layout, then shrink the window on a
run LIMIT rather than exposing an empty/uneditable line. A cluster whose own
span exceeds the run limit needs a bounded diagnostic placeholder owned by the
editor, with caret stops at its original start/end. Preserve or delete that
entire source span, and permit navigation to neighbouring clusters. Scanning a
long cluster must use bounded memory. Cache line/cluster boundary information
by document revision to avoid rescanning unchanged prefixes on each keystroke.

Treat omitted extent as unknown, not as the full line's measured width. Use
window navigation/continuation indicators while calculating wider extents lazily;
do not claim a complete horizontal scrollbar extent from a partial window.
Test editing/splitting a line beyond 1 MiB and saving untouched omitted bytes,
including a mark sequence larger than a run, before F4 is considered complete.
