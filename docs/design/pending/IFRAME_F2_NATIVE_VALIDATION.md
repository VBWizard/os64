# F.2a — native ownership and adoption

Implementation record for the native half of [F.2](IFRAME_F2_OWNERSHIP.md),
PR [#537](https://github.com/VBWizard/os64/pull/537), based on `f239ea75`
(the reviewed brief, PR #533). F.2 remains in progress:
libdom clusters, JavaScript wrappers and service routing belong to F.2b.
Yonder still creates one binding. This PR exposes no visible iframe behavior.

## Ownership contract

libhtml stores physical allocation owner, current Document node and creation
realm separately. Creation realm is a host-assigned byte, not a JS context.
Group admission assigns it to the already-parsed document; subsequent
creation uses that arena's realm. Adoption preserves the byte and allocation
identity while updating the current Document, including shadow trees and
configured template contents. Clone/import allocate new destination nodes.

A lifetime group owns its admitted arenas through group teardown. The first
member can retain existing borrowers. Later members require finished parsers
and no external holds, pins, readers or ranges. Failed admission preserves
caller ownership and accounting. Ungrouped documents use the singleton path
without allocating a group descriptor. Operations on a group are serialized.
Group membership is a memory-lifetime contract, not same-origin permission.

The first member carries detached-unit queues. Individual bodies, strings,
attributes and state records keep their physical ledgers, even when a tree
contains several arenas. Holds dispatch to that ledger. Pins use a group
storage epoch; document revisions remain local. A sibling pin can delay
reclamation, bounded by both the physical arena cap and aggregate cap.
Fragment staging charges its peak before publication and restores charges
on refusal. Group teardown requires released borrowers and frees arenas last.

The host supplies and retains explicit windowless/inert Document records.
`os64_html_document_templates` associates a Document with its pre-created
inert template Document before identity is exposed. Nested templates reuse
that association. The legacy parser does not allocate binding-owned inert
records. F.2b must configure them and retain the records while nodes name them.

Explicit adoption validates membership, hierarchy/root restrictions and depth
before detaching. Source removal updates ranges and source notifications;
metadata and detached-range registrations then move to the destination.
Insertion/replacement validate their reference child before adopting.
Same-arena windowless moves retain the existing single-revision notification
pair, preserving custom-element adopted-callback detection in legacy libdom.

libpage groups preallocate both state indexes and affected control records
before the native move. Dirty values, selection, checkedness and validity
records follow nodes; source/destination form peers are repaired. Payloads
keep their original allocation charge. Shared adopted value buffers retire
while any group model exists, including buffers published later to an old
source model. Releasing the last model reclaims the retired list. This is a
conservative bounded retention rule, not independent snapshot reclamation.
Import reads dirty source state and creates destination records; it preserves
the existing policy of not copying custom validity.

Free models, then page-state groups, then remaining HTML borrowers and HTML
groups. Individual free of an admitted state/document is a lifetime error.
Detached creation results that were never inserted retain the existing
until-arena-teardown policy. The binding must hold adoption results across
later mutations. State adoption does not enter JavaScript.

## Positive and negative evidence

Host runs use ASan/UBSan and LeakSanitizer with `detect_leaks=1`, outside the
sandbox where thread attachment is available. Test output is streamed to
bounded files. No leak check was disabled.

| Check | Result |
|---|---|
| `tools/test_html_dom_host.sh` | 799,483 checks; zero failures |
| `tools/test_libpage_host.sh` | 254,708 checks; zero failures; 415 allocation cuts; PAGE lifetime-fatal check |
| HTML parser/reference/fragment suite | 1,277 native checks; 10,531/10,531 reference cases; 376 roundtrips; corpus snapshots preserved |
| Parser fuzz | 115,712 mutations, four chunkings each, 30-second campaign |
| HTML reclamation | 310,833 checks; 216,000 fragment-churn steps flat at 39,279 bytes / 104 nodes |
| DOM, shipping engine | 43,439 checks; zero failures; 3,886 binding allocation sweep points |
| DOM, instrumented engine | 307,535 checks; zero failures; 36,473 binding/engine sweep points |
| `tools/test_yonder_scripts_host.sh` | 24,903 checks; zero failures; 114 completed refusal cases |
| `tools/test_iframe_ownership_mutants.py` | Passing HTML/page baselines; 9/9 variants compiled and caught |
| QEMU native consumers | `htmltest` exit `0x48640000`; `pagetest` exit `0x50670000`; both observed by parent `os64_wait` |

The three-document native oracle creates in A, moves to B, creates fresh
descendants in B, then moves to C before querying their metadata. It checks
A/B creation bytes, C node documents and unchanged physical identities.
Additional cases cover both move directions, mixed-ledger collection,
windowless Documents, consumed/empty fragments, nested templates, shadow
attachment after adoption, lost slot marks, XML/HTML moves and live ranges.
Page tests cover dirty controls, import, source form removal, repeated
readoption with stable charges and all 14 migration-preparation allocation
cuts. Each cut checks trees, revisions, holds and ledgers, then retries.

QEMU uses a frozen seven-file cohort: two consumers, their wait-status parent,
and four matching shared libraries. A SHA-256 manifest records the cohort.
It reuses the existing kernel and boots a disposable disk; native consumers
exercise three realm keys and retained source control snapshots. These keys
are native metadata, not installed JavaScript realms. No P5 test is required
for this native foundation.

The mutant runner compiles isolated source variants and requires passing
baselines plus recognized assertions/sanitizer failures. Compilation failure,
timeout and arbitrary crash are not accepted as proof. It checks creation
realm corruption, missing document migration, wrong physical free ledger,
missing range migration, wrong template document, ignored sibling pins,
missing state migration, early snapshot-buffer free and ignored aggregate
state cap. Each child log is capped at 8 MiB and read back at most 64 KiB.

## Cost and bounded recovery

Public `HNode` remains 152 bytes. Private `HMeta` grows 72 → 88 bytes;
`HDoc` grows 1,400 → 1,472. The realm byte consumes existing flag padding.
The block header remains 48 bytes, using its padding for the physical owner.
No existing limits were raised.

| Corpus document | Before arena bytes | After arena bytes | Increase |
|---|---:|---:|---:|
| 68k-news | 591,896 | 591,968 | 72 |
| example | 17,128 | 17,200 | 72 |
| floodgap | 290,456 | 290,528 | 72 |
| hacker-news | 564,648 | 564,720 | 72 |
| textfiles-computers | 1,479,928 | 1,742,192 | 262,264 |
| wikipedia-html | 5,951,096 | 6,213,360 | 262,264 |

The last two cross an extra 256 KiB permanent-chunk boundary (plus its
48-byte header and 72-byte document growth). This cost is retained capacity,
not merely the requested metadata bytes.

With A's pin stalled, B reaches its 512 KiB local cap after 324 fragment
replacements. B goes **5,616 → 513,972 → 5,616 bytes** after releasing A;
the aggregate peak is **529,764 bytes**. Refusal is nonsticky and retry works.
Additional tests retain replaced text using a pin held solely in either
sibling and verify aggregate staging-cap rollback.

With an old source page model and a 64 KiB state-group cap, 13 value
replacements reach refusal. State bytes go **1,909 → 55,586 → 6,000** after
model release; aggregate peak is **63,922 bytes**. Retry succeeds. The final
live value remains charged; retired copies reclaim. F.8 must account for
these cross-document retention costs.

## Remaining F.2 acceptance

F.2b must supply cluster-wide wrapper identity, creation-realm prototypes,
operation routing, stable custom definitions, shared reactions/observers,
explicit retained Document ownership and root teardown through both libjs
policies. The full DOM runs here protect the existing single-binding path;
they do not prove cross-binding JavaScript behavior.

Named pinned-WPT baseline comparisons and the actual multi-realm DOM guest
fixture remain F.2b acceptance work. No WPT expectations or exclusions were
changed, and native metadata cases are not claimed as unmodified WPT passes.
Child retirement, slot reuse policy and child-only installation rollback
remain F.3. F.2's tracker row cannot become done on this native PR alone.
