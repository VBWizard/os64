# E2 evidence — 2026-10-13

Branch `codex/explorer-e2`, based on E1C merge `faa321a5`. This packet changes
userland, assets and image packaging; no kernel source or ABI changes.

## Host and build results

| Check | Result | Receipt |
| --- | --- | --- |
| Real PNG catalog, cache, leases, limits and fault injection | PASS | [Catalog](catalog-host.txt) |
| Real config parser + theme resolver + catalog + shipped PNGs | PASS | [End to end](theme-e2e-host.txt) |
| Resolver ownership, metadata cap, fallbacks and failures | PASS | [Resolver](theme-stub-host.txt) |
| Scribe/grootmenu consumer harness | 1,550 checks, 0 failures | [Consumers](consumers-host.txt) |
| Menu/action/status/gallery host suite | 150,351 checks, 0 failures | [Toolkit/gallery](menu-gallery-host.txt) |
| Source/export dimensions, alpha, byte cap, provenance and deterministic archive | PASS | [Assets](assets.txt) |
| Actual FAT and ext2 assembly recipes | 130 PNGs, metadata, config and final libimage byte-identical | [Packaging](packaging.txt) |
| Native archive extraction | Exit 0; 133 extracted files byte-identical | [Extraction](native-extraction.txt) |

Host C suites use ASan/UBSan with leak detection enabled. Sandbox LSan thread
attachment returned EPERM despite TracerPid 0; successful runs were outside
that sandbox. Reproduce from the checkout:

```sh
tools/test_icons_host.sh
tools/test_icons_theme_e2e_host.sh
tools/test_icons_theme_stub_host.sh
python3 tools/test_scribe_host.py --menus
python3 tools/test_ui_action_host.py --real --menu
python3 tools/build_icon_assets.py --archive output/icons/roam-classic.tar.gz
```

Cross-builds succeeded for libimage, Scribe, grootmenu, actiongallery and
scribefonttest. The packaging checks ran the root GNUmakefile recipes into
scratch images. They reused existing kernel/Limine artifacts and skipped the
unrelated kernel dependency fetch. Added, changed and deleted PNG dependency
probes each scheduled both images for rebuild; installed membership matched.
No repository disk image was changed.

`git diff --check` passed. `tools/stale_refs.sh` was run before both logical
commits; its reported names are existing E1 handoff/evidence paths and historical
audit receipts. Each hit was reviewed; no interface was retired and no added
superlative comment was reported.

The catalog harness measures 103,083 bytes of cold-load allocation and 117,995
bytes for its 16-bit RGBA 64 px case, including the host allocation header.
These are tested transient allocations, not whole-process RSS or proof of a
universal heap-arena bound. Retained caches charge actual Os64 allocation
capacity plus headers. Selected and default catalogs have separate budgets.
The toolkit suite uses synthetic providers; the end-to-end suite separately
runs the real config parser/catalog/PNG decoder, including selected pixels,
missing-key fallback, corrupt PNG refusal, BUSY teardown, allocation failures
and retry after an existing file cannot be opened.

## QEMU walkthrough

Scratch q35 VM, 4 vCPUs, 4 GiB, PS/2 input, E1K-compatible existing ISO.
The VM was stopped after testing. Final built-library hashes are in
[final-sha256.txt](final-sha256.txt); the staged final libimage was extracted
and compared after shutdown. The gallery, Scribe and desktop smoke checks
were repeated after the last provider open-failure fix:

- [Final gallery](gallery-final.png): selected folder, file, drive and app at
  48/64 px; the last app card demonstrates disabled rendering.
- [Final Scribe](scribe-final.png): File menu with 16 px icons and readable labels.
- [Final desktop menu](desktop-final.png): launcher icons through the shared menu.

The following screenshots precede that narrowly scoped open-failure fix;
artwork and consumer code match the final revision:

