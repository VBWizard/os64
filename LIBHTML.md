# LIBHTML.md — bytes in, a tree out

*The design record for `libhtml`, os64's HTML parser. Written 2026-09-11 at
the start of the browser arc as the spec Codex builds from, and tightened
the same morning on her first read (encoding precedence, the document
node, the arena budget, resource failure, and the foreign-content ruling
are hers). BROWSER.md is the campaign constitution; this file owns the
parser and nothing above it.*

## The ruling

**libhtml turns the bytes of a page into the tree the HTML Standard
specifies for those bytes, and does nothing else.** No fetching, no
rendering, no style, no scripts. A stream of bytes goes in through a push
interface, and a document tree comes out that every consumer walks the
same way: the line-mode browser, the graphical browser after it, and
whatever else one day needs to read a page.

**It is a WHATWG-shaped parser, not a "simple tag soup" parser, and the
reason is the test suite.** The web is tag soup, and browsers turn the
same soup into the same tree because they all run the one algorithm the
HTML Standard writes down. That algorithm has a public reference test
suite with thousands of inputs and their expected trees. A homemade "good
enough" parser is exactly the thing that cannot be diffed against
anything, and whose bugs then arrive one page at a time, each one a
morning. A spec-shaped parser is diffed once, on the host, in seconds.
That is what makes this Codex work: the same reference-diffable property
that made gzip, PNG and JPEG hers.

**Scope is cut by MODE, never by approximation.** Where the standard has a
part we cannot run, the whole part is out and the tests that exercise it
are skipped by name. Everything else is in exactly, so its tests pass
exactly. There is no "mostly like the spec" middle, because the middle is
the part nobody can test — and because a part that is approximated changes
the tree AROUND it, not just inside it (an SVG parsed as HTML swallows the
paragraph that should have broken out of it). The first draft of this file
approximated foreign content and templates; it was wrong by its own rule.

**What passing proves.** A parser that passes the pinned suite conforms to
the pinned suite. That is strong evidence and it is not browser
equivalence: browsers carry behaviour the suite does not cover, and the
standard moves. The claim this file makes is the one the harness can
check, and the harness pins the standard's revision alongside the tests
so the claim has a date.

## Where it sits

`userland/libhtml/` builds `/lib/libhtml.so`, prelinked like every library
(`app_bases.py --libs`, `link/lib.ld`). Its only dependency is `libos64.so`,
for the heap and strings. It never learns about `libfetch`, `libtls`,
`libgzip` or a window: a page is bytes by the time it arrives, whatever
carried them. LIBIMAGE.md's ruling applies word for word — libos64 provides
the world a parser needs and does not become the parser.

```text
line-mode browser / graphical browser / a future `html2text`
                        |
                   libhtml.so
                        |
                   libos64.so
```

Names carry the `os64_html_` prefix, the same way libpng's carry
`os64_png_`.

## What goes in

Bytes, pushed in whatever pieces the transport delivers them:

```c
os64_html_parser_t   *os64_html_parser_new(const os64_html_options_t *opt); // NULL = heap or initial arena budget failure
int64_t               os64_html_parser_feed(os64_html_parser_t *p,
                                            const void *bytes, size_t len);
os64_html_document_t *os64_html_parser_finish(os64_html_parser_t *p);
void                  os64_html_parser_destroy(os64_html_parser_t *p);      // abandon: no document
void                  os64_html_document_free(os64_html_document_t *doc);
```

- **`feed` returns 0 or a refusal by name**, and a refusal is final: the
  parser accepts no more input, and `finish` returns the document built so
  far with `doc->refusal` carrying the same name. The names:
  `OS64_HTML_TOO_LARGE` (the input passed `max_bytes`),
  `OS64_HTML_ARENA_EXHAUSTED` (the tree passed `max_arena_bytes`),
  `OS64_HTML_TOO_DEEP` (a start tag would push the open-elements stack
  past `max_depth`), `OS64_HTML_WORK_EXHAUSTED` (the parse passed
  `max_work` steps), `OS64_HTML_NO_MEMORY` (the heap said no). A page cut
  short is still a page, so a refused parse still yields a tree.
