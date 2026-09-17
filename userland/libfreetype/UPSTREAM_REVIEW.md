# Upstream pin review — FreeType

Reviewed 2026-09-16 for the os64 font backend (work packet F1). This is a
SOURCE SELECTION AND PORT REVIEW, not an independent security audit of
FreeType and not a claim that malformed fonts have been exhaustively
explored. What it records is: which bytes were imported, how that can be
re-derived, which upstream fixes were considered and why each was taken or
left, what the configuration turns off, and what remains unproven.

## The pin

| | |
|---|---|
| Release | 2.14.3, 2026-03-22 |
| Tag | `VER-2-14-3` |
| Tag object | `c740f0fda4274d6ffd2e5b64a25b06ef69803a07` |
| **Commit** | **`0a0221a1347e2f1e07c395263540026e9a0aa7c7`** |
| Archive | `freetype-2.14.3.tar.xz` |
| Archive SHA-256 | `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f` |
| Primary source | <https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz> |
| Mirror | <https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.xz> |

**THE TAG OBJECT AND THE COMMIT ARE DIFFERENT SHAs, and telling them apart
matters.** `VER-2-14-3` is a signed ANNOTATED tag, so GitHub's generated
tarball is named after the TAG OBJECT (`freetype-freetype-c740f0f/`). An
earlier feasibility probe recorded `c740f0f` as "the commit"; it is not one.
The revision this import claims is `0a0221a…`, the commit the tag points at.
`tools/import_freetype.py` records both.

### What was checked about the bytes

- **Two independent mirrors served identical bytes.** Savannah (the GNU host)
  and SourceForge both returned 2,670,220 bytes with the SHA-256 above.
- **The release is signed, and the signature verifies.** `gpg --verify` on
  `freetype-2.14.3.tar.xz.sig` gives a good signature from RSA key
  `E306 7470 7856 409F F194 8010 BE6C 3AAC 63AD 8E3F`, "Werner Lemberg
  <wl@gnu.org>" — FreeType's maintainer, and the same name that signed the
  git tag (with a different, DSA key: `58E0 C111 E39F 5408 C5D3 EC76 C1A6
  0EAC E707 FDA5`). The key was fetched from `keyserver.ubuntu.com` and is
  not in any local web of trust, so this is trust-on-first-use: it proves
  the archive was signed by whoever holds that key, and the mirror agreement
  above is the independent half.
- **The release archive's `src/` and `include/` are byte-identical to the
  tagged git tree**, checked by diffing against the GitHub tarball for the
  same tag. The only difference is `src/dlg/` and `include/dlg/`, the
  bundled `dlg` logging library, which the git tree carries as a submodule
  and which this port does not import or build.

### Reproducing the import

```sh
tools/import_freetype.py --verify     # does the tree match manifest.json?
tools/import_freetype.py --import     # re-derive it from the release archive
```

`--import` downloads the archive, refuses to continue unless the digest
matches, copies the selected directories, applies `patches/*.patch` in order,
and rewrites `manifest.json` with a SHA-256 for every one of the 334 imported
files. `--verify` re-hashes the working tree against that manifest, so an
accidental edit inside `upstream/` is one command away from being seen.

## What is imported

Whole source directories, not hand-picked files, because upstream's supported
single-object build compiles one amalgamation per module (`src/cff/cff.c`
includes every other file in `src/cff/`) and the `#ifdef`s inside those files
are what the configuration actually steers.

| Imported | Why |
|---|---|
| `include/` (minus `dlg/`) | the public and internal headers |
| `src/autofit/` | the hinter TrueType faces get here |
| `src/base/` | core objects, calculation, outlines, streams |
| `src/cff/` | OpenType/CFF outlines |
| `src/psaux/`, `src/pshinter/`, `src/psnames/` | required by CFF — charstrings, its native hinter, charset names |
| `src/sfnt/` | the container TrueType and OpenType share |
| `src/smooth/` | the grayscale rasterizer |
| `src/truetype/` | TrueType outlines |
| `LICENSE.TXT`, `docs/FTL.TXT`, `README`, `docs/CHANGES`, `docs/CUSTOMIZE`, `docs/INSTALL.ANY`, `modules.cfg` | the licence os64 exercises and the documents this review cites |

