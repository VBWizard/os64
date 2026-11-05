# INTL.md — ECMA-402 for yonder's pages

*Opus, 2026-11-04. Slice A of GITHUB_WALLS.md (Fable's, same day): Intl is
the wall that empties all three of the GitHub pages before they draw, by
module-scope constructions (`new Intl.DateTimeFormat(...)` first, then
NumberFormat, Collator and ListFormat) that abort megabyte-long modules
half made. Reviewed by Fable the same day (build it; ten findings, folded
in); Chris ruled the rest the same day (§ 9).*

## 1. What pages ask of it, counted

The census is the spec (GITHUB_WALLS § 5). It reads every script yonder
kept on Chris's P5 (`~/os64-corpus`, the 10/21–10/25 pulls: 2,871 distinct
scripts after de-duplicating by content) with
`~/os64-opus-kit/tools/intl_census.py`, and adds GitHub's own module
chunks by hand: a module script is not kept (§ 8), so the corpus cannot
see the pages that started this.

**268 scripts on 90 hosts name `Intl`.** By constructor, counting the
scripts that name it and the hosts they came from:

| Constructor | Scripts | Hosts | Shape of the calls |
|---|---|---|---|
| DateTimeFormat | 130 | 57 | 83 calls with no options; the rest field bags (`year/month/day/weekday/hour/minute/second`, `hour12`/`hourCycle`, `timeZoneName`), `timeZone` fixed (`"UTC"`, `"America/Los_Angeles"`, `"America/New_York"`) or a variable; `era` once. GitHub adds `dateStyle`/`timeStyle` (§ 1 of GITHUB_WALLS) |
| NumberFormat | 74 | 43 | `style:"currency"` (USD mostly, `currencyDisplay` symbol/narrowSymbol/name), `notation:"compact"`, `useGrouping`, fraction and significant digits, `style:"unit"` (degree), `signDisplay` once |
| ListFormat | 23 | 21 | mostly no options; `type: conjunction/unit`, `style: long` |
| RelativeTimeFormat | 27 | 18 | `numeric:"auto"` above all; `style:"long"` |
| PluralRules | 27 | 18 | cardinal and `type:"ordinal"`, `minimumFractionDigits` |
| Locale | 22 | 15 | constructed from an expression, read for its parts |
| DisplayNames | 11 | 9 | `type:"currency"` |
| Segmenter | 8 | 6 | `granularity: word/sentence` |
| Collator | 5 | 4 | `usage:"sort"`, `sensitivity` |
| DurationFormat | 2 | 1 | |
| `getCanonicalLocales` | 6 | 4 | |

Locale arguments: an expression 107 times (a page passing
`navigator.language` or its own setting), none 72, `"en-US"` 65, other
English (`en`, `en-CA`, `en-GB`) 25, `de-DE` twice.

The methods called, in the scripts that name Intl: `format` 541, `select`
194, `compare` 163, `resolvedOptions` 138, **`formatToParts` 83**,
`formatRange` 10. Probes: `typeof Intl` 34 times, `"Intl" in` once.

**The built-ins ECMA-402 redefines matter as much as the constructors.**
`localeCompare` called with an argument on 72 hosts, `toLocaleString` on
32, `toLocaleDateString` on 23, `toLocaleTimeString` on 13. QuickJS has
these today, ignoring their locale and options (§ 2), so a page that asks
for `toLocaleDateString("en-US", {month: "long"})` gets a different string
than other browsers give it, silently, today.

## 2. What QuickJS has

No `Intl`. Bellard's tree has no plan to add it, and the ruling to stay on
it is made (GITHUB_WALLS § 3). What it does have:

- `Date.prototype.toLocaleString` / `toLocaleDateString` /
  `toLocaleTimeString`: a fixed `M/D/YYYY, h:mm:ss AM` rendering, no
  arguments read.
- `Number.prototype.toLocaleString`: `toString`, no grouping.
- `String.prototype.localeCompare`: code-point order after NFC.
- `toLocaleUpperCase` / `toLocaleLowerCase`: the locale-free mappings,
  which are what every locale but Turkish, Azeri and Lithuanian uses.
- The local time zone: libjs's port reads os64's `TZ` (a POSIX string,
  `EST5EDT` or `CET-1CEST,M3.5.0,M10.5.0`, `os64/date.h`), so
  `getTimezoneOffset` is right for the current rules.

