# The browser's second draft — what the first one taught

**Status:** the case for stopping and redesigning, written 2026-09-12 from the
review record of PR #98 (`wend`, the line-mode face). Chris's call; this is
the evidence and the reading of it, for Fable to take into a new design.

**The goal is not new and BROWSER.md does state it:** "Chris named the boss
fight — a graphical browser, os64 as daily driver". The mismatch is narrower
than "nobody said so", and worth naming exactly, because Fable wrote that
document and it got most things right.

Three things are missing from it, and all three are why this went the way it
did:

- **CONFORMANT is never said.** "Daily driver" is there; the standard it has
  to meet to be one is not. Every deferral in the booked table is justified
  against "the pages this face is aimed at", which is a bar the line-mode
  face has to clear and a daily driver does not.
- **JavaScript is nowhere in it.** The two bosses named are TLS and layout.
  JavaScript is a third and a larger one, and it reaches back into rungs
  already built (below).
- **The hard layer was mis-sized.** BROWSER.md says "HTML parsing is not the
  hard part — a tag-soup tokenizer for the real web's common elements is a
  weekend." The parsing was not the hard part and Quinn's libhtml proves it.
  What produced 62 findings was the layer the sentence does not mention: what
  a parsed page MEANS. Form ownership, successful controls, the data set,
  encodings, URL resolution. That layer is neither parsing nor layout, it is
  most of a browser's conformance surface, and the ladder has no rung for it.

## The record

`wend` is ~4,400 lines of app code with a host harness. It went through seven
review rounds in one day.

| Round | Findings | P1 | in `render.c` | in `wend.c` |
|---|---|---|---|---|
| 1 | 12 | 2 | 5 | 7 |
| 2 | 9 | 3 | 4 | 5 |
| 3 | 6 | 1 | 5 | 1 |
| 4 | 12 | 0 | 9 | 3 |
| 5 | 7 | 2 | 5 | 2 |
| 6 (self-review) | 5 | — | 4 | 1 |
| 7 | 16 | 1 | 12 | 4 |

**62 findings from the reviewer, 9 of them P1, plus 5 found in a self-review.
Every one was real. None was declined.** The rate does not converge: rounds
went 12, 9, 6 and then back up to 12, 7, 16.

After round seven I predicted where round eight would find things and looked.
Three more, in five minutes, all in the class the last three P1s came from:

```
method=dialog        -> sends http://host/s?pw=secret
formmethod=dialog    -> sends http://host/s?pw=secret
disabled default btn -> implicit submit fires; a browser would do nothing
```

A `dialog` form submits nothing to a server at all. We put the password in a
query string.

## Reading one: the rules are durable, the code that holds them is not

BROWSER.md says `render.c` is *deliberately throwaway* — "it walks and it
prints, it does not lay out … a cell grid is a rough draft of the wrong
thing." That is still the right call about LAYOUT.

But of `render.c`'s 2,517 lines of function body, **844 (33%) decide what
goes on the wire**, not what goes on the screen: form ownership, the form
data set in tree order, submitter overrides, URL resolution, percent
decoding, charset decoding, the successful-control rules. A form is submitted
identically whether it is drawn in cells or in pixels. The graphical browser
needs every one of those rules byte for byte.

So the loss is not "we fixed the wrong half". It is sharper than that:

> **Sixty-two findings' worth of specification reading now exists only inside
> a file we plan to delete, in a form nothing else can call, plus a hundred
> review threads on a pull request.**

The knowledge is the asset. It is currently stored in the least durable place
available.

## Reading two: one hazard, many doors

The nine P1s are not nine problems. They are two, reached through nine doors.

**Hazard A — the form goes out in a way the page did not ask for.**

1. R2: a GET form with a `formmethod=post` button was sent as a GET.
2. R3: `<button>` submitters carried no overrides at all, and crashed.
3. R5: implicit submission used the text box, not the default button, so the
   button's `formmethod` was bypassed.
4. R6: a default button inside a `hidden` subtree was invisible to the lookup.
5. R7: an unreachable submitter naming its form by `id` was recorded against
   the wrong form.
6. Predicted, confirmed, still open: `method=dialog`, `formmethod=dialog`, a
   disabled default button.

**Hazard B — values leave an encrypted page in the clear without asking.**

