# libhtml

Optional streaming HTML-to-tree library for os64 userland. The settled scope,
reference revisions, deliberate deviations, and evidence are in
[LIBHTML.md](../../LIBHTML.md). Public declarations live in
[html.h](include/html/html.h); only consumers link against `libhtml.so`.

```c
os64_html_options_t opt = os64_html_options_default();
opt.charset = "utf-8"; /* HTTP transport label, when present. */
os64_html_parser_t *p = os64_html_parser_new(&opt);
if (!p) {
    /* Initial heap or arena allocation failed. */
    return;
}
int64_t status = os64_html_parser_feed(p, bytes, length);
/* Further feed calls can be made while status == OS64_HTML_OK. */
os64_html_document_t *doc = os64_html_parser_finish(p);
/* finish consumes p, including on refusal. Walk doc->document or doc->html;
 * head/body are nullable. Inspect doc->refusal after finish as EOF may fail. */
render(doc);
os64_html_document_free(doc);
```

For cancellation call `os64_html_parser_destroy(p)` instead of `finish`.
A supplied options object uses its values literally, including zero limits.
Nodes and attributes are read-only views. A finished document is changed
through the verbs at the foot of `html.h` (create, insert, replace, remove,
attributes, text, clone), which keep every rule a reader relies on; the design
is [DOM.md](../../docs/design/pending/DOM.md). Elements made by formatting
reconstruction share one list of attribute records, so a verb copies an
element's list before it first changes it. A document owns the
allocation ledger; scratch buffers are charged to the same budget and released
on finish. Stable node/string storage uses geometric arena chunks. A parser
needs serialized calls, and so do a document's verbs; distinct documents
share two counters (document marks and pin numbers), advanced atomically, and
nothing else. A verb refuses a node of another document; `os64_html_clone`
copies one across.

`core.c` owns allocations, pins and retirement, topology primitives, limits,
and the parsing API. `dom.c` holds the verbs that change a finished document.
`encoding.c` selects/decodes the byte stream and preprocesses newlines.
`tokenizer.c` implements token states and character references. `tree.c`
implements insertion modes, formatting reconstruction/adoption, templates,
foreign integration, and foster parenting. Reprocessing is iterative; bounded
mode delegation follows the standard's calls to another insertion mode.

The parser implementation is project code. Entity, SVG-adjustment, and quirks
tables are generated from the imported WHATWG data under
`tools/html5lib-tests/standard/`; its license is included there. Tag enum input
is `tags.txt`. Regenerate with `tools/gen_html_tables.py`; `--check` verifies
without writing. Upstream tokenizer/tree fixtures retain their licenses and
exact bytes. Saved corpus pages retain their publishers' content; their URLs,
fetch dates, transport charset, and hashes are in `tools/html_corpus/SOURCES.json`.

Validation:

```sh
# Omit the ASAN override when LeakSanitizer is available outside ptrace.
ASAN_OPTIONS=detect_leaks=0 tools/test_html_host.sh
tools/test_html_dom_host.sh        # the verbs
make -j4
# Inside os64:
/tests/testrun htmltest
```

The host script checks reference provenance/inventory, exact token/tree results,
chunking, every byte prefix, heap failure points, topology/UTF-8, encoding,
resource refusals, saved trees, and a 30-second mutation pass. It does not fetch
network resources or update expected results. `HTML_FUZZ_SECONDS` selects a
longer fuzz budget. `tools/update_html_fixtures.py` and
`tools/update_html_corpus.py` are explicit refresh commands; review their inputs
and regenerated expectations together. `tools/test_html_dom_host.sh` proves the
verbs: cases by hand, every allocation failed in turn, pins and retirement, and
random walks checked step by step against a second tree kept by the test. Corpus snapshots can be regenerated
with `tools/test_html_corpus.py --driver <host-driver> --refresh-snapshots`.
