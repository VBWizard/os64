# F15 B2 — native owner and borrower diagnostics

Implementation PR: **#614**, source `c1085571`.

Baseline: merged `userland` `6e62460e` (#610), embedding ABI 5, engine
`31bcaae4fa1778113d0c3cebc4de4435ab7d34f9ef8306e334e89774c06fa631`.
This extends the [B1 scalar diagnostics](IFRAME_F15_RETENTION_DIAGNOSTICS.md)
with native HTML/page observations. The coordinated boundary is
independent of F4's active ScriptOrModule work: no engine changes, existing
record layout changes, new pins, collection, owner release or slot reuse.

## Public observations

| Query | Local facts returned by value |
|---|---|
| `os64_html_retention_snapshot(doc, scan_limit)` in `html/retention.h` | Arena identity/creation byte, physical bytes/peak, retired block bytes, native hold counter, occupied snapshot pins, parser presence, group membership/charging, reader and associated-range prefixes. |
| `os64_html_node_retention_snapshot(node, scan_limit)` | The physical allocation owner's snapshot, logical document arena identity, original creation byte and node non-parser/parser reference counters. |
| `os64_page_retention_snapshot(state, scan_limit)` in `page/retention.h` | Physical state bytes, group/private ownership and a prefix of distinct model objects. |

The caller owns live inputs and runs on their owning thread. NULL is invalid;
a stale pointer cannot be validated by these queries. Snapshots allocate
nothing and do not collect, call out, mutate, acknowledge loans or extend
input lifetime. The HTML query deliberately reads `retired_block_bytes`
without invoking `os64_html_retired_bytes`: that older API also walks pending
detached node units. The pending-node census remains unknown here.

Each reader/range/model list visits at most `min(scan_limit, 64)` entries.
`seen` is a lower bound until `complete` is true; zero with an incomplete
list does not mean no borrowers. A zero budget can still report a genuinely
empty list as complete. At the exact bound, reaching the list's end reports
complete without visiting an additional entry. Model objects and their
reference counts differ; retaining one model twice still occupies one list
entry. Model references remain explicitly unknown.

Adoption does not move allocation charges or creation slots. Node queries
select the physical allocation owner rather than infer it from the logical
document. Range lists describe current document association, which can
migrate independently of the range's allocation. Node non-parser references
also include internal change-log references and need not equal the owner's
native hold counter.

Zero local pins/models cannot prove independent release: a sibling HTML pin
can retain this arena's replaced blocks, and a model in another page state
can preserve this state's retired shared value bytes. Group borrowers,
unclassified incoming edges and exports remain unknown even with complete
empty local lists. Retired page bytes are included in physical charges but
their subset is not enumerated. These observations describe accounting;
neither OK/valid nor zero values are an eligibility certificate. The existing
group teardown and borrow contracts remain authoritative. B1 structures and
unknown masks are unchanged.

## Validation

`userland/libpage/tests/retention_cases.h` is a public-API oracle shared by
the page host and existing F16 host/guest consumer. It exercises real holds,
reader/range creation and release, parser presence, private model state,
multiple model references, adopted physical ownership, migrated ranges,
foreign pins/models retaining source bytes, zero/one/64/oversized budgets,
and unchanged versions/physical charges across repeated queries.

| Receipt on the pinned baseline | Result |
|---|---|
| `bash tools/test_libpage_host.sh --retention` | 197 checks; queries also run with allocation failure armed and unchanged allocation/live counters. Existing live-state teardown still stops with the PAGE badge. |
| Full `bash tools/test_libpage_host.sh` | 254,905 checks, allocation refusal sweep and PAGE guard pass. |
| Full `bash tools/test_html_dom_host.sh` | 800,487 checks, ownership/adoption, ranges/pins, allocation refusal and independent random tree oracle pass. |
| Matching target-core and instrumented-core DOM `--retention` | 3,186 checks per profile, including the native oracle through six host routing cases under both teardown policies. |
| DOM `--recovery` | 2,532 / 6,672 checks respectively; existing F16 local/global routing and refusal sweeps pass. |
| Isolated mutations | Eight rejected by named oracles: HTML scan clamp, reader/range completeness, physical owner selection, page model completeness/charge and HTML group/page reference coverage. Rejected variants are not positive leak evidence. |
| Cross-header and ELF checks | Both host/cross compilers accept the standalone headers. HTML exports add two queries and page exports one; old exports remain and private list helpers stay hidden. Engine manifest/teardown audit passes. |
| Frozen QEMU consumer | Shared native and six F16 cases under both policies; target heap returns to baseline with allocator accounting intact; parent observes exact child exit `0x446f0200`. Nineteen matching cohort files, SHA-256 manifest. |

Positive host runs retain ASan/UBSan and LeakSanitizer outside the sandbox;
the instrumented engine uses ASan with native/wrapper UBSan as before. Changed
native units and the host consumer are rebuilt against matching #610/31bc
objects; target libraries and guest are built together from this worktree.
The frozen NONET kernel retains its known unrelated ARP preboot failure,
outside the consumer receipt. No new full-DOM, WPT or P5 campaign is claimed.

F15 remains in progress. Deep engine/callback/export edges, release-stage
injection and the reviewed F4c/d identity/lifetime contract remain separate
cuts in the [reclamation brief](IFRAME_F15_RECLAMATION.md). F11's browser/P5
acceptance or session-measurement alternative is unchanged.
