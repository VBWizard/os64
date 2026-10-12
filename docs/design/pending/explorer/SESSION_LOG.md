# Explorer session history

Append entries for work sessions or substantial handoffs. Record outcomes and
evidence, not a transcript. The board and latest handoff describe current state.
Two entries on one date may describe different work periods in the same chat.

## 2026-10-09 — Umbrella design (Quinn)

- **Scope:** Plan the native file explorer and reusable platform components.
- **Result:** 15 delivery slices, shared component boundaries, IPC comparison,
  transfer-lifetime/correctness requirements and two stored concept images.
- **Branch / commit:** `codex/explorer-design`, `337f4744`, based on `f3f5f50e`.
  Local commit; no PR or merge.
- **Evidence:** Repository API inspection; linked repository paths and image
  files checked; PNG headers/dimensions checked; whitespace and
  `tools/stale_refs.sh` passed. No runtime changes or runtime tests.
- **Decisions:** Details-first browser, early reusable picker integration and
  staged delivery. Native local message channels proposed for E6, not implemented.
- **Next:** E1 interaction/API design before the menu/status gallery.

## 2026-10-09 — Multi-session organization (Quinn)

- **Request:** Chris confirmed File Open/File Save reuse and required structured
  progress and decisions across sessions, including potential development by Opus.
- **Result:** Added the project entry point and slice board, decision log, current
  handoff, slice-record template and this history. Linked them from the umbrella.
- **Branch / commit:** `codex/explorer-design`; the commit containing this entry.
  Local only; no PR or merge. No slice implementation started.
- **Decisions:** D001 and D006 record the explicit requirements. Other entries
  distinguish product requirements, Quinn's selected plan and unresolved proposals.
- **Evidence:** Relative document links/anchors and referenced files checked;
  all 15 slice IDs present once in the board; whitespace and stale-reference
  checks passed. No runtime tests apply to this documentation-only change.
- **Next:** Create E1's record, inspect menu/focus/action consumers and prepare
  its design discussion. Quinn owns that design; Opus has not been assigned work.

## 2026-10-11 — Application name confirmed (Quinn)

- **Decision:** Chris confirmed Roam; D007 resolves the open naming choice O001.
- **Result:** Updated the umbrella design, decision log, tracker and handoff.
- **Branch:** `codex/explorer-design`; local documentation changes.
- **Evidence:** Documentation diff and whitespace checked; no runtime changes.
- **Next:** E1 interaction/API design remains the resume point.

## 2026-10-11 — Independently hideable interface (Quinn)

- **Decision:** D008 records Chris's requirement for independent menu-controlled
  visibility of interface components, including the menu bar itself.
- **Result:** Added bare-directory and directory/address/menu acceptance layouts
  to the umbrella; updated tracker and handoff. Xfe is a simplicity reference.
- **Open:** Hidden-menu recovery gestures, persistence and visibility scope.
- **Branch:** `codex/explorer-design`; local documentation changes.
- **Evidence:** Documentation diff and whitespace checked; no runtime changes.
- **Next:** Discuss recovery and shared visibility actions during E1 design.

## 2026-10-11 — Shared popup menus and recovery agreement (Quinn)

- **Decisions:** Chris accepted the proposed menu recovery gestures. D009 adds
  a reusable right-click menu widget and grootmenu integration to E1.
- **Evidence:** Read grootmenu's row painting, sizing and appearance handling,
  menu.h and libui font contracts on userland at `6d988565`. The launcher uses
  the bitmap painter and private popup logic; the shared menu API parses config.
- **Result:** Updated umbrella, decisions, tracker and handoff. Documentation
  diff/whitespace checked; no runtime changes or runtime tests.
- **Branch:** `codex/explorer-design`; updates remain local and uncommitted.
- **Next:** Write E1's implementation contract for shared actions, popup/menu-bar
  widgets and status bar, including grootmenu migration and acceptance checks.

## 2026-10-11 — Icons as a shared foundation (Quinn)

- **Decision:** D010 records Chris's requirement for icons, an initial asset
  family and support reusable by future desktop icons.
- **Result:** E1 accounts for icon references and layout; E2 owns the initial
  assets and shared rendering, with small and larger examples in its gallery.
  Updated umbrella, decisions, tracker and handoff.
- **Evidence:** Documentation diff/whitespace checked; no assets or runtime
  code changed. Visual style and concrete API contracts remain design work.
- **Branch:** `codex/explorer-design`; local, uncommitted documentation changes.
- **Next:** Include icon layout and the E2 boundary in the detailed E1 design.

