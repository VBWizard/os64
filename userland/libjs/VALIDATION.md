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

## PR #195 output transaction correction, 2026-10-02

The P2 finding was reproduced at `9af1f2a9`: a failing combined install replaced
or leaked `print`, and console-only retry retained that unselected capability.
The initial target-core negative control reported seven failed assertions,
including the resulting unwanted output bytes. Those shared regressions now
pass on both host profiles and inside os64.

The correction stages selected functions/atoms and retains complete property
descriptors before publishing. Failed attempts restore prior data/accessor
values and attributes or remove newly published names. The borrowed handle and
installation flag commit after publication succeeds; a function seen by a
property trap before commit cannot write. A trap or allocation refusal that
prevents restoration retires the runtime rather than leaving it reusable.

- Final maintained host suites: **698 target-engine checks** and **2,890
  sanitized-engine checks**, zero failures and zero live fixture allocations,
  with normal-exit LeakSanitizer enabled. The allocation sweep also checks
  absence of staged capabilities on failure. The sanitized accessor-publication
  sweep checks restoration of getter identity and property attributes; the
  target arena profile has no native allocations for that replacement fixture.
  A separate native exotic-property case proves that staged callbacks cannot
  write and that rollback refusal retires the runtime.
- Strict explicit consumer and root builds, target import/export/ELF/dependency
  and relink checks, and contract/manifest checks: PASS. The published symbol
  inventory and required libraries are unchanged by this correction.
- QEMU VM 56206: **365 checks, zero failures, 41 native calls**, with JSRT
  (`0x4A535254`) and both intended JSFA (`0x4A534641`) fatal statuses. Shared
  transaction cases cover non-object/accessor console rejection, fixed `log`
  and `print` targets, original print identity/attributes, getter non-execution,
  preserved console log and console-only retries. The kernel teardown test
  passed before consumer commands after a quiet boot; the VM was stopped after
  screenshot and file extraction.

All three returned guest library files matched the current build byte for byte.
The corrected libjs is 4,520,760 bytes with SHA-256
`00e6b3eb776b2cd0bff140ff7788ed3bbb2ccdad3b4a22ef0763d81312844b98`.
libmath and libos64 retain the sizes and hashes in the helper-slice table above.
These are scratch-image runtime observations; C1, independent validation and
normal image integration remain separate work.

## PR #195 EOF-before-growth correction, 2026-10-02

The second P2 finding was reproduced at `2fd6007d` with two failed assertions:
4,095- and 8,191-byte sources evaluated successfully through the buffer API
under measured memory budgets, but file evaluation exhausted those budgets.
The old reader grew at the buffer boundary before checking for EOF.

The reader now probes one byte at an intermediate buffer boundary. EOF keeps
the existing terminated-source capacity; extra input is retained after growth.
Probe read errors preserve their service code, cancellation is observed before
growth, and these outcomes still close the owned input. The wrapper's direct
file-to-evaluation handoff is distinct from the engine's compiler allocations.

- Maintained host suites: **822 target-engine checks** and **3,014
  sanitized-engine checks**, zero failures and zero live fixture allocations,
  with normal-exit LeakSanitizer enabled and expected fatal badges. Cases cover
  both boundary sizes, equivalent buffer evaluation, one byte beyond each
  boundary, probe read/close error precedence, cancellation on a positive/EOF
  probe, short reads preserving the probed byte, and growth-allocation refusal
  after the extra byte is confirmed. Construction accounting is measured for
  each engine profile. Extra-byte sources can fit the sanitized profile's
  budget; the actual guest profile reaches its memory limit after growth.
- Strict explicit consumer and root builds, header/manifest checks, target
  import/export/ELF/dependency and relink audits: PASS. Symbol inventories and
  dependencies are unchanged.
- QEMU VM 56207: **395 checks, zero failures, 41 native calls**. Both boundary
  sizes pass file and buffer evaluation under the same measured budget; the
  additional-byte fixtures return LIMIT/MEMORY. The full JSRT success status
  and both intentional JSFA fatal statuses match expectations. The kernel
  teardown test passed after a quiet boot before consumer commands. The
  screenshot showed the returned shell and expected invariant diagnostics;
  the VM was stopped after extraction.

