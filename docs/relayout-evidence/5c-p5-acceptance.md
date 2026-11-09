# 5c hardware acceptance

**Status: passed on 2026-10-31 after the final three-pair comparison below.**
The earlier measurements are retained to show the regressions and follow-ups;
they are not pooled with the final campaign.

## Initial comparison

The frozen 5b control and reviewed 5c candidate completed three alternating
pairs for original mutation, localized mutation, streamed article and the
synthetic float/append fixture. Mutation fixtures used 2,000 rows and 20
samples after five warm-ups. Timed viewport was 2526 by 1259 at 100% zoom,
dark appearance, scripts and Modern Web enabled. The article used a fresh
starter window, maximization, then address-bar navigation without a reload.
An initial small-window article run and its reload were excluded and replaced
with the prescribed sequence before pairing.

Raw console JSON and native corpus diagnostics were preserved. Each report
matched its partner's workload settings; all 18 article checkpoint heights
matched. Native counters reported no partial layouts or clock errors.
Numbers below are medians of three run medians or totals, not pooled samples.

| Operation | Control ms | Candidate ms |
| --- | ---: | ---: |
| Original paragraph edit | 39.45 | 10.75 |
| Original list replacement | 58.05 | 60.80 |
| Local paragraph edit | 41.10 | 10.90 |
| Local list replacement | 38.55 | 9.85 |
| Paragraph around float | 56.90 | 10.10 |
| Append one row | 53.35 | 10.35 |
| Article checkpoint | 20.90 | 21.35 |
| Article total | 412.10 | 444.60 |

The positive reuse cases improved substantially. The original replacement
was 4.74% slower in this aggregation. Article total increased in each pair:
6.67%, 7.29% and 12.25%. These initial results left the fallback/no-regression acceptance gate
open; the final campaign below closes it after the follow-up changes.

Native article stream counts matched: 18 layout attempts, 7,927 elements,
5,294 boxes and 3,795 measurements, with zero geometry reuse. Full cascade
replacement is expected for its broad selectors. Median native stage totals
were 348,046 versus 362,610 microseconds for cascade work and 49,327 versus
64,957 microseconds for layout work. These are whole-stream stage totals,
not measurements of one checkpoint or proof of a single cause.

## Memory and initial layout

A read-only observer polled test stdout during both cohorts and captured
the process heap after a complete report. Each accepted snapshot matched the
report and passed the allocator's consistency audit. The metric is the
process lifetime maximum live allocator payload through that snapshot,
including startup and maximization. It is neither RSS nor isolated libflow
metadata. Polling overhead is present in both cohorts.

| Workload | Control peak bytes | Candidate peak bytes |
| --- | ---: | ---: |
| Original mutation, median of three | 53,755,376 | 54,707,024 |
| Local mutation, median of three | 50,651,488 | 43,337,296 |
| Article, median of three | 59,999,216 | 60,448,128 |
| Graft, supplemental memory pair | 56,042,592 | 57,002,640 |

Native first displayed-layout times were 10/10 ms for original mutation
(control/candidate medians), 10/10 ms for local mutation, and 40/40 ms for
the article. Mutation windows initially measured 826 by 459; the article
was first displayed at 2526 by 1259. The supplemental graft memory pair
recorded 60/70 ms at 826 by 459. The native timer's approximately 10 ms
granularity limits these comparisons; zero readings do not mean free work.
These headers do not measure application launch or replace raw first opt-in
layout timing for a large populated fixture.

The graft report also records `initial_layout_ms`: the forced geometry read
after replacing all 2,000 rows in an already loaded gallery, excluding the
DOM creation itself. This is a setup/rebuild measurement, not a cold process
start. Its three control values were 123.80, 123.30 and 122.50 ms; candidate
values were 171.00, 159.90 and 158.80 ms. The medians are 123.30 and 159.90 ms,
a 29.68% increase. The supplemental memory pair recorded 125.60/171.60 ms.
This distinct regression required follow-up alongside the fallback cases;
the coarse initial displayed-layout headers could not clear it.

## Fallback follow-up

