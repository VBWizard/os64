# Combined font implementation review

**One review checkout:** `/home/yogi/src/os64/.worktrees/font-settings`, branch
`codex/font-settings`. This tree contains the combined F0–F5 implementation from
Opus and Quinn, including the subsequent P5-driven fixes. There is no need to
switch to the earlier stage worktrees to review the delivered feature.

Chris requested review of the combined work. Prior receipts are context, not
instructions to exclude those components. In particular, F3 has no independent
approval receipt, and the integrated F5/follow-up review remains pending.
No mainline merge or push is implied by this handoff.

Start with [FONTS-OVERVIEW.md](FONTS-OVERVIEW.md) for the user-facing feature,
then use this index to inspect contracts, implementation and evidence.

## Full review range

Pre-font base: **`3b82356413ab8f183bd6506febe8d06fea6e7de0`**, merged PR #109.
Latest implementation at this handoff: **`020040b`**. Later commits completing
this index are documentation only. From this checkout:

```sh
git diff --stat 3b82356413ab8f183bd6506febe8d06fea6e7de0..HEAD
git diff 3b82356413ab8f183bd6506febe8d06fea6e7de0..HEAD
```

That is the complete font feature, including the pinned third-party import,
fixtures, build integration and later kernel Restore adjustment. The narrower
`07d8a69..HEAD` range below is useful for F5, but is not the full feature.

## Component index

| Slice | Code / contract | Report and prior review |
| --- | --- | --- |
| F0: design and contracts | [FONTS.md](../../FONTS.md), [FONT_CONTRACTS.md](../../FONT_CONTRACTS.md) | [Freeze](F0-FREEZE.md), [Fable R3](FABLE-REVIEW-R3.md) |
| F1: Opus's FreeType backend plus Quinn's corrections | `userland/libfreetype/`, its `UPSTREAM_REVIEW.md` | [Original report](F1-REPORT.md), [B1 correction](F1-B1-REPORT.md), [Implementation review](F1-IMPLEMENTATION-REVIEW.md) |
| F2: text layout/cache/drawing | `userland/libos64/text*.c` | [Report](F2-REPORT.md), [Fable acceptance](FABLE-REVIEW-F2.md) |
| F2.5: shared provider and replacement transactions | [FONT_PROVIDER.md](../../FONT_PROVIDER.md), `font_provider.c`, `font_adopt.c` | [Report](F25-REPORT.md), [Foundation receipt](F25-FREEZE.md), [Opus handoff](OPUS-F4-HANDOFF.md) |
| F3: terminal fonts | `userland/apps/gterm/` | [Report](F3-REPORT.md); independent review pending |
| F4: Opus's widgets and Scribe plus review fixes | `userland/libos64/ui*.c`, `userland/apps/scribe/` | [Report](F4-REPORT.md), [Quinn review](F4-QUINN-REVIEW.md), `f4-evidence/c3-review-r2/` |
| F5: configuration, installation, discovery, live settings | [FONT_SETTINGS.md](../../FONT_SETTINGS.md), `font_config.c`, `font_discovery.c`, `font_install.c`, `ui_font_settings.c`, Workshop | [Report](F5-REPORT.md), `f5-evidence/` |
| Follow-ups: combined catalog, role-fit, adaptive settings, logging | See the sections below; includes `kernel/src/gui/window.c` | [Role-fit](F5-ROLE-FIT-FOLLOWUP.md), [Responsive layout](F5-RESPONSIVE-LAYOUT.md), [Log routing](F5-FONT-LOGGING.md) |

Historical reports retain the branch names, pending statuses and measurements
from their checkpoints. They do not override later acceptance receipts or the
current implementation. Read the associated correction/follow-up reports too.

## Consolidation audit

Checked against `020040b`: the branch tips for `codex/font-design` (`0ab3cee`),
`opus/freetype-backend` (`f065c7c`), `codex/text-layout` (`4b0a839`),
`codex/terminal-fonts` (`ab7a1cd`) and `opus/font-widgets` (`3bf7b1f`) are all
ancestors of this combined branch. The F2.5 implementation `788de99` is also
an ancestor. Those six earlier worktrees, including `font-provider`, had no
tracked edits or untracked files at the audit.

The provider branch's final commit `1cd6b50` contained only coordinator docs;
it was not an ancestor. Its two handoff/freeze documents and packet links have
now been copied here verbatim from that commit's additions. No missing
implementation or code merge was required.

The separate `APP_INSTANCE_POLICY.md` in `/home/yogi/src/os64` is a future-feature
proposal, deliberately outside this font review. Title-bar font selection and
window-decoration changes are also future work.

## Scope and provenance

- Frozen F0: `23bf6dd` and [FONT_CONTRACTS.md](../../FONT_CONTRACTS.md).
- F2.5 provider: `788de99`, [FONT_PROVIDER.md](../../FONT_PROVIDER.md).
- F3: `aa8b7a2`, report `ab7a1cd`, [F3-REPORT.md](F3-REPORT.md).
- Accepted F4 plus independent receipt: `3bf7b1f`,
  [F4-QUINN-REVIEW.md](F4-QUINN-REVIEW.md) and `f4-evidence/c3-review-r2/`.
