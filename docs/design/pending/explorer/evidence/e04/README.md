# E4 evidence

Source `243663dc` on `codex/roam-e4`, base `5a521b63`. No P5 acceptance claimed.

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

## Native walkthrough and delivery

QEMU q35, four CPUs, 4 GiB, TCG, 1024×768; the unchanged E3 kernel boots ext2
root with a FAT volume and a separate ext2 `/home`. All disks are scratch copies
under `/tmp/roam-e4-native`; no repository or P5 disk was booted or modified.
Final binaries are identified in [package hashes](package-sha256.txt). All nine
[guest-installed binary hashes](installed-sha256.txt) match. The final
[os64get receipt](os64get.txt) reports 1 installed, 141 unchanged, 0 failed
after removing the guest copy of Roam. [Native exits](native-exit.txt) record
clean Roam, font driver and Appearance Workshop exits; an immediate close after
requesting the large directory also completed cleanly.

The [QMP driver](native_walkthrough.py) exercised actual native input and
rendering, separate from the controlled host fixtures. Main modes are `launch`,
`walk`, `history-scroll`, `panes`, `restore-panes`, `font`, `reset-font`, `file`
and `close`. The walkthrough confirms:

- ext2/FAT navigation, empty folders, UTF-8/spaced/long filenames and icons;
- repeated address input, filtered counts, hidden names and truthful errors;
- Back restores selection and scroll; Refresh preserves the last selected row;
- 10,001 real files yield 10,000 rows and a persistent incomplete notice;
- all surrounding panes can hide, Alt reveals the menu, and context recovery works;
- live 32px Interface font and Appearance Workshop's Midnight palette;
- file activation explains its unconfigured state without launching a process.

[Initial view](roam-initial.png), [FAT](roam-fat.png),
[restored selection](roam-restored-selection.png),
[restored scroll](roam-restored-scroll.png),
[failed navigation](roam-failed-navigation.png), [filter](roam-filtered.png),
[incomplete listing](roam-large.png), [minimal view](roam-minimal.png),
[Alt recovery](roam-menu-revealed.png), [right-click recovery](roam-context-recovery.png),
[32px font](roam-font32.png), [dark palette](roam-dark.png) and
[file activation](roam-file-message.png) preserve the visible results.

To reproduce, copy an E3 system image and data image, extract its ext2 partition
into `ext2.img`, then run [the fixture generator](make_fixtures.py) once against
those offline scratch copies. Install the new `/bin/roam`, `/lib` payload and
`etc/menu.conf` into ext2 and FAT, then put ext2 back at sector 133120. The FAT
partition starts at sector 2048. Repack the boot module as in E3's native recipe,
using the same GUI-enabled Limine entry and only the new scratch disk paths.
The fixture generator expects the scratch directory to exist and the fixtures
not to be present; inspect `fixture-ext2.txt` for debugfs errors.

Serve `userland/bin` and the icon lot through `os64serve.py`, then use guest
`os64get -a -n -f 10.0.2.2`. The client uses normal `/bin`, `/lib` and `@icons`
routing. Runtime checks are functional TCG evidence, not P5 latency measurements.

[P5 instructions](../../E04_HANDOFF.md) give the remaining hardware walkthrough.
