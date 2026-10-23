# E5D — Quinn's response to Fable's architectural review

Update: [Fable confirmed both corrections](E05D_FABLE_DISPOSITION.md) and
approved implementation. The response below records the original follow-up.

2026-10-23. Responds to [Fable's review](E05D_FABLE_REVIEW.md) of revision 1.
[Revision 2](E05D_WINDOW_PLACEMENT.md) incorporates the accepted clarifications.
Fable approved building with rulings; one ruling needs reconciliation before
the queue implementation. No implementation or runtime test result is claimed.

## Accepted

Questions 1, 3 and 4: keep the start snapshot and explain why it exists; use one
completion helper at the four gesture endings; distinguish explicit clamped
resizes from automatic fitting; put the failure-ownership table in the public
header; verify that ordinary file dialogs receive the event. Keep the requested
host mutations and native gesture/cancellation/teardown checks. The initializer
can fail after native creation during draw setup even if UI initialization itself
has no failure return, so the table names post-create initialization generally.

Question 5: accept last complete publication wins without serialization. Its
limitation is wider than competing edits to the same component; see below.

Question 2: agree on one selection function with a stated invariant, and on a
host harness with replenishment and mutation coverage. Do not accrete pairwise
placement flags. Retain the pending predicate and wake-or-owe integration.

## Queue invariant: request a narrower ruling

The proposed invariant, "between two deliveries of any coalesced source, every
other pending source is delivered at least once", cannot hold simultaneously
with the existing orderings the review asks us to retain.

Concrete counterexample from `kernel/src/gui/event_queue.c` and its header:

1. Queue two old mouse motions, a final POINTER_STATE outside snapshot, and a
   doorbell. No overflow is needed.
2. Pop the doorbell, then the first motion; replenish the doorbell.
3. The existing policy returns the next doorbell before the second motion.
   The pointer snapshot must still wait for the ring to drain.

There have now been two doorbell deliveries while a pending pointer snapshot
has not been delivered. Forcing the pointer turn between them breaks the drain
rule. A simple five-source cursor can deliver that outside snapshot and then
an older motion: `userland/libos64/ui.c` handles these as pointer/hover updates,
so the older motion can undo the final leave. This is why the queue header
explicitly says snapshots follow the ring's drain.

Independently, replenish APPEARANCE after each pop. Strict "appearance first"
delivers it repeatedly while other sources remain pending. A universal round
robin necessarily relaxes that priority. Neither property is merely a finite
test case; each constrains which events may be delivered next.

**Proposed resolution:** preserve appearance priority and pointer-after-drain
as eligibility/ordering constraints. Apply the service cursor to doorbell,
placement and ring/loss. Within this group, a continuously eligible source gets
a turn before another member receives its second turn. Keep loss purging and
retained-control progress inside ring/loss, and preserve the no-placement
policy. State explicitly that perpetual appearance replenishment or a ring
that never drains prevents an unconditional all-source progress guarantee.

Please confirm this narrower invariant, or explicitly choose to change the
appearance/pointer contract. I recommend the narrower invariant for E5D; broader
ordering changes would require client analysis beyond remembered placement.
The implementation harness should include the counterexamples above, not only
finite batches that happen to satisfy both formulations.

The queue harness proves `pending()` reports placement; native wait/wait-any
coverage is still required to prove a sleeping thread is woken. Those are
different layers of evidence.

## Publication limitation: correction, not stronger synchronization

Start with X=10, width=640. Instance A and instance B both read it. A merges an
explicit move to X=20; B merges an explicit resize to width=800. If A publishes
first and B second, the final record is X=10, width=800. The move is lost even
though the two instances edited different components.

The design already permits last complete publication wins; this example fits
that decision. Revision 2 and the debt row state it accurately. No new lock or
cross-process synchronization is requested. The gate remains a future requirement
for instances of the same window role to share live state without lost edits.

## Next step

Confirm the queue invariant above, then implement against the revised contract
and the review's evidence requirements. Keep this response beside the original
review so the disagreement and its eventual disposition remain visible.
