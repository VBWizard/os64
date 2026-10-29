# Relayout 5a validation

5a adds a bounded native mutation history and opt-in layout phase measurements.
It does not implement incremental restyle, layout reuse or frame callbacks.
The design and exact ownership/profiling contracts are in
[RELAYOUT.md](../design/pending/RELAYOUT.md) and the public headers.

## Automated evidence

- Native HTML DOM: 798,676 checks, zero failures. New cases cover independent
  readers, copied attribute names, no-op listener notifications without log
  entries, custom state, immutable fragment membership after moves, detached
  subtree lifetime, eviction within an atomic version, zero retention, allocation failure with listener
  delivery preserved, recovery and streaming parser records sharing a version.
  Existing hierarchy, allocation and random mutation checks remain enabled.
- libflow: 24,093 checks, zero failures. The corpus's 12 layout dumps match;
  597 sampled fuzz trees pass. Relation/allocation sweeps retain their checks
  on refused and partially completed layouts.
- Yonder script host: 12,534 checks, zero failures. Injected clocks verify the
  four flow phases, unchanged layout dumps, no clock reads without a sink,
  invalid clock reporting, cleared output on refused input, and separate model
  and cascade timing in the owning page diagnostic.
- The actual measurement page runs through Yonder's host at both 200 rows /
  3 samples and the default 2,000 rows / 20 samples: 16 checks per run, zero
  failures. Each covers completion of all three workloads, cancellation,
  viewport-change rejection and teardown. Host clocks are synthetic; their
  values are correctness evidence, not performance results.
- Canvas bindings: 129 checks per engine profile; custom elements: 241 per
  profile, all passing. The baseline integration merge additionally passed
  the full DOM suite (29,819 target-engine and 162,562 sanitized-engine checks)
  before the 5a changes. Canvas and custom elements share libdom's mutation
  dispatcher instead of replacing each other's document listener.
- Target Yonder and matching libraries cross-build with warnings as errors.
  Sanitizer runs retain ASan, UBSan and LeakSanitizer.

Reproduce with `tools/test_html_dom_host.sh`, `tools/test_libflow_host.sh`,
`tools/test_yonder_scripts_host.sh`, `tools/test_layout_measure.sh <output>`,
`tools/test_dom_host.sh --canvas`, and `tools/test_dom_host.sh --custom`.
Keep leak detection enabled. The measurement-page harness uses the production
script and changes only its select defaults for the quick run.

## Target smoke check and hardware baseline

The isolated QEMU guest loads the target cohort and completes both the
200-row / 3-sample smoke check and the default 2,000-row / 20-sample run.
The results table and JSON distinguish cached geometry reads, paragraph
changes and list replacement. The saved corpus report contains all six
phase totals, 52 model rebuilds and 53 layout attempts, with zero refusals
or clock errors. This establishes functionality; emulator timings are not
the P5 baseline.

Hardware acceptance passed on 2026-10-29. The P5 completed the default
workload, and corpus entry 229 records `written: when asked`, with zero
refused/partial results and zero clock errors. The screenshot confirms the
completed table and its viewport: 2,526 × 1,259 CSS pixels, device pixel
ratio 1, 100% zoom, dark appearance, Scripts and Modern Web enabled.

| Experiment | Median | Worst | Samples |
|---|---:|---:|---:|
| Read unchanged geometry | 0.100 ms | 0.200 ms | 20 |
| Change one paragraph | 63.900 ms | 109.600 ms | 20 |
| Replace one list item | 55.300 ms | 56.900 ms | 20 |

The native totals span 52 model rebuilds and 54 layout attempts:

| Phase | Cumulative time | Share of the six measured phases |
|---|---:|---:|
| Model rebuild | 21.279 ms | 0.70% |
| Cascade construction | 432.828 ms | 14.28% |
| Computed styles | 83.864 ms | 2.77% |
| Box construction | 19.925 ms | 0.66% |
| Layout | 2,414.759 ms | 79.65% |
| Public tree | 59.009 ms | 1.95% |

This run supports the planned cascade/restyle and layout-reuse work. Model
rebuild and public-tree time are each below the design's 10% investigation
trigger on this fixture; real-page measurements can differ. The table was
transcribed from the screenshot, and native totals were retrieved from the
corpus report. Individual raw sample values have not been retrieved.
The report's initial-load dimensions precede the enlarged measurement
viewport; future comparisons should use the viewport recorded by the page.

To reproduce, install the frozen cohort, enable Scripts and Modern Web,
and launch:

```
yonder --layout-profile /tests/pages/layout-measure.html
```

Use 100% zoom and a fixed window size. Leave the defaults at 2,000 rows and
20 samples; choose **Run measurements** and wait for **Complete**. Record the
results table or copy its JSON report, which includes raw samples and the
viewport. After clicking ordinary page text, press `d` to save the page's
corpus report (an existing writable `diagnostics` directory must be configured
in `yonder.conf`). Keep the corpus entry together with the table/JSON.

The native `layout:` line is cumulative for that page diagnostic, including
setup, warm-ups and presentation work. Reload before a comparable repeat;
keep settings and dimensions fixed. It reports model rebuild, cascade, style,
box construction, layout and public-tree time, work counts, refusals and clock
errors. A failed run or a nonzero clock-error count is not a timing baseline.
The table's sample intervals exclude timer waits and include mutation plus
any geometry-forced layout. They do not measure total frame or paint time.

Future 5b/5c comparisons use the same fixture and settings. Real-page checks
on danlegt, Wikipedia and the Fetch specification remain part of the broader
relayout acceptance; this synthetic baseline does not establish those costs.
