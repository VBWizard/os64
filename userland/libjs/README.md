# JavaScript embedding library

R2's two slices implement runtime construction/destruction, ABI-checked context
access, serialized class-ID allocation, bounded source evaluation, controlled
Promise jobs, execute-and-drain, diagnostics, sticky cancellation, bounded file
loading, selected output bindings and copied arguments. Native
hosts register capabilities through the borrowed context using the pinned
QuickJS API. Creation removes SharedArrayBuffer exposure and grants no host
output, arguments, filesystem, networking or process functions.

`make -C userland js-core` builds the freestanding engine and compiler helpers.
`make -C userland js-library` links the engine and runtime against the real
libmath/libos64 libraries. Both are explicit targets; the default image does
not install libjs. `js-runtime-test` additionally builds the guest consumer at
`userland/obj/js/runtime-core-test`, for installation as `/tests/jsembedtest`
in a disposable validation image. Its reserved slot preserves the existing
application addresses.

Read CONTRACT.md before implementing a consumer and port/README.md for the
adapter's accounting, headers and diagnostic formats. Upstream originals stay
byte-identical; generated copies receive the hash-checked manifest patch series.
`exports.map` publishes 186 engine symbols and eleven runtime symbols, with
adapter/compiler helpers private. The two R2 slices provide the complete
standalone embedding interface. Their review/merge, Opus's C1 runner integration,
image/licence installation and independent
validation are tracked in JAVASCRIPT_TASKS.md.

Checks:

- `tools/test_js_runtime_host.sh`: shared guest/host consumer cases, the actual
  cross-built engine and M1 maths, an ASan engine build, ASan/UBSan wrapper and
  support fixtures, normal-exit leak checks, allocation-failure sweeps, clock
  failures/deadlines, native bindings/finalizers, Promise checkpoints, class
  allocation across threads, cross-thread cancellation, owned input cleanup,
  partial output writes, transactional property/handle publication, copied
  arguments and installer failure sweeps. Fatal fixtures
  verify the full JSFA badge through a host exit hook.
- `tools/test_js_port_target.sh`: strict target core and binding example,
  import/export and ELF audits, a symbol-only core link with trap dependencies,
  real shared-library dependencies, public maths headers and relink triggers.
- `tools/test_js_contract_headers.sh`: source hashes and contract examples.
- `tools/test_js_port_host.sh`: adapter fixtures with ASan/UBSan and the actual
  cross-built core on the host, using host libm as a substitute for M1.
- `tools/test_js_engine_host.sh`: isolated upstream engine baseline.
- `tools/test_js_support_host.sh`: libos64 prerequisites.

VALIDATION.md records measured results and distinguishes host fixtures from
actual os64 execution. These checks do not establish full ECMAScript/numerical
conformance, complete interrupt coverage or browser event-loop integration.
