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
   article. Set the viewport on the starter, then type
   `file:///tests/pages/layout-stream-article.html` in the address bar. Clicked
   file links are refused by the current navigation policy; the address bar
   uses the local-file route. The article forces
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

## First matched P5 session: two gains, a regression signal, one invalid pair

The 2026-10-30 session used the frozen control `9dca4a7d` and candidate
`8406bc21`. Six completed screenshots and native reports are retained outside
`/tmp`, with hashes and a machine-readable summary. Corpus 458/459 are the
control original/companion pages, 461 the control article, and 462/463/464
are the corresponding candidate pages. Corpus 460 is the starter page and
is excluded from timing comparisons.

Both original/companion pairs show the same 2,526 × 1,259 CSS-pixel viewport,
device ratio 1, 2,000 rows, 20 samples and five warm-ups. Native records confirm
Scripts and Modern Web enabled, dark appearance and 100% zoom.

| Workload | Control median | Candidate median | Change | Control worst | Candidate worst |
|---|---:|---:|---:|---:|---:|
| Original: paragraph | 65.400 ms | 43.850 ms | -33.0% | 100.300 ms | 159.100 ms |
| Original: list replacement | 61.000 ms | 67.900 ms | +11.3% | 66.200 ms | 75.200 ms |
| Companion: paragraph | 60.200 ms | 43.750 ms | -27.3% | 82.800 ms | 100.700 ms |
| Companion: list replacement | 60.350 ms | 40.350 ms | -33.1% | 62.700 ms | 51.700 ms |

Unchanged geometry is 0.100 ms median / 0.200 ms worst in all four runs.
The candidate improves typical paragraph timing and both companion medians,
but the original list replacement is slower in this session. Paragraph maxima
also increase. These are individual runs, not estimates of between-run
variance; the original list result requires follow-up rather than dismissal.
The earlier refined candidate's near-baseline list result does not override it.

All four native records have 52 model rebuilds, 54 layout attempts, 106,499
elements, 105,851 boxes and 105,374 measures, with zero refusals/clock errors.
Candidate original/companion records show 26/51 cascade updates and
53,134/104,209 copied styles. Layout reuse remains zero.

| Cumulative phase | Original control | Original candidate | Companion control | Companion candidate |
|---|---:|---:|---:|---:|
| Model rebuild | 10.661 ms | 12.565 ms | 10.400 ms | 15.128 ms |
| Cascade | 528.709 ms | 268.350 ms | 346.790 ms | 14.246 ms |
| Computed styles | 122.580 ms | 120.669 ms | 138.108 ms | 156.917 ms |
| Boxes | 26.923 ms | 28.236 ms | 25.131 ms | 22.607 ms |
| Layout | 2,408.234 ms | 3,113.027 ms | 2,368.605 ms | 2,541.204 ms |
| Public tree | 58.642 ms | 59.887 ms | 46.052 ms | 55.427 ms |

These totals span setup, warm-ups and all workloads, so they cannot isolate
the list-replacement cost. The original candidate's cascade total is lower
while its layout total is higher; no cause is established from that alone.

### Article pair: viewport mismatch

Both articles complete 18 checkpoints with zero native refusals or clock
errors. Control reports 407.200 ms total, 20.350 ms median and 40.600 ms worst
at **2,526 × 1,259**. Candidate reports 402.400 / 20.000 / 47.300 ms at
**826 × 459**. Their checkpoint heights and native work counts differ:
control 7,927 elements / 5,294 boxes / 3,795 measures; candidate
7,825 / 5,209 / 3,043. Both report 17 model rebuilds and 18 stream layouts.

This is not a valid timing pair. A completed result only guarantees a stable
viewport during its own measurements, not agreement with another run; resizing
after completion does not rerun the checkpoints. Repeat the candidate article
at the control viewport before drawing a performance conclusion. Its zero
updates/copied styles remains expected for this broad-selector workload.

The starter's file link was refused on the P5. QEMU had opened the article
directly, missing that route. The corrected protocol uses typed address-bar
navigation after maximizing the starter, leaving the frozen cohorts unchanged.

**Acceptance remains open.** Next hardware evidence is a fresh candidate
original-page run to check the list regression signal, plus a candidate article
run with the matching viewport. Preserve the existing control/companion results;
there is no reason to discard or repeat all six runs.

### Corrected candidate article: corpus 466

The tester maximized the window and reloaded the article. Only the latest
record, corpus 466, is used for this rerun; intermediate record 465 is excluded.
The retained screenshot ending in `candidate2` reports 18 completed checkpoints
at 2,526 × 1,259, device ratio 1. Its checkpoint heights match the control.
The native record confirms dark appearance, 100% zoom, Scripts and Modern Web
on, 17 model rebuilds, 18 stream layouts, 7,927 elements, 5,294 boxes and 3,795
measures, with zero refusals/clock errors. These counts match control corpus 461.

| Article checkpoint timing | Control 461 | Candidate 466 |
|---|---:|---:|
| Total | 407.200 ms | 469.100 ms |
| Median | 20.350 ms | 24.250 ms |
| Worst | 40.600 ms | 54.200 ms |

The candidate total is 61.900 ms (15.2%) higher. This replaces the invalid
small-viewport candidate comparison; it does not meet the non-regression gate.
Reloading after maximizing provides a matching geometry workload, though one
run per build does not establish run-to-run variance or isolate the cause.

Candidate stream phases are model 3.261 ms, cascade 369.149 ms, styles
6.421 ms, boxes 1.391 ms, layout 83.720 ms and public tree 3.294 ms.
The final displayed layout is separate (941 elements, 512 boxes, 413 measures).
Zero updates/copied styles is expected. The cascade phase includes early
hidden-measurement release in the candidate, so its increase alone does not
measure extra selector work. Original-list follow-up and investigation of this
article slowdown remain open. Frozen delivery files are unchanged.
