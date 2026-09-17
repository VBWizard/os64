# Font contracts — F0 R3

2026-09-16, os64 base `3b82356413ab8f183bd6506febe8d06fea6e7de0`.
This record makes the [feature design](FONTS.md) concrete. Fable's
[R3 design review](docs/fonts/FABLE-REVIEW-R3.md) approves backend, layout and
configuration with no design findings remaining. The [freeze record](docs/fonts/F0-FREEZE.md)
identifies the contract baseline. This approval does not establish an implemented
font feature or complete the separate F1 implementation review. The six F1
questions have dispositions in [F0-F1-DECISIONS.md](docs/fonts/F0-F1-DECISIONS.md).

## The first handoff: backend table v1

Give Opus this document and
[`font_backend.h`](userland/libos64/include/os64/font_backend.h). The header is
standalone C11, uses standard integer/size types, and does not require linking
libos64. F1 implements `os64_freetype_backend_v1()`, returning a constant operation
table. F2 accepts an injected table, allowing contract tests to use the fake
backend before F1 arrives. The getter is the intended exported engine entry;
upstream symbols and private helpers remain hidden.

The proposed production graph is `libos64 -> libfreetype`, with no reverse link.
Memory callbacks carry caller context; the backend performs no file/GUI/syscall
operations. F1's test executable can independently link both libraries. F2 will
measure trivial-program spawn latency before/after the added dependency for
non-GUI libos64 consumers before
the production dependency is integrated. No loader or kernel change is implied.

R3 names the review document revision. The backend table remains revision 1 and
the getter remains `_v1`: R3 clarifies semantics without changing declarations,
field layout or enum values. It is not a promise to support mixed-version
installed libraries. The getter's table revision and struct size must match
before use. Record the exact freeze commit after review; do not call the contract
frozen merely because its header compiles. Changes update this record, header,
fixtures and affected work packets together.

## Backend semantics and bounds

- An engine copies the allocator descriptor. The callback context remains alive
  through engine destruction. It is not a global allocator. Track requested
  allocation bytes, including adapter/engine bookkeeping and retained glyphs;
  host allocator overhead and borrowed font bytes are excluded. Zero memory cap
  selects 64 MiB; caps over 128 MiB return BAD_ARGUMENT. An engine-cap refusal returns LIMIT during creation, face opening and
  rendering, without invoking the allocation callback for the refused request.
  A callback returning NULL yields NO_MEMORY. Record the refusal reason per
  operation so a previous cap failure cannot relabel a later callback failure.
  Engine creation must fail if required module initialization runs out of memory;
  it must not return a partially initialized engine. Reallocation
  is implemented privately with failure preserving the original allocation.
- One face handle binds immutable input bytes, nominal pixel height and hinting.
  Changing size creates a new handle; no exposed mutable size switch. Borrowed
  bytes must remain immutable until face_close; F2 owns their retained copy.
  Face/glyph handles belong to the engine that created them. Pointers to foreign,
  destroyed or fabricated handles are caller errors, not safe validation input.
- Inputs are nonempty and at most 32 MiB. At most 64 live faces per engine;
  pixel height 1..256. Enforce these before expensive work. Reject collections,
  variable fonts and fonts lacking a usable Unicode charmap with UNSUPPORTED in
  this profile. Detect `fvar` and `CFF2` tables explicitly: disabling upstream
  variation support also removes flags that might otherwise identify them.
  Accept static SFNT TrueType outlines and OpenType/CFF1 outlines. Select
  a Unicode charmap capable of the broadest supported scalar coverage. Do not
  silently fall back to a symbol/legacy charmap. Reject invalid scalar values
  (surrogates or above U+10FFFF) as BAD_ARGUMENT; unmapped scalars return MISSING.
- Normal hinting uses the selected engine's normal grayscale target. The selected
  FreeType configuration disables the TrueType bytecode interpreter: TrueType
  uses the autofitter, while CFF retains its native Adobe hinter. Do not force
  CFF through the autofitter to make both formats use the same path. NONE disables
  native hinting and autohinting. Identify outline format from parsed content,
  not the filename suffix; equal family names do not imply equal hinted results.
  Use outline rendering rather than embedded bitmap strikes. No LCD/SVG/color
  renderer is enabled. Bitmap/color-only glyphs return UNSUPPORTED; glyph-local
  failure is not a successful empty glyph. F1 records exact upstream flags.
