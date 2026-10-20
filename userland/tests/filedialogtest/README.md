# Save and Open dialogs

`filedialogtest` exercises the shared `os64/ui_file_dialog.h` windows without
reading or writing the selected file. New Folder creates a directory when the
user requests it.

```
/tests/filedialogtest save /home download.html
/tests/filedialogtest open /home
```

The test prints the selected absolute path and whether replacement was approved.
Escape or the window close button cancels. In a replacement or New Folder prompt,
Escape returns to the file chooser.

The shared browser supplies Back, Forward, Up, Refresh, editable location,
breadcrumbs, favorites, mounted volumes, sorting, and a substring name filter.
Ctrl+L focuses Location, Ctrl+F focuses Filter, Ctrl+H toggles hidden files,
Alt+Up goes to the parent, and F5 refreshes. Enter or double-click enters a folder
or accepts a file. Save carries the proposed filename through folder navigation.
Open chooses one existing file. Neither mode adds a filename extension.

## Calling from an application

Keep a zero-initialized `os64_ui_file_dialog_t` alive for the pending operation.
`os64_ui_file_dialog_open` takes optional dispatcher/parent handles and copied
options for the mode, title, starting directory, and proposed filename. The icon
provider is borrowed until close. Supplying a dispatcher registers the window
and its event/paint callbacks. Check `result` after each dispatcher turn; copy
`path` and `replace_existing` when accepted, then close the dialog. Retry a
refused close before releasing its storage or destroying the parent window.

A caller with its own event loop can pass a null dispatcher and wait on the
dialog's `window`, then call `os64_ui_file_dialog_pump`. To keep processing parent
events, wait on both windows or relay the dialog's readiness to the parent loop,
as Yonder's Settings window does. Polling only when the parent receives input
leaves an idle parent's dialog unresponsive. The window does not impose native
modality on its parent; the caller owns pending-operation policy.

Save returns a path and explicit replacement consent, not a file handle. With
`replace_existing == false`, create with mode `"x"`, or publish with
`OS64_RENAME_NOREPLACE`. This also protects against a destination that appears
after selection. The caller reports I/O failures and owns download/serialization
and atomic-publication policy. Cancellation does not create or truncate a file.

## Verification

`python3 tools/test_file_dialog_host.py` runs the real dialog composition,
browser, directory model, data view and fonts with controlled filesystem/worker
completions. It covers navigation, missing files, replacement and refusal,
creation, stale IDs, keyboard selection, font adoption, allocation failures,
and native-window lifecycle through a mocked syscall boundary. ASan, UBSan,
and leak detection are enabled. Use `--build-only --output /tmp/dialog-tests`
to retain a binary for a leak-enabled run outside a sandbox that denies thread
attachment. The harness writes a rendered Save preview alongside its binary.

Native QEMU checks exercise Save results, Keep File and Replace, missing and
existing Open targets, and New Folder while retaining a suggested Save name.
The caller-specific Yonder link-download action is separate from this fixture.
