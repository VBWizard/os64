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

## 2026-10-11 — Defer gclock Interface Apply investigation

Chris confirms resizing works again, but Interface font Apply leaves gclock's
text unchanged in his setup, as it did before E1K. At his request, record the
unresolved behavior in DEBTS and qualify the controlled QEMU result rather than
start another implementation task. Effective configuration, requested size and
cause remain unverified. This follow-up changes documentation only.

## 2026-10-11 — PR #257 owned-popup close escalation fix

Accepted the new P1: repeated Alt+F4 on an undrained owned popup could terminate
its task and document. The compositor now repeats close requests without
arming or taking escalation for owned popups. Windows without a popup owner
retain their existing escalation policy. Updated the ABI/design comments and
moved a misplaced task-exit comment to its actual function.

The corrected native probes reproduce SIGTERM for stalled and full-queue
owned popups before the fix, then pass without SIGTERM afterward. An ordinary
window still escalates, and the default popup suite passes 159 checks. Both
kernels were exercised in four-core QEMU with the same fixture. The initial
full-queue probe used a refused direct-focus operation; it was corrected and
both sides rerun before recording the comparison. [Evidence](evidence/e01k/README.md#pr-257-review-round-2--owned-popup-close-escalation)
includes reports, controller, serial logs and hashes. P5 revalidation and
re-review remain pending; gclock font adoption remains deferred in DEBTS.

## 2026-10-11–12 — E1B shared menus and action gallery

Implemented immutable menu models, bar/overflow, retained popup widgets, cascade
controller, optional synthetic icon provider and E1K-backed multi-window
dispatch. The gallery demonstrates keyboard/pointer/context navigation, tall
UTF-8 panels, independent action targets, visibility and hidden-bar recovery.
Dismissal covers both queue orders; replacements drain surviving queues before
handle reuse. INPUT_LOST cancels interaction; font adoption closes menus before
old text bindings are released. Application callbacks run after family teardown.

Host checks pass with ASan, UBSan and LeakSanitizer: 1,807 fake-font and 1,854
real-font menu/action/status checks, plus existing UI/editor, Scribe and font
contracts. Allocation sweeps and six behavioral mutations are covered. The
whole userland cross build passes. Isolated four-core QEMU exercises PS/2 and
USB HID, pointer dismissal/wheel/edges, font/color changes, VT and external
focus, close routing, overflow and repeated-open resource stability.
[Evidence and the P5 walkthrough](evidence/e01b/README.md) retain reports,
controllers, screenshots and hashes. P5 acceptance awaits Chris.

The branch began at PR #257 merge b36e9ddc and fast-forwarded to current
userland f7f2f508 before publication. Corrected stale E1K pending banners;
E1A/E1K are complete. Scribe/grootmenu migrations remain E1C and real icons
remain E2. No kernel work or deferred gclock investigation is included.

E1B is published as [PR #259](https://github.com/VBWizard/os64/pull/259),
against userland. Tested implementation commit: `7c6de5cf`; publication metadata
changes documentation only. Ordinary review and P5 acceptance remain pending.

## 2026-10-12 — E1B P5 report and first PR review fixes

Chris reports that the P5 walkthrough passed with no breakage. Long overflow
heading intentionally shares the tall UTF-8 fixture; active bar hover switches
top-level headings. Retrieved the supplied gallery log and retained it in
[P5 evidence](evidence/e01b/p5-validation.txt): 32 callbacks see `menus=0`, popup
pitch matches width, and exit reports zero font bytes/references. The installed
commit/hash was not supplied; this hardware report precedes the review fixes.

Addressed the five PR #259 findings: refresh closes invalid descendants and
updates help; Alt/F10 chooses an enabled heading; native destruction failures
propagate without freeing registered popup/session state; supplied bars must
belong to the owner UI; owner wheel is consumed during active menus. Close and
disposal now return a toolkit result, allowing retries before owner teardown.
Font adoption/release refuse while surviving popups cannot be destroyed.

The new navigation/ownership/wheel regressions fail before fixes and pass after.
Destruction injection tests cover rejected disposal, callback prevention,
root/child replacement, partial child-first closure, font refusal and recovery.
Final fake/real host suites pass 2,079/2,127 checks with ASan, UBSan and
LeakSanitizer enabled outside the sandbox. Real editor checks, font contracts,
six mutation gates and full userland cross build pass. Reproduction and source
hashes are retained in [review evidence](evidence/e01b/review-r1/README.md).
Reran the retained four-core QEMU controller on the review binaries: HID,
PS/2/fonts, external focus and quit modes pass. Dismissal/next-click behavior,
real-font adoption, overflow, popup-local wheel and close cleanup remain correct.
Live heap bytes/blocks return to the warm baseline after 24 more cycles; every
gallery exit reports zero font bytes/references. The E1K kernel is unchanged.
Ordinary re-review remains pending; E1C migrations and the gclock debt are untouched.

Checking the native overflow screenshots revealed that the initial evidence's
child capture did not open a child. The original trace and kernel implementation
confirm this predates review changes: extended PS/2 navigation is press-only,
but the menu held-key guard waited for a release. A targeted regression fails
before the fix. Decoded navigation now repeats while command/mode keys retain
their guard. Corrected the controller's redundant Right press and added a
child-image assertion; corrected the earlier evidence claim in the same change.

## 2026-10-12 — E1B second PR review fixes

Addressed the five round-two findings on PR #259: refresh preserves failed
teardown's ERROR over stale-action status; wheel/affordance scrolling clears
hidden selection/help; selection movement cancels pending children before the
queue drain can open them; direct hidden mnemonics highlight the overflow
affordance; standalone focus treats zero as an external destination, not owner.
The last report's nondismissal was not reproduced: the previous owner-content
branch already closed standalone menus, but incorrectly armed suppression.

Before-fix regressions reproduce the first four findings. Keyboard queue-race
coverage includes arrows, Home/End, pages and duplicate mnemonics. Final host
suites pass 2,526 fake-font / 2,574 real-font checks with ASan, UBSan and
LeakSanitizer enabled outside the sandbox; six mutation gates and full userland
cross build pass. [Second review evidence](evidence/e01b/review-r2/README.md)
records reproduction and source/binary hashes. The P5 report predates this
follow-up; ordinary re-review remains pending. Kernel/E1C/gclock scope is unchanged.

Reran four-core native HID and PS/2 checks, plus focused probes on both inputs.
Hidden Fixtures highlights overflow, entering overflow directly preserves it,
and Left opens Actions. Wheel clears selection; the first Enter invokes nothing,
then Down/Enter invokes visible Row 03 with menus closed. Broader font/overflow,
external-focus and close routing checks pass. Live heap returns to the warm
baseline after 24 more cycles; gallery traces exit with zero font bytes/references.
The focused controller's first pointer path crossed Actions and correctly
switched headings; that trial was discarded and the retained direct-entry path
was verified. No code change was made for that controller path.

## 2026-10-12 — E1B third PR review fixes

Reproduced all four new findings: registered shortcuts precede menu keys,
failed hidden-bar refresh leaves a temporary row, pending child intent survives
Escape/Left parent collapse, and Page counts enabled moves instead of model rows.
The isolated pending-collapse test reproduces UBSan's null-parent dereference.

Centralized pending cancellation in close_to and guarded panel creation against
missing parents. Menu gestures/mnemonics now precede unmodified shortcuts;
failed retained-view refresh closes/collapses temporary reveal. Page computes
one viewport's model-row target before adjusting for an enabled landing entry.
Fixed an adjacent ownership detail by assigning the session view after retain
succeeds, so failed retention cannot release a borrowed reference.

Complete fake/real suites pass 3,673/3,721 checks with ASan, UBSan and
LeakSanitizer enabled outside the sandbox; the isolated fixed collapse passes
1,258 checks. Six mutation gates and strict full userland build pass.
[Third review evidence](evidence/e01b/review-r3/README.md) records reproduction,
source/binary hashes and limits. P5 evidence remains Chris's earlier report;
ordinary re-review remains pending. No kernel, E1C or gclock debt changes.

Four-core native HID, PS/2/fonts, external-focus/cleanup and quit controllers pass.
Tall-menu End/Page Up, dismissal/next-click, real Interface adoption, overflow
child creation and close routing remain correct. Every gallery trace exits with
zero font bytes/references; live heap returns to the warm baseline after 24 more
cycles. Shortcut conflicts, stale reveal and pending-collapse races are hosted
in deterministic sanitizer tests, not claimed as induced native races.

## 2026-10-12 — E1B fourth review and invariant audit

Mapped the four findings to parent-path commitment, published bar state, input
delivery pairing and observable request outcomes. Audited sibling transitions
rather than stopping at the reported lines. Refresh uses the shared selection
transaction, top-level replacement commits after successful teardown, and
bar publication schedules damage for successful or invalidated snapshots.
A context menu no longer highlights a remembered top-level heading. External
loss and INPUT_LOST share input reset; coverage resets pairing and preserves
menus. Bar-mode/opening share target preparation and event-only close errors
reach the configured error hook.

The first regression matrix reproduces all four findings with 45 failures.
Expanded deterministic sequences observe native path/model/registration state
between events and render the owner through production dirty rectangles.
Complete fake/real suites pass 84,040/84,088 assertions with ASan, UBSan and
LeakSanitizer enabled outside the sandbox. Forty-four parameterized sequences
and 5,622 observation points are separate from those repeated assertions.
The strict userland build passes. [Audit](E01B_INVARIANTS.md) and
[fourth-review evidence](evidence/e01b/review-r4/README.md) record reproducibility
and limits. Review remains pending; the earlier P5 report is separate evidence.

Final native HID/pointer, PS/2/font/overflow, external-close and focused held-input
controllers pass. Separate down/up routing confirms the first F10 and shortcut
work after release outside the family, for both keyboards. Archived traces and
pixel checks pass; all gallery exits release font bytes/target references, and
live heap returns to the warm baseline after 24 more openings. Real-font
UI/editor (1,626), Scribe (2,470) and font contract/ABI checks pass with leak
detection. The evidence includes a focused P5 repeat for the updated build;
no new P5 deployment or result is claimed.

## 2026-10-12 — E1B fifth review: publication and viewport effects

Confirmed all four findings against `36e93f67`: deferred failure notification
preceded cleanup and hid failed teardown, bar-only refresh skipped selection
normalization, a no-op wheel closed descendants, and downward scroll allowed a
one-row final viewport. The audit extends selection eligibility after state
publication and viewport effects. Failure cleanup gives teardown precedence
before notification across deferred/root opening and preparation paths. Entry,
refresh and bar-target publication share selection normalization; an invalid
pending root is cancelled before its heading changes. A nonmutating viewport
calculation gates descendant closure, preserving live/pending paths on no-op
scrolling. Final viewport bounds use actual command/separator heights, including
selection reveal.

A scratch baseline compiles current regressions against the reviewed menu
sources and records 78 failing assertions without fixture faults. Complete
fake/real suites pass 125,601/125,649 assertions with ASan, UBSan and LeakSanitizer
enabled outside the sandbox. Fourteen deliberate hazards are detected. Tests
cover allocation/native opening failure combined with successful/refused parent
teardown, error hook observation, bar-only refresh/same-view publication,
invalid pending root, no-op/actual scroll and three mixed-height viewport sizes.
The strict userland build passes. [Fifth review evidence](evidence/e01b/review-r5/README.md)
records reproduction and manifests. Review remains pending; earlier P5 evidence
remains separate from these changes.

Final native HID and PS/2 scroll controllers preserve an open child on no-op
wheel and show a packed final viewport (rows 18–49 at the bitmap Interface size),
then invoke visible command 118 with menus closed. The standard keyboard,
pointer, font/overflow and external-close controllers also pass. Archived trace,
image and heap checks confirm zero font/target ownership at exit and stable
live heap across warm/repeated openings. A focused P5 repeat is documented;
no new hardware result or deployment is claimed.

## 2026-10-12 — E1B sixth review: binding identity and fresh candidates

Confirmed both findings against `e37cca21`: an inactive bar could bind a foreign
registry model while retaining its old view, and opening eligibility read a
preceding snapshot. Bar model binding now validates the retained view's registry
before retention/preparation/publication, matching the existing view setter.
Opening prepares a retained fresh candidate and evaluates its requested heading
or overflow group against that snapshot. Horizontal navigation passes the same
candidate into opening. The saved target is replaced after successful teardown;
declined/failed requests preserve it. Shared enabled-state lookup also keeps
reveal/reflow checks against the prepared candidate.

A regression-first check caught that refreshing a candidate sharing the live
view could invalidate its parent row during a declined request. That route now
shares explicit menu refresh's selection reconciliation, including partial
teardown failure, without querying twice. Tests check the old root model,
selection/child path and independently retained target.

The reviewed-source scratch baseline reproduces 26 failing assertions without
fixture faults. Complete fake/real suites pass 150,351/150,399 assertions with
ASan, UBSan and LeakSanitizer enabled outside the sandbox; sixteen injected
hazards are detected and the strict full userland build passes. Between-event
observation checks registry pairs as well as ownership and selections.
[Sixth review evidence](evidence/e01b/review-r6/README.md) records both snapshot
transitions through mnemonic/pointer/overflow and keyboard navigation, stale
candidates, failed teardown and the intermediate reconciliation regression.
Review remains pending. Earlier P5/editor/Scribe/font evidence is retained
separately; no additional hardware result or kernel/E1C/gclock work is claimed.

Final native HID/pointer, PS/2/font/overflow and external/close controllers pass.
Archived traces show callbacks after menu closure and zero font/target ownership
at exit; repeated openings return live heap to the warm baseline. Both new
findings and same-view snapshot interruption are deterministic host fixtures.


## 2026-10-12 — E1C production consumers

Chris confirmed E1B PR #259 merged and asked to bring the real applications
closer. E1C lives in `codex/explorer-e1c` from `88f8b672`, worktree
`.worktrees/explorer-e1c`, with uncommitted changes at this checkpoint.
grootmenu now adapts config nodes to shared menus; Scribe has shared actions,
File/Edit/View/Help, context menus and hideable menu/status bars. Existing
field editing and save/quit policies are retained. Kernel and icon artwork
remain outside this slice.

[Evidence and the P5 walkthrough](evidence/e01c/README.md) record 5,999 passing
host checks across four suites, ASan/UBSan/LSan, cross-builds and native QEMU
walkthroughs with USB HID support and PS/2 input. LSan required running the
same binaries outside the sandbox after attachment EPERM. A final host-tested
fix preserves grootmenu's parent after refused child creation; native hashes
and final hashes are recorded separately. P5 acceptance and review are pending.
The scratch VM was stopped; repository disk images were not modified.


## 2026-10-12 — E1C P5 acceptance and publication authorization

Chris verified both Scribe's menu and the desktop menu, including Alt-alone
recovery of the hidden bar and readable desktop menu text. He authorized
pushing E1C and moving to the next slice. This is user-reported hardware
acceptance without an installed-binary hash; ordinary PR review remains.
E2 icons follow: the provider/cache packet is distinct from the visual family,
and artwork should begin with a small style preview before the full catalog.


## 2026-10-12 — E1C button-row cleanup

Chris requested removing Scribe's duplicate Open/Save/Save As buttons before
review, while preserving the path textbox. The document reclaims the row;
menu actions and shortcuts retain Open, Save As and save-before-quit fields.
Scribe and menu consumer suites pass with ASan/UBSan/LSan (2,526 and 1,335
checks); cross-builds and QEMU layout/save/quit checks pass. Follow-up evidence
is in `evidence/e01c/toolbar-removal/`. Earlier P5 acceptance predates this
cleanup. E2's selected preview direction is classic colorful icons with crisp
outlines; icon work has not begun.


## 2026-10-13 — E1C first review

Chris confirmed testing the button-removal follow-up and submitted PR #260
for review. Addressed three findings: empty child cascades suppressing the
launcher, stale Scribe scrollbar ranges after chrome changes, and duplicate
startup popup error diagnostics. The consumer harness now covers each case.
Host suites pass 4,072 checks with ASan/UBSan/LSan; cross-builds and focused
QEMU checks pass. Evidence and limits: `evidence/e01c/README.md`, Review 1.


## 2026-10-13 — E1C merged; E2 icons and installable sets

E1C PR #260 passed its final review and merged as `faa321a5`. Chris approved
the generated classic colorful icon preview and authorized E2 shipment. He
added installable icon sets: an extracted folder plus configuration selects
another family without changing apps. D016 records the product choice and
engineering details.

E2 in `codex/explorer-e2` supplies a bounded PNG catalog, manifest/config
resolver, selected-to-default fallback, 26 generated source masters and 130
production exports, provenance, deterministic archive builder, menu consumers
and a larger-icon gallery. Sol handled provider, resolver and consumer work;
Quinn coordinated contracts, artwork, integration and validation. Files were
partitioned in the shared E2 worktree for this delegated packet.

Host tests pass with ASan/UBSan and leak detection enabled outside the sandbox.
FAT/ext2 recipes install byte-identical assets and final libimage. Os64's own
`gunzip | tar` successfully extracts the archive; native files match source.
QEMU verifies installed/partial sets, light/dark and selection/disabled states,
Scribe at Interface 16/32, desktop menu icons and missing-art text fallback.
[E2 evidence](evidence/e02/README.md) records exact scope and final binary hashes.
The open/stat ABI's ambiguous failed lookup is documented; no kernel change is
part of E2. Next: ordinary PR review and Chris's installed P5 acceptance.

Publication: E2A `8d01acc3` and E2B `d63809c9` are pushed;
[PR #261](https://github.com/VBWizard/os64/pull/261) is open against `userland`.
No merge has been performed. The scratch QEMU instance is stopped.


## 2026-10-13 — P5 icon delivery correction

Chris ran the larger-icon gallery on P5 and found no artwork. E2 omitted the
os64get serving/routing path despite passing disk-image packaging checks.
Added `assets/icons/default=icons` to serving examples and `@icons/*` to the
shipped routing map; this explicit whole-lot rule outranks suffix rules while
exact names retain priority. Ordinary `@lot` semantics are unchanged.
Documented first-update client bootstrap and destination creation. Corrected
the server comment that incorrectly claimed collections cannot share names,
and the guide's stale 256-file limit (the installer allows 1024).

The regression fails before the fix, passes with ASan/UBSan/LSan afterward,
and checks all 133 shipped filenames against the config. Single/batch smoke
tests and cross-build pass. Native QEMU os64get downloads 133 files with exit 0;
readback matches source bytes and the gallery renders that downloaded set.
Evidence: `evidence/e02/wire/`. Follow-up goes into PR #261; P5 retest pending.


## 2026-10-13 — P5 gallery acceptance and E2 review 1

Chris confirms the installed P5 icon gallery is visually accepted, with an
explicit thumbs-up for the disabled app icon. This user-reported acceptance
follows the os64get delivery fix `a3602b30`; no installed binary hash was
captured. It precedes this review correction.

PR #261 has one P2: transient manifest read/parse failures were collapsed into
invalid-manifest fallback, silently replacing the selected set for the app's
lifetime. Reproduced before editing production code. The manifest helper now
returns a typed result; creation preserves NO_MEMORY and detected I/O failures
for both default and selected manifests, and for icons.conf. The output stays
NULL with temporary state cleaned up; retry can recover the selected family.
Selected catalog construction failures propagate too. Missing/invalid
manifest fallback remains; failed open plus failed stat retains the existing
ABI ambiguity documented by E2. The host allocation sweep covers real config
and manifest parser buffers, and selected/default read faults exercise retry.

Review validation: both host suites pass ASan/UBSan/LSan; affected library and
consumers cross-build. Native QEMU gallery smoke check passes with the revised
library and downloaded set; the staged library matches its recorded hash.
The scratch VM is stopped. Receipts: `evidence/e02/review-r1/`.


## 2026-10-13 — E1B retrospective and E3 design packet

Chris requested an audit of Sol's E1B PR #259 after six finding-bearing rounds.
The review record contains 24 findings: 20 inline and four in a conversation
comment, distributed 5/5/4/4/4/2, followed by a clean round. Several later
findings revisited the same invariant through a different path or exposed an
interaction introduced by a fix. Existing extensive tests were real evidence,
but final-state assertions and simplified painting missed intermediate-state
and production-rendering obligations. The standalone-focus report also needed
reproduction analysis; not every reported trigger was confirmed as written.

Chris authorized applying the lessons to the next design and Sol handoff.
E3 now specifies immutable ID-based data/projections, separate focus/selection/
anchor, exact range/filter/sort behavior, transactional publication, bounded
visible-row resources, explicit resize failure/retry and callback obligations.
The handoff supplies state examples, an invariant-to-entry-point map, interrupted
input and paired-failure cases, production rendering tests, performance measures
and separate host/QEMU/P5 acceptance. No E3 implementation started.

E2 PR #261 is verified merged as `f39b0ae0`; the E3 design worktree/branch is
`.worktrees/explorer-e3` / `codex/explorer-e3-design`, based on that commit.
Updated E2 and central status without extending Chris's earlier P5 report to the
later manifest fix. Reconciled the umbrella's generic selection ownership and
stale next-step text. D017 records engineering choices and their authority.
The next action is assignment of the E3 packet to Sol, not a kernel or IPC task.

Design validation: local Markdown file targets and whitespace checks pass.
The stale-reference scan reports live E1B/E1K/icon documentation paths and their
historical audit receipts; inspected hits remain valid. No code identifiers or
files were retired. Reviewed selection examples and failure transitions against
the current input/font APIs. Runtime tests are not applicable to this docs-only
packet and remain unrun for E3. The packet is published on its design branch;
no separate PR or merge, allowing the implementation PR to carry the design.


## 2026-10-13 — E3A/E3B implementation

Chris assigned E3 from `origin/codex/explorer-e3-design` in a fresh worktree.
Quinn implemented copied domain-scoped data, indexed projections, typed stable
sorting and detached selection in `ee24fb64`, then the transactional Details
widget/gallery in `8b5d13a7`. The branch is `codex/explorer-e3-implementation`;
the worktree is `.worktrees/explorer-e3-implementation`. No kernel or ABI source
was changed, and existing worktrees were retained.

The real-font/renderer suite passes 14,875 checks, including 251 replacement
and 204 font preparation failure points, complete tuple/pixel checks, paired
failures, interrupted input, callback guards and lifecycle/resource bounds.
The detached core passes 276 checks; existing menu and font/listbox suites and
real PNG/theme checks pass. ASan/UBSan/LeakSanitizer remain enabled; sandbox
thread attachment EPERM with TracerPid 0 was diagnosed and runs passed outside
it. Font-error notices are deferred until a dispatch boundary; model/layout
publication preserves held-key guards while input loss resets them.

Normal FAT/ext2 image recipes and guest-read library/gallery hashes agree.
Final QEMU source `8b5d13a7` exercises 0/1/10000/100000 rows, sort/filter/refresh/
reset, selection/typeahead/activation, columns, an independent second view,
Interface 32, light/dark appearance, resize and existing gallery modes. Its
100 ordinary events after 10 warmups measure median/p95/max 46.194/48.486/49.554
ms under TCG. Host p95 is 1.947 ms. Neither is claimed as P5 latency.

[E3 evidence](evidence/e03/README.md) records commands, source/installed hashes,
logs, screenshots and the invariant map. Independent review and Chris's P5
HID/performance walkthrough remain pending. E3 is implemented, not accepted;
E4 follows accepted E3.

Published as [draft PR #264](https://github.com/VBWizard/os64/pull/264) against `userland`. The source remains `8b5d13a7`;
the publication follow-up changes documentation links only.


## 2026-10-13 — E3 clock diagnostic clarification

Chris requested that “clock unavailable/unqueried” distinguish an unqueried
clock from a failed query, and asked whether typing extends the search string.
Added last-query state and gallery status refresh for no-match queries. The
host regression verifies growing-prefix narrowing from Alpine to Alpha plus
initial/failure/recovery diagnostics through the production dispatcher.
ASan/UBSan/LeakSanitizer passes 14,948 checks; the library/gallery cross-build
passes. The four-file `/tmp/e3-wire` acceptance bundle is refreshed; hashes and
[the follow-up receipt](evidence/e03/README.md#clock-diagnostic-follow-up--2026-10-13)
record its scope. Earlier QEMU/image/guest receipts remain for `8b5d13a7`; no
P5 follow-up install or hardware acceptance is claimed.


## 2026-10-13 — E3 synthetic filenames use underscores

Chris requested underscore examples after a search containing a space ended
its prefix. Updated synthetic filenames, including long-name suffixes, and the
P5 acceptance example. The production typeahead regression verifies
`Beta_000002` moves to `Beta_000010` on the final `1` in `beta_00001` and matches
the full filename after another `0`. ASan/UBSan/LeakSanitizer passes 14,975
checks and the gallery cross-build passes. The `/tmp/e3-wire` gallery is
refreshed; [follow-up evidence](evidence/e03/README.md#underscore-fixture-follow-up--2026-10-13)
records hashes and scope. Space handling is unchanged; P5 acceptance is pending.


## 2026-10-13 — E4 creates Roam

Chris authorized the first real browser after E3 merged and explicitly reserved
file associations for discussion before E5. Quinn integrated the E4 contract,
Sol's directory/controller and reusable UI/application packets, and focused
E3 selection-clone/view-restore additions. Source `243663dc` is on
`codex/roam-e4` in `.worktrees/roam-e4`, based on `5a521b63`.

Roam provides asynchronous read-only directory browsing, Details/icons,
navigation/history, address/breadcrumbs, filtering, favorites/volumes and
independent pane visibility including menu recovery. No kernel changes or
file-launch policy were introduced. Ctrl+[ / Ctrl+] avoid Os64's reserved
Alt+Left/Right VT bindings. Native integration caught reserved shortcut
registration, a menu-owned dispatcher hook collision, and a missed shortcut
release across address focus; fixes have host regressions. Font layout and
failed-navigation address publication were checked against actual rendering.

The full userland build and directory/controller suites pass. The real-font
Details suite passes 20,713 checks; Roam composition passes 60. ASan/UBSan and
LeakSanitizer remain enabled; attachment-denied sandbox runs were repeated
outside it. The final QEMU walkthrough uses real FAT/ext2 directories, a
10,001-file fixture, history/selection/scroll/error/filter tests, hide-all/menu
recovery, and live font/palette changes. [Evidence](evidence/e04/README.md)
preserves screenshots, source/package identification and delivery results.
Independent review and P5 acceptance remain pending; [the handoff](E04_HANDOFF.md)
is the next action. Discuss file associations before beginning E5.

Published as [draft PR #267](https://github.com/VBWizard/os64/pull/267) against `userland`,
with source `243663dc` and native evidence `f930fda3`. The publication follow-up
adds links only; the implementation is not merged.


## 2026-10-13 — E4 first review corrections

Addressed PR #267's three findings: pool-owned input lifetime after worker
failure, failed-create attachment ownership, and transactional breadcrumb
geometry with retry. The controller regression reproduced the original
use-after-free, then passed with ASan/UBSan/LeakSanitizer. The real-font Roam
suite passes 2,283 checks, including 60 allocation failures and recovery;
the userland build and stale-reference checks pass. The follow-up evidence
is recorded in [E4 validation](evidence/e04/README.md#pr-267--first-review-follow-up).
Chris also reported more than ten minutes of successful hands-on browsing and
hide-all minimalist mode; the machine was not specified. File associations
remain reserved for discussion before E5.


## 2026-10-13 — E4 second review corrections

Addressed PR #267's four findings: active-view accounting against the saved
history budget, lost help tokens on replacement failure, ignored Details
layout errors, and rejected filter text left visible. Layout collects geometry
before preparing Details, preserves prior bounds on refusal, and retries
through sync. The earlier comment about refused runs preserving hit boxes
was incomplete because it ignored Details preparation; the code and comment
now cover that child. Controller tests and 3,931 real-font UI checks pass with
ASan/UBSan/LeakSanitizer; the full userland build and stale-reference checks
pass. [Review evidence](evidence/e04/README.md#pr-267--second-review-follow-up)
records the allocation sweeps and remaining native-test scope.


## 2026-10-20 — Scribe uses the shared file dialogs

Chris clarified that the shared Open/Save dialogs were already implemented
in `42410e34`; he requested Scribe as their first app consumer before Yonder.
He settled association storage: Roam configuration selects the file path.
D019 records that instruction without conflating it with picker integration.

Quinn implemented the Scribe consumer on `codex/scribe-file-dialogs`, based on
`aa04123c` plus the dialog branch. Open, Save As, unnamed Save and choosing a
save-before-quit destination use the picker. Pending choice disables editing;
Open asks about unsaved changes. Cancel and failed I/O preserve the document.
Save without replacement consent creates exclusively. Strict picker Open
preserves the buffer if a selected file disappears; CLI new-file startup stays
available. Dialog cleanup precedes the parent and its borrowed resources.

[E5 evidence](evidence/e05/README.md) records 2,584 Scribe, 1,655 menu and 1,928
shared-dialog checks, all passing with ASan/UBSan/LeakSanitizer outside the
sandbox after diagnosed attachment EPERM. Cross-build and final-build QEMU
round trips passed; saved file bytes and the installed binary were verified
offline. The delivery bundle is `/tmp/scribe-dialog-wire`.

The old E4 review-pending status was stale: PR #267 is merged as `a3dd4b65`.
The tracker and handoff now point to E5. Review and Chris's P5 walkthrough
remain pending; no hardware acceptance is claimed. Yonder and associations
remain separate implementation work.

Published implementation `29252438` as [draft PR #319](https://github.com/VBWizard/os64/pull/319)
against `userland`, including the shared-dialog dependency. Review/P5 remain
pending; the branch has not been merged.
