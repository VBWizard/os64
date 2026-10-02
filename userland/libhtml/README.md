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

A host that runs scripts sets `opt.scripting`. The parse then stops at each
script, and the host does what it does with one before it resumes:

```c
static int64_t run_scripts(os64_html_parser_t *p, int64_t status)
{
    while (status == OS64_HTML_SCRIPT) {
        run(os64_html_parser_document(p), os64_html_parser_script(p));
        status = os64_html_parser_resume(p);
    }
    return status;
}

status = run_scripts(p, os64_html_parser_feed(p, bytes, length));   /* each chunk */
status = run_scripts(p, os64_html_parser_end(p));                   /* no more input */
os64_html_document_t *doc = os64_html_parser_finish(p);
```

Between two calls the tree is a document: it can be read, pinned and changed
with the verbs. A page left mid-load keeps what was built with
`os64_html_parser_abandon(p)`. The contract is in `html.h` under THE PARSE
THAT STOPS.

Nodes and attributes are read-only views. A document is changed
through the verbs at the foot of `html.h` (create, insert, replace, remove,
attributes, text, clone), which keep every rule a reader relies on; the design
is [DOM.md](../../docs/design/pending/DOM.md). Elements made by formatting
reconstruction share one list of attribute records, so a verb copies an
element's list before it first changes it. A document owns the
allocation ledger; scratch buffers are charged to the same budget and released
on finish. (Input held while a parse is stopped is the heap's: `max_bytes`
bounds it.) Stable node/string storage uses geometric arena chunks. A parser
needs serialized calls, and so do a document's verbs; distinct documents
share two counters (document marks and pin numbers), advanced atomically, and
nothing else. A verb refuses a node of another document; `os64_html_clone`
copies one across.

`core.c` owns allocations, pins and retirement, topology primitives, limits,
and the parsing API, with the stop at a script and the input held meanwhile.
`dom.c` holds the verbs that change a document, and the rules the parser
keeps once a verb has moved a node under it.
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

The host script checks reference provenance/inventory, exact token/tree results
with scripting off and on, chunking, every byte prefix, heap failure points,
topology/UTF-8, encoding, resource refusals, saved trees, a 30-second mutation
pass, and the parse that stops: each call's contract by hand, a parser
abandoned at every stop, and random verbs run at every stop of random
documents (`html_driver --checks <walks> [first]` runs more of those, or one). It does not fetch
network resources or update expected results. `HTML_FUZZ_SECONDS` selects a
longer fuzz budget. `tools/update_html_fixtures.py` and
`tools/update_html_corpus.py` are explicit refresh commands; review their inputs
and regenerated expectations together. `tools/test_html_dom_host.sh` proves the
verbs: cases by hand, every allocation failed in turn, pins and retirement, and
random walks checked step by step against a second tree kept by the test. Corpus snapshots can be regenerated
with `tools/test_html_corpus.py --driver <host-driver> --refresh-snapshots`.
