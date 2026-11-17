# F15 — binding incarnations to actual runtimes

Implementation PR: **#638**, source `fadc1744`; depends on #635.

Depends on the [private issuer](IFRAME_F15_INCARNATION.md) (#635). Proof baseline:
`e0b8f188`, engine `ab1447d8`, embedding ABI 5. This binds its process-local
namespace to wrapper runtime construction; it does not expose checked tickets.

After existing argument, ABI, clock, stack and wrapper-storage validation,
creation reserves an incarnation before allocating the wrapper. Exhaustion
returns `ADMISSION_REFUSED` with null output before allocation/publication.
Later allocation/construction failure burns the reserved ID. Successful
construction stores it until the wrapper is freed; cancellation, engine failure,
realm activity and synchronous destruction finalizers do not replace it.

The hidden `js_owner_runtime_incarnation` query takes a caller-supplied live
runtime on its owner thread, including synchronous finalizer unwind. NULL
returns zero. A copied number neither retains storage nor authorizes execution,
security, publication or reclamation. The native caller must not dereference a
freed runtime; checked operations will compare old IDs against a new supplied
live runtime rather than follow token-owned pointers.

Moving the existing `host_settings_enabled` boolean into tail padding leaves
`sizeof(os64_js_runtime_t)` at **13,080 bytes**, matching the predecessor. No
metadata arena is allocated, engine allowance is unchanged, and no public
header/export, ABI, profile, realm cap, root release or reuse changes. Numeric
issuer exhaustion is the new finite refusal condition. Sibling host
builders compile the issuer beside runtime.c; explicit runtime harness links
include that object. This keeps CLI, DOM, acceptance and Yonder fixture builds
consistent with production's new private dependency.

## Evidence

- Leak-enabled ASan/UBSan host checks outside sandbox: full shipping-engine
  suite **29,870 checks**, instrumented-engine suite **96,094 checks**, zero
  failures/live allocations. Compile-failure sweeps and full fatal-exit badges
  pass in both builds.
- Both teardown policies create/destroy 32 pairs of real runtimes. Native
  finalizers observe the original identity during destruction; a live sibling
  retains its separate identity. Failed wrapper allocation burns its number
  without publication/leak. A host-only issuer call-boundary hook injects
  exhaustion to prove refusal occurs before allocation; production has no
  counter reset/fault-injection interface.
- Four defects (constant identity, clear at finalization, hide after cancel,
  ignore exhausted issuer) fail named assertions on both engine builds:
  **eight negative controls**. These failures are not positive leak receipts.
- Os64: **388 checks**, target heap balances, child exit `0x4f520200`, parent
  `OWNER RUNTIME QEMU COMPLETE PASS`. The executable links the actual runtime,
  issuer and engine objects directly so inspection stays hidden. The five-file
  dependency closure contains child, parent, libmath, libos64 and libfreetype.
  Only unloaded debug sections are stripped from the frozen executable copies.
- CLI: 199 stub and 40 real-library checks, zero failures. Production audit
  remains 84 runtime exports / 32 math / 26 os64 imports. Manifest/header and
  teardown allocation/global/visibility audits pass. The wrapper layout and
  guest source/build-input/object hashes are recorded with the receipt.

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_js_owner_runtime_mutations.py --artifacts /path/printed/by/host/script --output /tmp/runtime-identity-mutants-new
python3 tools/test_js_owner_runtime_guest.py --base-image /path/to/testrun-nonet.img --boot-iso /path/to/matching.iso --output /tmp/runtime-identity-guest-new
```

The guest script preserves inputs and prior output directories. Initial guest
staging attempts (FAT capacity, then a manifest path) supply no positive receipt;
the accepted run is a fresh directory with complete hashes and parent verdict.
The unrelated NONET ARP preboot failure is outside this consumer's oracle.
No P5 acceptance, checked runtime profile, metadata storage admission, exported
owner migration, C3 agreement, context pin or reclamation is claimed.

Quinn agreed the runtime.c ownership and private boundary. Storage admission is
a separate explicitly selected private-constructor cut; legacy constructors
must not silently reserve that arena or lose engine allowance. F15 stays code.

The shared test262/Intl runner builder also compiles the issuer. Both affected
entry points run with leak detection: the pinned DateTimeFormat format-name
subset passes 2/2, and the browser capture agrees on 5,635/5,635 fields across
1,127 option sets. A recursive source-builder audit includes nested tools.
