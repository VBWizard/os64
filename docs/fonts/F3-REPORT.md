# F3 — terminal font integration report

Implementation commit **aa8b7a276164ad90f0fe0826c3d91ef8a4c60ac0** on
`codex/terminal-fonts`, worktree
`/home/yogi/src/os64/.worktrees/terminal-fonts`. Independent review and publication
are pending. No merge is requested or implied by this report.

## Prerequisites and ownership

Exact parent: **788de9900282e941a7a93aa110ffa69154daa066**, the F2.5 shared
provider/adoption foundation. This includes accepted F2 `4b0a839` (implementation
`32d8243`), reviewed F1 `f065c7c`, and frozen F0
`23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Changes are gterm, its local `font_grid` consumer, terminal host/guest fixtures,
and documentation/evidence. `abi/include/os64/charset.h` has comment corrections
because its claims that every terminal is 8×16 and gterm directly draws the
embedded bitmap indices became false. No declarations, constants, kernel code,
PTY ABI, terminal parser, F0/F2/F2.5 implementation or shared build manifest changed.
The F4 widgets/Scribe and F5 settings files are untouched.

## Delivered behavior

[The integration design](../../GTERM_FONTS.md) records resource ownership, geometry,
transaction order, snapshot publication, preparation cost and the F5 seam.

- Provider-validated monospace metrics drive initial dimensions, painting,
  cursor, selection and existing PTY resizing; capacity and PTY bounds remain.
- `gterm_grid_consumer` exposes prepare/barrier/commit/abort through the frozen
  shared coordinator. Candidate runs/resources precede the PTY resize barrier;
  commit and painting allocate nothing. Refusal preserves active state.
- The finite Latin-1/CP437 alphabet is represented by retained F2 cell runs.
  F2 still owns glyph caching/rasterization/fallback. ANSI colors, attributes,
  box/block drawing and per-cell clipping are preserved.
- Header probes no longer overwrite the displayed header. Full snapshots are
  accepted only at the installed dimensions and full cell count. Failed reads
  preserve displayed state and retry. Successful adoption clears selection/drag.
- Copy retains high bytes instead of replacing signed high-byte chars with spaces.
  This fixes the old mismatch between the painted byte and the clipboard byte.
- The old resize comment incorrectly called the geometry fence the syscall's
  only possible refusal. The code now documents allocation refusals too.

Production `gterm [program args...]` starts with the shared builtin defaults.
Font configuration/live settings are F5 work. This is a validated integration
seam and testable font switching, not a shipped font picker or Unicode PTY.

## Host and cross-build evidence

Commands from the worktree:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_gterm_fonts_host.py -O 2 --output /tmp/f3-host-o2
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_gterm_fonts_host.py -O 0 --output /tmp/f3-host-o0
make -C userland -j4
git diff --check
tools/stale_refs.sh
```

Both optimization levels passed **2,406,573 checks**, including **4,482 injected
allocation-denial cases** and zero live bytes after teardown. These are raw
assertion counts, including per-pixel clipping assertions, not distinct scenarios.
ASan and UBSan were enabled; leak sanitizer was disabled and the sized allocation
ledger checked complete cleanup. Tests link the actual pinned FreeType engine,
provider/coordinator, F2 and production terminal code.

Coverage: geometry fences/capacity/overflow, proportional rejection, preparation
failure at each callback-allocation position in the exercised replacements,
barrier refusal, unchanged-grid adoption without a resize call, successful and
refused surface resize, matching/short/mismatched snapshots, byte-preserving copy,
selection hit coordinates, cursor rectangles, both charsets, line joins, full
blocks, and clipping of every high/printable byte at both sizes. Painting is
checked for zero allocation callbacks. Test logs are [O2](f3-evidence/host-O2.txt)
and [O0](f3-evidence/host-O0.txt).

