# Font work packets

For the integrated implementation review, start at
[FABLE-FONTS-REVIEW.md](FABLE-FONTS-REVIEW.md) in `.worktrees/font-settings`.
It indexes the complete F0–F5 code, prior reviews and latest follow-ups.
The assignment packets below preserve the original work breakdown.

Frozen F0 contract: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Read [FONTS.md](../../FONTS.md) for the design and
[FONTS_WORK_PLAN.md](../../FONTS_WORK_PLAN.md) for dependency and ownership rules.
These are shareable assignments, not claims that the feature is implemented.

The [F0 R3 contracts](../../FONT_CONTRACTS.md) provide approved headers and
concrete policies. [Fixture instructions](../../tools/fonts/README.md) describe
what is executable and what its tests establish. All three design gates are approved; the freeze record names the implementation baseline.

Read [F0's six answers to F1](F0-F1-DECISIONS.md) and share the
[Fable review brief](FABLE-REVIEW.md) for the architecture review. The input
manifest records the exact local files in both worktrees used for this handoff.

For the first external assignment, use the filled-in
[FreeType audit and backend handoff](../../FREETYPE_HANDOFF.md). It includes the
base, repository map, prior probe evidence, interface checkpoint and acceptance
criteria; the generic template below remains available for later packages.

1. [F0: contracts and fixtures](00-contracts.md)
2. [F1: FreeType backend](01-freetype-backend.md)
3. [F2: text layout and drawing](02-text-layout.md)
4. [F3: terminal integration](03-terminal.md)
5. [F4: widgets and Scribe](04-widgets-editor.md)
6. [F5: configuration and font settings](05-configuration.md)

Use the [handoff template](HANDOFF.md) to attach actual base/contract commits and
scope. F1's source audit can start before F0's interface freeze. Other production
code depends on the relevant frozen contract; an implementer may review current
code and prepare fixtures while that dependency is being resolved.

Current architecture verdict: [Fable R3 review](FABLE-REVIEW-R3.md) approves backend,
layout and configuration contracts with no design findings remaining. The
[freeze record](F0-FREEZE.md) identifies the baseline for downstream assignments.
F1 implementation review and guest validation remain separate gates.
