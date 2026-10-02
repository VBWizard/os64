# JavaScript foundation validation

Worktree: `.worktrees/js-runtime-foundation`, branch
`codex/js-runtime-foundation`, base `b55c3770`. This foundation has no libjs.so
or runner; runtime headers and examples are interface proposals for review.

- `tools/test_js_contract_headers.sh`: manifest hashes verified; runner/job
  examples syntax-check with the cross compiler; custom binding syntax-checks
  with host compatibility headers and upstream system-include warning scope.
- `tools/test_js_engine_host.sh`: PASS. The five pinned engine files, patched
  in a disposable directory to omit Atomics while retaining stack checks,
  execute arithmetic, BigInt, regex, JSON, one Promise job, host-capability
  absence checks, and recursive stack failure. Follow-up cases verify engine
  reuse and retained jobs after script/job exceptions, rejected-promise handling,
  and class-ID slot reuse. A run with a PATH containing the required tools but
  no ripgrep passes; a deliberately ineffective patch is rejected by the
  Atomics profile guard. This uses host libc and libm, not the os64 wrapper.
- `tools/test_js_support_host.sh`: PASS. ASan/UBSan string boundary fixtures
  pass; the existing heap host battery plus size-query cases passes 215 checks,
  including operation-specific diagnostics for free/realloc/size-query misuse.
  LeakSanitizer needs permission to inspect/suspend threads through ptrace;
  the recorded leak verdict comes from normal-exit runs with that permission.
  It remains enabled for the string fixture. The existing heap death harness
  uses longjmp and deliberately leaves corrupt allocator state, so its battery
  is separate from that sanitizer executable.
- Authored source and documentation pass `git diff --check`; the byte-identical
  upstream import retains its original whitespace, and the unified patch retains
  space-prefixed blank context lines. These two paths are excluded from the
  authored-file whitespace check. `tools/stale_refs.sh` was read in full:
  follow-up hits are the existing public-header link and live status shorthands
  for prefixed enums in js.h, not retired APIs. It reports no new superlative
  claims. Removed class-ID/accessor signatures have no remaining live callers.
- Strict `make -C userland -j8` and root `make -j8`: PASS. The unchanged kernel
  is built to package the guest image; existing ignored build dependencies
  were copied from the main checkout into this isolated worktree.

Guest evidence: a fresh QEMU boot of the rebuilt image ran
`/tests/testrun jssupporttest`: 1 passed, 0 failed, 0 skipped. The dispatcher
accepted the fixture's `0x4A535550` (JSUP) badge. `/tests/testrun malloctest`:
4 passed, 0 failed, 0 skipped, including the four-thread and death fixtures.
Both dispatcher runs returned status 0.
The guest's `/lib/libos64.so` was copied through `/home` and compared with
`userland/bin/libos64.so`: byte-identical, 789272 bytes. The VM used disposable
copies of both worktree disks, with FIFO/QMP monitor helpers copied into `/tmp`
from the main checkout. Boot and all commands ran in one foreground tool
process because the validation executor terminates background processes when
the foreground command completes.
The VM was stopped after capture. `make` builds the boot/root image; the separate
`disk/os64_data.img` target created a fresh home disk for this validation.

The library prerequisite code is isolated in commits `f7f3c8c1` and `d15828f1` on
`codex/js-support`; the interface and source-preparation work is stacked above
it on `codex/js-runtime-foundation`. The main checkout's concurrent work is
preserved separately.
R0 behaviour, target QuickJS C adaptation, maths conformance, runtime limits,
unhandled rejections, cancellation, leaked-value fatal handling, and browser
integration are not validated by these foundation checks.

## R1 target adaptation evidence, 2026-10-01