## 2026-10-11 — Slice boundaries (Quinn)

- **Decision:** D011 records Chris's preference for logical slices, which may
  be substantial, with focused kernel-touching work.
- **Result:** Clarified roadmap boundaries, kernel validation and separate
  filesystem prerequisites; updated decisions, tracker and handoff.
- **Evidence:** Documentation diff/whitespace checked; no runtime changes.
- **Branch:** `codex/explorer-design`; local, uncommitted documentation changes.
- **Next:** Apply these boundaries to the E1 implementation contract and later
  kernel foundation designs.

## 2026-10-11 — Channel design and architectural review boundary (Quinn)

- **Decision:** D012 records Chris's request for a standalone message-channel
  design reviewed by Fable, plus Fable review for serious kernel changes.
- **Boundary:** Conventional syscall additions following established contracts
  retain normal review/testing without an automatic extra Fable requirement.
- **Result:** Updated umbrella, decisions, tracker, handoff and slice template;
  no channel design or Fable review claimed complete.
- **Evidence:** Documentation diff/whitespace checked; no runtime changes.
- **Branch:** `codex/explorer-design`; local, uncommitted documentation changes.
- **Next:** Continue E1 design; prepare the standalone channel design before E6
  implementation and route it for the required architectural review.

## 2026-10-11 — Detailed foundation designs and native-popup choice (Quinn)

- **Scope:** Turn the agreed product direction into concrete foundation designs
  and an independently usable implementation handoff.
- **Decision:** Chris selected native popups extending beyond their owner window
  with a focused GUI prerequisite for Fable review; recorded as D013.
- **Documents:** E01_MENUS.md defines shared actions, per-target state views,
  menus/status, icon integration and grootmenu/Scribe migration. E01A_HANDOFF.md
  is Ready for userland implementation. GUI_POPUPS.md is the separate E1K
  architecture proposal awaiting Fable. E02_ICONS.md defines provider/asset
  boundaries and future desktop reuse; visual choice remains open.
- **Source evidence:** Audited current userland `6d988565`; relevant GUI/libui,
  grootmenu and Scribe sources match the design baseline `f3f5f50e`. Confirmed
  private bitmap menu rendering, decoder/library dependency direction,
  single-window GUI waits, same-task-only focus information, the 32-window
  handle table and canvas cleanup/locking caveats. These constrain the designs;
  no runtime fixes are claimed.
- **Allocation evidence:** libpng streams IDAT and allocates an output plane,
  two filtered rows and one inflater. A compile-only host probe that includes
  `userland/libgzip/inflate.c` and declares a byte array of
  `sizeof(struct os64_inflate)` produced symbol size `0x88c8` (35,016 bytes).
  E2 records the resulting 117,962-byte maximum requested load payload at its
  64 KiB encoded / 64×64 decoded caps, excluding allocator overhead. Runtime
  instrumentation remains required before claiming an aggregate hard cap.
- **Self-review corrections:** State snapshots belong to target views, not a
  global command; avoid circular libos64/image dependencies; candidate geometry
  needs publication/input epochs; preserve actual GUI-lock → signal-lock cleanup;
  budget 16-bit PNG rows; use immediate submenu hover without inventing a timed
  wake facility in the userland slice.
- **Verification:** Whitespace/diff check and local-link/fence/record validation
  passed (10 documents, 45 local links, 15 top-level slices, D001–D013). The
  stale-reference check returned prose-only candidates for `../EXPLORER.md`,
  `E01_MENUS`, `SESSION_LOG` and `SLICE_TEMPLATE`; every reported reference was
  read and points to a retained/new document, not a retired symbol. No added
  superlative comments were reported. No runtime, QEMU or P5 tests apply to the
  documentation-only changes.
- **Branch/publication:** `codex/explorer-design`; local checkpoint, no PR/merge.
  Main checkout and installed files remain unchanged.
- **Next:** Hand E1A to its implementer; route E1K for Fable architectural review;
  choose E2's initial visual family with Chris. E6 message channels still need
  their own standalone design and review later.

## 2026-10-11 — Fable review disposition and smaller popup foundation (Quinn)

- **Request:** Chris reported Sol working on E1A and delegated disposition of
  Fable's adjacent GUI_POPUPS_REVIEW.md feedback to Quinn.
- **Decision:** D014 selects focus-taking owned popup windows, any-window wait,
  content readback and birth-size popup capacity. Remove the kernel menu-session
  transaction design. E1B closes menus before Interface font adoption. Correct
  the draft's VT rule: a VT switch alone preserves the popup.
