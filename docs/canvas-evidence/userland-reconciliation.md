# Userland reconciliation after animation frames

This merge brings userland `f78c8653` (through #443) into Canvas integration
`0bed4a91` (through #442). It includes the intervening userland history;
Canvas remains on the integration branch. Pointer events/capture and C6
remain the next work. Userland advanced to #445 (CSS filters) during this
reconciliation; that later work is outside this pinned snapshot.

## Combined behavior

- Picture catalog keys include URL byte length and Canvas request mode.
  Long data URLs remain whole, CSS span lookups select display pictures,
  and rehashing preserves separate no-CORS/anonymous/credentialed entries.
  Worker diagnostics, cancellation and ownership improvements from the
  data-picture backport coexist with Canvas origin-clean metadata.
- Script fetches keep HTTP/bytecode caching and response filtering. Native
  image fetches retain opaque pixels with their taint verdict. Both paths
  record request timing.
- The shared DOM mutation dispatcher queues observer records before its
  Canvas/custom-element consumers. Same-value width assignments both reset
  the bitmap and produce observer records. Teardown releases observers,
  frame callbacks and Canvas resources.
- Cascade match memoization includes the element, pseudo-element target
  and update generation. Incremental updates recompute pseudo-element
  winners and inherited custom properties. Layout rebuilds subtrees with
  generated nodes: counters/quote depth can change without changing the
  originating element's ordinary style. Copied-style scratch and counter
  state are both freed on failure.
- The browser keeps animation and download deadlines, the idle wake fix,
  profiling and exit-when-loaded switches, and userland's stack/budget changes.

## Regression uncovered during validation

The larger combined DOM profile exposed an existing QuickJS failure path:
`JS_ToCStringLen2` held its source reference when allocating the UTF-8 export
failed. LeakSanitizer traced a 5 MiB retained string to this path. Patch 0017
returns that reference on refusal; a deterministic engine test exercises both
Latin-1 and wide strings with allocations above the native small-object arena.
The regression failed before the fix. The patch changes the generated engine
identity, so older cached bytecode falls back to source compilation.

The Storage quota fixture separately receives a 32 MiB engine budget so its
5 MiB area-quota checks can hold wide strings and temporary exports alongside
the full binding. Production budgets are unchanged. Engine memory refusal
remains covered by the dedicated regression and the existing failure sweeps.

## Validation

- Browser: 15,610 checks, zero failures, including the combined image-catalog
  and same-value Canvas reset/observer regressions; classic and module
  bytecode caching remain covered.
- DOM behavioral mode (`--mutants`, which omits the exhaustive full-installer
  sweep): 7,729 target-engine checks and 15,080 sanitized-engine checks,
  zero failures; 357 and 1,404 binding allocation sweep cases respectively.
- Engine adapter: 681 sanitized checks and 443 target checks, zero failures;
  expected fatal-path checks also pass. UTF-8 refusal leaked before patch
  0017 and releases its allocations after it.
- CSS: full host suite passed; 1,740 incremental cascade checks, zero failures.
- Layout: 303,807 checks, zero failures, including fresh-layout comparisons
  for generated text/counters, pseudo-element changes and source-tree teardown.
- Fetch jobs: 661 checks, zero failures, covering native image tainting and
  cached script responses together.
- ASan/UBSan and LeakSanitizer were enabled for the sanitizer runs.
- Native Yonder/shared-library and kernel builds passed. QEMU displayed the
  final animation gallery at 50%: canvas pixels change while running, remain
  identical while paused, and change again after resuming. The data-picture
  gallery displayed PNG, uppercase DATA, SVG, CSS data backgrounds including
  the long URL, JPEG and GIF; its deliberately broken sixth card stayed blank.
- The retirement-reference check was inspected: the named historical design
  documents still exist. Observer comments now name the shared dispatcher.

P5 acceptance of this reconciliation remains pending. The handoff contains
22 runtime files (10 changed, 12 reused) and the existing animation and
data-picture galleries. Exercise pause/resume and inspect both galleries
with Scripts and Modern Web enabled; card 6 of data-pictures is intentionally
broken. This is a focused compatibility check, not another timing campaign.
