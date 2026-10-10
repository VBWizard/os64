# WebP still-image integration

Implemented 2026-10-10 on `codex/webp-decoder`. Lossy VP8, lossless VP8L and
VP8X/ALPH still images decode through `libimage`. Yonder, gview and desktop
wallpaper use that shared path. Animation returns `UNSUPPORTED`; WebP remains
absent from the HTTP Accept list. ICC profiles and EXIF orientation are not
applied. The WallpaperCave AVIF file remains outside this codec's scope.

## Review handoff

Proposed title: **Add bounded WebP still-image decoding to libimage and its consumers**.

Yonder previously refused WebP pictures delivered by sites. The shared image
dispatcher now decodes lossy, lossless, and alpha WebP stills using the pinned
libwebp 1.6.0 decoder, so Yonder, gview, and wallpaper receive ordinary owned
ARGB images. The wrapper validates the complete container, enforces input,
pixel, and allocation limits, and serializes upstream state with sleeping
contention. Animation remains explicitly unsupported and is identified in
Yonder's diagnostics; HTTP Accept is unchanged to preserve GIF negotiation.

The change includes the library and notices on both disk formats, decoder
and benchmark tests, demo assets with corrected os64get destinations, and
straight-alpha wallpaper compositing. No kernel allocator or TLB behavior
changes are included. The cleanup cost found during stress testing is tracked
in DEBTS.md with the [memory-path evidence](memory-profile.md).

The P5 results cover real-site browsing, both decoder backends, twelve
navigation rounds with outstanding jobs, and cache-off shutdown under image
load. Shutdown releases the tracked heap but can visibly wait for synchronous
decodes; its click-to-exit latency was not measured. Other measurement and
coverage limits are recorded below. This handoff is for code review, not a
claim that every item in the design's acceptance list has been completed.

## Final verification — 2026-10-10

The final diff review covered the wrapper's framing arithmetic, allocation
ownership and failure paths, decoder serialization, dispatch/status mapping,
wallpaper compositing, build dependencies, license installation, test harnesses,
and the evidence's stated scope. It identified no additional production defect.
The root build succeeded with the existing artifacts up to date. A fresh
scalar/SSE2 core audit plus shared-library and disk-image checks passed, as
did a fresh wrapper ASan/UBSan/LeakSanitizer run over 38 small cases and ten
near-limit fixtures. Leak detection remained enabled outside the sandbox.
The unchanged JPEG/image, GIF, Yonder, and guest regressions retain their
earlier results below; they were not represented as freshly rerun tests.

Final receipts: [build](final/build.txt), [audit](final/audit.txt),
[wrapper sanitizers](final/wrapper-sanitizers.txt), and
[stale-reference scan](final/stale-refs.txt). The scan reports the existing
WebP design link and JPEG harness path as potentially retired; both targets
still exist and the listed references remain valid. No new superlative
comments were reported. Untracked wrapper/test sources were read separately
because the scan operates on tracked diffs. `git diff --check` passed.
Staging then included those sources in the scan, with the same live-path
references plus references within the saved scan receipt. Git attributes
preserve raw text receipts, pinned upstream bytes, and patch context spaces;
the staged whitespace check uses those narrow exceptions. Imported upstream
sources and generated vector headers are marked for GitHub's diff display.

## Implementation

The four-function public ABI lives in
[`webp.h`](../../userland/libwebp/include/webp/webp.h). The
[wrapper](../../userland/libwebp/port/decode.c) validates the complete RIFF
extent before calling upstream, checks canvas/raster agreement, enforces the
20 MiB input / 16 Mi pixel / 256 MiB allocation defaults, and transfers a
normal heap pixel buffer on success. Temporary allocations include aligned
bookkeeping, refund their charge when freed, and retain enough ownership
information for defensive cleanup. Allocation refusal distinguishes `LIMIT`
from `NO_MEMORY`; failure clears the result.

