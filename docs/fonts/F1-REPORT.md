# F1 completion report — FreeType audit and backend

This is Opus's original R1 report and evidence. Quinn's subsequent B1 correction,
current R3 header and new validation are recorded in [F1-B1-REPORT.md](F1-B1-REPORT.md).
Its guest screenshots/logs precede that correction.
The subsequent full implementation review and fresh guest evidence are in
[F1-IMPLEMENTATION-REVIEW.md](F1-IMPLEMENTATION-REVIEW.md).

```text
F1 status: backend complete; contract reconciled against R1, not frozen
os64 base and actual branch/worktree:
    base   3b82356413ab8f183bd6506febe8d06fea6e7de0 (merged PR #109)
    branch opus/freetype-backend
    tree   /home/yogi/src/os64/.worktrees/freetype-backend
FreeType tag + full commit + archive/import digest:
    release 2.14.3, tag VER-2-14-3
    tag object c740f0fda4274d6ffd2e5b64a25b06ef69803a07
    COMMIT     0a0221a1347e2f1e07c395263540026e9a0aa7c7
    archive    freetype-2.14.3.tar.xz
               36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
Frozen backend contract revision and any approved deviations:
    NONE FROZEN. Built against F0 review candidate R1 as published mid-session:
      FONT_CONTRACTS.md                                sha256 3a333480aa32ece7…
      userland/libos64/include/os64/font_backend.h     sha256 e1b361f0fd29e936…
    (both from .worktrees/font-design; the header is copied verbatim into this
    tree because the build needs it — it is F0's file, not mine.)
    Deviations: none taken unilaterally. Six questions are raised below.
Files/commits delivered; separate shared build patch:
    Uncommitted. Listed below.
    Shared build patch: docs/fonts/f1-shared-build.patch
    Evidence:           docs/fonts/f1-evidence/
Enabled formats/modules/render modes and intentional exclusions:
    see "The configuration", below, and UPSTREAM_REVIEW.md in full
Host tests:        943 checks, 0 failures, ASan+UBSan at -O2 and at -O0
                   (logs: docs/fonts/f1-evidence/host-asan-O{2,0}.log)
Cross build:       leaf, no DT_NEEDED, no undefined symbols, one exported symbol
QEMU:              /tests/fonttest PASS twice, on DIFFERENT roots —
                   text boot on the ext2 root, GUI boot on the FAT lifeboat;
                   specimen window rendered; e2fsck green on both ext2 volumes
Hardware:          NOT RUN. No P5 claim is made.
Measured costs:    below
Unrun checks:      below
Publication:       nothing pushed, no PR opened, nothing committed
```

## What was delivered

| Path | What |
|---|---|
| `userland/libfreetype/upstream/` | FreeType 2.14.3, 334 files, whole module directories |
| `userland/libfreetype/patches/` | 11 backported upstream fixes |
| `userland/libfreetype/manifest.json` | provenance + a digest per imported file |
| `userland/libfreetype/port/` | the adapter, the configuration headers, the private runtime, the nonlocal return |
| `userland/libfreetype/fixtures/` | four licensed faces + their licences + `FIXTURES.md` |
| `userland/libfreetype/{sources,shared}.mk`, `exports.map` | the build |
| `userland/libfreetype/UPSTREAM_REVIEW.md`, `README.md` | the audit and the reproduction instructions |
| `tools/import_freetype.py` | `--import` / `--verify` for the pinned tree |
| `tools/font_kern_report.py` | the INDEPENDENT kerning reader — where the pinned expectations come from |
| `tools/test_freetype_host.{c,py}` | the host harness |
| `userland/tests/fonttest/fonttest.c` | the guest fixture |
| `license/freetype-LICENSE` | shipped to `/etc/licenses/freetype.txt` |
| `userland/libos64/include/os64/font_backend.h` | **F0's file, copied verbatim** |
| `GNUmakefile`, `userland/GNUmakefile` | **shared — see the separate patch** |

## The configuration, in one paragraph

SFNT + TrueType + CFF outlines, the smooth grayscale rasterizer, the
autofitter's Latin module, CFF's three PostScript helpers, GPOS pair kerning
beside the legacy `kern` table, error strings. **Two hinters run, split by
format:** the autofitter for TrueType (which has no other hinter once the
bytecode interpreter is out) and the Adobe CFF engine, natively, for
OpenType/CFF. Off: the TrueType bytecode
interpreter, font variations, embedded bitmap strikes, colour layers, SVG
glyphs, the SFNT BDF table, the monochrome and SDF renderers, every
standalone Type 1/CID/PFR/FON/BDF/PCF driver, file streams, environment
properties, compression wrappers, legacy Mac containers, incremental faces,
and upstream's inline assembly. Eleven upstream translation units plus four
of os64's own.

