# Relayout 5b: matched comparison

The current original-page paragraph/list medians are below the 5a control,
and the non-positional companion improves both mutations. Repeated article
measurements on the P5 are comparable to control; the earlier large article
slowdown did not recur in the controlled follow-up below. This evidence
supports review of 5b. **The alternating manual article comparison below
clears the performance gate.** Code review remains separate. The earlier
article result and elevated paragraph maximum remain disclosed; neither has
an established root cause.

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

The companion results in this table are from candidate `8406bc21` (corpus
463), not the later r3 build `483df1bb`. The production-code delta between
those candidates is confined to libgarb declaration-order scratch reuse;
there is no fresh companion hardware measurement at r3.

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

## Full-cascade scratch follow-up: initial experiment

Temporary host and target instrumentation compared the control and candidate
before changing code. The host article trace had 36 additional allocation
attempts in the candidate and roughly 250 KiB more live payload near the final
layout. Guest tracing recorded 124,011 versus 124,047 allocation-family calls,
5,502,936 versus 5,545,500 region-search steps, and 283,754 versus 285,975
first-fit steps; both ended the final layout at 64 regions. Largest-free-block
rescans were fewer in the candidate (4,777,791 versus 5,370,011). Those results
do not support an increased search count as the explanation for the P5 gap.
Instrumented timings are not used for performance claims. Probe sources were
restored and probe binaries remain separate from deliveries.

The full-cascade path nevertheless allocated and freed a temporary sorted
list of applied declarations for each styled element. The follow-up shares
that scratch buffer across elements within a build or update. It grows to the
largest declaration list encountered and is released at the end of that pass,
including an update failure. Published winners and their ownership are
unchanged; the retained cascade keeps no scratch buffer between mutations.
This removes avoidable work from the fallback path without claiming to explain
the entire measured regression.

Validation:

- Full libgarb suite passes. Final focused update suite: 1,727 checks, zero
  failures. A 256-element build uses 13 allocation calls and a 100-element
  update uses seven; the regression bounds reject per-element temporary
  allocation. Mixed declaration counts exercise buffer growth and compare
  winners with a fresh cascade. Each of the seven batch-update allocation
  positions is failed independently; refused updates require replacement and
  release their temporary storage. Existing inline/growth sweeps cover eight
  and four allocation positions.
- Full Yonder scripted-page suite: 12,566 checks, zero failures.
- Focused libflow restyle suite: 25,463 checks, including 164 allocation
  positions failed individually and persistently; zero failures.
- Original and companion measurement fixtures pass at 200/3 and 2,000/20:
  four runs of 16 checks. Captured article: nine checks, zero failures.
- ASan, UBSan and LeakSanitizer remain enabled. Target cross-build passes.

The uninstrumented QEMU article total is 5,526.100 ms, median 277.050 ms,
worst 509.100 ms. It uses the same 828 × 509 light/100% workload as the prior
5,774.300 ms candidate and 5,692.700 ms control. That is 4.3% below the prior
candidate and 2.9% below control in individual emulator runs. Native counts
remain 17 model rebuilds, 18 stream layouts, 7,825 elements, 5,209 boxes and
3,043 measures, with no refusals/clock errors. Stream cascade/layout totals
are 4,676.346/522.023 ms. This is directional evidence, not P5 acceptance.

The original 2,000/20 target page also completes with cached/paragraph/list
medians 0.350/450.550/636.050 ms and maxima 0.500/783.300/673.700 ms.
It records 52 model rebuilds, 53 layouts, 26 cascade updates and 53,134 copied
styles, with zero refusals/clock errors. Its layout total is 23,846.768 ms.
The hardware comparison still needs the original page and the article at the
matched viewport. Only libgarb changes in the replacement cohort.

A subsequent isolated comparison put the previous `8406bc21` libgarb back
into the same guest, with the same original fixture and a fresh browser.
It reported 0.400/429.750/619.750 ms medians and 0.700/728.900/694.300 ms
maxima. The scratch candidate's paragraph/list medians are 4.8%/2.6% higher
in this pair, despite fewer allocations. This prevents claiming a general
speedup from the article result. Hardware validation remains necessary;
the scratch change is not evidence that either P5 regression is resolved.

The same heap-buffer candidate repeated the original page at medians
0.350/448.550/593.050 ms and maxima 0.400/769.000/645.300 ms. This run
showed list timing variation, while paragraph timing remained above the
previous-library comparison. The final refinement below avoids allocating
heap scratch for short declaration lists.

## Stack-backed declaration scratch

