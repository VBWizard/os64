# JPEG decoding for os64

The codec uses a pinned decoder-only import of libjpeg-turbo 3.2.0 behind
os64-owned names in libjpeg.so. No kernel changes are required. libimage.so
owns format dispatch and moves BMP/PPM out of libos64; gview and desktop use
that common image API. libjpeg depends on libos64, not on graphics or files.

## First supported profile

Complete-buffer 8-bit sequential and progressive Huffman JPEG, grayscale,
RGB/YCbCr and CMYK/YCCK. Scalar accurate integer IDCT, no SIMD, encoding,
arithmetic or lossless JPEG, 12/16-bit output, ICC color management, or
incremental presentation. Unsupported variants return UNSUPPORTED.
CMYK conversion is a device-color approximation, not ICC-managed output.

EXIF orientation 1 through 8 is applied to the returned opaque 0xAARRGGBB
pixel plane, with dimensions swapped for transposes. Read only the primary
IFD orientation before the first scan from bounded APP1 Exif/TIFF data. Do not follow thumbnail or
sub-IFD pointers. Invalid/ambiguous orientation metadata refuses the image;
unrelated metadata is skipped. The orientation step writes decoded rows
directly into their final positions, without another full image allocation.

## Resource and failure contracts

Defaults: 20 MiB encoded input, 16,384 per dimension, 16 Mi pixels, 128 MiB
allocation budget per decode including output and working memory, and 256
scans. The budget counts requested bytes, including upstream pool headers;
libos64 allocator bookkeeping, page rounding and stack space are outside it. Limits are enforced before allocation/expansion. The caller's input
buffer is borrowed and its size bounded separately. Each decoder owns its
allocation accounting and error escape; no global mutable decoder state.

Warnings indicating damaged input, truncation and synthetic end-of-image
recovery refuse the decode. No partial pixels escape on failure. Successful
completion requires a real EOI marker. Bytes after EOI are ignored as trailing
container data. All allocations are freed on failure; success transfers only
the pixel plane to the result. Error statuses distinguish malformed input,
unsupported JPEG, resource limits and allocation failure.

The private error escape is a userland nonlocal return with an audited x86-64
ABI boundary. It is not a public setjmp API and never jumps across threads.

## Host and guest evidence

The host corpus cross-checks 25 valid images (baseline/progressive, three
subsampling modes, grayscale, CMYK/YCCK and both EXIF byte orders) and seven
explicit metadata/format/resource refusals. Valid fixtures also exercise every
truncated prefix, each allocation failure, deterministic mutations and two
concurrent decoders under ASan/UBSan. The adjacent BMP/PPM/blit harness passes
72 checks. PNG's independent color/filter/CRC/truncation suite also passes.

The guest `jpegtest` and `pngtest` pass in QEMU with the production shared
libraries and heap; the guest filesystem passes e2fsck. Fresh ext2 and FAT
images contain the license notice byte for byte, and ext2 contains both new
shared libraries. Chris also viewed five images successfully on the P5;
that visual checkout covers those images, not the full supported profile.

## Proof and packaging

Preserve imported bytes with a SHA-256 manifest; keep adaptations separate.
Ship upstream license notices and the IJG acknowledgment in the library and
at `/etc/licenses/libjpeg-turbo.txt` on ext2 and FAT images.
Host tests use independently generated baseline/progressive/color/subsampling
fixtures, orientation transforms, malformed/truncated input and allocation
failure injection under ASan/UBSan. Guest tests prove shared loading, real
allocation, oriented pixels and cleanup. gview provides the visual checkout.

## Build and checkout

`make -C userland -j4` builds the shared libraries and their consumers.
`python3 tools/test_jpeg_host.py` runs sanitized codec and BMP/PPM/blit tests.
`python3 tools/audit_jpeg.py` checks the pristine import and compiled ABI.
The import manifest pins the official release archive hash and each retained
file. Upstream files are unmodified; port/ holds configuration and adapters.
The upstream-only warning exceptions cover unused callback parameters and
signed/unsigned expressions; the wrapper keeps full project warning checks.

On the P5, transfer the changed executables/libraries as usual, including
libimage.so and libjpeg.so. Run `testrun jpegtest` and `testrun pngtest`, then
`gview /home/photo.jpg`. Portrait EXIF photos should open upright. Desktop
wallpaper uses the same loader. ICC-managed color and scaled-to-window display
are separate work; large photos remain subject to explicit resource limits.
