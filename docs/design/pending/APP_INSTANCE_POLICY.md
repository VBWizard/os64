# Opt-in single-window applications

Status: proposal only, 2026-09-20. Chris requested investigation and this
plan while the font work goes to Fable. No implementation is included or
authorized by this document. Inspected baseline: `020040b` on
`codex/font-settings`, in `.worktrees/font-settings`.

## Recommendation

Give applications an opt-in **open or activate** operation for their primary
window. Control Center and Appearance Workshop opt in. Scribe, gterm and
other applications retain their existing ability to open multiple instances.
This is a moderate, bounded feature. Most of the care is in simultaneous
launches, ownership and cleanup; it does not depend on window decorations.

| Application | Repeated launch |
| --- | --- |
| Control Center | Bring its existing window forward, or create it if absent |
| Appearance Workshop | Bring its existing window forward with its draft intact |
| Scribe | Open another instance as it does now |
| Other applications | Keep current behavior unless explicitly opted in |

Activation unminimizes, raises and focuses the existing window. It preserves
size, position, maximization, pinning, selected page, unsaved changes and any
confirmation dialog. In particular, it must not invoke maximize/Restore and
therefore cannot bypass the new minimum-size Restore rule. Raising respects
the existing stacking bands; it does not grant always-on-top status.

A second launch can briefly create a process, but it exits successfully before
creating another window or initializing the application. The user-facing rule
is one primary window per opted-in app, not a prohibition on two transient
processes. This keeps launchers simple and uses their existing child reaping.

## What the current code provides

- `userland/apps/grootmenu/grootmenu.c:launch` and
  `userland/apps/controlcenter/controlcenter.c:open_tool` call `os64_spawn`.
  Neither retains an application-instance registry. A launcher-only check
  would miss shell launches and competing launchers.
- Both target applications create their window near the beginning of `main`,
  before their UI setup. This is a suitable place for the opt-in operation.
- `abi/include/os64/gui.h` exposes window creation and owner-checked state
  queries, but has no public application-ID lookup/activation operation.
- `kernel/src/gui/window.c` already provides `wm_raise` and
  `wm_set_minimized`. These operate within the existing GUI locking model.
- `kernel/src/gui/gui_client.c` owns client handles and checks ownership.
  `gui_task_destroy_windows` removes a task's windows on teardown; individual
  destruction and creation rollback already have canvas-lifetime machinery.
- Window creation stages task-backed canvas memory before taking `kGuiLock`
  for final handle/window insertion. A new lookup must account for that gap.

The recommendation needs a small kernel/API addition, not just an app-side
flag. Its scope should be agreed with Chris before implementation. The kernel
would enforce a generic window identity, not contain a list of special apps.

## Proposed API and identity

Add a separate operation, tentatively `os64_gui_window_open_single`, leaving
ordinary `os64_gui_window_create` unchanged. Pass a bounded request containing
a stable application ID and the usual title, frame and creation flags.
Use a copied, validated request structure rather than squeezing additional
arguments into the existing six-argument syscall. Choose the syscall number
and finalize the structure during implementation review.

Proposed results:

- Positive: a newly created handle owned by the caller; continue normal setup.
- Zero: an existing window was activated; exit the duplicate process with 0.
- Negative: a normal GUI error; report it and exit without an unkeyed fallback.

Never return another process's handle for the duplicate to paint or destroy.
The operation authorizes activation by application identity; it does not grant
access to the existing window's canvas or other owner-only operations.

Use bounded, case-sensitive ASCII IDs, e.g. `os64.controlcenter` and
`os64.appearance`, with a proposed maximum of 63 bytes plus NUL. Reject invalid
or overlong IDs rather than truncating them. Titles are presentation and may
change independently; filenames, PIDs and translated captions are not IDs.
The duplicate's creation parameters do not alter the existing window.
Desktop and popup windows are outside this primary-window operation.

