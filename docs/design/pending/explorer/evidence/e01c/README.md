# E1C — production menu consumers

2026-10-12. Branch `codex/explorer-e1c`, based on E1B merge `88f8b672`
(PR #259). Chris verified Scribe and the desktop menu on P5 and authorized
publication. Ordinary review remains pending. Hardware acceptance is
user-reported; no installed-binary hash was captured.

## Behavior

- grootmenu adapts the existing config parser and command nodes to shared
  models/actions. Standalone native hosting supplies Interface-font rendering,
  keyboard/pointer cascades, scrolling and dismissal. Commands launch after
  popup teardown; quoting, named-menu invocation and config lookup remain.
- Scribe has File/Edit/View/Help, file actions, context menus and a
  bottom status widget. Menu and status visibility reclaim layout space.
  Alt/F10 and Show Menu Bar recover a hidden menu. Clipboard commands retain
  their document or entry-field target across popup focus changes. Right-click
  leaves the caret/selection intact. Existing help, save errors and unsaved
  quit policies remain; new notices re-show hidden status.
- No kernel changes, artwork, picker, browser shell or IPC are included.

## Original E1C automated results

| Suite | Result |
| --- | --- |
| `tools/test_scribe_host.py` | 2,890 checks, zero failures |
| `tools/test_scribe_host.py --menus` | 1,335 checks, zero failures |
| `tools/test_ui_text_host.py` | 148 checks, zero failures |
| `tools/test_ui_text_host.py --real` | 1,626 checks, zero failures |

All runs use ASan/UBSan and enabled leak detection. The sandbox's LSan
thread attachment failed with `EPERM`; diagnosis used
`LSAN_OPTIONS=verbosity=1:log_threads=1`. The same compiled binaries passed
outside the sandbox with `ASAN_OPTIONS=detect_leaks=1`. Logs are adjacent.
Final relinks reused the unchanged, already instrumented FreeType objects.

The consumer harness drives production Scribe and grootmenu code, the real
menu controller/dispatcher, real font engine and grammar parser. It replaces
native window queues, disk/config transport and spawn. It checks saved
clipboard targets, right-click selection preservation, hidden-bar recovery,
Escape/help ordering, repeated Ctrl+Q, repeated popup cleanup, refused native
destruction and retry, initialization allocation failures, retained parent menus after refused child
creation, quoted argv,
missing/empty/unresolved menus, cycles, oversized rows and parser maximum depth.

Cross-build passed for `scribe`, `grootmenu`, `tests/scribefonttest` and their
library dependencies. `binaries.sha256` identifies the original E1C publication outputs;
`qemu-binaries.sha256` identifies the native walkthrough outputs.
Source whitespace checks passed. The two captured document files intentionally
retain their extra final newline; publication checks exempt that byte-preserving
evidence from the blank-at-EOF rule. `stale-refs.txt` records the retirement audit and
its dispositions. The old private cascade/polling/depth description and old
Scribe chrome description were inaccurate for these consumers and are updated.

## QEMU

Tests used scratch disk copies, the E1B acceptance ISO (E1K native popup
kernel), port 55556, four vCPUs and 4 GiB RAM. No repository disk image was
written. The first boot had USB HID keyboard support; the final boot used
PS/2 input with the recorded native binaries. The subsequent grootmenu fix
keeps a parent open after child-creation failure; its injected-failure regression
passed on the host. That failure path was not repeated in QEMU. The VM was stopped after flushing results.

Observed and retained:

- Scribe File menu and shortcuts; save produced `e1c-note.txt` with the
  entered text. Help remains while its menu is dismissed. View hides the
  bar and F10 temporarily reveals it with layout reflow.
- Live Interface Apply to 32 pixels updates Scribe's bar/dropdowns and a
  newly opened desktop root menu; Document remains 16 pixels. The Apply
  report is `e1c-font32.txt`; it is session state, not a persisted preference.
- Desktop right-click opens the shared root; keyboard navigation opens Tools.
  The cascade remains across a console VT round trip (`groot-vt.png`).
  Choosing Editor launches Scribe after the menu family disappears.
- Dirty Scribe requests a save on Ctrl+Q; a second press discards. Saving to
  `/dev/full` keeps both the text and the prompt, with a visible write error
  (`save-failure-final.png`).
- Final PS/2 boot uses a scratch `menu.conf` item:
  `item "Quoted note" /bin/scribe "/home/quoted note.txt"`.
  Enter opens that exact path, and Save creates it (`quoted-note.txt`).
  Input started during launch, so the saved text is the suffix that arrived
  after startup; the filename/argv check is the purpose of this fixture.
- `grootmenu missing` reports the missing name and exits 1. The first
  `serial-apps.txt` retains the diagnostic; `e1c-missing.txt` holds the status.

The native walkthrough and host suites are complementary. No complete native
no-GUI/config-failure matrix was run.

## P5 report

Chris confirms that both Scribe's menu and the desktop menu work correctly.
He specifically verified recovery of the hidden menu with Alt alone and the
improved readability of the desktop menu. This is user-reported acceptance,
separate from the instrumented host/QEMU evidence; it does not assert that
every optional walkthrough step was performed.

## P5 walkthrough

Build/install from this worktree, including `libos64.so`, `scribe`,
`grootmenu` and the rebuilt `tests/scribefonttest` if using that fixture.
These consumers need the merged E1K/E1B platform already in `userland`.

1. Right-click the desktop. Check the current Interface font, mouse and
   keyboard cascades, screen-edge placement and dismissal by clicking outside.
2. Open Scribe. Try File/Edit/View/Help, including Open/Save/Save As.
   The duplicate button row is removed; commands still reveal the path field.
   Select document text, right-click elsewhere in the text and copy it.
3. Open a filename or Find field. Select some field text, use its context
   menu and the Edit menu for Cut/Copy/Paste; the document must stay untouched.
4. Hide both bars. Recover the menu with Alt/F10, dismiss it, then restore it
   permanently with the context menu. Check that the document reclaims space.
5. Change Interface font/size with a menu open. It should close before font
   adoption. Reopen and check the new font; the document keeps its own role.
6. Exercise Help, a save failure and dirty quit. Escape dismisses menus before
   leaving Help; a second Ctrl+Q at the save-before-quit prompt discards.
7. Switch VTs while a menu is open, return, then close the application. Check
   for stranded popups. Record the installed revision or binary hashes.

## Follow-up: remove duplicate Scribe buttons

At Chris's request, remove Open/Save/Save As buttons below the menu and
reclaim their row for the document. File actions and shortcuts retain the
path textfield for Open, Save As and save-before-quit. Update the font
transaction snapshot to track the menubar in place of the removed button.
The current design and chrome documentation now describe this layout.

`toolbar-removal/` records the follow-up outputs: Scribe passes 2,526 checks
and menu consumers pass 1,335, with ASan/UBSan and leak detection enabled
outside the sandbox. The Scribe suite's allocation-failure sweep has fewer
allocation sites after deleting the buttons; its cases remain enabled.
Both Scribe and scribefonttest cross-build successfully. The retirement
audit and whitespace check pass.

QEMU used the same scratch disks and E1B ISO with the rebuilt Scribe staged
before boot. Screenshots show the reclaimed document space, Open and Save As
fields, successful save, and dirty-quit path prompt. Enter at that prompt
saved the added line and exited. `saved-document.txt` records the exact saved
bytes after guest sync. The VM was stopped. The original P5 acceptance above
predates this follow-up; this layout change has host/QEMU validation.