The strict cross-build passed with warnings treated as errors. The guest fixture
and gterm link to libos64; neither directly links the rasterizer. The accepted
libos64→libfreetype dependency remains. This is userland testing against the
accepted existing kernel/ISO, not a new kernel build or hardware validation.

## Guest evidence and reproduction

The [guest runner](f3-evidence/run_guest.py) copies the accepted F2/F2.5 private
images into a new output directory, installs and byte-verifies final payloads,
boots QEMU, drives production event/render paths, captures screenshots and shuts
the guest down. Base images are not modified or booted. It installs into both
FAT (offset 1 MiB) and ext2 root (offset 65 MiB, length 256 MiB); the tested GUI
entry boots FAT. The separate home/data ext2 partition begins at 1 MiB.

```sh
python3 docs/fonts/f3-evidence/run_guest.py . /tmp/os64-f25-guest/checked-final /tmp/os64-f3-guest/final
```

QEMU: q35, 8 GiB, 8 vCPUs, qemu64 with rdrand/rdseed, two private NVMe disks,
1024×768 GUI, stdio monitor, serial file. `os64_kernel.iso` is the accepted F2
ISO copied into the worktree. The runner expects the existing `/tests/fonts`
directory and the GUI boot entry second in that ISO's menu.

The generated `NoBoxes.ttf` derives from the pinned DejaVuSansMono fixture.
`tools/fonts/make_terminal_fixture.py` removes U+2500–259F cmap mappings, rebuilds
the cmap/directory/checksums, and preserves outlines. It requires only Python's
standard library. Source licensing remains the DejaVu fixture license. This
forces missing box/block glyphs in a real font without changing the rasterizer.
It is a test asset, not an installed product choice.

The guest fixture keys are: `1` = 12px, `2` = 28px, `3` = propose the other size
but make the barrier issue an invalid one-column resize to the real PTY,
`4` = reject proportional DejaVu Sans, `5` = builtin, `6` = assert clipboard is
exactly `COPY ME`. Other input, mouse selection and window management pass through
the production loop. The child exits on `q`; SIGWINCH redraws its specimen after
reading its actual terminal dimensions.

Observed child and master dimensions agree:

| Operation | Cell | Grid reported by child |
|---|---|---|
| Builtin startup | 8×16 | 100×38 |
| 12px outline | 7×15 | 114×40 |
| 28px outline | 17×33 | 47×18 |
| Maximize at 28px | 17×33 | 60×22 |
| Restore at 28px | 17×33 | 47×18 |

The runner exercises selection/copy at both sizes, real refusal preserving
selection and geometry, proportional rejection, maximize/restore, font switching
while a mouse drag is active, and continued child input afterward. Specimens
include Latin-1 accents, ANSI foreground/background/bold/reverse, CP437 single
and double corners/junctions, shades and fractional blocks. Production gterm is
then launched separately, runs `echo alive`, and closes through normal shell exit.
Screenshots, serial output, payload identities and guest assertions are retained
under [f3-evidence](f3-evidence/). The final run shut down cleanly, and offline
read-only checks of the copied root/home ext2 filesystems passed; see
[image checks](f3-evidence/image-checks.txt). The guest assertion transcript is
[guest-terminal.txt](f3-evidence/guest-terminal.txt); full guest serial output
is [serial.txt](f3-evidence/serial.txt) (trailing whitespace removed), monitor input is
[actions.txt](f3-evidence/actions.txt), and the final strict build transcript is
[strict-build.txt](f3-evidence/strict-build.txt).

## Remaining boundaries

F5 supplies production choices, installation/discovery, settings and publication.
F4 supplies proportional widgets/editor integration on its separate branch.
There is no UTF-8 terminal parser change, terminal reflow, new syscall, kernel
change, direct rasterizer dependency in gterm, or real-hardware evidence here.
Preparation now constructs a bounded run collection before adoption; no startup
or throughput benchmark is claimed. Tests prove the exercised failure paths and
settings seam, not memory-pressure behavior across every possible font file.
