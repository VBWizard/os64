# E5 — Scribe consumes the shared file dialogs

## Status and ownership

Accepted, 2026-10-20. [PR #319](https://github.com/VBWizard/os64/pull/319)
merged as `ddaad166`. Chris reported a clean Codex review and successful P5
use, including resizing; no installed binary hash was captured. Quinn owns
the integration.
Implementation `29252438` is on `codex/scribe-file-dialogs`, worktree `.worktrees/scribe-file-dialogs`,
starts from `aa04123c` and includes the shared-dialog commit `42410e34`.
E4 is merged as `a3dd4b65`. This is userland composition; no kernel ABI changes
or separate Fable architecture review are required.

## Outcome and scope

Chris requested Scribe as the first real Open/Save dialog consumer, so he can
try the shared windows before Yonder uses them. Open, Save As and unnamed Save
use `os64/ui_file_dialog.h`. Named Save keeps its direct-save behavior.
Save-before-quit uses the picker when a destination needs choosing.

File associations and Yonder integration are separate work. Chris's association
requirement is that Roam's configuration select the associations-file path.

## Design

Scribe owns one pending file operation and dialog, serviced by its existing
dispatcher. Creation and completion happen outside event callbacks, after
menu teardown. The editor is disabled while choosing a file; cancellation
preserves its bytes, path and dirty state. Opening over unsaved edits asks
before replacing the document.

The picker returns a path and replacement consent. Save creates exclusively
when consent is absent, including when a destination appears after selection.
The buffer writer owns I/O and reports write/sync failures without clearing
dirty state or completing a pending quit. Open from the picker requires an
existing file; command-line startup retains Scribe's new-file behavior.

Dialog cleanup precedes parent/font/icon teardown. A refused cleanup retains
the pending operation and borrowed storage for retry.

## Acceptance

Host checks cover cancellation, delayed close, failed creation, current-folder
and filename defaults, dirty-document protection, exclusive creation races,
write/sync failures and save-before-quit. Existing Scribe/menu and shared-dialog
suites remain applicable, with ASan/UBSan/LeakSanitizer enabled.

Cross-build Scribe and libos64. Native QEMU acceptance uses disposable files:
Open, unnamed Save, Save As, replacement consent/refusal, cancellation, and
quit after saving. P5 acceptance belongs to Chris after delivery.

## Evidence and handoff

[Evidence](evidence/e05/README.md) records 2,584 Scribe checks, 1,655 menu
integration checks and 1,928 shared-dialog checks, with ASan/UBSan/LeakSanitizer.
The cross-build and native QEMU file round trip passed. Review and P5
acceptance are user-reported as recorded above.

### Try on P5

The delivery bundle is `/tmp/scribe-dialog-wire`: install `scribe` to `/bin`
and `libos64.so` to `/lib` through the usual os64get routes. Close applications
using the previous library before replacing it. Roam's installed icon/font
assets and libimage dependencies remain applicable. Rebuild from this worktree
with the absolute Scribe/libos64 targets in `userland/GNUmakefile` if needed.

1. Run `scribe`, type a few lines, and press Ctrl+S. Browse a folder, enter a
   filename and save. The dirty marker should clear.
2. File → Save As proposes the current folder and filename. Try Cancel, then
   save to a second name. Choosing an existing name requires Replace; Keep
   File returns to the chooser.
3. Ctrl+O opens the browser. Choose an existing text file. Try an unknown name
   and cancel; the current document remains intact.
4. Edit, then Ctrl+O. Enter continues without saving; Escape cancels. Even after
   continuing, cancelling the picker keeps the edits.
5. Start an unnamed document, edit, then Ctrl+Q and Enter. Save As opens; Cancel
   keeps Scribe running, while a successful save closes it.

Known limit: the buffer writer saves directly. Replacement is explicitly
consented, but a write/sync failure can leave a partial destination; the editor
retains the document and does not complete a pending quit. Atomic save remains
separate work in the Scribe design.

Next: [compact footer and shared size memory](E05A_DIALOG_POLISH.md).
Parent-owned stacking needs a separate window-manager follow-up. Yonder's
consumer and Roam file launching are separate tasks. The associations-file
path must remain configurable when the latter is implemented.