- **The byte limit is a prefix, exactly.** The parser processes precisely
  the first `max_bytes` bytes, then behaves as if the input ended there;
  later bytes are discarded and `doc->truncated` is set. A resource refusal
  while processing that prefix or its EOF takes precedence over `TOO_LARGE`. The tree for a
  limit-hit parse is identical whether the limit fell mid-chunk, on a
  chunk boundary, or was crossed by a single oversized feed.
- **`finish` ends the input** (the standard's EOF steps run — and those
  steps can allocate, so `finish` can hit the arena or heap limit too; the
  document it returns then carries the refusal and the tree up to that
  point). `finish` frees the parser. **`destroy` frees the parser without
  producing a document**, for the download that was cancelled at byte
  40,000 — the parser was born under a fetch, and fetches are interrupted
  (`OS64_INTERRUPTED` is a first-class answer in this OS).
- **A partial document is a whole document to its consumer.** Whatever
  `finish` returns — after a refusal, after a truncation, after an EOF that
  ran out of memory halfway through popping the stack — is safe to walk
  and safe to free. There is no state in which a returned document dangles.
- **Chunk boundaries are invisible.** The tree for a page fed one byte at a
  time is identical to the tree for the same page fed whole. This is the
  ansi.c lesson (a sequence arrives in whatever pieces `write()` was called
  with, and the harness caught four real bugs before the kernel ever ran
  it), and it is tested the same way: every reference input is also run
  byte-by-byte and in random chunk sizes.
- **`options`:** `charset` (the label from the HTTP `Content-Type`, or
  NULL); `max_bytes` (default 8MB — a page, not a disk image);
  `max_arena_bytes` (the tree's budget, default 64MB — see Bounds for why
  it cannot be derived from the input); `max_depth` (open elements;
  default 512, the cap the major engines ship); `max_work` (steps, see
  Bounds for what a step is and why bytes do not bound time). Passing NULL
  selects defaults; otherwise initialize with `os64_html_options_default()`
  and override fields. Supplied zero limits mean zero, including an empty
  byte prefix. The constructor copies the charset label. Nothing else. **Scripting
  is OFF and not an option:** os64 runs no JavaScript, so the parser takes
  the standard's scripting-disabled branches — `<noscript>` content is
  parsed as markup and shows, and the "in head noscript" insertion mode
  is live.

## Character encoding

The tree is **UTF-8, always**. That is decided today, before a byte is
written, because the graphical browser needs Unicode text and every table
built for Latin-1 would be rebuilt the week it arrives. The line-mode face
folds UTF-8 to its terminal's Latin-1 or CP437 at its own edge (that
transliteration table is the FACE's, and the `os64_utf8_*` decode helpers
it will want belong in libos64's str.h — booked below, not libhtml's).

The input encoding is decided once, before tokenizing, by the standard's
own precedence:

1. **A byte-order mark** — UTF-8, UTF-16LE, UTF-16BE. The BOM outranks
   everything, including the transport, because it is the file itself
   saying what it is, and a server's default header is the commonest lie
   on the web.
2. The caller's `charset` label — the transport's `Content-Type`.
3. The standard's **prescan** of the first 1024 bytes for `<meta charset>`
   or `<meta http-equiv="content-type">`. The parser holds those bytes
   until it has 1024 of them or `finish` arrives, THEN starts tokenizing.
4. **Windows-1252**, the standard's default for the legacy web. ISO-8859-1
   and US-ASCII labels also mean Windows-1252, as the standard says and as
   every browser does — the difference is the 0x80..0x9F row, which 1990s
   pages full of curly quotes rely on.

**DELIBERATE DEVIATION, written down and tested: the encoding is frozen
after step 3.** The standard calls the prescan's answer *tentative* and
lets a `<meta charset>` found later in the document change it, which in a
browser means re-navigating or reparsing from the top. We do neither: a
late meta that disagrees with the frozen choice is recorded
(`doc->charset_late_meta` carries its label) and otherwise ignored. The
harness has a case for it — a 1252-decoded page with a UTF-8 meta at
beyond byte 1024 — so the deviation is a tested fact, not a surprise.

