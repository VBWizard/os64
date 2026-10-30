# Relayout 5b validation

5b updates the cascade from native mutation history and copies unaffected
computed styles into a fresh tree. Box construction and layout still run
in full. The ownership, invalidation and bounded-renewal contracts are in
[RELAYOUT.md](../design/pending/RELAYOUT.md).

## Automated evidence

- Full libgarb suite passes, including 1,277 incremental-cascade checks and
  nine update allocation-failure positions. Updated winner dumps match
  independent full cascades after class/custom-property changes, text edits,
  moves, removals and repeated updates through renewal. Lost history and
  nested relational/direction selectors request full builds. Existing parser,
  selector, media, allocation and memory-budget tests remain enabled.
- Full libflow suite: 49,556 checks, zero failures. The new restyle tests
  contribute 25,463 checks, comparing layout dumps and computed-property
  serialization against independently rebuilt cascades. They free the source
  tree before reading copied style lists. Inheritance, sibling rules, moves,
  removals, root font/body-link changes, disclosures and display changes are
  covered. All 164 incremental-layout allocation positions are failed both
  individually and persistently; ownership returns to its baseline.
  The existing 12 corpus dumps match and 597 sampled fuzz trees pass.
- Yonder scripted-page host: 12,548 checks, zero failures. New cases check
  cascade identity/update counts, displayed and measured style reuse,
  repeated geometry reads, releasing the measured owner during publication,
  immediate geometry after inline stylesheet edits, and media invalidation.
  The duplicate stylesheet/import ownership sweep and existing animation,
  face-budget recovery, synchronous geometry and browser tests pass.
- The unchanged measurement page completes both 200 rows / 3 samples and
  2,000 rows / 20 samples through the browser host: 16 checks per run,
  zero failures, including cancellation and viewport-change rejection.
- Target Yonder and matching libraries cross-build. ASan, UBSan and
  LeakSanitizer remain enabled for host tests.

Reproduce with `tools/test_garb_host.sh`, `tools/test_libflow_host.sh`,
`tools/test_yonder_scripts_host.sh` and
`tools/test_layout_measure.sh <output-directory>`. Focused restyle runs are
`tools/test_garb_host.sh --updates`, `tools/test_libflow_host.sh --restyle`
and `YONDER_RESTYLE_ONLY=1 tools/test_yonder_scripts_host.sh`.

## Target and hardware evidence

The isolated QEMU guest completed the unchanged 2,000-row / 20-sample page.
Its saved diagnostic records 52 model rebuilds, 53 layout attempts,
50 cascade updates and 2,040 copied computed styles, with zero refused or
partial results and zero clock errors. Layout reuse remains zero, as expected
before 5c. These counters establish that the target executable uses the new
path; emulator timings do not establish a P5 performance improvement.

The frozen delivery cohort contains 23 files, including 18 binaries reused
byte-for-byte from the 5a delivery. It includes a SHA-256 manifest and the
unchanged measurement fixture. The local handoff records the exact source
commit and delivery paths.

The first P5 run completed, but performance acceptance remains pending:
it exposed the regression recorded below. The corrected P5 comparison is
recorded below as well; performance and review follow-up remain open. For comparison with [the 5a baseline](5a-validation.md), use the same settings:
Scripts and Modern Web on, dark appearance, 100% zoom, a fixed 2,526 × 1,259
CSS-pixel viewport, 2,000 rows and 20 samples. Reload before the run:

```
yonder --layout-profile /tests/pages/layout-measure.html
```

Choose **Run measurements**, wait for **Complete**, retain the table/JSON,
and press `d` with ordinary page content focused to save the corpus report.
The `layout:` fact now includes `styles reused` and `cascade updates`;
`reused` continues to count layout reuse and remains zero in this slice.

The fixture's `:nth-child(even)` rule makes the conservative restyle rule
revisit the row group after a row edit. Consequently it is a useful unchanged
comparison but not a best-case demonstration of isolated style reuse. No
speedup is assumed. The dominant layout phase from 5a is addressed by 5c.

## First P5 comparison: regression, not acceptance

