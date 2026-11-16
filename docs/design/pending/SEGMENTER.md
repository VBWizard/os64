# SEGMENTER.md — `Intl.Segmenter`

*Opus 5.5, 2026-11-07; Chris approved it and took every lean the same day
(all three granularities at once, dictionary scripts booked, Unicode 17,
built by me). § 4 is the design as built, with what the build changed
said where it changed it. INTL.md § 4 booked
`Intl.Segmenter` as absent ("a page that tests for it falls back"). A
social network's logged-out front end does not test: it constructs one at
module scope, `new Intl.Segmenter(void 0, {granularity: "grapheme"})`,
throws `TypeError: not a function`, and its boot never reaches the render
(found by replaying its P5 record; MODULE_FAILURES.md is why nobody saw
the error). This is an INTL.md constructor, built by its rules.*

## 1. What pages ask of it, counted

The local corpus (the 10/21–11/04 pulls, 13 distinct scripts that name
`Intl.Segmenter`, de-duplicated by content), plus the page above:

| Construction | Scripts | Hosts | Guarded? |
|---|---|---|---|
| `granularity: "word"` | 6 | 3 (a search engine, a chat assistant, a news site) | all test `typeof Intl.Segmenter` (or `"Segmenter" in Intl`) first; the search engine's then constructs with no `try` |
| `granularity: "sentence"` | 1 | 1 (the news site, beside its word one) | behind a feature flag |
| `granularity: "grapheme"` | 1 + the page above | 2 | the ad script tries and catches; **the page above neither tests nor catches** |
| no options (grapheme by default) | 2 | 1 (a design tool) | tests `typeof … === "function"` |
| granularity from a variable | 1 | 1 (a mail client) | tests, then tries |

What they do with it: iterate `segment(text)` for `segment` (a design
tool splitting into graphemes), keep the segments whose `isWordLike` is
true (word counts, search terms), call `containing(i)` (the page above
trims leading and trailing white space by grapheme, walking backwards
with `containing`), and read `index`. No `resolvedOptions` beyond
`locale`.

**The finding that shapes the design:** every page but one already works
today, on its fallback, *because* `Intl.Segmenter` is absent. The moment
it is present, those pages take the Segmenter path instead, and they ask
for `word` (and one for `sentence`). A grapheme-only Segmenter would turn
the search engine's script, which tests for the constructor and then
constructs a word segmenter with no `try`, from working into throwing.
That is INTL.md's rule 1 (§ 4: "each constructor ships whole or not at
all … a present one that misbehaves is rule 1's worse case"). **So all
three granularities ship together, or none does.**

## 2. What ECMA-402 asks (§ 18, Segmenter)

- `new Intl.Segmenter(locales, options)`: `localeMatcher`, and
  `granularity` one of `"grapheme"` (the default), `"word"`,
  `"sentence"`; anything else a RangeError. Locale resolution is INTL.md's
  (§ 5): what the data holds is en and und, and segmentation in CLDR is
  root's rules for both.
- `resolvedOptions()` → `{locale, granularity}`;
  `Intl.Segmenter.supportedLocalesOf`; `@@toStringTag` "Intl.Segmenter".
- `segment(string)` → a Segments object: `containing(index)` returns the
  segment data of the segment holding that code unit (undefined outside
  the string), and `[Symbol.iterator]()` returns a Segment Iterator whose
  `next()` yields segment data in order.
- Segment data: `{segment, index, input}`, plus `isWordLike` for the word
  granularity only (true for a segment of letters, numbers or ideographs,
  false for spaces and punctuation).

test262 `intl402/Segmenter/` is the conformance suite; the runner
INTL.md built (`tools/test262_host.sh`) takes it as a new directory, added
to `tools/test262_fetch.sh`'s list, with its expectation file.

## 3. The rules: Unicode's, at the engine's version