Decoders in this version: UTF-8 (an invalid sequence becomes U+FFFD by the
standard's rules, never a crash and never a dropped byte), Windows-1252,
UTF-16 under a BOM. **Any other label is refused by name into the
document** — `doc->charset_unsupported` carries the label, the bytes are
decoded as Windows-1252 so the ASCII half of the page still reads, and the
face can tell the user what happened. Shift_JIS, GBK, KOI8-R and the rest
arrive when a page demands one (consumer-driven, the roadmap rule).

After decoding, the standard's **input stream preprocessing** runs: CRLF
and lone CR become LF, and the tokenizer's rules turn NUL into U+FFFD
where they say so. "Verbatim" everywhere below means verbatim AFTER that
step. The tree holds no NULs, so every string in it is also a C string.

## What comes out

A document, its nodes carved from **one arena that `document_free` releases
in one call**. No node is freed alone, no consumer keeps a node past the
document, and a page's whole cost is one number.

```c
typedef enum { OS64_HTML_DOCUMENT, OS64_HTML_FRAGMENT, OS64_HTML_DOCTYPE,
               OS64_HTML_ELEMENT, OS64_HTML_TEXT, OS64_HTML_COMMENT } os64_html_node_kind_t;
typedef enum { OS64_HTML_NS_HTML, OS64_HTML_NS_SVG, OS64_HTML_NS_MATHML } os64_html_ns_t;
typedef enum { OS64_HTML_NO_QUIRKS, OS64_HTML_LIMITED_QUIRKS, OS64_HTML_QUIRKS } os64_html_quirks_t;

typedef struct os64_html_attr {
    const char *name;               // UTF-8; lowercased for HTML, case-adjusted for foreign
    const char *value;              // UTF-8, entities decoded, "" when absent
    const char *ns;                 // NULL, or the foreign attribute's namespace (xlink:, xml:)
    struct os64_html_attr *next;    // source order
} os64_html_attr_t;

typedef struct os64_html_node {
    os64_html_node_kind_t kind;
    os64_html_ns_t    ns;           // ELEMENT: the namespace it was inserted in
    os64_html_tag_t   tag;          // ELEMENT: the known HTML tag, or OS64_HTML_TAG_UNKNOWN
    const char       *name;         // ELEMENT: the tag name (lowercased HTML / case-adjusted foreign)
                                    // DOCTYPE: the doctype name
    const char       *public_id, *system_id;   // DOCTYPE: NULL when absent; the API distinguishes "" from absent
    const char       *text;         // TEXT / COMMENT: UTF-8, NUL-terminated
    size_t            text_len;
    os64_html_attr_t *attrs;        // ELEMENT
    struct os64_html_node *template_contents;  // <template> only: an OS64_HTML_FRAGMENT node, the standard's DocumentFragment
    struct os64_html_node *parent, *first_child, *last_child, *prev, *next;
} os64_html_node_t;

typedef struct os64_html_document {
    os64_html_node_t *document;     // the DOCUMENT node: doctype, comments and <html> are its children
    os64_html_node_t *html;         // the <html> element, for convenience; never NULL (storage reserved at construction)
    os64_html_node_t *head, *body;  // convenience; nullable after refusal; body is also NULL for framesets
    os64_html_quirks_t quirks;      // the doctype's verdict, by the standard's table
    int64_t  refusal;               // 0, or the OS64_HTML_* name the parse ended on
    bool     truncated;             // the byte limit cut the input
    const char *charset;            // the encoding used, canonical label
    const char *charset_unsupported;// the label refused, or NULL
    const char *charset_late_meta;  // a meta past the prescan that disagreed, or NULL
    size_t   node_count, arena_bytes, input_bytes;
    size_t   peak_arena_bytes;      // maximum live charged storage, including scratch
    uint64_t work;                  // consumed work units
    size_t   parse_errors;          // tokenizer and tree recovery diagnostics, counted
    os64_html_parse_error_t first_errors[16];   // {byte offset, name} — an instrument, never a verdict
} os64_html_document_t;

const os64_html_attr_t *os64_html_attr(const os64_html_node_t *e, const char *name);
os64_html_tag_t         os64_html_tag_from_name(const char *name);
const char             *os64_html_tag_name(os64_html_tag_t tag);
const char             *os64_html_status_name(int64_t status);
```

- **The document node is real**, with children: the doctype, any
  document-level comments (before `<html>` or after `</html>` — the
  reference dumps show them there), and the `<html>` element.
  `doc->html` is a convenience pointer, not the root, and **it is never
  NULL, including on a document returned after a refusal**: the document
  node and the `<html>` element are carved from the arena when the parser
  is CONSTRUCTED (the constructor's NULL covers the case where even that
  fails), and a refusal that lands before the standard's "before html"
  mode has run inserts the reserved element exactly as the EOF steps
  would have. `head` and `body` carry no such promise — a refusal before
  they exist leaves them NULL, and a consumer checks. The doctype is a
  node because the reference format prints it and because its name and
  ids are what `quirks` was computed from; a consumer that wants only
  the page starts at `doc->html`.
- **Three quirks states**, because the standard has three and the
  graphical layout will honour limited-quirks differently from full.
- **`ns` names the namespace the standard inserted the element in.**
  Foreign elements keep their case-adjusted names (`foreignObject`,
  `clipPath`) and their namespaced attributes (`xlink:href`), exactly as
  the standard's adjustment tables say. **`os64_html_tag_t` names every
  HTML element the tree-construction algorithm itself switches on, plus
  the ones a renderer switches on** (headings, lists, `pre`, `br`, `hr`,
  `img`, `a`, the table family, the phrasing set, `form` and its
  controls). Foreign elements and unknown HTML elements are
  `OS64_HTML_TAG_UNKNOWN` with `name` and `ns` kept, so a consumer can
  still see what they were. A renderer that draws no SVG skips the `ns`.
- **`<template>` carries its contents on a separate branch**, the
  standard's document fragment: `template_contents` is a node whose
  children are the template's, and the template element's own child list
  is empty. Template contents are not part of the page; a renderer never
  walks that branch. They are parsed exactly because the "in template"
  insertion mode changes how the tokens AROUND a template are handled —
  a template inside a table would otherwise foster-parent its contents
  into the page.
- **Text is verbatim** (after preprocessing, above). Whitespace is
  preserved exactly; collapsing runs of it is the renderer's job, and
  `<pre>` is why. Adjacent text is merged into one node, as the
  standard's insertion does.
- **Comments are kept.** They cost an arena slice and nothing else, and a
  renderer skips a kind it does not draw.
- **Attributes keep source order; a duplicate name keeps the first**, as
  the standard says. Values are verbatim after entity decoding.
- **The parse never fails on input.** Only resources refuse. Every
  malformed byte sequence has a defined outcome in the standard, and the
  standard's parse errors are COUNTED (with the first sixteen kept, by
  offset and name) as an instrument for a page-info view and for the
  harness, never as a reason to stop. Tripwires over silence, but a
  tripwire on somebody else's HTML is a report, not a refusal.

