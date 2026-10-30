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
it exposed the regression recorded below. Fable's restyle design review
also remains pending. For comparison with [the 5a baseline](5a-validation.md), use the same settings:
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
5b delivery; only libgarb changes. P5 remeasurement remains required.

The corrected target then completed the same QEMU workload with 35 cascade
updates, 1,402 reused styles and no refusals/partial layouts or clock errors.
Its paragraph/list medians were 648.700/612.050 ms, with worst values
1,199.900/879.600 ms. Cumulative layout time was 26.484 seconds, versus
38.340 seconds for the original 5b target. These are encouraging functional
and comparative emulator results; the replacement still needs P5 validation.
