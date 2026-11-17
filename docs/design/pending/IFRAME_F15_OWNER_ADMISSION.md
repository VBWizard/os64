# F15 — explicit private metadata admission

Implementation PR: **#639**, source `88e74211`; depends on #638/#635.

Depends on [runtime identity](IFRAME_F15_RUNTIME_IDENTITY.md) (#638) and the
[issuer](IFRAME_F15_INCARNATION.md) (#635). Proof baseline `763c6fda`, engine
`ab1447d8`, embedding ABI 5. This reserves bounded storage; it does not select
checked export lifetimes or reclaim a realm.

The subsequent [realm-owner milestone](IFRAME_F15_REALM_OWNERS.md) assigns actual
primary/child owners and adds private checked source execution. The results
below describe the admission-only cut.

## Implemented boundary

The hidden `js_owner_runtime_create` constructor explicitly selects metadata
capacities and inherits ABI 5 creation's configuration/output alias and owner-thread
obligations. The public constructors pass no selection and allocate no arena.
They keep the existing wrapper charge, engine allowance, defaults, limits and
borrowed-pointer lifetimes. The wrapper stays **13,080 bytes**: a union holds
its numeric incarnation or private sidecar pointer, and a spare padding byte
records selection. There is no public header, export or ABI change.

The selected path checks arena sizing/overflow before reserving an identity.
After wrapper allocation it checks the requested reservation, allocates one
aligned host block for the header and pools, then checks its **actual usable
allocation charge** against the remaining configured memory. Wrapper charge
plus metadata charge is subtracted from the engine allowance before creating
the engine. Rounded-charge refusal frees both host allocations and publishes
nothing. Engine/context construction refusal unwinds the engine before freeing
the native arena; failed admission still burns its reserved incarnation.

The arena and table use the runtime's actual incarnation. Its scalar query
returns capacities, metadata counts, requested/charged bytes, wrapper charge
and engine allowance. It allocates/calls no engine and remains valid through
synchronous destruction finalizers. Independent host storage is freed after
normal engine teardown or tracked engine-leak bailout, then the wrapper ends.

The table has no owner rows or tickets issued by this cut. Those empty metadata
counts do not describe the live engine's owners, exported pointers or graph.
Existing raw APIs keep their legacy lifetimes even on this private construction
path. There is no operation to expose this table, migrate exports, shorten
borrows, detach providers, uncharge owners, release roots or reuse slots.

## Evidence and replay

- Full leak-enabled ASan/UBSan/LSan host suites outside sandbox pass **30,143**
  shipping and **100,503** instrumented checks, with zero failures/live
  allocations. Both root teardown policies, compile-failure sweeps and fatal
  exit badges pass. The successful constructor trace is faulted one allocation at a time: **56**
  shipping and **1,090** instrumented cuts per policy, with arena/engine/native
  heap balance after refusal. A deliberate retained engine object exercises
  reporting teardown's tracked bailout and independent arena cleanup.
- The native heap census equals wrapper plus metadata plus engine charges.
  Non-aligned requested size exercises actual rounding. A ceiling allowing the
  request but refusing its rounded host charge publishes no runtime.
- A pressure allocation succeeds on the legacy sibling and fails with MEMORY
  on the selected runtime. Its identity and metadata survive the failure;
  native finalizers inspect the original charged metadata during teardown.
- Seven defects (requested instead of actual charge, skipped engine reservation,
  default arena selection, construction/destroy leaks, ending metadata before
  finalizers, wrong capacity) fail named assertions in both builds: **14**
  negative controls. Existing identity counterexamples cover the union getter.
  All eight existing identity counterexamples are also rejected after the union change.
  Negative failures, including deliberate leak variants, are not positive LSan
  receipts; signals/build failures cannot count as behavioral rejection.
- Os64: **428 checks**, heap integrity/accounting/live blocks/live bytes balance,
  exact child exit **0x4f520300** and parent `OWNER RUNTIME QEMU COMPLETE PASS`.
  It links production runtime/issuer/table/core objects directly. The five-file
  closure includes child, parent, libmath, libos64 and libfreetype. Copied ELFs
  omit unloaded debug sections; source and object hashes accompany the receipt.
- Default CLI passes 199 stub/40 real checks; the pinned test262 format-name
  subset passes 2/2. Production audit
  retains 84 runtime exports / 32 math / 26 os64 imports. Header/manifest,
  allocation/global/visibility and wrapper layout checks pass.

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_js_owner_admission_mutations.py --artifacts /path/printed/by/host/script --output /tmp/owner-admission-mutants-new
python3 tools/test_js_owner_runtime_guest.py --base-image /path/to/testrun-nonet.img --boot-iso /path/to/matching.iso --output /tmp/owner-admission-guest-new
```

The shared consumer includes the earlier runtime identity tests. Six sibling
host builders compile both private dependencies; the nested test262/Intl
builder is included. NONET's unrelated ARP preboot failure remains outside the
child/parent oracle. There is no new P5 acceptance claim.

Quinn agreed the private opt-in boundary and actual-charge/rollback ordering.
Public checked-profile negotiation, owner-row assignment to realms, ticket
export migration, conservative legacy escape accounting, F4 C3 publication,
independent retained-edge proofs and reclamation remain separate work. Zero
metadata counts grant no release authority. F15 remains code.