Everything the tree does NOT carry is deliberate: no style, no computed
anything, no back-pointer from nodes to byte offsets (the errors carry
offsets; nodes do not need them), no mutation API. A consumer that needs
to rewrite a tree builds its own.

## The algorithm: what is in, and what is out by name

**The tokenizer is complete.** Every state in the standard's tokenizer,
including the raw-text states (`script`, `style`), RCDATA (`textarea`,
`title`), `plaintext`, comments in all their malformed spellings, doctype
tokens in theirs, and CDATA sections (honoured in foreign content, a bogus
comment outside it — exactly the standard). Character references: **the
full named table** (2,231 names, generated into a C file from the
standard's `entities.json` by a script checked in beside it, so the table
can be regenerated and never hand-edited), numeric references with the
standard's replacement table for the 0x80..0x9F range and the rules for
surrogates, noncharacters and zero.

**Tree construction is complete for a document with scripting disabled.**
Every insertion mode the standard defines for that case, by name so the
inventory is checkable against the standard's own list: initial, before
html, before head, in head, in head noscript, after head, in body, text,
in table, in table text, in caption, in column group, in table body, in
row, in cell, in select, in select in table, in template, after body, in
frameset, after frameset, after after body, after after frameset. **Plus
the rules for parsing tokens in foreign content**, with the SVG and MathML
adjustment tables, the breakout list, and the integration points.