Eleven translation units are compiled (`sources.mk`), plus four of os64's own.
`src/base/ftsystem.c` is imported but NOT compiled: `port/ftsystem.c` replaces
it, which is upstream's documented arrangement (`docs/CUSTOMIZE` §III).
`base/ftbbox.c`, `ftglyph.c`, `ftbitmap.c` and `ftmm.c` are not compiled
either — they are entry points the backend does not call, and an unused
parser is still a parser.

## Upstream fixes since the tag

At review time `master` was **107 commits ahead** of the tagged commit, with
no newer release. Every one was read; the ones landing in code this
configuration compiles are classified below. There are no published CVE
identifiers for any of them in the repository, and `docs/CHANGES` for 2.14.3
says only "a bunch of potential security problems have been found".

Pinning `master` was rejected: it is unreleased, and it carries two whole new
parsers (`VARC` variable composites, `HVF` hierarchical variable fonts) that
would be new attack surface accepted to get fixes for old surface.

### Backported — 11 patches, in `patches/`

All are memory-safety or integer-overflow fixes in modules this build
compiles. Each applies cleanly to the pin, in upstream order, and each patch
file is upstream's own `.patch` with its commit SHA in the header.

| Patch | Upstream | What it fixes |
|---|---|---|
| 0001 | `1f705ff` | `psaux/psstack.c` `cf2_stack_setReal`: off-by-one in an index check |
| 0002 | `1ec3406` | `psaux/psstack.c` `cf2_stack_roll`: operated on the wrong end of the stack |
| 0003 | `df2fb11` | `psaux/psintrp.c` `cf2_doBlend`: bounds check that accounts for all operands |
| 0004 | `968af86` | `psaux/psintrp.c`: off-by-one in the charstring subroutine nesting limit |
| 0005 | `342e580` | `cff/cffload.c` `cff_index_get_name`: `memcpy` with a NULL argument |
| 0006 | `675a94f` | `autofit/aflatin.c`: signed integer overflow |
| 0007 | `ef54557` | `autofit/aflatin.c`: signed integer overflow (second site) |
| 0008 | `f901085` | `autofit/afgsub.c` `af_validate_coverage`: last element unchecked |
| 0009 | `27229dc` | `autofit/afgsub.c` `af_validate_coverage`: reject overlapping ranges |
| 0010 | `b08a2eb` | `autofit/afgsub.c` `af_validate_coverage`: unsigned counters |
| 0011 | `0db9eca` | `truetype/ttgload.c` `TT_Load_Simple_Glyph`: unsigned point counts |

The first four are the Adobe CFF charstring interpreter, which runs a stack
machine out of every `.otf` file this backend opens; they are the most
directly reachable of the set.

### Not backported, and why

| Upstream | Reason |
|---|---|
| `f01dec5` `[base] Prevent signed overflow` | **Does not apply to the pin.** It patches a REWRITTEN `FT_Outline_Get_Orientation` (`f031395`, also post-tag). The pinned version already reduces its range by shifting and accumulates through the saturating `ADD_LONG`/`MUL_LONG`. |
| `ee38c68` `(gray_raster_render): Fix a coding typo` | **Not present in the pin.** The `min_ey`/`min_ex` typo is in the dynamic-pool rasterizer added post-tag (`a39ef76`); 2.14.3 uses a fixed stack pool and has no such expression. |
| `09b51e9` `Bump rendering limits` | RELAXES a bound (10 → 16 × ppem) to accommodate decorative glyphs. Not a fix; the tighter limit is the conservative choice and nothing here needs the looser one. |
| `cbe1276` `Protect FT_LOAD_COLOR` | Unreachable: colour layers are compiled out and the backend never sets `FT_LOAD_COLOR`. |
| `b6bcd21`, `9a4751a`, `2c041d3` (`FT_Bitmap_Blend`) | `ftbitmap.c` is not compiled. |
| `12f5eb3` (TrueType GS leak), `287206a` (`Ins_WCVTF` CVT copy-on-write), `1803559`/`7d600a0`/`6d0ae3a` (`Ins_SHZ`), `7974be7` (`Ins_IUP`) | All in `ttinterp.c`, the bytecode interpreter, which is compiled out entirely. |
| `0d45c7f`, `6d9fc45`, `5178bda` (GX variations, HVAR/VVAR) | Variation support is compiled out. |
| `867c296` (`sbix`), `b6c6934` (`ttbdf` OOB read) | Embedded bitmaps and the SFNT BDF table are compiled out. |
| `3221895` (LCD clip box) | Subpixel rendering is not enabled. |
| `50ef529` (autofit pointer wrap-around) | 32-bit only; os64 is 64-bit. |
| `e86492e` (pfr), `a3cb585` (winfonts), `f3ca71c` (cid), `ca53609` (pcf), `0d6de69` (sdf), `e8c8c6477` (otvalid), `656cb77` (WOFF), `5c79d6c` (ftstroke) | Modules this build does not compile. |
| the `ttvarc`/`hvf` series, `d939d15`, `0c9b8e9` and their follow-ups | New features added after the tag; not present in the pin. |

