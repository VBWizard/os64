# Source-over validation — 2026-09-25

Branch `codex/source-over`, based on merged GIF animation `3fe7e29a`.
Author host/QEMU checks and Chris's P5 observations are distinguished below.
Independent review of this feature has not taken place.

## Host and build

`ASAN_OPTIONS=detect_leaks=0 python3 tools/test_jpeg_host.py` passed the JPEG
reference cases and **180 image/drawing checks, zero failures**. The blend
test exhausts **16,777,216** alpha/source/background channel combinations
against a floating-point oracle, checks exact alpha 0/1/128/254/255 results,
and covers mixed-alpha runs, four-edge and extreme-origin clipping, pitched
subimages, padding/guards, invalid inputs and opaque equality with blit.
The original blit tests remain, including a new assertion that it copies
alpha-zero and partial-alpha words verbatim. [Full output](host.txt).

ASan/UBSan were enabled; LeakSanitizer is disabled for the host environment.
`make -j8` passed the strict full build. `readelf -Ws` confirms both public
drawing symbols in `libos64.so`. [Artifact hashes](build-sha256.txt).
The body of `os64_draw_blit` was compared to the base and is unchanged.
`git diff --check` passed. The retirement audit's BMP format names and
`fill_rect` shorthand still refer to supported documentation and the live
`os64_draw_fill_rect` helper. Its all-prefix requirement is in the explicitly
archived GIF planning handout. An old libdraw claim that the kernel masked blit was
part of the userland API was incorrect and has been corrected.

## QEMU

Private root/home disk copies, q35 with eight CPUs and e1000 user networking.
A temporary ISO adds `GUI BACKSTOP=10` to the ext2-root entry. No tracked
boot configuration changes are included.

The guest fetched [alpha-checker.png](alpha-checker.png) over HTTP and opened
it in gview. Its top 128 rows alternate 32-pixel squares of transparent
magenta `(240,20,180,0)` and opaque green `(40,210,100,255)`. The lower 64 rows
use orange `(240,80,20)` with alpha equal to x, covering 0..255. The fixture
is a 256x192 RGBA PNG generated with Pillow. The
[reference](alpha-expected.png) is Pillow's `Image.alpha_composite` over an
opaque `(48,48,48)` background.

The [normal view](checker.png), [enlarged view](large.png) and
[cropped view](small.png) match the reference **pixel-for-pixel** across
their complete canvases. Canvas origin is `(65,84)` in the captures. The
384x288 enlarged canvas centers the source at `(64,48)` over the mat; the
128x96 small canvas shows source rectangle `(64,48,128,96)`.
[Comparison results](pixel-checks.txt). Ctrl+Alt+right-drag was injected with
explicit QMP modifier press/release events.

A transparent GIF continues to animate: [frame one](gif-1.png),
[frame two](gif-2.png), with unpainted areas showing the mat. `/tests/giftest`
returned **0**, covering existing decode/sequence behavior through the new
shared-library build. [Guest output](guest-giftest.txt).
The [JPEG view](jpeg.png) also matches Pillow's decoded RGB pixels exactly.

After sync/shutdown and confirmation that QEMU stopped, both extracted
partitions passed read-only `e2fsck -fn`: [root](fsck-root.txt),
[home](fsck-home.txt).

## P5

Chris tested `alpha-checker.png` in gview on the P5 and confirmed the expected
green/dark-gray checkerboard across the upper two-thirds and the smooth
gray-to-orange/red fade across the lower third. This is user-reported visual
validation on real hardware; the exact pixel comparisons above ran against
QEMU captures.

## Scope

This proves straight-alpha image drawing onto an opaque GUI canvas. It does
not establish window translucency, linear-light blending, or BMP alpha-mask
decoding. See [SOURCE_OVER.md](../../SOURCE_OVER.md) for the public contract.
