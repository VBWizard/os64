# DECLINED.md — true, understood, and not being fixed

DEBTS.md is the ledger of work that WILL be done: every row there names a gate,
and when the gate fires the debt is paid. This file is the other half. A row
here is a finding that is **correct** — the analysis held, the failure is real
and reachable in principle — and that os64 is choosing to live with anyway.

**The bar is a comparison, not a severity.** A finding is declined when the
FIX costs more than the BUG, counting the fix's own risk: new code in a path
nobody walks is new code that can be wrong, and a ledger entry is cheaper than
a mistake in a place nothing tests. So the question is never "how bad would
this be" alone — it is "how bad, times how often, against what the cure costs
and what the cure might break."

That comparison has a shape worth writing down, because it caught the authors
of this file out once already:

- **A cheap fix is not a candidate.** If the change is a handful of lines in a
  path that already exists, FIX IT. Writing the row costs more prose than the
  patch costs code, and the row then has to stay true forever. Most findings
  that *feel* like edge cases fail here — they are rare, and they are also
  four lines.
- **RARE IS NOT THE SAME AS ADVERSARIAL.** os64 has three users, so a bug in
  ordinary use is unlikely to be MET. But a program that dials strangers'
  machines — gopher, os64get, the resolver, telnet — takes input chosen by
  somebody else, and the odds that input is hostile do not fall because the
  user count is small. One careless server is enough, and it does not have to
  know you exist. A finding whose trigger is "what a remote peer sends" is
  judged on blast radius, not on population.
- **Say what would reverse it.** Every row names the thing that would make
  this worth paying. A declination with no reversing condition is a decision
  nobody can revisit, which is how a ledger becomes a graveyard.

Rows leave this file in both directions: a reversing condition that fires
moves the row to DEBTS.md (or straight to a patch), and a row that turns out
to be WRONG — the analysis did not hold — is deleted, not amended.

Not to be confused with DEBTS.md's "explicitly NOT debts" list, which is a
third thing again: those are ratified DESIGN CHOICES that were mistaken for
defects. A row there is not a bug anybody is living with; it is the system
working as ruled.

| Declined | Why it is not worth paying | What would reverse it | Source |
|---|---|---|---|
| **Kept bytes bound to their record by length.** The replay runner takes a kept page's `<name>.html` as the page when its length is the record's `bytes:`, and its index when its lines are the record's `kept lines:` (docs/design/pending/REPLAY.md § 3), so bytes of the same length swapped in under a record are replayed as that page. | The possibility is a copy from the P5 interrupted between a record and its bytes, with the two from different sessions AND the stale bytes exactly the same length; the corpus is the person's own, written by one yonder under the claim's exclusive open and copied whole. The severity is one page's verdict wrong, named by its address in the diff. The cure is a content digest written by yonder and checked by both readers, a new index version, the vectors and mutants for each check: a format change in every reader to guard against an accident the corpus's own handling does not produce. | A corpus merged from more than one machine, or shared beyond its author (someone else's kept pages replayed here). | Codex, PR #326 round 7 (`tools/test_yonder_replay_host.c`, the length check); declined by Fable on Chris's challenge, 2026-10-22 |
