# SVG raster-cache foundation

2026-10-27. This 6c increment implements owned raster value keys and a bounded
cache in `libsvg`. It follows [radial gradients](SVG_RADIAL.md). The library foundation is connected to inline and image painting by
[the Yonder cache integration](SVG_CACHE_YONDER.md), which owns its bounded
cache and provides the consumer tests and hardware gallery.

## Value boundary

`svg_raster_key_create` takes a prepared scene, render options and destination
extent. It validates the target through the painter's validation routine,
then encodes a versioned field stream with fixed-width little-endian words,
finite binary64 values and explicit lengths. Signed zero is normalized.
The key owns its bytes and can outlive the scene; no DOM pointers, provider
storage or source/instance identities become key bytes.

The stream includes processing mode, hierarchy/drawing order, paths, local and
viewport transforms, viewport clips, silhouette clips, fill/stroke values,
dashes, opacity, visibility and both kinds of gradient descriptors/stops.
Local transforms matter independently of final transforms: group object
bounds walk the local ancestry. Curve controls, arc radii/rotation/flags,
output dimensions, fractional origin and numerical quality also participate.
The backend pointer identifies a separate process-local cache domain; its
immutable descriptor/implementation must outlive its keys and entries. Keys
are not a persistent serialization format.

Destination pointers, row padding, allocator bookkeeping and rendering work
quotas are not pixel values. Limits still govern the operations doing work;
a hit spends copy/comparison work without spending geometry/raster work.
The key is deliberately conservative rather than a visual-equivalence
canonicalizer: equivalent artwork with differently structured scene graphs
may miss. This does not permit different keys to share pixels accidentally.

The adapter must resolve current styles/resources before preparation and key
creation. Dark-page and media state affect the resolved values, not a borrowed
page identifier. A future provider with a new pixel-affecting field must extend
the key alongside the painter. Prepared-scene memoization and dependency
invalidation are separate work.

## Ownership and admission

A cache belongs to a context and can accept keys created by another context.
Keys and caches keep their respective contexts alive through the normal
`SVG_BUSY` destruction guard. Cache entries copy key bytes and tightly packed
pixels. Lookup copies pixels to a caller-owned target and preserves its row
padding; it does not lend cache pointers to painters or retain document pins.

A 64-bit digest selects candidates. Backend identity, key length and the full
encoded bytes establish equality. Tests force equal digests for different
colors and verify that the wrong pixels cannot be returned.

Entry and byte limits are literal, with zero disabling storage. Entry byte
accounting includes the entry descriptor, key, pixels and libsvg allocation
header. The cache descriptor and transient caller-owned keys are separate
allocations in their owners' shared ledgers. No cache storage escapes that
ledger. Least-recently-used entries are evicted before admission so old plus
new retained allocations stay within the cache cap; a refused replacement can
therefore leave fewer entries. Oversized entries are refused before eviction.

Insertion takes the actual rendering status. A failed render is returned
without publication. Allocation refusal and cancellation publish no partial
entry, and later operations can retry. A cancelled hit may have copied some
rows; the caller must discard that destination. Work charging and cancellation
polling cover key serialization, candidate comparisons and row copies. Release
and clear finish cleanup without cancellation. Access is caller-serialized;
context busy guards reject callback reentry rather than providing a lock.

Counters expose hits, misses, stores, evictions, entry count and retained
bytes. Clearing counts removed entries as evictions; duplicate insertion
refreshes recency without adding a store. These are cache-operation counters,
not browser latency measurements.

## Consumer sequence

1. Resolve and prepare a fresh scene through the existing provider.
2. Allocate the destination and create a bounded key for its actual extent.
3. Read the cache. On a miss, use `svg_scene_render`, then pass its status and
   pixels to `svg_raster_cache_store`.
4. Release the temporary key and scene. The cache can survive both.
5. Paint the caller-owned output and free it through its original ledger.

The host may bypass key/store admission failures as an optimization policy,
but must preserve cancellation and discard failed output. A cache-store failure
does not invalidate a successful uncached render. The browser integration must
own a bounded cache, keep old retained bytes in the shared admission total,
and use this sequence for both inline SVG and isolated image documents.
The [consumer tests](SVG_CACHE_YONDER.md) cover navigation/close cleanup,
mutation/zoom/media variants, memory pressure and counters. The public stats layout grows
with owned key/cache counts, so delivery must rebuild dependent consumers as
part of the normal matching binary/library cohort.

## Evidence

The painter harness includes cache contracts and its existing geometry,
compositing, clip, gradient, allocation and cancellation regressions. Fifteen
cache round trips compare cached and uncached output exactly, including groups,
curves, strokes, linear/radial paint, object-coordinate clips, fractional origin
and empty scenes. Additional checks cover changed resolved fields, normalized
zero, source-independent keys, backend separation, forced hash collisions,
context destruction, padding, byte/entry caps, recency and recovery after
an eviction followed by allocation failure.

Key construction sweeps 191 cancellation points; insertion and hit-copy each
sweep 69. Allocation failure is tested as physical exhaustion and budget
refusal. ASan, UBSan and LeakSanitizer remain enabled. The existing core suite
passes 156,298 checks against the new context/scene lifetimes. The painter
suite passes 1,833,163 assertions. The freestanding library
build links the new public APIs without a system allocator dependency.

[Cache results](../../svg-evidence/cache-results.json) record test counts,
source hashes, compiler flags and binary hashes. Reproduce with:

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-cache-paint
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --output /tmp/svg-cache-core
make -C userland svg-library
```

Browser-facing adoption is recorded in [SVG_CACHE_YONDER.md](SVG_CACHE_YONDER.md).
Corpus-led memory/latency measurements and the remaining static image route
acceptance gates remain in 6c; broader static graphics remain in 6d.