- **Review record:** Preserve Fable's original review unchanged; add
  GUI_POPUPS_DISPOSITION.md with point-by-point decisions and the Sol handoff.
  Rewrite GUI_POPUPS.md and synchronize E1, umbrella, board and current handoff.
- **Independent checks:** Read focus/create/destroy, waiter registration,
  capacity allocation/clamping, event queue, pointer routing and decoration
  updates on `6d988565`. Confirmed that focus insertion evicts the oldest event,
  ordinary edges can drop, create focuses before attach could run, and appearance
  can change geometry without an owner press. Read the explicit VT ruling in
  DEBTS.md. Existing heap reservation sites remain a separate concurrency fix.
- **Remaining review:** R1 safe initial focus and delayed-event identity;
  R2 shared input-loss/reset behavior and dismissing-click ordering. These are
  refinements beyond the received review, not approved by it. The full native
  packet is not Ready. Wait/capacity work has separable reviewed directions.
  No follow-up was sent to Fable by this session.
- **E1A:** Sol's implementation is underway in `.worktrees/explorer-e01a`,
  branch `codex/explorer-e01a`. Action/status requirements are unchanged. Read
  status only; no edits or implementation review in that worktree. E2 icon
  contracts and visual-choice status are unchanged.
- **Validation:** Local document check passed for 12 Markdown documents,
  63 local links/anchors, balanced fences, D001–D014 and E1–E15. `git diff --check`
  passed. `tools/stale_refs.sh` returned 1 for prose-reference candidates:
  retained document/source paths and short names for existing focus, ownership
  error and wait APIs. Read every reported hit; none names a deleted source
  artifact. No new superlative comments. Historical entries and the preserved
  review intentionally name the superseded design. No runtime tests apply to
  this documentation-only change.
- **Publication:** Local checkpoint on `codex/explorer-design`; no push, PR,
  merge or deployment. Main checkout and Sol's working files remain untouched.


## 2026-10-11 — E1A implementation and acceptance

Sol implemented E1A on `codex/explorer-e01a` in `.worktrees/explorer-e01a`, based
on current `userland` at `6d988565`. No relevant source drift was present.
Design documentation was reconciled from `d33e651e` without merging its older
runtime base. The action/status contract remained unchanged after Fable's E1K
review. The active design worktree and unrelated main-checkout work were not edited.

Delivered copied immutable registry descriptors, retained target views, bounded
atomic snapshots and validation, tracked button bindings, explicit shortcut
routing and an independent transactional status widget. The real `actiongallery`
uses distinct target and application contexts, view toggles, status token
producers and deferred close. No native menu stubs or kernel changes were added.
The current UI Western text policy is shared by action/status input validation.
PS/2 escape bursts remain with the existing decoder; printable keypad input is
handled separately from their overlapping physical codes. F10 recognition has
no menu reveal behavior. A pre-existing Scribe fixture helper-name collision was
isolated in the host include, leaving production Scribe unchanged.

Actual host results: 231 focused fake-font checks, 278 real-font checks,
148/1626 existing UI checks and 2470 Scribe checks passed, plus font contracts
and strict cross build. ASan/UBSan and leak detection remained enabled. Sandbox
LSan attachment failed with EPERM and TracerPid 0; outside-sandbox runs passed.
QEMU showed button/shortcut invocation once, independent and stale targets,
focused-panel hiding, help-token ownership, persistent notice priority, live
32px Interface reflow, stable warmed heap usage across another twenty rounds
and exit code zero. USB input was tested with the keyboard attached at boot;
a preliminary hotplug attempt was not usable and prompted no kernel changes.

[Durable evidence](evidence/e01a/README.md) contains commands, logs, screenshots,
heap reports and source/binary hashes. Files remain local and uncommitted under
the QEMU testing guide's no-commit instruction. No PR, independent ordinary
review, merge, native-menu acceptance or P5 deployment is claimed. Next action:
ordinary E1A code review and findings, with E1K/E1B/E1C continuing separately.


## 2026-10-11 — Chris's OS checkout and publication request

Chris confirmed the gallery worked perfectly in his OS checkout and explicitly
requested commit and push for review. Hardware and installed revision were not
reported; the confirmation is recorded separately from automated QEMU evidence.
This request supersedes the QEMU testing guide's earlier no-commit instruction.
The implementation is prepared for publication on `codex/explorer-e01a` against
`userland`, with ordinary review and merge pending. No production code changed
after the recorded host/QEMU checks; source/binary hashes were revalidated.