## Evidence

### Cross build

```
$ x86_64-elf-size userland/bin/libfreetype.so
   text    data     bss     dec     hex
 413230   13376       0  426606   6826e
$ x86_64-elf-readelf -dW userland/bin/libfreetype.so   → SONAME only, no DT_NEEDED
$ x86_64-elf-nm -D -u userland/bin/libfreetype.so      → (nothing)
$ x86_64-elf-nm -D --defined-only …                    → os64_freetype_backend_v1
$ x86_64-elf-readelf -rW …                             → 760 relocations, all R_X86_64_RELATIVE
$ x86_64-elf-readelf -lW …                             → LOAD R | LOAD R+E | LOAD RW
```

Prelinked at `0x00007f0044000000`, inside the 4GB prelink region, 418KB
mapped against a 64MB slot. `bss 0` — the library has no zero-initialised
global state at all, and every `.data` symbol is one of upstream's `const`
service tables carrying relocations. **Strict build throughout**
(`-Wall -Wextra -Werror`, `-O2`, freestanding, PIC, no red zone,
`-masm=intel`); `NOWERROR=1` was not used. Two `-Wno-` flags apply to
UPSTREAM FILES ONLY (`-Wno-unused-variable -Wno-unused-but-set-variable`),
because trimming the module set leaves locals a fuller build consults.

### Host tests

```
$ tools/test_freetype_host.py            # ASan + UBSan, -O2
943 checks, 0 failures
$ tools/test_freetype_host.py -O0        # same, other level
943 checks, 0 failures
```

Host compiler `cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`; cross compiler
`x86_64-elf-gcc (GCC) 14.2.0`. Flags:
`-O2 -g -std=c11 -fcf-protection=none -fno-builtin
-fno-tree-loop-distribute-patterns -DOS64_FREETYPE_HOSTED
-fsanitize=address,undefined -fno-sanitize-recover=all`. No sanitizer
exclusions, leak detection on.

What it covers: both formats at 16/24/32px; metadata; equal-versus-unequal
advances; empty glyph; negative left bearing; ink-rectangle consistency;
partial coverage (proving grayscale, not a widened 1-bit mask); invalid
scalars and surrogates distinguished from missing glyphs; the 64-face
ceiling; interleaved independent engines; repeated open/close with the
accounting returning to a baseline each round; truncation at ten lengths per
font; a byte-by-byte `cmap` corruption sweep; **allocation denial at every
reachable allocation point** (not a sample — the harness censuses a full
cycle, then fails the Nth for every N, requiring no leak and a destroyable
engine each time); and the private C runtime against the host's libc.

**The nonlocal return is proved, not inferred.** A host-only counter in
`port/runtime.c` lets `test_charmap_validator` require that the validator's
jump actually EXECUTED during the corruption sweep, at `-O2`, under ASan with
`__asan_handle_no_return`. A separate test drives the jump directly through
five frames and checks the value, the zero-to-one promotion, and that
`volatile` locals survived.

### QEMU — and WHICH ROOT each boot mounted

**The two boots used two different root filesystems, which is better coverage
than one and was described wrongly in the first version of this report.** The
second Limine entry is called `/QEMU GUI Boot` and its comment reads "QEMU
Boot plus the optional GUI subsystem", which is what misled me: it also
carries a different `ROOT=`.

| Run | Limine entry | `ROOT=` | Partition | Filesystem |
|---|---|---|---|---|
| text | `/QEMU Boot (ext2 root)` | `1ec5f5ab-71b7-45cd-a7a4-05646e878e57` = `EXT2_PARTUUID` | partition 2 | **ext2 root** |
| GUI | `/QEMU GUI Boot` | `2f4fd02e-68b4-4c82-98bc-72467529b3fc` = `DISK_PARTUUID` | partition 1 | **FAT lifeboat** |

Confirmed from the GUIDs in the root `GNUmakefile` and from the serial logs:
the GUI boot's wire carries `BOOT: Root filesystem found, mounting`, the FAT
spelling. (The ext2 boot's equivalent line is a `printd` and goes to logd's
file under `LOGD=`, per SUCCESSION.md.)

**So the library, the fixture and the font files were exercised on BOTH
volumes, through both filesystem drivers** — ext2 for the text run, FAT for
the GUI run. That was not deliberate, and it is now the reason the fixture
files are installed to both volumes rather than a nicety.

Both runs: NVMe root plus the `/home` data disk, headless, monitor on 55556,
`-machine q35 -m 8g -smp 8`. Image contents verified BEFORE the runs, on both
volumes: `/lib/libfreetype.so`, `/tests/fonttest`, `/tests/fonts/` (4 fonts +
3 licences), `/etc/licenses/freetype.txt`.

