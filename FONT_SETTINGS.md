# Font settings implementation

F5 implements the frozen configuration and publication contract in
[FONT_CONTRACTS.md](FONT_CONTRACTS.md) and the
[F5 packet](docs/fonts/05-configuration.md). Work starts on
`codex/font-settings`, from accepted F4 `3bf7b1f`. F3 `ab7a1cd` is integrated into this development branch. Chris authorized
finishing the combined feature before Fable reviews it; F3 independent review
therefore remains pending, without blocking F5 implementation.

## Boundaries and sequence

1. `font_config` owns strict, line-diagnosed configuration parsing, absolute
   path resolution, bounded file loading and provider preparation. It never
   publishes settings or mutates an active consumer. Discovery uses the same
   loader and provider metadata, with filenames kept as asset identities.
2. `ui_session` owns a preserving line envelope and generation-checked
   component publication. Font configuration joins the existing transport;
   palette/treatment writers must retain font and unknown dotted-key lines.
   Saved compositions and startup preservation use the same envelope rules.
3. Applications adopt prepared role sets through the existing F3/F4
   transactions. A rejected replacement leaves the active generation usable
   and retryable. Publication is not confirmation that every process adopted.
4. Appearance Workshop exposes independent roles, sizes, preview, refresh,
   Apply and persistence. Installation copies supported font files into the
   top configuration directory's `fonts/` directory, without rebuilding.
   The first compatible installation requires a reboot before publication.

## Loader policy

Compiled defaults are builtin/16 in each role. A missing configuration uses
those defaults; a malformed or unreadable selected file is diagnosed, never
silently replaced with a lower ladder entry. Parsing and loading are separate:
syntax, duplicate sources and paths can be checked without opening an engine.
Successful decoding produces resolved absolute paths suitable for relocation
when saved at the top of the ladder. Relative paths are never process-relative.

Each source is capped at the frozen backend limit (32 MiB); one preparation
holds at most 64 MiB of distinct source bytes. Identical resolved paths across
roles are read once per preparation. A later preparation reads fresh bytes;
existing provider sets and runs retain their own immutable contents.
Fallback slots retain their configured indices for diagnostics even when the
first slot is absent. Any source failure rejects the complete candidate.

## Verification

Use focused ASan/UBSan host tests for grammar, provenance, path resolution,
whole-candidate refusal, I/O and allocation failures, replacement identity and
terminal suitability. Envelope tests must exercise unknown-line preservation,
component conflicts, legacy snapshots and complete-payload size limits.
Strict cross-builds precede an isolated QEMU installation with real Scribe and
gterm, unsaved edits, live changes and reboot persistence. Hardware acceptance
is separate. Record each completed checkpoint and its limits in `docs/fonts`.

## Explicit reload identity

The session font component includes `fonts.serial`, a positive uint64 set to
its publication generation. Palette/treatment updates preserve it verbatim.
Consumers use this serial to distinguish a font reload (even when filenames
and sizes are unchanged) from an unrelated palette publication. It is transport
metadata, not a `fonts.conf` key. Older font components without it use their
session generation. This concrete envelope field is an F5 implementation
addition for Fable's combined review; it does not change the frozen backend,
layout or provider ABI.

## Consumer geometry and preview

Scribe uses F4's application planner and retains unsaved bytes, caret, selection
and view state across accepted changes. Fixed-coordinate libui tools use a
default text-row fit check; their previous fonts remain usable on refusal.
gclock measures a clock row before staging its centered label. gterm clamps
its initial frame to the screen before preparing the grid, so a large saved
font cannot open a preferred 100-column window beyond the display.

Workshop's specimen prepares its three retained runs with the candidate and
commits them without allocation. Its editor and confirmation dialog follow
the session independently; the specimen remains a local draft. Font Undo is
one step, and font Reset returns to the last saved/initial choices. Theme
loading preserves the independent font draft.
