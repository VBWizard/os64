/* os64_ftoption.h — which FreeType this is.
 *
 * Upstream's defaults are a general-purpose font library: every container it
 * has ever parsed, every renderer it has ever shipped. os64 wants a much
 * narrower thing — scalable outlines from `.ttf` and `.otf` files, rendered
 * to grayscale coverage — and every parser beyond that is attack surface
 * carried for nothing. So this file starts from upstream's list and then
 * says, one switch at a time, what os64 is NOT asking for. Each `#undef`
 * below deletes a parser, a renderer, or a dependency.
 *
 * The argument for each choice, and the exclusions' cost, is in
 * UPSTREAM_REVIEW.md. What belongs HERE is the reason a reader needs while
 * looking at the switch.
 */

#ifndef OS64_FTOPTION_H_
#define OS64_FTOPTION_H_

#include <freetype/config/ftoption.h>


/* ── what the library may reach for ────────────────────────────────────── */

/* A font engine that reads the environment is a channel into the rasterizer
 * that nothing asked for, and os64's kernel-side readers have no environment
 * at all. Rendering configuration arrives as arguments, through the backend's
 * own calls. */
#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES

/* NO FILE I/O, EVER. A face is bytes the caller owns and keeps alive; the
 * engine never opens anything. This is the switch that makes that structural
 * rather than a promise — `FT_New_Face` and the stream machinery are not
 * compiled at all, so an accidental call is a compile error, and the
 * dependency inventory can carry no `fopen`. */
#define FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT

/* Upstream's inline assembly is AT&T syntax; os64 compiles `-masm=intel`
 * project-wide. Not a preference — the two cannot share a translation unit,
 * so the portable C paths are the only ones that build here. */
#define FT_CONFIG_OPTION_NO_ASSEMBLER

/* No compression wrappers. A compressed font container would drag in an
 * inflate implementation and a second parser between the file and the
 * outline; os64 installs plain `.ttf`/`.otf` files. */
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB

/* No legacy Macintosh containers: resource forks, `FOND` resources and the
 * data-fork guessing that goes with them are 1990s interop os64 has no
 * consumer for. */
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK

/* No SVG glyph documents — that is an XML parser and a renderer behind a
 * font file. No incremental faces: that interface exists for PostScript
 * printer drivers feeding glyphs in on demand. */
#undef FT_CONFIG_OPTION_SVG
#undef FT_CONFIG_OPTION_INCREMENTAL

/* Error strings cost a few kilobytes of table and let the backend report an
 * upstream failure BY NAME instead of as a number nobody can look up from
 * inside os64. Worth it: "invalid table" in a fixture's output is a lead,
 * `error 0x8E` is a homework assignment. */
#define FT_CONFIG_OPTION_ERROR_STRINGS


/* ── the SFNT container: outlines only ─────────────────────────────────── */

/* Embedded bitmap strikes (`EBDT`/`CBDT`/`sbix`) are a second image format
 * inside the font, and `sbix` in particular hands the engine PNG data. os64
 * asked FreeType for outlines; the bitmap font it already has is its
 * fallback. */
#undef TT_CONFIG_OPTION_EMBEDDED_BITMAPS

/* Colour layers (`COLR`/`CPAL`) paint one glyph as several coloured ones.
 * The backend's output is a single coverage mask, so a colour glyph could
 * not be represented even if it parsed. Colour-only glyphs report as
 * unsupported instead. */
#undef TT_CONFIG_OPTION_COLOR_LAYERS

/* The `BDF` property table inside an SFNT answers a question this backend
 * never asks: it maps Unicode scalars to glyph indices through the character
 * map and stops there. */
#undef TT_CONFIG_OPTION_BDF

/* TT_CONFIG_OPTION_POSTSCRIPT_NAMES — the `post` table's glyph names — LOOKS
 * equally unwanted and is kept, because it is not independently switchable.
 * `src/sfnt/ttcmap.c`'s SYNTHETIC UNICODE charmap, which builds a Unicode map
 * out of glyph names for a CFF face that carries no `cmap`, sits under
 * FT_CONFIG_OPTION_POSTSCRIPT_NAMES and calls `tt_face_get_ps_name`, which
 * sits under this one. Turning this off while that is on is a build failure,
 * not a smaller library. It is worth having anyway: a synthesized Unicode
 * charmap is the difference between serving such a face and refusing it. */

/* Font variations (`fvar`/`gvar`/`HVAR`/`avar`) are out of scope: a variable
 * font's instance is part of a face's IDENTITY, so supporting it means
 * variation coordinates in the configuration syntax, in the backend contract
 * and in every cache key.
 *
 * AND TURNING IT OFF IS NOT THE SAME AS REFUSING SUCH A FONT. Without this
 * option FreeType does not set FT_FACE_FLAG_MULTIPLE_MASTERS either, so a
 * variable font would open quietly at its default instance and answer to a
 * cache key that does not describe it. port/backend.c refuses one by probing
 * for the `fvar` and `CFF2` tables itself — which is why that probe is not
 * the redundant belt-and-braces it looks like. */
#undef TT_CONFIG_OPTION_GX_VAR_SUPPORT

/* THE TRUETYPE BYTECODE INTERPRETER IS OFF, so the autofitter hints TRUETYPE
 * faces. It is a stack virtual machine that runs instructions out of the font
 * file — the largest single piece of parser surface in the library, and the
 * one upstream fixes most often — and a face's own hinting intent at small
 * sizes is what the autofitter gives up in exchange.
 *
 * THIS SWITCH DOES NOT REACH OPENTYPE/CFF. The CFF driver advertises
 * `FT_MODULE_DRIVER_HAS_HINTER` unconditionally and defaults to the Adobe
 * hinting engine in psaux/pshinter, so a `.otf` is natively hinted whichever
 * way this is set; `FT_Load_Glyph` reaches for the autofitter only when a
 * driver has no hinter of its own. The configuration therefore runs TWO
 * hinters, split by format — as it would with the interpreter on, so
 * consistency is not what turning it off buys. Parser surface is.
 *
 * UPSTREAM_REVIEW.md carries the argument and what would reverse it. */
#undef TT_CONFIG_OPTION_BYTECODE_INTERPRETER
#undef TT_CONFIG_OPTION_SUBPIXEL_HINTING

/* Pair kerning from `GPOS` as well as the legacy `kern` table. This is
 * PAIRS ONLY and upstream says so: it is not shaping, and it does not make
 * contextual positioning work. The backend's kerning call is the consumer. */
#define TT_CONFIG_OPTION_GPOS_KERNING


/* ── the autofitter: Western text ──────────────────────────────────────── */

/* The autofitter carries a hinting module per writing system. os64's first
 * language scope is Western text and common symbols, which the Latin module
 * covers — including Greek and Cyrillic, which share its stem and blue-zone
 * model. A glyph outside any compiled module is scaled without hinting
 * rather than refused, so removing these narrows quality, never coverage. */
#undef AF_CONFIG_OPTION_CJK
#undef AF_CONFIG_OPTION_INDIC

#endif /* OS64_FTOPTION_H_ */