- Metrics and adjustments are signed 26.6 pixels with downward Y. Ascent and
  descent are nonnegative magnitudes; line_height is at least their sum.
  Convert/check upstream values without wrapping. Glyph advance_x is nonnegative
  in this horizontal profile; negative or unrepresentable results return LIMIT.
  Kerning is signed, scaled to the selected size and returned without whole-pixel
  grid fitting in both hint modes (`FT_KERNING_UNFITTED` for F1). Keep the 26.6
  precision supplied by the engine; ordinary fixed-point conversion is not a
  promise of exact real-number arithmetic. F2 accumulates these values before
  surface quantization. NORMAL can have whole-pixel advances and fractional pair
  adjustments; NONE can have fractional advances as well. Both need a fractional pen. Enable the selected pin's basic GPOS pair support
  alongside legacy kern support. FreeType prefers legacy kern when available,
  otherwise uses its supported GPOS subset; do not sum the two sources.
  GPOS coverage is best effort: the selected pin handles PairPos formats 1/2
  and extension lookups but requires first-glyph XAdvance alone (valueFormat1
  0x0004) and no second-glyph value record (valueFormat2 0). Other value layouts
  yield OK/zero through this API, indistinguishable from an absent pair. Zero
  therefore cannot establish that a font contains no positioning information.
- Lookup returns MISSING for glyph index 0. Both render and pair_adjust reject
  zero/out-of-range glyph indices with BAD_ARGUMENT. F2 supplies its own marker
  after fallback is exhausted, so it does not require this API to render .notdef.
- Render returns an owned immutable glyph, with tightly packed top-first 8-bit
  grayscale coverage and integer baseline-relative left/top bearings. Width and
  height are at most 4096; products and translated positions use checked wide
  intermediates. Coverage values are 0..255. Normalize upstream signed pitch and
  reject unexpected pixel modes. `ink` is the raster rectangle, not a promise
  to trim every transparent border. Its edges equal left/top and width/height
  multiplied by 64. Zero-area glyphs use zero dimensions/stride/ink/bearings,
  NULL coverage and retain their advance. Memory caps still apply to masks.
- A glyph owns its image/metrics independently of its source face. Closing that
  face does not invalidate the glyph. The engine stays BUSY until its faces and
  glyphs are released. NULL close/release is harmless; dangling-handle reuse is
  invalid. The view's coverage pointer lives until glyph_release.
- Metadata names are copied UTF-8, NUL-terminated and truncated at scalar
  boundaries with a flag. Prefer Unicode name-table typographic family/subfamily
  (16/17), then legacy family/subfamily (1/2), independently for each field.
  Within a name ID, prefer Microsoft Unicode records over Unicode-platform
  records; within Microsoft prefer en-US 0x0409, and within Unicode-platform
  records prefer language 0, then the first record in table order. The latter
  is a neutral/default preference, not a claim to detect an English translation.
  This is deterministic initial label selection, not locale negotiation.
  Decode UTF-16BE with U+FFFD for unpaired surrogates/odd trailing bytes. NUL ends
  a display name. If no usable Unicode record exists, accept the engine's lossy
  ASCII fallback; remaining non-ASCII bytes become U+FFFD. Do not claim lossless
  legacy-name recovery. Absent names are empty, allowing F5 to show a filename.
  Labels never determine file paths, font identities or cache keys. Fixed-width
  metadata is a hint for terminal suitability checks, not proof of coverage or
  equal metrics.
- Calls on one engine, including child getters/releases, are serialized by the
  caller. Separate engines have independent mutable state. No backend callback
  may reenter its engine. F1 must verify any upstream global configuration used.
- With valid output storage, failures clear outputs. No half-created handle is
  returned; existing objects and allocation accounting remain usable. Missing
  glyph, malformed source, unsupported features, limits and allocation refusal
  are distinct statuses. Destruction of a busy engine makes no change.

These resource limits are deliberate initial policy, not measured worst-case
requirements. Audit feedback can revise them before freeze. They bound memory
requests; they do not establish a hostile-font CPU-time bound.

