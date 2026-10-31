# Relayout 5c: browser integration and acceptance

This completes the four implementation steps in
[RELAYOUT_GRAFT.md](../design/pending/RELAYOUT_GRAFT.md). The foundation and
geometry-copy slices passed review in #430 and #431. Integration is isolated
from userland. **P5 timing, memory and manual visual acceptance remain pending.**

## Browser contract

Displayed updates and synchronous hidden measurements use
`flow_layout_incremental` through the shared wrapper. Font and picture epochs
belong to a stable process-lifetime owner, not a temporary Page address. Picture
arrival/release and font arrival, fallback retry, failure and release advance
the corresponding epoch before another layout can consume its answers. Clearing
the batching flags does not reset these values; overflow disables reuse.
Model rebuilds also advance both epochs when the resolution base changes:
relative URLs can select different already-loaded resources without an arrival.
History rebases discard hidden measurements even under an unchanged absolute
base, so a later base removal uses the new document URL.

Web-font unicode-range selection depends on the whole document's characters.
A DOM-version change with font-face rules therefore conservatively advances the
font epoch. Shared form-state identity/version is captured at the actual call,
including reads before displayed bookkeeping catches up. Native DOM history
covers SVG and canvas sizing attributes. Canvas pixel writes do not invalidate
geometry. The hidden measurement cache also checks resource/state inputs and
appearance/reduced-motion settings before returning an existing answer.

A hidden rebuilt model can have new image indices before the displayed image
map is rebuilt. The size resolver now looks up the current model's image URL
in the existing catalog. It neither starts I/O nor borrows an old model index.
A regression inserts an earlier image and verifies the kept image's dimensions.

Copied runs bypass the shaping pass's font resolver calls. For copied layouts,
the font report is reconstructed from the resulting public text, line/span metrics
and atomic content. A bounded deduplication table avoids repeated identical
requests. Those reports count font requests rather than repeated shaping
lookups. Removed text cannot leave a stale family record; kept text retains its
face-use record, including an empty inline span whose font contributes metrics
without a text run. The reporting work is charged to the layout phase.

## Automated checks

- Complete Yonder scripted-page suite: **12,639 checks, zero failures**, including
  displayed/hidden geometry reuse, shared state, image arrival and canvas size.
- Extended font suite: **2,976 checks, zero failures**, including loaded-face
  copying, family/face reporting and document-wide font dependency invalidation.
- Final focused browser suite: **90 checks, zero failures**, including epoch
  overflow and appearance/reduced-motion changes in the hidden cache, plus
  history URL changes, base attribute changes, and absolute base removal.
- Existing mutation fixtures: quick and full (2,000 rows / 20 samples), original
  and localized-selector variants: **16 checks each, zero failures**.
- New float/append fixture: **16 checks, zero failures**, including both manual
  buttons, both full experiments, cancellation and viewport invalidation.
- Unchanged captured article: **9 checks, zero failures**, 18 parser geometry
  checkpoints, zero incomplete layouts and clock errors. It retains 7,825 style
  elements, 5,209 boxes and 3,043 IFC measures, with zero geometry/style reuse or
  incremental cascade updates, as expected for this broad-selector control.

All host checks use ASan, UBSan and LeakSanitizer with leak detection enabled.
The target Yonder executable and matching shared libraries build with warnings
as errors. Geometry ownership/allocation evidence is in
[5c-geometry.md](5c-geometry.md).

The new browser fixture's full run records 55 layout attempts, 108,390 copied
boxes, 4,397 IFC measures, 80 intrinsic hits, 4,026 marker measures and a peak
new reuse-metadata allocation of 1,378,320 bytes, with zero partial layouts.
These totals include setup and manual button actions; they are not per-append
work counts. The isolated libflow append oracle proves one new IFC and one
marker measurement for a single append. Metadata excludes the separately
bounded source snapshot and other browser heap owners.

## Hardware handoff

Use the frozen 5b control and 5c candidate with identical settings and a fixed
matching viewport. Alternate three runs per cohort. Workloads are:

1. `layout-measure.html`: original positional-selector mutation page.
2. `layout-measure-local.html`: localized-selector companion.
3. `layout-graft.html`: synthetic grid-column/float and 2,000-row append page.
4. `layout-stream-article.html`: unchanged captured article, reached by the
   established starter/maximize/address-bar protocol from the 5b comparison.

For each completed run save the report and native diagnostic (`d` while page
text has focus). Preserve raw samples, work counts and the viewport/settings.
Compare first opt-in layout cost and process memory as well as updates; native
`layout graft` reports peak **new reuse metadata**, not total process memory.
The new page's `initial_layout_ms` is its reset/setup geometry read, not browser
startup or a standalone process-memory measurement. Capture those separately.
No QEMU timing is claimed as P5 performance acceptance.

## QEMU functional run

The isolated 4-core / 4-GiB guest completes the full 2,000-row / 20-sample
float/append page at 828 × 509 CSS pixels. Its native record reports 53 layout
attempts, 104,312 copied boxes, 4,387 IFC measures, 77 intrinsic hits and 4,025
marker measurements, with zero partial layouts or clock errors. The peak new
reuse metadata is 1,378,320 bytes. These totals differ from the host run because
its startup/manual actions differ. Screenshots and the native corpus record
are retained with the frozen cohort's local evidence.
