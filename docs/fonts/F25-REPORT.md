# F2.5 — Provider and consumer handoff foundation

2026-09-17, Quinn. Worktree `.worktrees/font-provider`, branch
`codex/font-provider`, parent `4b0a839b6f2c0c6baea3b1eb060651e7f636fe46`.
This is the shared foundation for parallel F3 and F4 implementation. The separate
Opus handoff receipt records its exact committed branch point.

## Delivered boundary

[FONT_PROVIDER.md](../../FONT_PROVIDER.md) fixes the ownership and replacement
contract. The implemented public entry points in `os64/font_provider.h` and
`os64/font_adopt.h` provide:

- Context creation with production FreeType by default or an injected backend.
  The production getter is called inside libos64; applications do not need a
  direct FreeType dependency. F2's original mandatory-backend API is unchanged.
- Reference-counted, immutable three-role sets prepared from caller-supplied
  bytes, with ordered fallback, implicit builtin fallback and primary metrics.
  The checked preparation form identifies the failed role/source for F5's
  configuration-line diagnostic. Context-wide failures remain distinguishable.
- Shared terminal validation against metadata and printable ASCII coverage,
  equal advances and positive integral cell width. The provider supplies stable
  primary row geometry and does not use fallback height to expand rows.
- An adoption coordinator: prepare consumers, attempt one optional final
  fallible barrier, then commit without allocation. Failure aborts staged plans
  in reverse order. This permits a PTY resize without mutating active UI state
  before all other preparation succeeds.

Sets and preparation allocations are charged to the F2 context; no duplicate
rasterizer/cache/loader or private allocation budget was introduced. F5 owns
file/config resolution, publication and generation policy. F3/F4 own their
window-specific preparation and adoption callbacks. Their production adapters,
actual PTY changes and editor behavior are not implemented by this foundation.

The F0 headers and production F1/F2 implementation files remain unchanged.
The work plan now records Fable's already-committed F2 acceptance instead of
saying its review is pending. The old F2 input manifest describes candidate
`32d8243`; it is not a hash manifest of these later documentation changes.

## Validation

- Real-engine host suite passes at **-O2 and -O0 with ASan+UBSan**. It exercises
  builtin/default roles, copied source buffers, independent roles, retained runs
  after set release, identity changes, implicit fallback, primary row rounding,
  proportional/incomplete-ASCII/unequal/fractional terminal rejection, diagnostics,
  transaction order, reverse abort, barrier refusal and multiple-barrier rejection.
- Exhaustive allocator denial covers **672 allocation positions** in a full
  candidate preparation while an old set remains usable. Each point checks
  failure cleanup and final zero live bytes. The harness's large raw check count
  includes allocation-ledger assertions inside this sweep; it is not that many
  independent scenarios. LeakSanitizer is disabled for this execution environment;
  sized frees and zero-live accounting supplement ASan/UBSan.
- Real DejaVu Sans Mono (TrueType) and Source Code Pro (CFF) validate at 12 and
  28 nominal pixels. Their measured cells are respectively 7x15 / 17x33 and
  7x16 / 17x36. Nominal font size is not row pitch.
- The existing fake-backend F2 suite passes **7,161 checks** unchanged. F2's
  earlier real-font evidence remains attached to its accepted implementation.
- A complete default-strict **userland build** passes. ELF checks show
  `fontsettest -> libos64 -> libfreetype`; libos64 now imports the backend getter.
  The leaf retains no reverse dependency and exports its single getter.
- A fresh QEMU text boot runs the public API guest test against the built
  library and real fonts. Three roles are prepared at 12 and 28 pixels, input
  buffers are freed, a refusal fixture retains the old set, successful adoption
  transfers ownership, and final destruction reports zero live bytes. The guest
  screenshot and its output were inspected. This tests the callback protocol;
  its refusal fixture is not evidence of an actual PTY resize or editor relayout.
- The guest's installed ext2 library/test bytes match the build. Both ext2 root
  and home pass offline `e2fsck -fn` after shutdown. The private test images use
  the accepted F2 kernel/ISO; no new kernel build or kernel runtime behavior is
  claimed. P5, full F3/F4 interaction and F5 Apply/Save remain untested here.

Commands from the worktree root:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_font_provider_host.py --real --output /tmp/os64-f25-real-o2
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_font_provider_host.py --real -O 0 --output /tmp/os64-f25-real-o0
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py --output /tmp/os64-f25-f2-regression
make -C userland -j4
```

Receipts and guest reproduction details: [f25-evidence](f25-evidence/README.md).

## Coordination status

This foundation is ready for F3/F4 development against the pinned interface.
It has coordinator verification, not a new independent Fable approval or merge
approval. Consumer implementation and review remain separate. If review exposes
a contract change, coordinate it across both consumer branches rather than
silently changing a header underneath one implementer.

F4 may extend its UI APIs to construct the generic consumer descriptor and stage
application layouts. It must retain one owner for textfields, textviews and
Scribe. F3 uses the same provider/adoption boundary for gterm. F5 supplies resolved
sets and publication metadata later. No competing provider or persistence schema
is needed to start either consumer package.