- [Light gallery](gallery-light.png) and [dark gallery](gallery-dark.png).
- [Extracted set](gallery-installed-set.png): Os64's own `gunzip -c archive |
  tar -x -f -` extracted a USTAR archive in `/home/icons`; `icons.conf` selected
  the resulting folder. The files were read back and compared after shutdown.
- [Partial set](gallery-partial-set.png): replacement 64 px folder artwork
  intentionally uses the drive; the missing 48 px folder and other keys use
  the shipped family.
- [Scribe at Interface 32](scribe-icons32.png): larger rows and 32 px icons.
- [Missing assets](scribe-missing-assets.png): renaming the default directory
  succeeds and Scribe retains a usable text menu; the folder was restored.

Repeated lifetime, eviction, cache-hit and teardown assertions are host
coverage. The native walkthrough demonstrates visuals and interaction, not
an instrumented long-running memory soak. Native missing-directory fallback
was exercised in Scribe; the toolkit host suite covers absent providers.

## Limits and next acceptance

Chris has confirmed visual acceptance of the installed P5 gallery after the
os64get correction, specifically praising disabled rendering. This is a user
report without an installed-binary hash; it predates review 1 below.
To walk this build on P5:

1. Install the branch's userland, libraries, `/etc/icons/default` and
   `/etc/icons.conf` through normal image assembly.
2. Open Scribe's menus and right-click the desktop; check small icons and text.
3. Run `/tests/actiongallery --icons-page`; press D for dark/light and click
   cards. Check 48/64 px readability and the disabled card.
4. Follow [set installation](../../../../../../assets/icons/README.md), select
   another or partial folder in `/home/icons.conf`, and start a new instance.

The setting takes effect in new instances; there is no GUI chooser or live
switch. Assets are resampled exports of generated raster masters, not separately
hinted artwork for each size. The open/stat ABI cannot prove absence when both
lookups fail, so that ambiguous case maps to MISSING; a successful stat after
an open failure produces retryable IO. Decode failures retain their typed
status and do not silently substitute default artwork. Desktop icon placement
and activation remain outside E2.


## P5 delivery follow-up — os64get icon collection

Chris reported an icon-free gallery on P5 after the initial publication. E2
packaged disk images but omitted the valet delivery path. The artwork lives in
`assets/icons/default` as PNG files. Merely adding `@icons` is insufficient:
existing suffix rules route PNGs to `/home/images` and `theme.conf` to `/etc`.

The follow-up adds `@icons/*` routing between exact filenames and suffix rules,
while retaining ordinary lot behavior. The shipped config and server examples
include the collection; [P5 bootstrap instructions](../../../../../../assets/icons/README.md#p5-updates-through-os64get)
cover fetching the new client first, creating the destination and updating the
effective user config. No automatic directory creation or installer behavior
was changed.

The routing regression failed before the fix. It now passes precedence,
last-match, nonmatching-lot, absent-lot and all 133 shipped-file assertions.
Existing single/batch installation smoke tests also pass with ASan/UBSan/LSan:
[host results](wire/host.txt). The cross-built client downloaded the collection
from the real os64serve into an empty `/etc/icons/wire` on scratch QEMU:
[133 installed, exit 0](wire/native.txt). Downloaded PNGs and metadata were
extracted after shutdown and compared byte for byte with the sources. A new
gallery instance selected that directory and [rendered the downloaded set](wire/gallery.png).
The [tested client hash](wire/os64get-sha256.txt) records the native binary.
The temporary VM and loopback server were stopped. Chris subsequently confirmed
the P5 gallery visually accepted, as recorded in review 1 below.


## Review 1 — preserve transient theme creation failures

[Finding](https://github.com/VBWizard/os64/pull/261#discussion_r4259064423): a
failed manifest read/parse returned the same boolean as invalid contents,
allowing successful construction of a default theme after a transient failure.
The [regression failed before the fix](review-r1/before.txt).

`read_manifest` now returns a typed icon status. Creation maps allocation
failure to `OS64_UI_NO_MEMORY` and detected read failure to `OS64_UI_ERROR`,
leaves the caller's theme NULL, and cleans up temporary catalogs/metadata.
Both manifest paths, configuration errors and selected catalog construction
errors are covered. Missing/invalid manifests retain their fallback policy.
Open failure with successful stat is an I/O error; failed stat retains the
existing lookup ambiguity, documented in the public contract.

- [Resolver fault tests](review-r1/resolver-host.txt): selected/default manifest
  open/read/allocation/parse failures and config IO/NO_MEMORY; clearing each
  fault permits a retry that recovers the selected family.
- [Real-parser tests](review-r1/real-parser-host.txt): allocation sweep through
  actual config and manifest parser buffers (at least nine failure sites),
  NULL outputs and zero live allocations after failure, successful retry,
  open/read errors on both manifests, and existing missing/invalid fallback.
- Both suites pass ASan/UBSan/LSan outside the sandbox with leak detection on.
  libimage, Scribe, grootmenu and actiongallery cross-build successfully.
- [Native gallery smoke check](review-r1/gallery.png) uses the revised library
  and the set previously delivered through os64get. The stopped scratch VM's
  libimage matches the [recorded binary hash](review-r1/sha256.txt). Transient
  fault coverage is from the host tests; the native check verifies normal use.

The pre-commit stale-reference audit reported existing evidence links rather
than retired interfaces; each hit was checked. No new superlative comments
were reported.

Chris's P5 gallery approval applies to the preceding installed revision,
including the disabled app state; it does not claim hardware fault injection
or acceptance of this later error-handling change.