1. R1: a form on an https page with an `http://` action sent without asking.
2. R2: a `y` typed ahead answered the question it was never shown.
3. R2: the check read the page's `<base>`, which a page can move to http.
4. R5: bytes already queued in the terminal answered it instead.

Six consecutive rounds on Hazard A. Each fix was correct and each was an
instance, not the class.

**The cause is mechanical.** "Which submitter applies, and what does it
impose" is currently computed in six places across two files: `form->post`,
`spot->has_method`/`spot->post`, `form->unreachable_submit`,
`form_default_submitter()`, the method decision inside `wend_form_url()`, and
the refusal inside `form_send_if_alone()`. A rule spread over six sites grows
a new door every time the surface grows. That is the whole pattern, and it is
a design property, not a diligence problem.

## Reading three: two finished rungs cannot carry a daily driver

Worth knowing before a redesign, because both are already built and reviewed:

- **libhtml exposes no tree mutation.** It parses once into an arena and
  frees the document whole. There is no append, insert, remove, or create in
  its public header. JavaScript needs a live DOM: mutation, event dispatch,
  and re-layout driven from script.
- **libfetch sends no request body.** There is no method or body on the
  request side at all — only a response body to read. No POST, no XHR, no
  `fetch()`.

Neither is a defect. Both are correct for the ladder BROWSER.md described.
Both are load-bearing for the thing Chris actually wants, and finding that
out after the layout engine is written would be much worse than finding it
out now.

## What went wrong at the design level

1. **The durable half and the throwaway half were built in one file**, with
   no seam between "what this page means" and "how this terminal draws it".
2. **The constitution says to implement a thing when something asks for one**
   and justifies deferrals with "the pages this face is aimed at do not use
   it". That is right for a line-mode face and wrong for a daily driver.
   While it stands, the next model will decline conformance work on written
   grounds and be correct to. I did, repeatedly, and cited the document.
3. **Conformance was treated as something a reviewer finds**, not something
   the design guarantees. Sixty-two findings is what that costs per layer,
   and it does not scale to CSS, layout, the DOM, or an engine.

## What a second draft should carry

- **Add the missing rung.** Between "parse it" and "lay it out" there is
  "what does it MEAN", and that rung is where conformance lives and where
  every finding on this PR landed. It is also entirely front-end agnostic.
- **Name the shared layers before writing them.** The line between "what the
  page means" and "how this front end draws it" is the seam, and everything
  above it belongs in a library both faces link, the way libfetch and libhtml
  already are. Form semantics, URL and encoding handling, and the navigation
  model are all above that line.
- **Make the test corpus the durable artifact.** Markup in, expected result
  out, survives any rewrite of the code under it. The wend harness already
  half has this shape: its 56 form cases would survive a rewrite, its 43
  line-rendering cases would not. A conformance corpus that outlives three
  renderers is worth more than any of them.
- **Give conformance a number, per layer.** The web platform tests include
  form-submission and URL cases that are pure data and could run host-side
  exactly the way `tools/test_wend_host.sh` does. "Are we conformant" becomes
  a percentage that moves instead of a verdict that is argued. libhtml
  already proved the shape.
- **One door per rule.** Every P1 above came from a rule with more than one
  implementation site. The design test for a new rule should be "where is the
  single place this is decided".

## Open questions, honestly

- Is a forms/semantics library the right seam, or the wrong one designed
  against a single consumer? os64's own rule warns against building the
  second abstraction before the second caller exists. The counter-argument is
  that the second caller is already specified and is the point of the
  project.
- How much of the line-mode face is worth keeping at all, given the answer
  above? It is a useful thing to have and it is also the reason the durable
  code is in an app.
- Does the DOM need to be mutable from the start, or can libhtml stay
  parse-once until an engine exists? Retrofitting mutation into an arena is
  not cheap.
- What happens to PR #98. Its hazard fixes and its wire semantics are worth
  keeping whatever the redesign decides; the cell renderer may not be.

## One thing that is not in doubt

The review gauntlet works. Sixty-two real findings, none declined, several of
them passwords that would otherwise have gone out in query strings. The
problem is not the reviewer or the diligence. It is that we were asking a
reviewer to supply, one finding at a time, a property the design should have
guaranteed.