The run at 2026-10-30 01:17 UTC (October 29 local time), corpus 299, used
the same 2,526 × 1,259 viewport, dark appearance, 100% zoom, 2,000 rows
and 20 samples as 5a. It completed with no refused/partial layouts or clock
errors. The counters show 50 cascade updates and 2,040 reused styles.

| Experiment | 5a median | 5b median | 5b worst |
|---|---:|---:|---:|
| Read unchanged geometry | 0.100 ms | 0.100 ms | 0.200 ms |
| Change one paragraph | 63.900 ms | 136.900 ms | 300.700 ms |
| Replace one list item | 55.300 ms | 138.450 ms | 308.100 ms |

Paragraph and list medians increased by 114.2% and 150.4%. The two runs
each record 52 model rebuilds and 54 layout attempts. Cumulative layout
time rose from 2,414.759 ms to 5,285.197 ms; cascade time rose from
432.828 ms to 733.041 ms, and computed style time from 83.864 ms to
139.946 ms. Low reuse on this selector-heavy fixture explains limited
benefit, but does not excuse this slowdown.

A read-only heap snapshot after the run records 112,718,384 live bytes,
367 regions (325 dedicated), and an intact heap audit. The 5b policy of
retaining old cascade allocations for up to 64 updates is a suspect: the
Os64 allocator walks its region ledger during allocation/free. That is a
hypothesis pending a controlled retention-limit experiment, not an
established attribution. The screenshot and original corpus/heap reports
are preserved with the local proof artifacts; raw individual sample values
have not been retrieved.

## Retention correction

An isolated one-variable QEMU experiment shortened the 64-update limit to
one update. On the same default workload, cumulative layout time fell from
38.340 to 28.857 seconds (the earlier 5a QEMU run recorded 25.412 seconds).
Paragraph/list medians fell from 960.550/922.200 ms to 774.900/787.900 ms.
This supports retention as a contributor; these emulator runs do not prove
the full cause or establish the corrected P5 performance.

The correction uses retained bytes rather than a one-update limit: renew
before the next update once added state payload and inline-attribute parse
storage reach one initial state's payload, with a 4 KiB floor. A single
update may cross that threshold; the existing hard allocation caps remain.
The independent 64-update guard remains as well.

New tests exercise 20 broad updates over 2,000 elements and require early
renewals, while 20 small edits over the same-sized cascade retain incremental
reuse. The full cascade suite passes (1,321 update checks); the focused
style/layout oracle passes 25,463 checks including 164 allocation positions;
the full browser suite passes 12,548 checks. Sanitizers and leak checks remain
enabled. The replacement cohort reuses 21 of its 22 binaries from the first
5b delivery; only libgarb changes. The P5 remeasurement is recorded below.

The corrected target then completed the same QEMU workload with 35 cascade
updates, 1,402 reused styles and no refusals/partial layouts or clock errors.
Its paragraph/list medians were 648.700/612.050 ms, with worst values
1,199.900/879.600 ms. Cumulative layout time was 26.484 seconds, versus
38.340 seconds for the original 5b target. These are encouraging functional
and comparative emulator results; the subsequent P5 run follows.

## Corrected P5 comparison: substantial recovery, remaining regression

Corpus 300 and the completed screenshot record the corrected build
`850ff5a5` at 2026-10-30 01:37 UTC (October 29 local time). The viewport,
appearance, zoom, row count and sample count match both prior runs.

| Experiment | 5a median | First 5b median | Corrected 5b median | Corrected worst |
|---|---:|---:|---:|---:|
| Read unchanged geometry | 0.100 ms | 0.100 ms | 0.100 ms | 0.200 ms |
| Change one paragraph | 63.900 ms | 136.900 ms | 64.750 ms | 162.300 ms |
| Replace one list item | 55.300 ms | 138.450 ms | 66.700 ms | 88.700 ms |

The correction reduced the paragraph and list medians by 52.7% and 51.8%
relative to the first 5b build. Relative to 5a, they remain 1.3% and 20.6%
higher. These are individual runs, not a statistical estimate of variance;
the results do not establish a speedup over 5a or close performance acceptance.