Worktree `.worktrees/js-target-port`, branch `codex/js-target-port`, base
`a426386d` (merged PRs #188 and #189). This slice implements the private target
adapter and engine build; it does not implement the os64 runtime API or runner.

- `make -C userland -j8 js-core`: PASS, strict cross compilation of all five
  engine files and the four adapter files, then a partial link with the required
  cross-libgcc helpers. No upstream original was modified.
- `tools/test_js_port_target.sh`: PASS. The remaining imports are exactly 32
  maths functions and 18 libos64 services (plus the linker GOT marker); no host
  libc, pthread or unresolved compiler-helper names remain. The target binding
  example compiles. A symbol-only shared link with trap-only dependency libraries
  has 186 engine exports, including `__JS_FreeValue` and `__JS_FreeValueRT`,
  matching the compiled example's engine imports. Adapter/compiler helpers stay
  hidden; the ELF has SysV hashes, named dependencies, no text relocations and
  no writable/executable LOAD segment. These traps provide no maths or OS
  behavior and are not production binaries or guest evidence.
- `tools/test_js_port_host.sh`: PASS. The sanitized source/adapter build passes
  675 checks; the actual cross-built core and its integer helpers, linked into
  a host fixture executable, pass 437 checks. Both use host libm and controlled
  heap/clock/syscall inputs. Cases cover rounded-capacity accounting, failure
  rollback, integer/truncation/stream formatting, diagnostic ties-even versus
  JavaScript ties-away rounding, epoch/subsecond conversion, negative dates,
  expanded years, timezone signs and DST transition boundaries, BigInt, regex,
  JSON, stack checks, Promise jobs, repeated lifecycle and constructor allocation
  failure cleanup, including a caller-owned constructor ceiling and distinct
  budget/OS-allocation failure flags. ASan makes upstream use large allocations
  instead of small-object arenas, so the builds have different constructor
  allocation inventories; the sweep measures each rather than hard-coding it.
  ASan covers the host-built engine; ASan/UBSan cover the adapter and fixture
  support. The prebuilt cross core is not sanitizer-instrumented. LeakSanitizer
  stays enabled for normal exits; its thread inspection requires ptrace
  permission, which was available for the recorded normal-exit leak checks.
  The separate fatal processes exit immediately through
  the host exit hook, which verifies the full os64 JSFA badge and maps it to
  host status 99; this is not a guest exit-status observation.
- `tools/test_js_contract_headers.sh` and `tools/test_js_engine_host.sh`: PASS,
  including hashes for the expanded patch series. The baseline engine probe
  intentionally remains an engine-only host check with patch 0001.
- Strict `make -C userland -j8`: PASS. R1's explicit core target is separate
  from the default image population. Adding libjs/libmath to the placement map
  leaves the existing libraries' assigned addresses unchanged.

This initial R1 handoff did not include a QEMU engine run, libmath numerical
evidence, production `libjs.so` link or runner. Production linkage is added in
the review follow-up below. R2 must add its embedding operations and capability
policy before default image registration.
The allocator's generic fallback realloc references remain visible in the audit;
wholesale teardown reclamation and its allocation-ledger proof remain later work.

## R1 review follow-up evidence, 2026-10-02

The branch merges `userland` at `d5eecf1b`, including the accepted M1 library
(PR #191, merge `3b509e8b`) and the shared-library dependency corrections
(PR #193). The placement conflict retains both libmath and libjs in the map.

- The zero-byte allocation fatal fixture failed against the previous adapter:
  both normal suites passed, but `js_malloc(0)` returned rather than taking
  the invariant-failure path. The corrected adapter diagnoses
  `zero-byte allocation`; `js_realloc(NULL, 0)` remains allocation-free and
  an existing block resized to zero is released.
- `tools/test_js_port_target.sh`: PASS. The maintained test builds the real
  production `libjs.so` with `--no-undefined`, checks its two named dependencies,
  verifies all 32 maths and 18 libos64 imports against their real exports, and
  retains the 186-symbol engine surface and ELF protections. Generated core
  and formatter dependencies identify libmath's public `math.h`; the duplicate
  private header is removed. Dry runs freeze binary inputs to verify that
  changing either `libjs/shared.mk` or `tools/app_bases.py` schedules a relink
  through its direct prerequisite.
- `tools/test_js_port_host.sh`: PASS, 675 sanitized source/adapter checks and
  437 cross-core host checks, zero failures. Host libm remains the numerical
  substitute for these execution fixtures. Leak detection remains enabled on
  normal exits with ptrace permission. Separate processes for leaked values,
  clock failure and zero-byte allocation pass for both builds, checking the
  named invariant diagnostic and full JSFA badge through the host exit hook.
- `tools/test_js_contract_headers.sh`, `tools/test_js_engine_host.sh`, and
  strict `make -C userland -j8`: PASS.

The real dependency link is build evidence, not guest engine execution or a
new numerical conformance measurement. R2, the runner, and combined QEMU
acceptance remain open.

## R2 runtime core evidence, 2026-10-02

Worktree `.worktrees/js-runtime-core`, branch `codex/js-runtime-core`, base
`ef43d227` (merged R1). This implements eight runtime operations alongside the
186-symbol engine API. File/output/argument helpers, the runner and normal
image/licence registration remain separate work.

- Strict `make -C userland -j8 js-runtime-test` and root `make -j8`: PASS.
  Runtime and guest-consumer code retain `-Wall -Wextra -Werror`; root builds
  the unchanged kernel and normal image. Existing ignored dependency sources
  were copied from the main checkout. The optional consumer's `jsembedtest`
  slot is `0x14800000`; its addition preserves all 161 existing application
  addresses. The normal image does not include libjs or this consumer.
- `tools/test_js_port_target.sh`: PASS. The core retains 32 maths and 18 os64
  imports. The real shared library adds `os64_micros`, exports eight wrapper
  operations, and retains its two named real dependencies, SysV hash and ELF
  protections. Adapter/compiler helpers remain hidden. Binary inputs, including
  the runtime object, are frozen in dry runs to verify direct relink edges.
- `tools/test_js_contract_headers.sh`: PASS, including the four-patch manifest
  and unchanged upstream-original hashes.
- `tools/test_js_runtime_host.sh`: PASS, 475 checks with the actual target core
  and libmath objects; 2,539 with the engine source instrumented by ASan. Both
  use the actual cross-built M1 maths, controlled host heap/syscall fixtures,
  ASan/UBSan wrapper/support code, normal-exit LeakSanitizer and zero live
  fixture allocations. The target engine/math objects themselves are not
  sanitizer-instrumented. ASan bypasses engine arenas, so the constructor sweep
  measures 1,086 native allocations versus the target profile's 54. Cases cover
  repeated lifecycle, ABI mismatches, native callbacks/classes/finalizers,
  teardown/re-entry guards (including aliased outcomes), absence of host/shared
  capabilities, exceptions/reuse, cumulative jobs and deadlines, rejection
  checkpoints and safe diagnostics, source/memory limits, clock/heap failure,
  truncation, serialized class slots and real cross-thread cancellation.
  LeakSanitizer remains enabled with its required thread-inspection permission.
- Separate fatal fixture processes for leaked values and active destruction
  pass with both profiles. The host exit hook checks the full JSFA badge and
  maps it to host status 99; the guest observations below independently verify
  the os64 status.

Constructor refusal initially reproduced four defects in the pinned source.
The actual target profile exposed GC-list retention of a freed raw context
and an unchecked global-variable object allocation. The finer sanitized sweep
also exposed a leaked lazy-global value and a second release after Proxy's
consuming property installation failed. Patch 0004 fixes those four sites;
upstream originals remain byte-identical. The target raw-context regression
and both complete constructor-failure sweeps pass after the corrections.

The first guest run overlapped the kernel's late memory measurement with our
consumer commands. The JavaScript fixture passed, but the background teardown
measurement reported losses. A second boot waited for the kernel test before
running commands: two independently measured quiet windows passed with zero
loss. This distinguishes the consumer fixture's result from the first boot's
background measurement; it is not a kernel fix or proof of concurrent-test
isolation. Final guest results and returned-library hashes are recorded below.

The final QEMU run (VM 56204, eight cores) used this worktree's normal ISO and
disposable copies of its root/home images. The root copy received the explicit
`js-runtime-test` ELF at `/tests/jsembedtest`, `libjs.so` at `/lib/libjs.so` and
the retained QuickJS licence. After a 45-second quiet boot, the kernel teardown
test had passed before any consumer commands ran. `/tests/jsembedtest` then
reported **280 checks, zero failures, 37 native calls**, including the real
monotonic deadline and finalizer re-entry checks. Husk's captured full exit
status was `0x4A535254` (JSRT). Separate `--leak` and `--active-destroy` runs
diagnosed their intended invariants and returned `0x4A534641` (JSFA).

Guest `cp` copied the three loaded library files to `/home`; after `sync`,
`vmget` returned them for byte-for-byte comparison with the build outputs.
All three matched:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| libjs.so | 4,486,952 | `492f6da78cd8995ff2b8dbaf597692aff119f37efe5b94e8337d5cff667977ef` |
| libmath.so | 150,400 | `3e9cfc3b62a863895bec2291975bd97c568ea713c5744150b0a83d97bfa0c6ae` |
| libos64.so | 789,248 | `514270439f291eb2f1a738010f703934c6e2224fe4086acdd88bd3f03fd2315c` |

The screenshot showed both intended fatal diagnostics and a returned Husk
prompt; the VM was stopped after extraction. These were scratch-image
insertions, not additions to the normal image's installation rules.

This core fixture is not a C1 runner, full ECMAScript/numerical conformance
suite, complete interrupt-coverage audit, browser integration test or J1/J2
acceptance. Production limits remain unselected. Structured line/column data
remains unavailable, and stack overflow remains an ordinary engine exception
because no reliable wrapper-owned stack-failure signal is exposed.

## R2 helper slice evidence, 2026-10-02

Worktree `.worktrees/js-runtime-helpers`, branch `codex/js-runtime-helpers`,
stacked on core commit `45cae4fc` in PR #194. This adds `run_file`,
`install_output` and `install_args`; the shared library now exports eleven
embedding operations plus the 186 engine symbols. There is no C1 runner change.

- Strict explicit guest-consumer build and root `make -j8`: PASS. Normal image
  installation and application placements are unchanged by the helper slice.
- Target/ELF/dependency/relink audit: PASS, 32 maths and 22 os64 imports with
  real libmath/libos64 dependencies. The core's own inventory remains 32 maths
  and 18 os64 imports. Header/manifest checks and shell syntax: PASS.
- Maintained host suite: PASS, **642 checks** with the actual target engine and
  M1 maths; **2,784 checks** with ASan on the engine. Both retain ASan/UBSan on
  the wrapper/support fixtures, enabled normal-exit LeakSanitizer and zero live
  fixture allocations. Both intentional fatal modes retain their expected JSFA
  badge. The original core cases run along with the helper cases.
- Added cases cover copied UTF-8 arguments, empty arguments, invalid masks and
  pointers/counts, installer repetition and post-evaluation refusal, native
  helper re-entry, selected names and existing console-member preservation,
  embedded-NUL conversion, partial/zero/error writes, conversion exceptions and
  deadlines, fixed-property installation errors, avoiding a console getter,
  constructor-independent installer allocation-failure sweeps, argument memory
  ceilings, short/empty/exact/oversized file reads, file-buffer allocation and
  memory-ceiling failures, cancellation during loading, owned close on failure,
  first read/close error precedence, loading before the deadline, BUSY without
  opening input, file exceptions/reuse, and late deadline-overflow refusal
  preserving the previous completed turn's state. The named os64 close errors
  exercised here are NOT_COMMITTED (`-3`) and DEFERRED (`-5`).

QEMU VM 56205 used this worktree's normal ISO and disposable root/home copies,
with the explicit consumer, libjs and QuickJS licence inserted into the root
copy as in the core run. The existing kernel teardown test passed after a
45-second quiet boot, before consumer commands. The consumer passed **319
checks, zero failures, 38 native calls**, including real os64 file creation,
12,000-byte file loading/growth, Promise draining, empty/missing input, copied
arguments, selected output names, borrowed output lifetime and byte comparison
of output containing an embedded NUL. The captured full status was JSRT
(`0x4A535254`); both intentional fatal modes returned JSFA (`0x4A534641`).

After guest `cp` and `sync`, returned library copies matched the build files
byte for byte:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| libjs.so | 4,510,744 | `022cf37e1d0bc5a7e2a11379df6221a967c327902ae6835a906a8536601c7c49` |
| libmath.so | 150,408 | `d1a505d69987c9c22f1348cabae50a6db7703133e704ee6e8e58dbb90b99dfc8` |
| libos64.so | 789,264 | `8004c9774c27eb6ad51c24fa4fa1ab09f8bbc245d64322a88b02912492951bd8` |

The screenshot showed the expected invariant diagnostics and returned shell
prompt; VM 56205 was stopped after extraction. These fixtures validate the R2
library handoff; Opus's C1 integration, independent V1 validation, normal
image/licence registration and J1/J2 acceptance remain separate gates.
