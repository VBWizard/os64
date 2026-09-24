# Fonts and shared text layout

Frozen F0 contract: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Design record, 2026-09-16. Based on merged `userland` at `3b82356` (PR #109).
Chris approved pursuing FreeType and proportional text after the feasibility
investigation and selected UTF-8 Western-language text and common symbols for
the first release. The implementation contracts have R3 design approval; the
reviewed F1 backend is integrated at `f065c7c`. The F2 implementation candidate
and its host/guest evidence are recorded in [F2-REPORT.md](docs/fonts/F2-REPORT.md),
with a [source map](docs/fonts/F2-IMPLEMENTATION.md). Fable's
[F2 implementation acceptance](docs/fonts/FABLE-REVIEW-F2.md) is recorded at
`4b0a839`. [F2.5](FONT_PROVIDER.md) fixes the shared provider/adoption boundary;
toolkit migration remains a separate package. Kernel console and window-decoration
integration are separate work.

## Decision status and collaboration

Settled product scope: installable standard outline fonts, independent font
roles, proportional and monospace rendering, preservation of Scribe's original
file bytes, and UTF-8 Western text/symbols before complex-script shaping.
Chris also requires explicit module boundaries and documentation that permit
parts of the feature to be handed to another implementer without conversation
history. These requirements bind the work packages.

The engine dependency direction, concrete C interfaces, exact Unicode support
profile, limits, and configuration/live-Apply semantics are specified in the
[F0 R3 contracts](FONT_CONTRACTS.md); their baseline is recorded in
[F0-FREEZE.md](docs/fonts/F0-FREEZE.md). The six F1 questions have dispositions in
[F0-F1-DECISIONS.md](docs/fonts/F0-F1-DECISIONS.md); the
[Fable R3 verdict](docs/fonts/FABLE-REVIEW-R3.md) approves all three design gates.
F1 has passed its separate implementation review with preserved host/guest evidence
in [F1-IMPLEMENTATION-REVIEW.md](docs/fonts/F1-IMPLEMENTATION-REVIEW.md). Task owners use the recorded
contract baseline and must not invent competing contracts.

[FONTS_WORK_PLAN.md](FONTS_WORK_PLAN.md) defines ownership, dependencies,
integration checkpoints, and the work packets in [docs/fonts](docs/fonts/README.md).
This file is the product/architecture authority. The work plan describes delivery;
packets describe bounded assignments. The approved headers and their contract tests
define the implementable interfaces. A change to one of those interfaces
must update the affected design text and packets together.

## Product direction

Install standard outline `.ttf` and `.otf` files without rebuilding applications.
Offer independent interface, terminal, and document font choices. Terminal fonts
retain a regular cell grid; Scribe can use proportional or monospace text without
changing its plain-text file format. Keep the existing bitmap font as a fallback.

`fonts.conf`, resolved through the existing configuration ladder, names the
chosen faces and sizes. A settings application can edit that same configuration;
the file and its documented semantics must work without the settings application.
Do not encode a different font-selection policy in each consumer.

Proposed configuration, with pixel sizes explicit rather than an invented DPI:

```text
ui.face = fonts/DefaultSans-Regular.ttf
ui.size = 16
terminal.face = fonts/DefaultMono-Regular.ttf
terminal.size = 16
document.face = fonts/DefaultSans-Regular.ttf
document.size = 18
```

Names above illustrate the format, not selected font families. The config file
uses the configuration ladder. Relative font paths resolve beside that selected
config file; absolute paths name that file directly. The existing resolver
accepts filenames, not slash-containing font paths. F0 R3 documents this
correction and how Save preserves the resolved meaning of relative choices.
Resolve and retain the loaded font contents so replacing a pathname does not
silently mutate an existing face. File names identify installed assets, while
family/style metadata supplies readable labels. Collection face indices and
font-variation controls need explicit syntax before they become supported input.
An absent configuration uses compiled defaults. F0 R3 requires rejecting a
malformed candidate as a whole, with a diagnostic and a usable previous/default
font; see its configuration section for the precise startup/live rules.

## Fable feedback reconciled with the probe

Adopt the measurement interface before the rasterizer integration. FreeType
supplies glyph images and metrics; os64 owns selection, measuring text runs,
line layout, caching, fallback, and application policy. A width helper alone
cannot specify caret placement or account for both advance and painted bounds.

The proposed engine is a separate `/lib/libfreetype.so`, with pinned upstream
sources, build configuration, license records, and a source-selection review
modeled on `userland/libtls/UPSTREAM_REVIEW.md`. Confirm the final release pin
and its relevant fixes before importing it. The investigated version was 2.14.3.
Choose the BSD-style FreeType License, retaining its notices and required credit.
Bundled font files have their own license records.

The useful outline-font module set includes base, SFNT, TrueType, CFF, smooth,
and auto-hinting, plus CFF's `psaux`, `pshinter`, and `psnames` dependencies.
The standalone Type 1 and CID drivers can be omitted without removing the helpers
CFF needs. Omit PFR, Windows FON, BDF, PCF, compression wrappers, and the upstream
cache module from this initial configuration. Include the monochrome rasterizer
only if the supported rendering modes require it; the probe included it.

Memory faces avoid stdio: os64 owns a bounded font-file buffer for the lifetime
of the face and uses `FT_New_Memory_Face`. Allocator callbacks provide a natural
place for allocation accounting and failure injection.

The probe's unresolved runtime helpers were malloc/realloc/free, memchr/memcmp/
memcpy/memmove/memset, qsort, strcat/strcmp/strcpy/strlen/strncmp/strncpy/strrchr/
strstr. libos64 provides some equivalent operations, with different names and
occasionally different semantics; boolean `os64_streq` is not `strcmp`.
Build the adapter from the compiled dependency inventory rather than assuming
that allocation, sorting, and nonlocal returns exhaust the work.

A general public setjmp/longjmp API is not required merely to import FreeType.
The isolated probe used GCC's nonlocal-return builtins, as the existing JPEG
port does privately. That is a candidate private mechanism, not a conforming
replacement for a public C setjmp ABI. It needs malformed-char-map and cleanup
regressions at optimized settings. If a reusable assembly implementation is
chosen instead, specify its saved registers, compiler annotations, return-value
semantics, stack lifetime, and same-thread restrictions, then test them. Its
small assembly body does not make those contracts disappear.

## Library dependency direction

Today libdraw and libui are source components of `libos64.so`, not independent
shared libraries. Linking libos64 to a new library that itself imports libos64
would create a dependency cycle. The loader has cycle handling, but it is not
a reason to introduce this architecture accidentally.

Preferred draft: keep public text/layout integration in libos64 and place the
FreeType engine below it, using explicit allocator callbacks and a private
runtime shim. The engine must not call back into GUI code or perform file I/O.
A narrow os64-owned backend API can hide FreeType handles and version-specific
structs. Text layout owns the engine instance and its face lifetimes. Compare
this with a separately factored UI/text library before freezing the build graph;
that alternative is a larger migration. Measure the dependency's effect on
non-GUI processes rather than assuming that a shared object is free to load.
No kernel or loader changes are proposed by this draft.

## Shared layout contract

The unit of measurement is a positioned text run, not a byte count multiplied
by a font width. Both the existing bitmap backend and the FreeType backend must
produce the same kind of result. A convenience `text_width(face, text)` derives
its answer from that result rather than implementing separate spacing rules.

A run specifies:

- The exact input bytes and encoding policy, a retained font instance, size,
  render/hinting mode, and tab origin/interval.
- Positioned glyphs with resolved face identity, glyph index, baseline-relative
  positions, advance, and visible ink bounds. Advance and ink bounds differ:
  spaces advance without ink; italic glyphs can overhang their advances.
- Total logical advance, ink bounds, ascent/descent, and line spacing.
- Mappings between source byte ranges, legal caret stops, and pixel positions.
  Preserve room for multi-codepoint clusters and multiple glyphs per cluster so
  a later shaping backend does not require replacing the editor-facing API.

Painting, width measurement, mouse hit-testing, selection rectangles, caret
position, and scrolling consume this same result. Fit-to-width and ellipsis
must also use its positions. Hit-testing chooses an insertion boundary using
caret positions, not glyph ink boxes. A line's beginning and end remain valid
stops even when its visible ink is empty. Layout failures are reported, not
returned as a zero width that looks like successful empty text.

Use bounded fixed-point positions internally and defined pixel rounding at the
surface boundary. Keep fractional accumulation consistent between measuring
and drawing. Kerning changes the relationship between adjacent glyphs, so
summing independently rounded glyph widths is insufficient. FreeType 2.14.3's
optional basic GPOS pair-kerning support can supplement the legacy kern table;
full contextual shaping still needs a higher-level engine.

Draw selection and background regions separately from glyph masks; opaque
per-glyph cells would erase overhangs and adjacent kerned glyphs. The first
outline renderer uses grayscale coverage with clipping into existing surfaces.
LCD subpixel assumptions are unnecessary for the initial portable renderer.

Cache keys include resolved font-content identity/face index, size, glyph index,
and rendering configuration. Variation coordinates or transforms must join the
key if supported. Do not key by pathname and size alone. Cache immutable rendered
glyphs under a memory budget; own mutable FreeType face/size state per context
or serialize access. Eviction must not invalidate a glyph retained by a paint
operation. Shape/layout caches also depend on text and layout settings.

Fallback is resolved before measuring, and drawing uses that same resolved
choice. Bound the fallback list and finish with a visible missing-glyph marker.
For a terminal, replacement glyphs retain the selected cell advance. Preserve
terminal line/block graphics explicitly; do not silently substitute proportional
advances into a grid. Specify fallback baseline alignment and line-height policy
before integrating it with editable text.

## Consumers and migration

1. Define and test the run contract with the bitmap backend. Preserve the legacy
   byte/charset drawing APIs for their current consumers while migrating users
   deliberately. Test agreement between measurement, draw positions and hit tests.
2. Import and adapt FreeType with a standalone guest specimen rendering installed
   monospace and proportional outline fonts. Check source pin, malformed input,
   allocation failure, cleanup, clipping, and bounded cache behavior.
3. Use selected font cell metrics throughout gterm. Derive its
   rows and columns from the drawable area, use the existing `os64_pty_resize`
   operation, and update pointer selection/cursor geometry together. Preserve
   the PTY's byte plus charset contract; this slice does not add a Unicode PTY.
4. Migrate widget labels, buttons, list rows, fields, and menus to text metrics.
   Revisit layout minima and text insets together. A larger font at 1024x768 must
   have an intentional scroll/reflow behavior instead of clipping settings away.
5. Migrate Scribe through the shared textview/textfield implementation. Horizontal
   scrolling and widest-line tracking become pixel-based. Up/Down remembers a
   preferred pixel X. Tabs use defined pixel stops; document positions remain
   byte offsets into the original buffer. Proportional text does not require
   adding soft wrapping, which is a separate editor feature.
6. Add the font settings UI, persistence, and live changes on top of the same
   configuration and face-resolution rules. Extend appearance publication only
   after font identity and relayout semantics are settled. Existing session
   payloads exclude geometry, and the schema fixes font.w/font.h at 8/16: those
   fields are compatibility constraints, not ready-made scalable-font controls.

A live font change must acquire a usable replacement before dropping the old
instance, invalidate affected measurements, recompute layout and scroll ranges,
and retain document contents, selection, and valid focus. Failure retains the
previous usable font and reports the problem. Independent contexts keep their
own role choices. How Fonts Apply/Save interacts with the Workshop composition
is an explicit design decision; this draft does not silently expand its schema.

## Text-language scope: selected 2026-09-16

Chris selected UTF-8 Western-language text and common symbols first, with
complex-script shaping as a later slice. Full multilingual shaping is not a
prerequisite for this release. The text-run boundary must still accommodate
clusters and positioned glyphs so that extending the language scope does not
replace the editor-facing interface.

The narrower scope still needs a precise supported repertoire and editing
contract before code starts. Existing UTF-8 helpers do not make byte-wise cursor
movement correct. Preserve input bytes on load/save, visibly represent invalid
sequences without silently repairing the file, and prevent movement/deletion
inside a supported UTF-8 sequence. Combining marks, grapheme boundaries,
normalization, and unsupported scripts need explicit behavior; do not claim
complete Unicode editing from codepoint iteration alone. No automatic
normalization or encoding conversion is proposed.

The contract package must publish a concrete support profile and examples:
ASCII; accented Latin in precomposed and decomposed input; common punctuation,
currency and symbol characters; malformed byte sequences; and unsupported
sequences. Accented Western text must not be quietly redefined to mean ASCII
or precomposed characters alone. Specify which combining sequences receive
correct placement and cluster editing, and how others are visibly represented.
This is a design obligation, not a claim that the current renderer supports them.
Terminal byte/charset compatibility remains distinct from Scribe's UTF-8 policy.

## Verification and research evidence

The isolated probe cross-compiled 17 translation units of FreeType 2.14.3 using
os64's x86_64-elf toolchain, -O2, PIC, no red zone, Intel assembly syntax, and
-Wall -Wextra -Werror. Its combined object sections total 499143 bytes, excluding
font files, runtime adapters, ELF packaging, and glyph caches. Linked against a
Linux host runtime, those objects rendered DejaVu Sans and Sans Mono TrueType
faces and Nimbus Sans OpenType/CFF at 16/24/32 pixels. The corresponding 16-pixel
i/W advances were 4/16, 10/10, and 4/15. These are host feasibility results, not
an os64 runtime or hardware-validation claim.

Implementation acceptance includes host regressions for run geometry, caret
round trips, selection, tabs, fallback, invalid bytes, large/empty lines, and
allocation failures; strict cross builds and dependency/export checks; then
QEMU rendering and real editor/terminal interaction. Check changing fonts while
text is selected, scrolled, or being edited. Check terminal resize refusal and
charset/line-drawing behavior. P5 appearance and performance require their own
acceptance run. Fable's design review precedes implementation review for this
cross-cutting feature.

## Primary references

- [FreeType module selection](https://github.com/freetype/freetype/blob/VER-2-14-3/modules.cfg)
- [FreeType customization](https://github.com/freetype/freetype/blob/VER-2-14-3/docs/CUSTOMIZE)
- [FreeType license](https://freetype.org/license.html)
- [Glyph retrieval and kerning](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html)
- [GCC nonlocal-return builtins](https://gcc.gnu.org/onlinedocs/gcc/Nonlocal-Gotos.html)
- [HarfBuzz's role](https://harfbuzz.github.io/what-is-harfbuzz.html)
- Local contracts: `APPEARANCE.md`, `LIBDRAW.md`, `PTY.md`, and
  `userland/libtls/UPSTREAM_REVIEW.md`.

Current architecture verdict: [Fable R3 review](docs/fonts/FABLE-REVIEW-R3.md) approves backend,
layout and configuration contracts with no design findings remaining. The
[freeze record](docs/fonts/F0-FREEZE.md) identifies the baseline for downstream assignments.
F1 implementation review and guest validation are recorded separately; F2 and
consumer acceptance still require their own evidence.
