# F15 — private owner-ticket metadata prerequisite

Implementation PR: **#634**, source `1bb2d85b`.

Proof baseline: merged `userland` `7ee102a6` (#625); publication includes
`ba3fbbdd` (#620). The media-session merge changes none of the recorded private
source/build inputs or guest cohort files. This implements a bounded
private bookkeeping component for the [export-lifetime proposal](IFRAME_F15_EXPORT_LIFETIMES.md).
It does not implement a checked runtime profile or change the ABI 5 embedding
contract. F15 remains `code`; physical reclamation is gated by the
[F15 plan](IFRAME_F15_RECLAMATION.md) and F4 inputs.

## Implemented boundary

`userland/libjs/runtime/owner_tickets.[ch]` accepts caller-owned, aligned,
malloc-like storage and an externally supplied live incarnation. The caller
zero-initializes the table, keeps it and its storage live, and serializes use
on the owner thread. Init checks capacity, alignment, size/range overflow and
overlap with the table before writing. Resetting a live table refuses.

| Record | Meaning and invariant |
|---|---|
| Owner ID | Numeric incarnation and owner serial. Owner rows are cumulatively reserved, bounded by the supplied capacity and `OS64_JS_MAX_REALMS`; closing holders does not recycle rows. |
| Ticket | One holder record. Copying its value aliases that holder; explicit duplicate reserves an independent holder. Resolve returns an ID, not an engine/native pointer or authority to execute. |
| Loan | An independent admitted-use metadata record. Closing its original ticket does not end it. Promotion can reserve a new ticket after that close; capacity/exhaustion refusal preserves the loan and output. |
| Token issuer | Tickets and loans use one non-reusing 64-bit issuer, preventing a token's fields from matching the other pool. Exhaustion refuses before wrap. Cells can be reused without reusing serials. |
| Unknown escape | Sticky retention metadata; zero holder/loan counts do not clear it or certify release. It is separate from F16 failure and health state. |

Reservations check capacity and issuer exhaustion before committing any owner,
token or count. Refused operations leave outputs and metadata unchanged.
Output objects must be outside the table/storage. Loan end follows the
surrounding engine/settings/value/finalizer unwind; metadata close/end invokes
no engine cleanup or callback. There is no owner erase or table-reset API.

The unit stores metadata arena pointers, but no runtime, context, DOM or native
owner pointers, values, roots or callbacks. It allocates nothing and has no
imports or mutable globals. `shared.mk` builds it as a hidden object in
`libjs.so`; no existing production caller uses it. Public headers, exports,
constructors, engine patches and `runtime.c` are unchanged.

## Evidence

The same shared oracle runs against sanitized native code, the actual
cross-built production object with a sanitized host driver, and an Os64
consumer. The second mode does not instrument the target object itself.

| Check | Observed result |
|---|---|
| ASan/UBSan/LSan, leak detection enabled outside sandbox | 23,358 checks, zero failures; caller arenas balance. |
| Actual production object, sanitized host driver | 23,358 checks, zero failures. |
| Os64 child/parent in isolated NONET QEMU | 23,360 child checks, heap integrity/live blocks/live bytes balanced; parent observed exact exit `0x4f570200` and `OWNER TICKETS QEMU COMPLETE PASS`. |
| Negative controls | 14 deliberate variants rejected in both modes: 28 named behavioral failures. Signals, loader/build errors and sanitizer aborts do not count as successful rejection or leak-check evidence. |
| Production link and private object audit | 84 public runtime exports; 32 math/26 os64 imports unchanged. 13 private hidden functions, zero private object imports or mutable globals; no owner-ticket dynamic export. |
| Existing regressions | Manifest/header and teardown allocation/global/visibility audits pass. CLI: 199 stub checks and 40 real-library checks, zero failures. |

Tests cover copied aliases, independent duplicates, stale IDs after cell reuse,
cross-kind confusion, foreign incarnations with repeated local numbers,
promotion after reentrant close, capacity/exhaustion atomicity, storage refusal,
cumulative owner reservation and sticky unknowns. Churn reuses bounded token
cells 4,096 times. An independent 512-operation history model recomputes
per-owner and global counts from live returned IDs after each operation.

Mutation oracles remove the live/token incarnation checks, reuse serials,
separate ticket/loan issuance, duplicate a closed ticket, consume a promotion
loan, partially reserve an owner, recycle owner rows, wrap either issuer,
miscount ended loans, clear unknowns, reset a live table or allow size overflow.
All 28 runs fail a named assertion rather than relying on a crash.

The frozen guest cohort contains four files: child, parent, `libos64.so` and
its `libfreetype.so` dependency. It loads no `libjs` or QuickJS. Metadata records
the private object, source/build-input hashes, source image/ISO and cohort
SHA-256 manifest. The unrelated NONET ARP preboot failure is outside the
child/parent oracle. There is no new P5 acceptance claim.

## Replay

```sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_js_owner_tickets_host.sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_js_owner_tickets_mutations.py --output /tmp/owner-ticket-mutants-new
python3 tools/test_js_owner_tickets_guest.py --base-image /path/to/testrun-nonet.img --boot-iso /path/to/matching.iso --output /tmp/owner-ticket-guest-new
```

Run leak-enabled host checks where LSan can attach. Guest inputs must be a
matching bootable TESTRUN/NONET ISO and FAT image with `/tests` and `/lib`;
the script copies them and refuses an existing output directory. Source
images and previous receipts remain intact. The consumer target can also be
built with `make -C userland js-owner-tickets-test` without preparing QuickJS.

The [private incarnation issuer](IFRAME_F15_INCARNATION.md) implements the numeric
namespace prerequisite separately; it does not bind identities to runtimes.

The [runtime identity](IFRAME_F15_RUNTIME_IDENTITY.md) and
[private storage admission](IFRAME_F15_OWNER_ADMISSION.md) follow-ons supply
separate receipts beyond this metadata-only baseline; they enable no checked
exports or reclamation.

## Unproved integration obligations

The tests supply distinct incarnation numbers. They prove numeric binding,
including a new table lifetime at the same address, not a production fresh
incarnation issuer. Integration must provide that issuer, account for arena
storage through runtime admission/budgets, and keep the live table valid.

No checked constructor/profile, export migration, legacy-escape detection,
runtime teardown wiring, execution admission, C3 target/security publication,
engine/context pins, provider detachment, quota uncharge, root release,
collection or realm-slot reuse is enabled. The unit creates no runtime, so
these receipts do not prove either runtime root-teardown profile. Default
embedding behavior remains on its existing contracts. Metadata zeroes cannot
override independent engine/native/creator/borrower obligations.

Quinn boundary-read the agreed private interface before implementation. The
F4 lane owns target/publication/cache contracts and its DOM/engine work; this
component reserves none of those interfaces. The next integration slice must
state and prove its actual issuer, storage-admission and lifetime obligations
before consuming these IDs as runtime exports.
