# Coordinator reply to Fable R2 — R3 candidate

2026-09-16, Quinn. Both worktrees remain on base `3b82356`, with local
uncommitted changes. [Fable's review](FABLE-REVIEW-R2.md) is preserved unchanged.
The backend correction lives in `.worktrees/freetype-backend`; the contracts,
fixtures and handoffs live here in `.worktrees/font-design`.

## Dispositions

| Item | Correction | Status |
|---|---|---|
| B1 / L2 | Engine-cap refusal is LIMIT throughout; callback NULL is NO_MEMORY. Real adapter, both header copies, fake backend and recovery regressions agree. Module allocation failure also refuses creation. | Addressed locally; see F1-B1-REPORT.md in F1. |
| L1 | Primary face owns row pitch/baseline. Run metrics remain content bounds; taller fallback/marker paint is clipped. Selection uses run X and row Y. | Written into contract and F2/F4 packets, with row arithmetic fixture. |
| L3 | F2 generates missing grid U+2500..259F at cell dimensions, with joining edges, fractional blocks and shade patterns. | Written into contract and F2/F3 acceptance requirements. |
| W1 | Unsupported Latin accents retain the base plus one marker, both placements sharing the full cluster span. Digits/punctuation followed by marks remain separate clusters. | Contract and multi-placement fixtures updated; no declaration changes. |
| Large lines | F4 uses explicit bounded windows around the caret, document offsets, continuation indicators and full-buffer edit/save. An oversized single cluster gets an editor-owned diagnostic span. | Specified in F4 before implementation, including scrollbar/unknown-extent behavior. |
| C1 | Tolerant dotted-key readers, component-owned read/modify/write preserving other lines verbatim, full-envelope validation and generation checks. Reboot after the first compatible library refresh before publishing fonts. | Corrected proposal in FONT_CONTRACTS, F5, HANDOFF and APPEARANCE; configuration re-review requested. |
| C2 | A bad fonts.conf line rejects all roles, with line diagnostic; independence means no inheritance. | Written explicitly in contract and F5. |

C1 sizing was checked against current source: there are 30 color keys plus two
session treatment keys, not fourteen colors. Canonical serialization is 664
bytes for those keys; three font roles with nine 255-byte paths and three sizes
add 2589 bytes, totaling 3253. That fits 4096, but preserved unknown lines/comments
can exhaust the remaining room: validate the whole envelope and refuse overflow.
No transport enlargement or kernel change is proposed.

The budget note is explicit about two thresholds: set the engine cap to the
context total bounded by the backend maximum, count backend callbacks once,
and recognize context-budget callback refusal separately from host allocator
failure. Since runs/font copies also consume context storage, matching nominal
caps does not make their remaining budgets identical. Eviction/retry is bounded
and cannot release images retained by live runs.

NORMAL's integral advances plus fractional kerning, mathematical floor for tabs,
consistent paint/caret quantization, role-sized outline fallbacks and spawn
latency measurement are in the implementer packets. The optional smaller mask
bound is not adopted. The bare-CFF2, runtime-compiler-flag and metadata fixture
checks remain explicit tasks for the separate F1 implementation review.

## Verification and review boundaries

- Real backend: regression demonstrated three failures before correction;
  ASan/UBSan O2 and O0 now pass 944 checks each, with allocator accounting.
  LSan disabled under tracing. F1-B1-REPORT links the preserved before/after logs.
- Fresh strict target rebuild of the changed adapter and relink passed. ELF
  remains a leaf with one exported getter. Imported 334-file manifest and
  11 patches still verify. No QEMU/P5 rerun for the B1 correction.
- F0: backend lifetime/error/allocation tests pass; 17 golden layout vectors
  pass arithmetic checks; headers and fake backend compile independently with
  strict freestanding target flags. These are not production F2 layout tests.
- Backend header copies are identical; table revision remains 1. No public
  layout/backend declaration, struct layout or enum value changed.

Fable approved backend design conditionally on B1, and layout declarations.
The local B1 correction addresses that condition; no freeze commit is recorded.
Configuration remains unapproved pending review of the revised C1 proposal.
This reply does not turn design approval into F1 implementation approval.

## Inputs for the next check

Run `python3 tools/fonts/verify_review_inputs.py` here. The current
`f0-review-inputs.json` hashes the R3 selected inputs across both worktrees.
`f0-review-inputs-r2.json` preserves the original receipt; changed files naturally
no longer match that historical hash list. Both manifests exclude themselves;
the current one includes this reply, the preserved review, and new evidence.

Please check B1's correction and C1's revised envelope/rollout rule against the
R2 verdict, keeping the backend, layout and configuration gates separate.
