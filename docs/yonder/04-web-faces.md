# 04 — Font families for the web: serif, sans, mono × regular, bold, italic, bold-italic, at any size

*The font arc gave os64 three ROLES — interface, terminal, document — each
one face at one size. A page needs FAMILIES: the standard's Rendering
chapter says a body is serif, `pre`/`code`/`tt`/`kbd` are monospace,
headings are bold at six sizes, `b`/`strong` are bold, `i`/`em` are italic,
and any of those may nest. In this text engine bold is a different FILE,
so "bold" is a question the provider cannot answer today.*

## Where it stands

- `os64/font_provider.h`: `OS64_FONT_ROLE_UI` / `TERMINAL` / `DOCUMENT`,
  each a primary plus up to two configured fallbacks plus the implicit
  builtin, opened at ONE pixel height into an `os64_font_set_t`. A view
  hands out the ordered `os64_text_font_t*` list a run is laid out with.
- `os64/text.h`: `os64_text_font_open` opens font bytes at a pixel height
  with `os64_font_face_options_t`; a run takes 1..8 fonts (primary, then
  fallbacks). Encoding profile is Western UTF-8 v1; no shaping, no bidi
  layout. Context memory is budgeted (`memory_cap`, `cache_cap`) and every
  open counts against it.
- `os64/font_config.h`: `fonts.conf` on the conf ladder, `<role>.face`,
  `<role>.size`, `<role>.fallback.N`; discovery over `/etc/fonts` and
  `/home/fonts`; installation; Appearance Workshop's Fonts page edits the
  primary of each role and preserves the fallbacks.
- **What SHIPS in `/etc/fonts` is two files**: DejaVu Sans and DejaVu
  Sans Mono, regular only, installed by the root `GNUmakefile`'s
  `FONT_PRODUCT` list from `userland/libfreetype/fixtures/` (where
  `FIXTURES.md` records each file's source, digest and licence, and the
  DejaVu licence lands as `/etc/licenses/DejaVu.txt`). The repo's
  `etc/fonts/` directory is GITIGNORED — a local stash (Roboto, JetBrains
  Mono, Terminus and more on this machine) that reaches no image. **No
  serif family ships at all.** No bold or oblique DejaVu.
