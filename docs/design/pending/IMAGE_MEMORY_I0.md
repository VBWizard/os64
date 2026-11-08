# Image memory I0: stylesheet preload

2026-11-08. Implementation evidence for the stylesheet prerequisite in
[IMAGE_MEMORY.md](IMAGE_MEMORY.md). This addresses the missing stylesheet
activation pattern found on Microsoft Design wallpapers. Hardware acceptance
and the live-site comparison remain pending; this document claims no measured
hardware memory improvement.

## Behavior

Connected `link rel="preload" as="style"` elements fetch their responses without
applying CSS or holding document load. Changing `rel` to `stylesheet` joins an
in-flight compatible request or parses its retained response. Preload completion
and stylesheet completion are separate element events. The familiar
`onload="this.onload=null;this.rel='stylesheet'"` pattern works; a handler that
stays installed can receive the second event.

The preload account belongs to the document, separately from stylesheet tables
that layout rebuilds replace. It retains responses independently of HTTP cache
acceptance, including `no-store` replies and failures. One compatible consumer
can claim an entry. Response bytes remain available across canceled staged
parses and are released when a live consumer completes, or at document teardown.
A consumed failure stays failed across unrelated layout rebuilds.

The account admits 64 entries and 16 MiB of retained response bodies per
document, with the existing per-sheet fetch cap and worker reservation. Excess
requests or bodies produce diagnostics rather than successful completion. The
memory census reports retained preload bodies and their account metadata.
Worker input copies remain covered by the worker reservation.

Reuse checks the resolved URL, style destination, CORS mode, credentials,
referrer policy and integrity compatibility. An empty consumer integrity
attribute accepts the retained outcome; nonempty integrity strings must match
exactly. This is conservative compatibility, not SRI verification or full
normalization of integrity metadata. Link integrity remains unenforced as
recorded in DEBTS; `fetch()` still rejects nonempty integrity.

The shared native fetch path applies hop and CORS checks to network and cached
responses. Styles retain MIME headers and the final URL for parsing and imports;
script fetches still receive filtered opaque responses. Ordinary stylesheet
requests use the same path. The quirks-mode MIME exception checks the final
resource origin. Local stylesheet reads require a local referring document.

The document mutation log invalidates obsolete preload and consumer events
before task delivery. Link insertion and relevant attribute changes are found
through the existing stylesheet rebuild/stream walks. Media and type hints are
checked before requesting a preload. `as`, `crossOrigin`, `referrerPolicy` and
`integrity` bindings support these changes, and Modern Web reports `preload`
through `relList.supports()`.

Other preload destinations, modulepreload lifecycle work, SRI verification,
responsive image selection and metadata bitmap ownership remain separate work.
The [HTML preload lifecycle](https://html.spec.whatwg.org/multipage/links.html#link-type-preload)
and [stylesheet processing](https://html.spec.whatwg.org/multipage/links.html#link-type-stylesheet)
provide the acquisition/event and activation contracts.

## Validation

The host tests exercise the production stylesheet worker and native fetch path.
They cover no-store handoff without a second request, pending consumers, distinct
completion events, dynamic insertion, request-property reflection, incompatible
CORS requests, HTTP/MIME/network failures, stale completions before and after
activation, navigation, document base URLs and ownership limits. They also run
the delivered gallery with the production PNG decoder, substituting identical
PNG bytes in a data URL for the harness's text-only HTTP image transport.

The native-fetch suite passes 823 checks with ASan, UBSan and LeakSanitizer,
including opaque native responses from the network, fresh cache and 304
validation, script opacity and CORS refusal. The full Yonder host suite passes
23,780 checks with the same sanitizers enabled, including the gallery, limits,
queued completion guards and replacement of a canceled parse. The M0 image
ownership readings remain unchanged. Target Yonder and its matching shared
libraries build successfully; `git diff --check` and `tools/stale_refs.sh` pass.

## Hardware comparison

`tools/generate_image_memory_fixture.py` generates the shared PNG, CSS and three
stylesheet comparison pages beside the original M0 image-memory pages:

| Page under `/tests/pages/` | Expected width at 100% zoom |
|---|---:|
| `image-memory-preload-active.html` | 160 CSS px after event-driven activation |
| `image-memory-preload-inactive.html` | 1920 CSS px; intentional horizontal overflow |
| `image-memory-preload-ordinary.html` | 160 CSS px from an ordinary stylesheet |

Use Modern Web and scripting enabled. The active and ordinary pages should
report PASS and match visually; the inactive page should remain large. The PNG
must be installed beside the HTML, as described in
[IMAGE_MEMORY_BASELINE.md](IMAGE_MEMORY_BASELINE.md).

The original M0 diagnostic records are preserved, and their image ownership
readings match the host baseline. Compare Microsoft Design `/wallpapers/` at the same viewport, zoom and
cache conditions, recording stylesheet activation and horizontal extent as well
as the memory census. Attribute any resulting image-size/memory change to CSS
activation separately from I1–I3.
