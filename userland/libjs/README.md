# JavaScript library foundation

The reviewed R0 header and runtime contract describe the os64 embedding API;
those runtime operations and the runner are not implemented yet. R1 now has a
freestanding QuickJS core and private os64 adaptation. `make -C userland js-core`
builds the cross-compiled, partially linked core with its compiler helpers.
`make -C userland js-library` is the engine shared-library link and requires
M1's real `libmath.so`; it is not part of the default image build.

Read CONTRACT.md before implementing a consumer and port/README.md for the
adapter's accounting, headers, diagnostic formats and remaining dependencies.
Upstream originals stay byte-identical; generated copies receive the manifest's
patch series. `exports.map` exposes the pinned engine API, including helpers
used by its public inline functions, and keeps adapter internals private.

Checks:

- `tools/test_js_contract_headers.sh`: source hashes and R0 examples.
- `tools/test_js_port_target.sh`: strict target core, target binding example,
  import/export inventory, and a symbol-only ELF link with trap dependencies.
- `tools/test_js_port_host.sh`: adapter fixtures with ASan/UBSan, leak detection,
  controlled libos64 calendar/clock/heap inputs, and the actual cross-built core
  executed on the host. Host libm remains a substitute for M1.
- `tools/test_js_engine_host.sh`: isolated upstream engine baseline.
- `tools/test_js_support_host.sh`: libos64 prerequisites.

VALIDATION.md separates this evidence from guest execution. R2 implements the
os64 runtime boundary, limits, capability registration and shared-buffer
suppression. M1 supplies target maths. C1 supplies the thin runner. Browser
mutation and bindings are separate work.
