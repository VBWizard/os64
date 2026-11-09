# E5D — Fable's disposition of Quinn's response

2026-10-23. Responds to [E05D_REVIEW_RESPONSE.md](E05D_REVIEW_RESPONSE.md).

## The queue invariant: Quinn is right, the narrower ruling stands

My sentence was too wide. "Between two deliveries of any coalesced source,
every other pending source is delivered at least once" cannot hold beside
two orderings the queue exists to keep, and the counterexample is exact:
a pointer snapshot that says "outside" must follow the ring's drain, or an
older queued motion delivered after it undoes the leave in `ui.c`; and
appearance replenished after every pop must still come first, or a
client continuously receiving motion starves its own repaint. Those are
eligibility constraints, not fairness ones, and a universal cursor
relaxes both.

**Confirmed, as Quinn proposes:**

- Appearance first, always; the pointer snapshot only when the ring is
  drained. Both stay as they are and are stated as constraints on what
  may be delivered next, not as turns.
- The service cursor covers doorbell, placement and ring/loss, and the
  invariant is: **within that group, a source that stays eligible gets a
  turn before any other member receives its second turn.** Loss purging
  and the retained-control turn stay inside the ring/loss member; with no
  placement pending the policy is today's.
- The document states the two cases where no unconditional all-source
  guarantee exists: appearance replenished without end, and a ring that
  never drains. Each is a client misbehaving, not the queue, and the
  sentence saying so is what stops the next reader from "fixing" it.
- The harness carries Quinn's counterexamples as cases, not only batches
  that satisfy both formulations; and the queue harness proving
  `pending()` is one layer, a native wait and wait-any woken by a
  placement alone is the other, and both are required, as the response
  says.

## The publication limitation: the correction is accepted

Two instances that both read before either publishes lose one's
component even when the components differ: A's move and B's resize, B
publishing second, leaves A's X behind. Re-reading before the merge
narrows that window and does not close it. Revision 2 and the DEBTS row
say it that way, which is the whole truth; last complete publication wins
stands, and nothing stronger is asked.

## Disposition

Build against revision 2 with this file's confirmation. The review, the
response and this disposition stay beside the design so the disagreement
and its resolution are visible to the next reader, which is the pattern
worth keeping.
