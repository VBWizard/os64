# F5 — configurable fonts, live adoption and Appearance Workshop

**Implementation: `419b36d`; image build fix: `bda3dc5`**, branch `codex/font-settings`, worktree
`.worktrees/font-settings`. Integration parent **`07d8a69`** merges F3
`ab7a1cd` onto accepted F4 `3bf7b1f`. F5 implementation and verification are
complete for combined review. Fable's F3/F5 implementation review is pending;
no approval, mainline merge, push or hardware acceptance is claimed.

Chris authorized finishing this integrated branch before asking Fable to review
it. [The review packet](FABLE-FONTS-REVIEW.md) identifies the boundaries and
provenance. [The overview](FONTS-OVERVIEW.md) explains the feature without
requiring its implementation vocabulary.

## Delivered behavior

- Workshop has a Fonts page with independent Interface, Terminal and Document
  roles, 8–96 pixel outline sizes, a three-role preview, Refresh, Install, Apply,
  Save, one-step Undo and Reset. Builtin stays 8×16. Invalid terminal choices
  explain the fixed-width requirement and retain the previous preview.
- Installation accepts an absolute source path, stages a bounded copy in the
  top configuration directory's `fonts/`, validates the complete role set,
  syncs and publishes with no-replace rename. An existing filename is refused.
  Installation alone neither applies nor saves a choice. No rebuild is needed.
- `fonts.conf` is strict, case-insensitive for keys, last-wins for valid repeated
  keys, and first-file-wins on the system ladder. Relative faces resolve beside
  that file. Bad lines reject the whole candidate with original line/role/source
  provenance. Save writes absolute paths and removes obsolete fallback keys.
- Discovery includes builtin, selected files and the selected configuration
  folder's `fonts/`; it bounds scans at 256 entries, results at 128 assets,
  sources at 32 MiB each and source reads at 64 MiB. File identity is the path
  for selection, fresh content instances for the provider; labels include the
  filename. The filesystem iterator is not recursive or extension-filtered.
- Apply uses generation-checked `/sys/appearance` publication. Save changes
  next-startup choices while pinning the complete old session first. Empty-store
  font Apply preserves application-specific color defaults. Palette/treatment
  writers preserve font and unknown future components, including raw unowned
  lines and comments. Named theme Save/Save As carries the raw envelope.
- Production Scribe and gterm follow the shared settings through their F4/F3
  consumers. Fixed-layout libui tools refuse rows that do not fit. gclock stages
  a measured clock label. gterm caps its preferred startup frame to the display
  before computing its grid and PTY dimensions. A refused adoption remains
  retryable; success of publication is not acknowledgement from every process.
- Normal FAT/ext2 image recipes install builtin-default `etc/fonts.conf`,
  licensed DejaVu Sans and DejaVu Sans Mono under `/etc/fonts/`, and their license
  under `/etc/licenses/`. The pinned test fixtures remain available separately.

## Contract and ownership changes

