# F0 freeze record

Frozen contract commit: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (`23bf6dd`).
Branch: `codex/font-design`. Base: `3b82356413ab8f183bd6506febe8d06fea6e7de0`.
Approval: [Fable R3](FABLE-REVIEW-R3.md), all three design gates approved.
This receipt records the existing commit; it does not change the frozen interfaces.

| Contract | Frozen baseline |
|---|---|
| Backend | Table revision 1, `os64_freetype_backend_v1`, font_backend.h |
| Layout/drawing | text.h, text_draw.h and FONT_CONTRACTS.md's R3 semantics |
| Configuration | R3 fonts.conf and appearance-envelope contract, including reboot rollout |
| Fixtures | tools/fonts, fixture schema revision 2, 17 arithmetic vectors |

Use the full commit above in each downstream assignment. Changes to declarations,
semantics or fixtures require a coordinated contract revision and updated packet.
The freeze commit contains the approved contract; this follow-up supplies its
literal identity and updates status prose that necessarily preceded that identity.

## F1 correction alongside the freeze

F1 remains a local candidate in `opus/freetype-backend`, separate from this commit.
Its B1 correction and evidence are described in `docs/fonts/F1-B1-REPORT.md` in
that worktree. Fable's R3 review confirms B1; the freeze input manifest records
the relevant source/evidence hashes. The backend header copies are identical.
Nothing in this freeze accepts the complete F1 implementation or commits its
upstream import. F1 receives a local copy of this receipt for its next review.

## Checks and remaining gates

Fresh F0 verification passed on 2026-09-16:

- ASan/UBSan backend lifetime, geometry, error and allocation tests;
- 17 layout-vector arithmetic checks;
- strict freestanding target compilation of headers and fake backend;
- preprocessing before/after header status edits: identical declarations;
- identical F0/F1 backend header copies, selected-input hashes, staged whitespace
  checks and stale-reference scan.

Command: `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_font_contracts.py --output
/tmp/os64-font-contracts-freeze`. LSan is disabled under tracing; allocation
accounting checks cleanup. These are contract-fixture checks, not production
layout tests. This turn did not rerun F1 tests, QEMU or P5.

Next: complete F1 implementation review, including B1, bare CFF2 refusal,
the target compiler-loop-pattern regression, malformed/truncated/duplicate
metadata names, and a fresh QEMU run. F2 owns spawn-latency measurement before
integrating the production dependency. F5 owns the preserving appearance
protocol and reboot-rollout tests. Those implementation gates remain open.

The R2 and R3 reviews and input manifests are historical receipts. The current
input manifest tracks selected files after recording the freeze identity;
`git show 23bf6dd:docs/fonts/f0-review-inputs.json` retrieves the freeze snapshot.
No push, PR or merge is part of this local freeze.
