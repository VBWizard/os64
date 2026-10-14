# PILE3_CLAMPS_SCHEDULE.md — lane A: the parser's clamps, then the schedule

Status: brief, written 2026-10-14 by Opus (S8a's builder) at Chris's request,
after #265 (S8a, CSS animations) merged as `89fb6465`. Two PRs, one after
the other, because both rework the same numeric and scheduling functions in
`userland/libflow/motion.c`. Lane B ([PILE3_TIMELINE_SWAP.md](PILE3_TIMELINE_SWAP.md))
runs beside this one. This file ships with A1. Once A2 is built, its
as-built text goes in PILE3.md and this brief is struck.

## Why this lane exists

S8a took thirteen Codex rounds. Nearly every finding was another corner
of two designs that cost more than they bought:
- **Every double a page could write was held**, so the code carries long
  double, `held()`, `fits()` and the clock's tolerance windows.
- **The schedule predicted every change** ahead of time: easing
  crossings, step edges, rounding windows, and a two-look probe of the
  value underneath.

Chris ruled both, 10/14; PILE3.md § Animations, both slices, Booked has
the rows. Both PRs should come out **net-negative in lines**. If one
grows, stop and say why before going on.

**Order.** Four simplifications come before S8b (animations in place):
A1 and A2 here, the timeline swap in lane B, and sparse keyframes last.
After S8b come S9 (web fonts), then the perf slice, then transitions with
`:hover`, which PILE3.md books ("as long as we don't forget").

## Read first

1. CLAUDE.md, especially the opening comment rules and how a review round
   is run. Then SUCCESSION.md.
2. PILE3.md § Animations (slices S8a and S8b): "The animation model
   (slice S8a)", then "Animations, both slices" — its Contracts, Booked
   and Proof sections.
3. The code:
   - `userland/libflow/motion.c`: the clock, the easings, the mix, the
     walk (`motion_walk`), and the schedule from `past_level` to the end
     of the file.
   - `flow_motion_due` in `userland/libflow/flow.c`.
   - `userland/libflow/internal.h` and `userland/libflow/include/flow/flow.h`.
   - The conversions in `userland/libflow/style.c`: `ms_of`, and the
     transform functions' parse, including the unit `skew(a, b)` keeps.
   - libgarb's parse of the `animation-*` longhands, the easings and the
     transform functions.
4. The proof:
   - `tools/test_libflow_motion.inc`, run with `bash tools/test_libflow_host.sh`.
     It includes THE SCHEDULE AGAINST THE VALUE fuzzer: `MOTION_FUZZ_CASES=200000
     MOTION_FUZZ_SEED=<n>` runs it longer, and `kFuzzRegressions` holds the
     seeds it has caught.
   - `motion_cases` in `tools/test_yonder_scripts_host.c`.
   - `tools/test_motion_mutants.py` and `tools/test_libflow_transform_mutants.py`.

## A1 — the input domain, clamped at the parser

Built 10/14. As built, with the limits chosen and Chrome's answers:
PILE3.md § Animations, both slices, Contracts, and What Chrome draws.

**The ruling (Chris, 10/14):** "the input domain is clamped at the parser
to what a page means, and past it a value is held at the limit." It
replaces the contract "The extremes are held, not resolved". Chris, on the
old one: "I really didn't know what we were signing up for."

- **Clamp each value once, at its one door.** That is libgarb's parse, or
  the first conversion into libflow's fields when that is the single
  door. A value past the limit is held AT the limit, never refused.
  Refusing would change which declaration wins, and that is a cascade
  question, not a numeric one.
- **Limits:** the leans I offered Chris (not yet probed):
  - times ±1e12 ms;
  - easing outputs (`cubic-bezier` y) ±1e6, since the x controls are
    already [0, 1] by spec;
  - matrix and scale numbers ±1e15;
  - animation names per element about 256.

  **Angles are clamped in magnitude and NOT reduced to a turn.** I said
  "reduced to a turn" once, and it is wrong: `rotate(0)` to
  `rotate(720deg)` spins twice. Check each limit against Chrome before
  writing it down. The numbers are yours to decide; tell Chris what you
  chose and why.
- **Then delete what the clamp makes dead.** Remove a function only when
  the clamp proves it dead, and say in the PR which clamp proved it:
  - the long double paths (there are about 37);
  - `held()` and `fits()`;
  - the long double in `decompose_2d`, `invert4` and `ms_of`;
  - possibly `F_BLEND_DEPTH_MAX`. Nested blends recurse once per level,
    and about 650 levels overflow the 1 MB userland stack. With names
    per element clamped well below that, the depth bound may go, or may
    stay as belt and braces. Your call.
- **The contract text changes in the same PR as the code.** That means
  the Contracts bullet, this debt's Booked row (struck, or moved to as
  built), and every comment that says "held" or "extremes". A doc that
  claims code that is not there is the failure this lane exists to end.
- **Proof:**
  - The fuzzer at the domain's edges: controls, times and counts at and
    just past each limit.
  - The existing suites, unchanged in what they assert, except the cases
    that tested values past the new limits. Each of those becomes a test
    that the value is held at its limit.
  - Mutants: one for each clamp, and remove the mutants of deleted code.