The initial candidate recorded a new geometry snapshot even after releasing
an unusable hidden source during full-cascade fallback. The article then
discarded that snapshot on the next generation. An isolated host probe kept
the same parser and layout code while disabling geometry inputs. Across five
alternating pairs under ASan/UBSan with leak detection, the median elapsed
time inside 19 flow calls fell from 235,773 to 196,174 microseconds; summed
snapshot allocation sizes fell from 1,585,880 bytes to zero. That sum is not
peak memory. Host timing varied and uses a bitmap font; it does not establish
the hardware improvement or explain the entire native cascade difference.

The follow-up lets Yonder avoid speculative snapshots when the cascade does
not track DOM changes. Existing cascade-generation and geometry safety checks
remain in force. Frozen comparison bundles are preserved unchanged.

One subsequent hardware confirmation used the same settings and article load
sequence, replacing Yonder and libgarb in a separate frozen cohort. Both
completed reports matched their heap captures and contained no partial
layouts or clock errors. All 18 article checkpoint heights matched the
initial comparison. Results for this single follow-up, alongside the prior
three-run control and candidate aggregates:

| Operation | Control aggregate ms | Initial candidate aggregate ms | Follow-up ms |
| --- | ---: | ---: | ---: |
| Original paragraph edit | 39.45 | 10.75 | 10.45 |
| Original list replacement | 58.05 | 60.80 | 59.45 |
| Article checkpoint | 20.90 | 21.35 | 21.10 |
| Article total | 412.10 | 444.60 | 415.10 |

Article snapshot metadata was zero in both the stream and displayed phases.
Stream work counts were unchanged; native cascade/layout totals were 347,911
and 51,390 microseconds. Process allocator peak live payload was 60,068,912
bytes, compared with the control median of 59,999,216. Initial displayed
article layout was 50 ms at 2526 by 1259.

The original mutation run retained 52,262 reused boxes, a 54,706,896-byte
allocator peak and a 10 ms initial displayed layout. Its replacement timing
is 2.41% above the prior control median; this workload still records snapshots
because its cascade can update locally for other mutations.

This confirmation supports removal of the avoidable fallback bookkeeping:
article total is within 0.73% of the prior control median. It is one run,
not a new three-pair campaign or proof that every remaining performance gate
has closed. The original replacement overhead and repeatability of the
corrected article therefore required the final comparison below.


## Text-run allocation follow-up

A private native probe timed Yonder's text allocator callbacks during the
large gallery setup. In one diagnostic run, that layout made 41,148 allocation
calls and 20,624 frees. Time inside allocation callbacks was 102,253 of the
layout's 186,081 microseconds. The per-call clock reads add overhead, so these
figures locate work; they are not acceptance timings or a measured explanation
of the entire control/candidate difference. Starting at the final window size
also produced substantially different timings from starting small and resizing.

Text runs owned up to five allocations with the same lifetime: the run record,
copied bytes, glyph placements, image pointers and caret positions. The
follow-up stores them in one checked-size allocation. Glyph cache ownership,
retained-run lifetime, the public API and allocator policy stay the same.
Frozen vectors, allocation-denial cleanup, retained runs, real-font rendering,
UI text and browser tests cover compatibility. A regression test denies the
second allocation after warming the glyph cache and checks bytes, glyphs,
carets and lifetime for ordinary, empty, tab-only and long runs. The original
implementation fails that test.

One subsequent hardware confirmation used the same 2,000 rows, 20 samples,
five warm-ups, 2526 by 1259 viewport, 100% zoom and dark-page setting. The
frozen cohort changed only libos64. The complete report exactly matched its
post-report heap capture; the native record contained the manual diagnostic,
104,287 reused boxes and no refused/partial layouts or clock errors.

| Graft measurement | Prior control | Prior candidate | Text-run follow-up |
| --- | ---: | ---: | ---: |
| Setup/rebuild ms | 123.30 | 159.90 | 91.20 |
| Paragraph around float, median ms | 56.90 | 10.10 | 10.65 |
| Append row, median ms | 53.35 | 10.35 | 10.20 |
| Peak live allocator payload, bytes | 56,042,592 | 57,002,640 | 55,883,040 |

