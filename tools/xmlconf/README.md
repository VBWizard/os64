# XML conformance gate, format 1

Source: https://www.w3.org/XML/Test/xmlts20130923.tar.gz
SHA-256: `9b61db9f5dbffa545f4b8d78422167083a8568c59bd1129f94138f936cf6fc1f`.
Keep the archive and its extracted license notices under ignored `.vendor/`.
The upstream tests are not relicensed or copied into the library.

The independent Python catalog reader selects XML 1.0 Fifth Edition,
Namespaces 1.0 cases before invoking the parser. XML 1.1, other editions,
namespace-disabled cases and external-entity requirements are excluded with
explicit reasons. DTD validity tests are recorded separately from
well-formedness tests. The parser's unsupported status is not a passing
rejection of malformed XML and is not acceptance of a valid document.

JSON report version 1 has `version`, `archive_sha256`, `cases` and `counts`.
Each case has `id`, `path`, `expected`, `selection`, `reason`, `actual` and
`verdict`. Verdicts distinguish accepted valid, rejected not-well-formed,
unexpected acceptance/rejection, unsupported, and excluded. Any unexpected
result fails the command; unsupported cases remain visible and prevent a
claim of complete profile conformance. Local event tests additionally verify
normalization, namespaces and content: acceptance alone cannot verify those.

Harness controls must include a legal document, a mismatched closing tag,
an unsupported DTD declaration and a truncated document. Running those through
a deliberately all-accept or all-reject adapter must fail. One-byte and varied
feeds must agree with a whole-buffer parse. No parser result may change the
catalog's expected answer or erase a case from the denominator.

Run `python3 tools/xmlconf/fetch.py`, then `tools/test_xml_host.sh` from the
worktree. `--local-only` explicitly omits xmlconf; the default requires the
pinned archive. Python 3.12+ supplies safe tar extraction and Expat supplies
the independent catalog parser. Catalog external entities are restricted to
local files inside the corpus; the library under test has no resolver.

`required.json` format 1 contains a sorted, unique `required` array of catalog
IDs. These 489 cases exercise admitted grammar without requiring a complete
DTD implementation. They must continue to receive the catalog's
well-formedness verdict; changing one to UNSUPPORTED fails the gate. The
catalog, not this list, supplies their expected result. The initial acceptance
cases are catalogued `invalid`: they are well-formed documents lacking DTD
validity. Upstream's `valid` cases all hit omitted DTD features in this profile.
The local suite supplies legal positive documents without DTDs and verifies
their actual event contents.

The report also records `required_regressions`. `--require-complete` fails on
any selected unsupported case. The default gate proves the explicit required
set and checks for unexpected verdicts throughout the selected corpus; it
does not certify complete XML conformance. Case removal or a changed required
set is a reviewable change to `required.json`.

`controls.json` format 1 supplies `cases` with XML `input`, catalog-style
`expected`, and required driver `actual` status. The runner executes this
vector before the corpus. Uniform accept/reject adapters must fail it;
malformed protocol output or a missing result also fails. Driver subprocesses
have timeouts so a parser regression cannot park the gate indefinitely.
