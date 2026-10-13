# E3 evidence — 2026-10-13

Implementation branch `codex/explorer-e3-implementation`, fresh from
`origin/codex/explorer-e3-design` (`75227ada`), based on `userland` E2 merge
`f39b0ae0`. E3A is `ee24fb64`; E3B's original host/native/package receipts use `8b5d13a7`.
The clock diagnostic follow-up below has separate host and build evidence. No kernel or ABI source changed. Review and P5 acceptance are
pending in [draft PR #264](https://github.com/VBWizard/os64/pull/264) against `userland`; this record does not call the slice accepted.

## Acceptance matrix

| Check | Result | Receipt |
| --- | --- | --- |
| Detached data/projection/selection | PASS, 276 checks | [Core](core-host.txt) |
| Real font/widget behavior and renderer | PASS, 14,875 checks | [Real renderer](widget-real-host.txt), [transition map](INVARIANTS.md) |
| Existing menu/action/gallery consumers | PASS, 150,399 checks | [Menu regression](menu-regression.txt) |
| Existing font/text/listbox consumers | PASS, 1,626 checks | [Font regression](font-regression.txt) |
| PNG theme/catalog and cache diagnostics | PASS | [Icons](icons-regression.txt) |
| Production painting/damage | PASS, public renderer exercised with real fonts; no paint allocations, icon acquisition, preparation or I/O | `render`, `allocation_sweep`, `bounds_and_independence` in the host harness |
| Cross-build and normal FAT/ext2 installation | PASS; installed library and executable byte comparison | Final package hashes below |
| os64get delivery | PASS, transfer verification and installed-byte checks | QEMU-installed hashes below |
| QEMU PS/2, font/appearance, columns, resize, activation, independent view | PASS; final source, PS/2 and real PNGs | Native logs/screenshots below |
| P5 HID walkthrough and ordinary-event p95 | NOT RUN | Chris's walkthrough must identify installed hashes and its own revision |
| Independent review / merge | PENDING | PR against `userland`; E4 follows accepted E3 |

## Host reproduction

Ubuntu host under WSL2, Linux `6.18.33.2-microsoft-standard-WSL2`, x86_64.
GCC host 13.3.0, cross GCC 14.2.0, QEMU 8.2.2. Host C checks use `-O2`,
ASan/UBSan/LeakSanitizer and the production FreeType backend with the shipped
DejaVu Sans at nominal 17 px. The viewport is 640 by 420, at (11,13), in an
800 by 480 surface. Synthetic icon leases isolate acquisition/release faults;
the separate icon suite and native walkthrough use real shipped PNGs.

```sh
tools/test_ui_data_host.sh /tmp/e3-core
python3 tools/test_ui_dataview_host.py --real --output /tmp/e3-widget-real
python3 tools/test_ui_action_host.py --real --menu --output /tmp/e3-menu-regression
python3 tools/test_ui_text_host.py --real --output /tmp/e3-text-regression
tools/test_icons_theme_e2e_host.sh
```

Leak checks remain enabled. [The sandbox diagnosis](lsan-sandbox.txt) records
thread attachment EPERM; `TracerPid` was 0. The same binaries/scripts passed
outside the sandbox with leak detection enabled. No `detect_leaks=0` run is
used as acceptance evidence.

The replacement sweep denies 251 successive production preparation points,
checks the complete tuple, revision, sort descriptor, viewport and actual pixels
between attempts, then retries successfully. The font sweep covers 204 failure
points. Additional cases cover failed scroll followed by failed font adoption,
failed resize/retry, failed reveal/columns, partial icon/text cleanup, missing
icons, context targeting, interrupted PS/2/HID, orphan burst bytes, held keys
across replacement and focus loss, reface/generic teardown, BUSY callbacks,
preparation before joining a UI, and two independent views. The gallery's
font-error test verifies that deferred notice text uses the committed font,
through the production dispatch boundary after candidate discard.

## Resource and performance measurements

For 10,000 rows in the host fixture:

| Measurement | Bytes / count / microseconds |
| --- | --- |
| Snapshot-owned bytes | 3,721,810 |
| Live view metadata | 92,632 |
| Peak live plus candidate metadata | 192,880 |
| Live font engine requested bytes | 1,377,231 |
| Font engine requested-byte high-water | 1,448,324 |
| Prepared text runs / live icon leases / visible rows | 52 / 12 / 12 |
| Construction, including font preparation | 30,087 us |
| Size descending sort plus publication | 4,869 us |
| No-match prefix input | 714 us |
| 100 scroll-to-paint events after 10 warmups: median / p95 / maximum | 1,727 / 1,947 / 2,793 us |

These are host results, not P5 latency. Font counts include context-owned
faces, source copies, runs and retained caches; they use the font allocator's
requested sizes. Model/view counters include actual heap capacities and their
16-byte headers. Immutable objects referenced twice by the same transaction
are charged once. The app-owned snapshot is separate from the 16 MiB view
budget; producer input buffers and shared font/icon engines have separate
lifetimes and accounting.

The public maximum fixture builds 100,000 rows; the compact one-column core
fixture owns 12,848,717 snapshot bytes and an 800,072-byte projection.
100,001-row snapshot and projection requests return LIMIT. Real oversized
heap allocations test capacity accounting and reuse of the same immutable
projection without duplicate charges. Painting 100 and 10,000 rows at fixed
bounds visits the same viewport capacity, including a partial row; selected
count does not determine paint work. Repeated replacement and create/dispose
cycles release runs and successful leases, and context teardown reaches zero.
Shared glyph and PNG cache retention is measured separately from leaks.

## Native reproduction and scope

Use the normal image recipes with scratch destinations, reusing unchanged
kernel/Limine artifacts and skipping the kernel dependency fetch:

```sh
make -j8 -o kernel -o limine/limine \
  DISK_IMAGE=/tmp/e3-native/final-os64.img \
  EXT2_TEST_IMAGE=/tmp/e3-native/final-ext2.img \
  EXT2_STAGING=/tmp/e3-native/final-staging \
  /tmp/e3-native/final-os64.img
```

The initial window title exceeded the ABI's 31-byte payload and was shortened
before native acceptance. The first scratch VM reached its one-hour timeout;
final receipts come from a restarted VM with the final packaged image.
The final source and installed hashes, command log, controller and screenshots
are linked below. QEMU uses q35, 4 vCPUs, 4 GiB and PS/2 input; it does not
substitute for P5's HID, appearance or latency report.

P5 is not run. Deploy both the library and gallery before Chris follows
[E03_HANDOFF.md](../../E03_HANDOFF.md#evidence-and-acceptance). Preserve his
installed hashes, revision and performance samples as a separate receipt.

## Final native receipts

The final source is [hashed here](source-sha256.txt). Both image recipes match
[the package hashes](package-sha256.txt); guest-read copies from `/lib` and
`/tests` match [the installed hashes](native-installed-sha256.txt). The native
walkthrough and transfer use source commit `8b5d13a7`. No later code change is
included in this report.

[Native output](native.txt) records 100 ordinary events after 10 warmups:
median 46,194 us, p95 48,486 us, maximum 49,554 us. The requested window is
1000 by 720; the initial table retains 96 runs and 23 icon leases. At the
10,000-row timing point, the snapshot owns 3,723,216 bytes, live/peak view
metadata is 93,624/106,216 bytes, and live/peak requested font allocations are
214,506/406,544 bytes. The PNG pixel cache holds 2,080 bytes at 16 px and
10,304 bytes after the 32 px font walkthrough. QEMU is TCG on this host;
these values are not P5 latency or acceptance. Header sort/publication takes
37,261/37,913 us in the recorded directions; matching prefix input is timed
separately in the same log. The host report separately measures no-match input.
The native 100,000-row fixture owns 37,044,176 snapshot bytes and constructs/
publishes in 2,920,044 us. Its UI stays virtualized.

The [os64get transfer](os64get-transfer.txt) fetched and CRC-verified the four
served files; the final image already held identical bytes. [The fresh-install receipt](os64get-install.txt) removes the gallery from
`/tests`, fetches and installs it through the `@tests` routing, and reports
1 installed, 3 unchanged, 0 failed; the identical libraries remain installed. No P5 deployment or
installed-binary claim is made.

Reproduce the scratch GUI ISO and VM with the recorded [Limine input](limine.conf):

```sh
xorriso -indev os64_kernel.iso -outdev /tmp/e3-native/final.iso \
  -boot_image any replay -map /tmp/e3-native/limine.conf /boot/limine/limine.conf \
  -map /tmp/e3-native/final-os64.img /boot/os64_disk.img
qemu-system-x86_64 -machine q35 -cpu qemu64,+rdrand,+rdseed -m 4g -smp 4 \
  -no-reboot -cdrom /tmp/e3-native/final.iso -boot d -display none \
  -serial file:/tmp/e3-native/serial.log \
  -qmp unix:/tmp/e3-native/qmp.sock,server=on,wait=off \
  -drive file=/tmp/e3-native/os64.img,format=raw,if=none,id=nvme1 \
  -device nvme,drive=nvme1,serial=nvme1-serial \
  -drive file=/tmp/e3-native/data.img,format=raw,if=none,id=data1 \
  -device nvme,drive=data1,serial=data1-serial \
  -netdev user,id=e3net -device e1000,netdev=e3net
```

Use copies of the final system image and an existing data image; do not boot
repository disks directly. Copy [the QMP controller](native_walkthrough.py)
to `/tmp/e3-native/control.py`. After the shell is ready, its modes are `launch`,
`benchmark`, `sort`, `selection`, `filter`, `columns`, `second`, `empty`, `one`,
`max`, `tenk`, `font`, `resize`, `close`, `launch-dark`, `legacy` and `icons`.
`resize` uses the platform's Ctrl+Alt right-drag gesture. Serve the library lot
and the fixture as `tests`; fetch with `os64get -a -n -f 10.0.2.2`. Copy installed
files to guest `/home`, run `sync`, then extract that data partition for hashes.

Screenshots show:

- [Initial Details](details-initial.png), [warmed timing](details-benchmark.png),
  [Size descending with folders first](details-size-descending.png).
- [Selection](details-selection.png), [end/activation](details-end-activation.png),
  [typeahead](details-typeahead.png), [filter](details-filtered.png),
  [cleared filter](details-filter-cleared.png), [refresh](details-refreshed.png)
  and [identity reset](details-domain-reset.png).
- [Column resize](details-column-resized.png), [header menu](details-header-context.png)
  and [popup input precedence](details-popup-consumes-keys.png).
- [Second view](details-second-view.png), [independent navigation](details-second-independent.png)
  and [closed second view](details-second-closed.png).
- [Empty](details-empty.png), [one row](details-one.png), [100,000 rows](details-max.png),
  [Interface 32](details-font32.png), [appearance](details-appearance.png),
  [small window](details-small-window.png), [last row](details-small-last-row.png)
  and [dark](details-dark.png).
- [Existing gallery menu](legacy-menu.png) and [existing icon page](legacy-icons.png).

The [core stale-reference scan](stale-core.txt) is clean. The implementation
scan reports the live E03 contract/handoff Markdown link names, inspected as
valid references; no API was retired and no added superlative claim was found.

## Clock diagnostic follow-up — 2026-10-13

Chris requested separate not-yet-queried and unavailable clock states. Details
now exposes `clock_queried` alongside last-query availability, resets both when
its clock provider changes, and refreshes the gallery diagnostic at the dispatch
boundary even when a clock query accompanies a no-match search. The initial
label is “clock not checked yet”; a failed query says “clock unavailable”; a
successful query removes the label.

Typeahead behavior remains the contract's growing prefix, single-letter cycling,
one-second idle expiry, and fresh-letter fallback if the clock is unavailable.
The regression now extends `a` through `al`, `alp`, and `alph`, verifying that
`alph` moves from an Alpine match to an Alpha match. The gallery regression
uses the production dispatcher to verify initial, failed and recovered clock
states without a selection change.

[The production-font ASan/UBSan/LeakSanitizer run](clock-followup-host.txt)
passes 14,948 checks. FreeType objects from the preceding full real-font run
were reused for the final harness build; leak detection stayed enabled outside
the sandbox. Cross-building `libos64.so` and `actiongallery` passes. The refreshed
[acceptance bundle hashes](clock-followup-bundle-sha256.txt) identify the four
files served from `/tmp/e3-wire`. These are build hashes; the original native
receipts above remain tied to `8b5d13a7`. This follow-up has not been installed
or walked through on P5.
