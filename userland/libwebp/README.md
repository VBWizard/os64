# libwebp decoder import

This directory contains the pristine libwebp 1.6.0 decoder source closure and
the compatibility headers used by the host probes and freestanding audit.
It is not wired into the Os64 build or image dispatcher. The public wrapper,
bounded allocator, serialization, shared-library ABI and packaging are the
next implementation step.

`manifest.json` records the archive, release commit and per-file SHA-256 hashes.
`sources.mk` selects 27 common upstream translation units and seven SSE2 units;
`port/cpu.c` supplies the fixed SSE2 callback. The comparison build omits those
seven units and reports no optional CPU features. It remains x86-64 code and
may contain compiler-generated SSE2 instructions.

The closure follows upstream's decoder target. It retains incremental and
rescaler sources as dependencies of upstream decoder entry points; this does
not enable those features in the proposed Os64 interface. Encoder helpers in
shared utility files are discarded using function/data sections and linker
garbage collection. `palette.c`, the encoder, upstream CPU probing, optional
ISA backends, demux and mux are excluded. `thread_utils.c` supplies the
synchronous worker interface with threading disabled.

The pristine headers retain required type declarations, including declarations
used by discarded helpers. Source filenames and declarations are not used as
proof of the executable dependency closure: `tools/audit_webp.py` examines the
retained code and imports after linking.

The compatibility headers redirect allocation and memory operations without
patching upstream. The probes supply the allocation implementation. A guest
allocator implementation is not provided by this milestone. Guest assertions
trap; host assertions remain enabled.

`COPYING`, `PATENTS`, `AUTHORS`, and source notices retain upstream attribution.
The combined notice at `license/libwebp-LICENSE` is checked byte-for-byte by the
audit. Its installation and embedding belong to consumer integration.

See [design](../../docs/design/pending/WEBP.md) and
[measurement evidence](../../docs/webp-evidence/README.md) for the supported
profile, selected limits, reproduction commands and remaining validation.
