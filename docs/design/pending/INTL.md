# INTL.md — ECMA-402 for yonder's pages

*Opus, 2026-11-04. Slice A of GITHUB_WALLS.md (Fable's, same day): Intl is
the wall that empties all three of the GitHub pages before they draw, by a
module-scope `new Intl.DateTimeFormat(...)` that aborts a megabyte-long
module half made. A design for Fable's review and Chris's rulings (§ 9);
no code until both.*

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
- **The C underneath is small.** Three things JS cannot do well or at all:
  the time-zone table (§ 5), the host's time-zone name, and nothing else.
  Everything else is JavaScript over intrinsics captured at install, as
  streams.js captures its own, so a page that replaces `Array.prototype.push`
  or `Date.prototype.getUTCFullYear` cannot change what Intl does.

## 4. What ships, by constructor

The cut is by demand (§ 1), and each constructor ships whole or not at
all: an absent constructor is a feature test's honest `undefined`
(`Intl.Segmenter` was absent from Firefox until 2024, and pages test for
it); a present one that misbehaves is rule 1's worse case.

| Ships | Why |
|---|---|
| `Intl.DateTimeFormat` — `format`, `formatToParts`, `formatRange`, `formatRangeToParts`, `resolvedOptions`, `supportedLocalesOf`; every option, `dateStyle`/`timeStyle` and the field bags, `timeZone`, `hour12`/`hourCycle`, `timeZoneName` (all six values), `era`, `dayPeriod`, `fractionalSecondDigits` | 57 hosts, the GitHub wall |
| `Intl.NumberFormat` — `format`, `formatToParts`, `formatRange`, `formatRangeToParts`, `resolvedOptions`; decimal, percent, currency (ISO 4217's codes and their symbols in en-US), unit (the sanctioned simple units), compact notation, scientific and engineering, every digit and rounding option, `signDisplay`, `useGrouping` | 43 hosts |
| `Intl.PluralRules` — cardinal and ordinal, the digit options, `select`, `selectRange`, `resolvedOptions` | 18 hosts; NumberFormat's compact and RelativeTimeFormat need its rules anyway |
| `Intl.RelativeTimeFormat` — `format`, `formatToParts`, `numeric`, `style` | 18 hosts, GitHub's relative dates |
| `Intl.ListFormat` — the three types and three styles | 21 hosts |
| `Intl.Collator` and `String.prototype.localeCompare` through it — `usage`, `sensitivity`, `ignorePunctuation`, `numeric`, `caseFirst` | 72 hosts call `localeCompare` with arguments |
| `Intl.Locale` — parsing and canonicalizing a BCP 47 tag, its parts, `maximize`/`minimize` for the English tags | 15 hosts; every constructor's locale argument is parsed the same way |
| `Intl.getCanonicalLocales`, `Intl.supportedValuesOf` | small, and the second answers truly what the data holds |
| The redefined built-ins: `Date.prototype.toLocaleString`/`DateString`/`TimeString`, `Number.prototype.toLocaleString`, `BigInt.prototype.toLocaleString`, `Array.prototype.toLocaleString` (unchanged in shape, its elements' now honoured) | 32 hosts; as § 1 says, wrong silently today |

| Booked, absent (a feature test answers `undefined`) | Why it waits |
|---|---|
| `Intl.Segmenter` (6 hosts) | grapheme and word breaking is Unicode's segmentation data, a different and larger table; a page that tests for it falls back |
| `Intl.DisplayNames` (9 hosts) | names of languages, regions, currencies and scripts: data, not formatting; booked with a second locale |
| `Intl.DurationFormat` (1 host) | the newest constructor, one host; built on NumberFormat, so cheap when a page asks |

## 5. Time zones

The hard part, because pages pass zone names and read them back:

- **Names.** IANA names, from the build machine's tzdata at build time:
  every zone and link `/usr/share/zoneinfo` holds (about 450 here), each mapped
  to the POSIX rule its TZif file's footer carries (`America/New_York` →
  `EST5EDT,M3.2.0,M11.1.0`). A generated table, a few tens of KiB,
  compiled into libjs. A name not in it is a `RangeError`, as other
  browsers throw for one they do not know, so a page's typo behaves as it
  does elsewhere.
- **The host's zone.** `resolvedOptions().timeZone` with no `timeZone`
  must be an IANA name. os64's `TZ` is a POSIX string; the table maps it
  back by rule (`EST5EDT,M3.2.0,M11.1.0` → `America/New_York`, the
  canonical zone first with that rule; tzdata's own `EST5EDT` zone where
  the string is that bare legacy form). A `TZ` no zone carries resolves to
  `Etc/GMT±N` for its standard offset, as Chrome does for an unnamed
  offset; no `TZ` is `UTC`.
- **History is the honest gap.** A footer is the zone's CURRENT rule. A
  date from before the rule took effect (US 2007, the EU's 1996 change, a
  country that has since moved zones) formats with today's rule, off by an
  hour across a DST change that has since moved. Dates near now, which is
  what pages format, are right. The full transitions (the TZif body) are
  about 400 KiB for every zone; booked, with the cost named, for the day a
  page shows a wrong historical hour. QuickJS's own `Date` already has the
  same gap, so Intl and `Date` agree.
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
- **Each realm.** Compiled once a process and kept as bytecode, as
  streams.c keeps its own (STREAMS.md § 8: a realm then reads bytecode in
  about a thirtieth of the compile). And LAZY: the realm installs `Intl` as
  an object whose constructors are small stubs, and the first `new
  Intl.X` (or the first redefined built-in called with arguments) reads
  the full bytecode. A page that never formats pays only the stubs.
  Whether the stubs are honest (each a real constructor with the right
  `length`, `name` and prototype, so nothing observable changes when the
  rest arrives) is a § 9 question for Fable.
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
- Changing what `Date.prototype.toString` prints (ECMA-262's, not 402's).

## 8. Proof

- **test262's `intl402/`** is ECMA-402's conformance suite, as WPT is the
  platform's. libjs has no test262 runner (VALIDATION.md keeps 59 upstream
  functions, not test262). The slice adds one: a host runner over a pinned
  test262 checkout, read like the WPT runner (expectations per directory,
  required and passing sections, lost/new reporting), run for `intl402/`
  and for `built-ins/Date/prototype/toLocale*` and the other redefined
  built-ins. The gate: every test in a shipped constructor's directory
  passes, except those that need a locale, calendar, numbering system or
  historical zone the data does not hold, each listed with its reason.
- **The census's own calls, as fixtures:** every literal option bag § 1
  found, formatted against a fixed date and number, compared with the
  string Chrome produces for it (captured once into a file in the tree,
  as W1's captures were). This is the "formats like Chrome" line, held by
  test.
- **The three GitHub pages, on QEMU and the P5:** each draws; the record
  carries no `Intl` miss.
- **The corpus census rerun after** (with the module keep below): no
  script dies on Intl.

**A census tool fix rides with the slice:** yonder keeps a classic
script's source in the record's folder (`script_arrived` → `diag_keep`),
but a module and everything it imports arrive through the module loader
and are never kept. GitHub's pages are all modules, so their records hold
33 stylesheets and no script. Keeping module sources too (the same index,
a `module` kind) is a few lines in yonder and makes the next census see
the sites that matter most.

## 9. Rulings and questions

1. **Fidelity (Chris's).** My lean: like Chrome for en-US, held by the
   captured fixtures; where Chrome and the spec disagree (CLDR versions
   drift), Chrome's string, because that is what page authors tested
   against.
2. **Time-zone history (Chris's).** My lean: current rules now, the
   transitions booked (§ 5).
3. **Lazy stubs (Fable's).** Are constructor stubs that load the rest on
   first use an honest cut, or does something observable break (a page
   reading `Intl.DateTimeFormat.prototype` before constructing; property
   order on `Intl`)? My lean: install the real prototypes and constructors
   eagerly (they are small) and load only the formatting machinery and
   data lazily, so nothing observable depends on when it arrived.
4. **The switch (Chris's).** Intl is language, but it is new surface this
   arc adds, and MODERN_WEB.md rule 4 says one switch covers the arc's
   surface. My lean: behind `modern = on`, like every other slice; with it
   off, a page sees today's QuickJS (no Intl, today's `toLocale*`).
5. **Where the slice ends.** My lean: three PRs in order, each merged
   before the next starts (Chris, 2026-11-04: no long chains):
   (A1) the realm door, the option machinery, Locale, DateTimeFormat with
   the zone table, the date `toLocale*` built-ins, the test262 runner and
   the module keep — GitHub's pages draw after this one;
   (A2) NumberFormat, PluralRules, `Number`/`BigInt.toLocaleString`;
   (A3) RelativeTimeFormat, ListFormat, Collator with `localeCompare`,
   `getCanonicalLocales`, `supportedValuesOf`.
   A1 alone leaves `Intl` without NumberFormat, which a page tests with
   `Intl.NumberFormat` and finds absent; rule 1 is per interface, and each
   constructor is one.
