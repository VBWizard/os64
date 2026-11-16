# F15 B3 — kept-payload ledger diagnostics

Implementation PR: **#617**, source `33463cc8`.

Current qualified composition: merged #615 (`fe4b1d2b`), described below.

Initial baseline: merged `userland` `4ccb6ee2` (#614), embedding ABI 5, engine
`31bcaae4fa1778113d0c3cebc4de4435ab7d34f9ef8306e334e89774c06fa631`.
This is the native payload cut coordinated with the F4 lane. Callback-holder
conversion/invocation fields remain outside this change; the identity/export
contract still depends on F4c/d. B3 leaves existing B1/B2 structures,
production record layouts, engine code and lifetime policy unchanged.

## Observation and limits

`os64_dom_kept_retention_snapshot(binding, caller_abi, scan_limit)` in
`dom/kept_retention.h` returns a new structure by value. It reads the binding's
existing body-quota ledger and a prefix of its `DKept` list. It reports:

- Full quota bytes and prefix quota bytes, distinct from total native heap use.
- Records seen, empty/segmented/block-backed records and records whose native
  reference counter exceeds one, plus the prefix maximum reference counter.
- Local-list completeness and the binding's drained/closed state.

The query visits at most `min(scan_limit, 64)` records and does not walk a
record's piece list. Prefix counts, charges and the maximum are lower bounds
until complete. An empty list is complete even with a zero budget; a nonempty
list with zero budget remains unknown. Full quota bytes remain visible when
the prefix is empty. Disjoint record charges sum to a subset of the existing
quota ledger; reference counts are compared, not summed.

Quota is an existing accounting boundary: body blocks/piece storage and Blob
records are charged, response records and allocator overhead are excluded.
Sixty-five empty response records can therefore coexist with zero quota.
Conversely, an empty Blob's record consumes quota. The snapshot does not
reinterpret quota as an exact retained-heap census.

Records stay on their allocating binding's ledger when foreign objects refer
to their backing. Script aliases of the same Blob do not independently
increment its native reference counter. A Blob slice adds a native backing
reference; a constructed Response copies into engine storage outside this
native census. Unknown incoming edges, reference roles, other payload
families, allocation overhead and piece details remain explicit even when
the local list is complete and empty. No count certifies release or reuse.

The caller keeps a live binding and uses its owning thread. NULL inputs and
ABI mismatch refuse data. The query allocates nothing, enters no engine
scope, calls no provider, samples no clock, collects nothing and releases no
reference. It remains valid after drain and engine destruction, before native
binding free. Root teardown's existing finalizer/native-sweep ordering governs
release; diagnostic inspection does not acknowledge any of it.

## Initial validation on the pinned baseline

The host uses a fresh coherent native cohort including the merged style-list
layout changes. The final query and consumer are rebuilt against those
matching objects. Target libraries and consumer are built together.

| Receipt | Result |
|---|---|
| DOM `--retention`, target-core / instrumented-core | 3,652 checks per profile; zero/one/64/oversized budgets, uncharged response records, empty Blob records, segmented storage, explicit join/release, quota-prefix distinction, repeated allocation/clock-purity checks. |
| Shared host/guest public oracle | Foreign script aliases preserve a single native reference; a foreign slice shares physical backing; fenced provider inspection and post-engine-destruction queries preserve lifetime boundaries. |
| Host ordinary departure | Foreign Blob data remains readable and its source charges remain intact after source departure. |
| DOM `--fetch` | 1,523 / 10,526 checks; existing Blob, buffer, response/XHR, quota and refusal cases pass. |
| DOM `--recovery` and `--departure` | 2,532 / 6,672 recovery checks; 353 departure checks per profile. |
| Nine isolated mutations per profile | Eighteen named rejections: scan clamp, completeness, full quota, prefix charge, sharing, maximum references, empty-record meaning, incoming coverage and an allocating query. Rejected variants are not positive leak evidence. |
| Header/ELF/engine audits | Standalone host/cross headers, source manifest and engine teardown audit pass; DOM exports add one query and retain the old union. Target query relocations reference only its ABI string and `os64_strcmp`. |
| Frozen QEMU consumer | Nineteen matching cohort files with SHA-256 manifest; shared cases and six F16 modes under both teardown policies pass. Target heap returns to baseline, allocator accounting holds and parent observes exact child exit `0x446f0200`. |

Positive host runs retain ASan/UBSan and LeakSanitizer outside the sandbox;
the instrumented engine uses ASan with native/wrapper UBSan. The known
unrelated NONET fixture-kernel ARP preboot failure is outside the consumer
receipt. No new full-DOM, WPT or P5 campaign is claimed.

Response construction uses the Streams brand-check door, which marks a
recovery-enabled cluster as having used Streams. Its copy case is confined
to the existing Streams/global-fallback mode. Blob-only aliases exercise the
plain child-local READY modes without clearing that conservative gate.

F15 remains in progress. These observations complement
[B1](IFRAME_F15_RETENTION_DIAGNOSTICS.md) and
[B2](IFRAME_F15_NATIVE_BORROWS.md); deep engine/callback/export classification,
release-stage injection and the reviewed identity/lifetime contract remain
separate cuts in the [reclamation brief](IFRAME_F15_RECLAMATION.md).


## Composition with merged F4 active-script records

PR #617 composes merged `userland` `fe4b1d2b` (#615), including its review
follow-up. Engine identity is
`50e0bb8409b1d0d01fd0dad73adaa305e4a449e49b3eacac33d58c322cc632ba`;
embedding ABI 5 remains compatible and bytecode format is 6. The B3 query,
headers and shared consumer are byte-identical to `33463cc8`. The new engine's
closure/reaction/job ScriptOrModule roots remain unclassified incoming edges;
the native ledger does not enumerate them or supply a release certificate.

The host is rebuilt as a fresh matching native/engine cohort; prior 31bc
objects are not mixed into this proof. Target libraries and the frozen guest
are rebuilt together. The original receipts above retain their 31bc scope.


| New-engine composition receipt | Result |
|---|---|
| Fresh DOM `--retention` | 3,652 per profile. |
| `--fetch`, `--recovery`, `--departure` | 1,497 / 10,560 fetch, 2,532 / 6,672 recovery, 353 departure per profile. Allocation-cut counts vary with engine storage. |
| `--promise-settings`, `--handler-settings` | 246 and 5,526 per profile; accepted settings behavior preserved. |
| Nine mutations per profile | Eighteen named rejections on the new matching cohort. |
| Headers/preparer/teardown/link | Pass, six preparer tests, 222 engine / 80 runtime exports; DOM retains its old export union plus the new query. |
| Fresh frozen QEMU cohort | Nineteen matching files, shared cases under both root policies, target heap baseline/accounting and exact parent exit pass. |

Positive host runs retain leak-enabled sanitizers outside the sandbox. The
query's target relocations remain its ABI string and `os64_strcmp`; the merge
adds no engine entry to diagnostic inspection. Both original and composition
proofs retain their source/cohort manifests. The kernel fixture's unrelated
NONET ARP preboot failure stays outside the consumer claim. This composition
adds no full-DOM, WPT or P5 receipt.