## 3. The shape

**Self-hosted JavaScript, installed by libjs at realm start, with en-US
data and the Gregorian calendar, complete to ECMA-402 for that data.**

- **Why JavaScript.** Intl is option parsing (`GetOption` against a list of
  allowed values, in an order the spec fixes and test262 checks), number
  formatting, and calendar arithmetic. QuickJS already has the arithmetic
  (`Date`'s UTC fields, `Number.prototype.toFixed`/`toPrecision`, BigInt);
  what is missing is the shape around it. In C each option read is a
  dozen lines of `JS_GetProperty` and exception plumbing; in JS it is a
  call. Streams S1 made the same choice for the same reason and measured it
  (§ 6).
- **Why libjs and not libdom.** Intl is the language (ECMA-402 extends
  ECMA-262), not the DOM. The `js` command and any future libjs host get it
  too, and it touches nothing of the binding. libjs's runtime gains one
  door, `os64_js_install_intl`, which a host calls on a realm; yonder's
  scripts host calls it when the Modern Web switch is on (§ 9.4).
- **Why en-US only is not half an object.** ECMA-402 makes the set of
  supported locales the implementation's: a request for `de-DE` resolves
  to the best supported locale (`resolvedOptions().locale === "en-US"`),
  `supportedLocalesOf(["de-DE"])` answers `[]`, and a page that wants German
  learns it cannot have it, by the standard's own door. The same holds for
  calendars (`gregory`) and numbering systems (`latn`). MODERN_WEB.md
  rule 1 asks for an interface complete to its standard; this is that,
  with the standard's own subset of data, which is what Chris's machine
  and its one user need. A second locale is data, not code (§ 7).
- **One locale, named once.** Pages pass `navigator.language` straight to
  Intl's constructors, and read Intl's default back to compare with it, so
  the two must agree. libjs owns the only locale data there is, and both
  libdom and yonder already include its public header, so the name lives
  there: `OS64_JS_LOCALE` (`"en-US"`) in `os64/js.h`. libdom's
  `navigator.language` and `navigator.languages` (Slice B) read it, and
  libjs hands it to the Intl script as the default locale, which never
  spells the tag itself. yonder's `Accept-Language` stays agent.c's: those
  are captured wire strings with their q-values, one per costume; yonder's
  own already asks for `en-US` first. Agreed with the Slice B session
  before either was written: Slice B adds the define, and this slice uses
  it from userland once B has merged, or from B's branch if A1 is ready
  first. A1 does not define it.
- **The C underneath is small: one zone door** (`libjs/intl/intl.c`),
  three verbs over the zone table (§ 5): a name's canonical zone, a
  zone's offset in minutes at an instant, and the machine's own zone.
  Offsets come from libos64's own POSIX-rule evaluator (`os64/date.h`,
  what `Date`'s local time already runs on), so `Date` and `Intl` cannot
  disagree about an offset and no second evaluator is written in
  JavaScript. Whether an offset is daylight time the script decides
  (above the year's lower offset), and the names it prints are CLDR's,
  not the rule's abbreviations. Everything else is JavaScript over intrinsics captured at
  install, as streams.js captures its own, so a page that replaces
  `Array.prototype.push` or `Date.prototype.getUTCFullYear` cannot change
  what Intl does. (Fable's finding 3.)
- **`format` and `compare` are getters, not methods.**
  `DateTimeFormat.prototype.format`, `NumberFormat.prototype.format` and
  `Collator.prototype.compare` are accessors answering a function bound
  to the instance, made once and kept, so `dates.map(dtf.format)` and
  `names.sort(collator.compare)` work; the census shows both idioms, and
  test262 holds the shape. (Finding 7.)

## 4. What ships, by constructor

The cut is by demand (§ 1), and each constructor ships whole or not at
all: an absent constructor is a feature test's honest `undefined`
(`Intl.Segmenter` was absent from Firefox until 2024, and pages test for
it); a present one that misbehaves is rule 1's worse case.

| Ships | Why |
|---|---|
| `Intl.DateTimeFormat` — `format`, `formatToParts`, `formatRange`, `formatRangeToParts`, `resolvedOptions`, `supportedLocalesOf`; every option, `dateStyle`/`timeStyle` and the field bags, `timeZone`, `hour12`/`hourCycle`, `timeZoneName` (all six values), `era`, `dayPeriod`, `fractionalSecondDigits` | 57 hosts, the GitHub wall |
| `Intl.NumberFormat` — `format`, `formatToParts`, `formatRange`, `formatRangeToParts`, `resolvedOptions`; decimal, percent, currency (ISO 4217's codes and their symbols in en-US), unit (the sanctioned simple units), compact notation, scientific and engineering, every digit and rounding option, `signDisplay`, `useGrouping` | 43 hosts |
| `Intl.PluralRules` — cardinal and ordinal, `notation`, the digit options, `select`, `selectRange`, `resolvedOptions` | 18 hosts; NumberFormat's compact and RelativeTimeFormat need its rules anyway |
| `Intl.RelativeTimeFormat` — `format`, `formatToParts`, `numeric`, `style` | 18 hosts, GitHub's relative dates |
| `Intl.ListFormat` — the three types and three styles | 21 hosts |
| `Intl.Collator` and `String.prototype.localeCompare` through it — `usage`, `sensitivity`, `ignorePunctuation`, `numeric`, `caseFirst`, over the collation § 4.1 names | 72 hosts call `localeCompare` with arguments; GitHub's checks module constructs one at module scope |
| `Intl.Locale` — parsing and canonicalizing a BCP 47 tag, its parts; `maximize`/`minimize` by CLDR's likely subtags for the English tags and every `und` row, and the identity for any other tag the data lacks (the algorithm's own answer when no row matches: `new Intl.Locale("de-DE").maximize()` is `de-DE`) | 15 hosts; every constructor's locale argument is parsed the same way; primer-react's RelativeTime constructs one in render |
| `Intl.getCanonicalLocales`, `Intl.supportedValuesOf` | small, and the second answers truly what the data holds |
| The redefined built-ins: `Date.prototype.toLocaleString`/`DateString`/`TimeString`, `Number.prototype.toLocaleString`, `BigInt.prototype.toLocaleString`, `Array.prototype.toLocaleString` (unchanged in shape, its elements' now honoured) | 32 hosts; as § 1 says, wrong silently today |

| Booked, absent (a feature test answers `undefined`) | Why it waits |
|---|---|
| `Intl.Segmenter` (6 hosts) | grapheme and word breaking is Unicode's segmentation data, a different and larger table; a page that tests for it falls back |
| `Intl.DisplayNames` (9 hosts) | names of languages, regions, currencies and scripts: data, not formatting; booked with a second locale |
| `Intl.DurationFormat` (1 host) | the newest constructor, one host; built on NumberFormat, so cheap when a page asks |

### 4.1 Collation, named

Real en-US collation is CLDR root, the Unicode Collation Algorithm over
DUCET: tens of thousands of weights. It is why other browsers answer
`"a".localeCompare("B")` with -1 and sort `_` before `1` before `a`, where
code-point order puts `_` after the capitals. A collation that folds case
and then compares code points would be "present but wrong" on every
symbol. So the collation is written down (Fable's finding 2):

- **Latin in CLDR root's order:** the collation elements of CLDR's root
  collation (`allkeys_CLDR.txt`, UCA 17, the release CLDR 48.2 ships)
  for U+0000-024F (Latin-1 and Latin Extended-A and -B, so "ā" sorts
  between "a" and "b"), the combining marks U+0300-036F (so "e" plus an
  acute accent is "é") and general punctuation U+2000-206F, with the
  root's contractions among them ("l·"); generated, not typed. As built
  (A2): wider than the Latin-1 first planned, because names are.
- **Three levels:** the base letter (accents and case ignored), then the
  accent, then case, lower before upper; `sensitivity` stops at the level
  it names (`base` the first, `accent` the second, `case` the first and
  third, `variant` all three), `caseFirst` reorders the third.
- **`numeric: true`** compares a run of digits as the number it spells;
  **`ignorePunctuation`** skips spaces, punctuation and the whitespace
  controls (tab, newline; CLDR's `maxVariable` is punct: symbols still
  count).
- **Beyond those ranges, code-point order** after every letter in the
  table, the gap booked by name (DEBTS: full DUCET, its size stated). The
  collator does not normalize: two strings that differ only in the order
  of two or more stacked marks compare unequal (DEBTS).

The fixtures hold what the 72 hosts ask (`{numeric: true, sensitivity:
"base"}` over names and version strings) against Chrome's order.

### 4.2 The data, from CLDR, versioned

DateTimeFormat's hard half is turning a field bag (`{weekday: "long",
hour: "numeric"}`) into Chrome's pattern, which ICU does with CLDR's `en`
`availableFormats` table, its append items and the hour-cycle rewrite.
That table is taken verbatim (about sixty entries) into a data file in the
tree, its CLDR version named as the zone table names its tzdata, and the
spec's BasicFormatMatcher runs over it; hand-made patterns would match
Chrome on the bags the census saw and differ on the next. The same rule
for every table the formatters read: the `dateStyle`/`timeStyle` patterns
and their `{date}, {time}` / `{date} 'at' {time}` glue, month and day
names, the compact suffixes, the currency symbols, the unit patterns,
plural rules, the relative-time and list patterns. Data files with a
version, generated or copied from CLDR, never literals typed from memory.
(Finding 6.)

## 5. Time zones

The hard part, because pages pass zone names and read them back:

- **Names.** CLDR's zone names (its bcp47 table, every name and alias
  other browsers' ICU accepts; `tools/intl_zones.py`), each mapped to the
  POSIX rule its TZif file's footer carries in the build machine's tzdata (`America/New_York` →
  `EST5EDT,M3.2.0,M11.1.0`). A generated table, a few tens of KiB,
  compiled into libjs. A name not in it is a `RangeError`, as other
  browsers throw for one they do not know, so a page's typo behaves as it
  does elsewhere.
- **The host's zone.** `resolvedOptions().timeZone` with no `timeZone`
  must be an IANA name, and os64's `TZ` is a POSIX string, which many
  zones share (`EST5EDT,M3.2.0,M11.1.0` is New York's rule and Toronto's
  and thirty others'). So, without guessing silently (Fable's finding 4):
  a `TZ` rule names the table's preferred zone for that rule
  (`kRulePreferred`, generated beside the names: the metazone's golden
  zone when one carries the rule, else the first by name), matched by
  its text, else by keeping the same times under other names (`EST5EDT`,
  what the P5 runs, is New York's rule written short, and resolves to
  `America/New_York`, as Node's V8 resolves the IANA name `EST5EDT`;
  Chrome 156 keeps that name, booked in DEBTS); a rule no zone carries is an offset zone (`+04:15`) at its
  standard offset; no `TZ` is the kernel's offset as an offset zone, and
  no offset is `UTC`. The real cure is letting the system's `TZ` be an IANA
  name too (`bootenv.conf`'s `TZ = America/Toronto`, resolved through the
  same table); that is libos64's parser and is booked, not this slice.
- **History is the honest gap.** A footer is the zone's CURRENT rule. A
  date from before the rule took effect (US 2007, the EU's 1996 change, a
  country that has since moved zones) formats with today's rule, off by an
  hour across a DST change that has since moved. Dates near now, which is
  what pages format, are right. The full transitions (the TZif body) are
  about 400 KiB for every zone; booked, with the cost named, for the day a
  page shows a wrong historical hour. QuickJS's own `Date` already has the
  same gap, so Intl and `Date` agree.
- **The offset of a named zone at an instant** is the zone door's (§ 3),
  evaluating the rule from the table with libos64's code, so the
  `M.m.w.d` and `/time` forms tzdata's footers use are read by one
  evaluator.
- **Zone names in output** (`timeZoneName`): `short` gives `EST`/`EDT` for
  the zones whose en-US names CLDR gives (the US ones) and `GMT-5` for the
  rest, which is CLDR's own rule for en-US; `long` gives CLDR's long names
  for the same set and `GMT-05:00` for the rest; `shortOffset`,
  `longOffset`, `shortGeneric`, `longGeneric` as CLDR defines them.

## 6. Cost

- **Size.** streams.js is 146 KiB of source. I estimate Intl at 60–90 KiB of
  JavaScript (the formatters, the option machinery, and en-US data
  written as JS literals: month and day names, the date patterns per
  skeleton, plural rules, currency symbols, compact suffixes, units) plus
  the C zone table. To be measured, not promised.
- **Each realm, measured before chosen.** Compiled once a process and
  kept as bytecode, as streams.c keeps its own (STREAMS.md § 8: a realm
  then reads bytecode in about a thirtieth of the compile), and installed
  EAGERLY if the measurement allows: the budget is about a millisecond and
  a few hundred KiB a realm on the P5. Lazy only if a realm pays more, and
  then real constructors and prototypes installed eagerly with only the
  data loaded on first use, never stub constructors: a page reading
  `Intl.DateTimeFormat.prototype.format` before constructing, the order of
  `Intl`'s properties and `Function.prototype.toString` would all tell a
  stub from the real thing, and a load that fails inside `new
  Intl.DateTimeFormat` mid-page is a failure path no page expects.
  (Fable's finding 9, the § 9.3 ruling.)
- **Measured (A1, 2026-11-04, natively on the dev machine, a realm reading
  the kept bytecode):** all eager, the install cost 5 ms and 660 KiB a
  realm, nearly all of it the script's own tables (the pattern map) and
  CLDR's data, over budget. So the ruled fallback: the constructors and
  prototypes are installed eagerly and whole (0.4 ms, 195 KiB), the
  pattern and interval tables are built by the first format that needs
  them, and the data (`intl_data.js`) is evaluated by a realm's first
  construction (about 0.8 ms and 690 KiB with the tables, in a realm that
  formats). A refused allocation in that first construction is the
  runtime's sticky failure like any other, swept in libjs's suite. The
  first compile in a process costs about 6 ms for the script and 5 ms for
  the data.
- **Measured (A2's NumberFormat and PluralRules, 2026-11-05, the same
  way):** the script is 163 KiB of source and 111 KiB of kept bytecode,
  the data 177 KiB and 133 KiB, each under the 512 KiB the process keeps.
  The install now costs 0.44 ms and 302 KiB a realm (A1's was 0.32 ms and
  201 KiB, measured beside it): reading the script's bytecode is most of
  it, and it grows with the script. The number tables (307 currencies, the
  units, the plural rules) are a deferred part of the data, written as a
  function the first NumberFormat or PluralRules calls, so a realm that
  formats only dates builds none of them: its first construction costs
  1.2 ms and 563 KiB (A1's 476 KiB; the deferred function's own bytecode
  is the difference), and a realm whose first construction is a
  NumberFormat 1.6 ms and 709 KiB. Two levers are measured and not pulled:
  stripping the script's line numbers saves 64 KiB of the install (at the
  cost of the frames a failure inside Intl reports), and a separate
  compiled script for the number tables would save the deferred
  function's 87 KiB in a dates-only realm.
- **Measured (A2's RelativeTimeFormat and ListFormat, 2026-11-05, the same
  way):** the script 174 KiB of source and 119 KiB of bytecode, the data
  187 KiB and 137 KiB. The install costs 0.52 ms and 318 KiB a realm.
  RelativeTimeFormat's fields are in the deferred number tables, which its
  first construction builds too (1.9 ms and 735 KiB as a realm's first);
  the list patterns are small and in the data proper, so a realm's first
  ListFormat costs what its first DateTimeFormat does (1.3 ms, 569 KiB).
- **Measured (A2's Collator, 2026-11-05, the same way):** the script 191
  KiB of source and 128 KiB of bytecode, the data 202 KiB and 148 KiB.
  The install costs 0.54 ms and 335 KiB a realm. The collation (815
  characters' elements and the root's contractions) is a deferred part of
  the data of its own, built by a realm's first Collator (2.3 ms and 759
  KiB as a realm's first construction, with its dense table). Each element
  is written as one number, not an array: as arrays the table cost a realm
  200 KiB more. A compare walks both strings' primaries in step and stops
  at the first that differs: sorting 10,000 random words costs about
  176 ms natively, 1.3 µs a compare (a plain JavaScript comparator, 8 ms;
  Chrome's ICU, under 10 ms in Node), and two 100,000-character strings
  that differ at the front compare at once.
- **The page's bytecode cache.** A kept blob is keyed by the engine's
  build identity, a hash over upstream and the libjs patch series
  (BYTECODE_CACHE.md). Intl's script is not among those sources, and a
  compiled script names a global by its atom, not by what the realm holds
  under it, so a blob stays good with Intl present or absent. The Intl
  bytecode itself is process memory, as streams.js's is, and is not in the
  page cache.

## 7. What it does not do

- Locales beyond English: `en-US` data, with `en` and `en-GB`/`en-CA`/
  `en-AU` resolving to `en-US` (a page asking for British dates gets US
  ones, as one asking for German gets English). A second locale is a data
  file and a census, not code; booked.
- Calendars other than Gregorian, numbering systems other than Latin
  digits; `supportedValuesOf` says exactly which.
- Segmenter, DisplayNames, DurationFormat (§ 4).
- Historical time-zone rules (§ 5).
- An IANA name in the system's `TZ` (§ 5; libos64's, booked).
- Collation beyond Latin and its marks (§ 4.1; full DUCET, booked).
- Likely subtags beyond English and `und`: `maximize` and `minimize`
  answer the tag itself for a language the data lacks (§ 4).
- Changing what `Date.prototype.toString` prints (ECMA-262's, not 402's).

## 8. Proof

- **test262's `intl402/`** is ECMA-402's conformance suite, as WPT is the
  platform's. libjs has no test262 runner (VALIDATION.md keeps 59 upstream
  functions, not test262). A1 adds one (`tools/test262_host.sh`, the
  checkout pinned by `tools/test262_fetch.sh`): each test run as written
  and strict through the real library, held to one expectation file per
  directory (`tools/test262/expected/`) that lists only what is excluded,
  each with its reason; an excluded test that passes fails the run, so
  the list cannot go stale. A1 runs `intl402/` for what it ships; each
  later cut adds its directories (A3 the redefined built-ins under
  `built-ins/`). The gate: every test in a shipped constructor's
  directory passes, except those that need what the data or the engine
  does not hold (a locale, calendar, numbering system, historical zone,
  Temporal), a later cut's surface, or a host hook, each listed with its
  reason.
- **The census's own calls, as fixtures:** every literal option bag § 1
  found, formatted against a fixed date and number, compared BYTE FOR BYTE
  with the string Chrome produces for it (captured once into a file in the
  tree, as W1's captures were). This is the "formats like Chrome" line,
  held by test. Byte for byte matters: CLDR puts U+202F NARROW NO-BREAK
  SPACE before `AM`/`PM` and U+2009 THIN SPACE around a range's en dash,
  and engines have disagreed about writing them. **Chrome's capture
  settled it** (Chrome 156, 2026-11-04, `tools/intl_captures/`): Chrome
  writes an ASCII space for both, in `format`, in `formatToParts` and in
  every range, and libjs does the same (Node 22's older V8 still keeps
  them in parts and ranges, which is why a capture and not a V8 build is
  the oracle). `tools/test_intl_capture_host.sh` holds libjs to that
  capture's 5,635 fields, unfolded, so a wrong space fails. (Fable's
  finding 5.) The capture page asks NumberFormat's and PluralRules'
  cases too (a matrix of styles by modifiers, a seeded random sample of
  option sets, values that are Numbers, BigInts and strings, ranges,
  parts and plural categories), and RelativeTimeFormat's and ListFormat's
  (every style, units and values about now, lists of every length); the
  Chrome 156 capture in the tree predates them, so they are held by the
  next one, and until then by test262 and by § 9.6's rule.
- **The glass:** `tools/pages/intl.html` shows the strings in yonder,
  checked against Chrome's, on QEMU and the P5. Since Intl writes only
  ASCII spaces, yonder's boxes for U+2000-block spaces (DEBTS) are not on
  its path.
- **The three GitHub pages, on QEMU and the P5:** each draws (after A2,
  § 9); the record carries no `Intl` miss.
- **The corpus census rerun** before A2 is cut, with module sources kept
  (below), and after A3: no script dies on Intl.

**The module keep is not this slice's** (finding 8): yonder keeps a
classic script's source in the record's folder (`script_arrived` →
`diag_keep`), but a module and everything it imports arrive through the
module loader and are never kept, so GitHub's records hold 33 stylesheets
and no script. Keeping module sources too (the same index, a `module`
kind) is a few lines of yonder that touch nothing Intl builds: its own
small PR, merged first, so the census before A2 sees GitHub's modules
(the compact NumberFormat and the Collator of finding 1 were found by
hand because the census could not).

## 9. The cut, and the rulings

Fable reviewed this design on 2026-11-04 (INTL_REVIEW.md, untracked):
build it, with ten findings, all folded in above. Every ruling is
made:

1. **Fidelity (Chris RULED 2026-11-04; Fable concurred).** Chrome's strings for en-US,
   held byte for byte by captured fixtures (§ 8), over CLDR data with a
   named version (§ 4.2); where Chrome and Firefox disagree, Chrome.
2. **Time-zone history (Chris RULED 2026-11-04; Fable concurred).** Current rules; the
   transitions booked with their 400 KiB named (§ 5).
3. **Lazy loading (Fable's, ruled).** Measure first; eager if a realm
   pays under the budget; otherwise real constructors and prototypes with
   only the data loaded on first use; never stub constructors (§ 6).
4. **The switch (Chris RULED 2026-11-04; Fable concurred).** In yonder, behind
   `modern = on` like every slice; with it off, a page sees today's
   QuickJS (no `Intl`, today's `toLocale*`). The `js` command installs it
   unconditionally: a shell has no Modern Web switch and no classic page
   to protect.
5. **The cut (Fable's finding 1, ruled).** DateTimeFormat alone does not
   draw GitHub's pages: their modules construct NumberFormat (compact),
   Collator and ListFormat at module scope, and primer-react's
   RelativeTime constructs RelativeTimeFormat and Locale in render. So,
   each PR merged before the next starts (Chris, 2026-11-04: no long
   chains), after the module keep's own small PR (§ 8):
   - **A1:** the realm door and the zone door, the option machinery,
     Locale, DateTimeFormat with the zone table, and the test262 runner
     (the gate exists before the first formatter merges).
   - **A2:** NumberFormat, PluralRules, RelativeTimeFormat, ListFormat,
     Collator. They share A1's machinery, and together they are what a
     page constructs. **GitHub's pages draw after A2.** Built as three
     stacked PRs (Chris, 2026-11-05: "stacking PRs if necessary"):
     NumberFormat and PluralRules, then RelativeTimeFormat and ListFormat
     (built on them), then Collator.
   - **A3:** the redefined built-ins (`toLocale*`, `localeCompare`),
     `getCanonicalLocales`, `supportedValuesOf`.

   Each constructor ships whole or not at all, at every step.
6. **Where V8 and ECMA-402 differ (A2).** Chrome formats numbers
   through ICU, and in places it answers otherwise than ECMA-402 or
   test262 does. Chrome's capture is the oracle (ruling 1), and the one in
   the tree predates NumberFormat, so until the next one the rule is:
   test262 where it tests the behaviour, V8's answer (Node 22's, the V8
   at hand) where it is silent; a Chrome capture overrules both. Node 22's
   V8 is older than test262's pin in three places, and libjs follows
   test262 in each: PluralRules' `notation` option (ECMA-402 2025); a
   `roundingIncrement` with no fraction digits given, which ECMA-402 now
   rounds to whole numbers where Node's V8 throws; and a currency's own
   digits, the default in standard notation only. Where test262 is
   silent, libjs answers as V8 does, each written at its code: an
   accounting `currencySign` with `signDisplay: "never"` reports
   `"standard"`; a `roundingPriority` with trailing zeros stripped reports
   `"auto"`; PluralRules resolves `en-US` to `en`; a plural range CLDR
   does not list is `other`; a range's approximately sign is never written
   into an accounting negative; a range's two ends are one value when
   they were equal before rounding as the same kind of input (Numbers
   with Numbers, strings and BigInts with each other, a zero equal to a
   zero whatever its sign) or are equal as rounded, sign and all; a
   rounding that carries a value to the next exponent formats the rounded
   value there (999.9 compact by increments of 2 is "2K", where ECMA-402
   rounds the original again and writes "0K"); `formatRangeToParts`
   reports the parts beside a collapsed currency code's spacing as ICU's
   spans place them, one character early; RelativeTimeFormat's `numeric: "auto"`
   finds its words ("today", "tomorrow") for a value that rounds to a
   whole -2..2 at hundredths, so 0.004 days is "today" (ECMA-402 matches
   the exact value), and rounds its number half to even, as ICU's
   default formatter does ("in 1 day" for 1.0005); and ListFormat's
   `formatToParts` writes no part for an empty element, joining the text
   around it into one literal. One
   departure is deliberate: V8 writes a percent per a unit in the short and
   narrow widths as the percent alone ("2%" for `percent-per-second`) yet
   keeps the denominator in its compact and range forms ("12K%/s",
   "1–5%/s"), which is ICU contradicting itself; libjs composes it as CLDR
   does everywhere ("2%/s").
   Where test262 speaks against V8, libjs follows test262 as the rule
   says: a Collator whose `caseFirst` option overrides a `-u-kf` in the
   locale resolves without it ("en", where V8 keeps "en-u-kf-upper").