The parts worth naming because they are the hard ones, and they are in
whole: the list of active formatting elements with reconstruction (an
unclosed `<font>` or `<b>` that survives a `<p>` is the most common shape
on the old web); the **adoption agency algorithm** for misnested
formatting (`<b><i>x</b>y</i>`); foster parenting in the table modes (the
old web IS table layout, and foster parenting is what keeps a 1997 page's
stray text where its author saw it); foreign content's breakout (a `<p>`
inside `<svg>` ends the SVG — approximating SVG as HTML would have
swallowed it, and a self-closing `<svg/>` would have swallowed the rest of
the page).

**Out, by name, with the consequence stated:**

- **Scripting-enabled behaviour.** Tests marked `#script-on` are
  skipped; `#script-off` and unmarked tests run. This is not a cut, it is
  the truth about the machine.
- **XML-output coercion.** The four `xmlViolation.test` cases are skipped
  individually as `xml-output-coercion`. They test optional conversion to an
  XML infoset, including changes to text and comments that libhtml preserves.
  HTML tokenizer cases remain mandatory.
- **Fragment parsing.** Cases with a `#document-fragment` line drive the
  standard's fragment algorithm (`innerHTML`), which exists for scripts.
  Skipped by name; there is no fragment entry point.
- **The "change the encoding" restart** — the deviation above, tested.
- **`document.write`, `<isindex>` rewriting.** Not applicable; removed
  from the standard.

The skip list is a checked-in file, one line per skipped test naming the
suite file, the input's hash, and the reason from the list above. A test
that is skipped for a reason not on that list is a finding.

## Bounds

**The tree's size cannot be derived from the input's, so it has its own
budget.** Reconstruction of active formatting elements CLONES elements:
one page that opens a formatting element and then writes ten thousand
paragraphs reconstructs that element ten thousand times, and ten open
formatting elements make it a hundred thousand nodes — quadratic in the
input, by the standard's own design. (Noah's Ark bounds identical entries
in the formatting list at three; it bounds nothing about the tree.) So
`max_arena_bytes` is an ENFORCED budget on everything the parse allocates
— nodes, strings, the open-elements stack, the formatting list, the
pending-table-text buffer, the prescan hold — and crossing it is
`OS64_HTML_ARENA_EXHAUSTED`, a refusal like any other. The harness has an
adversarial-growth case that must refuse by name rather than exhaust the
host.

Every other quantity that grows is capped, and every cap is a refusal by
name — nothing is silently dropped:

- input bytes: `max_bytes`, refusal, prefix semantics as above.
- open-elements depth: `max_depth`, and **crossing it is a refusal
  (`OS64_HTML_TOO_DEEP`), not a dropped tag.** The engines drop the start
  tag and carry on, and the first draft of this file copied them; Codex
  pointed out what that does: with nested `<div>`s, the dropped inner
  `<div>`'s end tag closes the OUTER one, and the standard's end-tag
  rules make the consequence different for every element. That is silent
  divergence, page by page — the thing the whole design exists to refuse.
  So depth is a resource like bytes and arena, refused by name with the
  same partial-document guarantee, and a 513-deep page renders up to the
  point it went absurd.