```
fonttest: PASS (peak 105928 bytes, 2146 allocations)   # ext2 root, text boot
fonttest: PASS (peak 109764 bytes, 6446 allocations)   # FAT lifeboat, GUI boot
```

The kerning assertions passed against values decoded independently from the
fixtures' own tables (−131, −348, −29, −246 in 26.6 against expected −131,
−348, −28, −245 ±2). `make fsck-ext2`: e2fsck green on both ext2 volumes.

**Not covered:** a GUI boot on the ext2 root, and a text boot on the FAT
lifeboat. Each run proves one filesystem and one display path, and the two
happen to cross rather than overlap; nothing here separates a
filesystem-dependent failure from a display-dependent one.

### Evidence artifacts

In the worktree, not the scratchpad, so the paths outlive the session.
Absolute root: `/home/yogi/src/os64/.worktrees/freetype-backend/`

| Repo-relative path | What |
|---|---|
| `docs/fonts/f1-evidence/host-asan-O2.log` | host harness, ASan+UBSan, `-O2`: 943 checks, 0 failures |
| `docs/fonts/f1-evidence/host-asan-O0.log` | the same at `-O0`: 943 checks, 0 failures |
| `docs/fonts/f1-evidence/kern-fixtures.txt` | `tools/font_kern_report.py --fixtures` output — the independent kerning read |
| `docs/fonts/f1-evidence/qemu-text-ext2.log` | serial log, text boot, **ext2 root** |
| `docs/fonts/f1-evidence/qemu-gui-fat.log` | serial log, GUI boot, **FAT lifeboat** |
| `docs/fonts/f1-evidence/guest-text-ext2-verdict.png` | terminal specimen + PASS, ext2 root |
| `docs/fonts/f1-evidence/guest-gui-fat-specimen.png` | the window: 4 faces × 4 sizes, antialiased, FAT lifeboat |
| `docs/fonts/f1-evidence/guest-gui-fat-verdict.png` | the PASS line on that boot's VT1 |
| `docs/fonts/f1-shared-build.patch` | the shared-makefile change, for Quinn to apply separately |
| `docs/fonts/F1-REPORT.md` | this file |
| `docs/fonts/F1-REPLY-R1.md` | the point-by-point reply to Quinn's review of the first version |

The screendumps were taken through the QEMU monitor (`screendump`) and
converted from PPM to PNG. Earlier copies under
`/tmp/claude-1000/-home-yogi-src-os64/992d58a3-fc84-40f5-885e-dfb2c70a9e9c/scratchpad/`
are the same images and will vanish with the session.

### Measured costs

Per-face, one face open at one size with every printable ASCII glyph rendered
and released. Includes the adapter's bookkeeping and the 16-byte header on
each block; excludes the font file, which the caller owns.

| Face | peak bytes | allocations | held while open |
|---|---|---|---|
| DejaVu Sans (TTF) | 51,260 | 465 | 49,307–49,410 |
| DejaVu Sans Mono (TTF) | 45,704 | 457 | 44,504–44,571 |
| Source Sans 3 (CFF) | 109,772 | 968 | 106,060–106,106 |
| Source Code Pro (CFF) | 75,082 | 910 | 71,378–71,421 |

**An open face's cost is essentially independent of pixel size** (tens of
bytes across 16→32px), and CFF faces cost roughly twice a TrueType one. For
F2: a cache holding rendered masks adds to these, and the numbers above are
what a face costs before a single mask is retained.

Library: 413,230 text / 13,376 data / 0 bss, 418KB mapped. The `.so` is
1,815,888 bytes on disk, almost all of it `-g` debug info — the loaded
footprint is the 418KB. Font fixtures add 1.6MB to each of the two volumes.

## Reconciling R1 with the engine — six questions for F0

None of these was decided unilaterally in a way that cannot be reversed; each
is one small change if the answer differs.

1. **Does `render` accept glyph index 0?** R1 says lookup treats 0 as MISSING
   and that `pair_adjust` refuses it as BAD_ARGUMENT, and is silent about
   `render`. This implementation refuses it, for consistency with the only
   explicit statement. **But F2 will want `.notdef`** — FONTS.md asks for "a
   visible missing-glyph marker", and index 0 is where every font keeps it.
   If F2 should draw it from this backend, that is one line here.

2. **GPOS pair kerning is narrower than it sounds.** FreeType 2.14.3 accepts
   a `PairPos` subtable only when `valueFormat1 == 0x0004, valueFormat2 == 0`
   (an X advance on the first glyph, nothing on the second) and says so in a
   comment. Extension lookups and both PairPos formats ARE handled. A font
   whose kern feature adjusts the second glyph contributes nothing and reports
   zero, indistinguishable from "no pair". R1 should say pair kerning is
   best-effort coverage rather than a guarantee.