The final implementation keeps up to 32 temporary declarations on the stack;
larger lists share a growable heap buffer for that build/update. Heap storage
is released at pass end or failed update, and no scratch pointers escape into
published styles. This retains the per-pass allocation reduction while avoiding
heap scratch for common small edits. Both full and incremental callers use the
same storage contract.

Final validation remains leak-enabled (ASan/UBSan/LSan): full libgarb suite
passes, with 1,719 focused update checks; full Yonder suite passes 12,562 checks;
libflow restyle passes 25,463 checks and its 164-position single/persistent
failure sweep. The expanded scratch fixture uses heterogeneous rule lists that
cross the local-buffer threshold and grow the heap buffer. Its 256-element
build uses 16 allocation calls and its 100-element update uses five. Failing
each of the five update allocation positions preserves refusal/cleanup
contracts; winners match a fresh cascade on success. Existing inline-style and
slot-growth sweeps now cover seven and three positions. Lower suite totals
reflect fewer allocation-failure positions, not disabled cases.

Both measurement pages complete their quick/full host runs (16 checks each),
and the captured article passes nine checks. The target build passes. The
replacement cohort changes only libgarb relative to `8406bc21`; P5 acceptance
is still pending.

The final uninstrumented article run reports total 5,545.400 ms, median
279.800 ms and maximum 496.000 ms: 4.0% less total time than the installed
candidate and 2.6% less than the 5a control in these emulator
runs. Work counts match the prior QEMU article. Cascade/layout phase totals
are 4,692.115/523.244 ms, with zero refusals or clock errors.

The final original-page run reports cached/paragraph/list medians
0.400/443.050/615.600 ms and maxima 0.500/666.600/653.800 ms. Against the
isolated prior-library run, paragraph median is 3.1% higher and list median
0.7% lower; both maxima are lower. The repeated intermediate runs demonstrate
variation, so this does not establish a general timing win. The intended
allocation reduction is verified, and the article result is encouraging, but
the P5 regressions remain unresolved until remeasurement. Preserve the old
cohorts; use the replacement only for the candidate's original-page and
matched-viewport article follow-up.

## P5 delivery follow-up: missing native profile facts

The screenshots ending in `delivery` show completed runs of the r3 delivery
(`483df1bb`). Corpus 555 is the original page; corpus 557 is the latest,
maximized/reloaded article. Intermediate article record 556 is excluded.
Screenshots, both reports and a numerical summary are retained with hashes
outside `/tmp`.

Both screenshots report 2,526 × 1,259 CSS pixels and device ratio 1. The original
uses 2,000 rows, 20 samples and five warm-ups. Native records confirm dark
appearance, 100% zoom, Scripts and Modern Web on. The original report's
`laid out at: 826x459` describes arrival before maximization; its measurement
JSON records the larger viewport used for the completed experiment.

| Original workload | Profiled control median | Delivery median | Delivery worst |
|---|---:|---:|---:|
| Unchanged geometry | 0.100 ms | 0.100 ms | 0.200 ms |
| Paragraph change | 65.400 ms | 37.000 ms | 159.200 ms |
| List replacement | 61.000 ms | 57.900 ms | 67.600 ms |

The observed paragraph/list medians are 43.4%/5.1% below control. The maxima
remain above control's 100.300/66.200 ms. The article completes 18 checkpoints
with heights matching control, but reports 483.000 ms total, 24.050 ms median
and 48.300 ms maximum. Its total is 18.6% above control's 407.200 ms and 3.0%
above the preceding candidate's 469.100 ms. The article regression has not
been resolved by the scratch change.

**Neither new native report contains `layout:` or `layout stream:` facts.**
This is consistent with layout profiling being disabled in these launches.
Pressing `d` saves the current diagnostic; it does not enable profiling.
The previous controls/candidates were profiled, so these displayed times must
not be treated as a fully matched profiled comparison. Phase timings, refusal
counts, clock-error counts and reuse counts are unavailable for these runs;
their absence is not evidence that those counts were zero.

Acceptance remains open. Retain these observations, but obtain native phase
information with `yonder --layout-profile` before attributing the remaining
article cost. Use the article after maximization and reload, and the original
page's Run measurements button after maximization. No new binary or delivery
change was made while recording this evidence.

## Profiled delivery rerun: original confirmed, article viewport unmatched

Only the last two new records are used, as requested: corpus **560** is the
original page, **559** the article. The screenshots ending in `delivery2`
are retained with both records and a hashed numerical summary. Earlier extra
runs from this rerun session were not retrieved or used.

