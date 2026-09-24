# Font fixtures

Four faces the tests measure against, and the licences they travel under.
**Font files are licensed separately from FreeType** — the engine's licence
says nothing about these, and each one's terms are recorded below.

They are here rather than taken from the development host on purpose: a test
whose inputs are "whatever fonts this machine happens to have installed" has
no pinned expected values and cannot be reproduced anywhere else.

Nothing was subset, re-encoded or otherwise modified. Each file is the exact
byte sequence its release archive contains.

## The files

| File | SHA-256 | Bytes |
|---|---|---|
| `DejaVuSans.ttf` | `7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954` | 757,076 |
| `DejaVuSansMono.ttf` | `b4a6c3e4faab8773f4ff761d56451646409f29abedd68f05d38c2df667d3c582` | 340,712 |
| `SourceSans3-Regular.otf` | `08df266400933d3178d081a45f94a08814c3e55b4b7dd2e0ff69cb1329f13ab6` | 334,924 |
| `SourceCodePro-Regular.otf` | `9f9664e2edf6f045c11e774f9bd0be6993971f2544a39061a5ce478b96b051f8` | 131,128 |
| `LICENSE-DejaVu.txt` | `7a083b136e64d064794c3419751e5c7dd10d2f64c108fe5ba161eae5e5958a93` | 8,816 |
| `LICENSE-SourceCodePro.txt` | `7c940e28a5388e9bba866cf0e408edda45fe0899ba98665b8f6ab31dc5e4b8ff` | 4,566 |
| `LICENSE-SourceSans3.txt` | `89ad2c4f66dd29127527493e729c31e731f111cf10faf5774c3db9275ed0c22c` | 4,579 |

## Where they came from

**DejaVu Sans and DejaVu Sans Mono**, version 2.37, from
`dejavu-fonts-ttf-2.37.tar.bz2`
(SHA-256 `fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7`),
<https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2>.
Licence: the Bitstream Vera Fonts Copyright, a permissive BSD-style licence
that allows redistribution and modification with its notice retained; DejaVu's
own changes are public domain, and glyphs imported from Arev carry Tavmjong
Bah's equivalent notice. The archive's `LICENSE` is included verbatim as
`LICENSE-DejaVu.txt`. The Reserved Font Name clause applies to "Bitstream
Vera" and "Tavmjong Bah", not to "DejaVu".

**Source Code Pro Regular**, release `2.042R-u/1.062R-i/1.026R-vf`, from
`OTF-source-code-pro-2.042R-u_1.062R-i.zip`
(SHA-256 `754a2e3ebb945ae905d720ac5896b3b34acc9546dd6551ef9536869788629dae`).
**Source Sans 3 Regular**, release `3.052R`, from
`OTF-source-sans-3.052R.zip`
(SHA-256 `a4ebbdea20b08ccbd7bf3665a9462454eefdd01d9a6307129d3b3d4672981074`).
Both from <https://github.com/adobe-fonts/>. Licence: **SIL Open Font License
1.1**, which permits redistribution bundled with software provided the licence
travels with the fonts and the Reserved Font Name "Source" is not used for
modified versions. Neither is modified. The licence texts were fetched from
each repository at its release tag (the OTF archives carry none) and are
included as `LICENSE-SourceCodePro.txt` and `LICENSE-SourceSans3.txt`.

The three licence files ship beside the fonts in `/tests/fonts` so the terms
are on the machine that carries them.

## Why these four

Each covers something the others do not, which is what makes the set a test
rather than a sample.

| | Outlines | Metrics | Kerning comes from |
|---|---|---|---|
| DejaVu Sans | TrueType `glyf` | proportional | the legacy `kern` table |
| DejaVu Sans Mono | TrueType `glyf` | monospace | neither |
| Source Sans 3 | OpenType `CFF` | proportional | `GPOS` only |
| Source Code Pro | OpenType `CFF` | monospace | neither |

**The kerning column is the point of having two proportional faces.** DejaVu
Sans carries BOTH a `kern` table and `GPOS`, and upstream prefers `kern`, so
it can never exercise the GPOS path. Source Sans 3 carries no `kern` table at
all, so it is the only fixture that proves `TT_CONFIG_OPTION_GPOS_KERNING`
does anything — and it is also where the case for reporting kerning unrounded
comes from (see UPSTREAM_REVIEW.md).

## The pinned kerning values

`tools/test_freetype_host.c` and `userland/tests/fonttest/fonttest.c` both
assert against the table below, and neither derives it from the engine. The
font-unit values come from **`tools/font_kern_report.py`**, an independent
reader of the same bytes that shares no code with FreeType or with the
backend, converted with `units × ppem × 64 ÷ unitsPerEm` to give 26.6 pixels.
A wrong answer from the backend therefore cannot agree with itself into a
pass.

Regenerate them:

```sh
tools/font_kern_report.py --fixtures
```

It prints all four fixtures against `AV`, `AT`, `To` and `LT` at 16 and 32px
— a superset of the rows below, which are the pairs the tests actually pin.
Its output is saved as `docs/fonts/f1-evidence/kern-fixtures.txt`.

That tool deliberately copies ONE thing from FreeType — 2.14.3's rule for
which GPOS pair tables it consents to read at all — because a reader that
accepted more would report kerning the engine does not apply and call the
engine wrong. It is marked `GPOS_ACCEPTED_*` in the source so it can be found
when the pin moves.

| Face | unitsPerEm | Pair | Font units | 16px | 32px |
|---|---|---|---|---|---|
| DejaVu Sans (`kern`) | 2048 | `AV` | −131 | −65.50 | −131.00 |
| DejaVu Sans (`kern`) | 2048 | `AT` | −159 | −79.50 | −159.00 |
| DejaVu Sans (`kern`) | 2048 | `To` | −348 | −174.00 | −348.00 |
| Source Sans 3 (`GPOS`) | 1000 | `AV` | −14 | −14.34 | −28.67 |
| Source Sans 3 (`GPOS`) | 1000 | `LT` | −120 | −122.88 | −245.76 |
| Source Sans 3 (`GPOS`) | 1000 | `To` | −66 | −67.58 | −135.17 |

The tests allow ±2/64 to absorb the fixed-point scale's own rounding, which
is far tighter than the gap between a right answer and the wrong table.

**A fixture version bump invalidates every one of these.** They belong to
these exact files; re-derive them rather than assuming they carried over.
