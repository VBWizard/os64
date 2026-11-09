# F1 implementation review — Quinn

2026-09-16 (America/New_York). Candidate: local `opus/freetype-backend`,
base `3b82356413ab8f183bd6506febe8d06fea6e7de0`. F1 remains uncommitted.
The backend contract is table revision 1, frozen in F0 commit
`23bf6dddfd1077bf844c661d8762a9b52e3a68f9`.

This reviews the standalone backend, including the earlier B1 allocation
correction. It does not accept the later layout, terminal, Scribe or font
configuration implementations. The production `libos64 -> libfreetype` edge
and its spawn-latency gate still belong to F2.

## Corrections made during this review

| ID | Trigger and previous behavior | Correction and regression |
|---|---|---|
| I1 | A UTF-16 name ended with NUL and an odd trailing byte; conversion appended U+FFFD after the terminator. | Stop conversion immediately at NUL. Tests cover this case, isolated surrogates, valid surrogate pairs, odd lengths, exact-fit and truncated UTF-8 output. |
| I2 | Lazy allocation of SFNT name strings failed, but `face_open` returned OK with incomplete names. | Propagate the recorded allocation refusal after metadata reads and destroy the incomplete face. The synthetic-name denial sweep reproduced two such sites and checks cleanup. |
| I3 | Platform ranking selected a BMP map ahead of a full-repertoire Unicode map, losing U+10000. | Rank full-repertoire formats 8/10/12/13 above BMP formats; skip variation-selector format 14 as a base map. Synthetic two-map fonts exercise all four full-repertoire formats. The original comment incorrectly treated the Unicode platform itself as evidence of coverage. |
| I4 | A valid one-face TTC passed a `num_faces > 1` collection check. | Reject the `ttcf` container signature independently of face count. A one-face collection built from the pinned TrueType fixture must return UNSUPPORTED. |
| I5 | A bare CFF1 program could open through a synthesized Unicode map despite the static-SFNT contract. | Require `FT_IS_SFNT`. A CFF table extracted from the pinned Source Sans fixture must be refused outside its SFNT container. |
| I6 | The negative-pitch copy branch read before the bitmap allocation. | Locate the top row at the high-address end of the buffer. A test wrapper reverses the real rasterizer's row storage and requires the same top-down output mask. ASan reproduced the old out-of-bounds read. The shipped smooth renderer uses positive pitch, so this was a latent branch defect, not a demonstrated crash from an accepted font. |
| I7 | Changing only a font fixture or the engine license left the ext2 image stale. | Add both to the ext2 target's prerequisites. Dry runs suppress the recursive binary builders to isolate the image dependency and demonstrate before/after behavior. |
| I8 | The FAT image carried the backend but omitted `/etc/licenses/freetype.txt`. | Install the engine notice on FAT as well as ext2; extract and byte-compare it on both fresh images. |

These are adapter and packaging fixes; imported FreeType source and patches
are unchanged. Name selection comments now describe deterministic language
preferences without claiming that a designer's name must be English or ASCII.
The backend header remains byte-identical to the frozen F0 header.

The earlier B1 tests remain in the suite: module installation must fail if an
allocation was suppressed internally; cap refusal is LIMIT without calling the
allocator; callback refusal is NO_MEMORY; cleanup and retry must work.

## Fable's requested checks

**Bare CFF2:** the test feeds a CFF2 header and minimal index scaffold directly,
without an SFNT directory, and gets UNSUPPORTED. This is an early-version-refusal
test, not a claim that the scaffold is a complete valid CFF2 font. The source
trace is decisive: `cffobjs.c` enables CFF2 loading only after finding an SFNT
CFF2 table; the raw path keeps `cff2` false, and `cffload.c` rejects a raw major
version other than 1 before parsing its body. SFNT CFF2 refusal has a separate
existing test.

**Private target runtime:** `runtime_test.c` exercises copying, overlapping
moves, comparison, fill, sorting and nonlocal return. It is linked with the
same production runtime and assembly objects used in `libfreetype.so`, and
runs both in guest `fonttest` and a small Linux launcher that supplies only
entry/exit. Neither execution substitutes host libc for the private aliases.
The sanitizer harness still uses host memory interceptors for parser coverage.

The anticipated functional failure from removing
`-fno-tree-loop-distribute-patterns` **did not reproduce** with target GCC 14.2.0
and the remaining freestanding flags: that variant also returned zero. The
flag remains an explicit build requirement, checked independently by
`tools/test_freetype_target_runtime.py --mutation`. This is a policy tripwire
plus a functional runtime regression, not evidence that flag removal crashes
this compiler. The port/build comments were corrected accordingly.

