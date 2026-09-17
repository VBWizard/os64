# F2 implementation map

Branch `codex/text-layout`, isolated worktree `.worktrees/text-layout`.
Prerequisites: frozen F0 `23bf6dddfd1077bf844c661d8762a9b52e3a68f9`, receipt
`0ab3cee`, reviewed F1 backend `f065c7c`; combined base `7c9e2d2`.
F1's uncommitted-review descriptions are historical: its reviewed candidate
was checkpointed locally to give F2 an exact dependency. No push is implied.

F2 implements the existing text.h/text_draw.h declarations. This document is the
source map; [F2-REPORT.md](F2-REPORT.md) records validation and the outstanding
independent review.

| Component | Responsibility |
|---|---|
| `text_cache.c` | Context budget, backend callbacks, copied font bytes, unique instance identities, reference counts and glyph LRU |
| `text_decode.c` | Explicit encodings, strict UTF-8 through the existing decoder, W1 cluster boundaries and display substitutions |
| `text_w1_data.h` | Generated finite Latin/composition and box-stroke tables |
| `text_bitmap.c` | Existing bitmap charset mappings and scalable box/block/shade masks |
| `text.c` | Resolved positioned runs, common metrics, caret/selection/hit/fit operations |
| `text_draw.c` | Allocation-free retained-mask painting, clipping, grid clipping, source-over blend and translation preflight |
| `text_internal.h` | Private object layout and internal seams; internal functions have hidden ELF visibility |
| `tools/test_text_host.py`, `.c` | Actual implementation against frozen vectors, failure/budget/lifetime/profile/drawing tests; optional real FreeType integration |
| `userland/tests/texttest` | Public-API guest specimen and lifetime/cleanup checks |
| `userland/tests/textspawn` | Non-GUI spawn/exit/reap batches for dependency measurement |

The cache is per context. A font instance's identity binds its copied source,
pixel size and hint mode immutably; glyph IDs cannot collide across opens or
configuration generations. Synthetic grid masks additionally key on cell width,
height and baseline offset. Runs pin images and retain their font instances.
Closing the caller's font reference does not invalidate a run. Releasing the
last font reference discards its unpinned entries and closes its backend face.

The total budget includes context/allocator bookkeeping, font copies, backend
allocations, cache records and runs. Backend requested bytes pass through the
context allocator once; engine statistics are not added again. The cache target
counts records and mask coverage; opaque backend bookkeeping/face scratch stays
in the total budget. A render's net allocation growth cannot stand for evictable
mask cost because it can also grow persistent face scratch storage. Pinned masks
can exceed the target; the total cap remains binding. Retry after a cap refusal
requires actual eviction and occurs at most once for that operation. Allocator
failure stays NO_MEMORY and does not start a retry loop.

The backend table is explicit in context options. No new public getter/default
resolver has been invented; F5 owns role resolution. Bitmap entries preserve
legacy byte charset lookup. Their internal glyph IDs include an encoding tag;
procedural grid masks use diagnostic glyph ID 0 with the primary font identity.
The production build records `libos64 -> libfreetype`; the leaf has no reverse
edge or undefined imports. The dependency experiment and its timing limitations
are in the report. These IDs are not arguments for the outline backend. Rendering consumes the
retained image, never reconstructs it from a placement ID. Missing/control
markers retain the specified zero font identity and zero glyph ID.

Runs are source snapshots, without a second run cache. This keeps source, font
choices, positions and image retention in one immutable object. Callers retain a
run for repeated measurement/paint; releasing it releases that snapshot. F4 owns
editor row/window caches and does not need to duplicate glyph storage.

Unicode reference inputs are retained under `tools/fonts/unicode-17.0.0` with
SHA256 pins and their license. `generate_w1.py --check` reproduces 161 canonical
ASCII-base compositions, 190 Latin letters in U+00C0..017F and all 128 box-stroke
descriptions. Block/quadrant/shade geometry is procedural. The Unicode notice
also rides the FAT/ext2 images beside the engine notice.

F3/F4/F5 remain separate packages. No terminal, editor, persistence protocol,
loader or kernel interface is changed here. Validation results and remaining
review work are recorded in [F2-REPORT.md](F2-REPORT.md).