**Six of the twelve skipped safety fixes are skipped because the bytecode
interpreter is off.** That is the single largest reduction this configuration
buys, and the argument for it is below.

### Residual risk, stated plainly

- Fixes upstream has not found are not covered by anything here.
- Backporting selectively means this tree is not any upstream revision. The
  eleven patches are recorded with their SHAs so a future bump can drop them
  and take the release that contains them.
- The 107-commit review was a read of subjects and diffs against the enabled
  module list. A fix whose subject does not say what it touches could have
  been miscategorised.
- **Allocation is bounded; CPU time is not.** The engine's memory cap is
  enforced at every allocation, and the harness proves the engine survives a
  refusal at each. Nothing here bounds how long a pathological outline can
  spend in the rasterizer. `TT_CONFIG_OPTION_MAX_RUNNABLE_OPCODES` — upstream's
  only work limit — governs the bytecode interpreter, which is off. Malformed
  -input test runs use timeouts for exactly this reason.

## The configuration

`port/os64_ftoption.h` starts from upstream's defaults and removes. Every
switch has its reason beside it there; this is the summary and the
consequences.

**Off — parsers and renderers:** embedded bitmap strikes (`EBDT`/`CBDT`/`sbix`),
colour layers (`COLR`/`CPAL`), SVG glyph documents, the SFNT `BDF` property
table, font variations (`fvar`/`gvar`/`HVAR`/`avar`), the monochrome and SDF
renderers, and every standalone Type 1 / CID / PFR / Windows FON / BDF / PCF
driver.

**Off — the outside world:** file streams
(`FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT`, so `FT_New_Face` is not compiled
and the engine has no way to open anything), environment properties, zlib and
LZW compression wrappers, legacy Macintosh containers and resource-fork
guessing, the incremental-face interface, and upstream's inline assembly.

**On:** SFNT + TrueType + CFF outlines, the smooth grayscale rasterizer, the
autofitter's Latin module (which hints TrueType), the three PostScript
helpers CFF requires (`pshinter` among them, which hints CFF), GPOS pair
kerning alongside the legacy `kern` table, and error strings.

### Three findings that shaped the configuration

**1. `FT_CONFIG_OPTION_NO_ASSEMBLER` is required, not preferred.** Upstream's
inline assembly for `FT_MulFix` is AT&T syntax; os64 compiles `-masm=intel`
project-wide. The two cannot share a translation unit, so the portable C
paths are the only ones that build here at all.

**2. `TT_CONFIG_OPTION_POSTSCRIPT_NAMES` cannot be switched off
independently in 2.14.3.** `src/sfnt/ttcmap.c`'s SYNTHETIC UNICODE charmap —
the one that builds a Unicode map out of glyph names for a CFF face carrying
no `cmap` — is guarded by `FT_CONFIG_OPTION_POSTSCRIPT_NAMES` and calls
`tt_face_get_ps_name`, which is guarded by `TT_CONFIG_OPTION_POSTSCRIPT_NAMES`.
Disabling the second while the first is on is a build failure, not a smaller
library. It is left ON, which is the better answer anyway: a synthesised
Unicode charmap is the difference between serving such a face and refusing
it, and this backend refuses any face without one.

