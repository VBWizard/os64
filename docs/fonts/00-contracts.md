# F0 — Shared contracts and fixtures

Frozen F0 contract: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Read [the design](../../FONTS.md), [the work plan](../../FONTS_WORK_PLAN.md), and
AGENTS.md. This package turns the design proposals into interfaces other authors
can implement independently. It owns shared headers and contract changes.

Local deliverable: [FONT_CONTRACTS.md](../../FONT_CONTRACTS.md), approved
`os64/font_backend.h`, `os64/text.h`, `os64/text_draw.h`, and the fake backend /
golden fixtures under `tools/fonts/`. The contract is R3; see [the baseline record](F0-FREEZE.md). Host tests
and target header compilation are available via `python3 tools/test_font_contracts.py`.
The six F1 questions have [written dispositions](F0-F1-DECISIONS.md).
[Fable R3](FABLE-REVIEW-R3.md) approves backend, layout and configuration.
The freeze record separates that baseline from downstream implementation gates.

## Inputs and current code

- `userland/libos64/include/os64/draw.h`, `userland/libos64/include/os64/ui.h`,
  and `userland/libos64/ui_internal.h`.
- `draw.c`, `ui_text.c`, `ui_theme.c`, `ui_session.c`, and `conf.c` under libos64.
- `userland/GNUmakefile`, `userland/libjpeg/shared.mk`, and libjpeg's private port.
- `abi/include/os64/pty.h` and `charset.h` for compatibility boundaries.
- F1's upstream dependency audit when available.

## Deliverables

Specify the backend beneath layout, font-instance ownership, positioned-run
representation, and operations to measure, draw, map source offsets to carets,
and hit-test pixel positions. API names in FONTS.md are explanatory, not an ABI.
Select actual header paths and update the other packets with those names.
Document source-buffer lifetime, font/glyph retention, errors, thread ownership,
allocation budgets, and limit/overflow behavior beside the declarations.

Use an immutable resolved-font identity; pathname reuse must not alias old and
new font contents. Define how runs retain their source/font data and what remains
valid after a cache eviction or configuration change. Specify kerning, baseline,
fallback, tabs, advance versus ink extents, clipping and rounding. Include empty
runs, trailing spaces, negative ink bearings, and a glyph with zero advance.

Publish a Unicode support profile with a finite test repertoire. Cover valid
UTF-8, ASCII, precomposed and decomposed Western accents, punctuation/symbols,
invalid sequences, and visible unsupported-input behavior. Define caret and
delete boundaries for supported clusters, independently from storage byte offsets.
Do not claim all grapheme handling or multilingual shaping. Choose the required
Unicode data source/version and mark handling strategy during this package.

Settle fonts.conf syntax, path resolution and fallback semantics, size bounds,
role inheritance, initial font suitability checks, and startup/live changes with
F5's owner. A kernel change or decoration feature is outside this contract.

Create deterministic fixtures and a fake backend with varying advances, a
kerning pair, overhanging ink, missing glyphs, a combining cluster, and forced
allocation failures. These let F2/F4 progress without the real rasterizer.

## Completion criteria

Provide example call flows for a label, an editable line, and a fixed-cell terminal.
Have a consumer implementer review whether the headers answer ownership and error
questions without reading the engine. Check fixture byte ranges and expected
positions manually as well as mechanically. Record the Fable design-review
outcome and outstanding questions. Name the contract commit when it is frozen;
do not label the draft frozen while an ABI decision is still open.

Use [HANDOFF.md](HANDOFF.md) for the returned integration report. Broader toolkit
rewrites and font importing belong to the downstream packages.

Current architecture verdict: [Fable R3 review](FABLE-REVIEW-R3.md) approves backend,
layout and configuration contracts with no design findings remaining. The
[freeze record](F0-FREEZE.md) identifies the baseline for downstream assignments.
F1 implementation review and guest validation remain separate gates.
