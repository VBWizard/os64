# Canvas integration: review disposition and S3 merge

Response to [Fable's architectural review](CANVAS_INTEGRATION_FABLE.md),
2026-11-03. Fable reviewed `2c2a7518` over `8faadcf5` and recommended merging;
the performance findings were not merge gates. The review is preserved as
written. This document records the dispositions and subsequent integration.

## Dispositions

1. **Accept: web-font dependency invalidation is too broad.** Booked in
   DEBTS under Browser layout. Compare actual document character dependencies
   before invalidating geometry reuse; preserve independent font/resource,
   stylesheet and base-URL invalidation. A hash alone must not authorize
   reuse after a collision. Acceptance includes unchanged-character and
   changed-selection tests plus a P5 web-font graft measurement.
2. **Accept: text draws allocate maximum scratch.** Booked in DEBTS. The
   229,376-byte reservation per draw is real; its runtime impact is not yet
   measured. Prefer bounded reuse or demand-based growth over a guessed
   per-glyph allowance. Ledger charges, failure atomicity and the geometry
   command cap remain requirements.
3. **Accept: gradient getters walk retained wrappers.** Booked in DEBTS.
   A direct association needs explicit ownership; a native opaque slot or
   DOM-owned lookup is an implementation choice, not settled by this review.
   Identity through save/restore and cross-context use must be preserved.
4. **Do not change cancellation pressure.** [Pointer Events Level 3,
   section 4.2.7](https://www.w3.org/TR/pointerevents3/#the-pointercancel-event)
   requires cancellation pressure to match the last dispatched pointer
   event. The zero-pressure rule applies to pointerup. The existing special
   case is intentional; a source comment and regression assertions make the
   distinction explicit. Tests observe captured down/move/up pressure and
   down/move/cancel pressure, then verify the physical release tail is drained.
5. **Remove the unused query.** Neither production nor tests call the
   boolean dirty query; its Yonder wrapper is unused too. Remove both and
   their declarations. Presentation continues through the damage visitor
   and paint acknowledgement, which retain pending reset semantics.
6. **Test the combined build.** Chris tested S2 and S3 on userland and
   accepted the combined Canvas/S3 cohort at `94c1c55d` on 2026-11-04.
   The checked streaming and Canvas pages worked correctly. This was
   functional browsing acceptance, not another timed performance campaign.

## Merge before cleanup

At Chris's request, userland through `afad35b5` (Streams S3, PR #459) was
merged first as `43b60349`; the review cleanup follows that merge.

The overlapping fetch paths retain both contracts: script responses may
stream through a pipe, while native image requests read complete bounded
bodies and preserve the origin verdict for Canvas tainting. Opaque script
bodies remain unavailable. Native images do not enter the script pipe.

The integration's unsafe-method cache invalidation already runs per hop,
before redirect rewriting or body publication. It therefore subsumes S3's
final-response invalidation before streaming, including a body that later
fails. Keep that earlier invalidation and both sets of cache tests; remove
the redundant streaming-only call and its request bookkeeping.

The window loop and WPT runner service streamed bodies and Canvas animation.
Body wakeups retain their own bell; Canvas retains its frame/idle follow-up
and paint acknowledgement. This preserves the different reasons each can
need another turn.

## Verification and handoff

Verification results and the final frozen cohort are recorded in
[integration regression](../../canvas-evidence/integration-regression.md).
Chris accepted the combined S3/Canvas candidate on 2026-11-04. Final Codex
review of the merge and follow-ups remains the step before merging to userland.