The native report records 52 model rebuilds, 54 layout attempts, 35 cascade
updates and 1,402 reused styles, with zero refused/partial results and zero
clock errors. Cumulative phase times are:

| Phase | Corrected 5b time |
|---|---:|
| Model rebuild | 19.122 ms |
| Cascade | 567.971 ms |
| Computed styles | 115.673 ms |
| Box construction | 24.262 ms |
| Layout | 3,056.432 ms |
| Public tree | 59.999 ms |

The screenshot and original corpus report are retained with the local proof
artifacts. Raw individual sample values have not been retrieved. The frozen
replacement bundle remains unchanged. The broad restyle rule, update
bookkeeping and design-review findings require follow-up before merge.

## Refinement after the design review

The follow-up narrows invalidation by mutation kind. A text edit under
`:nth-child(even)` restyles its row without changing the other rows' selector
matches. Child-list changes still account for positional selectors. `:empty`
propagates inherited/following effects, and removal batches conservatively
ignore historical sibling boundaries whose current links may have moved.
UA disclosure/legend behavior is also preserved.

The cascade keeps its node table and dirty-node buffer. Removed slots are
erased before the document pin rotates, connected additions create slots,
and broad dirty sets request a fresh cascade before winner computation.
Rule match stamps avoid resetting the rule array. Sibling-suffix marks and
per-parent removal stamps bound repeated bookkeeping within a parser batch.
The retained-byte renewal limit remains. Copied computed styles are steady
by construction, avoiding their redundant semantic comparison; a size
assertion prompts review of the deep-copy inventory when its structure grows.

Validation on this refinement:

- Full libgarb suite passes. The final update suite has 1,699 checks, zero
  failures; allocation sweeps cover nine inline-style and six table-growth
  positions. State changes, `:empty`, nested filtered selectors, batched
  moves, removal with a rescued descendant, remove/create identity safety,
  parser appends across table growth, and repeated deletion compare with
  fresh cascade dumps.
- Full libflow: 49,556 checks, zero failures. After the final sibling-walk
  refinement, focused restyle revalidation passes 25,463 checks, including
  the 164-position single/persistent allocation-failure sweep.
- Full Yonder scripts: 12,548 checks, zero failures. Final focused browser
  restyle revalidation: 19 checks, zero failures.
- The original and non-positional measurement fixtures both complete at
  200/3 and 2,000/20: four runs of 16 checks, zero failures. ASan, UBSan and
  LeakSanitizer remain enabled throughout the host validation.

The first QEMU run of the refinement completed the original 2,000/20 page:

| Median | Original 5a QEMU | Refined 5b QEMU |
|---|---:|---:|
| Change one paragraph | 715.250 ms | 457.200 ms |
| Replace one list item | 708.700 ms | 639.950 ms |

The unchanged-read control was 0.400 ms. Native counters record 52 model
rebuilds, 53 layout attempts, 26 cascade updates and 53,134 reused styles;
zero refused/partial results and clock errors. Cumulative phases were model
54.849 ms, cascade 1,795.003 ms, style 1,405.784 ms, boxes 302.711 ms,
layout 23,660.909 ms and public tree 661.138 ms. This run preceded the last
sibling-suffix bookkeeping refinement. The final candidate repeated the original workload at 0.400 ms cached,
463.150 ms paragraph and 622.900 ms list medians (worst 0.500, 622.300 and
663.900 ms respectively). These are about 35% and 12% below the original 5a
mutation medians. Single emulator runs, including the first run with host
suites running alongside it, are directional evidence only.

The final original-page report repeats 52 model rebuilds, 53 layouts,
26 updates and 53,134 copied styles, with zero refusals or clock errors.
Its cumulative cascade/style/layout times are 1,793.069 / 1,386.261 /
23,280.913 ms.

The non-positional companion also completes on target. In a fresh QEMU
browser process its cached/paragraph/list medians are 0.400 / 521.250 /
475.900 ms (worst 0.600 / 1,344.200 / 524.300). Its native report has 52
model rebuilds, 53 layouts, 51 cascade updates and 104,209 copied styles,
with zero refusals or clock errors. Cumulative cascade/style/layout times
are 85.788 / 1,835.535 / 27,744.346 ms. A preceding companion run after the
original benchmark in the same browser was noisier: paragraph/list medians
584.950 / 515.450 ms. These observations motivate fresh browser processes
for comparisons; they do not establish a cause for the variance. There is
no 5a companion baseline yet, so neither run establishes its speedup.