`fonts.serial` is a concrete F5 transport addition for Fable's review. It is a
positive uint64 stamped with the font publication generation. Explicit font
Apply reloads even unchanged paths/sizes; unrelated color changes preserve the
serial. Legacy components without it fall back to the envelope generation. It
is not a `fonts.conf` key. [FONT_SETTINGS.md](../../FONT_SETTINGS.md) and the
[contract addendum](../../FONT_CONTRACTS.md#f5-implementation-addendum-for-combined-review)
record this addition.

New public boundaries are `font_config.h` (parse/load/prepare/discover/install),
`font_settings.h` (current/Apply/Save), `os64_ui_font_follow`, raw theme snapshot
load/save, and checked configuration-key removal. Font-provider, text-layout and
backend APIs are unchanged. F5 owns the coordinated ui_session/theme/saved and
Workshop changes; F3/F4 are integrated before their production hookup is edited.
No kernel code or syscall changed.

## Validation

### Clean image build follow-up

Chris's `make clean && make` exposed a FAT packaging failure after `db3fe41`:
the recipe copied `fonts.conf` before creating `/etc`. The earlier userland
build and guest payload refresh did not exercise that image recipe. Product
font installation now follows parent-directory creation, and failure to create
`/etc/fonts` is fatal. The ext2 sibling already creates its parents first.

The exact clean root build failed with exit 2 before the fix and passed with
exit 0 afterward, including ISO assembly. Four linker RWX warnings remain for
kernel test fixtures (`arg_echo`, `stat_test`, `env_fill`, `glutton`); no build
errors remain. [Build diagnostic excerpts](f5-evidence/clean-build/build-excerpts.txt)
retain both outcomes and full local log hashes.

[Package checks](f5-evidence/clean-build/package-check.txt) compare configuration,
both product fonts, their license, Workshop and both font libraries against
source/build files in FAT and ext2. Private copies of the freshly generated boot
and home disks were then booted with the freshly generated ISO, without payload
injection. The existing guest harness ran with `--resume --label clean` to skip
its installation phase. Workshop discovered both faces and applied DejaVu Sans
to the interface; screenshots were inspected. The guest shut down with exit 0.
[Initial Fonts page](f5-evidence/clean-build/fonts.png),
[applied interface](f5-evidence/clean-build/fonts-applied.png),
[actions](f5-evidence/clean-build/actions.txt),
[serial receipt](f5-evidence/clean-build/serial.txt).
Existing home data and the three root-level backup images were preserved.

### Original implementation checks

All host suites below use ASan and UBSan. `ASAN_OPTIONS=detect_leaks=0` disables
LSan; the configuration and terminal suites additionally prove their own
allocation-ledger cleanup. Counts are assertions, including per-allocation and
per-pixel checks, not numbers of independent scenarios.

| Check | Result / receipt |
|---|---|
| Strict cross-build | Complete userland build passed with `-Wall -Wextra -Werror`; final UI wording and terminal startup-fit rebuilds passed. [Build logs](f5-evidence/build.txt), [UI](f5-evidence/build-ui-message.txt), [terminal](f5-evidence/build-terminal-fit.txt) |
| Config/provider/discovery | 1,407,755 assertions, zero failures, 1,141 allocation-denial positions, no live allocations. Fresh pinned backend objects. [Receipt](f5-evidence/host-config.txt) |
| Installation/persistence | Native filesystem: staged copy, unsuitable terminal, malformed font, read/write/sync failures, cleanup, existing-name refusal, deleted duplicate fallback keys, comments, independent Apply/Save, atomic replacement refusal and a fresh-process startup read. [Receipt](f5-evidence/host-settings.txt) |
| Appearance/session | Component CAS and conflicts, serial preservation/reload, unknown lines, malformed keys, 4096-byte refusal, first font Apply preserving app defaults, startup pinning, UI pages and interaction regressions. [Receipt](f5-evidence/host-appearance.txt) |
| Named saved compositions | Raw font/future/comment preservation through Save As and replace, full schema, concurrent create, checked writer and startup failures. [Receipt](f5-evidence/host-saved.txt) |
| Real widgets | 1,376 checks, zero failures. [Receipt](f5-evidence/host-ui.txt) |
| Scribe | 2,470 checks, zero failures. [Receipt](f5-evidence/host-scribe.txt) |
| Terminal | 2,406,573 checks, 4,482 allocation-denial cases, zero live bytes. [Receipt](f5-evidence/host-gterm.txt) |
| Link boundaries | Applications depend on libos64; libos64 depends on libfreetype. [ELF receipt](f5-evidence/elf-dependencies.txt) |
| Diff/prose | `git diff --check` passed; staged stale-reference scan reported no retired references or new superlative claims. [Receipt](f5-evidence/stale-refs.txt) |

Host commands and guest reproduction are in [f5-evidence/README.md](f5-evidence/README.md).
Source and binary digests identify the tested checkpoint. The native filesystem
settings test reuses freshly built config-suite FreeType objects; widget,
Scribe and terminal scripts build their own backend objects.

## QEMU acceptance

Private copies of the accepted F4 disks and ISO were used with q35, 8 vCPUs,
8 GiB, two NVMe devices and a 1024×768 GUI. Both userland roots were refreshed
before the initial boot; existing user disks and P5 were untouched. This tests
the first-upgrade reboot boundary, not compatibility with running old libraries.

The retained screenshots were inspected:

1. Select DejaVu Sans for Interface; install Source Sans 3 CFF through the actual
   Workshop path field; select it for Document; preview and Apply. The corrected
   first publication preserves Workshop's colors.
   [Installed preview](f5-evidence/02-installed-cff.png),
   [live Apply](f5-evidence/03-fonts-applied-colors-preserved.png).
2. Open real Scribe, insert unsaved text, select it, then publish 20- and
   28-pixel proportional document fonts while a real gterm is open. Selection,
   unsaved bytes and horizontal scrolling remain coherent. Help opens/returns;
   the caret is visible. [Before](f5-evidence/04-scribe-unsaved-selected.png),
   [after](f5-evidence/08-scribe-large-selected.png),
   [help return](f5-evidence/10-scribe-help-return.png).
3. Real gterm changes from builtin to DejaVu Sans Mono at 16 and 24 pixels;
   its prompt and existing output remain usable. [16px](f5-evidence/06-terminal-mono16.png),
   [24px](f5-evidence/07-terminal-mono24.png).
4. Workshop refuses proportional terminal text, preserving its valid selection;
   a larger document draft previews and saves for startup. Public API guest
   checks prove refusal leaves serial unchanged, palette Apply preserves it,
   explicit reload changes it and Save leaves the current session alone.
   [User-facing refusal](f5-evidence/18-friendly-terminal-refusal.png),
   [API receipt](f5-evidence/f5-api.txt).
5. A cold boot reads UI 16 / terminal 24 / document 28 at session serial zero.
   Scribe opens the saved file at that size. The final gterm cold-start check
   records a fully visible 992×704 frame at (16,32), fixing the oversized preferred
   100-column frame exposed by the first reboot.
   [Startup receipt](f5-evidence/f5-reboot.txt),
   [Scribe](f5-evidence/17-reboot-scribe.png),
   [fitted terminal](f5-evidence/19-reboot-terminal-fitted.png).
6. Offline byte comparison proves Scribe saved exactly the original UTF-8 bytes
   plus the inserted text/newline, and the installed CFF equals its source.
   Both ext2 filesystems pass `e2fsck -fn`.

The FAT read-only checker reports a four-cluster free-space-summary discrepancy,
not a clean result. A separate boot/shutdown of **unmodified F4 images** produces
the same four-cluster discrepancy; their pre-boot image checks clean. This is
isolated as inherited boot/filesystem behavior, not attributed to fonts or
silently repaired. [Before](f5-evidence/fat-baseline-before.txt),
[baseline boot](f5-evidence/fat-baseline-boot.txt),
[F5 check](f5-evidence/fat-check.txt). No kernel repair is included.

The initial guest used the complete implementation before two final app-only
repairs: clearer refusal wording and gterm's screen cap. The second boot verifies
final wording, and the third verifies final gterm startup. The library is the
same across these runs. Per-boot payload digests and action streams preserve
that distinction; the final binary manifest matches `419b36d`.

## Limits and next review

- Fable's combined review remains pending, particularly F3 and `fonts.serial`.
- No P5/hardware validation, mainline merge, push or PR is claimed.
- Fallbacks are configuration-editable; the UI edits primary faces and sizes.
  Discovery is bounded to the selected folder/selected files. Installation does
  not overwrite existing names; uninstall and font browsing across many folders
  are outside this slice.
- Fixed-layout tools may retain their current fonts at larger sizes. Scribe
  and gterm have application-specific layout/grid transactions. Retry by Apply.
- Chris deferred the Workshop maximize/Apply/restore inconsistency on
  2026-09-20. His 1024×768 observation and the limits of source inspection are
  recorded in the [handoff checkpoint](FABLE-FONTS-REVIEW.md#handoff-checkpoint--2026-09-20).
  This is not a claim that resizing revalidates font fit or that higher
  resolution resolves the behavior.
- The inherited Western UTF-8/symbol profile, bitmap compatibility fallback,
  large-line Scribe extent debt, and non-Unicode PTY remain as documented in
  F0–F4. Complex shaping, window decorations, kernel console/titlebar fonts and
  direct bitmap applications are outside F5.

Ready for [the combined Fable review](FABLE-FONTS-REVIEW.md).
