# Frame Studio: review follow-up, 2026-09-24

PR #127's shortcut correction is included through `codex/ui-editing` commit
`2aa0a043`. Printable Ctrl chords no longer alias the already-translated
clipboard control codes. The regression covers Ctrl+8, Ctrl+6, Ctrl+# and
Ctrl+!, preserving text, selection and clipboard; it produced seven failing
assertions before the correction and passes afterward. The edited textfield
blocks use the surrounding file's tab indentation.

PR #128's WM preflight now logs the blocking window id, title and geometry
rule under DEBUG_GUI. Bare-frame windows use the installed active/inactive
border palette. The content-size creation helper documents its UTF-8 title
mode, the shared kernel compile rule is quiet, and the reviewed blocks in
existing tab-indented files follow that indentation.

Validation:

- PR #127: real-font UI-text suite, 1,442 checks; Scribe, 2,470 checks;
  appearance/list/picker suite; strict userland build.
- Integrated PR #128: decoration/storage/startup host suite and strict
  kernel/userland/ISO build. Host suites used ASan/UBSan with
  `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer cannot run under the
  sandbox's tracing environment. These runs do not establish leak freedom.
- QEMU: eight CPUs, 1024x768, ext2 GUI boot with DEBUG_GUI; private copies of
  disk images and a temporary boot configuration. `decorationtest --hold`
  passed and exited zero. The narrow-window refusal named window 5, `Narrow`,
  and the decoration minimum-width rule. Its publication-preservation checks
  passed; the other diagnostic branches were checked in code, not individually
  triggered in this run.
- Ctrl+Alt+T retained 500x240 client content. All 1,484 pixels of its bare
  border matched active `#d5a650`; after Alt+Tab, all matched inactive
  `#59616b`. See [guest results](review/qemu.txt),
  [active border](review/bare-active.png) and
  [inactive border](review/bare-inactive.png).
- `git diff --check` and `tools/stale_refs.sh` passed.

The color-picker comment was checked at #127's reviewed commit and already
limits its claim to setting the current RGB. That matches the implementation's
same-color early return; no correction was needed.
