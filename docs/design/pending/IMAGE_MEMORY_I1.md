# Image memory I1: native image HTTP cache

2026-11-08. Implementation evidence for I1 in
[IMAGE_MEMORY.md](IMAGE_MEMORY.md). Host validation is complete; P5 acceptance
and live-site transfer measurements remain pending.

## Behavior

Native image jobs now pass the browser's cache to the shared fetch engine.
This includes requests caused by image metadata access and Canvas source
acquisition. Eligible responses can answer from a fresh cache entry or use a
304 validation instead of transferring the image body again.

Native requests select the identity's image Accept profile. Without a preset,
they use the picture worker's supported image types, including SVG when Modern
Web is enabled. Explicit request headers retain precedence in the shared fetch
engine; script and stylesheet destination profiles keep their existing behavior.

The existing request policy remains in force: image destination, GET, document
origin and strict-origin-when-cross-origin referrer policy; no-CORS with included
credentials, anonymous CORS with same-origin credentials, or credentialed CORS
according to the image request mode. Cache keys, Vary matching, live CORS
checks, cache disablement and response cache policy use the shared machinery.
Native pixels keep the response's taint verdict. A cross-origin redirect detour
does not become origin-clean merely because its final URL returns to the
document's origin. The native engine does not cache redirect chains.

Display fetches still use `way_fetch_whole`. Its headerless cache entries cannot
supply the response headers needed by the native engine, so they remain misses.
A complete cacheable native response replaces such an entry. A later display
fetch that writes a headerless response can require another native fetch. I1
does not claim a single transfer across arbitrary alternating consumers; moving
display requests to the header-aware path remains a prerequisite of I3.

HTTP caching saves transfers, not decoded pixel storage. Native images still
decode on a new acquisition and retain their full-resolution bitmap. The
metadata ownership change, responsive source selection and picture budget are
unchanged in this slice.

## Validation

`tools/test_yonder_image_cache.inc` runs the production picture worker, native
fetch engine and cache, with recorded fake HTTP, a fixture filesystem and the
production PNG decoder. It checks the 1920×1080 baseline PNG through network,
fresh-cache and 304 paths, including request counts, decoded dimensions,
retained bytes and taint. It also covers disabled or absent cache, removal of a
cached entry, no-store responses, Vary on Accept, image identity profiles,
credential separation, CORS rejection, and same-origin/cross-origin redirects.

The legacy-entry cases deliberately replace complete entries with the
headerless format used by display fetching, twice, and require a network miss
followed by a native cache hit each time. This tests native handling of that
format; the display transport itself is stubbed in this host suite.

- Full Yonder host: 23,897 checks, zero failures.
- Focused image run: 3,532 checks, zero failures.
- Shared native/script fetch suite: 823 checks, zero failures.
- ASan, UBSan and LeakSanitizer enabled for each run.
- Target Yonder and its dependency closure build successfully.
- Diff checks pass. The stale-reference scan reports the retained M0/I0
  document links; both targets still exist and the references remain valid.

The original ownership fixtures retain the M0 readings: 57,600 B of display
pixels on each page; no native pixels for untouched images; 8,294,400 B for
metadata and Canvas acquisition. This slice claims no retained-memory reduction.

## Hardware comparison

Keep the accepted I0 cohort and its records. Install the matching I1 cohort,
enable Modern Web, scripting and HTTP cache, and repeat Microsoft wallpapers
at 100% zoom with the same window geometry. Save `d` after images settle, leave
the page, then revisit and save another record. Compare the `picture fetch`
network, fresh-cache and validated counts and transferred entity bytes. Existing
cache contents, response policy and headerless entries can affect the first
visit; fewer transfers must be demonstrated by the counters, not inferred from
visual similarity or timing alone.

The original `/tests/pages/image-memory-canvas.html` remains a local regression
check for one draw in onload and immediate readback. Local file pages do not
exercise HTTP caching. A controlled network fixture must be served over HTTP
to measure its transfers; the binary delivery service is not an HTTP server.
