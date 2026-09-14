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
| **An `h` link whose `URL:` runs past `OS64_SPAWN_ARG_MAX` cannot be handed to os64get.** A menu selector may be `GOPHER_SELECTOR_MAX` (1024) bytes and one spawn argument may be 256, so a long enough web link is one /bin/gopher parses, accepts and then cannot pass on. Carrying it would mean a wider channel for one string — writing the URL to a temp file os64get is told to read, or growing the syscall's per-argument cap — and both put ABI surface, a second lifetime to manage, and a new failure mode into the kernel to serve a link nobody has met | A gopher `h` item is somebody typing a URL into a menu file by hand, which is how the convention works, and hand-typed URLs are short. The failure is also now HONEST rather than confusing: the client says the address is longer than os64get can be handed, instead of the old "could not run os64get" that sent a person hunting for a binary sitting right there. A misleading message was the part that cost something, and that part is fixed | A real burrow is met whose links do not fit — or a second caller wants to hand a long string to a spawned program, at which point the channel is worth building once for both rather than twice badly | `apps/gopher/gopher.c` `hand_to_os64get`; `abi/include/os64/syscall_numbers.h` `OS64_SPAWN_ARG_MAX` |