Boundaries are **UAX #29** (Unicode Text Segmentation): extended grapheme
clusters (GB1–GB999, with Unicode 15.1's Indic conjunct rule GB9c and the
emoji ZWJ rule GB11), default word boundaries (WB1–WB999), and sentence
boundaries (SB1–SB998). They are applied at **Unicode 17.0**, the version
the engine's own tables are at (`libunicode.h`), so `\p{…}` in a page's
regular expression and `Intl.Segmenter` agree about every character. (The
bidi data in `tools/unicode/` is pinned at 16.0; this pins its own files
at 17.0 beside it, and moving bidi up is not this slice.)

**Where yonder and other browsers will differ, said now:**

- **Dictionary scripts.** Chrome's word breaking uses ICU, which finds
  words in Thai, Lao, Khmer, Myanmar, Chinese and Japanese with
  dictionaries. UAX #29 has no dictionary and gives those scripts' letters
  Word_Break Other, so a Han, hiragana, Thai, Lao, Khmer or Myanmar run
  breaks after every letter (with its marks), and only a katakana run
  stays whole (WB13). Booked, with the dictionaries' size as the reason;
  the corpus calls are for counting and finding English words.
- **`isWordLike`** is "the segment holds a letter or a number"
  (General_Category L* or N*), one bit in the table. ICU decides it by
  the rule that made the segment (letter, number, kana and ideograph
  rules are word-like, space and punctuation are not), and for words of
  letters, digits, kana and ideographs the two agree. They part where a
  character's category and its Word_Break class do: `①` and `½` are
  numbers with Word_Break Other, `ⓐ` a symbol with Word_Break ALetter.
  The capture (§ 6) carries those beside an `_` alone and an emoji, and
  where Chrome differs the table's bit follows Chrome, not memory.
  (Designed as "follows ICU's rule statuses".)
- Sentence breaking is UAX #29's, without ICU's abbreviation suppressions
  (CLDR's "Mr." list is a locale tailoring ICU applies for some locales
  only).

## 4. Where it lives

**The boundary finding is C, the API surface is the script.**

- `userland/libjs/intl/segment.c`: pure (no engine headers), like
  `psf2.c` or `tty_reflow.c`, so the host can run it under ASan against
  Unicode's own tests. One entry:
  `intl_segment_next(kind, const uint16_t *text, size_t length, size_t at, bool *word_like)`
  returns the end of the segment that starts at `at` (UTF-16, as a JS
  string is; a lone surrogate is a code point of its own, whose
  Grapheme_Cluster_Break is Control, as UAX #29 classes surrogates). The
  three rule sets are written out as UAX #29 states them, one function
  each, each rule a commented line with its number, so a reader holds the
  code against the standard. Each is a forward scan from a boundary that
  keeps the little state its rules look back at (the regional indicators
  so far, GB11's emoji sequence, GB9c's conjunct, the two characters WB4
  leaves, the open sentence terminator), and SB8's look ahead keeps its
  answer for as far as it holds, so a scan is linear in what it reads.
- `userland/libjs/intl/segment_data.inc`: **generated**, one merged range
  table, each range's start and its classes (Grapheme_Cluster_Break,
  Word_Break, Sentence_Break, InCB, Extended_Pictographic, and the letter
  or number bit `isWordLike` reads), binary searched. As built: 4,881
  ranges (the bit adds a few), a start and a packed word each, 38 KiB of
  read-only data. It is C data, shared by every process that
  maps the library, and costs a realm nothing, unlike a table in the
  script (INTL.md § 6: every realm reads the script's bytecode).
- `tools/intl_unicode.py` writes it from pinned files, each with its
  sha256 in `tools/unicode/segmentation/PINNED.json`:
  `auxiliary/GraphemeBreakProperty.txt`, `WordBreakProperty.txt`,
  `SentenceBreakProperty.txt`, `DerivedCoreProperties.txt` (its InCB
  lines), `emoji/emoji-data.txt` (Extended_Pictographic),
  `extracted/DerivedGeneralCategory.txt` (the letter or number bit), and
  the three `*BreakTest.txt` files, which are the oracle (§ 6). The
  generator refuses a file whose hash moved. As built, two changes: the
  pins have a directory of their own, since `tools/unicode/PINNED.json`
  is read as one object by the bidi generator and its test; and only the
  three test files are kept in the tree (every host-test run reads them),
  while the property files, `DerivedCoreProperties.txt` alone a megabyte,
  are fetched into `.vendor/ucd-17.0.0/` by `--fetch` and verified there,
  as only regenerating the table needs them (`--check` says whether the
  checked-in table is current).
- **The door** (`intl.c`, beside the zone functions): `segment(kind,
  string)` answers a whole string in one call, an ArrayBuffer of each
  segment's end times two plus its word-likeness. (Designed as one call
  per segment; the engine hands C a string only as UTF-8, so each call
  would convert the whole string again, and iterating a long one would be
  quadratic.) `segment.c` is compiled into intl's object, as `zones.inc`
  is included there, so no build list that names `intl.c` changes.
