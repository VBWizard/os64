# PILE3_TIMELINE_SWAP.md — lane B: the timeline, built and swapped

Status: brief, written 2026-10-14 by Opus (S8a's builder) at Chris's
request, after #265 (S8a, CSS animations) merged as `89fb6465`. One PR. It
runs beside lane A ([PILE3_CLAMPS_SCHEDULE.md](PILE3_CLAMPS_SCHEDULE.md)),
and this file ships with it. Once the PR is built, its as-built text goes
in PILE3.md and this brief is struck.

## Why this lane exists

A layout today changes the page's timeline **in place** as it goes. When
the layout fails, or the face discards its tree, the layout has to undo
what it did. That transaction was patched in rounds 3, 10 and 12 of #265,
and each patch added state:
- `FRecordState now`/`was`;
- `FRecord.made`, `.after` and `.gone`;
- `flow_timeline.base` and `.undoable`;
- `f_timeline_undo` and `flow_timeline_undo_incomplete`;
- the OOM path in `f_timeline_end` that marks unmet records gone.

**The ruling (Chris, 10/14): build and swap.** A layout READS the shown
timeline and builds a fresh record set beside it. It inherits each
record's start and hold by the same name matching used today, and
carries the set with its tree. The face COMMITS the set when it shows
that tree. Failing or discarding means freeing the set. There is nothing
to undo, and every item in the list above goes.

**Order.** Four simplifications come before S8b (animations in place):
the clamps and the scheduler in lane A, this swap, and sparse keyframes
last. After S8b come S9 (web fonts), then the perf slice, then
transitions with `:hover`, which PILE3.md books ("as long as we don't
forget").

PILE3.md § Animations, both slices, Booked has the row. No contract text
changes; libflow's API gains a commit. The PR should come out
**net-negative in lines**. If it grows, stop and say why before going on.
The cost should be about even: a fresh record per animation per layout
against today's undo bookkeeping, both noise next to a layout.

## Read first

1. CLAUDE.md, especially the opening comment rules and how a review round
   is run. Then SUCCESSION.md.
2. PILE3.md § "The animation model (slice S8a)", and "Animations, both
   slices": its Contracts, Booked and Proof (the timeline paragraph).
3. The code:
   - `userland/libflow/motion.c` § The timeline: `FRecordState`,
     `FRecord`, `struct flow_timeline`, `f_timeline_begin`,
     `f_timeline_element` (the name matching), `f_timeline_undo`,
     `flow_timeline_undo_incomplete` and `f_timeline_end`.
   - `flow_layout` in `userland/libflow/flow.c`: begin, undo on failure,
     end.
   - The timeline contract in `userland/libflow/internal.h` and
     `userland/libflow/include/flow/flow.h`.
   - In `userland/apps/yonder/yonder.c`:
     - `Page.timeline` (its comment sets the lifetime: the tree points
       into it);
     - `page_timeline`, `page_lay_out` (which calls
       `flow_timeline_undo_incomplete` before the reclaim retry);
     - `lay_out_shown`, `arrive_now`, the staged rebuild in
       `script_rebuild`, and `page_clear`.
4. The proof:
   - The timeline cases and the allocation-failure sweep in
     `tools/test_libflow_motion.inc`. Each allocation is failed in turn,
     alone and with every one after it, and the timeline must come
     through with its start times.
   - `motion_cases` and the face-budget retry case in
     `tools/test_yonder_scripts_host.c`.
   - `tools/test_motion_mutants.py`.

## The design questions that are yours

- **Lifetime.** Today the shown tree's `FMotion` records point into the
  timeline. After the swap, each tree points into its own set. Choose:
  - who owns a set (the tree, freed by `flow_free`, or the face);
  - when the old set may go. At a staged rebuild the old tree is still
    alive until the new one is published, and both may be read in
    between.

  Write the answer in the header's contract before the code.
- **An incomplete tree that is SHOWN carries its unmet records forward.**
  A layout that ran out of room part-way met only part of the page, and
  the elements it never reached must keep their starts. Today that is
  "an incomplete layout dropping nothing". Keep that case and its test.
- **The publish sites.** The face shows a tree in `arrive_now`, in
  `lay_out_shown` on success, and at the staged rebuild's publish in
  `script_rebuild`. Prefer one door that all of them go through to a
  commit call at each.
- **Holds.** A record holds its node (`os64_html_hold`/`release`), and a
  fresh set takes its own holds. Committing releases the old set's
  holds, and freeing a discarded set releases its own. The case "an
  element gone and its hold let go" must still pass.

## Codex round 13 finding on #265, booked here

It was booked because #265 took no changes after round 13: an unreviewed
fix would have reached `userland` unreviewed. The thread on #265 says it
went here.

**Thread 4274148552, `yonder.c` ~1766: a stuck rebuild freezes the
page's animations.** The sequence:
1. A script changes the DOM, and the staged rebuild that should follow
   is still owed (it failed for room, or a font-capacity retry released
   the tree).
2. `motion_tick` calls `lay_out_shown`, which reaches `page_lay_out`'s
   stale-version guard and returns false without attempting a layout.
3. `lay_out_shown` records that as a frame that cost `UINT64_MAX`.
4. When `script_rebuild` later succeeds, nothing resets `motion_laid`.
   `motion_next` stays `YONDER_NEVER` for the page's life.

**Fix:** `motion_next` returns `YONDER_NEVER` while
`g.page.rendered_version != os64_html_version(page_doc(&g.page))`. Do not
fix it only inside `lay_out_shown`: a frame that stays due and is refused
every time would spin the ticker.

**Test** (in yonder's host harness):
- `yonder_scripts_step` changes the page, so the rebuild is owed: nothing
  is due.
- `script_rebuild` runs: frames are due again, and `motion_us` is not
  `UINT64_MAX`.

It belongs in this PR because the swap reworks what the face records for
each layout attempt.

## How the work is run

- **Branch and PR.**
  - Branch off `userland` in a worktree under `.worktrees/`.
  - Never commit on `userland`. Chris merges.
  - Ask Chris before each commit; he asks for every commit.
- **Review.**
  - Chris schedules Codex rounds; never post `@codex review` yourself.
  - Fix each finding as its class: name the invariant and audit every
    site.
  - Each round, run the suites plus only the touched mutants (`--only`).
  - Run the FULL mutant harness once, after a clean round.
  - Reply on, and resolve, only the threads you answered.
  - A finding that arrives after the rounds are over goes to a later
    slice. It is not fixed on the branch.
- **Mutant harnesses.**
  - Anchor-check EVERY `tools/*_mutants.py` that names a file you
    touched: exec its list and count each `old` in its file; each must
    appear exactly once. Mutants of the deleted undo code go with it, and
    the swap gets its own: a commit skipped, a discarded set committed,
    holds not released.
  - Never run two harnesses in one tree; they edit sources in place. Use
    scratch worktrees to go parallel.
  - Partition files need a final newline: `while read` drops the last
    line without one.
- **The suites:** `tools/test_libflow_host.sh`, `test_garb_host.sh`,
  `test_yonder_warp_host.sh`, `test_yonder_host.sh`,
  `test_yonder_scripts_host.sh`, `test_yonder_stream_host.sh`,
  `test_dom_host.sh`, `test_html_dom_host.sh`, and `make -C userland`.
- **In the guest:**
  - `tools/pages/pile3-motion.html`, plus a page whose script restarts an
    animation by class.
  - Boot from scratch copies, headless, monitor on a free port of 55556
    or above; `ps` first, because sessions share the box.
  - In TCG, yonder's cost rule freezes the big fixture at about 220 ms.
    That is the policy working, not a bug. Use a light page to see
    motion.
- **Both lanes touch `motion.c`** (lane A the numerics and the schedule,
  this lane the timeline section) and the two test files. Expect
  whichever merges second to rebase.

## After this lane: sparse keyframes

The fourth ruling (Chris, 10/14) is **sparse keyframes, LAST**. Each
keyframe holds only the longhands it sets; the rest are read from
underneath at mix time, with their context (`em`, `currentColor`,
`inherit`, settled widths, `!important`) resolved there. That pays the
booked "a keyframe's `em` while `font-size` animates".

It starts from a design note backed by Chrome's answers to probe pages,
written BEFORE any code. The probe recipe is in
[PILE3_CLAMPS_SCHEDULE.md](PILE3_CLAMPS_SCHEDULE.md) § How the work is
run. The code waits for lanes A and B to merge. The
note can be written while either is in review, by whoever Chris gives it
to.
