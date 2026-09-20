# Combined font implementation review

Please review `codex/font-settings` in `.worktrees/font-settings`. Chris asked
Quinn to finish the integrated feature before returning it for Fable's review.
This request includes F3 and F5 implementation review; no F3 approval receipt
was found and none is inferred. F4 has Quinn's accepted implementation review.
No mainline merge or publication is implied by this development branch.

Start with [F5-REPORT.md](F5-REPORT.md) for the final behavior and evidence,
[FONT_SETTINGS.md](../../FONT_SETTINGS.md) for implementation decisions, and
[FONTS-OVERVIEW.md](FONTS-OVERVIEW.md) for the user-facing explanation.

## Scope and provenance

- Frozen F0: `23bf6dd` and [FONT_CONTRACTS.md](../../FONT_CONTRACTS.md).
- F2.5 provider: `788de99`, [FONT_PROVIDER.md](../../FONT_PROVIDER.md).
- F3: `aa8b7a2`, report `ab7a1cd`, [F3-REPORT.md](F3-REPORT.md).
- Accepted F4 plus independent receipt: `3bf7b1f`,
  [F4-QUINN-REVIEW.md](F4-QUINN-REVIEW.md) and `f4-evidence/c3-review-r2/`.
- F5 implementation: `419b36d`, clean image build fix `bda3dc5`, integration
  parent `07d8a69`. Review `git diff 07d8a69..HEAD` on this branch for settings
  and packaging, and inspect the F3 change separately.
  The report and evidence checksums identify the validation artifacts.

The backend/provider boundaries remain unchanged. The responsive-layout
follow-up changes the existing kernel Restore path; no syscall or ABI is added. Historical checkpoint reports are receipts for those
checkpoints, not claims about the integrated production selector.

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
resolved. This was deferred at that checkpoint; the responsive-layout follow-up below
supersedes that deferral. No expanded diagnostic UI was requested. Other hazards remain within the combined review scope.

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
