# F2 — Text layout and drawing implementation candidate

2026-09-17, Quinn. Branch `codex/text-layout`, worktree `.worktrees/text-layout`.
Base `7c9e2d2` combines F0's frozen contract/receipt (`23bf6dd`, `0ab3cee`)
with the reviewed F1 checkpoint `f065c7c`. The F1 review evidence was checked
against its 420-file input manifest before that checkpoint was committed.
The freeze-receipt add/add conflict retained F0's complete receipt.

**Status: implemented and locally validated; independent F2 review is pending.**
This is an implementation handoff, not approval to merge or a claim that the
whole font feature is finished. No branch has been pushed as part of this work.

## What now works

The frozen public text interfaces have real implementations in libos64. A caller
creates a budgeted context, supplies the F1 backend table, opens immutable font
instances from copied file bytes, lays out a single line and retains the result.
That result owns its source snapshot, resolved font/glyph choices, fractional
positions, masks, legal caret boundaries and line/ink metrics. Painting,
measurement, fit, hit-testing and selection use that same result.

Implemented scope includes fallback before measurement, fractional pair kerning,
strict W1 UTF-8 decoding, the pinned Latin composition table, explicit legacy
Latin-1/CP437 interpretation, bitmap compatibility, grayscale clipping/blending,
fixed-cell mode and procedural U+2500..259F masks. Unsupported accents retain
the original bytes and become a base plus marker in a single source cluster.
This is the finite W1 profile, not general Unicode shaping or normalization.

The per-context LRU retains glyph images while runs pin them. Font identity
binds copied bytes, size and render options; reopening a font cannot change a
retained run. Context and backend budgets distinguish LIMIT from allocator
NO_MEMORY. A cap refusal may evict unpinned images and retry once; failed
allocations do not become successful empty runs. Drawing allocates nothing and
preflights translated coordinates before modifying pixels.

See [F2-IMPLEMENTATION.md](F2-IMPLEMENTATION.md) for file ownership, allocation
accounting, private glyph IDs and the handoff seams. Public F0 headers, kernel,
loader, ui_text.c, Scribe and gterm are unchanged. The old byte drawing functions
retain their behavior; their bitmap font remains the compatibility source.

## Evidence

- **7,910 checks pass at both -O2 and -O0 with ASan+UBSan**, exercising the actual
  implementation and real F1 engine. The tests include the 17 frozen geometry
  vectors, all 161 W1 compositions, strict decoding, byte preservation, fallback,
  tabs, clipping, caret ties, zero advance, kerning monotonicity, overflow,
  pinned-cache eviction, reload identity, context/backend caps and allocation
  refusal cleanup. Legacy bitmap output is compared against the existing byte
  renderer; missing grid box glyphs intentionally acquire procedural coverage.
- The Unicode generator reproduces the checked-in output from SHA256-pinned
  Unicode 17.0.0 data and license. A modified input was rejected. No host Unicode
  database determines the generated profile. Both FAT and ext2 image recipes
  install the Unicode license.
- The default strict full build passes. `libos64.so` exports the 13 public text
  functions while private `text_*` helpers are hidden. Its dependency is the
  FreeType leaf; that leaf still has one exported symbol, no DT_NEEDED entries,
  no undefined imports, and its `--no-undefined` link rule.
- A fresh graphical QEMU boot renders four real fixture fonts (two TrueType,
  two CFF OpenType), each at 18 and 28 pixels, through the public F2 API. The
  caller's font handles and file buffers are released before drawing. The test
  publishes its specimen, checks BUSY during retention, then releases all runs
  and destroys the context with zero caller-accounted bytes remaining. Peak
  context-accounted allocation is **3,755,042 bytes**. The screenshot was inspected
  for glyphs, accent equivalence, baseline/caret guides and selection positions.
  Guest ext2 filesystems pass offline checks after shutdown; installed library
  bytes and the Unicode license are compared with the built inputs.
- The separate [box atlas](f2-evidence/box-atlas.png) covers all 160 procedural
  scalars at 24x32. Visual inspection caught disconnected double-corner rails;
  the corrected joins and narrow-cell collapse have focused regressions. Atlas
  cells are ordered U+2500..259F, 16 columns, with four-pixel gutters. Pixel tests
  cover joining edges, full/lower blocks, shades and nonempty coverage.

Host commands, from the worktree root:

```sh
python3 tools/fonts/generate_w1.py --check
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py --real --output /tmp/os64-f2-final-o2
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py --real -O 0 --output /tmp/os64-f2-final-o0
make -j4 DISK_IMAGE=/tmp/os64-f2-guest/os64.img \
  EXT2_TEST_IMAGE=/tmp/os64-f2-guest/ext2.img \
  EXT2_STAGING=/tmp/os64-f2-guest/staging
```

LeakSanitizer is disabled for this execution environment; ASan and UBSan remain
enabled. The allocator ledger checks sized frees and zero live storage, including
failure cleanup. Host logs, guest logs, specimen, ELF checks, filesystem checks,
source/artifact hashes and the private-image QEMU driver are in
[f2-evidence](f2-evidence/README.md). These are local QEMU results, not P5 results.

## Non-GUI dependency experiment

Before adding the production dependency, a private A/B build changed only the
libos64 link to add `DT_NEEDED libfreetype.so`. Both variants booted successfully
and passed the real-font text test. `textspawn` uses ordinary libos64 APIs to
spawn `/bin/true`, wait for exit and reap it 50 times per batch. Six batches run;
the first is warmup. This measures the whole spawn/exit/reap cycle, not loader
latency in isolation. QEMU used q35, eight CPUs, 8 GiB and the same disk contents
apart from the dependency-bearing library. The timer is 100 Hz.

| Variant | Measured ticks per 50 cycles | Median elapsed time per cycle |
|---|---|---|
| Without FreeType dependency | 60, 86, 165, 42, 65 | 13.0 ms |
| With FreeType dependency | 118, 52, 86, 160, 74 | 17.2 ms |

Warmup batches were 44 and 153 ticks. The measured ranges overlap substantially
(8.4–33.0 ms versus 10.4–32.0 ms per cycle). The observed median difference is
4.2 ms, about 32%; these samples are too noisy to assign that difference solely
to the dependency or establish a stable overhead. No performance threshold was
specified by F0. The agreed leaf dependency is integrated with this limitation
recorded; **this is not evidence that it has zero cost**. A controlled hardware
measurement remains useful before treating startup cost as settled. No loader
change was introduced to influence the experiment.

The A/B payloads predate the final box-corner and lookup-retry fixes; the pair
isolates the dependency against the same intermediate F2 code. The final guest
run and final ELF receipts identify the completed candidate separately.

## Review and continuation

Fable can review the candidate against [02-text-layout.md](02-text-layout.md),
[FONT_CONTRACTS.md](../../FONT_CONTRACTS.md), the source map and the retained
receipts. Pay particular attention to two-budget retry ownership, caret
monotonicity, fallback/marker source spans and procedural cell geometry.
An independent implementation review remains necessary after design review.

F3 can then consume fixed-cell runs without changing PTY semantics. F4 can use
measured runs and source-boundary operations for widgets and Scribe. F5 owns
font files, role resolution, settings and generation invalidation; none of those
product flows is implemented here. This package does not enable a font picker,
change the desktop font or make Scribe a proportional-font editor by itself.