3. **Kerning is reported UNROUNDED** (`FT_KERNING_UNFITTED`), in both hint
   modes. Grid-fitted kerning flattens Source Sans 3's "AV" at 32px — about
   0.45px — to zero, and a line whose every small kern rounded away has
   silently lost its spacing. 26.6 exists so the fraction survives; where to
   round is a surface decision and this backend has no surface. R1 says
   "normalize it to the chosen size/hint mode", which this reads as the
   representation rather than the rounding. **Confirm.**

4. **`NORMAL` hinting is TWO hinters, split by format** — autofit for
   TrueType, the Adobe CFF engine natively for OpenType/CFF. Compiling the
   bytecode interpreter out changes the TrueType half only: the CFF driver
   advertises `FT_MODULE_DRIVER_HAS_HINTER` unconditionally, and
   `FT_Load_Glyph` reaches for the autofitter only when a driver has none.
   R1 permits this ("allowing its normal native/autofit choice"), and
   `NONE` still turns both off. **The consequence F5 should hear:** the same
   typeface as a `.ttf` and as an `.otf` will not hint identically here, and
   that is true whichever way the interpreter switch is set.

   *(This corrects the first version of this report, which claimed the
   autofitter hinted both formats. It does not. Caught by Quinn; the audit,
   the configuration comments and this paragraph are all fixed.)*

5. **Variable fonts are detected by TABLE PROBE, not by a FreeType flag.**
   With `TT_CONFIG_OPTION_GX_VAR_SUPPORT` off, FreeType does not set
   `FT_FACE_FLAG_MULTIPLE_MASTERS`, so a variable font would open silently at
   its default instance. `face_open` probes for `fvar` and `CFF2` directly
   and returns UNSUPPORTED, which is what R1 asks for — recorded because it
   is not obvious and would break quietly if the probe were removed as
   redundant.

6. **Names cannot be UTF-8 through FreeType's own helper.** Upstream flattens
   name-table strings to ASCII, replacing everything else with `?`. To deliver
   R1's "copied UTF-8 … invalid name input becomes U+FFFD … truncated at
   scalar boundaries", the port reads the name table directly and converts
   UTF-16BE itself, preferring typographic family/subfamily (16/17) over the
   legacy ids (1/2) and English where offered, falling back to FreeType's
   ASCII copy for faces with no Unicode-encoded name entry. If F5 wants a
   different name preference, that is where it lives.

Two smaller notes: **`OS64_FONT_FACE_MAX` is 64 and each face costs 45–110KB**,
so a full table is 3–7MB against a 64MB default cap — the two limits are
consistent, worth stating. And **`line_height` is clamped** to at least
`ascent + descent`, because some faces declare less and R1 requires the
floor; the clamp is silent.

## Known limitations

- **Allocation is bounded; CPU time is not.** Nothing here limits how long a
  pathological outline can spend in the rasterizer. Upstream's only work
  limit governs the bytecode interpreter, which is off. Malformed-input test
  runs use timeouts for this reason.
- **The upstream-fix review was a read, not a proof.** 107 post-tag commits
  classified against the enabled module list; a fix whose subject does not
  say what it touches could have been miscategorised.
- **No fuzzing.** The corpus is designed — truncation, corruption sweep,
  exhaustive allocation denial — and a corpus is not a search.
- **Selective backporting means this tree is no upstream revision.** The 11
  patches carry their SHAs so a future bump can drop them.
- **No hardware run**, and no claim about the P5.
- `GNU_RELRO` is not emitted, so upstream's relocated `const` service tables
  sit in a writable segment. Nothing writes them; noted because "const in C"
  and "read-only at run time" are not the same claim.

## Remaining integration work

- F0 freezes the contract; the six questions above want answers first.
- `docs/fonts/f1-shared-build.patch` is the only change to shared files
  (`GNUmakefile`, `userland/GNUmakefile`): the library slot, the build
  include, the `all` target, the fixture's extra library, the debugger
  symbol line, `USERLAND_LIBS`, the `/tests/fonts` install on both volumes,
  and the licence install. It is applied and tested in this worktree; apply
  it to the integration branch rather than merging this tree's copies of
  those two files.
- `userland/libos64/include/os64/font_backend.h` in this tree is a verbatim
  copy of F0's. Integration should take F0's, not this one.
- Nothing is committed, pushed, or proposed for merge. `tools/stale_refs.sh`
  clean; `git diff --check` clean; no trailing whitespace in the new files.
