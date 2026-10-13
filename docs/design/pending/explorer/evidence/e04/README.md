# E4 evidence

Work in progress on `codex/roam-e4`, base `5a521b63`. No P5 acceptance claimed.

## Toolkit history restore

The real-font Details suite, including E4's detached selection clone and atomic
history restore, passes with ASan/UBSan/LeakSanitizer: [receipt](restore-host.txt).
New coverage injects failures at successive restore preparation points and
checks domain, selection/focus/anchor, viewport, live revision, pixels and lease
balance. Success retains compatible current column presentation, restores the
saved selection/viewport, and invalidates earlier live enumeration tokens.

Reproduce:

```sh
python3 tools/test_ui_dataview_host.py --real --output /tmp/roam-e4-restore-host
ASAN_OPTIONS=detect_leaks=1 /tmp/roam-e4-restore-host/test_ui_dataview userland/libfreetype/fixtures
```

The sandbox denied LSan thread attachment with EPERM and TracerPid 0. The same
binary passed outside the sandbox with leak detection enabled. Native results are recorded separately from these host checks.

## Directory, controller and application composition

- [Directory model](directory-host.txt): normalized/bounded paths, empty reads,
  malformed/duplicate names, real typed metadata, control-byte display escaping,
  raw-name refresh identity, hidden/filter choices, 10,001-entry truncation,
  cancellation, allocation/read/close refusal, concurrent raw references,
  mount parsing and Home fallback.
- [Browser controller](browser-host.txt): fake work scheduling drives reversed
  completion, two admitted jobs plus latest intent, cancellation, failed
  admission/publication/Back, filter changes during refresh, history eviction,
  stale activation and shutdown ownership. Data/projection/selection code is
  real; filesystem, pool and view preparation are controlled fixtures.
- [Roam UI](ui-host.txt): 60 checks with the production toolkit and FreeType;
  action/menu registration, repeat Ctrl+L down/up, address Ctrl+A, deferred
  activation, failed-navigation address restoration, column reorder, font
  refusal/32px/tiny bounds, overflow, visibility and persistent incomplete notice.
  The browser is a controlled fixture. Idle sync leaves painting clean; a new
  mount label with unchanged count damages its sidebar.

These suites pass ASan/UBSan/LeakSanitizer with leak detection enabled. Compile
in the sandbox, then run the same binaries outside it when thread attachment is
refused; do not disable leak checks.

```sh
python3 tools/test_directory_host.py --output /tmp/roam-directory
python3 tools/test_browser_host.py --output /tmp/roam-browser
python3 tools/test_roam_ui_host.py --output /tmp/roam-ui
make -C userland
```

The full userland build passes with warnings treated as errors. Kernel code and
ABI are unchanged. `stale_refs.sh` reported existing E3 Markdown link tokens and
historical E3 receipts, not retired code; every printed hit was inspected. No
new superlative comment claims were reported.
