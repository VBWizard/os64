# E5D — Reusable remembered window placement

## Status and scope

Design revision 1, 2026-10-23. Owner: Quinn. Branch `codex/window-placement`,
worktree `.worktrees/window-placement`, based on userland `12dc6b64` (merged
PR #325). Implementation has not started; this document proposes the contract.

Chris authorized this slice and named Scribe as a second consumer alongside
Roam. Applications should opt in during window initialization. His instruction
for this slice is: “if it requires kernel we just need to run the design past
Fable before we're done.” The design adds a kernel event and queue state; get
Fable's review before implementing that contract. [Review request](E05D_FABLE_REVIEW_REQUEST.md).
This extends the review scope of D012 for this specific slice.

E5C is merged and Chris reports the final Open With refinement works on the P5.
Executable/file attributes are on his separate to-do list. Worker lifetime,
transfer services and general file operations remain outside E5D.

## User-visible contract

Roam and Scribe reopen at the user's remembered normal position and content
size, including after reboot. Save after a completed user move or resize, not
on close. An older instance closing cannot overwrite a newer choice.

Keep preferences independent by application/window role. Different documents
in Scribe share its main-window preference; document paths are not config keys.
Do not remember maximized/minimized state, pinning, opacity or decoration choice
in this slice. Maximize/minimize/restore preserve the remembered normal record.
A deliberate move or resize out of maximized state commits the resulting normal
geometry, matching the WM's existing gesture behavior.

A smaller screen or larger decoration clamps the live window to fit without
rewriting the stored preference. Moving that clamped window must not replace
its remembered larger size; changing only its height must not replace its
remembered width. Automatic font/minimum-size adjustments do not save.

Shared Open/Save/application dialogs retain their common, size-only preference
in `file-dialog.conf`. Their position remains relative to the owning window.
Migrate their existing size code to this helper to demonstrate both policies
and avoid two competing implementations of remembered size.

## Findings in the current implementation

- `kernel/src/gui/window.c:wm_move` changes the frame but emits no event.
- `wm_resize` emits WINDOW_RESIZE for both user gestures and automatic changes.
  It also runs for minimum-size growth, maximize/restore and decoration work.
- `kernel/src/gui/compositor.c` knows the gesture origin and completion. Resize
  uses a rubber band and commits on release; move changes the live frame.
- The public event is 32 bytes, with union offset 4 and tick offset 24. A
  20-byte placement payload fits without changing these offsets or total size.
- `gui_event_queue` already retains coalesced appearance, pointer and doorbell
  state outside the input ring. Its pending predicate drives poll and both waits.
- `CREATE_CONTENT_SIZE | CREATE_FIT_SCREEN` fits using installed decorations
  under the GUI lock. The helper can reuse that existing create contract.
- Scribe currently requests a fixed 640×440 frame at (140,90), regardless of
  the user's previous arrangement. Its `scribe_build` initializes the UI after
  native creation; adoption must remove that second initialization.

Polling would add idle work and cannot reliably distinguish automatic changes.
Saving at close misses the requested behavior and lets old instances overwrite
preferences. The WM supplies user intent; libos64 owns configuration policy.

## Kernel event contract

Add `OS64_GUI_EVENT_WINDOW_PLACEMENT` / `INPUT_EVENT_WINDOW_PLACEMENT`, using
17 if still free when implemented. No new syscall or creation flag is needed.
Emit for ordinary windows; exclude desktop and owned popup windows. Old clients
may ignore the new event. Keep WINDOW_RESIZE's existing repaint semantics.

Proposed union member, identical in public and kernel event definitions:

```c
struct {
    int32_t x, y;             /* frame origin, screen coordinates */
    uint32_t width, height;   /* content dimensions, excluding decoration */
    uint32_t changed;         /* X=1, Y=2, WIDTH=4, HEIGHT=8 */
} placement;
```

Unmasked fields are unspecified and must not be used. Assert event size 32,
union offset 4, placement.changed offset 20 and tick offset 24 on both sides.
`tick` is the last contributing commit's timestamp, not a cross-boot version
or a promise of global ordering among application instances.

### Capture intent at gesture completion

Track gesture-start frame, content dimensions and maximized state under
`kGuiLock`, alongside the existing drag/band state. On release compare the
actual final geometry with that start snapshot and populate changed components.
For a changed gesture that started maximized, publish all four components: the
result is now a deliberate normal window. A no-op gesture emits nothing.

Publish after a successful resize commit, using actual clamped dimensions.
A refused resize emits no placement update. For a normal move, report changed
position components only. Do not put persistence notifications in `wm_move`
or `wm_resize` themselves: their automatic callers must remain distinguishable.
Both titlebar moves and Ctrl+Alt moves/resizes use the same completion helper.

Live moves need an explicit cancellation rule. If a surviving window's move
is cancelled by a VT transition, decoration change or another WM transition,
finish its already-applied position change before the automatic transition.
An untouched move emits nothing. A cancelled resize outline has applied no
geometry and emits nothing. Destruction drops gesture references and pending
state without publishing to a retiring window. The implementation must audit
`gui_grab_release`, `gui_decoration_changed`, VT loss/regain and resize cancellation,
not only the ordinary mouse-release branch. Preserve the existing ownership
and draining of remaining physical button releases.

Automatic create fitting, minimum-size growth, decoration relayout, maximize,
minimize and restore never originate this event. If the user completes a
resize and immediately maximizes before the app runs, the event still contains
the committed normal size; the consumer must not query live maximized geometry
and substitute it for the snapshot.

### Coalescing, delivery and lifetime

Add one inline pending placement record to `gui_event_queue_t`, protected by
`kGuiLock`. Merge each incoming masked component into that record and OR masks.
Do not replace the entire payload: a resize followed by an automatic adjustment
and a user move must preserve the explicitly resized dimensions, not copy the
later automatically adjusted live dimensions. This is a per-component record
of unconsumed explicit edits, not a snapshot of all current window state.

Placement survives input-ring overflow and INPUT_LOST purging. Include it in
`gui_event_queue_pending`, so poll, event_wait and event_wait_any agree. Queue
through `wm_deliver_event` to use the existing waiter wake-or-owe protocol.
No allocation, filesystem operation, new worker or new scheduler lock is needed.
The record dies with its owning window and is zero-initialized on creation.

Proposed pop order preserves appearance first and eligible doorbells next,
then placement, then the existing loss/ring/pointer logic. Placement yields to
one ring/control record after it is delivered when the ring is nonempty.
When the ring is empty but placement is pending, a doorbell that has just been
delivered must yield to placement rather than using the empty-ring exemption.
A delivered placement clears that doorbell yield so the two coalesced sources
can alternate without ring input. With queued input, reset placement's yield
when a ring/control record is consumed. With no placement pending, retain the
existing queue policy. Tests must demonstrate bounded progress for all three
sources under replenishment, and preserve INPUT_LOST-before-retained-input rules.
Placement normally precedes a queued close so the last completed gesture can
be saved before a conventional app exits.

## Application opt-in and ownership

Add an opt-in window initializer layered over native create, draw-context init
and `os64_ui_init`. Proposed shape (names subject to review):

```c
os64_ui_window_options_t options = {
    .title = title,
    .x = 140, .y = 90,
    .content_width = 640, .content_height = 440,
    .remember = {
        .config = "scribe-window.conf",
        .mode = OS64_WINDOW_REMEMBER_POSITION_AND_SIZE,
    },
};
os64_ui_status_code_t rc = os64_ui_window_open(&g.ui, &g.ctx, &g.win, &options);
```

Omitting `remember.config` opts out. Roam names `roam-window.conf`; file dialogs
name `file-dialog.conf` with `OS64_WINDOW_REMEMBER_SIZE`. A dedicated config
name identifies one window role. Multiple instances may share that role;
unrelated roles must use different files. There is no global app-to-window
registry, title-based identity or process-exit save hook.

The UI embeds an app-owned placement tracker with copied, bounded config name,
policy and preferred geometry. The initializer validates options before native
creation, loads the preference, fits the create request and binds the tracker.
It initializes the UI once; app code then composes widgets and registers its
existing font/layout hooks. It does not register a dispatcher or change its
ownership. Existing `os64_ui_init` remains supported and defaults to opt-out.

On failure, preserve any native handle whose cleanup was refused in `*out_window`
and retain valid app-owned storage for retry, matching the file-dialog lifetime
contract. Document this on the initializer; apps must not drop a positive handle
merely because initialization returned an error. No heap-owned global tracker
outlives the window. Reinitialization resets the tracker for the new handle.

The dispatcher processes opted-in placement events before application command
filters and menu routing. Consume handled events there to avoid duplicate
persistence if an app normally forwards events into `os64_ui_dispatch`.
Direct `os64_ui_dispatch` also handles this event before widget/root guards.
Thus Scribe keeps recording moves while an Open/Save chooser disables editor
input. Its custom event allowlist needs no per-feature persistence branch.
Provide the same app-owned lower-level tracker/prepare/event helpers for raw
GUI consumers; they forward the event themselves, without duplicating policy.

## Stored records and restoration

Use the system configuration search path, never a compiled home directory.
For position+size, store one checked value, for example:

```
geometry = 1,140,90,960,640
```

Fields are version, signed frame x/y, positive content width/height. Reject
unknown versions, extra fields, trailing junk and numeric overflow. Keep desired
dimensions bounded to a documented storage limit (32767, matching the dialog's
existing bound); cap the live request to current screen/native limits before
creation. Invalid/unreadable preferences use caller defaults without writing
anything during startup. Preserve the read failure status for diagnostics.

Size-only retains `size = width,height` in `file-dialog.conf`, so existing
preferences survive migration without a first-open rewrite. Full-position
consumers use the versioned record; modes do not reinterpret each other's key.

Use frame x/y and content dimensions consistently through capture, storage and
restore. The existing fitted content create accounts for current borders and
titlebar metrics. Font/layout growth can adjust the live window afterward, but
never replaces the preferred geometry. Opening a dialog or restoring a window
on a small display performs no preference write.

On a placement event, mask away fields outside the selected policy. If none
remain, do nothing. Re-read the current complete record before merging the
explicit fields, falling back to this window's preferred/default record only
when that setting is absent or malformed. An I/O failure is not an absent
record: refuse that save. Merge only explicit components, then publish one
complete value via the checked atomic config writer. Preserve unrelated settings
in the destination. A lower-ladder source must be preserved on first override;
reuse the checked source-to-target merge introduced in E5C when a source exists.
Separate configuration syntax validation from numerical preference validation:
a syntactically valid file with an invalid geometry value can be repaired by
an explicit user gesture. Reject malformed whole-file syntax, embedded NULs,
oversized input and failed reads before publishing.

If no source resolves, use a checked first-publication helper with NOREPLACE,
not the existing writer's assumption that any failed open means an absent file.
This is a small extension to the shared config writer using existing rename
flags: merge from empty, validate, write/sync a unique temporary, then publish
only if the destination is still absent. A concurrent creator or an existing
unreadable destination causes refusal and retains that file; a later explicit
edit can retry through the normal readable-source path. Do not broaden config
search or filesystem error semantics in this slice.

No writes during drag motion, initialization, maximize/restore or close. One
coalesced completion leads to at most one publication; no timer or worker pool.
A write failure leaves the old file intact and the application usable; expose
the failure in tracker status rather than interrupting editing with a dialog.
A later explicit edit may retry; do not add an exit-time retry that could
publish an older instance's geometry.

Independent applications write independent files. For concurrent instances of
the same role, the last successful complete publication wins. Re-reading before
merge reduces stale-component overwrites but is not a serialized transaction;
we do not promise physical gesture-time ordering between delayed processes.
Closing an old instance writes nothing. Persisted geometry is a preference,
not a guarantee that a crash before the UI consumes an event saves that gesture.

## Consumer changes and implementation order

1. Obtain Fable review of this kernel/event and library contract; record findings
   and disposition beside this design. No executable changes precede that review.
2. Add the ABI payload, coalesced queue slot and compositor completion/cancellation
   hooks, with kernel/host tests. Correct touched comments that claim resize is
   the only geometry observation or that placement is saved only at exit.
3. Add the app-owned persistence helper and opt-in UI window initializer. Test
   parser bounds, masked merging, checked first publication, write failures and
   initialization cleanup.
4. Adopt in Roam and Scribe; remove Scribe's duplicate UI initialization. Use
   explicit content-size defaults, and keep menu/font/dispatcher setup intact.
5. Migrate file-dialog size memory to SIZE mode. Remove its superseded baseline/
   maximize heuristics only after the existing regressions pass with the new event.
6. Cross-build, QEMU acceptance, independent code review and P5 acceptance. Use
   controlled HMP mouse press/release for native gesture tests; the previous
   standalone QMP button driver was unreliable in this VM.

## Required evidence

| Area | Cases |
| --- | --- |
| ABI/queue | Size/offset assertions; full ring; INPUT_LOST; masked coalescing; repeated doorbells/placement/input all progress; wait and wait-any wake on placement alone |
| User intent | Titlebar and chord moves; all resize corners; no-op and refused gestures; actual clamped values; cancellation after live move versus uncommitted resize; teardown/handle reuse |
| Automatic changes | No commits from create fitting, decoration changes, minimum/font growth, maximize/minimize/restore; resize then maximize before dequeue still persists normal dimensions |
| Preferred geometry | Large→small→large display round trip; move only and one-axis resize on small display preserve untouched preferred fields; manual resize/move out of maximized |
| Persistence | Missing/malformed/oversized config, integer limits, unknown version, read/write/sync/rename failures, first-publication collision, comments preserved, unrelated roles isolated, concurrent same-role publication and old-instance close |
| Toolkit | Opt-out unchanged; one-time initialization; copied options; failed native cleanup retains handle; dispatcher and direct dispatch process once; modal command filters do not swallow placement |
| Consumers | Roam and Scribe move/resize/relaunch/reboot; Scribe repeated documents; shared Open/Save size-only compatibility and parent-relative position; existing 4,760 dialog checks adapted without dropping their behavioral assertions |

Use ASan/UBSan with LeakSanitizer enabled for host coverage, escalating the run
if sandbox thread attachment prevents leak checking. Build and native evidence
must identify tested binaries and distinguish QEMU from Chris's P5 report.

## Questions for Fable

1. Is the masked, coalesced completion event the right boundary between user
   intent in the compositor and policy in libos64, including cancellation?
2. Does the proposed scheduling slot preserve bounded queue progress and wake
   semantics without adding lifetime or lock-order hazards?
3. Are the per-component merge rules sufficient for clamped-screen restoration
   and intervening automatic geometry changes?
4. Is the one-call UI initializer's partial-failure ownership clear, and does
   dispatcher interception cover consumers whose input is temporarily disabled?
5. Are the same-role last-publication semantics acceptable for this slice, or
   is stronger cross-process write serialization required before implementation?
