# Yonder SVG raster reuse

2026-10-28. This 6c increment connects the [raster-cache foundation](SVG_CACHE.md)
to Yonder's inline SVG, image and background painting. The gallery is
`/tests/pages/svg-encore.html`, **SVG: an encore**. Scripts enable its reversible
flower-color button; Modern Web enables SVG rendering.

## Rendering and ownership

The adapter resolves current styles and resources and prepares a fresh scene
before creating a value key. A match copies owned cached pixels into the
caller's destination, bypassing geometry lowering and rasterization. A miss
renders normally and admits a copy when space and work permit. The destination
retains its original allocator and lifetime. Cache entries retain neither DOM
pins nor provider storage, and equivalent values from a newly parsed document
can reuse an earlier entry.

Inline clips use their actual origin and extent. SVG images use the resolved
CSS viewport and output pixel dimensions, including zoom; their independent
cascade resolves media state before lookup. Processing mode and backend identity
remain cache boundaries. Mutation changes the resolved key, rather than relying
on source addresses or a handwritten dependency list. Returning to an earlier
value can reuse a retained variant. This is raster reuse: preparation and image
cascade construction still happen, and synchronous misses can still be costly.

A cache is created lazily when the installed page paints SVG. Window calls are
serialized; incoming picture workers continue to return documents, not cached
pixels. The cache's allocator points to the installed Page, whose address stays
stable during painting. Staged layouts borrow the page state without owning
its cache. Navigation and close release the cache before clearing the page.

## Admission, cancellation and pressure

The window retains at most 32 entries and 8 MiB of entry allocations, including
keys and allocation headers. The wrapper and libsvg context are accounted
separately in the same 64 MiB SVG/backend/destination ledger; transient keys
are capped at 256 KiB. Cache comparison/copy work has its own finite 64 MiB
work-unit budget. The existing document/cascade caps remain separate.

The page allocator can trim retained entries when mandatory SVG scratch or
output needs room, or when a physical allocation fails. Trimming is suppressed
while read/admission operates in the cache's owning context, avoiding allocator
callback reentry. Cache creation or admission failure leaves uncached painting
available. Key failures bypass reuse unless cancelled. If a transient key
competes with raster scratch, the renderer releases it and retries once without
caching. A failed store does not invalidate successfully rendered pixels;
cancellation does, and the output is discarded. Failed or cancelled operations
do not publish partial entries.

The diagnostic record's `svg raster cache` fact exposes hits, misses,
rasterization attempts and optimization bypasses. `svg raster memory` records
entry count, retained owned bytes, evictions and the shared ledger's peak.
These are work/retention counters, not a claim about whole-page latency.

## Evidence

ASan, UBSan and LeakSanitizer are enabled for the host suites. The adapter checks
owned cold/warm output equality, reuse after source destruction, dark/reduced-
motion and 25%/400% variants, disabled admission/key bounds, cache-creation
refusals, 35 render-allocation cuts in both refusal modes, and 847 cold/324 warm
cancellation cuts. Its existing shape, stroke, clip, viewport, use, image and
gradient regressions also pass.

The real window suite passes 10,991 checks. Repeated paints reuse the same
raster; script mutation produces fresh colors while keeping both variants;
memory pressure evicts entries and rendering recovers; image media changes
produce fresh pixels and restoring the earlier media state reuses its variant.
Page teardown releases retained ownership. Existing window tests continue to
verify shared-resource mutations, image isolation, background painting and zoom.
The freestanding Yonder build passes.

The frozen delivery contains 39 files. Thirty-five reuse the prior radial
cohort's exact bytes; Yonder, libsvg and the two new gallery assets differ.
Unchanged shared-library allocated sections were verified before reuse;
QuickJS generated source and build inputs match despite diagnostic-path
strings in independently built binaries. The matching libsvg includes the
reviewed gradient corrections and cache API.

QEMU verifies the frozen cohort at 25%, 100% and 400%. Both large gardens
restore pixel-for-pixel after toggling, the small views change together, and
the independent postcard remains byte-identical across the shared mutation.
Its private dark-media sky changes when dark pages are enabled. The clean
gallery diagnostic records 46 cache hits, 43 rasterizations, no bypasses,
34 evictions, 7,466,000 retained bytes and a 12,759,383-byte peak SVG ledger.
Yonder closes normally; the guest shuts down cleanly and read-only checks pass
for both private filesystems. These counts describe the exercised gallery
session, not a corpus-wide speed or hit-rate claim.

[Source/binary evidence](../../svg-evidence/cache-yonder-results.json) and the
[frozen manifest](../../svg-evidence/cache-yonder-delivery.sha256) identify the
cohort. Screenshots show the [initial gardens](../../svg-evidence/yonder-encore-top.png),
[changed flowers](../../svg-evidence/yonder-encore-changed.png),
[restored gardens](../../svg-evidence/yonder-encore-restored.png),
[small views and postcard](../../svg-evidence/yonder-encore-bottom-changed.png),
[dark media](../../svg-evidence/yonder-encore-dark.png),
[25% view](../../svg-evidence/yonder-encore-25.png) and
[400% detail](../../svg-evidence/yonder-encore-400-final.png).

The gallery compares two full-size gardens, two small shared views and a
standalone postcard. Changing the flowers affects the shared views; the postcard
retains its own apricot flowers. Dark pages change its sky through its private
media query. Hardware acceptance and corpus-led latency/fidelity measurements
are separate gates; this slice does not complete 6c or introduce a frame loop.

Reproduce the host checks with:

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-cache-yonder
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
```