The original completes 2,000 rows / 20 samples / five warm-ups at 2,526 × 1,259,
device ratio 1. Cached/paragraph/list medians are **0.100/40.200/58.050 ms**;
maxima are **0.100/147.000/63.800 ms**. Compared with profiled control corpus
458, paragraph and list medians are 38.5% and 4.8% lower. Paragraph maximum
remains above control's 100.300 ms; list maximum is below its 66.200 ms.
This confirms the original-page median improvement in a profiled run, without
establishing better paragraph tail latency or estimating between-run variance.

The native profile is present: 52 model rebuilds, 54 layouts, 106,499 elements,
105,851 boxes, 105,374 measures, 26 cascade updates and 53,134 copied styles;
zero refused/partial results and clock errors. Phase totals are model
12.674 ms, cascade 184.630 ms, styles 105.727 ms, boxes 24.283 ms, layout
2,953.100 ms and public tree 61.680 ms.

The article also has profiling enabled and completes 18 checkpoints, but its
measurement JSON records **826 × 459**, not the control's 2,526 × 1,259.
Its total/median/maximum are 408.400/20.250/47.400 ms. Native work counts are
7,825 elements, 5,209 boxes and 3,043 measures across 17 model rebuilds and
18 stream layouts, with zero refusals/clock errors and zero style/cascade
reuse, as expected. These smaller-viewport counts and screenshot heights
confirm that this is not a valid comparison to control corpus 461.

Retain the confirmed original-page result; it does not need another run for
this checkpoint. The article still needs a profiled measurement at the control
viewport. To avoid its automatic small-window run, open the starter with
`yonder --layout-profile /tests/pages/layout-stream.html`, maximize, and type
`file:///tests/pages/layout-stream-article.html` directly in the address bar.
Save the completed report only after checking its recorded viewport. Delivery
files remain unchanged and the 5b performance gate remains open.

## Correctly sized profiled r3 article: corpus 562

Only the newest record, corpus 562, is used for this rerun, matched to the
screenshot ending in `stream-delivery3`. It completes 18 checkpoints at
2,526 × 1,259 CSS pixels, device ratio 1, with heights matching control corpus
461. Native settings confirm dark appearance, 100% zoom, Scripts and Modern Web
on. Profiling is present. This is a valid geometry/work-count comparison.

Total geometry time is **485.800 ms**, median checkpoint 23.700 ms and maximum
61.300 ms, versus control's 407.200 / 20.350 / 40.600 ms. Total time is
78.600 ms (19.3%) higher. These individual runs establish the outstanding
regression signal, not a statistical estimate of variability or its cause.
The scratch change has not cleared the article performance gate.

| Stream phase | Control 461 | R3 candidate 562 |
|---|---:|---:|
| Model rebuild | 2.869 ms | 3.016 ms |
| Cascade | 283.844 ms | 360.303 ms |
| Computed styles | 6.048 ms | 6.436 ms |
| Boxes | 1.425 ms | 1.454 ms |
| Layout | 47.439 ms | 110.038 ms |
| Public tree | 3.270 ms | 3.143 ms |

Both records have 17 model rebuilds, 18 stream layouts, 7,927 elements,
5,294 boxes, 3,795 measures and zero refusals/clock errors. The candidate's
zero cascade updates and copied styles are expected for the broad selectors.
Its displayed-layout record is separate: 941 elements, 512 boxes, 413 measures,
one layout, no errors; cascade 30.278 ms and layout 17.036 ms.

The cascade phase boundary still differs from 5a because early release of the
hidden measurement moved into that phase. Do not attribute its full increase
to selector computation. The unchanged layout algorithm's phase also costs
more despite matching work counts, so allocation/lifetime effects and timing
variation remain investigation targets, not established causes. Relative to
the preceding profiled candidate corpus 466, cascade time is lower
(360.303 versus 369.149 ms) but layout time is higher (110.038 versus 83.720 ms).

The hardware evidence collection for this candidate is now usable: retain the
confirmed original-page result from corpus 560 and this article result. No
further viewport/profiling correction is needed. Investigation remains before
5b performance acceptance, review or 5c. Delivery files are unchanged.

## Automated P5 follow-up: repeated measurements, unchanged implementation

The follow-up ran control and candidate on the same P5, with automatic
checkpoint collection and exit. Production source was unchanged. Temporary
diagnostic copies used private dependencies so the installed browser and
libraries could remain intact. All 22 installed files were read back and
SHA-256 checked against the frozen r3 delivery: every file matched.

First, temporary text counters isolated work inside layout. At the P5's
2,526 × 1,259 viewport, both builds made 9,741 text-layout calls over 141,819
bytes, with 140,733 glyph-cache hits, 361 misses, 26 font opens and no glyph
evictions during the 18 measurements. QEMU and the P5 agreed on these work
counts. Four successful P5 runs per build had overlapping layout-phase
times: control 55.708–69.650 ms and candidate 56.564–68.495 ms. These probes
add timing overhead and are diagnostic evidence, not acceptance timings.