- **execution time: `max_work`, because bytes do not bound steps.** The
  arena caps how much the parse BUILDS, not how much it LOOKS AT: a page
  that pushes a million distinct formatting entries makes every later
  push scan a million entries for Noah's Ark, quadratic time on linear
  memory, and the same shape hides in the stack walks ("any other end
  tag", foster-parent lookup, the adoption agency's inner searches). The
  honest per-token bound is O(depth + formatting list), depth is capped
  above, and the list is not — capping it would be an approximation that
  changes trees. So the parse COUNTS: tokenizer/tree dispatches and elements
  examined in stack, formatting, and attribute walks consume work units. List
  movements charge their entry count; variable-length attribute comparisons
  also charge compared-name/value lengths. Fixed grammar comparisons stop at
  the grammar word boundary. `max_work` is the budget,
  crossing it is `OS64_HTML_WORK_EXHAUSTED`. The default is set from the
  corpus measurement below (real pages are tiny by this measure; the
  budget exists for the page that is not) and written down with the
  number that justified it.

The library is `-fsanitize`-clean on the host under address and undefined
behaviour, on the whole reference suite, on the adversarial cases, and on
a fuzz pass (mutations of the suite's inputs: bit flips, truncation,
splicing two inputs, and repeated spans) for a 30-second wall-clock budget.
The separate prefix sweep covers every offset of each in-scope fixture. It is
the same bar PNG met.

## Proof before integration

`tools/test_html_host.sh` is the harness and the acceptance bar, in the
`test_png_host.sh` shape:

1. **The standard and suite, pinned to the approved historical baseline.**
   V1 uses WHATWG HTML revision `e981fc31912af57267fad15222f0add44625e87a`
   (2024-11-26) and the original html5lib tree/tokenizer fixtures at
   `a9f44960a9fedf265093d22b2aa3c7ca123727b9` (2023-08-17). Inputs, licenses,
   commits, and SHA-256 hashes live under `tools/html5lib-tests/`.
   `tools/update_html_fixtures.py` reproduces the import. Current WHATWG/WPT
   migration is deferred as recorded in Design changes below. **Inventory
   before code:** `INVENTORY.json` records per-file counts and `SKIPS.tsv`
   names each excluded case, so the acceptance denominator is reviewable.
2. **Tokenizer:** every HTML `.test` JSON case (the four explicitly named
   XML-output-coercion cases are excluded), honouring `initialStates`,
   `lastStartTag` and `doubleEscaped`, compared token for token.
3. **Tree construction:** every `.dat` case, our tree serialized in the
   suite's own format (attributes sorted for the comparison only; the
   tree keeps source order) and diffed against the expectation. The
   script prints the counts — run, passed, skipped — and FAILS on any
   non-skipped difference. The target is 100% of the in-scope tests, and
   "in scope" is the skip list, nothing softer.
4. **Chunking:** every case again, fed one byte at a time and in random
   chunk sizes from a printed seed, trees compared to the whole-feed
   tree. The byte-limit prefix rule is tested the same way: a limit
   placed at every offset of a case, across chunkings, one tree each.
5. **Resource failure:** allocation-failure injection (the host harness
   wraps the heap and fails the Nth allocation for every N) proving that
   every failure point yields a document that walks and frees cleanly, or
   a NULL constructor; the late-meta deviation case; a depth case that
   refuses by name at exactly `max_depth`.
6. **Adversarial work and growth, MEASURED:** the harness prints steps
   and arena bytes for every corpus page and for crafted inputs (the
   formatting-list flood, the reconstruct-per-paragraph page, the deep
   nest, the foster-parent flood), and each crafted input must refuse by
   its name within a stated wall-clock bound rather than run the host
   out of anything. The defaults for `max_work` and `max_arena_bytes`
   are justified by those numbers in a comment beside them.
7. **Fuzz** under the sanitizers for the budget above.
8. **A real-page corpus** under `tools/html_corpus/`: saved pages from
   the web the line-mode browser will actually visit — example.com, a
   Wikipedia article, the Hacker News front page, 68k.news, a
   textfiles.com page, a Floodgap gopher HTML page — with their tree
   dumps checked in beside them, so a change to the parser shows up as a
   reviewable diff to a tree and not as "a page looks different".

QEMU then proves the facts the host cannot: `/tests/htmltest` parses an
embedded page through the real `.so` on the real heap and checks a few
tree facts with a badge code — the library links, loads, allocates and
frees in ring 3.

## Booked before the first line (the known-debt rule)

