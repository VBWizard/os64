# libpage

The page model translates libhtml's tree into controls, links, form ownership
and requests. [LIBPAGE.md](../../LIBPAGE.md) is the design; the public seam is
[page.h](include/page/page.h).

A mutable document has one explicit `os64_page_state_t` owned alongside it.
Build models with `os64_page_build(doc, url, options, state)` and use
`os64_page_rebuild(old)` after the document version moves. A failed rebuild
returns NULL and preserves the old model and state. Each model pins the
document; release models, then state, then the document. State does not take
a document pin. Passing NULL state creates private shared state that survives
until its last model is freed, including models returned by rebuild.

State is keyed by node, including detached or never-inserted controls, and
options have independent node keys. Its default ceiling is 16 MiB, including
allocation headers and script-operation scratch. Builds keep default records
where normalization differs from
markup and preserve existing records; property setters can spend the cap on
a whole group. Reservations reuse spare lookup capacity and copy the live
table when it needs to grow. Model and node option indices share the HTML
list-of-options walk, including its excluded subtrees. `state_bytes` reports live
charged memory. `state_version` reports effective property changes separately
from HTML's version. Getter-only cache fills leave the revision alone;
effective dirty-value normalization advances it.

Person edits use control indices and preserve disabled/readonly restrictions.
Script setters use nodes and input value modes. Text writes dirty state;
hidden/button/tick values write HTML's value attribute; files accept empty;
select value/selectedIndex sets option records and may clear the selection.
All live models republish changed state before old value storage is freed.
Activation and person edits/reset refuse STALE models.

Default getters may cache sanitized values, with NO_MEMORY leaving their
outputs unchanged. Current dirty getters allocate nothing. After HTML changes,
dirty reads and rebuilds stage re-sanitization for current type/constraints; observed file
mode clears the old value. Historical input-mode transitions require D5
state-aware attribute mutation, since a final tree version cannot tell
which transitions happened between observations. Returned bytes expire when
the value changes, a default cache refresh follows an HTML mutation, or state
is freed. Equal assignments retain the existing bytes.

D6 must add node holds for retained state keys and model node references,
releasing them when their owners end. Model pins already protect snapshot
strings; they are not a substitute for holds under subtree reclamation.
D3 relies on libhtml's present document-lifetime node contract.

Host validation: `bash tools/test_libpage_host.sh --rebuild`; the default
invocation also exercises the existing request, numeric and allocation cases.
`/tests/pagetest` supplies guest coverage. Source-copy mutation testing is
provided by `tools/test_libpage_rebuild_mutants.py`.
