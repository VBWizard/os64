# E5A — Shared dialog layout and remembered size

Implemented by Quinn on `codex/file-dialog-polish`, based on `5fb5f7a6`.
PR #319 merged as `ddaad166`; Chris reported a clean review and successful P5
use, including resizing. No installed binary hash was captured.

The footer uses a filename row and a controls row, wrapping the action buttons
when necessary. Message space appears only for a prompt or error. Open and Save
share a content-size preference across applications. `file-dialog.conf` stores
`size = width,height` in pixels through the configured config search ladder;
there is no hardcoded home path. Each open reads the preference afresh.
A completed resize publishes the pair atomically, including before Cancel.
Closing an unchanged older dialog does not replace a newer preference.
Maximize/restore and automatic screen/font clamping do not save a preference.
Missing or malformed preferences use 880 × 700; failed persistence leaves the
previous config intact and file selection available.

Parent-owned stacking needs a separate window-manager follow-up. File
associations and Yonder integration remain separate work.

[Evidence](evidence/e05a/README.md): 4,223 shared-dialog and 2,584 Scribe
checks passed with ASan, UBSan and LeakSanitizer enabled. Cross-build passed.
Native QEMU verified resize/Cancel/restoration across Scribe and filedialogtest,
wrapped controls, replacement message expansion, and preservation of the
newer preference when an older dialog closes. Review and P5 acceptance of
this follow-up remain pending.

## Delivery and P5 walkthrough

Bundle: `/tmp/file-dialog-polish-wire`, with `SHA256SUMS`. Install `scribe`
to `/bin`, `libos64.so` to `/lib`, and `filedialogtest` to `/tests` through the
usual os64get routes. Rebuild/deploy these consumers together: the app-owned
dialog structure changed. Close applications using the previous library
before replacing it. This bundle has not been installed on P5.

1. Open Scribe's picker and check the compact footer. Resize narrow enough to
   wrap the action buttons; the checkbox and New Folder should remain usable.
2. Resize Open, Cancel, then open Save As: it should retain the chosen size.
3. Launch `/tests/filedialogtest open /home`: it should use that size too.
   Resize it, cancel, then reopen Scribe's picker to see the new size.
4. With two dialogs open, resize the newer one and cancel both. Closing the
   older unchanged dialog should not restore its previous size.
5. Trigger replacement consent: its message row appears; Keep File removes it.

`file-dialog.conf` is resolved through `/etc/os64.conf`'s configured `conf`
ladder and writes to its top directory. Moving that ladder to `/home/yogi`
moves the preference destination without a source change. Invalid sizes fall
back to the default; dimensions are decimal integers from 1 through 32767,
separated by a comma. Native creation clamps to the display; composition
raises the minimum for the current font. A screen too small for those controls
can refuse creation. The stored preference is unchanged by these adjustments.