Calls serialize around DSP initialization, allocation context and decoder
cleanup. Contending callers use `os64_sleep(1)`. Private `InitGetCoeffs` remains
lazy and protected by the same gate; this implementation does not claim that
eager initialization makes parallel decoding safe. Signals/callbacks must not
re-enter the decoder.

The framing pass follows the [container specification](https://developers.google.com/speed/webp/docs/riff_container)
when ignoring reserved fields, out-of-order metadata, unknown chunks and
inactive `ANIM` data. Validating the whole container still catches malformed
chunks after the raster. Only the validated reconstruction span is borrowed
by upstream, so extended header additions and ignored metadata need no copy.
The generated [alpha adaptation](../../userland/libwebp/port/alpha_dec.patch)
removes upstream 1.6.0's rejection of reserved alpha bits and unknown optional
preprocessing hints. Its exact diff is checked during generation; pristine
import hashes remain unchanged.

Yonder reserves 276 MiB per admitted picture job and retains `webp, animated`
in diagnostics while counting the missing format as `webp`. Desktop wallpaper
uses the existing straight-alpha blend operation: its previous opaque copy
exposed hidden RGB values in transparent images. Placement and clipping are
unchanged; fully opaque pixels still copy directly.

The build includes `libwebp.so`, its dependency from `libimage.so`, public
header paths, prelink assignment and debug symbols. Both image formats include
the library, `webptest`, visual fixtures and `/etc/licenses/libwebp.txt`.
The same notice is embedded in the library. P5's image-mirroring deployment
includes these payloads through the existing library/image lists.

For `os64get` delivery, the demo images have exact-name routes to
`/tests/pages`. Image suffix rules take precedence over the `@pages` lot,
so the PNG and WebP fixtures would otherwise land in `/home/images`.
The production config parser and router were checked against the shipped
configuration: the page and six images resolve to `/tests/pages`, while
ordinary PNG and WebP downloads retain `/home/images`. An existing
`/home/os64get.conf` takes precedence over the system configuration and needs
the same fixture routes, or an explicit download destination.

## Results

On 2026-10-10, Chris confirmed that WebP images loaded on two real sites on
the P5. The site URLs were not recorded. A separate standalone-image report
described a full-width band roughly 100–200 pixels tall in Yonder and IE;
Chris clarified that Chrome did not display it. Fetching the supplied
[image URL](https://4kwallpapers.com/images/wallpapers/kawah-ljen-crater-3840x210-27374.jjpg)
returned HTTP 200, `Content-Type: image/jpeg`, and
`Content-Disposition: attachment; filename=kawah-ljen-crater-3840x210-27374.jjpg`.
The 407,882-byte body is a baseline JPEG whose actual dimensions are
3840×210. Pillow decoded it completely, and visual inspection confirmed
that the source itself is a narrow landscape strip. This report therefore
does not demonstrate WebP decoding or missing image rows in Yonder.

| Check | Result |
| --- | --- |
| Full userland/kernel/images build | Passed with warnings treated as errors. |
| Pristine/core audit | 71 original files match; scalar and SSE2 compile freestanding. |
| Shared library audit | Four public exports, seven OS imports; expected dependencies, relocations and W^X; no TLS, pthreads, CPU probing or encoder entry points. |
| ext2/FAT packaging | Thirteen codec, dispatcher, test, notice and visual-fixture payloads match source bytes, including both benchmark executables. |
| Wrapper host corpus | 38 cases; independent Pillow reference pixels, full framing, reserved fields, animation refusal, truncated prefixes and framed truncated raster bodies. |
| Failure and concurrency checks | Allocation failure at each attempted allocation; exact budget threshold; mutation cleanup; six callers racing first initialization, mixed limits/failures and retained output. |
| Near-limit wrapper corpus | Ten characterization fixtures reproduce the recorded pixel hashes and allocation peaks, including both compressed-alpha paths and 16,384-wide VP8L. |
| Sanitizers | ASan, UBSan and LeakSanitizer passed. Sandbox attachment failed with EPERM/TracerPid 0; successful runs used outside-sandbox execution with leak checks enabled. |
| Adjacent host regressions | JPEG codec corpus; 195 image/draw checks; 132 GIF still fixtures; GIF sequence corpus; 2,651 Yonder checks, zero failures. |
| QEMU ext2 and FAT | `webptest` passed with exit 0: shared dispatch, reference pixels, six callers, limits, truncation, unsupported animation, still sequence, mislabeled filename and heap verification. |
| QEMU applications | Yonder shows five supported images and refuses the animated sixth; gview opens alpha WebP; wallpaper's transparent, partial-alpha and opaque screenshot pixels match source-over reference values exactly. |

The initial FAT test reached its file-load step but could not create a file
under `/tmp`, which that image did not provide. The test uses `/home` for its
temporary file and cleans it up; the corrected FAT run passed. No decoder
change was needed for that failure.

Evidence: [host wrapper](wrapper-host.txt), [shared audit](shared-audit.txt),
[adjacent regressions](regressions.txt),
[ext2 guest](guest-ext2.txt), [FAT guest](guest-fat.txt),
[Yonder's actual page record](yonder-diagnostic.txt),
[Yonder screenshot](yonder.png), [lower page](yonder-lower.png),
[gview](gview.png), [wallpaper](wallpaper.png).

The maximum production-wrapper charge remains **168,586,521 bytes** for
compressed alpha using the 32-bit lossless path, below the 256 MiB budget.
These sampled peaks are not a worst-case bound or a process-RSS bound.

## Reproduction

From the repository root, with the cross-compiler on PATH and host Pillow
available:

```sh
make
python3 tools/audit_webp.py --output /tmp/webp-audit --shared --images
python3 tools/bench_webp_host.py --output /tmp/webp-characterization
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_webp_host.py \
    --output /tmp/webp-wrapper --characterization /tmp/webp-characterization
```

Run sanitizer binaries outside an environment that denies LeakSanitizer's
thread attachment. `--write-guest` explicitly regenerates the committed
synthetic guest vectors and visual assets; normal host tests do not change
them. Their source images are generated by this repository, with no downloaded
artwork. Normal lossy/alpha/lossless references use Pillow's libwebp decoder;
reserved-bit and future-header variants retain their unmodified image's
reference pixels.

In an Os64 build from this worktree:

```sh
/tests/webptest
yonder file:///tests/pages/webp-stills.html
gview /tests/pages/webp-alpha.webp
```

For wallpaper, set `image = /tests/pages/webp-alpha.webp` in the guest's
`/home/desktop.conf` and restart the desktop. The screenshot tests used a
disposable QEMU guest (q35, qemu64 with SSE2, 8 GiB, eight vCPUs); host timing
and this guest's scheduling do not establish P5 performance.

The first [P5 scalar/SSE2 comparison](p5-benchmark.md#first-p5-report--2026-10-10)
passed pixel and heap checks and records gate waiting plus process CPU
accounting. The [P5 navigation run](stress.md#p5-navigation-result) passed
twelve rounds with outstanding picture jobs, repeated large-buffer recovery,
and normal idle shutdown, with caching enabled. A separate cache-off
[P5 close-under-load run](stress.md#p5-close-under-load-result) exited normally
with four picture jobs pending in its final page record and zero live heap
allocations in its final stable observer sample. Closing was visibly delayed;
the observer does not timestamp the click. Remaining measurement work includes
repeat hardware timings, isolated waiting-worker CPU measurement, and a
close-latency bound. The
[automated stress investigation](stress.md) reproduced long QEMU navigation
and shutdown delays with large WebP and PNG images. The synchronous API offers no prompt
mid-decode cancellation or FIFO fairness. Animation remains the next codec
slice; these results do not claim AVIF support.

The [P5 benchmark](p5-benchmark.md) supplies a one-command hardware report
with independent scalar/SSE2 builds, allocation/cleanup and gate timing,
pixel checks, and six-worker contention. The first hardware report and its
measurement limits are recorded there.