**3. GPOS pair kerning is narrower than "GPOS kerning" sounds.** With
`TT_CONFIG_OPTION_GPOS_KERNING` enabled, `src/sfnt/ttgpos.c` accepts a
`PairPos` subtable only when `valueFormat1 == 0x0004` and `valueFormat2 == 0`
— an X advance on the first glyph and nothing on the second — and upstream
says so in a comment ("for the limited purpose of accessing the simplest type
of kerning"). Extension lookups (type 9) ARE followed, and both `PairPos`
formats are read. A font whose kern feature adjusts the second glyph, or
carries any other value record, contributes nothing through this path and
reports zero rather than an error. Both fixture families happen to use the
accepted form. **F2 should treat pair kerning as best-effort coverage, not as
a guarantee**, and neither table's values are ever summed with the other's:
upstream prefers `kern` when present and falls back to GPOS, which is the
precedence the contract asks for.

### Which hinter runs, and why the interpreter is off

**Two hinters run, split by format**, and this is worth stating exactly
because it is easy to state wrongly:

| Format | Hinted by | Affected by the interpreter switch? |
|---|---|---|
| TrueType (`glyf`) | the **autofitter** | yes — it is the reason |
| OpenType/CFF | the **Adobe CFF engine** (`psaux` + `pshinter`) | **no** |

`FT_Load_Glyph` reaches for the autofitter only when the driver has no hinter
of its own (`ftobjs.c`, `!FT_DRIVER_HAS_HINTER( driver )`). With
`TT_CONFIG_OPTION_BYTECODE_INTERPRETER` undefined, `ttdriver.c` sets
`TT_HINTER_FLAG` to 0 and the TrueType driver advertises none — so TrueType
autofits. The CFF driver sets `FT_MODULE_DRIVER_HAS_HINTER` unconditionally
(`cffdrivr.c`) and `cffobjs.c` defaults `hinting_engine` to
`FT_HINTING_ADOBE`, so an `.otf` is natively hinted either way. Turning the
interpreter back on would change the TrueType half and nothing else; it
would still be two hinters.

`OS64_FONT_HINT_NONE` is the one setting that reaches both: the backend loads
with `FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT`, which turns off the native
hinter and the autofitter together.

**The argument for turning the interpreter off** is parser surface and
nothing else. It is a stack virtual machine executing instructions out of the
font file, and it is the piece upstream fixes most often — six of the post-tag
safety fixes in the enabled module set live in `ttinterp.c` alone, and
skipping them is why they appear in the "not backported" table. It is not a
consistency argument: the configuration renders TrueType and CFF through
different hinters whichever way this switch is set.

**Against it:** a well-hinted TrueType face's designer intent at small sizes
is not honoured. This is the configuration Linux shipped for years and the
autofitter has improved considerably since, but it is a real quality
difference, and it is a difference BETWEEN FORMATS as well as against other
systems — the same design as a `.ttf` and as an `.otf` will not hint
identically here. F5 should know that before picking default faces.

**What would reverse it:** a side-by-side comparison at 12–16px on real
hardware where the autofitter's rendering is judged worse. The change is one
`#undef` removed in `port/os64_ftoption.h`; the eleven-patch list then grows
by the `ttinterp.c` fixes listed above, which must be backported in the same
change.

The monochrome (`raster1`) renderer is likewise absent: the contract's output
is 8-bit grayscale coverage and nothing in os64 consumes a 1-bit mask. It is
one line in `port/os64_ftmodule.h` plus one in `sources.mk` if a consumer
appears.

## The runtime FreeType needs, and where it comes from

Reproduced for THIS configuration rather than copied from the earlier probe.
The library links with `--no-undefined` and records **no `DT_NEEDED` at all**,
so the inventory below is complete by construction: anything missing would
have failed the link.

| Upstream name | Supplied by |
|---|---|
| `ft_memchr/memcmp/memcpy/memmove/memset` | `port/runtime.c` |
| `ft_strcat/strcmp/strcpy/strlen/strncmp/strncpy/strrchr/strstr` | `port/runtime.c` |
| `ft_qsort` | `port/runtime.c`, heapsort |
| `ft_setjmp`/`ft_longjmp` | `port/nonlocal.S` |
| `ft_smalloc/scalloc/srealloc/sfree` | **poisoned** — the engine has no global allocator |
| `ft_fopen/fclose/fread/fseek/ftell/snprintf` | **poisoned** — there is no file I/O |
| `ft_strtol`, `ft_getenv` | **poisoned** — no environment |
| `memcpy/memmove/memset/memcmp` (compiler-emitted) | `port/runtime.c`, hidden |

"Poisoned" means the macro names a function that is declared and never
defined, so an accidental use is a link error that names the operation
instead of an undefined symbol that reads as a missing libc.

The port's helpers are its own rather than libos64's on purpose: os64's
spellings differ in contract where it matters (`os64_streq` is a boolean, not
`strcmp`'s three-way answer), and borrowing by name would have been a silent
behaviour change. They are hidden and the version script exports one symbol,
so they cannot interpose on libos64's copies or be interposed by them.

**`ft_qsort` is heapsort, not quicksort:** no recursion, no stack growth
proportional to the input, and an O(n log n) worst case rather than one an
adversarial ordering could push to O(n²).

### The nonlocal return

Exactly one site in this configuration uses one: the SFNT character-map
validator arms a jump in `tt_face_build_cmaps` and fires it from
`ft_validator_error` when a subtable fails a bounds check. (`pngshim.c`, the
other caller, belongs to embedded PNG bitmaps and is not compiled.)

GCC's `__builtin_setjmp`/`__builtin_longjmp` — the earlier probe's mechanism,
and the JPEG port's — were **not** adopted. Their documented contract is "for
the compiler's own exception handling, with restrictions", and a restriction
that goes wrong here is silent stack corruption in a parser reading somebody
else's file. `port/nonlocal.S` is a small, private, documented
assembly implementation instead: it saves the System V x86-64 callee-saved
integer registers plus the stack pointer and return address, promotes a zero
value to one, carries `returns_twice`, and is NOT exposed as a public C
`setjmp` ABI. `port/runtime.h` states the whole contract, including what it
does not save and what the caller owes.

The same assembly is compiled into the host harness, so the mechanism under
test is the one that ships. `test_charmap_validator` corrupts a real font's
`cmap` table byte by byte and requires that the jump counter ACTUALLY
INCREASES — proving the path was entered, rather than inferring it from a
rejected font — at `-O2`, under AddressSanitizer, with an
`__asan_handle_no_return` call so the abandoned frames' poison is cleared.

## What the port does that FreeType does not

Three places where the F0 contract and upstream disagree; each shapes code in
`port/backend.c` that would otherwise look like needless work.

1. **`free` is told its size.** The contract's callback receives the size that
   was requested; FreeType's receives a pointer. Every block carries a
   16-byte header with its own size, which is also what keeps the payload
   `max_align_t`-aligned.
2. **Masks are owned, not borrowed.** FreeType renders into one glyph slot per
   face and overwrites it on the next load; the contract promises a glyph that
   outlives its face. A render copies the coverage into a block of its own,
   normalising upstream's signed pitch to a tightly packed, top-row-first
   mask on the way.
3. **Names are UTF-8.** FreeType flattens a face's name-table strings to
   ASCII, turning everything else into a literal `?` (`tt_name_ascii_from_utf16`).
   The port reads the name table directly and converts UTF-16BE to UTF-8,
   with U+FFFD for unpaired surrogates and truncation on a scalar boundary.
   Faces whose names exist only in a legacy platform encoding fall back to
   FreeType's ASCII copy.

One more, which is a contract question rather than a disagreement:
**kerning is reported UNROUNDED** (`FT_KERNING_UNFITTED`). `FT_KERNING_DEFAULT`
grid-fits to whole pixels and, below 25ppem, applies a scale-down heuristic
from the era when callers drew at integer positions. At 32px Source Sans 3
tucks "AV" by about 0.45 pixels, which grid-fitting flattens to zero. 26.6
exists so the fraction survives the trip; where to round is a decision about
a surface, and this backend does not have one.

## Licence

FreeType is dual-licensed; os64 takes the **FreeType License (FTL)**, the
BSD-style option, as FONTS.md selected. The pinned `upstream/LICENSE.TXT` and
`upstream/docs/FTL.TXT` are the authoritative texts and travel with the
import; `license/freetype-LICENSE` is the copy the build installs to
`/etc/licenses/freetype.txt`, because a file on the machine is this machine's
documentation and the FTL asks that credit appear there. The GPLv2 option is
not exercised and os64's own MIT licence is unaffected.

Every imported file's notice was inspected: `src/` and `include/` carry the
FTL header uniformly; `src/psaux/`'s Adobe CFF engine files additionally carry
Adobe's BSD-style notice, which the FTL option accommodates and which travels
with the files. **Font files are licensed separately from the font engine** —
`fixtures/FIXTURES.md` records each fixture's origin, digest and licence.

## What was not done

- No fuzzing. The harness walks a designed corpus — truncation at ten lengths
  per font, a byte-by-byte `cmap` corruption sweep, exhaustive allocation
  denial — but it is a corpus, not a search.
- No CPU-time bound, as above.
- No hardware run. Everything recorded here is host and QEMU.
- No review of FreeType's own test suite or CI; upstream's tests are
  regression evidence for upstream, not acceptance for this port.
- The 2.14.2 LCD-filter changes and the `FT_Span` signedness change were read
  but not exercised: neither path is compiled.