## Layout, ownership and geometry

[`text.h`](userland/libos64/include/os64/text.h) and
[`text_draw.h`](userland/libos64/include/os64/text_draw.h) declare the F2 target.
They add no implementation to libos64 in F0. Existing byte/charset drawing APIs
keep their behavior; migration is explicit at each caller.

A text context owns a backend engine, immutable copied font bytes, bitmap font
instances and a bounded glyph cache. The 128 MiB default total budget includes
those owned bytes, backend-requested storage, runs and masks (no double counting
when a cache and run reference the same image). Zero context/cache caps select
defaults; a positive cache cap greater than the total cap is BAD_ARGUMENT.
An 8 MiB default cache target controls evictable entries; pinned images count
toward the total cap and can exceed the cache target. Exhausting the total cap
first permits eviction of unpinned cache entries. F2 sets the engine cap to
`min(context_total_cap, OS64_FONT_MEMORY_MAX)`; the context allocator counts
backend bytes once, without adding `engine_stats.live_bytes` again. The context
also counts its own runs/font copies, so its remaining budget can refuse a
callback before the engine cap fires. Track that local budget refusal separately
from an underlying allocator failure. On engine LIMIT or a context-budget refusal,
evict eligible entries and retry the failed operation at most once, and only if
storage was actually released. Other LIMITs still fail after that bounded retry;
an underlying allocator failure returns NO_MEMORY without a retry loop.
No eviction may invalidate a live run. The caller's
allocator and serialization rules match the backend; getters and painting also
require serialization with release on that context. All non-NULL failure outputs
are cleared; destruction returns BUSY with external font/run references.

Each successful font open owns a copy of its source and receives a new nonzero
64-bit identity within the context, even if a pathname or pointer was reused.
Reject identity wrap. Cache keys include that identity, glyph ID, immutable
pixel size and hinting; this renderer has no variable transform/variation state.
Runs retain font instances and images. Releasing the caller's font reference
does not invalidate runs. Externally held runs keep the context alive/BUSY.
The bitmap font instance is fixed at 8x16 and keeps existing charset mappings.

A run owns its input bytes and is one logical line with relative byte offsets.
An empty input accepts a NULL pointer and has one caret at byte 0 / X=0.
Nonempty NULL input, literal LF, invalid options, foreign-context fonts and
duplicate font handles are BAD_ARGUMENT. NUL is a visible control, not a string
terminator. Limit runs to 1 MiB source bytes and 1 MiB positioned glyphs; check
array-size arithmetic and final 26.6 positions. Oversized lines return LIMIT,
not a silently clipped prefix. F4 uses bounded windows around the caret for
oversized lines, preserving the complete document. See the editor packet for
source-offset mapping and explicit continuation indicators.

Fonts are ordered primary then fallbacks (1..8 total); resolution happens before
positions are computed. Layout falls back on MISSING or a glyph-local UNSUPPORTED;
malformed-engine errors fail layout; allocation and limit errors follow the
bounded cache-recovery rule above and fail if recovery cannot complete. Exhausted fallback
uses a synthetic outlined missing marker, width 8 pixels, with bitmap 8x16
metrics; the placement marks font identity 0 and diagnostic glyph ID 0.
Controls/unsupported clusters use that marker too. There is no missing glyph
whose measured advance silently differs from its painted advance.

Baselines align across faces. Run ascent/descent are maxima of participating
face metrics, including marker/bitmap metrics; include the primary even for an
empty run. Line height is max(participating line heights, ascent+descent).
This is the run's content box, not the consumer's row pitch. A consumer fixes
row pitch and baseline from the role's primary face at its selected size (an
empty primary-only run supplies those metrics). Taller fallback/marker ink is
clipped to the row, without changing pitch. F4 takes selection X from the run
and Y from the consumer's row. Ink union excludes empty masks. Logical width includes trailing spaces and tabs.
Glyph placement keeps source cluster spans and separate ink bounds. A run can
represent multiple glyphs for one cluster even though the initial Western accent
strategy below often selects one composed glyph.