The extra text counters and clocks were then removed. The remaining harness
uses the existing layout profiler, reads the page's completed JSON, and exits
after the final displayed layout. Output occurs after measurement. Three
control/candidate runs were collected for each entry route, alternating build
order. The fixture, dark appearance, zoom, scripts and final viewport match
the earlier comparison.

| Entry route and build | Total geometry time, three runs | Median total |
|---|---|---:|
| Direct article, control | 406.5 / 424.2 / 413.5 ms | 413.5 ms |
| Direct article, r3 candidate | 411.8 / 415.3 / 418.4 ms | 415.3 ms |
| Starter, resize, address navigation, control | 420.4 / 424.0 / 421.7 ms | 421.7 ms |
| Starter, resize, address navigation, r3 candidate | 406.7 / 414.9 / 422.1 ms | 414.9 ms |
| Same starter route, installed r3 libraries | 409.2 / 420.1 / 417.9 ms | 417.9 ms |

The starter route opens the original starter page at the normal initial
window size, requests larger window content through the window API, waits
for layout at 2,526 × 1,259, then calls the browser's typed-address navigation
path. It does not simulate typing or the window manager's maximize gesture.
The isolated dependencies are relinked as floating libraries, changing their
virtual placement. The last row checks that limitation with a separate
automatic runner linked to the actual installed r3 libraries; it does not
replace those libraries or the installed browser. No other diagnostic I/O was
started until the timed repetition batch had completed.

All 15 runs complete 18 checkpoints with identical heights, 7,927 styled
elements, 5,294 boxes, 3,795 measurements, 17 model rebuilds and zero
refused/partial results or clock errors. The expected full-cascade fallback
still has zero cascade updates and copied styles. Direct-entry candidate
median total is 0.4% above control; the starter-route candidate median is
1.6% below control. The installed-library median is 0.9% below the
starter-route control median. These small differences do not establish an
article speedup.

The installed-library layout phase is 50.009 / 58.563 / 52.717 ms, versus
110.038 ms in corpus 562. Its cascade phase is 344.018 / 345.926 / 349.807 ms.
As above, the cascade total includes hidden-measurement cleanup that the
control charges outside that phase; total geometry time is the comparable
end-to-end measurement.

**Conclusion:** the controlled repetitions do not reproduce a material
article regression and support presenting 5b for review alongside the
confirmed mutation improvements. They do not explain or invalidate corpus
562's 485.8 ms, prove better paragraph tail latency, or establish that a
production fix occurred during this investigation. Retain those limitations
in review. There is no new candidate delivery to install and no additional
manual viewport/profiling correction owed. 5c has not started.

The probe sources, link plans, captured output, numerical summaries and hashes
are retained in the local proof directory. An immediately repeated diagnostic
launch was refused once; a later retry and repetitions separated by a short
cleanup interval completed. That refused launch produced no timing sample
and is excluded. All temporary P5 diagnostic directories were removed after
the processes exited. Ordinary local binaries were rebuilt from restored
source, and the frozen r3 manifest still verifies in full.


## Fable's remeasure review: review allowed, manual merge gate open

The architecture review at `bc2bb84b` accepts the dependency flags, persistent
slot table, dirty-majority fallback and declaration scratch. Its ruling permits
code review now but requires a further alternating manual article comparison
before merge. 5c has not started.

**Update accounting.** Removing the first list row changes the positions tested
by `:nth-child(even)`; invalidation marks the surviving children and crosses
the dirty-majority threshold, so each of the 25 list operations (five warm-ups
plus 20 samples) falls back to a full cascade. The 25 paragraph operations can
update locally. The reported 26 updates are consistent with those paragraph
operations plus the completion/report update. The 54 layouts also include the
list fallbacks, bulk setup, initial display and maximization; unchanged geometry
reads add no layout. This is accounting from the fixture and invalidation code,
not a captured per-update trace. The non-positional companion's 51 updates are
consistent with both sets of 25 mutations plus the completion update.

**Tail latency remains unexplained.** Original-page paragraph maxima of
159.1, 159.2 and 147.0 ms exceed the matched control's 100.3 ms. The aggregate
reports do not identify which sample renewed a cascade or what consumed its
time. Neither renewal-by-bytes nor allocator traversal is an established
explanation. Lower medians do not establish better tail latency.

