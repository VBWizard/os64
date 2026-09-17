# Font delivery and collaboration plan

Frozen F0 contract: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Design planning record, 2026-09-16; base `3b82356`, merged PR #109.
[FONTS.md](FONTS.md) is the feature authority. Its settled product requirements
are accepted; the contract baseline is identified in [F0-FREEZE.md](docs/fonts/F0-FREEZE.md).
This checkout contains F0 contracts/fixtures. F1 has a separate candidate port
and preserved host/guest evidence in `.worktrees/freetype-backend`; it is not
integrated into this checkout or accepted by an implementation review.

[FONT_CONTRACTS.md](FONT_CONTRACTS.md) and its headers are the approved F0 R3
contracts. The fake backend has host sanitizer and strict target compilation
evidence; the golden vectors have arithmetic checks. Production layout, guest
rendering in F2, F1 implementation review and F3/F4/F5 implementation remain outstanding.
The six F1 questions are answered in
[F0-F1-DECISIONS.md](docs/fonts/F0-F1-DECISIONS.md); use the
[Fable R3 verdict](docs/fonts/FABLE-REVIEW-R3.md) for the completed architecture checkpoint.

## How to share the work

Give an implementer FONTS.md, this plan, and their linked packet. They should
read the repository's AGENTS.md and cited current source files. Packets contain
scope, ownership, prerequisites, deliverables, validation, and a completion
report format. They do not depend on access to chat history or workstation /tmp.
Use the [handoff template](docs/fonts/HANDOFF.md) to name the actual branch,
base commit, contract revision and allowed files when assigning a package.

F0 is frozen at the recorded commit. F1's candidate engine exists, with the B1
correction checked by design review; the next coordinator task is its complete
implementation review and fresh guest validation. F2 can build against the
frozen contracts and deterministic backend. Once F1/F2 integrate, F3's terminal
and F4's widgets/editor are separate application integration assignments.
F5 can implement the approved configuration parser; live Apply depends on F3/F4
adoption and invalidation. This plan describes work that can be shared; it does
not assign or launch agents.

## Packages and ownership

| ID | Work packet | Main ownership | Start dependency |
|---|---|---|---|
| F0 | [Contracts and fixtures](docs/fonts/00-contracts.md) | Architecture, public/internal interface headers, common test contract | Current merged base |
| F1 | [FreeType backend](docs/fonts/01-freetype-backend.md) | Upstream pin, private runtime adapter, engine module, backend tests | Candidate ready for implementation review |
| F2 | [Text layout and drawing](docs/fonts/02-text-layout.md) | Bitmap backend, font instances/cache, positioned runs, measurement/drawing/hit tests | F0 layout/backend freeze |
| F3 | [Terminal integration](docs/fonts/03-terminal.md) | gterm cell metrics, rendering and existing PTY resize use | F0 terminal rules; integrated F1/F2 |
| F4 | [Widgets and Scribe](docs/fonts/04-widgets-editor.md) | Widget measurement, editable text geometry, Scribe integration | F0 language/editor rules; integrated F1/F2 |
| F5 | [Configuration and font settings](docs/fonts/05-configuration.md) | fonts.conf, discovery/roles, persistence, settings UI, live application | F0 config freeze; live path after F3/F4 invalidation |
| INT | Coordinator integration | Root build/image manifests, shared ABI coordination, cross-package acceptance | Throughout; final acceptance after required packages |

Paths named in packets that do not exist are proposed additions, not existing
APIs. F0 R3 chooses `userland/libos64/include/os64/font_backend.h`, `text.h`,
and `text_draw.h` for the approved contract. They contain declarations and do not
establish implemented services. Use the exact baseline from F0-FREEZE.md for
dependent production code.
One owner writes each shared file at a time. In particular, ui_text.c is owned by
F4, not split between competing field/editor assignments. F2 owns draw.c during
its migration. F0 owns interface header changes; consumer owners request changes
instead of silently extending them. INT integrates edits to userland/GNUmakefile,
root GNUmakefile, and shared library/address/export inventories. Packages provide
a patch or isolated include fragment for those shared build changes.

