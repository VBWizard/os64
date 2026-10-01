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
  LeakSanitizer requires execution outside this sandbox to inspect threads;
  it remains enabled for the string fixture. The existing heap death harness
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
process because sandbox background processes do not survive its completion.
The VM was stopped after capture. `make` builds the boot/root image; the separate
`disk/os64_data.img` target created a fresh home disk for this validation.

The library prerequisite code is isolated in commits `f7f3c8c1` and `d15828f1` on
`codex/js-support`; the interface and source-preparation work is stacked above
it on `codex/js-runtime-foundation`. The main checkout's concurrent work is
preserved separately.
R0 behaviour, target QuickJS C adaptation, maths conformance, runtime limits,
unhandled rejections, cancellation, leaked-value fatal handling, and browser
integration are not validated by these foundation checks.