## A2 — the schedule (stacked on A1, or after it merges)

**The ruling (Chris, 10/14, "the middle ground"):** while an animation
that can show is in its ACTIVE phase, lay out at the frame rate. Work out
only the phases' edges: before → active, active → after, and the fill.
One cheap rule is kept: **an interval whose two ends are equal holds
still, and is due at its end** (carousel holds, pauses).

- **The contracts.** "The schedule is never late" stays, with the fuzzer
  as its referee. "A value that stands still is not asked for" narrows to
  "a value holding still between two equal keyframes is not asked for".
  The easing contract's sentence about the schedule finding crossings
  goes.
- **Delete:**
  - `ease_crossings` and `past_level`;
  - the step-edge prediction in `due_step`;
  - `change_due` and `clock_tol`, unless A1's clamped times leave a
    phase edge that still needs a window. Say which;
  - `changes_by`, `under_step`, `probe_alloc` and the ProbeLists;
  - the N-look machinery in `motion_walk`, if one look is all that
    remains.

  **Keep:**
  - the view rules: `in_place` with `motion_seen`, `may_own_canvas`, and
    the wake through stacked descendants;
  - `anim_shows` and `will_bit`.
- **The trap in the equal-ends rule.** An implicit keyframe rests on the
  value underneath it. When that value belongs to another animation that
  is active, the two ends are equal only at this instant. Treat such an
  interval as moving. Test it with a moving animation under a still one.
- **The yonder face is unchanged** (`motion_next`, `MOTION_COST_MS`,
  `MOTION_FRAME_MS`, the 1/3 duty). The frame rate there was already the
  ceiling, and this PR makes it the common case.

### Codex round 13 findings on #265, booked to A2

They were booked because #265 took no changes after round 13: an
unreviewed fix would have reached `userland` unreviewed. Each thread on
#265 says where it went.

1. **The wake through a seen stacked descendant is over-eager** (thread
   4274148558, `flow.c` ~1741). It calls `f_motion_next` on the
   ancestor's WHOLE motion, including in-place properties that draw
   nothing in the descendant (`background-color`). The cost is extra
   layouts; nothing is ever late or wrong.

   **Fix:** the wake asks only for what an ancestor's animation can draw
   into the descendant: the inherited properties, and the group effects
   such as opacity. So `f_motion_next`, or whatever replaces it, takes a
   property filter.
2. **`blend_same` probes only square boxes** (thread 4274148565,
   `motion.c` ~1667). Its two boxes are 0 and 1000x1000, so a tail whose
   percentage follows the width at one end and the height at the other
   compares equal. It feeds `same_value`, which the equal-ends rule keeps.

   **Fix:** compare by structure: the same `t`, the same `nfrom`/`nto`,
   and `function_same` on each tail, recursing for nested blends (bounded
   by `depth`). Sampling more box shapes cannot be complete: with
   `perspective()` the matrix does not vary linearly with the box. A
   structural compare can only call equal blends different, which errs
   early.

   Once the probe is deleted, check whether two DIFFERENT blend objects
   can still reach this comparison. If they cannot, say so and simplify.

## How the work is run (both PRs)

- **Branch and PR.**
  - Branch off `userland` in a worktree under `.worktrees/`; A2 branches
    off A1 if A1 has not merged.
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
    appear exactly once. #265 left the transform harness with eight dead
    anchors for ten rounds.
  - Never run two harnesses in one tree; they edit sources in place. Use
    scratch worktrees to go parallel.
  - Partition files need a final newline: `while read` drops the last
    line without one.
- **The suites:** `tools/test_libflow_host.sh`, `test_garb_host.sh`,
  `test_yonder_warp_host.sh`, `test_yonder_host.sh`,
  `test_yonder_scripts_host.sh`, `test_yonder_stream_host.sh`,
  `test_dom_host.sh`, `test_html_dom_host.sh`, and `make -C userland`.
  The cross build catches what the host suites cannot. A function named
  `word` breaks it under `-masm=intel`.
- **Chrome is the oracle.** Write a probe page that prints
  `getComputedStyle` into a `<pre id="out">` on load. A `paused`
  animation with a negative delay is a deterministic frame. Then run:

  ```
  timeout 60 "/mnt/c/Program Files/Google/Chrome/Application/chrome.exe" \
    --headless=new --dump-dom "file://wsl.localhost/Ubuntu-big2$PWD/probe.html" \
    | sed -n '/<pre id="out">/,/<\/pre>/p'
  ```

  `/mnt/c` is read-only from Chrome's side, so serve the page from the
  WSL path. Record each answer in the test that holds the code to it.
- **In the guest:**
  - `tools/pages/pile3-motion.html`, each row against headless Chrome.
  - Boot from scratch copies, headless, monitor on a free port of 55556
    or above; `ps` first, because sessions share the box.
  - In TCG, yonder's cost rule (no animation layouts on a page whose last
    layout or frame cost over 200 ms) freezes the big fixture at about
    220 ms. That is the policy working, not a bug. Use a light page to
    see motion.
- **Both lanes touch `motion.c`**, in different sections, and the two
  test files. Expect whichever merges second to rebase.