Generate the non-positional companion with:

```
python3 tools/layout_measure_local.py /path/to/pages/layout-measure-local.html
```

It preserves the measurement DOM and script, replacing just the striped
positional row rule with a uniform background and identifying its report as
`yonder-layout-local-v1`. `tools/test_layout_measure.sh` generates and runs
both pages. The frozen handoff includes both fixtures and matching libraries.

**P5 acceptance remains open.** The original-page remeasurement is recorded
below; compare the companion under both cohorts. The streamed
Wikipedia article comparison requested by the design review also remains
open: retain its input and settings and compare the native `cascade` phase
with the stream-layout count. A final-load screenshot alone does not prove
streaming reuse. These gates precede the 5b PR and work on 5c.


## P5 result after targeted invalidation

The refined cohort (`40d52cff`) was run twice, with Yonder closed between
runs. The tester reports similar displayed results. The first run's native
report was saved on page departure (corpus 343); the second was saved on
request (corpus 344) and has the retained screenshot. Settings match the 5a
baseline: 2,000 rows, 20 samples per workload, five warm-ups, 2,526 × 1,259
CSS-pixel viewport, device ratio 1, dark appearance, Scripts and Modern Web
enabled, and 100% zoom.

| Workload | 5a median | Previous correction median | Refined median | Refined worst |
|---|---:|---:|---:|---:|
| Read unchanged geometry | 0.100 ms | 0.100 ms | 0.100 ms | 0.200 ms |
| Change one paragraph | 63.900 ms | 64.750 ms | 36.300 ms | 158.800 ms |
| Replace one list item | 55.300 ms | 66.700 ms | 55.650 ms | 61.900 ms |

Paragraph median time is 43.2% below 5a. List replacement is 0.350 ms
(0.63%) above 5a, substantially recovering the prior 20.6% regression but
not demonstrating a list speedup or statistical equivalence. The paragraph
worst remains above 5a's 109.600 ms; the list worst is also above 5a's
56.900 ms. Faster typical paragraph edits do not establish better tail
latency.

Both native reports are clean, with 52 model rebuilds, 54 layout attempts,
26 cascade updates, 53,134 reused computed styles, zero refused/partial
layouts and zero clock errors. Their cumulative phase times are:

| Phase | First run | Second run |
|---|---:|---:|
| Model rebuild | 13.762 ms | 18.501 ms |
| Cascade | 215.946 ms | 213.032 ms |
| Computed styles | 104.032 ms | 95.646 ms |
| Box construction | 24.107 ms | 19.752 ms |
| Layout | 2,895.157 ms | 2,824.165 ms |
| Public tree | 61.462 ms | 55.336 ms |

The second run's cumulative cascade time is 50.8% below the 5a report.
Its layout total remains above 5a's 2,414.759 ms. These totals include setup,
warm-ups and presentation work as well as the measured operations; they
are not medians of individual samples.

The screenshot and both original reports are retained with the private proof
artifacts. First-run displayed medians and individual raw samples were not
retrieved. The frozen delivery is unchanged. The original-page result is
encouraging, while the companion and streamed-Wikipedia comparisons and the
performance acceptance decision remain open before review or 5c.

## Matched acceptance workloads

[5b-comparison.md](5b-comparison.md) records the separate stream profiling,
full-fallback retention corrections, matched 5a control, and the remaining
hardware protocol. Its automated P5 follow-up records repeated article totals
comparable to control, including a check against the actual installed r3
libraries. Together with the confirmed mutation improvements, that evidence
supports presenting 5b for review. The earlier article slowdown and elevated
paragraph maximum remain disclosed and unexplained; no production fix was
made during the follow-up. Fable permits code review now; merge remains on
hold for three manual control/candidate article pairs using title-bar
double-click maximization and actual address-bar input. The final section of
the comparison records the protocol, update accounting, companion revision
and navigation-lifetime check. 5c has not started.
