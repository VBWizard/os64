# libwebp decoder import

This directory contains the pristine libwebp 1.6.0 decoder source closure and
the compatibility code used by `libwebp.so`, host probes and freestanding
audits. The public wrapper decodes still images through `libimage.so`, with
bounded allocation and serialized calls. Both disk images include the library
and notices. Animation remains unsupported and WebP is absent from HTTP Accept.

`manifest.json` records the archive, release commit and per-file SHA-256 hashes.
`sources.mk` selects 27 common upstream translation units and seven SSE2 units;
`port/cpu.c` supplies the fixed SSE2 callback. The comparison build omits those
seven units and reports no optional CPU features. It remains x86-64 code and
may contain compiler-generated SSE2 instructions.

The closure follows upstream's decoder target. It retains incremental and
rescaler sources as dependencies of upstream decoder entry points; this does
not enable those features in the Os64 interface. Encoder helpers in
shared utility files are discarded using function/data sections and linker
garbage collection. `palette.c`, the encoder, upstream CPU probing, optional
ISA backends, demux and mux are excluded. `thread_utils.c` supplies the
synchronous worker interface with threading disabled.

The pristine headers retain required type declarations, including declarations
used by discarded helpers. Source filenames and declarations are not used as
proof of the executable dependency closure: `tools/audit_webp.py` examines the
retained code and imports after linking.

The compatibility headers redirect allocation and memory operations. Temporary
allocations carry aligned size/list records; the output is an ordinary heap
allocation. The whole upstream call and cleanup run under a gate whose waiting
callers use `os64_sleep(1)`. Eager DSP initialization runs under the same gate;
the private `InitGetCoeffs` guard remains lazy and is protected by serialization.
Guest assertions trap; host assertions remain enabled.

`tools/webp_generate.py` creates an adapted `alpha_dec.c` outside `upstream/`
and checks its exact diff against `port/alpha_dec.patch`. Upstream 1.6.0 rejects
reserved alpha bits and unknown preprocessing hints; the WebP container spec
requires ignoring the reserved bits and leaves preprocessing use optional.
The adaptation removes those rejection conditions while retaining compression
and filter validation. Regression fixtures cover both changes. Original files
remain byte-for-byte pinned. After full RIFF framing validation, the wrapper
borrows the reconstruction span for upstream's internal decoder, so ignored
metadata and future VP8X extension fields need no input copy or further patch.

`COPYING`, `PATENTS`, `AUTHORS`, and source notices retain upstream attribution.
The combined notice at `license/libwebp-LICENSE` is checked byte-for-byte by the
audit, embedded by `os64_webp_license()` and installed at `/etc/licenses/libwebp.txt`.

See [design](../../docs/design/pending/WEBP.md) and
[measurement evidence](../../docs/webp-evidence/README.md) for the supported
profile, selected limits, reproduction commands and remaining validation.
