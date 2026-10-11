# P5 WebP benchmark

Build/install the WebP worktree, including the `tests` lot, then run in Os64:

```sh
/tests/webpbench
```

The command runs both SSE2 and scalar decoders and writes
`/home/webp-bench.txt`, replacing a previous report. Progress goes to the
terminal. Keep the machine otherwise idle for the timing run and bring
back the complete report. A final `PASS` means the benchmark completed
with correct pixels and clean heap checks; it does not certify browser
navigation or cancellation. The report has a final `complete ... result=0`
line, and the command returns zero on success. An interrupted report is
partial evidence, not a pass.

`/tests/webpbench --quick` is a smaller installation/smoke check. It uses
512×512 fixtures and does not replace the full P5 measurement.

Both `/tests/webpbench` and `/tests/webpbenchscalar` must be installed. They
are included in both disk images and use the existing `@tests` os64get
route. Fixtures are embedded, so no image downloads or suffix routing are
needed for this benchmark. `webpbenchscalar` is the helper; the main command
runs it automatically and checks its exit status. Its output shares the
parent's report handle through `os64_spawn_redirected`.

## What is measured

The fixed corpus contains lossy, lossless, and lossy-with-alpha gradients at
512×512 and 4096×4096. The larger size reaches the 16 Mi-pixel default cap
and produces a 64 MiB output. Each mode records a small warmup separately,
three serial samples for each small fixture, one serial sample for each
large fixture, and a burst of six workers decoding the large alpha fixture.
Quick mode uses the small alpha fixture for that burst. Each decoded output
is checked against a reference CRC from Pillow's decoder before it is freed.
Input CRCs, dimensions, compiler/build timestamps, CPU brand, and online
core count are recorded too.

Both executables statically link the production wrapper and pinned decoder
sources with benchmark-only timing instrumentation. Their DSP state is
independent; no dispatch pointer is changed in a running initialized decoder.
The installed `libwebp.so` has no profiling clocks or extra ABI exports.
These are instrumented decoder builds, not timings of Yonder itself or the
dynamic-library call overhead. Scalar selects upstream's scalar backend;
it still uses the x86-64 compiler/ABI baseline, including SSE2 where the
compiler chooses it.

| Field | Meaning |
| --- | --- |
| `call_us` | Wall time inside the decode call, including its internal cleanup and waiting. |
| `gate_us` | Time acquiring the decoder gate; includes DSP initialization in the warmup. |
| `allocate_us` | Time in scratch/output heap allocation calls, including heap-lock waiting. |
| `scratch_free_us` | Time in upstream scratch-buffer heap frees, including lock waiting and unmap. |
| `other_decode_us` | Call time minus those three measured components. Includes reconstruction, fills/copies, demand faults, scheduling, and instrumentation overhead; **not pure decoder CPU time**. |
| `verify_us` | Pixel CRC verification outside the decode/free intervals. |
| `output_free_us` | Time releasing the finished pixel buffer through `os64_webp_free`. |
| `process_cpu_us` | Process CPU accounting delta for the concurrent burst, including verification, output release, and join work. It does not isolate waiting-worker CPU time. |

Worker results are stored separately and printed after joining. Output
release may contend with another worker's decoding, which is deliberate:
that interaction occurs in Yonder too. Serial samples separate it from
the concurrent case. The modes run SSE2 first, scalar second; thermal/cache
and background-load effects can affect comparisons. Large serial fixtures
have one sample each, so repeat the full run before interpreting small
differences. Preserve each report before running again.

## Analysis and reproduction

On the host:

```sh
python3 tools/analyze_webp_bench.py /path/to/webp-bench.txt
```

The analyzer requires both complete modes, the expected fixture/sample
counts, matching input/reference pixel CRCs, nonnegative phase accounting,
valid process CPU readings, six workers, clean heap verification, and a
successful scalar helper and overall completion. It prints serial medians
and contention summaries. CRCs are regression checks, not cryptographic
proofs of pixel identity.

[`fixtures.json`](../../userland/tests/webpbench/fixtures.json) records
input SHA-256 values, reference pixel CRCs, and the fixture encoder version.
Regeneration is explicit and outside the normal build:

```sh
python3 tools/generate_webp_bench.py --output userland/tests/webpbench
make
```

