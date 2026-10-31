# 5c hardware acceptance: initial comparison

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
6.67%, 7.29% and 12.25%. The fallback/no-regression acceptance gate remains
open; this evidence does not establish a completed 5c performance pass.

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
remain in force. Hardware validation of this follow-up, including the
original replacement case, remains outstanding. Frozen comparison bundles
are preserved unchanged.