**Manual protocol correction.** The tester confirms that corpus 562 used a
title-bar double-click to maximize, not edge dragging through intermediate
sizes. That removes the intermediate-drag premise from the retention
hypothesis. The automated route still did not reproduce the actual maximize
gesture or keyboard input, so the manual comparison remains required. Of the
three earlier slow candidate article runs, corpus 466 and 562 were profiled;
557 was not. All three have displayed geometry timings, but only the profiled
runs provide native layout-phase evidence.

**Navigation lifetime checked.** `page_departs` retires the script runtime;
it does not clear the displayed page's layout or cascade. `page_show` calls
`page_clear` after the replacement has a usable layout, and that call frees the
old flow tree and cascade. This preserves the old page when navigation fails.
The hidden measurement cache has separate cleanup through `measured_forget`.
Keeping the displayed page alive across navigation is intentional, and its
lifetime alone does not establish an allocator regression or justify freeing
its cascade early. No production change was made on this hypothesis.

**Remaining acceptance session.** Use the frozen control `9dca4a7d` and r3
candidate `483df1bb`, alternating control/candidate three times. Each run starts
a fresh installed Yonder with `--layout-profile /tests/pages/layout-stream.html`.
Double-click its title bar to maximize, wait for the starter to settle, then
type `file:///tests/pages/layout-stream-article.html` in the address bar. Keep
Scripts and Modern Web enabled, dark appearance and 100% zoom. Do not resize
after navigating. Verify completion, 18 sample/height entries and the same
2,526 × 1,259 viewport; save the screen and native record with `d` for each run.

Compare all six totals and native work counts. If candidate timings sit within
the control spread, the article gate passes and the earlier slowdown stays in
the evidence as an unresolved anomaly. If the roughly 15% slowdown recurs,
capture both builds' `/proc/<yonder>/heap` immediately before typing the article
address on a follow-up pair. Compare live regions and allocator state before
choosing a fix; more retained regions would support the hypothesis, not by
itself prove that a particular allocator or lifetime change cures it. Merge
and 5c remain on hold until this gate is resolved.


## Alternating manual P5 session: performance gate cleared

The tester installed the frozen control `9dca4a7d` and candidate `483df1bb`
in alternating order, three runs each, starting a fresh Yonder on the starter,
double-clicking the title bar to maximize, then entering the article address.
Corpus 595–606 records the six starter/article pairs; screenshots retain each
completed sample list. All six captured article files have the frozen fixture's
SHA-256 `b56f866156c264b6b085c83535637d32022783202a8f283f1e35c484500700fc`.

| Pair | Control corpus | Control total | Candidate corpus | Candidate total | Paired change |
|---|---:|---:|---:|---:|---:|
| 1 | 596 | 418.8 ms | 598 | 410.7 ms | -1.9% |
| 2 | 600 | 421.4 ms | 602 | 407.0 ms | -3.4% |
| 3 | 604 | 436.1 ms | 606 | 415.5 ms | -4.7% |

The median of run totals is 421.4 ms for control and 410.7 ms for candidate,
2.5% lower. All three candidate totals are below the control range of
418.8–436.1 ms, satisfying the gate's no-regression purpose. Three repetitions
support acceptance of this workload, not a general article-speedup claim.

Every run completed 18 checkpoints at 2,526 × 1,259, device ratio 1, 100% zoom,
dark appearance, Scripts and Modern Web enabled. The 18 measured heights
match across all screenshots. Native stream reports agree on 7,927 styled
elements, 5,294 boxes, 3,795 measurements, 17 model rebuilds and 18 layouts,
with zero refused/partial results or clock errors. Candidate cascade updates
and copied styles are zero, as expected for this full-fallback workload.
Native layout-phase totals are 55.132 / 48.680 / 55.252 ms for control and
47.913 / 45.910 / 54.537 ms for candidate. The earlier doubled layout cost
did not recur.

The tester varied the delay between completion and pressing `d`. This does
not invalidate the comparison: the fixture computes its geometry samples and
report during parsing, before completion. `d` saves diagnostics and a paint
reading; it does not repeat the geometry measurements. Each record retains
evidence of that request (a numbered paint reading or `written: when asked`).
The first five were subsequently written again at page departure. Idle/paint
intervals differ and are not used as geometry timings.

**Acceptance:** the manual article requirement is satisfied. Combined with
the original and companion results, this clears the 5b performance hold;
code review and merge remain separate steps. The earlier 15–19% manual
slowdown and the elevated paragraph maxima remain unresolved evidence, not
fixed defects or discarded samples. No production code or frozen binaries
changed for this session. The six screenshots, native records, captured
fixtures, checked sample transcriptions, analysis script and SHA-256 manifest
are retained outside temporary storage. 5c has not started.
