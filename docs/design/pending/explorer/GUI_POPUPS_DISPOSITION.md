# E1K review disposition

Quinn, 2026-10-11, under Chris's delegation to decide the design feedback.
Source: [Fable's review](GUI_POPUPS_REVIEW.md), retained unchanged. Current
contract: [native owned popups](GUI_POPUPS.md). The reviewed draft is in commit
`40077889`; this response does not imply Fable reviewed the revisions below.

**Take the smaller architecture.** Menus use ordinary native windows that take
focus, with owner links, conditional focus return, precise focus information,
any-window waiting and content-rectangle readback. Keep menu sessions entirely
in the toolkit. The first design put unnecessary layout/publication machinery
in the kernel; the product requirement was native menus, not atomic rendering
across a family of windows.

## Dispositions

| Review point | Decision | Result / reason |
| --- | --- | --- |
| Four real GUI gaps | Accept | Preserve evidence; solve through existing window contracts. |
| Popups take focus instead of owner-held keyboard grab | Accept | Shared menu controller handles family focus; no new grab lifetime in E1. E12 can design a drag grab when needed. |
| Same-task owner links, cycle checks, conditional return | Accept with refinement | Preserve links by identity; resolve safe opening before focus is taken, not only restoration after destruction (R1). |
| BUSY destroy while children attached | Accept | Refuse before mutation; toolkit closes deepest-first; task-exit sweep bypasses without dangling links or transient focus into dying windows. |
| `other_handle` in focus event | Accept direction, incomplete identity contract | Retain old fields/size, but slot reuse can make delayed events name a replacement. Pin identity/lifecycle rules in R1. No permanent 16-bit table promise. |
| Exclude popups from Alt+Tab; active ancestor chrome | Accept with implementation detail | Test first Tab from a cascade, and damage ancestors on focus changes. A changed paint predicate alone does not invalidate pixels. |
| Wait on any window; BUSY waiter collision | Accept | Add fairness, all-or-none registration, zero-window exit, cleanup, new-window backstop semantics, and the matching legacy-wait guard. |
| Content rectangle readback; userland placement | Accept | A snapshot is sufficient; do not promise geometry stability. Appearance can change frames without a user press, and popup chord movement remains possible. |
| Popup birth-size capacity | Accept | Both kernel snapshot and mapped canvas use it. Ordinary capacity stays unchanged; audit small-popup minima and existing clients. |
| Remove staged panels/fused publication | Accept | Close menus before Interface font adoption; failure leaves old owner font and closed menus. Next explicit opening uses new font. No automatic reopening from Appearance. |
| Measure create-to-publish flash first | Accept | Diagnostic checks it; no speculative hidden-window subsystem. |
| Remove private menu event queue | Accept | Keep ordinary window queues. Their input-loss behavior still needs a small shared contract (R2). |
| Existing queue is sufficient because focus is never dropped | Decline as a correctness proof | Focus insertion evicts oldest, including prior focus; ordinary button edges can drop. A later release can match the wrong retained press. R2 proposes a loss indication/reset on the existing queue. |
| Consume owner dismissing clicks in toolkit | Accept with explicit state machine | Account for both queue-drain orders and teardown-generated focus return. Do not forget dismissal before owner input dispatch; do not swallow a later unrelated click. |
| No VT-departure cancellation | Accept; correct my draft | DEBTS.md explicitly preserves popups across VT switches. Restore that behavior and acceptance case. |
| No root enrollment mode | Accept | Unattached grootmenu root, attached child windows, same widget/controller; process remains independent of desktop. |
| Popup WM close/minimize behavior | Accept | Alt+F4 dismisses focused popup; minimize/maximize stay declined. Owner teardown still cleans child windows first. |
| Separate `heapEnd` race fix | Accept scope; no implementation here | Source confirms both unsynchronized sites. Separate reservation/map concurrency fixture; do not claim CAS alone proves page-table safety. |
| No new sibling-TLB prerequisite | Accept scope | Existing limitation remains; never-reused VA does not prevent physical-page recycling. No broader safety claim or new canvas-sharing guarantee. |
| Raise 32-window limit only on evidence | Accept | Keep 32. Evaluate buffers and client consequences as well as table bytes if increasing it. |
| Every item automatically qualifies as routine/additive | Decline blanket classification | Destroy BUSY, waiter behavior and popup capacity alter existing contracts. Fable has reviewed those proposals here; R1/R2 extensions still require focused review under D012/D013. Userland does not inherit that gate. |
| Ship wait first, ownership/content, capacity separately | Accept logical boundaries | E1K-W, E1K-O, E1K-C name coherent outcomes. No requirement to fragment a cohesive implementation into a fixed PR count. |

## Effect on Sol's active E1A

**Continue E1A unchanged.** No action descriptor, view/context, binding,
shortcut or status-widget API requirement changes. No kernel or popup stubs
belong in it. The existing instruction to reset E1A shortcuts on ordinary focus
loss remains correct; E1B later intercepts recognized menu-family transitions.
Sol's worktree is `.worktrees/explorer-e01a`, branch `codex/explorer-e01a`, based
on `6d988565` at inspection. It contains in-progress action/status sources.
This review did not modify that worktree or assess its implementation.

For E1B, replace owner-only wait/session-notification routing with a shared
multi-window dispatcher. Replace owner-retains-native-focus with popup focus
and logical editor-target preservation. Replace the cross-window font barrier
with close-before-adopt. E1C's launcher uses no enrollment API. These changes
are reflected in [E01_MENUS.md](E01_MENUS.md).

E2's icon provider, asset inventory and future desktop reuse boundary are
unchanged. Fable read E2 but supplied no specific icon-contract revisions in
this review; do not call the artwork chosen or E2 implementation accepted.
The standalone E6 message-channel design/review remains future work.

## Focused follow-up for Fable

[GUI_POPUPS.md](GUI_POPUPS.md#remaining-focused-review-questions) contains the
complete follow-up. Review R1's conditional activation and delayed-event identity,
and R2's shared input-loss/reset contract. These are concrete refinements to
Fable's preferred architecture, not a request to resurrect the session manager.
No follow-up has been sent by this documentation change.

The full E1K ownership/input handoff remains not Ready until those contracts are
settled. Wait/capacity preparation can progress independently. No user decision
is needed to continue Sol's E1A work, and no implementation acceptance, merge or
deployment is claimed by this disposition.
