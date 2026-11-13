# F.4a — embedding global-this validation

Implementation based on `54ef25c2` (the F.4 brief), whose runtime baseline is
merged F.3b `6e25f323`. This cut adds a dormant engine primitive. Existing DOM
and Yonder consumers do not install a substitute receiver; WindowProxy,
settings attribution and cross-origin gates remain F.4b–d.

## Implemented contract

Patch `0022-browser-global-this.patch` adds `JS_SetGlobalThis` and a context
hold. The unset sentinel uses the original global without adding a reference.
Configured receivers are duplicated before replacement, marked during GC
and released during context destruction. Non-objects are refused without
changing the stored value or pending exception. Same-runtime ownership and
setup-before-author-execution are trusted embedding preconditions, not a
security validation of arbitrary JSValue bits.

The engine selects the receiver in sloppy null/undefined calls, ordinary
script evaluation, compiled script evaluation and indirect eval. Explicit
receivers, strict function calls and module this are preserved. Global
name/lexical storage, `JS_GetGlobalObject` and the writable `globalThis`
property are separate. The setter does not install DOM aliases.

Prepared engine identity:
`48a503302f21f8cd18a42534ff37d36e30d5ab84d9ee933edb9de270999edda3`.
Wrapper ABI 5/public layouts are unchanged; the new raw symbol is additive.
Bytecode-cache identity includes the prepared engine change.

## Evidence

- Full target runtime: **13,342 checks**, zero failures, zero live allocations.
- Full instrumented runtime: **60,690 checks**, zero failures, zero live
  allocations. Both engines' compile-failure sweeps and fatal-invariant
  subprocess checks pass; the runtime harness reports the expected exit badge.
- Final focused selector `--global-this`: **67 checks per engine**, zero
  failures, including the exported-receiver lifetime case. The shared fixture
  is included in the guest runtime source and both host engines.
- Six engine preparation tests, manifest/hash validation and contract-header
  syntax checks pass. Target runtime library/consumer link and the target
  allocation/global/visibility audit pass.

The full runtime run uses the same implementation as the final focused
checks. The focused fixture was subsequently strengthened with a native
function/context cycle and an exported receiver retained across context
collection. Those fixture-only additions are qualified separately; the full
suite totals above are not presented as counts from that later fixture.

The first fixture run incorrectly omitted bytecode `.keep` and invoked a
module without a host task. Both fixture errors were corrected before the
passing full run. They required no engine change. LSan's sandbox run reported
thread attachment `EPERM`; `TracerPid` was zero. Passing runs used the existing
binaries outside the sandbox with `ASAN_OPTIONS=detect_leaks=1`. ASan/UBSan
remained enabled; the instrumented engine uses the harness's established
QuickJS UBSan exclusion while the adapter/runtime remain instrumented.

Commands:

```sh
CCACHE_DIR=/tmp/os64-f3a-ccache ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 \
  bash tools/test_js_runtime_host.sh
python3 tools/test_js_prepare.py
bash tools/test_js_contract_headers.sh
make -C userland -j8 js-runtime-test
python3 tools/test_js_teardown_audit.py
# Reuse the harness's probe and sanitized-probe outside a restricted sandbox:
ASAN_OPTIONS=detect_leaks=1 /path/to/probe --global-this
ASAN_OPTIONS=detect_leaks=1 /path/to/sanitized-probe --global-this
```

The focused cases cover independent realms; top-level, strict and sloppy
calls; primitive and explicit receivers; direct/indirect eval; Function,
generator, async and async-generator calls; cached bytecode; resumed modules;
foreign bound/proxy/Promise calls; globalThis property poisoning; replacement,
self-assignment, invalid-input exception preservation and GC/root teardown.

Fault injection independently removes each of the four receiver-selection
changes, the GC mark, the release and the non-object guard. The initial
release mutant survived a wholly cyclic teardown: collection hid the missing
explicit release. The exported-receiver case keeps that object alive while
its context is collected, testing the hold independently of object collection.
The final test fixture catches **all seven mutants**: the four selection
omissions and invalid-type acceptance fail behavioral assertions; missing
mark/release edges reach the fatal engine invariant check. The normal
implementation passes both focused builds with leak detection enabled.

## Exposure blockers and integration limits

This is not proof that author code cannot receive the real global. The
prepared engine audit also found global-property accessor/reference paths
(`JS_GetGlobalVarRef`, `OP_get_var`, `OP_put_var`, `OP_put_ref_value`) and the
`groupBy` callback receiver using global storage. F.4d must resolve their
browser semantics and test for escape before publishing a WindowProxy.
The shared GroupBy implementation passes the real global as callback this;
[ECMAScript GroupBy](https://tc39.es/ecma262/multipage/keyed-collections.html#sec-groupby)
requires an undefined receiver. That existing behavior is not changed by
the dormant hook. Function-constructor parsing also evaluates its generated
function expression against global storage; the resulting callable's receiver
is selected on invocation, as exercised by this cut.

No new browser-frame WPT, corpus, QEMU execution or P5 acceptance is claimed.
Existing single-document consumers remain unconfigured, and full runtime
checks cover that default path. Frame-host and security acceptance remain in
the [F.4 brief](IFRAME_F4_SECURITY.md); changed normal-browsing behavior requires
its own small P5 regression cohort.

F.16's dormant `0023-execution-scope-snapshot.patch` was composed with this
patch in both orders after 0021 using zero fuzz. The other lane reports
byte-identical prepared `quickjs.c` and `quickjs.h` in both orders. Its combined
ASan engine also passes the 115-check execution-scope selector and a 67-check
global-this selector with LSan enabled, using coherent dependent objects.
This is focused combined evidence, not a full combined-runtime run or proof
of recovery/settings-security policy. Final F.4a lifetime/mutant evidence
above is qualified against its standalone patch.