| What | Why deferred | Trigger |
|---|---|---|
| Reading the tree DURING `feed` (progressive rendering) | v1 consumers render after `finish`; the push API already admits it without a signature change | the graphical browser wants first paint before last byte |
| More encodings (Shift_JIS, GBK, KOI8-R, ISO-8859-2..16) | consumer-driven; the ladder and the refusal are built so adding a decoder is one table | the first page that refuses by name |
| RENDERING foreign content and template contents | the tree carries them exactly; no face draws SVG yet | a page whose SVG must render |
| `os64_utf8_*` helpers in libos64 str.h (decode one code point, encode one, validate) | they are the FACE's need, and libos64 grows by consumer | the line-mode face's Latin-1/CP437 fold, the first customer |
| The line-mode face's transliteration table (UTF-8 → Latin-1 → CP437 fallback glyphs) | belongs to the face, not the parser | same slice as the face |
| Current WHATWG/WPT parser baseline | current processing-instruction semantics conflict with the legacy tokenizer fixtures; v1 uses the approved historical baseline | a separately reviewed migration with aligned token and tree expectations |
| The "change the encoding" restart | a browser re-navigates; we have no navigator inside the parser and the case is rare and recorded | a real page whose late meta matters, found via `charset_late_meta` |

## What the consumers owe

The renderer collapses whitespace, resolves `href` against the page's base
URL (the navigator's job — the parser does not know where the page came
from), decides what an unknown element means, skips the namespaces it does
not draw, and folds text to its glass. libhtml gives it the tree the
standard specifies, as far as a pinned suite can prove it. That is the
whole contract, and it is enough.

## Design changes during implementation

### 2026-09-11: exclude XML-output coercion (Chris approved)

The original acceptance requirement ran every tokenizer `.test` case. The
fixture inventory found four `xmlViolation.test` cases for the standard's
optional XML-infoset coercion mode. That mode changes HTML text and comments:
for example, it replaces a form-feed with a space. Running it would contradict
this library's preserved HTML output, and it has no HTML consumer. The revised
contract excludes that mode explicitly, records all four fixtures in
`tools/html5lib-tests/SKIPS.tsv`, and retains the mandatory HTML tokenizer
suite. No fixture input or expectation is rewritten to make it pass.

### 2026-09-11: compatible historical reference baseline (Chris approved)

The original design required current WPT tree-construction fixtures alongside
html5lib tokenizer fixtures and a matching standard revision. The initial
inventory exposed incompatible expectations: current WHATWG/WPT recognize
processing instructions, while legacy tokenizer fixtures require bogus
comments for those same inputs. One tokenizer cannot satisfy both.

Chris approved the recommendation to use the November 2024 standard snapshot
and the original html5lib tree/tokenizer fixtures for v1. The implementation
retains that snapshot's `in select` modes and bogus-comment processing of
`<?...>` syntax. The import records exact commits and file hashes. Migration
to the current standard and WPT is explicit follow-up work, not a conformance
claim about v1. The superseded inventory is replaced by the selected baseline's
inventory; reference expectations are not rewritten to fit the implementation.


### 2026-09-11: work accounting and observable API details

The original work definition counted stack and formatting-list visits. The
implementation also counts tokenizer/tree dispatches, attribute-list visits,
list movements, and variable-length attribute comparisons. Otherwise a page
could spend quadratic time comparing attributes without consuming the stated
budget. Unknown-name comparisons against grammar words are bounded by those
fixed words, and repeated foreign integration-point attribute searches are
metered. The parser still produces the reference tree until a named refusal;
work units are an implementation budget, not CPU instructions or elapsed time.

The original API sketch did not define zero-valued options or expose peak
storage/work measurements. `os64_html_options_default()` supplies explicit
defaults, NULL selects them, and a supplied zero is literal. This makes the
zero-byte prefix testable and avoids ambiguous partial initialization.
`peak_arena_bytes`, `work`, and `os64_html_status_name()` make the resource
contract observable. Constructor NULL includes an initial arena budget that
cannot hold the reserved parser/document, as well as heap failure.

The original diagnostic wording called all errors the standard's named errors.
The tokenizer uses those names; tree-construction recovery steps that the
standard leaves unnamed use libhtml diagnostic names. Offsets identify the
source byte triggering the diagnostic (the leading byte for decoded scalar
values, input length for EOF), not a source range. Counts and the first 16
records are diagnostic information, not a second conformance verdict.

The original sketch's comment claimed the fixture dump distinguishes absent
and empty doctype identifiers. The public tree preserves that distinction;
the upstream tree fixture format normalizes the pair when serializing it.
The header and document now state the API's actual distinction. Nodes and
attributes are read-only to consumers and live until `document_free`.
`node_count` includes allocated nodes detached by recovery; `arena_bytes`
includes retained capacity and allocator headers, rather than payload alone.
`charset` can be NULL if a resource refusal prevents encoding selection.