- The standard's generic family names — `serif`, `sans-serif`,
  `monospace` — are CSS's own, from CSS1 (1996), and the cascade will
  name families with exactly those words when it arrives. This packet
  uses them now so nothing is renamed later. (`cursive` and `fantasy` are
  deliberately not offered: no page's meaning depends on them.)

## Scope and ownership

Own: the shipped font files (adding a serif family and the missing DejaVu
styles), the `fonts.conf` grammar for families, the resolver in
`font_config.c` that turns a family + weight + style into file paths with
fallbacks, one new provider entry point that hands yonder an opened font
list for a (family, bold, italic, pixel size) request with a bounded
cache, and Appearance Workshop's tolerance of the new lines. Do not change
the three roles or how gterm and Scribe use them; do not change the text
engine.

## Deliverable

**Files.** Ship DejaVu Serif (Book, Bold, Italic, Bold Italic), DejaVu
Sans Bold, Oblique and Bold Oblique, and DejaVu Sans Mono Bold, Oblique
and Bold Oblique: ten more files under the DejaVu licence that already
ships, added to `userland/libfreetype/fixtures/` with their source and
digest recorded in `FIXTURES.md` the way the two present ones are, and to
`FONT_PRODUCT` so they land in `/etc/fonts` on both volumes. Twelve faces
cover the three families completely from one metrically consistent
superfamily, which is why DejaVu and not a mix. About 5.1 MiB on the image;
the FAT lifeboat has the room and the rule (it carries `/tests`, it can
carry a serif).

**Configuration.** `fonts.conf` grows family lines, first-hit-wins on the
ladder like the rest of the file:

```
family.serif      = fonts/DejaVuSerif.ttf
family.serif.bold = fonts/DejaVuSerif-Bold.ttf
family.serif.italic = fonts/DejaVuSerif-Italic.ttf
family.serif.bolditalic = fonts/DejaVuSerif-BoldItalic.ttf
family.sans       = ...                (four lines)
family.mono       = ...                (four lines)
family.serif.fallback.1 = ...          (optional, per family, any style)
```

Absent lines fall back by rule, never to nothing: a missing `bolditalic`
takes `bold`, a missing `bold` or `italic` takes the regular, a missing
family takes the built-in defaults (the DejaVu set above), which is what
an untouched system uses. Relative paths resolve beside the file, as the
role lines do. An unreadable file refuses the whole family candidate the
way a role is refused today, and the notice names the line.

**API.** One entry point on the provider:

```c
// `families`: names as a page wrote them, in order, ending in a generic
// (SERIF, SANS, MONO). THE FIRST CUT MATCHES NO NAME and takes the generic
// tail; the signature takes the list from day one because it is the
// shape LAYOUT.md's style struct holds and the shape a cascade will hand
// over, and a signature is the one thing a later change cannot hide
// (Opus's read of LAYOUT.md, 2026-09-25).
os64_font_status_t os64_font_family_open(os64_font_family_cache_t *,
    const os64_font_family_list_t *families,
    bool bold, bool italic,
    uint32_t pixel_height,
    os64_font_role_view_t *out);    // the ordered font list + metrics
```

backed by a cache keyed on (family, bold, italic, size) that is created ON
A TEXT CONTEXT and lives there — the context the runs will be laid out on,
because a run may only use fonts opened on its own context
(FONT_CONTRACTS.md: a foreign-context font is BAD_ARGUMENT) and each
context owns its own copy of every font file it opens. It is bounded by
that context's budget, because a page asks for the same twelve faces at
perhaps eight sizes and a browser opening a face per text run would
exhaust the budget on one page. yonder's arrangement (LAYOUT.md § Bounds)
is one PAGE context for all its page views, apart from libui's chrome
context, and the cache sits on the page context. The cache releases
least-recently-used entries under the context's budget rule and never
releases a font a live run still retains (the text engine's own retention
rule does that accounting). Yonder owns the cache instance; the provider
owns the resolution. Session font changes (Apply in Workshop) do not
reach families in this cut: a page's faces are the page's, chosen from
`fonts.conf` at yonder's start — booked below.

**Workshop** keeps its Fonts page as it is, and a `fonts.conf` carrying
family lines round-trips through Save unchanged (the merge-line-by-line
writer already preserves lines it does not edit; the test is that it
still does with these).

## Required evidence

- The config decoder's host test grows the family grammar: every fallback
  rule above as a case, relative paths, a missing file refusing the
  family and naming the line, round-trip through encode.
- A cache test on the host with the fake backend from `tools/fonts/`,
  under the budget in § Answers (Q2): all twelve faces asked for at eight
  sizes — 96 keys through a cache of 32 — never more than 32 held open by
  the cache, the least-recently-used one going first, a retained run
  keeping its font alive across an eviction, and the engine's live faces
  never past `OS64_FONT_FACE_MAX`. And the list contract (Q1): a list
  used after the next call is a use-after-free that ASan names.
- Guest: a probe under `/tests` (the harness lives in `/tests`, not
  `/bin`) that draws one line per face — twelve faces at three sizes,
  regular/bold/italic/bold-italic nested the way a page nests them —
  screendump, read the image. And Scribe and gterm unchanged, screendump.
- Spawn latency of a trivial program before and after: the F2 handoff
  measured this for libfreetype and the number must not move, because
  nothing here loads at spawn.

## Answers to Quinn's questions (Opus, 2026-09-27)

### Q1. How long does a returned font list stay valid?

**Until the next `os64_font_family_open` on the same cache, or the
cache's destruction — whichever comes first. Nothing else ends it.**

- An open call may evict entries; cache destruction releases the remainder.
  Timers, budget checks outside open, and work on another thread do not
  evict entries (a cache is serialized with its text context). Between
  two calls the list cannot
  change under a caller, and a caller that wants two lists at once
  cannot have them: it asks, uses, asks again.
- The list's storage may be the evicted entry's own, so using a list
  after the next call is a use-after-free, not a stale read. That is
  deliberate: ASan names it in the host test, where a "valid until
  eviction" rule would be a bug that only shows on the page that evicts.
- **The fonts IN the list outlive the list** by the engine's own rule: a
  run laid out with them retains them, so an eviction closes the cache's
  reference and a live run keeps the face open until the run is
  released. The list is borrowed; a run's fonts are owned.
- **The consumer's half** (libflow, mine): libflow asks for fonts,
  lays out the runs it needs with that list, and does not ask again until
  it is done with it. One site did not — a line fragment asked for the
  tab stops' font between getting its own list and laying out its run —
  and is fixed on my side before this packet lands, with the contract
  written on `flow_env_t.fonts` (flow.h) so the next site cannot drift.
  yonder's current resolver never evicts, so nothing breaks today.

### Q2. What memory budget proves the faces fit and leave room for layout?

Measured on the host, 2026-09-27, with the REAL FreeType backend
(`os64_freetype_backend_v1`) and the twelve DejaVu 2.37 files, counting
every byte the text context asks its allocator for:

| What | Measured |
|---|---|
| The twelve files | 5.12 MiB (Sans 2.62, Serif 1.37, Mono 1.13) |
| One open, beyond its file | 18.6 KiB of FreeType state |
| One open, in all | its WHOLE FILE again (`os64_text_font_open` copies the bytes per open), + 18.6 KiB |
| 12 faces × 8 sizes, all open | 42.7 MiB (41.0 of it file copies) |
| + every ASCII glyph drawn in all 96 | 49.1 MiB (6.3 MiB of glyphs, under the 8 MiB `cache_cap` default) |
| Runs of the largest corpus page (Wikipedia, 7,835 elements) | 8.8 MiB for one tree; **17.7 MiB for two**, which is what a relayout holds while it builds the new tree beside the old |

**Bytes are not the binding limit. Face COUNT is.** The FreeType engine
refuses a 65th live face (`OS64_FONT_FACE_MAX` = 64 per engine, and a text
context is one engine): the measurement's 65th open came back
`OS64_FONT_LIMIT` at 33 MiB. So twelve faces at eight sizes — 96 — cannot
all be open on one context however much memory there is, and the budget
has to be stated in faces as well as bytes:

- **The page context: `memory_cap` 128 MiB (`OS64_TEXT_MEMORY_DEFAULT`,
  what yonder uses today), `cache_cap` 8 MiB (the default).**
- **The family cache holds at most 32 entries.** The proof is the
  relayout: the new tree's faces come through the cache (≤ 32 open), the
  old tree's runs can keep at most its own ≤ 32 alive until it is freed,
  and 32 + 32 = 64. Worst case in bytes: 64 faces of the LARGEST file
  (DejaVu Sans, 742 KiB + 18.6 KiB) = 47.5 MiB.
- **The whole context at its worst:** 47.5 (faces) + 8 (glyphs) + 17.7
  (two trees of the biggest page) = **73.2 MiB of 128**, leaving 54.8 MiB
  — about three more Wikipedias of text — before a layout fails. A
  failed layout keeps the old page on screen (yonder's "Out of memory
  laying the page out" path), so the ceiling is graceful.
- **The honest edge:** a page that uses more than 32 distinct
  (face, size) pairs in ONE layout keeps its evicted faces alive through
  its own runs, so its next relayout may pass 64 and fail. A typical page
  uses 4–15 (Hacker News: DejaVu Sans at 9, 11, 12 and 13 px, regular
  and bold, is 8). Booked below with its trigger.

Two changes outside this packet would each move the ceiling, and both are
booked rather than taken here, because the packet does not change the text
engine: one copy of a file's bytes per context however many sizes open it
(96 opens would cost 5.1 MiB of bytes instead of 41), and a higher
`OS64_FONT_FACE_MAX` (its state is 18.6 KiB a face, so the count guards
nothing the byte budget does not).

## Booked out of this packet, by name

| What | Why | Trigger |
|---|---|---|
| Synthetic bold/oblique (FreeType embolden/slant) when a member is missing | the fallback-to-regular rule covers it honestly; a synthetic face is a lie about the file | a family a person installs with only a regular |
| `cursive`, `fantasy` | no page's meaning depends on them | never, probably |
| Live family changes through Workshop's Apply | the adoption dance (prepare/barrier/commit) is per role today; families are yonder's own cache | when Workshop grows a Web page |
| Weights beyond bold (100..900) | CSS's numeric weights want the cascade first | the cascade |
| Font matching by family NAME from a page (`font-family: Georgia`) | the cascade; and matching by name is a resolver over installed metadata | the cascade (it has ARRIVED: libgarb hands libflow the names as written, `flow_family_list_t`) |
| One copy of a font file's bytes per text context, however many sizes open it | each open copies the whole file (41 MiB of the 42.7 measured for 96 opens) | a page whose faces meet the byte budget before the face count |
| A higher `OS64_FONT_FACE_MAX` | 64 live faces per engine is the binding limit (Q2); the state it guards is 18.6 KiB a face | a page that uses more than 32 face-sizes in one layout |

## Implementation contract (Quinn, 2026-09-27)

The public types and lifetime rules are in `os64/font_provider.h`:
`os64_font_family_list_t` carries ordered name/length slices plus an explicit
`generic` tail. `os64_font_config_read` selects the startup file;
`os64_font_config_family_prepare` resolves it and creates the cache on the
caller's page context. Destroy with `os64_font_family_cache_destroy` before
destroying that context. All calls are serialized with the context.

The resolver reads and validates the selected family members before publishing
its candidate. Empty families select the twelve shipped DejaVu faces; for a
partially configured family, an absent regular uses its DejaVu regular, and
absent styles follow the inheritance rules above. Optional fallback slots
are compacted in numeric order. Duplicate source paths are read once.
`builtin` is accepted explicitly; its bitmap remains 8x16 at any request size.
Outline sizes follow the existing engine range, 1 through 256 pixels.

The cache snapshots each distinct input span once, charging the immutable
bytes to the page context. This adds about 5.1 MiB for the default families,
beyond Q2's per-open byte estimates. Reopening an evicted face cannot pick up
a file changed after startup. The temporary resolver reads are separately
bounded by the existing 64 MiB source-read limit.

**Q2's 32 + 32 face calculation applies to primary-only entries.** Each
configured outline fallback also consumes an engine face, and retained runs
can keep more faces alive than the cache holds. The entry cap is 32; on an
engine or context LIMIT, opening evicts older entries and retries, dropping
partially opened candidates before retrying. If retained runs still exhaust
the cap, the caller receives LIMIT with an empty output. The text engine and
its 64-face ceiling are unchanged. The 96-key tests exercise eviction; they
do not claim that 96 faces can be simultaneously open.

Each successful open publishes a separate borrowed list. The next open frees
that list even on a cache hit or an invalid request. Runs retain their font
handles independently; a host negative test requires ASan to diagnose using
the expired list.

Workshop publishes and edits roles only. Its Save preserves existing family
lines, including relative paths, spacing and comments, through the existing
line merge. The general config encoder includes family settings for callers
that explicitly serialize the whole config.

The guest specimen is `/tests/webfacestest`: twelve faces at 12, 18 and 26px,
plus nested regular/bold/bold-italic/italic transitions. Yonder/libflow's
consumer switch is a separate integration step in its owning worktree.
