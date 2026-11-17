# F15 — private incarnation issuer

Baseline: merged `userland` `4885f974` (#633/#634), embedding ABI 5. The
[owner-ticket metadata](IFRAME_F15_OWNER_TICKETS.md) requires externally fresh
incarnations; this implements a private issuer, without runtime wiring.

`owner_incarnation.[ch]` reserves nonzero 64-bit numbers from one process-local
namespace per loaded libjs instance. A lock-free compare/exchange is the
linearization point. Relaxed ordering supplies uniqueness; runtime publication
requires its own ordering. No identity is returned twice, reset or uncharged;
failed later admissions burn a number. The final representable identity is
usable, then exhaustion refuses without wrap or output mutation. Invalid
requests consume nothing. State and output must remain valid and disjoint.

The namespace lasts through the loaded library's lifetime. Tokens cannot cross
processes or unload/reload lifetimes. The parameterized private primitive permits
fresh local test issuers near exhaustion; those are not production namespaces
and the production singleton has no reset interface. Retrying a contended CAS
is lock-free, not a bounded-time or fairness guarantee.

The production object has two hidden functions, one eight-byte singleton,
no imports or allocation, and target `lock cmpxchg`. It is included in libjs
with no runtime caller. Public exports, ABI, engine patches, constructors,
configured budgets and legacy lifetimes remain unchanged. IDs alone confer
no runtime, execution, security or reclamation authority.

## Evidence and replay

- ASan/UBSan/LSan outside the sandbox, leak detection enabled: 34,876 checks.
- Actual cross-built issuer object with sanitized host driver: 34,876 checks;
  the target object itself is not instrumented in this mode.
- Eight threads reserve 16,384 actual-issuer IDs; sorted results are unique and
  contiguous. A separate terminal race permits one winner and preserves the
  seven losing outputs. This is a concurrency oracle, not ThreadSanitizer.
- Five mutants (wrap, old/repeated output, reset singleton, burn invalid request)
  fail named behavioral assertions in both modes: ten rejections. An initial
  mutant recipe failed to compile and is excluded; the corrected recipe passes.
- Os64 guest: 2,071 checks, heap integrity/accounting/live allocation balance,
  exact child exit `0x4f490200` and parent `OWNER INCARNATION QEMU COMPLETE PASS`.
  Four-file closure: child, parent, libos64 and libfreetype; no engine loaded.
- Production link audit: unchanged 84 runtime exports, 32 math/26 os64 imports.

```sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_js_owner_incarnation_host.sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_js_owner_incarnation_mutations.py --output /tmp/incarnation-mutants-new
python3 tools/test_js_owner_incarnation_guest.py --base-image /path/to/testrun-nonet.img --boot-iso /path/to/matching.iso --output /tmp/incarnation-guest-new
```

Guest inputs/receipts follow the metadata oracle's copy-and-preserve protocol.
Source/build-input and object hashes plus the cohort SHA-256 manifest accompany
the receipt. NONET's unrelated ARP preboot failure is outside the child oracle.
There is no new P5 claim or runtime-teardown-profile proof.

Quinn agreed the three-cut boundary: issuer, real runtime binding, then explicitly
selected private storage admission. Later cuts must prove actual runtime use,
admission rollback and real allocation charge under both teardown policies.
They must not silently charge metadata to legacy constructors. No checked
public profile, export migration, C3 agreement, pins, release or reuse is enabled.
F15 remains in progress.
