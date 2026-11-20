# Review: WHEEL_FILELIST_INNERTEXT.md

*Fable, 2026-11-19, at Chris's request. For the Opus who wrote the design.*

**Verdict: build it.** Slice A is ready as written. B and C each have one
thing to settle in the design before their code (findings 1 and 3). The
four slices and their order stand.

## What the design gets right

- Walls first: A ends both `ReferenceError`s, B the description's
  `TypeError`.
- Passive listeners arrive WITH the dispatched wheel (§ 4.1). Without the
  default passive value, a careless `preventDefault` on the window or the
  document would stop scrolling on ordinary sites. Keep that coupling: C
  never ships without it.
- `'onwheel' in div` stays false until C.
- `ui_mouse_family(kind)` in place of the scattered `== D_UI_POINTER`:
  the class is fixed, not the instance.
- Captures before code where the standard and the browsers may differ
  (the `files` setter, the flat tree).

## Findings

1. **B: a read of `innerText` forces a layout, and it needs only styles.**
   § 3.2 makes the layout current (`layout_current`) at each read. The
   four facts are style facts; no box position is used. Layout is the
   cost JS_SPEED_REPORT measured at about half a second on a heavy page,
   and a script that writes the DOM and reads `innerText` in a loop pays
   it every time round. Say in § 3.2 whether pass 1 (styles) can be made
   current without the box layout. If it can, B uses that. If it cannot,
   B says so, measures one read on a large page, and books the split
   under the perf arc.

2. **B: the YouTube proof runs where the first cut is off.** The host
   replay installs no provider, so there `innerText` is `textContent`.
   That proves the wall is gone and nothing about hidden text. Say so in
   § 3.5, put the real check of the description in QEMU (the P5 is
   Chris's), and add a line to the replay's known differences (REPLAY.md
   or its DEBTS section): a replayed page gets `textContent` for
   `innerText`.

3. **C: scroll first when nothing can cancel.** § 4.3 scrolls at once
   only when the page has no `wheel` listener. With § 4.1 in place the
   common case is a page whose wheel listeners are all passive, and those
   cannot cancel either: scroll at once and dispatch afterwards, in the
   same slice, not as a later DEBTS row. It is one more answer from
   `yonder_scripts_listens` (does any non-passive `wheel` listener
   exist?). The lifecycle table gains that row. What stays a DEBTS row is
   the finer test (non-passive listeners that are not on this event's
   path).

4. **B: the walker on a very deep tree.** `inner_text.c` walks a whole
   subtree in new C code. Either it does not recurse per level, or it
   names the limit it leans on (the parser's depth cap, if there is one,
   with the number). A host case at that depth.

5. **B: two cases for the host suite.** `<script>` and `<style>` inside
   the element read give nothing (they are `display: none` by the UA
   sheet, so this proves the provider sees the UA sheet's answer); and
   `innerText` read ON a `<script>` gives its source (not rendered, so
   `textContent`).

6. **C: the axis.** § 4.2 gives `deltaY` only. Whatever yonder scrolls
   sideways for (Shift+wheel, a horizontal wheel) is `deltaX`, with
   `deltaY` zero; say what yonder does today and match it.

## Scope, for Chris's ruling

- **A serves YouTube** (the name `WheelEvent`) and the form library
  (`FileList`). **B serves YouTube's description.**
- **C has no customer in the corpus.** YouTube needs the name, which A
  gives; no page seen has failed for want of a dispatched `wheel`. C is
  also the slice with the most reach (what `preventDefault` means for
  every listener; scrolling passing through script). My lean: build A
  and B, stop for Chris, and build C when he says or when a page needs
  it. The design keeps.
- **D** is designed after B, as the document says.

## Process

- The design ships with slice A's code, not as its own PR.
- Review as § 6 says: a cold Opus read for A; Codex for B and for C.
- C's P5 step is Chris's. With finding 3 built, a page with only passive
  listeners scrolls as it does today by construction; the step that is
  left is a page with a non-passive listener.
