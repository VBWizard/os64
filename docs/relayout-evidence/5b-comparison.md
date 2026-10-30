# Relayout 5b: matched comparison

The original P5 paragraph median improved by 43.2% after targeted
invalidation. List replacement was near its earlier baseline, while the
non-positional and captured-article comparisons remained open. The evidence
in [5b-validation.md](5b-validation.md) is not yet performance acceptance.

## Workloads and control

The control starts at `9fbeb1fc` (5a; code identical to the original baseline
at `4828f4d8`). It adds the same stream profiling as the candidate, plus test
harnesses. It contains no incremental cascade or computed-style reuse.
The candidate builds on `da39ced5`. Both cohorts use identical fixture bytes
and shared libraries except Yonder, libgarb and libflow. libjs is the same
previously tested binary in both cohorts; source code is unchanged between
these revisions. Unchanged binaries are reused, and SHA-256 manifests identify
what was delivered. Local handoff records retain the final source revisions.

Compare these three workloads in fresh browser processes:

1. `layout-measure.html`: unchanged 2,000-row / 20-sample baseline, five warm-ups.
2. `layout-measure-local.html`: identical DOM/script, with the positional stripe
   selector replaced by a uniform background. This isolates the effect of
   narrower dependencies without rewriting the baseline.
3. `layout-stream.html`: a starter for an instrumented captured Wikipedia
   article. Set the viewport before following its link. The article forces
   geometry at 18 fixed parser checkpoints and reports the raw durations,
   heights, total, median, maximum and viewport. The checkpoints have different
   document sizes; their median is not a repeated-operation latency estimate.

Use `yonder --layout-profile /tests/pages/<filename>`. Keep Scripts and Modern
Web enabled, dark appearance, 100% zoom and a fixed 2,526 × 1,259 CSS-pixel
viewport. Save each completed screen and its native record with `d` while
ordinary page text has focus. Run the control first and then the candidate,
restarting Yonder before each workload. Repeat a near-tie or anomalous result
before interpreting it as a regression or improvement.

## Captured article and limits

The input is the captured `https://en.wikipedia.org/wiki/Hobby_operating_system`
page, corpus entry 327. Its HTML SHA-256 is
`7b37995f6895346338c093dfa5543c58dcead758617014eec76959074a3ec5e3`.
The private capture retains its diagnostic, HTML and two downloaded CSS files.
Generate the workload with:

```
python3 tools/layout_stream_fixture.py /capture/en.wikipedia.org-0327.txt /output/pages
bash tools/test_layout_stream.sh /output/pages/layout-stream-article.html /output/proof
```

The generator inlines captured linked CSS in document order and preserves
inline CSS and selectors, including `:has` and `:dir`. It removes original
scripts, handlers, embedded documents and asset attributes, and replaces
non-fragment CSS resource URLs with empty data URLs. It rejects missing linked
CSS, imports and style terminators. Its HTML/CSS normalization is specific to
this fixture, not a general web sanitizer or faithful live-site replay.
`layout-stream-inputs.json` records input/output hashes, stylesheet order and
checkpoint character offsets. The generated article SHA-256 is
`b56f866156c264b6b085c83535637d32022783202a8f283f1e35c484500700fc`.
Captured third-party page content is not committed to the repository.

Yonder does not lay out every received chunk. The injected synchronous geometry
queries exercise its real growing-parser document path. The new `layout stream`
fact separates that work from the final displayed `layout` fact. It borrows the
stream's diagnostic without changing ownership. The broad selectors in this
capture require full cascades, so **zero updates and zero copied styles are
expected**. This is a fallback non-regression workload, not a reuse demo or a
measurement of live Wikipedia network/asset/script performance.

## Corrections exposed by the comparison

A full cascade cannot reuse styles from a different generation. The candidate
now releases an unusable hidden measurement before constructing its replacement,
including when changed stylesheet inputs prevented selecting that old source.
The new sheet table already holds the parses it needs. The shown page retains
its owners, and measurement failure drops stale geometry. The old comment that
the cached measurement was always released first was wrong; it now describes
retention for reuse and early release on full fallback.

Incomplete cascades and cascades with whole-tree selector dependencies no
longer register a mutation-log reader they cannot use. With no other readers,
parser/DOM mutations avoid allocating those log entries. Independent readers
continue receiving their changes.

## Validation and directional target evidence

ASan, UBSan and LeakSanitizer remain enabled:

- Full libgarb suite passes; 1,705 update checks, nine inline-update and six
  slot-growth allocation-failure positions.
- Focused libflow restyle suite: 25,463 checks, including 164 allocation positions
  failed individually and persistently; zero failures.
- Full candidate Yonder scripted-page suite: 12,566 checks, zero failures.
  Fallback cases preserve displayed styles through hidden replacement/refusal.
- Focused profiling checks pass on both builds: 23 checks each. Nonzero fake
  clocks check stream phase totals, disabled profiling, quiet streams and
  diagnostic transfer. Stream measurements do not inflate displayed totals.
- Both original/companion fixtures complete at 200/3 and 2,000/20 on both
  builds: four runs of 16 checks each, zero failures.
- The captured article passes nine host checks on each build, with zero leaks,
  no external stylesheet/script jobs and complete geometry results. Host fake
  clock values are not performance evidence. Target binaries cross-build.

Sequential QEMU article runs use the same inputs, 828 × 509 CSS pixels,
100% zoom and light appearance:

| Build | 18-checkpoint total | Median checkpoint | Worst checkpoint |
|---|---:|---:|---:|
| Instrumented 5a control | 5,692.700 ms | 281.700 ms | 526.900 ms |
| Candidate before these fallback corrections | 6,501.800 ms | 316.800 ms | 733.200 ms |
| Candidate with both corrections | 5,774.300 ms | 289.150 ms | 535.000 ms |

The corrected total is 11.2% below the uncorrected candidate and 1.4% above
control. These individual emulator runs do not establish equivalence or a
hardware speedup. An intermediate narrower release experiment totalled
6,391.100 ms with a host suite running alongside it; that run is retained but
is not a clean timing comparison.

All three main runs report 17 model rebuilds, 18 stream layouts, 7,825 elements,
5,209 boxes, 3,043 measures and zero refusals/clock errors. Both candidate runs
report zero cascade updates and copied styles. The final displayed layout is
separate: 935 elements, 507 boxes and 326 measures.

Control stream cascade/layout totals are 3,495.141/579.560 ms; corrected
candidate totals are 4,878.285/546.870 ms. The larger candidate cascade phase
includes releasing the old hidden measurement, whereas the control releases
it outside that phase. Compare end-to-end checkpoint timings as well as phases;
the phase difference alone cannot be attributed to selector work.

**P5 acceptance remains open.** The frozen paired cohorts are ready for all
three workloads. Do not infer a streamed-page speedup from a final screenshot,
zero counters, or the paragraph result, and do not proceed to 5c on emulator
numbers alone.