Prior timing columns are medians of three runs; prior memory columns are the
supplemental memory pair. The follow-up is one run, not a new repeated matched
campaign. Its setup time is 42.96% below the prior candidate median and 26.03%
below the control median. The two incremental operations remain near 10 ms,
and peak live heap payload is slightly below both prior memory observations.
These results support the allocation change without establishing repeatability
or closing the fallback and replacement acceptance considerations. Reduced
allocation count alone is not a performance gate.


## Final three-pair comparison and acceptance

After #438 and #439 merged, the frozen 5b control was compared with the final
candidate in this order: control 1, candidate 1, candidate 2, control 2,
control 3, candidate 3. Each batch ran graft, original mutation and article in
fresh Yonder processes: 18 planned runs across four installs. The candidate
runtime was `904e106c`, included in integration merge `94a8ad38`.
[The final evidence](5c-p5-final.json) retains per-run samples, medians, paired
changes, heap values, native counters, exclusions and installed-file hashes.

Each batch verified all 22 runtime files and four fixtures against its frozen
cohort. Personal/system font and browser settings were unchanged across the
campaign. The viewport was 2526 by 1259, device pixel ratio 1, zoom 100%, dark
appearance, scripts and Modern Web enabled. The Web default font size was
30 px. The article checkpoint heights differed from the earlier campaign;
the earlier font setting was not verified. Consequently the old article times
are not compared or pooled with these results. All 18 checkpoint heights match
exactly across the six article runs in this final campaign.

The user maximized each fresh window by title-bar double-click and waited
three seconds before measuring. Interactive fixtures used 2,000 rows, 20 timed
samples and five warm-ups. The article used the starter/address-bar sequence.
One candidate-1 mutation run was repeated for a manual diagnostic: the later
run was selected independently of its speed, and the earlier report is retained
separately. Control-1 graft lacks a separate manual paint reading, but its
complete report, automatic native layout totals and matching audited heap
capture are present. Other selected runs have the manual capture. Every
selected report exactly matches its captured heap report; native phases contain
no refused/partial layouts or clock errors. Slow samples were retained.

Values below are medians of three per-run medians, setup times or article
totals, never medians of pooled samples.

| Measurement | Control ms | Final candidate ms | Change |
| --- | ---: | ---: | ---: |
| Graft setup/rebuild | 167.90 | 103.20 | -38.53% |
| Paragraph around float | 58.25 | 10.75 | -81.55% |
| Append one row | 55.00 | 10.40 | -81.09% |
| Read unchanged geometry | 0.10 | 0.10 | unchanged |
| Original paragraph edit | 42.85 | 10.70 | -75.03% |
| Original list replacement | 56.85 | 50.20 | -11.70% |
| Article checkpoint | 21.15 | 20.10 | -4.96% |
| Article total | 424.60 | 404.90 | -4.64% |

The three remaining timing concerns improve in every pair: setup falls
37.76%–38.65%, replacement 10.23%–14.09%, and article total 4.64%–6.32%.
Candidate setup times are 103.20/105.20/103.00 ms; replacement medians are
50.20/50.90/49.70 ms; article totals are 404.90/397.60/409.10 ms. These are
repeatable improvements in this workload/profile, not universal speedups.

| Workload | Control peak bytes | Candidate peak bytes | Change |
| --- | ---: | ---: | ---: |
| Graft | 56,406,208 | 56,238,464 | -0.30% |
| Original mutation | 54,089,824 | 54,484,784 | +0.73% |
| Article | 60,528,944 | 60,529,760 | +816 bytes |

These are medians of process lifetime maximum live allocator payload through
post-report capture, including startup and maximization. Heap audits and
accounting identities passed. The original mutation case retains a small
memory cost of 394,960 bytes (0.73%); it is accepted as a bounded cost of this
tested profile, not described as a memory improvement. This measurement does
not replace sanitizer leak checks or establish a long-running memory bound.

Together with the earlier correctness/equivalence suites, manual gallery
acceptance and localized-mutation results, this final comparison closes the
5c hardware performance gate. The setup and broad-fallback regressions have
been addressed, the original replacement improves, and the positive reuse
cases remain fast. No additional workload or measurement round is required
for this gate. 5d (`requestAnimationFrame`) is the next scheduled slice.
The separate selector-invalidation proposal remains unscheduled; this result
does not make the broad-selector article a positive geometry-reuse case.
