# E__ — Slice title

Copy this template when a slice starts. Replace prompts with facts; use “not
run,” “unassigned” or “not decided” where appropriate. Remove inapplicable
sections with a brief reason. Link the resulting record from the slice board.

## Status and ownership

- State / updated date:
- Developer / reviewer:
- Branch / worktree / base commit / publication state:
- Prerequisite slices and commits actually present:
- Design / PRs / implementation commits / merge commit:
- Fable architectural review applicability / rationale (D012):
- If required, reviewed design revision / findings / disposition:

## Outcome and scope

Describe the user-visible outcome, included work and deferred work. Name the
shared components and their concrete consumers. Link governing umbrella
sections and decision IDs; record new decisions in the decision log.

## Design and interfaces

For small work, put the design here. For larger work, link the detailed design
and summarize its contract: ownership/lifetime, public interfaces, limits,
threading, error behavior, compatibility and relevant interaction examples.
List shared files/ABIs that need coordination with another active slice.

## Work checklist

- [ ] Settle the slice contract and relevant customer interaction choices.
- [ ] Implement the scoped change and its shared-consumer integration.
- [ ] Complete the acceptance checks below and record evidence.
- [ ] Resolve review findings; record commit and PR links.
- [ ] Merge and record deployment/customer acceptance where applicable.
- [ ] Update board, decisions, session log and latest handoff.

Split these into concrete tasks suited to the slice. A checked task needs
evidence where appropriate; a checklist is not a substitute for the evidence.

## Acceptance and evidence

| Check / expected behavior | Environment and tested commit | Command or interaction | Result / evidence |
| --- | --- | --- | --- |
| Replace with a meaningful acceptance condition | Not run | — | — |

Distinguish host, QEMU and P5. Record exact commands and meaningful failure
injection where used. Preserve ASan/UBSan and supported leak checks. Link durable
logs or give enough detail to reproduce; record reasons for missing checks.
For P5, name the installed revision and the behavior the customer confirmed.

## Review findings and remaining work

| Finding / source | Severity | Status | Fix commit / verification |
| --- | --- | --- | --- |
| None recorded | — | — | — |

“None recorded” does not mean reviewed. Track unresolved findings and product
limitations explicitly. When deferring work, link the owning slice or debt record.

## Handoff

- Last completed action:
- Working-tree state / uncommitted changes:
- Blocker or open choice, with who/what can resolve it:
- Exact next action:
- What another contributor must know before editing:

Update the project-level handoff to point here; avoid conflicting copies of
the detailed state.