For the current single desktop, scope identities to the GUI session. Future
independent desktops/users must get separate namespaces. An application ID is
not proof of executable identity: under the current cooperative desktop model,
a program could claim another app's ID. This feature must not be presented as
a security boundary. Stronger isolation would require authenticated app
identity, not more title or process-name matching.

## Atomic creation and lifetime

Prefer identity stored on the live window/handle record over a separate
persistent registry or lock file. Proposed creation flow:

1. Copy and validate the complete request before side effects.
2. Under `kGuiLock`, look for a matching live primary window. If found,
   unminimize/raise it and return the activation result without allocating a
   new canvas. Activation should still work when creation resources are full.
3. If absent, stage the usual creation resources outside the lock where the
   existing allocation path does so.
4. Reacquire the lock and **check the ID again before creating or showing a
   window**. A competing caller may have won while resources were prepared.
   If so, activate the winner, release the lock, and unwind the loser's staged
   canvas through the existing lifetime-safe rollback machinery.
5. Otherwise insert the new window and its identity in the same locked
   operation. Publication of the identity and window must be indivisible.
   Allocation/handle exhaustion leaves no registered identity behind.

Keep validation, cleanup ordering, address-space handling and ownership checks
explicit during implementation; do not hold a GUI lock across newly introduced
user copies or event waits. No lookup pointer may outlive its lock protection.

Destroying the window releases its identity. Owner exit/crash follows the same
window cleanup path, so no stale PID file prevents a later launch. Handle reuse
must not inherit the previous identity. Failed app setup after successful
creation must destroy its window before exiting.

Activation means that a live window was brought forward at that instant. It
does not promise the app has painted its first frame or is responsive. If that
owner subsequently fails during initialization or closes concurrently, the
user can launch again. Avoid adding a readiness broker or automatic takeover
for this first slice. A hung existing app is brought forward for the user's
normal close/force-close controls; launching it again does not kill it.

Do not automatically switch VTs or create a GUI on a non-GUI boot. Use existing
launch behavior. Workshop's confirmation UI is currently drawn in its main
window, so activating that window preserves the dialog. Future applications
with separate modal windows will need an explicit modal activation policy.

## Application changes and rollout

First implement and review the generic API and its lifetime/concurrency tests.
Then change the two applications' initial creation calls and handle the three
result cases. Exit the duplicate before starting Control Center's reaper or
Workshop's font/theme loading. Menu configuration and Scribe need no changes.

This policy applies to cooperating applications using the new operation;
ordinary creation remains available. It does not consolidate already running
old versions with unkeyed windows. Test deployment with the matching kernel
and freshly started applications. Do not add a user-facing toggle or a
`--new-window` escape for these two tools in the first slice.

## Acceptance checks

- Repeated menu, Control Center and shell launches leave one primary window.
  The duplicate exits 0 and is reaped normally.
- Concurrent launches on multiple CPUs produce one window, with no losing
  window flashing, leaked canvas, stale identity or foreign handle returned.
- Test the race with allocation failure, a full handle table, owner teardown,
  setup failure and handle reuse. Existing-window activation must work even
  when a fresh window could not be allocated.
- Minimize/cover/maximize the existing window, then relaunch. It becomes
  visible and focused with its original geometry/maximized state intact.
  Include a maximized Workshop whose saved Restore rectangle is now too small.
- Make unsaved Workshop font/theme edits, change tabs, and open confirmation;
  relaunch from another entry point. Draft, tab, preview and dialog survive.
- Close or kill the owner and launch again. A fresh working window opens.
- Launch Scribe twice with different files and verify independent windows.
  Check ordinary window creation and popup/menu behavior remain unchanged.
- Validate invalid requests, forbidden window kinds, no-GUI boots and ownership
  boundaries. Use focused tests, strict builds and an 8-CPU QEMU interaction
  pass; record P5 acceptance separately.

Defer general single-process applications, file-open argument forwarding,
per-document identity, background services, session restoration and arbitrary
cross-process window control. Those are separate contracts; these two settings
apps only need “bring my existing window forward.”
