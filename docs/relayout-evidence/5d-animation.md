# Slice 5d — animation frames

`requestAnimationFrame` and `cancelAnimationFrame` are available with Modern
Web enabled. This cut sits on the merged 5c acceptance, parent `5043130b`,
and targets `canvas-integration`. Pointer events and capture, followed by Canvas
C6 target work, remain separate cuts.

## Contract

The [rendering contract](../design/pending/RELAYOUT.md#requestanimationframe)
covers callback order, shared timestamps, microtask checkpoints, cancellation,
execution bounds, covered windows, cost pacing and the idle deadline. The queue
belongs to libdom; Yonder supplies rendering opportunities and presents once
after the batch. Script-only canvas pixel updates preserve the layout tree.
CSS motion runs on the same window loop after callbacks, and a script's fresh
layout is not laid out again for CSS before its paint.

`tools/pages/canvas-animation.html` (In Motion) has four samples: travelling
waves, an orbit, moving text, and an HTML element driven by the same timestamp.
Pause/resume uses cancellation and requests a fresh callback on resume. Reverse
changes direction. Scripts and Modern Web must be enabled; the CSS Animations
preference is independent of the script API.

## Standards checks

Pinned WPT: `a699fb16767be39ba3e61a8e8b6bef60383f08ee`.

| Directory / configuration | Pass | Fail | Timeout |
|---|---:|---:|---:|
| animation-frames, unchanged parent `5043130b` | 0 | 15 | 0 |
| animation-frames, this cut | 15 | 0 | 0 |
| animation-frames, Modern Web disabled | 0 | 15 | 0 |
| requestidlecallback, this cut | 18 | 4 | 1 |

Animation-frame coverage comprises 11 pages. The cross-realm exception page is
excluded because it requires an iframe script realm; the manual page is outside
the automated runner. The 15 applicable checks are required, including the
frame restart after repeated callbacks which make no DOM changes. Disabling
Modern Web restores the missing-API outcomes.

The idle suite retains its 17 baseline passes and gains the callback-period
check. Two newly enabled frame-deadline checks assume a 16.67 ms interval; this
OS's 100 Hz tick supports the documented 20 ms frame interval. They report
20 ms and remain visible failures. Two busy-loop scheduling failures and the
Date.now busy-loop timeout predate this cut. Expectations do not hide these
results or change the upstream tests.

The headless WPT loop now closes the rendering opportunity after callbacks and
CSS sampling. Its microsecond clock advances during simulated 10 ms waits as
well as script execution; previously only the millisecond timer clock advanced
while waiting, which made elapsed performance timestamps disagree with timers.

## Native and host checks

- The focused DOM frame probe runs against both the target QuickJS core and
  the sanitized core, with 47 checks each. It covers snapshots, shared
  timestamps, microtask cancellation, deferred requests, exceptions, quota,
  allocation refusal, teardown, runtime limits, and the idle rendering grant.
  Two callbacks individually below the task limit exhaust the combined limit,
  proving that a batch does not renew its budget between callbacks.
- The browser suite passes 12,485 checks and covers pixel damage without layout, one presentation for
  multiple callbacks, DOM/microtask edits in one cascade update, CSS/script
  layout sharing, covered-window suspension, resume without backlog, cost
  pacing, timer progress and navigation teardown.
- The DOM behavioral suite passes 15,052 checks; P2b passes 6,665 checks,
  including binding and engine allocation sweeps.
- ASan, UBSan and LeakSanitizer remain enabled. Host executions requiring
  thread attachment run outside the restricted sandbox.
- The native Yonder target and its shared libraries build with warnings treated
  as errors. The delivery compares allocated ELF sections with the previously
  validated cohort and reuses unchanged files.

Reproduction:

```sh
tools/test_dom_host.sh --animation
tools/test_dom_host.sh --mutants
tools/test_dom_host.sh --p2b
tools/test_yonder_scripts_host.sh
tools/test_yonder_wpt_host.sh html/webappapis/animation-frames
tools/test_yonder_wpt_host.sh html/webappapis/animation-frames --modern-off
tools/test_yonder_wpt_host.sh requestidlecallback
```

The `--mutants` DOM mode runs the behavioral suite without the exhaustive
whole-installer allocation sweep. The focused frame refusal and P2b allocation
sweeps run separately. The missing-global lookup fixture uses `noSuchGlobal`
instead of the newly implemented `requestAnimationFrame`.

QEMU visual checks cover moving drawings, stable paused pixels, resumption,
and zoom. P5 visual acceptance passed: the gallery's controls worked correctly
and the animations displayed successfully. A slight stutter was observed in
the orbiting square. Frame pacing and rendering performance remain follow-up
work; this cut makes no new hardware performance acceptance claim.
