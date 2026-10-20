# Explorer decision log

Use stable IDs. Distinguish customer requirements from selected engineering
directions and unresolved proposals. Each new entry records date, source/owner,
rationale, affected slices and a link to the governing design when available.
When a decision changes, mark it superseded and link its replacement; update
the current design as well. Do not leave readers to reconstruct current behavior
from a chain of historical entries.

## D001 — Shared directory browser for Open and Save dialogs

- **Status:** Accepted customer requirement, 2026-10-09.
- **Source:** Chris explicitly confirmed that the directory browser should form
  the basis of File Open and File Save dialogs and that both are needed.
- **Decision:** Share the directory model and browser composition. Picker modes
  add selection/filename validation and completion, while the calling application
  owns opening or saving the document. Scribe is the first planned picker consumer.
- **Reason:** Reuse navigation, sorting, selection and directory-loading behavior
  without duplicating a miniature explorer in each app.
- **Affected:** E4/E5; [component boundaries](../EXPLORER.md#3-component-and-process-boundaries).

## D002 — Reusable controls and higher-level components

- **Status:** Accepted customer requirement, 2026-10-09.
- **Source:** Chris requested shared components wherever reuse makes sense,
  including above the basic-widget level.
- **Decision:** Put general controls and actions in the toolkit; share the browser,
  picker, operation engine and transfer presentation at their respective layers.
  Identify concrete consumers and exercise those boundaries during integration.
- **Reason:** Explorer should improve the platform's application-building tools.
  A private collection of Explorer controls would duplicate work for later apps.
- **Affected:** E1–E15; [component boundaries](../EXPLORER.md#3-component-and-process-boundaries).

## D003 — Transfer lifetime independent of Explorer

- **Status:** Accepted product direction, 2026-10-09.
- **Source:** Chris proposed a separate copy application and endorsed Explorer
  following another application's transfer progress.
- **Decision:** Accepted transfers survive closing Explorer or its progress UI.
  Offer an attached display and a separate window over shared progress components.
  Explicit cancellation is distinct from closing a viewer.
- **Reason:** Browsing and transfer execution should fail or close independently.
- **Affected:** E6–E9; [transfer contract](../EXPLORER.md#5-transfer-ownership-and-correctness).
- **Still to settle:** The precise worker/service lifecycle and authority model
  remain engineering design work, not an approved ABI.

## D004 — Default app communication through local message channels

- **Status:** Proposed engineering direction, 2026-10-09; not ABI-approved.
- **Owner:** Quinn. Chris requested evaluation of a main app communication method.
- **Proposal:** Connected local message channels for app control and notifications,
  with bounded queues, caller patience and reconnectable local endpoints. Retain
  pipes for streams and consider shared memory when a bulk-data consumer needs it.
- **Reason:** Transfer viewers need connection lifetime, discovery and coherent
  state updates. Anonymous parent/child pipes alone do not supply reconnection.
- **Affected:** E6/E8/E9; [comparison and requirements](../EXPLORER.md#4-app-communication-native-local-messages).
- **Decision point:** E6 detailed design must establish feasibility, ownership,
  authority, waiting and GUI integration before implementation. The read-only
  browser does not depend on this decision being finalized.

## D005 — Fifteen slices, with early usable milestones

- **Status:** Selected project plan, 2026-10-09; subject to evidence-driven changes.
- **Owner:** Quinn under Chris's project-management delegation.
- **Decision:** Follow the [roadmap](../EXPLORER.md#6-delivery-slices), beginning
  with E1's menu/action/status design. Deliver browsing before the entire transfer
  architecture, then independent transfers and the richer daily-use workflow.
- **Reason:** Obtain real interaction feedback while the design is still small.
  Slice IDs are stable references, not a promise of one PR or one chat per slice.
- **Affected:** Whole arc. Split work as E1a/E1b or linked PRs if necessary; do
  not silently renumber later slices and invalidate handoffs.

## D006 — Repository records support sessions and contributor handoffs

- **Status:** Accepted customer requirement, 2026-10-09.
- **Source:** Chris asked to keep progress and decisions structured, develop across
  multiple chat sessions, and allow Opus to take development work.
- **Decision:** Maintain a slice board, decision log, per-slice records, session
  history and a current handoff. Assign work explicitly. Record actual branch,
  review and test state instead of assuming continuity between chat sessions.
- **Reason:** Another contributor should be able to resume from checked-in files.
- **Affected:** Whole arc; [working agreement](README.md#working-agreement).

## D007 — Application name: Roam

- **Status:** Accepted customer decision, 2026-10-11.
- **Source:** Chris confirmed Roam as the application name.
- **Decision:** Name the native file explorer Roam.
- **Reason:** Fits the application's purpose and the project's naming convention.
- **Affected:** Product naming and future launcher integration. Resolves O001.

## D008 — Independently toggleable interface, including the menu

- **Status:** Accepted customer requirement, 2026-10-11.
- **Source:** Chris requested menu toggles for the surrounding panes and bars,
  including the menu itself, with Xfe as a reference for a simple layout.
- **Decision:** Support a bare directory view, a directory view with address
  bar and menu, and independently chosen combinations of interface components.
- **Reason:** Let the same browser serve minimal and fuller workflows.
- **Affected:** E1 and later component slices; see the umbrella design's
  independently hideable interface contract.
- **Recovery accepted:** Chris agreed to Alt/F10 temporary menu reveal, Escape
  dismissal and a right-click Show Menu Bar command on 2026-10-11.
- **Still to settle:** Key routing, saved visibility and per-window versus
  per-pane scope.

## D009 — Shared popup-menu widget and grootmenu integration

- **Status:** Accepted customer requirement, 2026-10-11.
- **Source:** Chris requested a right-click menu widget, reusing or improving an
  existing widget if available, and reported grootmenu's font mismatch.
- **Inspection:** On `userland` at `6d988565`, grootmenu draws its own popup
  windows and rows using `os64_draw_text` and fixed bitmap metrics. The shared
  `os64/menu.h` API is a configuration parser; no popup-menu widget was found
  in libos64. Theme colors are shared, but Interface font adoption is absent.
- **Decision:** E1 supplies the reusable popup-menu widget and adapts grootmenu
  to it. Context menus, menu-bar dropdowns and cascades share presentation and
  interaction machinery, with Interface font measurement and rendering.
- **Boundary:** Menu configuration parsing and process launching remain outside
  the widget. Roam supplies actions; grootmenu supplies configured launch items.
- **Affected:** E1, its real-consumer acceptance and later Roam context menus.

## D010 — Shared icons with future desktop reuse

- **Status:** Accepted customer requirement, 2026-10-11.
- **Source:** Chris emphasized icons, stated that no icon set exists yet, and
  requested reuse of the same or similar functionality for desktop icons later.
- **Decision:** E1 accounts for optional action/menu icons; E2 delivers the
  initial asset family and shared lookup, sizing, ownership, fallback and
  drawing support. Keep this independent of Roam and suitable for larger
  desktop icons as well as menus and directory views.
- **Boundary:** Desktop icon placement, activation and persistence remain a
  later desktop feature. Demonstrate larger icons in the E2 gallery without
  requiring that feature to be built first.
- **Still to settle:** Visual style, initial asset inventory, sizes, state
  rendering and cache/lifetime contracts in E1/E2 design.
- **Affected:** E1/E2, E3/E14 consumers and future desktop work.

## D011 — Logical slices and focused kernel work

- **Status:** Accepted customer requirement, 2026-10-11.
- **Source:** Chris requested logical slices, explicitly allowing substantial
  slices, and asked to keep kernel-touching slices focused.
- **Decision:** Slice by coherent outcome, ownership and prerequisites. Size
  alone does not justify fragmentation. Keep kernel primitives and correctness
  changes separate from production service and GUI integration, while including
  the thin wrappers and diagnostics needed to verify their contracts.
- **Affected:** Whole roadmap. E6 remains the channel foundation; E8/E9 consume
  it. Missing filesystem guarantees identified by E7 become explicit focused
  prerequisites, rather than hidden additions to the operation-engine slice.
- **Handoff:** Each slice states scope, dependencies, acceptance and deferred
  work; kernel slices also specify lifetime, locking and failure behavior.

## D012 — Standalone channel design and targeted Fable review

- **Status:** Accepted customer direction, 2026-10-11.
- **Source:** Chris requested a separate message-channel design for Fable review,
  and limited additional Fable review to serious kernel changes.
- **Decision:** Prepare a standalone platform channel design linked from E6;
  obtain Fable's architectural review before implementation. Treat future app
  communication as a design customer alongside the first transfer-service use.
- **Review scope:** Fable reviews serious kernel work such as major new
  subsystems and changes to existing syscall contracts or behavior. Logical,
  conventional syscall additions using established mechanisms can follow normal
  code review and testing without an additional Fable gate. Scope and risk,
  rather than patch size, determine which category applies.
- **Evidence:** Record applicability/rationale per kernel slice, and the design
  revision, findings and disposition when Fable review is required. Fable's E1K
  review was received on 2026-10-11; see D014 for disposition
  and the focused follow-up. E6 has not yet received its standalone review.
- **Affected:** E6, any substantial kernel prerequisites and slice handoffs.

## D013 — Native popups with a focused GUI prerequisite

- **Status:** Accepted customer choice, 2026-10-11.
- **Source:** Chris selected native popups extending beyond the parent window,
  with a focused GUI prerequisite for Fable review, in the design clarification.
- **Decision:** E1K owns the native popup contract; E1's userland widgets consume
  it. Keep window association/input/lifetime in the GUI and actions/fonts/icons
  in userland. Do not emulate native ownership with app-specific polling and
  unconditional focus restoration.
- **Design:** [GUI_POPUPS.md](GUI_POPUPS.md) is the revised contract after Fable's
  first review; D014 selects the smaller architecture and records remaining gaps.
  [E01_MENUS.md](E01_MENUS.md) separates E1A, E1K,
  E1B and E1C without renumbering the 15 top-level slices.
- **Affected:** E1 and its grootmenu/Scribe consumers; E1A remains independent
  of native-popup implementation.

## D014 — Owned popup windows after Fable review

- **Status:** Selected engineering direction, 2026-10-11. R1/R2 follow-up remains
  open at that checkpoint; D015 records its closure and implementation authority.
- **Source/owner:** Fable's review of the `40077889` draft; Quinn's disposition
  under Chris's explicit delegation to decide what to take and leave.
- **Decision:** Popups take focus as ordinary native windows. Add owner links,
  conditional focus return, precise focus information, any-window waiting and
  content-rectangle readback. Use birth-size popup capacity. The menu controller
  stays in the toolkit; close menus before Interface font adoption.
- **Removed:** Kernel session object, private menu input queue, staged panel
  transactions, fused owner/panel publication and root-launcher enrollment.
  Correct the draft's VT cancellation rule: a VT switch alone preserves menus.
- **Reason:** The existing GUI can support native menus through focused window
  contracts. Cross-window atomic rendering is not a product requirement.
- **Remaining:** Safe initial focus/identity (R1) and ordinary queue loss/reset
  (R2) need focused review. Fable's first review does not cover Quinn's added
  refinements. Existing syscall behavior changes still follow D012.
- **Affected:** E1K/E1B/E1C. Sol's active E1A contract and E2 icons are unchanged.
- **Records:** [Original review](GUI_POPUPS_REVIEW.md),
  [disposition](GUI_POPUPS_DISPOSITION.md), [current design](GUI_POPUPS.md).

## D015 — E1K review closure and implementation

- **Status:** Selected, 2026-10-11. Chris authorized Quinn's E1K work after E1A.
- **Review:** Fable's second pass accepts R1 atomic owned creation and R2 loss
  signalling. Retain his review unchanged in [GUI_POPUPS_REVIEW.md](GUI_POPUPS_REVIEW.md).
- **Taken:** Parent handle encoded at birth, final focus/identity revalidation,
  uint16 same-task focus peer, BUSY child destruction and waiter conflicts,
  retiring-family task cleanup, popup exclusion from Alt+Tab, ancestor chrome
  damage, content readback and birth-size popup capacity.
- **Refined:** On INPUT_LOST discard queued gestures before client reset can
  be followed by an old down. Preserve control records/coalesced state. In E1B,
  tombstone handles and drain surviving queues after destruction before new
  creation; bounded dispatch may defer that creation into later turns.
- **Boundary:** No kernel menu sessions or IPC. Quinn implements E1B against the approved/merged E1K API; its controller
  includes dismissal expiry and post-destruction quarantine tests.
- **Record:** [E01K_HANDOFF.md](E01K_HANDOFF.md).

## D016 — Roam Classic and installable icon sets

- **Status:** Accepted customer choices, 2026-10-13; implementation details
  selected by Quinn under the existing design delegation.
- **Source:** Chris selected classic colorful icons with crisp outlines,
  approved the generated preview, authorized shipment and requested sets that
  can be unpacked into a folder for later personal artwork.
- **Decision:** Ship Roam Classic: 26 semantic keys at 16/24/32/48/64 px, with
  transparent source masters, generation prompts and export provenance.
  Reuse the E1 icon boundary through a bounded libimage provider.
- **Installation:** A set is a folder of named PNGs and a versioned `theme.conf`.
  `icons.conf` selects the absolute folder on the usual config ladder. Missing
  variants can use smaller artwork; missing keys use the shipped default.
  New app instances read the selection; a GUI picker/live switching is deferred.
- **Reason:** Menus, Roam and future desktop icons share one vocabulary and
  lifetime contract, while artwork can change independently of application code.
- **Affected:** E2 and its later consumers; [E2 contract](E02_ICONS.md).

## D017 — E3 identity, selection and transactional Details view

- **Status:** Selected engineering direction, 2026-10-13. Chris authorized the
  E3 design/handoff after the E1B retrospective; individual interaction details
  below are Quinn's design choices, not separately claimed customer approvals.
- **Owner:** Quinn. Intended implementation handoff: Sol when assigned.
- **Decision:** Reusable immutable data snapshots and ordered projections use
  domain-scoped row IDs. Each view owns independent selection/focus/anchor and
  presentation. Sort preserves IDs; filtering drops hidden selection; scrolling
  preserves offscreen selection. The directory producer owns filesystem identity
  matching and sort/filter meaning, not a second selection implementation.
- **Failure contract:** Prepare before publication. Replacement failure preserves
  the old complete view. An externally resized window whose layout cannot be
  prepared enters an explicit blocked state with retry, without stale hit targets.
- **Delivery:** E3A data/projection/selection, then E3B Details widget/gallery;
  one coherent PR is permitted. No kernel prerequisite or Fable gate. The handoff
  maps invariants to all mutation paths, interrupted sequences, paired failures
  and actual rendering evidence, applying the E1B review lessons upfront.
- **Affected:** E3/E4/E5/E14. [Contract](E03_DATA_VIEW.md),
  [implementation and acceptance packet](E03_HANDOFF.md).

## D018 — E4 implementation and the file-association conversation

- **Status:** Accepted customer instruction, 2026-10-13, with engineering
  details selected by Quinn.
- **Source:** Chris authorized making Roam after the E3 architectural check,
  and explicitly wants to discuss file associations together when that slice
  arrives. E3 PR #264 has merged as `5a521b63`.
- **Decision:** Implement the read-only E4 browser now. Reserve association and
  Open With policy for a conversation before E5; E4 activates directories and
  does not launch files based on their names or types.
- **Architecture:** Shared raw directory model, async browser controller and
  reusable browser UI, consumed by Roam. Preserve committed location until a
  prepared listing can publish; stale results cannot overwrite newer requests.
  History state is bounded. Kernel/IPC changes are outside E4.
- **Affected:** E4/E5; [E4 contract](E04_ROAM.md).

## Open choices

| ID | Choice | Resolve in | Current position |
| --- | --- | --- | --- |
| O001 (resolved) | Application name | D007, 2026-10-11 | Chris confirmed Roam. |
| O002 | Menu interaction, shortcuts and status help | E1 design | Detailed contract drafted in E01_MENUS.md; E1K second review closes R1/R2; E1K is merged; E1B implementation/evidence is in E01B_HANDOFF.md. |
| O003 (resolved) | Icon family, sizes and rendering states | D016, E2 | Roam Classic approved; provider, sizes and states are specified in E02_ICONS.md. |
| O004 (partly resolved) | Detailed directory fields, sorting and view behavior | D017 / E4 design | E3 fixes generic selection, ordering and column behavior. E4 still owns real metadata formatting, identity matching and directory-loading policy. |
| O005 | Channel and service contracts | E6/E8 design | D004 is a proposal; snapshot/subscription, authority and lifecycle need concrete contracts. |
| O006 | Visibility persistence and scope | E1 and component designs | D008 recovery gestures accepted; key routing and saved-state rules remain open. |

Add further choices when they become relevant. The umbrella's engineering
requirements remain in force even where this log has no separate decision entry.


## D019 — Scribe consumes the dialogs; association location is configurable

- **Status:** Accepted customer instruction, 2026-10-20.
- **Source:** Chris requested integrating yesterday's shared Open/Save dialogs
  into Scribe so he can see and use them before Yonder adopts them.
- **Decision:** Implement the Scribe consumer of `42410e34`. This is E5's
  picker integration, not file-association launch policy. For associations,
  Roam configuration must select the file path; no storage-format discussion
  is needed before proceeding. This resolves D018's requested conversation.
- **Affected:** [Scribe integration](E05_SCRIBE.md), later Roam associations.