Package implementation should use isolated branches/worktrees based on the
recorded contract commit. A handoff identifies the exact prerequisite commits;
"latest branch" is not a sufficient dependency. Rebase/merge the coordinated
base deliberately and re-read comments and interfaces that came across.

## F0 completion: make downstream work concrete

Before production implementations are assigned, publish:

- A written choice of library dependency direction, matching source/build graph,
  and the private-runtime versus shared-runtime support policy.
- Versioned-by-commit headers for backend operations, font instance ownership,
  positioned runs, metrics, source-offset/caret mapping, status/error results,
  drawing and invalidation. No FreeType types in application-facing interfaces.
- Explicit ownership/lifetime/thread rules and numeric bounds. Decide signed
  fixed-point representation, overflow handling, font/glyph/run/cache limits,
  fallback chain and baseline policy, tab stops, and malformed-input behavior.
- A concrete UTF-8 Western-text support profile, including composed/decomposed
  accents and supported editing clusters. Broader shaping remains separate.
- Configuration schema and resolution rules, pixel sizing, initial font roles,
  missing/invalid-file behavior, terminal font suitability, startup/session
  semantics, and legacy appearance compatibility.
- Test fixtures with expected positions and source ranges, not just screenshots.
  A fake proportional backend must exercise widths, kerning, overhangs, combining
  clusters, and missing glyphs without waiting for FreeType or installed fonts.

Freeze the contract as a named commit and record it in each assignment. This is
an engineering dependency, not a requirement for repeated user permission on
routine implementation details. Take product decisions back to Chris when they
change the accepted behavior; take cross-package design changes to the coordinator.

## Integration checkpoints

| Checkpoint | Required evidence |
|---|---|
| C0: contracts usable | Example call flows, ownership review, fixtures with unambiguous expected results, Fable design review |
| C1: font engine usable | Strict os64 link/export/dependency checks, host failure tests, real guest loading and rendering of TTF and OTF |
| C2: layout coherent | Bitmap compatibility; shared measurement/drawing/caret/hit-test results with proportional fonts; bounded cache and fallback tests |
| C3: consumers correct | gterm grid/resize/charset behavior; widget layout; Scribe editing/selection/scrolling and byte-preserving saves in QEMU |
| C4: complete product | Install/select/persist fonts; startup and live changes; legacy themes; failure retains usable state; 1024x768 acceptance |

Passing F1 does not complete fonts. Passing a host test does not establish guest
or P5 behavior. Report compile/link, host execution, QEMU and hardware evidence
separately. Record tests that were not run and why. The final implementation
review follows the design review; merge status and authorization remain separate.

## Change and delivery discipline

Every package returns a concise description of the behavior delivered, exact
commit(s), file ownership changes, test commands/results, known limitations, and
any contract deviations. Correct comments alongside changed behavior. Include
meaningful failure regressions and relevant strict builds; run diff checking and
stale-reference checks on applicable diffs. Runtime claims need guest evidence.
Avoid rebuilding unrelated subsystems solely to claim a larger test count.

If a mock passes but the real engine disagrees, reconcile the contract and real
behavior before adapting callers around the discrepancy. Changes to the support
profile, persistent schema, or shared interface invalidate affected fixtures and
need coordinated review. Keep open design questions explicit; a packet owner
must not turn a missing decision into undocumented behavior.

No package includes kernel console changes, new syscalls, decoration controls,
a Unicode PTY redesign, or full multilingual shaping. An observed need for those
is a scoped design issue to bring back, not implicit authorization to implement it.

Current architecture verdict: [Fable R3 review](docs/fonts/FABLE-REVIEW-R3.md) approves backend,
layout and configuration contracts with no design findings remaining. The
[freeze record](docs/fonts/F0-FREEZE.md) identifies the baseline for downstream assignments.
F1 implementation review and guest validation remain separate gates.