The source and fixture dependencies rebuild both executables. The scalar
decoder object excludes the SSE2 source group; the SSE2 object includes it.
`tools/audit_webp.py --shared --images --output /tmp/webp-audit` checks that
production library imports/exports remain unchanged and both benchmark
executables are packaged byte-for-byte in ext2 and FAT images.

## Validation before the P5 run

The full root build passed. On eight-core QEMU (q35, software emulation,
8 GiB, FAT root/ext2 home), both the quick and full commands completed
successfully. The full report contains 38 checked decodes across the two
backends, including both six-worker bursts and the 4096×4096 fixtures.
Heap verification was clean and the full command returned zero. An invalid
option returned two without replacing the completed report. The analyzer
accepted both reports and rejected truncated, bad-pixel, missing-scalar,
and nonzero-result variants.

Evidence: [full report](benchmark/qemu-full.txt),
[full summary](benchmark/qemu-full-summary.json),
[exit status](benchmark/qemu-full-exit.txt),
[quick report](benchmark/qemu-quick.txt),
[invalid-option status](benchmark/qemu-invalid-option-exit.txt).
The [packaging/ABI audit](benchmark/audit.txt) passed for 13 payloads in
both image formats. Symbol inspection confirmed that the scalar benchmark
object has no SSE2 backend, the SSE2 object includes it, and production
`libwebp.so` has neither the benchmark entry point nor a clock import.
The production wrapper's [ASan/UBSan/LeakSanitizer suite](benchmark/wrapper-sanitizers.txt)
also passed outside the sandbox with leak detection enabled. This sanitizer
result covers the production wrapper, not the guest benchmark instrumentation.

These QEMU results validate the measuring tool; they do not establish a
browser stress pass. The separate hardware run is recorded below.

## First P5 report — 2026-10-10

Chris ran the full benchmark on the P5; the retrieved
[report](benchmark/p5-full.txt) passes the analyzer, with its
[summary](benchmark/p5-full-summary.json) retained alongside it. It identifies
an AMD Ryzen 5 6600H with Radeon Graphics and 12 online logical CPUs,
GCC 14.2.0, and benchmark builds dated 2026-10-10. Both backends passed all
38 combined pixel checks and heap verification; the scalar helper exited
zero and the report ends with `complete scope=full result=0`.

The 4096×4096 serial samples measured:

| Fixture | SSE2 decode call | Scalar decode call | SSE2 scratch free | SSE2 output free |
| --- | --- | --- | --- | --- |
| Lossy | 317.36 ms | 315.49 ms | 0.40 ms | 67.32 ms |
| Lossless | 249.38 ms | 321.31 ms | 66.39 ms | 66.54 ms |
| Alpha | 525.46 ms | 509.80 ms | 83.48 ms | 67.70 ms |

Decode-call time includes scratch release; output release is additional.
The six-worker alpha burst took 5.531 s with SSE2 and 5.481 s with scalar;
maximum gate waits were 4.247 s and 4.227 s. Process CPU accounting for
those bursts was 7.194 s and 7.162 s, including decoding, verification,
cleanup, and join work, not just waiting.

This run supports retaining SSE2: its lossless decode call was about 22%
shorter, while the lossy/alpha differences were small and did not favor
SSE2 in this sample. One sample per large serial fixture, fixed backend
order, and unspecified background load do not establish a general speedup
or a regression in the other cases. Repeat measurements remain useful.

The P5's roughly 67 ms output release is far below the approximately
5.8 s in the eight-core QEMU run, so the emulator's cleanup latency must
not be attributed to the hardware. Cleanup and serial gate waiting are
still measurable, and the batching debt remains open. This benchmark does
not establish Yonder navigation/close latency, cancellation, or ordinary
site compatibility on the P5.

## Browsing on the P5

Revisiting sites whose WebP stills failed is useful with this build. Install
the matching Yonder, shared libraries (including `libwebp.so` and
`libimage.so`), and tests from the same WebP build. Build from the intended
worktree directory so the deployment uses its sources and artifacts.

Lossy/lossless stills and alpha are supported. Animated WebP and AVIF remain
unsupported. HTTP Accept does not advertise WebP yet, so a site that selects
formats using that header may continue to send JPEG/PNG. Explicit WebP URLs
and page resources can still exercise signature-based decoding. Record
the page URL and Yonder's page diagnostic for failures, including whether
the image was animated. Large-image navigation/close acceptance remains
open with the [shootdown performance debt](memory-profile.md).
