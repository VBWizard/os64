# F1 B1 correction — coordinator follow-up to Fable R2

Subsequent contract status: [F0-FREEZE.md](F0-FREEZE.md) records the approved
frozen baseline. The report below describes the B1 correction and its validation.

2026-09-16, Quinn. Local `opus/freetype-backend`, base `3b82356`;
uncommitted. This supplements Opus's original report without replacing its
historical evidence. It is a focused correction, not the full F1 implementation
review or a freeze/merge approval.

## What changed and why

The old allocation comment incorrectly claimed that cap refusals could not be
mistaken for callback refusals. The allocator bypassed the callback correctly,
but its error mapping discarded the reason. `port/backend.c` now records a
per-operation refusal status. Engine budget/size refusals return LIMIT; callback
NULL returns NO_MEMORY. Face opening, pair retrieval and rendering reset the
record before their potentially allocating work. Cleanup does not overwrite it.
Creation starts with a zeroed record, including its bootstrap allocation.

The regression also demonstrated that `FT_Add_Default_Modules` silently skipped
a module when allocation failed, returning an apparently usable engine. Creation
now rejects and destroys that incomplete engine, returning the recorded error.
The public header matches F0 R3 byte-for-byte; table revision 1, declarations,
structure layouts and getter name are unchanged.

The host harness measures space-glyph storage through the public table, fills
an engine cap with 512 retained glyphs, then requires LIMIT with no allocation
callback. It releases a glyph, injects callback failure and requires NO_MEMORY,
then restores the allocator and verifies recovery. It also checks cap refusal
at face opening/module initialization and callback refusal throughout creation.
The previous weak tight-cap test allowed both errors and did not prove refusal;
it was replaced. The existing allocation-denial sweep remains.

## Evidence

- [Before correction](f1-evidence/b1-before.log): 952 checks, three failures:
  partial module initialization returned OK, face-open cap returned NO_MEMORY,
  and full-cap render returned NO_MEMORY.
- [ASan/UBSan O2](f1-evidence/b1-host-O2.log) and
  [ASan/UBSan O0](f1-evidence/b1-host-O0.log): 944 checks, zero failures each.
  Commands: `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_freetype_host.py --keep`
  and the same with `-O 0`. LSan was disabled for this traced environment;
  callback accounting asserts zero live bytes and matching free sizes.
  Check counts differ because failed module creation exits denial-sweep rounds
  earlier, while the new explicit creation-status checks strengthen that path.
- [Target rebuild](f1-evidence/b1-cross.log): changed adapter compiled with
  x86_64-elf-gcc, `-O2 -Wall -Wextra -Werror`, then relinked with `--no-undefined`.
  Command: `make -C userland "$PWD/userland/bin/libfreetype.so"`.
  This was an incremental library build, not a clean whole-OS build.
- Fresh ELF inspection: one export, `os64_freetype_backend_v1`; no undefined
  dynamic symbols and no DT_NEEDED. Size: text 413599, data 13376, bss 0;
  426975 bytes total. Engine bookkeeping grows by eight bytes on this target.
- `python3 tools/import_freetype.py --verify`: 334 files match, 11 patches
  unchanged. No imported source or patch changed in this correction.
- `git diff --check` and `tools/stale_refs.sh` passed for tracked differences;
  the adapter/harness/header additions were read separately. Evidence-directory
  `.gitignore` now includes its log files in normal Git discovery.

No QEMU or P5 rerun was performed for B1. Opus's preserved guest evidence is for
the preceding candidate; it does not establish guest validation of these edits.

## Follow-up

The full review is recorded in [F1-IMPLEMENTATION-REVIEW.md](F1-IMPLEMENTATION-REVIEW.md),
including the checks listed below and fresh QEMU evidence. This report and its
logs retain their original B1 scope.

## Remaining implementation review at B1

Review the complete F1 port separately, including this correction. Fable's
additional checks remain: bare CFF2 refusal without an SFNT directory, a target
runtime regression for losing `-fno-tree-loop-distribute-patterns`, and malformed/
truncated/duplicate metadata-name fixtures. The optional four-em mask bound was
not adopted. Backend design approval remains distinct from implementation
acceptance and a recorded freeze commit.