### Implementation evidence (2026-09-11)

The host harness verifies imported file hashes, regenerates the inventory in
memory to check the skip manifest, and checks generated tables without editing
them. It runs 8,596 mandatory cases (7,032 tokenizer state/case combinations and
1,564 document trees), with 204 explicit skips. Allocation-failure injection
covers 63,708 failure points; the byte-prefix/chunk sweep makes 391,602 checks.
Both use an iterative topology/UTF-8 validator and allocation ownership counts.
Every in-scope fixture also runs whole, bytewise, and with seed `0x64a11` chunks.

The sanitizer build uses `-O2 -fsanitize=address,undefined`. A 30-second mutation
pass uses seed `0x64f022`; its printed count is machine-speed dependent. Each
mutation runs with three chunkings and varied work, depth, and arena limits.
The suite also tests 26 encoding cases in three chunkings, plus a meta tag
straddling the prescan boundary and a later conflicting declaration.

Saved pages produce identical trees, diagnostics, work, and peak arena costs
in all three chunkings. Their checked-in dumps are regression snapshots from
libhtml, not independent browser-oracle results; the upstream fixtures supply
that independent comparison. Measurements on this host:

| Page | Input bytes | Allocated nodes | Work units | Peak arena bytes |
| --- | ---: | ---: | ---: | ---: |
| example.com | 559 | 20 | 949 | 7,776 |
| Floodgap | 19,945 | 890 | 36,009 | 284,256 |
| Hacker News | 34,938 | 1,297 | 52,428 | 293,632 |
| textfiles computers | 70,392 | 4,938 | 133,348 | 1,187,872 |
| 68k.news | 98,372 | 1,539 | 150,636 | 581,280 |
| Wikipedia HTML | 797,390 | 15,470 | 1,210,369 | 4,255,104 |

The defaults are 100,000,000 work units and 64 MiB arena, about 82x and 15x
headroom over the largest measured page. Unsanitized Wikipedia parsing took
54.5 ms at `-O0` and 23.6 ms at `-O2` before the final bounded-comparison audit;
that measurement motivated the library's explicit optimization flag. Host
numbers establish neither guest timing nor a universal page-size promise.

The crafted tests use explicit smaller limits to exercise each refusal:
100,000 work units for formatting floods, reconstruction, and attribute floods;
65,536 arena bytes for foster-parent growth; depth 512 and the exact depth-three
boundary; and 1,000,000 work units for repeated foreign attribute lookup. A
400,007-byte long-foreign-name case must finish successfully. The entire
adversarial batch, including three chunkings, has a 20-second deadline.

Run `ASAN_OPTIONS=detect_leaks=0 tools/test_html_host.sh` in ptrace environments
where LeakSanitizer cannot run. AddressSanitizer and UndefinedBehaviorSanitizer
remain enabled, and the harness independently requires zero live allocations
following every parse/failure/cancellation. Outside that environment the command
can omit the ASAN override. `HTML_FUZZ_SECONDS` changes the mutation budget.

`make -j4` builds the kernel, shared library, consumers, disk image, and ISO with
the repository's strict warnings. `readelf -d userland/bin/libhtml.so` shows only
`libos64.so` as a dependency; private parser symbols are hidden. `/tests/htmltest`
is registered in `testrun`, checks repeated parsing/freeing and cancellation on
the real heap, and returns badge `0x48640000` on success.

The optimized shared library passed the isolated QEMU test on q35 with 8 CPUs,
8 GiB RAM, and a private ext2-root disk. The guest library was extracted and
byte-compared with the built `libhtml.so` before boot. Both startup consoles ran
`/tests/testrun htmltest`; both reported `1 passed, 0 failed, 0 skipped`.
The kernel and libos64 required no source changes. Boot configuration and disk
edits were confined to `/tmp/os64-libhtml-qemu/`; the tracked boot configuration
was not changed. The private ext2 partition passed `e2fsck -fn` after shutdown.
`git diff --check` and `tools/stale_refs.sh` also pass.