- F5 implementation: `419b36d`, clean image build fix `bda3dc5`, integration
  parent `07d8a69`. For a focused settings/packaging pass, use
  `git diff 07d8a69..HEAD`; use the pre-font base above for the full feature.
  The report and evidence checksums identify the validation artifacts.

The backend/provider boundaries remain unchanged. The responsive-layout
follow-up changes the existing kernel Restore path; no syscall or ABI is added.
Historical checkpoint reports are receipts for those checkpoints, not claims
about the integrated production selector.

## Decisions needing particular attention

1. `fonts.serial` is explicit reload metadata, stamped with the font publication
   generation. Palette/treatment writers preserve it. Consumers can therefore
   reload changed file bytes on an explicit font Apply without reloading fonts
   for a color change. This is an F5 envelope addition to the R3 contract.
2. Font Apply into an empty store preserves the partial startup overlay and
   application-specific palette defaults. Save pins the usable session before
   replacing disk choices. Readers read disk before the session store so a
   concurrent Save cannot leak next-boot choices into the current session.
3. Font and theme writers preserve unowned raw lines and comments, remove all
   duplicate owned keys, and refuse a complete envelope that exceeds 4096 bytes.
   Unknown dotted namespaces remain unowned; recognized keys remain validated.
   Named theme Save As carries the raw source envelope through the UI.
4. Installation stages a copy, validates the entire candidate role set, syncs,
   and publishes with no-replace rename. Existing names are refused. Discovery
   reads at most 64 MiB, scans at most 256 entries and returns at most 128 assets.
5. Fixed-layout libui participants validate initial/changed interface row heights
   against their slots; unrelated roles do not recheck unchanged installed heights.
   Scribe uses its measured planner; gterm uses the F3 PTY barrier. Per-process
   refusal keeps the old generation and remains retryable. Publication is not
   a global adoption acknowledgement.
6. Workshop preview has its own provider and staged specimen runs. Interface,
   terminal and document choices are independent. Apply/Save remain separate;
   installing does not itself change either session or startup settings.

Please report behavioral hazards and contract violations with a reproducer or
specific failing path. Known limits and deferred work are in F5-REPORT.md;
wording-only improvements are not review findings under AGENTS.md.

## Handoff checkpoint — 2026-09-20

The clean root build failure is fixed in `bda3dc5`. The report includes the
failing/passing `make clean && make` receipts, FAT/ext2 package comparisons,
and a fresh-image GUI boot that discovers and applies the shipped fonts.
Chris also reports successful 24-pixel terminal text and DejaVu Sans interface
text during hands-on use. This feedback is distinct from Quinn's guest evidence.

Chris deferred further investigation of one Workshop observation: at 1024×768,
changing the interface from Sans Book 16 to 17 pixels was refused; maximizing
and retrying Apply succeeded, and restoring appeared visually sound. Source
inspection confirms font adoption validates the current layout, whereas
Workshop's resize handler reruns layout without repeating adoption validation.
The particular refusal and its change after maximizing have not been isolated
or independently reproduced; clipping on other pages has not been ruled out.
Higher-resolution use is Chris's expectation, not evidence that the issue is
resolved. This was deferred at that checkpoint; the responsive-layout follow-up
below supersedes that deferral. No expanded diagnostic UI was requested. Other
hazards remain within the combined review scope.

The branch is local and unmerged. Three untracked root-level image backups
(`os64.img`, `ext2_test.img`, `os64_data.img`) are intentionally outside the
review and were preserved. Fable's review remains pending.

## Discovery follow-up

Chris's P5 trial exposed system fonts disappearing from Workshop after saving
personal settings and rebooting. The fix combines system and personal folders,
deduplicates byte-identical valid fonts, and preserves configured-path selection
through catalog aliases. Review the follow-up after `f9ca6ef`, including the
temporary memory bound and catalog lookup API. The
[discovery receipt](f5-evidence/discovery-union/README.md) records host regressions,
allocation-denial coverage and the guest fixture. Proportional terminal-entry
disabling remains a discussed UI improvement, not part of this correction.

## Role-fit follow-up

The post-`ce69b48` [role-fit follow-up](F5-ROLE-FIT-FOLLOWUP.md) narrows the default
height guard to initial/changed interface rows and corrects Workshop's misleading
failure message. It records a failing-before/passing-after regression, real-font
widget checks, image build and guest role changes. Full-draft publication and
custom application planners remain intact. The subsequent responsive-layout
follow-up below addresses settings-window resizing.


## Responsive settings follow-up

[F5-RESPONSIVE-LAYOUT.md](F5-RESPONSIVE-LAYOUT.md) records the larger-font
Workshop/Control Center planners, their atomic staging and dynamic minimums,
and guest evidence. Review the kernel Restore refusal as part of this slice:
a saved rectangle below the new minimum must leave the window maximized and
remain available after a later font reduction. No new syscall is introduced.

## Font diagnostic routing

[F5-FONT-LOGGING.md](F5-FONT-LOGGING.md) moves routine font diagnostics from
inherited stdout to the existing kernel-log/logd path. It includes a guest
refusal captured in logd's file with empty application stdout, alongside host
and build checks. Font adoption and retry rules are unchanged.
