# WebP decoding for os64

Status: decoder import and host characterization complete, 2026-10-10.
The bounded wrapper and consumer integration remain to be implemented.

Port the decoder portion of WebM's libwebp behind an Os64-owned interface
in `libwebp.so`. Add format dispatch to `libimage.so`, so Yonder, gview,
and desktop wallpaper gain WebP through their existing image API. The
first slice covers lossy and lossless still images with transparency.
Animation is the next slice. Decode by signature in this slice, and advertise
`image/webp` only when animation support ships. No kernel or audio changes
are needed.

## Library choice and license

Use upstream libwebp **1.6.0**, pinned to commit
`4fa21912338357f89e4fd51cf2368325b59e9bd9`. The import records the tag,
release archive URL, archive SHA-256, and hashes of the retained files in
`userland/libwebp/manifest.json`. Official release references and release notes
were checked for this import; check updates again when changing the pin.

Os64's [root license](../../../LICENSE) is MIT. libwebp's
[COPYING](https://raw.githubusercontent.com/webmproject/libwebp/v1.6.0/COPYING)
uses the BSD three-clause terms: source distributions retain the notice,
conditions and disclaimer; binary distributions reproduce them in accompanying
materials; Google and contributor names cannot be used to endorse the derived
product without permission. Those terms permit inclusion in MIT-licensed
Os64. The imported code retains its upstream license; our wrapper can remain
MIT. This does not require changing Os64's root license.

Preserve upstream `COPYING`, `PATENTS`, `AUTHORS`, and notices in retained
source files. The separate
[patent grant](https://raw.githubusercontent.com/webmproject/libwebp/v1.6.0/PATENTS)
covers specified Google patent claims necessarily infringed by the WebM
implementations, with a patent-litigation termination condition. It is not a
blanket grant for arbitrary modifications or third-party patents.

Audit notices for the actual retained source set before accepting the import.
Generate `license/libwebp-LICENSE` from its required notices and patent grant;
embed the same material behind `os64_webp_license()` and install it at
`/etc/licenses/libwebp.txt` on both ext2 and FAT images. Verify the installed
notice bytes. Preserve attribution and redistribution rights for test fixtures
separately from the codec license.

Upstream provides a
[decoder-only library target](https://raw.githubusercontent.com/webmproject/libwebp/v1.6.0/src/Makefile.am),
composed of decoder, decoder DSP, and decoder utility sources. Use that
dependency closure rather than importing the encoder or a video framework.
Our existing [JPEG port](../completed/JPEG.md) supplies the precedent for
pristine sources, private compatibility code, a narrow public ABI, and audits.

## Supported image profile

Accept complete RIFF WebP files containing a single still image:

- `VP8 ` lossy images, including extended files with an `ALPH` chunk.
- `VP8L` lossless images, including their alpha channel.
- `VP8X` extended still containers carrying either encoding.

Return a tightly packed, top-first `uint32_t` pixel plane in straight-alpha
`0xAARRGGBB` form. Request upstream `MODE_BGRA`, whose byte ordering matches
that integer layout on x86-64. Do not choose `MODE_ARGB` merely because our
integer format is called ARGB, and do not choose a premultiplied output mode.
Opaque pixels have alpha 255. Tests compare decoded channels even when alpha
is zero against the same upstream decoding mode.

The initial profile displays pixels in encoded orientation. ICC profiles,
EXIF orientation, and XMP contents are not interpreted; their containing
chunks must still fit within the file. Lossy conversion uses fancy chroma
upsampling (`no_fancy_upsampling = 0`), with neither scaling nor cropping,
without ICC color management. The host oracle uses the same settings. These
are explicit profile limits, including the difference from JPEG's applied
EXIF orientation.

Animated files return `UNSUPPORTED`, including a one-frame file encoded in
the animation container. Do not quietly return the first frame. Encoding,
resizing during decode, incremental presentation, and animation composition
are outside this slice. Whole-buffer decoding remains on Yonder's worker.
Truncated input is refused rather than displaying a partially recovered image,
even when another browser can display some of its rows.

## File validation and failure behavior

`libimage` recognizes `RIFF` at byte zero and `WEBP` at byte eight, with at
least twelve bytes available. Filename and HTTP content type do not choose
the codec. A RIFF AVI or WAV must not enter the WebP decoder.

The wrapper first validates the declared RIFF extent and walks its chunks
with checked size and padding arithmetic. It requires complete chunk headers,
payloads, and odd-length padding within that extent, including chunks after
the raster. Bytes following a complete declared RIFF extent are ignored;
they remain counted against the encoded-input cap. Pass only the declared
extent to upstream. Bare VP8/VP8L payloads, which some upstream APIs accept,
are outside our file API.

Validate the still-image structure against the
[WebP container specification](https://developers.google.com/speed/webp/docs/riff_container):
the required headers and reconstruction chunks, their sizes, ordering,
multiplicity, and agreement of canvas and raster dimensions. Skip well-framed
unknown chunks as the specification permits. Do not turn fields the
specification says readers should ignore into accidental compatibility gates.
Upstream owns compressed-bitstream validation; our framing pass is not a
second pixel decoder.

Recognized animation is refused after checking outer chunk framing; its
compressed frames are not validated by the still decoder. Structurally
inconsistent animation flags/chunks are malformed. Do not claim that an
`UNSUPPORTED` result proves the file would play correctly elsewhere.

Use `WebPGetFeatures` and the advanced decoder interface to retain explicit
failure codes and supply our output buffer. Header inspection is not proof
that a file decodes. Complete success requires the final decoder operation
to succeed, with output dimensions agreeing with the validated dimensions.
Partial output is freed and never published.

## Consumer interface

Add `userland/libwebp/include/webp/webp.h`, independent of upstream headers.
The proposed API follows `jpeg/jpeg.h`:

```c
typedef struct {
    uint32_t width, height;
    uint32_t *pixels;
} os64_webp_image_t;

os64_webp_status_t os64_webp_decode(const uint8_t *data, size_t length,
    uint64_t pixel_cap, size_t memory_cap, os64_webp_image_t *out);
void os64_webp_free(os64_webp_image_t *image);
const char *os64_webp_status_name(os64_webp_status_t status);
const char *os64_webp_license(void);
```

Statuses distinguish `OK`, `NOT_WEBP`, `MALFORMED`, `UNSUPPORTED`, `LIMIT`,
`NO_MEMORY`, and `BAD_ARGUMENT`. Zero caps select defaults. With a non-null
output argument, failure clears the result and transfers no ownership.
Callers supply an empty result; decoding does not free a previous image.
The free operation accepts NULL and zeros a freed result, making repeated
free of that result safe.

`libimage` maps those statuses to its existing vocabulary. In particular,
budget refusal is `OS64_IMAGE_LIMIT`, allocator failure is
`OS64_IMAGE_NO_MEMORY`, and truncated whole-buffer input is malformed rather
than a request for more input. Successful pixels transfer directly into
`os64_image_t` and can be freed with `os64_image_free`.

## Memory and ownership

The defaults below follow the [import characterization](../../webp-evidence/README.md).
JPEG's allocation cap is not a WebP budget.

| Resource | Bound |
| --- | --- |
| Encoded input | 20 MiB |
| Either dimension | VP8: 16,383; VP8L: 16,384 |
| Decoded pixels | 16 Mi pixels (16,777,216) |
| Allocation budget per decode | 256 MiB, including output and temporary allocations |

Ten host fixtures exercised both compressed-alpha paths and the four VP8L
transform types. The largest observed charge was 168,586,521 bytes (160.8 MiB),
leaving about 95.2 MiB below the selected budget. This is sampled headroom,
not a worst-case bound. The budget remains an enforced refusal threshold.

VP8 stores a 14-bit dimension directly; VP8L stores the dimension minus one.
The lossless limit of 16,384 is therefore intentional. Keep the encoding's
dimension limit distinct from the pixel cap: a maximum-width narrow image
can fit the latter when a maximum-width square cannot. The upstream readers
are [VP8GetInfo](https://raw.githubusercontent.com/webmproject/libwebp/v1.6.0/src/dec/vp8_dec.c)
and [ReadImageInfo](https://raw.githubusercontent.com/webmproject/libwebp/v1.6.0/src/dec/vp8l_dec.c).

In the non-palette VP8L path, `AllocateInternalBuffers32b` allocates an
internal 32-bit canvas plus row caches, in addition to our output. At 16 Mi
pixels those two canvases alone consume 128 MiB. Transforms, entropy tables,
and bookkeeping need more. Compressed alpha can also take the full 32-bit
VP8L path; do not budget on the optimized 8-bit alpha case alone.

The import characterization must record output bytes, peak temporary bytes,
peak total charge, and elapsed time for lossy opaque, lossy with raw alpha,
lossy with both compressed-alpha paths, and lossless with palette, predictor,
and color transforms. Include square and narrow maximum-axis fixtures,
high-entropy and transform-heavy cases, and images near the candidate pixel
cap. Record which decoder paths each fixture actually exercises.

Use those measurements together with a source-level accounting of variable
allocations to choose a pixel cap, a decoder budget, and documented headroom.
A sampled maximum is not a proof of a worst-case bound. Legal files can still
return `LIMIT` when their working set exceeds the explicit resource profile;
do not choose defaults that necessarily exclude an ordinary supported path
at the advertised pixel cap. Publish the resulting constants and evidence
before integration. Yonder's reservation then becomes the encoded-input cap
plus the maximum peak budget of the image paths it can run, including WebP;
recheck how many jobs its pool can admit at that cost.

Check dimensions, pixel products, strides, and output size before allocating
or starting raster expansion. The caller's input is borrowed and bounded
separately; do not copy it. The allocation budget includes port bookkeeping
and temporary allocations, but excludes allocator metadata, page rounding,
library static data, and thread stack space. Do not present it as a bound on
the process's total physical memory or execution time.

Upstream's
[allocation helpers](https://raw.githubusercontent.com/webmproject/libwebp/v1.6.0/src/utils/utils.c)
have overflow checks but no public per-decoder allocator context. Its optional
debug allocation accounting uses globals and environment settings; it is not
our resource contract.

The baseline interception points are `WebPSafeMalloc`, `WebPSafeCalloc`, and
`WebPSafeFree` in `src/utils/utils.c`; public `WebPMalloc` and `WebPFree` wrap
them. A private `compat/stdlib.h` maps the underlying malloc/calloc/free
family to our functions, keeping the upstream file pristine. Verify that
closure rather than assuming the wrappers catch unrelated direct allocations.
These functions route allocation through a budget for the active decode.
Temporary blocks carry aligned size records so free refunds their charge;
calloc clears memory and multiplication
is checked. Audit the compiled source closure for allocation paths that bypass
these functions. A refusal records whether the budget or the heap failed,
then returns NULL and lets upstream unwind normally.

Allocate the final output directly with `os64_malloc` and charge it to the
same budget. Give it to upstream as external output memory. It must remain
a normal heap pointer, without an interior allocation header: `libimage`
frees it directly. After decoder cleanup, temporary charges must be zero;
success detaches only the output. On failure, free the output too. Keep
allocation callbacks active until upstream cleanup has finished.

## Concurrency and runtime adaptation

Build with upstream worker threads disabled. This does not by itself make
independent calls safe: the
[DSP initialization code](https://raw.githubusercontent.com/webmproject/libwebp/v1.6.0/src/dsp/cpu.h)
contains shared function pointers and initialization guards, with a different
synchronization path when pthread support is disabled.

Separate table initialization from the allocation-context lifetime. Eagerly
initialize the selected decode/DSP paths on first use before publishing a
ready flag, with acquire/release ordering. Audit the complete initializer
closure, including alpha, upsampling, lossless, and private lazy guards such
as `InitGetCoeffs`; calling three public initializers does not prove that
every reachable guard is settled. Initialization runs with exclusive decoder
ownership. Subsequent calls use the same fixed CPU-feature callback. Record
any lazy writes that remain; serialization protects them, but parallel
decoding cannot begin until they too are made safe.

Allocation accounting still needs serialization: the selected allocation
callbacks have no decoder argument and libos64 exposes no current-thread ID.
For the first slice, use a private atomic ownership word with **timed sleeping
contention waits**. Attempt acquisition with acquire ordering; on contention,
call `os64_sleep(1)` and recheck. Release with a release store after cleanup
and clearing the active context. Do not spin or call `os64_yield` through the
decode. Signal-interrupted sleeps recheck the predicate. The active context
may refer to a call-local structure because upstream starts no worker threads.

This uses an existing non-GUI park, with no new handles or kernel API.
`os64_sleep(1)` rounds up to the active scheduler tick. It is periodic
rechecking, not a wake-on-release mutex: it spends a brief acquisition attempt
per wake and may add a tick plus scheduling latency after release. It promises
neither FIFO fairness nor prompt cancellation. Test that queued jobs make
progress under the bounded worker load. `os64_gui_event_wait` requires a
window and shares its event queue; it is not a generic address-based wait
primitive suitable for a codec used by command-line programs.

Measure waiting-worker CPU use, contention latency, navigation cancellation,
and pool shutdown in the guest. If periodic sleeping or serialization misses
the measured responsiveness target, revise this gate before shipping. A
wake-driven primitive or thread-keyed allocation contexts is a separate
change, not an assumed feature of the existing event API.

Calls must not re-enter from signal handlers or callbacks. Output release
uses the ordinary heap and does not need the decoder lock. Take no Yonder
pool, cache, or UI lock across the codec call. The private lock protects WebP
only; JPEG, PNG, networking, and UI work remain independent. In
`kernel/src/task.c`, `task_map_shared_object` marks writable library VMAs
copy-on-write. Writes privatize those pages per task, so the initialized
tables, ownership word, and active context belong to the process.

A later parallel implementation needs both safe publication of initialized
tables and independent allocation contexts. A current-thread identity API
plus a synchronized thread-keyed context table is one possible route without
a TLS ABI. It must prove context lifetime, thread-ID reuse, and cleanup before
dropping serialization. No new TLS ABI, pthread emulation, or kernel primitive
is part of this slice.

Keep the first slice's synchronous decoder API. Incremental internal feeding
is a follow-up option for cancellation: a callback-aware entry point could
poll between `WebPIAppend` calls while still withholding output until complete
success. It is not a free cancellation guarantee: one append may perform a
large amount of work, appending can copy input internally, and the callback,
cancellation status, wait behavior, and additional allocation charges need a
defined interface and measured evidence. Check cancellation before and after
the synchronous call using the existing worker lifecycle.

## CPU implementation

Use the scalar implementations plus upstream's SSE2 decoder intrinsics for
the production candidate. Os64 userland already builds with `-msse2` and
preserves that register state per thread. Supply a private `VP8GetCPUInfo`
definition reporting SSE2 and no other optional CPU features, excluding the
upstream probing implementation from the source closure. Keep the callback
fixed before initialization and hidden from consumers. This avoids runtime
CPU probing without sacrificing the supported baseline instructions.

With `HAVE_CONFIG_H`, enable the selected SSE2 sources and disable SSE4.1,
AVX/AVX2 and other architecture backends explicitly. Verify compiled code and
imports rather than relying on source filenames. Build a separate scalar
comparison artifact; do not toggle dispatch in a live initialized library.
Compare pixels and measure both builds on representative and near-limit P5
files before accepting the production choice. Do not assume a speedup factor.

Keep imported files pristine. Put configuration, C-runtime mappings, wrapper,
and any generated adaptation in `port/`, with narrow source-specific flags.
Use explicit feature settings to exclude worker threading and CPU backends
outside the selected baseline.
Audit the final binary for unexpected OS imports, CPU probing, encoding,
threading, stdio, environment access, and external dependencies. If a source
adaptation cannot be expressed without a patch, retain the original and record
the exact patch and reason separately. Keep assertions active in host testing;
malformed guest input must return a status rather than rely on assertions.

## Build and application integration

Add `sources.mk`, `shared.mk`, `exports.map`, and the public header beneath
`userland/libwebp/`. Export the four `os64_webp_` functions and hide upstream
symbols. The intended dependency is `libwebp.so -> libos64.so`; verify it from
the linked artifact. `libimage.so` gains `libwebp.so` as a dependency.

Register the library through the existing base-assignment machinery in
`userland/GNUmakefile` and `userland/tools/app_bases.py`; rebuild consumers
as needed if slot assignment changes. Follow existing PIC, relocation,
W^X, header-dependency, and source-list rebuild rules. Package the library
and license into both disk image formats and include them in P5 deployment.

Keep `image/webp` out of `OS64_IMAGE_ACCEPT` in the still-image slice.
Explicit advertisement can make a server replace a working animated GIF
with animated WebP, which this slice refuses. The accepted sequencing is
signature decoding first, animation next, then WebP negotiation. Retaining
the existing wildcard fallback does not guarantee that a server will avoid
WebP; it avoids introducing a new explicit preference for it.

Restate the rule in `image/image.h`: an Accept entry promises support for
the media type's ordinary still/animated uses, not merely recognition of
some files with that signature. Update the matching `picture.c` comment in
the implementation commit. Tests must distinguish supported signature dispatch
from advertised types: add WebP decode cases and assert its absence from
Accept until animation ships. Cover both picture requests and navigation's
`trip.c -> libway` use of the list.

The current page collector (`libpage/core.c`, `add_image`) uses image `src`;
it does not select `<picture><source type=...>` or `srcset`. The flow builder
treats `source` as boxless. There is no Accept-based source-selection site
to update in this tree. Preserve the `img` fallback in a fixture and recheck
this path at implementation time if concurrent Yonder work adds selection.

Keep `webp` visible in the census when animation returns `UNSUPPORTED`.
Extend the bounded format sniffer to recognize the VP8X animation flag,
record format on unsupported variants as well as unknown formats, and carry
the animation reason separately. Update `picture.c`, `picture.h`, `diag.c`,
and `yonder.c`'s `picture_record`: count the missing format as `webp` and
report the address with `webp, animated`. Do not let its existing format arm
discard the reason or imply that still WebP is undecodable. Use the animation
label only for a bounded, recognized animated container; malformed files
retain their decode failure. Verify the actual serialized census output.

The existing image-sequence API can wrap a successfully decoded still WebP
as one frame. It must propagate the animation refusal rather than fabricate
a still result. Derive Yonder's `PICTURE_RESERVE` from the measured decoder
budget, and update the comments that enumerate codecs,
memory costs, dependency lists, or supported formats in the same change.

## Validation and acceptance

Use a host harness for the production wrapper and selected source set, plus
a pristine host build of the pinned upstream decoder as the port oracle.
Set `MODE_BGRA`, fancy upsampling on, and scaling/cropping off on both sides.
Use the production SSE2 dispatch for the primary comparison and also compare
the scalar build. This proves port agreement, not independent verification
of the codec algorithm. Add
known-pixel lossless fixtures and independently prepared container cases to
check our contract rather than just comparing the library with itself.

Required evidence:

1. Lossy and lossless decoding, simple and extended containers, compressed
   and uncompressed alpha, odd dimensions, tiny images, transparent edges,
   and opaque output. Compare dimensions and pixels against the oracle;
   test the distinct VP8 and VP8L axis boundaries with narrow images.
2. Complete-buffer framing: every truncated prefix of small valid fixtures,
   corrupt sizes and padding, missing or duplicate reconstruction chunks,
   mismatched dimensions, unknown chunks, trailing bytes, and truncated
   metadata after the raster. Check each result against the declared policy.
3. Animated containers, including one-frame animation, return unsupported;
   still sequence wrapping and the existing GIF sequence behavior still work.
4. Boundary pixel and memory caps, arithmetic overflow, and failure injection
   at each allocation. No leaked temporary blocks, partial result, stale
   context, or held lock. A valid decode must succeed after each refusal.
5. Concurrent first use and mixed repeated success/failure with distinct
   budgets and fixtures. Run through the public wrapper; separate returned
   images remain valid while subsequent calls run. Check initialization
   publication, sleeping contention, interrupted waits, and progress without
   a GUI window. Check multiple processes in the guest as well as multiple
   threads.
6. ASan/UBSan with leak detection enabled. If sandbox thread attachment
   prevents LeakSanitizer from running, diagnose and rerun the same binaries
   outside the sandbox through the normal approval mechanism. Record any
   missing leak evidence; do not silently disable it.
7. Guest `webptest` through the production shared library and heap: expected
   pixels, transparency, refusal cleanup, repeated decode/free, concurrency,
   and heap validation. Host success does not replace guest evidence.
8. Import hashes, notices, exported symbols, dependencies, loader relocations,
   and installed image contents. Update sibling harnesses and audits that
   link or assert the dependency list of `libimage`, including the JPEG audit.
9. A controlled Yonder page with several WebPs, a misleading extension/MIME
   case, animation refusal with `webp, animated` in the census, and navigation
   during decode; gview and wallpaper visual checks. Verify request headers
   omit explicit WebP negotiation and a negotiation fixture still serves its
   working GIF animation. Check the `picture` element's `img` fallback.
   Record waiting-worker CPU use, latency, and peak charged bytes on QEMU and
   the P5, including scalar/SSE2 comparisons, with machine/build details.

The WallpaperCave `.jpeg` URL that served AVIF is not a WebP acceptance
fixture. WebP support does not make that file decodable.

## Implementation sequence

1. Pin and audit the import, license inventory, selected source closure, and
   runtime requirements. Produce SSE2 and scalar characterization builds,
   the working-set table, selected caps, and the binary audit. Choose the
   decoder budget and resulting pool reservation before integration.
2. Implement the bounded wrapper, framing checks, serialized allocation
   context with sleeping contention, initialization, and host corpus.
   Resolve dependency and concurrency mismatches before enabling WebP dispatch.
3. Wire `libimage`, packaging, notices, guest tests, and browser acceptance.
   Keep WebP out of Accept; update its rule and the census reporting.
   Review touched comments and sibling codec harnesses with the changes.
4. Implement animation as the next slice, using upstream demux and
   `WebPAnimDecoder` for composition/disposal. Define sequence ownership,
   timing and loop semantics, and a new memory/reservation analysis. Add
   `image/webp` to Accept with the animated sequence integration and its tests.

Step 1's import, source audit, host characterization, and selected defaults
are recorded in [the evidence report](../../webp-evidence/README.md).
It does not include guest execution or P5 performance results.
The acceptance items above are requirements for implementation, not results
claimed by this design.
