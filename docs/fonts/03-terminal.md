# F3 — Resizable monospace fonts in gterm

Frozen F0 contract: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Read [the design](../../FONTS.md), [the work plan](../../FONTS_WORK_PLAN.md), and
AGENTS.md. Start after F0's cell/charset/font contracts and an integrated F1/F2
baseline. Configuration can use an injected resolved font fixture until F5 lands.

The shared provider/replacement boundary is [FONT_PROVIDER.md](../../FONT_PROVIDER.md),
implemented by `os64/font_provider.h` and `os64/font_adopt.h`. Use its role sets
and prepare/barrier/commit/abort sequence. The assignment receipt identifies the
exact F2.5 base; the earlier F2 checkpoint alone does not contain these APIs.

Pinned F3/F4 foundation: [F25-FREEZE.md](F25-FREEZE.md).

## Scope and ownership

Own `userland/apps/gterm/gterm.c`, any local helpers, and terminal-focused tests.
Read PTY.md, gterm's selection/render/resize code, `abi/include/os64/pty.h`, and
`abi/include/os64/charset.h`. Shared text interfaces and build manifests are
coordinated with their owners. No kernel, PTY ABI or terminal parser changes.

## Deliverable

Replace hardcoded 8x16 cell assumptions with validated metrics for the selected
monospace font. Use the agreed font provider and fixed-cell drawing mode; do not
implement another font loader or glyph cache. Update initial dimensions, drawing,
cursor rectangle, pointer selection, and rows/columns calculations together.
Retain grid capacity and dimension bounds. Reject or explicitly fall back from
an unsuitable proportional face according to the F0/F5 contract.

When font metrics change, calculate and request the corresponding grid size
through existing os64_pty_resize. Handle refusal without pretending that the
snapshot and window now share the proposed geometry. Define transactional font
adoption with the coordinator: visible cells, pointer mapping, and PTY dimensions
must stay consistent on failure. Preserve selection according to the agreed
resize behavior rather than retaining ranges that name different cells.

The interpreted PTY remains byte plus per-cell charset. Preserve Latin-1/CP437
mapping, terminal line/block drawing, ANSI attributes, and foreground/background
semantics. This package does not turn the PTY into a Unicode terminal.

## Required evidence

Host-check cell calculations and bounds, unsuitable fonts, missing glyphs,
charset conversions, cursor placement, selection coordinates and refused resize.
In QEMU, use at least two font sizes; exercise text, line graphics, ANSI colors,
selection/copy, window resize, and font change. Verify the application's actual
reported terminal dimensions as well as the visible grid. Exercise a program
that reacts to terminal resizing. Check failure retains a usable terminal.

Return exact prerequisite commits and evidence through [HANDOFF.md](HANDOFF.md).
Do not extend kernel scope to remedy an unrelated issue without bringing back
a concrete diagnosis and proposed scope.

## R3 terminal acceptance

Consume F2's procedural fallback for missing U+2500..259F. Test a selected
monospace face without these glyphs at non-8x16 cell dimensions, with adjacent
CP437 corners, junctions, double lines, fractional blocks and shade patterns.
Joins must reach cell edges and glyphs must stay clipped to their own cells.
Changing metrics still commits only after the existing PTY resize succeeds.
