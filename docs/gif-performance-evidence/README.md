# GIF decoder performance evidence

Measured 2026-10-02 (America/New_York), against userland after PR #206:
`ae0a23d5f5af659c6d82a63e07fd831d1f96200b`.

## Change and result

The shared-library build enables `-O2` for `gif.c`. LZW expansion also avoids
palette/alpha work while producing sequence indices. Non-interlaced rasters
write an expanded string contiguously; interlaced rasters and first-picture
output split strings at row boundaries and choose an output mode/address per
span. Palette validation still happens before composition publishes a frame.

Across 26 animated GIFs (530 distinct frames), the final decoder/composition
path was **2.51–4.01x faster** than the unchanged decoder at `-O0`; the median
individual-asset speedup was **3.07x**. Compiler optimization alone yielded
2.09–3.15x. The code change at the same `-O2` level added 1.09–1.34x, with a
median of 1.24x. These ratios compare the medians of three samples per build
and asset. Each asset's requested owned storage was identical across builds;
playback made no allocations.

Representative times below are microseconds of host CPU per advanced frame,
including raster expansion, GIF disposal/composition and loop resets:

| Asset | Baseline `-O0` | Baseline `-O2` | Current `-O2` | Combined speedup |
| --- | ---: | ---: | ---: | ---: |
| roach.gif | 92.12 | 39.99 | 32.66 | 2.82x |
| tvstatic.gif | 186.63 | 74.28 | 60.63 | 3.08x |
| coolpenguin.gif | 99.79 | 43.26 | 39.75 | 2.51x |
| sparkle-sparkles.gif | 66.22 | 25.31 | 20.80 | 3.18x |
| feuer-fire.gif | 340.71 | 130.06 | 104.90 | 3.25x |
| anime-anime-girl-dance.gif | 601.78 | 256.96 | 220.33 | 2.73x |

These are host component measurements on a Ryzen 9 3900X under WSL2, using
GCC 13.3.0 and CPU affinity 10. They do not measure Yonder painting, UI damage,
animation scheduling, the compositor, or P5 CPU usage. The corpus covers the
animated assets referenced by the downloaded danlegt.com front page; it is
broader than the reported eight visible animations, and does not model their
positions, repetition as CSS backgrounds, or playback cadence. No conclusion
about current Yonder painting cost follows from this run.

## Method and reproduction

[results.json](results.json) records decoder source hashes, asset URLs and
hashes, dimensions, frame counts, requested owned bytes, individual timing
samples and medians. External artwork is not checked into the repository.
Download the recorded URLs and check their hashes when reproducing this
corpus; content at those URLs can change.

The benchmark uses the public sequence API with host adapters shared with
the sequence oracle. Adapters use host libc memory operations. Harness and
`image.c` objects stay at `-O2` in all three builds; the GIF object is the
variable. Opening, file I/O and a full warm-up pass are outside the timer.
The timed interval uses `CLOCK_PROCESS_CPUTIME_ID`, advances in batches of
128 until at least 0.4 CPU seconds, and includes loop resets. Each sample is
a fresh process. Build order rotates between repetitions. Timing has no
sanitizer overhead; correctness uses separate sanitizer runs.

For example, with valid multi-frame inputs downloaded from the recorded
URLs:

```sh
python3 tools/bench_gif_host.py \
    --baseline-ref ae0a23d5f5af659c6d82a63e07fd831d1f96200b \
    --seconds 0.4 --repeats 3 --cpu 10 --output /tmp/gif-results.json \
    /path/to/roach.gif /path/to/coolpenguin.gif /path/to/feuer-fire.gif
```

Choose a permitted host CPU for `--cpu`, or omit affinity. Omit
`--baseline-ref` to measure the current decoder alone. The baseline must use
compatible internal headers and the sequence API. The runner compares frame
counts, dimensions and owned bytes, and asserts allocation-free playback
and complete cleanup. It requires valid multi-frame inputs; use the pixel
oracles to verify new samples before treating their timing as evidence.

## Validation

- `tools/test_gif_host.py`: 132 fixtures passed at both `-O0` and `-O2`, with
  exact Pillow pixels, truncated prefixes, mutations and allocation failures.
- `tools/test_gif_sequence_host.py`: 42 synthetic cases passed at both levels;
  the `-O2` run also passed all 26 external animations, checking each composed
  frame, delay and loop behavior. The runs used ASan, UBSan and normal leak
  checking. Rollback, ownership, disposal and resource limits passed.
- New referenced-string fixtures cross three-pixel rows and interlace pass
  boundaries, with local palettes, frame offsets, transparency and
  restore-to-previous disposal. Their expected pixels come from Pillow.
- The neighboring JPEG corpus passed; libimage/libdraw passed 180 checks
  with zero failures.
- Strict userland and full OS/ISO builds passed with cross GCC 14.2.0. The
  GIF compile command carries `-O2`; `image.c` keeps its existing flags. The
  optimized GIF object has no new compiler-runtime dependencies.
- QEMU `/tests/giftest` passed with exit status 0, including the new vectors,
  file loading, disposal, looping, rewind and heap checks. The guest's
  `libimage.so` matched the built library byte for byte (SHA-256
  `a53e3422502d32358f4942aaf2deb5e5caaa3670bc0ffd61529ef5f0a206a68e`).

The sequence memory budget and failure contract are unchanged. A decoded
frame cache would be a separate memory/performance tradeoff; this change
retains on-demand expansion and does not add cache storage. P5 timing remains
to be measured after deployment.