- **The script** (`intl.js`): `Intl.Segmenter`, the Segments object and
  the iterator, by § 18's algorithms, calling the door. `containing(i)`
  needs the segment that holds `i`, and UAX #29's rules are not run
  backwards safely, so a Segments object asks the door once, the first
  time it is iterated or asked what contains an index, keeps the answer,
  and answers each later `containing` by binary search: the page above
  walks backwards one `containing` at a time, which would otherwise be
  quadratic.

## 5. What it costs

- Script: the three classes and their methods, a few KiB of bytecode a
  realm (measured in the slice against INTL.md § 6's 339 KiB).
- Data: the C table above, in the library's read-only data.
- Time: one binary search per code point, and the rules' small state. A
  word count over a long chat message is linear in its length.

## 6. Proof

- **Unicode's own tests**, through `segment.c` on the host under ASan:
  every line of `GraphemeBreakTest.txt` (766), `WordBreakTest.txt`
  (1,944) and `SentenceBreakTest.txt` (512), at 17.0
  (`tools/test_intl_segment_host.sh`). All pass.
- **test262 `intl402/Segmenter/`**, through the runner, with an
  expectation file listing only what is excluded and why: 150 of 158
  run and pass; the 8 excluded need locale data os64 does not carry or
  the `$262` host hooks.
- **A Chrome capture** (`tools/pages/intl-capture.html`, which Chris runs
  in Chrome, as for A2/A3): added probes for `isWordLike` on letters,
  digits, `_`, emoji and punctuation, a Thai and a Chinese sentence (to
  record the dictionary difference in the capture, not to match it), and
  each corpus call's shape. The capture becomes the gate for everything
  outside the dictionary scripts. The page carries the new cases;
  `intl_capture_check.py` runs the cases a capture has, so until Chris
  re-runs the page it holds everything the last capture had (Chrome 156)
  and the Segmenter cases join it with the next one.
- **The corpus calls as fixtures** (INTL.md's practice): each shape in § 1
  run against fixed text, in `tools/test_js_runtime_host.c`'s Intl cases.
- **The page above, replayed**: its boot goes past the construction. What
  it meets next is reported (MODULE_FAILURES.md makes sure it is): a
  stack overflow in the framework's render loop, which is its own slice.
- Hand mutants: one rule broken in each rule set (GB9c, GB11, WB6/7,
  SB8) must fail Unicode's tests.

## 7. Slices

One PR, because nothing can be exposed until all three granularities
pass: **A4a** the generator, the table, `segment.c` and Unicode's tests
(nothing visible to a page); **A4b** the door, the script's classes,
test262, the capture probes and the fixtures, which is where
`Intl.Segmenter` appears. MODULE_FAILURES.md is its own small slice and
can land first, so that whatever the page meets after the Segmenter is
on the record the day it happens.

## 8. Questions for you (answered: Chris took all four leans)

1. **Ship whole** (all three granularities at once, § 1's reason), not
   grapheme first. I lean whole: grapheme alone would break a search
   engine's script that works today.
2. **Dictionary scripts booked**, not built (Thai/Lao/Khmer/Myanmar/CJK
   words as UAX #29 has them). I lean book: the dictionaries are megabytes
   and no page we have asks.
3. **Unicode 17 for these files** (the engine's version), with bidi left
   at 16 until its own slice. I lean yes.
4. **Who builds it**: INTL.md was the first Intl session's; this is its
   next constructor. I can build it, or it goes to whoever you seat on
   Intl.