**Names:** tests exercise valid and malformed UTF-16, embedded NUL, odd length,
UTF-8 scalar-boundary truncation, preferred/legacy IDs, platform/language
ranking, duplicate tie selection, and allocation failure during name loading.

## Evidence and reproduction

All paths below are relative to `docs/fonts/f1-evidence/`.

- `impl-host-O2.log` and `impl-host-O0.log`: final ASan+UBSan runs,
  **1086 checks, zero failures each**. Commands:
  `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_freetype_host.py -O2`
  and the same with `-O0`. LSan is disabled under tracing; callback accounting
  still checks live bytes and free sizes. These are directed tests and denial
  sweeps over the exercised paths, not an exhaustive parser audit or fuzz run.
- `impl-before.log`: initial name/map regressions fail before correction.
  `impl-container-before.log`: the two container acceptance failures.
  `impl-map-siblings-before.log`: formats 8 and 10 expose the incomplete first
  map-selection fix. `impl-pitch-before.log`: ASan's negative-pitch read failure.
  `impl-backend-fixes.patch` records the adapter delta from the B1 candidate.
- `impl-target-runtime.log`: production-object execution, flag-policy rejection,
  and the separately reported flag-removal observation.
- `impl-provenance.log`: verify the retained release archive's pinned SHA256,
  extract into a fresh temporary directory, select the import modules, apply
  the eleven recorded patches, and compare all 334 files against both the
  working import and manifest. `python3 tools/import_freetype.py --verify`
  also passes. This is independent re-derivation of the pinned import, not a
  fresh online survey of upstream changes after Opus's audit cutoff.
- `impl-build.log` and `impl-final-build.log`: strict cross-build and image/ISO
  generation, using private image paths. Existing unrelated objects were
  reused; this was not a clean-room whole-OS rebuild. The complete backend was
  recompiled after the build rule changed. No `NOWERROR=1` escape was used.
- `impl-elf.log`: one export (`os64_freetype_backend_v1`), no DT_NEEDED or
  undefined dynamic symbols, relative relocations, separate R/RX/RW segments.
- `impl-image-dependencies.log` and `impl-image-payloads.log`: isolated
  dependency regression and byte comparisons of the backend, guest test,
  four fonts, three font-license files and engine notice on FAT/ext2.
- `impl-text-serial.log`, `impl-text-fonttest.log`, `impl-text-pass.png`:
  fresh text boot on the ext2 root; target runtime PASS, fonttest PASS,
  ASCII coverage masks inspected. GUI specimen correctly skips on this boot.
- `impl-gui-serial.log`, `impl-gui-fonttest.log`, `impl-gui-specimen.png`:
  fresh GUI boot on the FAT root; target runtime PASS, fonttest PASS, specimen
  inspected with all four faces at 12/16/22/30 pixels and all lines visible.
- `impl-fsck.log`: read-only forced e2fsck on the private root and home volumes
  after guest shutdown. `impl-checks.log`: whitespace, stale-reference and
  header-equality checks. `impl-inputs.json`: hashes of reviewed inputs and
  tested binaries, separate from F0's historical freeze manifest.

Guest setup: QEMU 8.2.2, q35, 8 GiB, eight CPUs, two private NVMe image copies,
BIOS ISO boot, serial log plus stdio monitor. Text root GUID:
`1ec5f5ab-71b7-45cd-a7a4-05646e878e57`; GUI root GUID:
`2f4fd02e-68b4-4c82-98bc-72467529b3fc`. The default Limine entry selects text;
the next entry selects GUI. In the guest, run
`fonttest > /home/f1-text.log` or `/home/f1-gui.log`; use Ctrl+Alt+F1 from GUI
and Alt+F8 from the terminal to inspect the specimen. Shut down the guest
before inspecting filesystem images. No P5 validation was performed.

## Disposition and next boundary

The corrected standalone F1 candidate is accepted by this implementation
review. No known blocking F1 finding remains. Fable's architecture approval
and F0 freeze remain separate records; their historical selected-input hashes
are not rewritten to pretend these later F1 edits were present at freeze time.
Nothing has been pushed, merged or opened as a PR by this review.

F2 can consume the frozen backend table, implement bounded glyph caching and
text measurement/layout, and measure process-start cost before adding the
production dependency. Fonts remain test fixtures until the later installation
and configuration slice chooses product defaults. The requested plain-language
font overview belongs with the completed feature, after those pieces exist.
