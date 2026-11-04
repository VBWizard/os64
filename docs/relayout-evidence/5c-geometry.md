# Relayout 5c geometry-copy evidence

This is step 3 of [the approved contract](../design/pending/RELAYOUT_GRAFT.md),
following the snapshot foundation in #430. The browser integration and P5
acceptance are separate work; these results do not establish hardware timing.

## Behavior and ownership

The explicit incremental entry builds fresh boxes/items and copies eligible
ordinary-block layout results into fresh arena records. It rebinds text, style,
inline-span and model indices, retaining immutable text runs. The source tree
can be freed before reading, hitting, selecting or painting the destination.
Intrinsic width hits copy the four width values, without placed height/lines.

Each call records its incoming margin extrema, containing dimensions, sizing
mode, first-placement state and untranslated origin. Float queries are tracked
in their inherited Space coordinates, including queries made with no floats
present. Copying refuses a previously influenced subtree or a translated query
that reaches above the new float bottom. Inner BFC queries have their own space.
Missing-image alt text is conservatively rebuilt because its processed bytes
are produced during pass 3 rather than in the fresh item table.

Copy staging is private until its subtree succeeds. Allocation, retention or
translation failure produces the established incomplete-prefix result; the
copy is not followed by a second speculative layout. Persistent snapshot,
matching tables and the largest live staging allocation share one metadata
ceiling. The previous snapshot is separately bounded and released with its tree.

## Automated evidence

`tools/test_libflow_host.sh --graft` compares each result with an independently
cascaded whole layout after freeing the source. It compares dumps, public box
geometry/styles/ink, current link/control indices, run bytes and glyph metrics,
caret positions, selection rectangles, clipping, hit identities, scroll reach
and painter traversal. Cases cover translated siblings, empty collapsed blocks,
equal-sum/different-extrema margins, negative-margin float intrusion, relative
Space offsets, flex/grid/table placement, ellipsis, markers and missing images.

The synthetic article uses a grid column and an infobox float. Changed content
and paragraphs beside the float are measured; unaffected paragraphs below it
reuse geometry. This is distinct from the unchanged captured Wikipedia article,
whose broad-selector fallback remains a later no-regression control.

Appending one row to 2,000 list items changes IFC measurements from **2,000 to
1**, copies **2,000 boxes**, and performs **one marker measurement**. The new
snapshot plus matching and peak staging uses **597,664 bytes**. This number is
libflow reuse metadata, not total process memory. FBox grows from 480 to 504
bytes (32 extra bytes per arena allocation after alignment), including legacy
layouts; hardware first-layout timing and process memory remain acceptance gates.

The allocation sweep fails **2,106 positions**, both as a single failure and
persistently from that point, checking **1,285 partial trees** against the
valid-prefix contract and returning ownership to the source baseline. Checked
retain overflow is tested separately; the source remains usable and an
incomplete result cannot supply geometry or intrinsic widths.

Host runs use ASan, UBSan and LeakSanitizer, with leak detection enabled outside
the sandbox. The freestanding target library builds with warnings as errors.

Recorded results: full suite **301,553 checks, zero failures**; final targeted
geometry/ownership suite **251,829 checks, zero failures**. The comment audit's
reported names are existing document links and a test-file wildcard, not retired
symbols. The test-harness comments were corrected: literal spec expectations
and whole-layout equivalence oracles prove different properties.