Publication completed: implementation commit `bd86ce4f` was pushed to
`origin/codex/explorer-e01a`; [PR #254](https://github.com/VBWizard/os64/pull/254)
is open against `userland`. The worktree is awaiting ordinary review; no merge
or review approval is claimed. A documentation follow-up records this publication.


## 2026-10-11 — E1A first review fixes

Chris requested the two findings on PR #254 be addressed. Against reviewed
commit `e0800cb8`, new host regressions reproduced both: Ctrl punctuation
bytes 27–31 were accepted as registrations but did not route; positive remaining
status width could show a clipped help fragment beside a persistent notice.
The before-fix harness exited 1 with 54 failures across 313 checks.

The matcher now unfolds character bytes 1–31, preserving physical Escape/editing
identities and rejecting bursts first. Status paint measures the secondary run
and suppresses it unless the complete message fits, retaining text/tokens.
The former paint comment described leftover clipping and missed the required
complete-fit rule; it was corrected with the code and public API documentation.
Both help and base-information sites use the shared fit gate. No decoder, kernel
or production consumer policy changed.

Focused suites passed with 387 fake-font and 436 real-font checks; ASan, UBSan
and supported leak detection remained enabled outside the sandbox. Strict cross
build passed. QEMU at the live 32px Interface face showed the notice alone, then
restored full retained help when the notice was cleared; gallery exit was zero.
[The R1 packet](evidence/e01a/review-r1/README.md) retains logs, screenshots and
source/binary hashes. Next action: push the review revision and ordinary re-review.

## 2026-10-11 — E1A hidden status row review fix

Chris reported another PR #254 finding. The gallery font planner counted the
status row even while hidden, refusing candidate fonts whose visible controls
fit. New regressions against `63f1211c` reproduced the refusal with the target
panel shown and hidden. The planner now budgets the status row according to
its actual visibility; retained hidden messages still participate in font
preparation. Tests derive boundaries from staged production geometry and check
refusal rollback, exact-fit adoption, message/token retention and showing the
status again. Fake/real focused suites passed 428/475 checks with ASan, UBSan
and LeakSanitizer enabled outside the sandbox. The gallery cross build passed.
[Second-review packet](evidence/e01a/review-r2/README.md) records commands,
logs and source/binary hashes; PR re-review is pending.

## 2026-10-11 — E1A binding teardown contract correction

Chris reported the public header's reversed teardown order on PR #254. That
comment was wrong: unbind dereferences live button, binding and UI storage.
The header now requires deferred teardown after dispatch, unbinding before
view/registry destruction and widget storage disposal. The handoff's related
paragraph uses the same sequence and distinguishes binding references from
the temporary invocation reference. No runtime code changed. Existing lifetime
and cleanup checks passed again: 428 fake-font and 475 real-font checks, with
ASan, UBSan and LeakSanitizer enabled outside the sandbox. Runtime hashes match
the R2 packet; [R3 evidence](evidence/e01a/review-r3/README.md) records the reused
host binaries and logs. Ordinary re-review is pending.

## 2026-10-11 — Gallery classified as an interactive test

Chris requested moving actiongallery into tests and pushing directly to userland.
The source now lives under `userland/tests/actiongallery`; automatic Makefile
discovery produces `userland/bin/tests/actiongallery` for installation to
`/tests/actiongallery`. The host harness includes the new source path, and the
window title and heading identify it as a test. The obsolete local
`userland/bin/actiongallery` output was removed. Historical evidence retains the
commands used for those revisions; the current handoff gives the new launch path.

The strict cross build passed at the new output path. The focused host suites
passed 428 fake-font and 475 real-font checks with ASan, UBSan and leak detection
enabled. Both sandbox runs encountered LeakSanitizer thread-attachment EPERM;
the same compiled binaries passed outside the sandbox with leak checking enabled.
No P5 deployment or additional guest test was performed for this relocation.

## 2026-10-11 — E1A accepted; E1K implementation and evidence (Quinn)

Chris confirmed the P5 test complete after gallery relocation. E1A is accepted:
PR #254 merged as `c6cc6e3c`, followed by `2bc9f2f4`. Hardware acceptance is
user-reported; no installed-binary hash was captured.

Chris authorized the next E1K work. Fable's second review is preserved beside
the design; D015 and E01K_HANDOFF.md record the selected contract. Quinn added
owned popup creation with locked identity/focus revalidation, conditional return,
child-destroy BUSY, task-family retirement, focus peer handles, any-window wait
with shared waiter-conflict rules, content readback, birth-size popup buffers,
ancestor chrome and popup exclusion from Alt+Tab. INPUT_LOST discards unsafe
queued gestures, and libui/action routing reset interaction state.

The [E1K evidence packet](evidence/e01k/README.md) records cross builds, queue
wrap/drop/eviction tests, 495/542 fake/real action checks, appearance regressions,
146 guest checks on four cores, existing focus/doorbell regressions and native
panel/Alt+Tab/outside-click screenshots. ASan/UBSan/LeakSanitizer remained enabled
on supported runs outside the sandbox. The fixture's initial signal-sender
stall was corrected using a separate sending process; no speculative kernel
change was made. Known coverage limits are explicit in the evidence packet.

Working branch: `codex/explorer-e1k`, base `2bc9f2f4`, isolated checkout
`.worktrees/explorer-e1k`. Changes are uncommitted for ordinary review, consistent
with the QEMU testing guide's reporting instruction. No P5 deployment occurred.
Chris asked whether to assign E1B to Sol; the ordering remains E1K validation and
review, then E1B menus, then E1C consumer migration. E2 visuals and E6's separately
reviewed channel design remain independent future work.

## 2026-10-11 — QEMU checkout exposes popuptest readiness assumption

Chris reported one line-55 failure in five QEMU runs, followed by four passing
runs; this was explicitly not P5 testing. The fixture incorrectly required B
on the next any-window readiness return. A deliberately queued A notification
reproduced the same assertion failure on the existing kernel.

The diagnostic now consumes unrelated events and verifies the target doorbell;
notification waits have a watchdog. Sibling readiness assertions and the
competing-waiter startup race were corrected without changing the kernel.
Ten consecutive four-core QEMU runs passed 153 checks apiece, zero failures and
exit 0, with deterministic unrelated readiness in each run. The
[regression packet](evidence/e01k/wake-regression/README.md) preserves the failing
reproducer, results and hashes. The exact unsolicited event in Chris's original
run was not logged and cannot be identified retrospectively. P5 acceptance and
ordinary code review remain pending.

## 2026-10-11 — Corrected E1K diagnostic validated in QEMU and on P5

Chris confirmed validation in both QEMU and on the P5 after the readiness-test
fix, and confirmed the startup explanation makes the diagnostic's purpose clear.
Record this as user-reported acceptance; no additional run counts, logs or
installed-binary hashes were supplied. The earlier five-run failure report
remains QEMU-only history. Current handoff and evidence summaries now reflect
the subsequent P5 validation. Ordinary code review remains the next E1K step
before Sol starts E1B; this update changes documentation only.

## 2026-10-11 — E1K pushed for ordinary review

Chris requested commit, push and a PR. Implementation commit `3fee1034` is
published on `codex/explorer-e1k`; [PR #257](https://github.com/VBWizard/os64/pull/257)
targets `userland`. The base still matched current `userland` at `2bc9f2f4`, and
the latest tested source/binary hashes matched before publication. Evidence logs
were included explicitly despite the repository's general log ignore rule;
trailing whitespace in retained logs was stripped for repository checks.
Local documentation links and the staged whitespace check passed. The retirement
scan was inspected: remaining references were live document/ABI names or deliberate
historical discussion; no new superlative comment claims were found.

No runtime changes or additional test runs were needed for publication. Chris's
QEMU/P5 acceptance and the ten 153-check QEMU runs remain the current evidence.
Ordinary code review is pending; E1B remains the dependent next packet.

## 2026-10-11 — PR #257 first review fixes

Accepted all three findings: preserve resizable borderless/pinned consumers by
making birth-size capacity an explicit create hint; give retained controls a
turn between repeated loss notifications; reset custom interaction state in
gterm and sibling consumers. The earlier claim that borderless + pinned alone
was a sufficient capacity classification was wrong: gclock can grow on an
Interface font change and via the WM resize chord.

Updated the ABI/design handoff and added regressions. Host queue/appearance/
consumer and Yonder tests pass with ASan, UBSan and leak detection; two QEMU
runs pass 159 checks each. The same pinned/borderless gclock process follows a
16-to-32px Interface change and then resizes through Ctrl+Alt+right-drag.
[Round-1 evidence](evidence/e01k/README.md#pr-257-review-round-1--2026-10-11)
records results, screenshots, limits and build hashes. P5 revalidation and
ordinary re-review remain pending.