Accumulate advances in 26.6; use checked int64 intermediates. For a pair in the
same face, add kerning before placing the right glyph. The boundary before that
glyph is also moved by the pair adjustment. Require strictly increasing carets
for nonempty clusters in this profile; if a pair adjustment would erase/reverse the preceding
cluster's logical interval, suppress that adjustment. Unsupported zero-advance
standalone input is rendered as a marker; a mark within a supported cluster has
no independent caret. This deliberately limited policy avoids ambiguous inverse
hit testing; later shaping can define more elaborate caret rules.

Tabs have no glyph and advance to the next strictly greater stop:
`origin + (floor((pen-origin)/interval)+1)*interval`. Origin is any representable
position, interval is positive, and the result must fit. Tabs/control markers
break kerning adjacency. Scroll position does not change the tab origin. Implement mathematical floor
for negative differences; C integer division alone truncates toward zero.

Caret positions include source start/end and supported cluster boundaries.
Interior byte offsets snap by BEFORE/AFTER; larger-than-length offsets fail.
Hit-testing clamps outside the run and selects the nearest caret, midpoint ties
to the later byte offset. Fit uses logical boundaries, not ink. Selection endpoints
must already be legal boundaries and ordered; its rectangle uses those X values
and the run's line box. Backspace/Delete remove adjacent cluster byte spans;
Up/Down retains a preferred pixel X. No edit rewrites untouched source bytes.

Paint uses retained masks without allocation, relayout or fallback changes.
Quantize a glyph origin once with `floor((position+32)/64)` (negative ties toward
positive infinity); use the same quantization for caret X and outward-round
ink/clipping bounds. Add the integer baseline
using checked wide intermediates. Clip before pointer arithmetic; source-over
blend coverage into opaque XRGB. Selection/background is painted separately.
The caller publishes damage. Empty clips are success; invalid surfaces or
unrepresentable translated positions fail before painting any pixels.

## Western UTF-8 profile W1

W1 is an explicit product subset, not Unicode grapheme/shaping conformance.
Reference data is [Unicode 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/ReadMe.txt),
chosen as a pinned version, not a claim about the latest release. The F2 table
generator must pin `UnicodeData.txt`, its digest/license, and reject unexpected
input rather than depend on a host Python Unicode version. The finite W1
composition table is defined by the rule below; F0 fixture cases are not a
substitute for generating and testing that whole table.

- Supported standalone scalars: U+0020..007E, U+00A0..017F, U+2010..2015,
  U+2018..201F, U+2020..2022, U+2026, U+2030, U+2032..2033, U+2039..203A,
  U+20AC, U+2122, U+2190..2193, U+2212, U+221A, U+221E, U+2260, U+2264..2265,
  U+2500..257F and U+2580..259F. Coverage still depends on the selected/fallback
  fonts. Soft hyphen U+00AD is visibly marked in a single-line editor rather
  than silently hidden. NBSP keeps its glyph advance and one editable cluster.
- A Latin base followed by a maximal U+0300..036F mark sequence is one editing
  cluster. If it is exactly one ASCII Latin letter plus one mark with a canonical
  two-scalar decomposition mapping to a character in U+00C0..017F, select that
  precomposed character for display. Example `65 CC 81` displays like `C3 A9`
  but retains a 3-byte source span and saves the original 3 bytes. This is a
  restricted display substitution, not normalization of the document. A missing
  composed glyph follows normal fallback/marker rules.
- Other Latin-base-plus-mark combinations draw the base followed by one visible
  marker for the whole mark sequence. Resolve the base normally, using a marker
  if that base is missing too. Both placements carry the full cluster byte span;
  there is no caret between them. For `71 CC 81`, display `q` plus a marker,
  with span `[0,3)` on both placements. Unsupported stacked accents use this rule.
  A Latin base means ASCII A-Z/a-z or a letter in U+00C0..017F according to the
  pinned data. An isolated maximal mark sequence is one marker/cluster. A digit,
  punctuation or other non-Latin scalar is a separate cluster; following marks
  form an isolated sequence, so a caret exists between that scalar and the marker.
  Scan long mark sequences without building an unbounded stack. This scope must
  be visible in product docs.