Guest copies of all three libraries matched the current build byte for byte.
libjs is 4,521,008 bytes with SHA-256
`5e5be11499c7bf5f12bacb20c94ea0002595f4b31142823f561f38bf1160e5ba`.
libmath and libos64 retain the sizes and hashes in the helper-slice table above.
C1, independent validation and normal image integration remain separate gates.

## V1 consumer acceptance, 2026-10-02

Worktree `.worktrees/js-validation`, branch `codex/js-validation`, based on
merged M1/R1/R2/C1 at `a2e409a7` plus task-status documentation `2b3ee4cb`.
The maintained consumer and provenance/inventory are in `tools/js_acceptance`;
the host runner is `tools/test_js_acceptance_host.sh`. This suite does not include
R2's `cases.h` or private runtime state. Quinn authored both R2 and this suite;
independent review accepted the V1 packet in
[PR #197](https://github.com/VBWizard/os64/pull/197), merged as `99df2636`.
The observations below record its execution evidence.

The initial persistent OS-allocation-refusal fixture crashed in
`find_own_property`, reached from `build_backtrace` while converting a thrown
object. The builder borrowed `current_exception`; backtrace allocation could
replace that exception and free the object still being used. Manifest patch
0005 retains the error through assembly and frees the temporary buffer on early
failures. The reproducing fixture is unchanged in its refusal/conversion
behavior and now returns HOST_FAILURE, refuses reuse and destroys with zero
live allocations. The isolated `--diagnostic-oom` process also passes with two
refused OS allocations and zero live allocations in both host engine profiles.
The patch covers the shared builder used by its native, bytecode and parser
callers. Public ABI/layout, symbol inventories and capabilities are unchanged.

- `tools/test_js_acceptance_host.sh`: **383 checks, zero failures** for each
  of the actual target-core/M1-maths and ASan-instrumented engine profiles.
  Host-only diagnostic refusal verifies complete allocation reclamation;
  ASan/UBSan cover the wrapper and consumer/service/support code. The second
  engine is instrumented with ASan, with individual allocations instead of
  the target's small-object arenas. Maths remains the target objects; neither
  executable depends on host libm. Normal-exit LeakSanitizer stays enabled;
  the recorded run had permission for its thread inspection. The leaked-value
  fatal processes verify the full `0x4A534641` badge through a host hook mapping
  it to host status 99; their immediate exits are separate from leak checks.
- Upstream selection: **55 functions passed, zero failed, four skipped** in
  each host profile and the guest. The skip reasons explicitly name the absent
  `std.gc`/`os.setTimeout` host capabilities. Assertions and fixed expected
  values come from the original pinned files, including the 2,000-digit pi
  expectation. A negative control rejects a deliberately incorrect expected
  value. The manifest covers the complete selected-file driver inventory of
  59 functions, not test262 or the other upstream test files. Retained files
  and licence match the pinned archive hashes; generated drivers are separate.
- Regression checks: runtime **822 target-core / 3,014 sanitized-engine
  checks**, zero failures and zero live allocations; runner **196 fake-runtime
  / 38 real-library checks**, zero failures. The existing fatal runtime modes
  pass. Header/manifest checks, target import/export/dependency/relink audits,
  and strict root/explicit-consumer builds pass.
- Fresh QEMU VM 56209: **375 checks, zero failures**, normal process status
  **0**, and a separate leaked-value process with actual guest status
  **`0x4A534641`**. Guest coverage includes native/absent capabilities, copied
  arguments and selected output, source/job callback re-entry, recoverable
  exceptions and retained jobs, rejection/reuse, jobs and diagnostic memory
  ceilings, real invalid-handle output failure, GC marking/cyclic finalization,
  and cancellation teardown. Persistent OS allocation refusal is host-only;
  guest diagnostic exhaustion uses the runtime's memory ceiling. The quiet
  boot passed the kernel teardown test before commands; pre/post boot totals
  were 31/34 passed and zero failed. The screenshot showed the returned shell
  and intentional invariant diagnostic. Disposable images and named-pipe VM
  helpers were used, and the owned VM was stopped after extraction.

The guest consumer and three libraries returned through `/home` match the
checkout's build byte for byte:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| jsaccepttest | 121144 | `b10589a743b139340b5f8c3d278c1f0c0b9f321a09d52173c571f39ef390635b` |
| libjs.so | 4521208 | `2a19576a70357459099e2b3f87e1de30aee82cbc00f9c3783fee13afb439499f` |
| libmath.so | 150400 | `9e488810d9e45876bbb3095029e75f4c6188025ea2a17c552ce5a718299e1434` |
| libos64.so | 789240 | `fbfbf8b3978faf89507643c35acaf48b47700c29e671d304ad8c0c75c27430b1` |

This V1 validation image received the optional consumer and matching libraries
through scratch-image installation; normal image/licence delivery belongs to I1.
J2's measured stack headroom, production defaults and floating-point scheduling
acceptance remain separate. No new P5 execution was performed in this packet.

## I1 standard-image integration, 2026-10-02

Worktree `.worktrees/js-integration`, branch `codex/js-integration`, based on
reviewed and merged V1 `userland` `99df2636`. The root GNUmakefile installs
`/bin/js`, `/lib/libjs.so` and the pinned QuickJS notice on both the ext2 root
and FAT rescue volume. Existing libmath/libos64 and libos64's FreeType dependency
remain in both library inventories. Both image targets depend on the pinned
notice source and install it at `/etc/licenses/quickjs.txt`. The engine, runtime,
runner, ABI and kernel are unchanged by this packet.

- `make -j8`: **PASS**, a strict fresh kernel/userland/image build. Ignored
  toolchain dependency sources and Limine build files were copied from the
  existing checkout. `make -C userland -j8 js-acceptance-test` and the separate
  normal home-image target pass. Logs: `/tmp/js-i1-strict.log`,
  `/tmp/js-i1-consumer-build.log` and `/tmp/js-i1-home-build.log`.
- Contract/header/source-manifest and target import/export/ELF/shared-dependency/
  relink audits: **PASS**, recorded in `/tmp/js-i1-headers.log` and
  `/tmp/js-i1-target.log`. The target core retains 32 maths/18 os64 imports;
  the production library retains eleven runtime/186 engine exports and
  32 maths/22 os64 imports, with direct libmath/libos64 dependencies.
- `python3 tools/test_js_image.py`: **18 byte comparisons passed**. The runner,
  four-library recursive dependency chain and pinned notice match the build
  on the standalone ext2 image, combined-disk ext2 root and FAT rescue volume.
  Direct `js` and `libjs.so` DT_NEEDED sets match their contracts; recursive
  traversal includes libos64's FreeType dependency. The notice matches its
  manifest SHA-256. Independent scratch copies with FreeType or the QuickJS
  notice removed from FAT each cause the audit to fail with the missing path.
- Fresh BIOS/QEMU boots: **ext2 root VM 56210 and FAT root VM 56211 pass**.
  Scratch copies preserve the normal root disk's payload. Only the optional
  consumer and test inputs are added to the home image. The ext2 run uses the
  normal ISO/default entry; the FAT run uses the same kernel and disk with a
  temporary ISO configuration selecting the FAT partition GUID. Captured
  command lines and screens confirm the intended root selections. Each boot
  passes 31 pre-boot, 34 post-boot and three late checks, including the quiet
  task-teardown test before JavaScript commands.
- After both VMs stop, read-only `e2fsck -fn` on each scratch disk's ext2
  partition and home partition returns **0**, with no filesystem errors.
  Logs: `/tmp/js-i1-fsck-{root,home}.log` and
  `/tmp/js-i1-fat-fsck-{ext2,home}.log`. `git diff --check` and
  `tools/stale_refs.sh` pass.

Both boots run `husk /home/i1-checks.sh`, staged in the disposable home image:

| Input | Observed result on both roots |
| --- | --- |
| `js -e 'print(6*7)'` | stdout `42`, status 0 |
| `js /home/i1-script.js alpha -v` | copied arguments `/home/i1-script.js\|alpha\|-v`, then Promise output `42`, status 0 |
| `echo 'print(21*2)' \| js -` | stdout `42`, pipeline status 0 |
| `js -e 'throw new Error("i1-marker")'` | diagnostic contains `i1-marker`, status 1 |
| `js /home/i1-primes.js \| wc -l` | `1229` lines, pipeline status 0 |
| `/home/jsaccepttest` | 375 checks, zero failures; 55 upstream functions passed, zero failed, four explicit host-feature skips; status 0 |
| `/home/jsaccepttest --fatal-leak` | intentional engine invariant diagnostic, actual guest status `0x4A534641` (`1246971457`), shell returns |

The argument file prints `scriptArgs.join("|")` and queues
`Promise.resolve(42).then(print)`. The primes file prints each prime from 2
through 10,000 using trial division through the square root. The consumer and
selected upstream fixtures are the unchanged merged V1 suite. The run uses the
libraries already installed at `/lib` on the selected normal volume.

The runner, four libraries, notice and optional consumer copied through `/home`
match the checkout's artifacts byte for byte on both roots:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| js | 32120 | `e0db9c65f41209a5fa7ed0e5dfe5b468e9883abd2e83893f46d4178741dde7f3` |
| libjs.so | 4521216 | `ee6ee651455fb61529808c2acec4516142fb278a418c52b6d24a1cbdaefbf72d` |
| libmath.so | 150400 | `58dd1965c1c5847a2bc4fc627c72e170bc3e0693c1d813a01de5a2f44162a8c1` |
| libos64.so | 789248 | `6b5bf5858aa4518fe70130bb9b99454b08bf8a33581e6522b71268fe2d776a02` |
| libfreetype.so | 1817032 | `62031b9d2224e27735003f11b8a2625286a6d761f0c2418ac51e23ca5c20f31a` |
| quickjs.txt | 1130 | `598fd7fc928e4350abce36e337ba5a1346923c5c692f5be92c3d8e29ddd7c18d` |
| jsaccepttest | 121152 | `6d6ae0f1ddd26439c9fe1deb9f03bc837474268b189f2461834c55a4ccea6452` |

Harness inputs and scripts are at `/tmp/os64-js-i1-harness/fixtures`,
`/tmp/js-i1-guest-run.sh` and `/tmp/js-i1-fat-guest-run.sh`. Captured outputs and
statuses use `/tmp/js-i1-guest-*` and `/tmp/js-i1-fat-guest-*`; the acceptance
files are `acceptance.txt`. Quiet-boot and full kernel logs use
`/tmp/js-i1-{quiet-kernel,kernel}.log` and
`/tmp/js-i1-fat-{quiet-kernel,kernel}.log`. Screenshots
`/tmp/js-i1-guest.png` and `/tmp/js-i1-fat-guest.png` show the returned shell and
intentional invariant diagnostic. Both owned VMs were stopped after extraction.
These temporary files supplement the maintained audit and documented commands;
they are not installed in the product image.

I1's implementation and acceptance are published in
[PR #198](https://github.com/VBWizard/os64/pull/198) for independent review. No new
P5 run or full ECMAScript conformance is claimed. J2's stack measurements,
production limit defaults and floating-point scheduling acceptance remain
separate at this I1 handoff, when the runner defaults were provisional. J2's
profile and acceptance are recorded below.

## J2 standalone profile and scheduling, 2026-10-02

Worktree `.worktrees/js-acceptance-limits`, branch `codex/js-acceptance-limits`,
based on reviewed and merged I1 `userland` `1b874edd`. The maintained guest
consumer is `tools/js_measure/consumer.c`, with a register-state helper in
`fp_pause.S`, built by the optional `js-measure-test` target. It uses public
runtime/binding APIs and `/proc/self/maps`, without private runtime fields or
kernel changes. Its README specifies sampling boundaries and the run procedure.

The standalone profile is published by the header helper
`os64_js_default_limits()`: **64 MiB engine memory, 256 KiB engine stack,
4 MiB source, 60,000 ms execution and `UINT64_MAX` jobs**. The runner and
standalone API example share it; command-line overrides are retained. The
values retain C1's defaults after measurement. There is no struct/layout,
export or ABI-identifier change. Creation still requires explicit positive
limits. The default deadline bounds ordinary Promise work; `--jobs` supplies
an explicit practical count cap. Native operations remain responsible for
bounded behavior and their own stack/heap use.

Fresh final QEMU boots use **eight CPUs (VM 56213)** and **one CPU (VM 56214)**,
each passing **105 J2 checks, zero failures**, status **0**. Both use the normal
root image; only the optional consumers and test inputs are added to scratch
home images. Earlier single-CPU measurement VM 56212 passed 99 checks before
the disturbance negative control and source-ceiling workload were added; final
acceptance uses the 105-check consumer on both configurations.

The consumer identifies its usable mapped 1 MiB native stack, then samples RSP
inside callbacks while recursion reaches an engine stack exception. Four
shapes at three budgets retain at least 128 KiB sampled headroom and permit
runtime reuse. The final one-/eight-CPU stack rows agree:

| Shape | Engine budget (KiB) | Reached depth | Sampled stack use (bytes) | Sampled headroom (bytes) |
| --- | ---: | ---: | ---: | ---: |
| Simple recursion | 128 | 207 | 133296 | 915280 |
| Eight live locals | 128 | 171 | 132720 | 915856 |
| Accessor recursion | 128 | 161 | 132928 | 915648 |
| Simple plus 64 KiB native frame | 128 | 207 | 198848 | 849728 |
| Simple recursion | 256 | 417 | 264336 | 784240 |
| Eight live locals | 256 | 346 | 264320 | 784256 |
| Accessor recursion | 256 | 325 | 264128 | 784448 |
| Simple plus 64 KiB native frame | 256 | 417 | 329888 | 718688 |
| Simple recursion | 768 | 1257 | 788496 | 260080 |
| Eight live locals | 768 | 1043 | 788464 | 260112 |
| Accessor recursion | 768 | 980 | 788128 | 260448 |
| Simple plus 64 KiB native frame | 768 | 1257 | 854048 | 194528 |

This supports retaining the 256 KiB standalone profile and 768 KiB runner cap
for the measured cases. RSP sampling is not a watermark of every instruction,
and frame costs depend on source/compiler/host calls. No safety guarantee for
arbitrary native callbacks or future script-to-layout calls follows from this
table. The native-stack configurability debt remains a browser-measurement gate.

Six checked workloads use the default profile. `JS_ComputeMemoryUsage` records
engine allocation charges at explicit callbacks; these are samples, not a
global peak or process RSS, and exclude wrapper/host input allocations. The
4 MiB comment-heavy input is fixture-owned host memory, copied through the
ordinary bounded library execution path. Durations are single observations
in QEMU under the session's host load, not a comparative CPU benchmark:

| Workload | One CPU (microseconds) | Eight CPUs (microseconds) | Sampled engine charge (bytes) | Jobs |
| --- | ---: | ---: | ---: | ---: |
| Primes through 10000, count 1229 | 53315 | 52405 | 151232 | 0 |
| JSON, 10000 records retained through stringify/parse | 443116 | 560587 | 4524192 | 0 |
| 8 MiB Float64Array, filled and checked | 1314427 | 1293601 | 8547952 | 0 |
| BigInt Fibonacci, 10000 steps / 2090 decimal digits | 64558 | 63673 | 157040 | 0 |
| Promise continuation chain | 2732645 | 2724171 | 163424 | 100000 |
| Source at 4 MiB ceiling, comment plus native probe | 228573 | 577007 | 4353664 | 0 |

Retaining 64 MiB memory and a 60 s deadline leaves room around these workloads;
the 4 MiB input case establishes the chosen source ceiling. They define a
configurable standalone starting profile, not the needs of every program or
P5 hardware. Existing runtime/runner suites cover the enforcement of overridden
limits and failure classification.

The scheduling phase creates two independently owned runtimes. Their callbacks
set distinct sentinels in **all sixteen XMM registers and two occupied x87
registers**, with different x87/MXCSR rounding controls. Assembly keeps that
state live around existing yield/sleep syscalls, compares the occupied x87
values, x87 control/status/tag fields, MXCSR and XMM values, then restores the
caller's saved state. It compares neither empty x87 values nor reserved FXSAVE
bytes. An intentional XMM/x87/MXCSR disturbance is rejected with all three
error bits for controls, x87 and XMM, proving the negative control.

Both workers complete **129 pauses each, zero state mismatches**, including
Promise callbacks. Script checks retain arithmetic, signed zero/NaN, maths,
number formatting and JSON behavior under normal rounding. Atomic peer counters
observe progress while paused: one CPU records 96/95 observations, eight CPUs
113/114. Proc reports record **259/141 intervening process switches** respectively.
The single CPU forces competition for the same register hardware; eight CPUs
exercise the normal SMP configuration. This establishes the tested FXSAVE/
SSE2 scheduling profile, not AVX/XSAVE, signal-frame coverage or forced CPU
migration.

Other final acceptance:

- Strict root and explicit consumer builds pass; logs are
  `/tmp/js-j2-final-strict.log` and `/tmp/js-j2-final-consumer-build.log`.
  Contract/header checks and target import/export/ELF/shared-dependency/relink
  audits pass in `/tmp/js-j2-headers.log` and `/tmp/js-j2-target.log`. The
  library retains eleven runtime and 186 engine exports.
- Runner host suites pass **196 stand-in / 38 real-library checks**, with
  ASan/UBSan wrapper/runner code, target core/M1 maths and normal-exit leak
  inspection. `/tmp/js-j2-cli-host.log` records the run; caller sanitizer
  options were preserved and leak detection was not disabled.
- Normal-image audit passes **18 byte comparisons**, including the recursive
  library chain and pinned notice, on standalone ext2, disk ext2 and FAT.
- Each final guest also passes V1's **375 checks**, **55 upstream functions
  passed / four explicit host-feature skips**, status 0. CLI expression,
  arguments/Promise, stdin, exception/status and prime-pipeline cases pass;
  each pipeline produces **1229**. The rebuilt runner at `--stack 768K` reports
  stack overflow with status **1**. The separate invariant fixture returns
  the full guest JSFA badge **`0x4A534641`**, decimal **`1246971457`**.
- Both quiet boots pass 31 pre-boot, 34 post-boot and three late checks,
  including task teardown before the JavaScript commands. After stopping the
  owned VMs, read-only `e2fsck -fn` on each ext2 root and home partition returns
  **0**. Logs use `/tmp/js-j2-{1,8}core-fsck-{root,home}.log`.
- `git diff --check` passes. The stale-reference report is reviewed: its two
  path hits, `docs/design/pending/JAVASCRIPT.md` and `kernel/include/thread.h`,
  come from rewriting the debt row. Both files remain present and their
  retained references are valid; neither path is retired. No added superlative
  comment claims are reported.

The copied measurement/acceptance consumers, runner, four libraries and notice
match their corresponding build/source bytes on both final guests. Relevant
artifact identities:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| jsmeasure | 50064 | `064a3a61baa4483c8b55b2f6a4f95a8b38177618a43b230227a96207a12ff2a2` |
| jsaccepttest | 121184 | `4e6bb2e83b50942c583670b097ea538ec45199659d056fe561c5d08e33557188` |
| js | 32336 | `f477bfd7f8c0d80c688486e3396fdb40ea83992031eaf6a232d202294e8f30c1` |
| libjs.so | 4521296 | `de0dcd6df091a9d8d3c608c399a3c2f1e172161ff912763e81767f96952462c1` |
| libmath.so | 150408 | `1a017343bf18c539c4f4935b51407db60af676c22f4748b63dccd7d76e905094` |
| libos64.so | 789272 | `29e3447278237735c714a7050ed3dce3d51fb7fa3f1f64ef78d748323215d36a` |

Harness preparation and foreground run scripts are
`/tmp/js-j2-prepare-harness.py` and `/tmp/js-j2-final-guest-run.sh`. Raw final
measurements, statuses, V1 output, CLI output and copied binaries use
`/tmp/js-j2-final-{1,8}core-*`. Kernel logs have `quiet-kernel.log` and
`kernel.log` suffixes; screenshots `/tmp/js-j2-final-{1,8}core.png` show the
returned shell and expected invariant diagnostic. Temporary artifacts
supplement the maintained sources and documented procedure.

J2's implementation and acceptance are ready for independent review. The
accepted R2/V1 suites provide lifecycle, failure, ownership, cancellation,
Promise, date/numeric and separate fatal evidence; this packet closes the
named stack/defaults/scheduling measurement gaps. It does not claim full
ECMAScript conformance, a complete interrupt-coverage audit, arbitrary native
stack safety, browser scheduling/layout acceptance or a new P5 run.

## Combined D7/D10 callback interfaces

The additive callback-safe budget check preserves existing ABI layouts and the
standalone profile. The pre-D8 D10 target audit reports twelve runtime exports
and the unchanged 186 engine exports. Runtime suites pass 906 target-core host
checks and 3098 sanitized-core host checks, with 53 native calls and no live
allocations. D10's browser/DOM/guest geometry evidence is recorded in
[DOM_D10.md](../../docs/design/pending/DOM_D10.md).

## D8 opt-in teardown validation, 2026-10-05

Implementation on `codex/dom-d8` is validated and approved by Fable.
The additive creation/reporting APIs preserve the existing fatal default and
public ABI layouts. [TEARDOWN.md](TEARDOWN.md) records the allocator/global and
native-ownership audit; [DOM_D8.md](../../docs/design/pending/DOM_D8.md) records
commands, counts, guest hashes, limitations and the D7 integration handoff.

Maintained runtime suites pass **939 target-engine host checks** and **4,163
sanitized-engine host checks**, zero failures/live allocations, including 10,000
reclaimed leaks per profile, tracked constructor refusal sweeps and a live
peer runtime. Five separate fatal controls pass per profile. Independent V1
consumer suites retain **383 checks each**, 55 upstream functions passing and
four explicit skips. Runner suites retain **196/38 checks**. Manifest/header,
production symbol/dependency/relink and teardown object audits pass.

Final QEMU VM 56309 passes **437 checks**, zero failures, 41 native calls and
JSRT status. Across 10,000 leaked runtimes its live heap bytes and block count
are unchanged, and heap verification passes. The delivered library and fixture
match the build byte for byte. The default leak still returns the full JSFA
badge. Yonder's host fixture passes **2,080 checks**, including counted/logged
lost-wrapper reclamation and a subsequent clean page. Joined D7 validation and
new P5 acceptance remain separate gates.

## D8/D10 joined userland validation

Joining D10 with `userland` `7c5431e3` retains callback-safe budgeting and
creation-selected teardown reclamation together. The target audit verifies
fourteen runtime exports and 186 engine exports. Runtime host suites pass
1,023 target-core and 4,247 sanitized-core checks, with zero failures/live
allocations. The browser suite passes 2,128 checks with geometry, native
overrun retirement and logged lost-wrapper recovery; the DOM suite passes
4,592 checks. Header, production link and teardown allocation/global audits
and the strict image build pass. These joined results are host/target-build
evidence; the guest measurements in the preceding sections retain their
original scope.

## D11 legacy arguments validation

D11 adds a hash-checked browser legacy arguments helper (patch 0007, after
D8's 0006) without changing the standalone inherited property. On `userland`
the target audit reports 187 engine exports and nineteen runtime exports. Browser binding tests cover explicit installation,
snapshots retained beyond return, recursive innermost-frame selection and
strict/arrow refusals. Source and header audits pass. Its primary-widget
consumer proof is recorded in
[DOM_D11.md](../../../docs/design/pending/DOM_D11.md); it does not claim a new
P5 run or full legacy-engine conformance.

PR #229's patch-placement regression is covered by `tools/test_js_prepare.py`.
Its four checks cover the pinned series, a seventeen-line
shift in both engine files, and refusal of changed C/header context. The
preparer requires exact context (`-F0`) and identifies a refused patch by name.
Controls restoring the context-free patch or default fuzz fail the respective
regressions. Regenerating patch 0007 with context leaves the prepared engine
sources byte-for-byte unchanged; its manifest hash is updated.
The DOM host profiles pass 3,087 target-core and 17,739 sanitized-core checks,
with zero failures and leak detection enabled. The manifest/header and target
symbol/dependency audits pass (187 engine exports, nineteen runtime exports).

## Global-miss handler validation

Yonder's diagnostics slice adds a hash-checked browser global-miss handler
(patch 0008, after D11's 0007), `JS_SetGlobalMissHandler`. The target audit
reports 188 engine exports and nineteen runtime exports. libdom's host
covers eleven lookup forms through `os64_dom_set_global_miss`: a bare name,
`typeof` at top level and inside a function, a closure's reference that
still throws, `window.x`, `window['x']`, `globalThis.x` and an optional
call are each heard once; a symbol, an index, a found name, `in`,
`hasOwnProperty` and a descriptor query are not; with no handler, nothing
is. `'fetch' in window` reads false throughout. The DOM host profiles pass
3,120 target-core and 17,766 sanitized-core checks with zero failures.
`tools/test_js_prepare.py` pins both hunks and refuses drifted context in
the inline-field hunk. The consumer is yonder's page file
([YONDER_DIAGNOSTICS.md](../../../docs/design/pending/YONDER_DIAGNOSTICS.md)).

## Named "not a function" validation

Patch 0009, after 0008, names the method in a call's "not a function". The
libdom host covers a missing method on a plain object and on `document`, a
non-callable property, a missing method whose argument calls a real one
(named), a bare non-function call (plain), and Quinn's three (#239), each
plain: a call after a skipped optional call, a missing method whose
argument fails first, and calls after an abandoned one, bare and computed. The DOM host
profiles pass 3,121 target-core and 17,767 sanitized-core checks with zero
failures, the sanitizer watching the held atom. The export count is
unchanged.
