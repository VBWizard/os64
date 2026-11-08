# Image memory: instrumented baseline

2026-11-08. First implementation slice of [IMAGE_MEMORY.md](IMAGE_MEMORY.md),
based on the design branch merged with `51e8d2f0` (page memory census).
Image loading, source selection, reduction and Canvas ownership retain their
baseline behavior. This slice supplies observations and comparison fixtures.
I0–I3 are not implemented by this slice.

## Controlled observations

The host exercises Yonder's catalog, worker, metadata provider and script
event delivery. The supplied PNG uses the production PNG decoder; the test
substitutes a data URL containing its identical bytes for the file transport.
The operating-system services are fakes. These are ownership and rendering
observations, not hardware latency measurements.

At 100% zoom, one 1920×1080 PNG has a fixed 160×90 display box:

| Page | Display raster | Native raster | Native raster used only for metadata |
|---|---:|---:|---:|
| `image-memory-untouched.html` | 57,600 B | 0 B | 0 B |
| `image-memory-metadata.html` | 57,600 B | 8,294,400 B | 8,294,400 B |
| `image-memory-canvas.html` | 57,600 B | 8,294,400 B | 0 B |

The metadata page checks natural dimensions and completion in its load
handler. The Canvas page draws once in `onload` and immediately reads the blue
pixel back. There is no animation loop, retry or second draw. All three pages
pass. A separate 400×200 synthetic decoder fixture produces 20,000 B display
plus 320,000 B native after metadata acquisition, reconciled to the retained
picture account. Pixel acquisition changes attribution without allocating
another raster.

This establishes that metadata can cause a full-resolution duplicate. It
does not assign a fraction of historical live-site memory to that mechanism.

## Record fields and boundaries

Existing `pictures`, `picture sizes` and census `memory` fields keep their
formats. Five bounded aggregate facts are added:

| Field | Meaning |
|---|---|
| `picture memory` | Current display raster, native raster, sequence and SVG charges; high-water mark of the retained picture account |
| `image acquisition` | Catalog entries by mode, native entries sharing a display URL, metadata/pixel provider calls, and retained native raster bytes with metadata calls but no pixel calls |
| `picture work` | Accepted job completions, resamples, decode calls, reduced products, cumulative/maximum decode-and-reduction duration, and calls with unavailable timing |
| `picture fetch` | Successful fetch outcomes: network, fresh cache, validation and stale fallback; network entity-body bytes |
| `picture refusals` | Retained-budget refusals, failed submissions, and decoder allocation/limit results |

The memory subdivisions are part of the existing picture owner; do not add
them to it again. Sequence charges include their encoded content and working
canvases. Metadata/pixel counts include polling and unsuccessful acquisitions;
they are not counts of distinct DOM reads or draws. A shared URL does not prove
compatible credentials, response identity or safely shareable pixels.

The retained peak excludes worker scratch, transient full decodes, pending
products, Canvas destination buffers and SVG raster caches. The census reports
process memory separately. Codec allocation peaks and concurrent allocations
are not instrumented here; the worker reservation is not a measured peak.
Decode timing includes SVG parsing or GIF sequence creation where applicable.

Fetch counts cover successful whole responses delivered by jobs accepted for
the current page, including HTTP error responses. They exclude abandoned
generations, failed partial transfers, intermediate redirect bodies, headers
and transport compression. Entity bytes are not wire bytes. Cache validation
is separate from fresh hits. Submission refusals combine no pool, allocation
failure and pool rejection; existing per-picture reasons distinguish them.

## Validation and hardware comparison

- Full Yonder host suite: 23,554 checks, zero failures, ASan/UBSan/LeakSanitizer
  enabled. This includes the synthetic baseline and allocation/cancellation
  sweeps. The subsequent delivery-page extension passes the focused image
  suite: 3,253 checks per run, zero failures in three runs, the same sanitizers
  enabled. All three runs give the identical ownership readings above.
- Target Yonder and its shared-library dependencies build successfully.
- `git diff --check` and `tools/stale_refs.sh` pass.

Reproduce the focused host run with
`YONDER_IMAGES_ONLY=1 ASAN_OPTIONS=detect_leaks=1 tools/test_yonder_scripts_host.sh`.
`tools/generate_image_memory_fixture.py` regenerates the PNG and three pages
without downloaded assets.

Hardware readings are pending. Deliver Yonder, its matching shared-library
dependency closure and these four fixture files in a frozen cohort. Record
the source commit and SHA-256 manifest with each measurement. Keep Modern Web
and scripting enabled, use 100% zoom and the same window geometry, and run
each fixture three times. The local-file fixtures isolate image ownership;
they do not measure HTTP caching.

For the live-site baseline, keep the same viewport, cache condition and
interaction sequence on Microsoft Design wallpapers, Unsplash, Wallpapercat
and the chosen Reddit feed. Preserve individual diagnostic records. On
Microsoft Design, record stylesheet activation and horizontal extent as well
as memory; I0's eventual geometry change must be attributed separately from
I2/I3. No hardware improvement is claimed by this baseline slice.

## Local-navigation correction after hardware feedback

The first hardware visit exposed a pre-existing navigation gap: relative
links resolved to valid file URLs, but `request_navigate` sent them through
libway's HTTP-only scheme check before the local reader could handle them.
Direct address-bar opening worked, which is how the initial fixtures were
tested. Local-document file GETs now use the existing local navigation path;
network-document file links remain refused.

The regression exercises all three comparison-page filenames through link
activation and checks the network-document refusal. The full host suite passes
23,610 checks with ASan, UBSan and LeakSanitizer; the image ownership readings
and immediate Canvas readback above remain unchanged. The correction is a new
frozen delivery with the original libraries and fixture files reused.

The PNG also needs explicit installer routing: the `*.png` name rule takes
precedence over the `@pages` lot and otherwise installs it in `/home/images`.
The supplied config names `image-memory.png` explicitly. With an existing
custom installer config, install it once with
`os64get HOST image-memory.png /tests/pages/image-memory.png`, replacing `HOST`
with the fixture server. Confirm the file is beside the HTML before measuring;
a placeholder with zero decode calls is a delivery failure, not an image-memory
result.