- Decode valid UTF-8 strictly: reject overlong forms, surrogates and values above
  U+10FFFF. An invalid lead/sequence consumes one byte as a diagnostic cluster;
  subsequent bytes are decoded afresh. Thus malformed bytes remain individually
  editable/preservable. Valid out-of-profile scalars get one marker per scalar.
  No bidi reordering, contextual scripts, emoji/ZWJ sequences or general Unicode
  grapheme promises. Controls (including CR and NUL) are visible single units;
  TAB uses stops and LF belongs to the document line model, outside a run.

Legacy LATIN1/CP437 runs consume one byte per unit and preserve the repository's
charset conventions. Positive `cell_advance` selects terminal grid mode, disables
kerning, and uses exactly one cell per byte (TAB/control interpretation is the
PTY's responsibility in grid mode). Replacement marks retain the cell advance.
Glyphs align to each cell origin/baseline and clip to that cell so overhang cannot
erase adjacent cells. Terminal F3 checks selected font fixed-width metadata and
printable ASCII advances for equality/positive integer cell width; fallback may
be clipped to that width. In grid mode, missing U+2500..257F box drawing and
U+2580..259F blocks/shades are generated procedurally at the cell's dimensions:
lines reach the appropriate edges, fractional blocks fill their cell fraction,
and shades use repeating patterns. Do not substitute an 8x16 bitmap fallback for
these missing scalars; it leaves gaps in larger cells. F2 owns this drawing rule,
F3 consumes it, and missing scalars outside these ranges follow normal fallback
and cell-sized marker rules. This does not change the existing console renderer.

## Configuration and invalidation contract for F5

`fonts.conf` follows `os64_conf_find/read` and its existing grammar. The first
file on the ladder wins; keys within that file are last-wins. Unknown keys,
malformed lines, invalid known values, truncated input or failed font loads reject
the candidate as a whole. Startup reports the failure and uses compiled bitmap
defaults; live changes retain the previous usable state. Role choices are
independent means no hidden inheritance, not partial acceptance of a broken file.
One bad line rejects all three roles and the diagnostic identifies the line.

Keys are `<role>.face`, `<role>.size`, `<role>.fallback.1`, `<role>.fallback.2`,
for roles `ui`, `terminal`, `document`. Case-insensitive keys; paths preserve case.
Sizes are decimal nominal pixel heights 8..96. `builtin` means the 8x16 bitmap
face and requires size 16; defaults are builtin/16 for each role until licensed
outline assets are selected. Missing keys use those defaults; absent fallback
slots are skipped, duplicates are rejected. Open fallback outline faces at the
role size, matching the primary; the builtin compatibility face stays 8x16. Compiled bitmap/marker fallback is
implicit after configured faces. Paths are nonempty and at most 255 bytes; the
existing unquoted grammar cannot encode `#` or leading/trailing whitespace in
a filename, and settings must report such an unsupported selection clearly.

Relative paths resolve against the directory of the selected `fonts.conf`,
absolute paths as written. Reject `.`/`..` components rather than making config
assets depend on the process cwd. This corrects the earlier proposal to send
`fonts/name.ttf` through the configuration lookup: the existing resolver accepts
filenames, not slash-containing paths. The config still uses the system ladder;
no new kernel resolver is needed. Saving a config at the top of the ladder must
serialize resolved absolute paths so moving the file does not retarget its fonts.
Discovery can enumerate the selected config's `fonts/` directory plus currently
selected absolute files; richer directories are a separate schema decision.

Prepare all three roles and affected layouts before adoption. Existing runs
remain valid snapshots; new runs use a new role generation. Cache identity is
content-instance identity, never the configuration pathname. Replacing font files
requires explicit reload and new objects. Restore focus/selection/byte offsets,
recompute pixel scroll bounds and clamp views after adoption. Preserve old state
on any prepare failure. F3 must coordinate grid changes with PTY resize success.

Live transport uses the existing opaque `/sys/appearance` payload and generation
compare-and-publish. F5 must implement the following userland envelope contract
in `ui_session` and its consumers before publishing font keys:

- The envelope is bounded text lines. Readers validate the existing `key = value`
  syntax, apply known keys, and ignore syntactically valid unknown keys under a
  dotted namespace. Unknown undotted keys, malformed lines and invalid known
  values still fail. A full snapshot still requires its palette keys. Preserve
  existing comments/blank lines as well as unowned setting lines.
- Writers read the payload at the expected generation, replace only lines owned
  by the requested component, and retain other lines verbatim. Palette owns the
  existing color-key set; treatment owns the existing session treatment metrics;
  fonts owns `fonts.*`. Ownership is a key/schema decision, not a broad prefix
  such as `font.*` that could consume legacy `font.w/h`. Unknown future dotted
  keys are unowned and retained. Remove all duplicate owned keys when replacing
  a component; keep normal last-wins semantics when reading.
- Theme-only snapshots leave font roles at startup settings. `font.w/h` remain
  fixed 8/16 compatibility fields, not selectors. Missing fonts keys follow the
  complete fonts component's defaults; deleting a fonts component restores
  startup choices. Component-only publication requires a usable base snapshot;
  generation zero is materialized from startup values before merging. A full
  repair may replace an invalid envelope, but must be explicit about losing
  unparseable settings; ordinary component Apply refuses an invalid base.
- Validate the full merged envelope against the 4096-byte payload cap, including
  preserved future keys/comments. Do not truncate paths or drop unowned lines to
  fit. Validate the complete snapshot, then compare-and-publish with the generation
  that was read. A conflict requires a fresh read and merge or an explicit
  conflict result, never overwriting intervening component changes.
- Rollout requires a reboot after refreshing to the first compatible libos64,
  before publishing `fonts.*`. Existing processes retain the old libos64 after
  refresh, whose strict decoder rejects those keys and whose writer drops them.
  Every appearance participant must run the compatible library. The reboot is
  the deployment boundary; no kernel change or mixed-old/new promise is implied.
  Future extensions can use the tolerant reader/preserving writer rule.

Save uses existing checked config replacement and preserves the complete usable
session envelope before changing startup disk choices. Apply and Save remain
separate actions. F5 must test reader tolerance, verbatim preservation, competing
component writers, complete-envelope size limits and the reboot rollout. These
are required implementation changes; today's ui_session/theme code does not
already provide this compatibility. See [APPEARANCE.md](APPEARANCE.md).

Per-process preparation can fail after publication (e.g. memory pressure): that
process retains its old usable generation and exposes a retryable failure. The
Workshop reports publication, not “applied everywhere”; processes do not repaint
atomically. The R3 review approves this protocol as a contract; F5 implementation and its
named compatibility tests remain required.

## Call flows and verification gates

Label: create context -> open selected font -> layout caption with fallbacks ->
read advance/line metrics -> place control -> draw run -> release run/font ->
destroy context. The caption's width and drawing come from the same run.

Editor: layout an immutable copy of the visible line -> retain run with document
revision -> hit/caret/selection operations return original byte offsets -> edit
the document -> discard/rebuild affected runs. A stale document revision must
not apply an old hit result to a new buffer. Save uses the original document
buffer. Scroll/reflow/font changes preserve byte positions and rebuild geometry.

Terminal: decode each PTY byte using its stored charset -> layout fixed-cell spans
with selected mono metrics -> paint with cell clipping -> compute candidate
grid -> ask existing PTY resize -> adopt metrics/grid together on success. A
refused resize keeps the old font/grid. No UTF-8 PTY ABI is implied.

F0's fake backend/fixtures live under `tools/fonts/`. They exercise allocation
failure, owned masks, distinct contexts and awkward glyph metrics. Golden line
fixtures record input bytes, resolved glyphs, positions, ink, carets, hits and
selection. They are independently specified acceptance inputs for F2; a fixture
consistency check is not proof that an unimplemented layout engine passes them.
Cross-compile headers and the fake backend with target flags as well as running
host ASan/UBSan tests. No QEMU rendering claim belongs to this declaration-only
slice. F1 and F2 must later pass real guest acceptance.

Freeze checklist: the six F1 questions have written dispositions; a consumer
review verifies lifetimes/error/caret contracts; Fable reviews the architecture;
fixture discrepancies and the live-envelope compatibility question are resolved;
record the exact contract commit in affected handoffs. Backend-only freeze may
precede configuration/layout freeze so F1 need not wait for downstream policy.
