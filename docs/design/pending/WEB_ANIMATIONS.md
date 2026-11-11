# WEB_ANIMATIONS.md — `element.animate`, `Animation`, `getAnimations`

*Opus 5.5, 2026-11-09, for Chris. Fable reviewed it the same day
(`~/os64-opus-kit/reviews/WEB_ANIMATIONS_REVIEW.md`): approved as two
slices, its nine findings folded in below; Chris took the four leans
of § 7. PILE3.md built CSS animations
(`@keyframes`, the `animation-*` properties, Web Animations 1's clock,
Easing 1, interpolation by animation type, the matrices) and left
"the Web Animations API (`element.animate`, `getAnimations`)" as
scripts' surface for the Modern Web arc. This is that surface. It adds
no new animation engine: a script's animation is handed to the one
PILE3 built.*

## 1. Why now, and what pages ask

**A sign-in dialog in the corpus.** Every request the page makes to its
API carries a transaction header, a SHA-256 over the request and an
*animation key*. The page makes the key like this (the P5 record of
2026-11-08, replayed with the calls logged):

```js
div = document.body.appendChild(document.createElement('div'))
a = div.animate({color: ['#62df76', '#31a6fd'],
                 transform: ['rotate(0deg)', 'rotate(165deg)'],
                 easing: 'cubic-bezier(0.23,-0.07,0.53,0.55)'}, 4096)
a.pause(); a.currentTime = 440
getComputedStyle(div).color; getComputedStyle(div).transform
```

yonder has no `Element.prototype.animate`, so the generator falls back
to the key `000`, the hash is wrong, and the site answers the dialog's
two API requests with 404: the
dialog draws its frame and a disabled Continue, and nothing in it. The
case needs, exactly: property-indexed keyframes with an easing,
`pause()`, a `currentTime` set while paused, a `getComputedStyle` that
shows the animated value at once, and that value serialised as Chrome
serialises it, since the page hashes the STRINGS (`rgb(r, g, b)`,
`matrix(a, b, c, d, e, f)`) or numbers read out of them (§ 6). The time
is 440 for this record's page; it appears to come from the page's
verification key, so another page asks another time. That the server then accepts the
id is not proved until yonder makes one; the replay can show the
generator hashing a real key before the P5 does.

**The corpus** (every pull to 2026-11-08, 298 hosts with scripts, each
script counted once by content; only forms jQuery's `.animate` and an
array's `.reverse` cannot make):

| What | Hosts |
|---|---|
| any of the API | **79** |
| `getAnimations()` | 47 |
| `animate()` with `fill`, `iterations`, `direction` or `composite` | 37 |
| `commitStyles()` / `persist()` | 31 |
| the effect: `getKeyframes`, `setKeyframes`, `updateTiming`, `getComputedTiming` | 30 |
| `animation.finished` awaited | 29 |
| `animate([...])`, keyframes as an array | 21 |
| CSS animations read as objects (`animationName`, `CSSAnimation`) | 13 |
| scroll timelines (`ScrollTimeline`, `ViewTimeline`) | 12 |
| `document.timeline` | 9 |
| the constructors (`new KeyframeEffect`, `new Animation`) | 7 |
| `composite: 'add'` or `'accumulate'` | 1 |

Of the 79, 20 use only `animate`, the `Animation`'s controls, its
promises and events, `getAnimations` and `document.timeline`; most of
the rest use the effect's methods and `commitStyles`/`persist` too,
together, the shape of one animation library on many sites. The 47
`getAnimations` hosts are mostly UI libraries that wait on
`el.getAnimations().map(a => a.finished)` before removing an element.

## 2. Where each part lives

The API is libdom's; the values are libflow's; the clock and the frame
are yonder's.

**libdom** (`webanimation.c` — `animation.c` is `requestAnimationFrame`'s — and a row of `modern.c`'s table):

- `Element.animate(keyframes, options)`, `Element.getAnimations()`,
  `Document.getAnimations()`, `document.timeline` (a
  `DocumentTimeline` whose `currentTime` is the document's
  `performance.now()` clock).
- `Animation`: `play`, `pause`, `finish`, `cancel`, `reverse`,
  `updatePlaybackRate`, `currentTime`, `startTime`, `playbackRate`,
  `playState`, `pending`, `ready`, `finished`, `id`, `effect`,
  `timeline`, `onfinish`, `oncancel`, `onremove`, and the `finish`,
  `cancel` and `remove` events (`AnimationPlaybackEvent`). The state
  machine is Web Animations 1 § 4.4 as written: a start time or a hold
  time, the pending play and pause tasks resolved at the next frame,
  the ready and finished promises replaced when they settle, finish
  notification, cancel. Setting the current time of an animation whose
  pause is pending completes the pause at once (§ 4.4.4's silently-set
  steps with § 4.4.9's pending pause task): hold time 440, start time
  unresolved, `playState` `paused`, `pending` false, the task gone. That
  is the sign-in page's `pause(); currentTime = 440`, and WPT's
  `setting-the-current-time-of-an-animation.html` holds it.
- **The timeline's time is the frame's.** `document.timeline.currentTime`
  is the document's `performance.now()` clock SAMPLED once per frame
  (update the rendering), not read live: every `currentTime`,
  `startTime` and `timeline.currentTime` a script reads within one task
  agree, and two `getComputedStyle` reads in one handler see one time.
- **Processing keyframes** (Web Animations 1 § 6.6.3, Animations 2 §
  3.2's sibling for scripts): both forms, property-indexed and an
  array of keyframe objects; offsets (missing ones spaced evenly),
  `easing` per keyframe; property names from camelCase
  (`backgroundColor`, `cssFloat`). A custom property (`--custom`) is
  not animated in A, and `composite` is checked but not kept: every
  effect replaces (§ 4). Both are DEBTS rows. Each value is parsed by libgarb as the declaration
  `name: value` would be (a value that does not parse drops that
  property from that keyframe, as a stylesheet's would), so a script's
  keyframe IS what a `@keyframes` block gives libflow today:
  `garb_set_t`s with an offset and an easing. As for `@keyframes`, a
  shorthand expands to its longhands at the parse
  (`garb_read_declaration` makes longhands). `var()` is where the two
  part: libgarb's cascade substitutes it in an element's `@keyframes`
  ahead of layout, per element, and a script's keyframes arrive from
  outside the cascade, so `garb_read_declaration` answers
  `GARB_DECL_HELD` for such a value and slice A drops it from that
  keyframe, as a value that does not parse is dropped. Libraries hand
  `animate()` resolved values; the substitution is a DEBTS row of its
  own, a libgarb entry that substitutes a list of sets against an
  element's custom properties.
- **Timing** (`EffectTiming`, `KeyframeAnimationOptions`): a number is
  the duration; else `delay`, `endDelay`, `fill`, `iterationStart`,
  `iterations`, `duration`, `direction`, `easing`, `id`, `composite`,
  `iterationComposite` (checked, not kept: `replace` only in A; § 4).

**The timing model's one home is libgarb** (Fable's finding 2). Web
Animations 1 § 4.5-4.10's arithmetic, local time to phase (before, active,
after, with `delay` and `endDelay`) to iteration and iteration progress
(`iterations`, `iterationStart`, `direction`) to eased progress, is
needed by libdom (an `Animation`'s `currentTime`, `playState`, finish
detection, and B's `getComputedTiming`) and by libflow (the value it
renders). libdom does not link libflow and will not; both use libgarb,
which already parses the easings. So the arithmetic moves there, pure
and host-tested once (`garb/timing.h`), and motion.c's clock calls it
for CSS animations too, before A's own code is written on top of it.

**libflow:** the environment gains the page's script effects: for an
element, in creation order, each effect's keyframes, its timing, and
its clock as a start time, a hold time and a rate. The clock is enough
for libflow to work the effect's local time at any `now` itself, so a
running script animation ticks on the face's ticker exactly as a CSS
one does, with no call into script per frame. (Those three fields are
the whole contract between the two halves: a change to them is a change
to this sentence.) `animate()` in style.c
appends them after the element's CSS animations: Web Animations 1 §
5.4's composite order puts CSS animations first and script animations
after, by creation. Everything after that is PILE3's: the effect
stack, the easings (per keyframe interval), interpolation by animation
type, the matrices, `will-change`'s side effects, and the animation
origin in the cascade. So `getComputedStyle` reads the animated value
the way it reads a CSS animation's today. One difference from a CSS
animation: `display: none` ends a CSS animation (Animations 1 § 2), but
a script's effect goes on applying to its target's computed style, so
an element that is not shown still reads its animated value.

**yonder:**

- An `Animation`'s state change (play, pause, a `currentTime` or rate
  set, cancel) bumps the document's version, so the next
  `getComputedStyle` (`layout_current`) lays out again, and the
  following frame repaints. The sign-in page's freeze-and-read is that path:
  `pause()`, `currentTime = 440`, then the read lays out at the held
  time.
- Each frame runs HTML's "update animations and send events" before
  painting: the pending tasks resolve, finished animations settle
  their promises, and the events go out in that step's order.
- `flow_motion_due` already asks when the page's animations next
  change; a running script animation is one of them.
- **A tree keeps the effects it was laid out with**, for its ticks, its
  paints and its computed styles, so an effect outlives its animation:
  libdom buries a collected animation's keyframes with the generation
  it died in, and yonder's scripts tell it of each tree made with them
  (the generation it is laid out at) and each tree that goes, freeing
  what died before the oldest tree still standing. A page's tree can
  outlive its scripts (a page's teardown frees the scripts first), so
  a binding whose effects a standing tree may read is kept, drained,
  until the last such tree goes.

## 3. getAnimations and CSS animations

`getAnimations()` returns every animation that targets the element
(and, with `{subtree: true}`, its descendants), CSS animations
included, as `CSSAnimation` objects carrying `animationName`, in
composite order. libflow's timeline already keeps a record per running
CSS animation (element, name, start, hold), so the object wraps that
record, and its `pause()`/`play()`/`currentTime` write the record's
hold and start as `animation-play-state` does. **CSS transitions are
not built** (PILE3.md § Transitions, designed, waits), so there are no
`CSSTransition` objects to return; the libraries that wait on
`getAnimations()` then wait on nothing for a transition, which is what
they do today with the function missing and `?.` in front of it.

**Between A and B**, `getAnimations()` returns script animations only.
A library waiting on a CSS exit animation (a menu sliding away) then
resolves at once and removes the element a moment early: a one-frame
visual on a few UI libraries, better than today's throw. A P5 tester
who sees a menu vanish rather than slide should know that is this, and
that B ends it.

**`CSSAnimation`'s controls** (B2, as built): the host answers the
document's CSS animations through `os64_dom_set_css_animations` (dom.h),
which yonder answers from the timeline the page shows
(`flow_timeline_at`). A `CSSAnimation` is made when `getAnimations()`
first answers its animation, and kept while that animation lives, so
asking again gives the same object. From then on the `CSSAnimation`'s
own state machine — the one every `Animation` has — IS the clock:
each change (pause, play, a seek, the rate, finish, cancel) is handed to
the host (`flow_timeline_drive`), whose tree draws it, and a driven
record keeps its clock through every layout. `animation-play-state`
then no longer moves the record; the binding reads the property's
changes at the update step and answers each as CSS Animations 2 says:
a change to `paused` pauses the `CSSAnimation`, a change to `running`
plays it without rewinding — until a script takes the play state over,
after which the property's changes are answered no more (slice C,
Chris's ruling of 2026-11-10, replacing B2's "API wins until the
property next changes"). `play()` and `pause()` take it over whenever
they succeed; `reverse()`, and a start time set, when they move the
animation between paused and not (`css_take_over`; WPT's
`CSSAnimation-pausing`, as Chrome and Gecko do). An animation whose
name leaves the list cancels its `CSSAnimation`, which keeps its
`animationName` and loses its owning element. Its effect is a
`KeyframeEffect` with the animation's target and timing and no keyframes
of its own (DEBTS.md).

The CSS animation events (`animationstart`, `animationend`,
`animationiteration`, `animationcancel`) come from the same "update
animations and send events" step, because `CSSAnimation.finished` and
`animationend` are the same moment told two ways, and a page that waits
on one should not see the other never happen. While any of the four has
a listener, the step keeps a table of each CSS animation's phase and
iteration as it last saw them and queues an `AnimationEvent` for each
boundary crossed since (CSS Animations 2 § 4.2's table), sorted with the
playback events by when each happened. For them the step wakes when the
host's animations change, a listener comes or goes, or the next
boundary of one at rate 1 comes; one at another rate is watched every
frame, as a running `CSSAnimation` is. A listener that arrives hears
only what happens from then on: the animations that were already there
are taken as they stand, so a page that listens again after a
`{once: true}` listener fired does not hear an old end a second time.

## 4. The slices

**A: what the sign-in page needs, and the core 20 hosts.** libdom's `animate`,
`Animation` (the whole object of § 2), `document.timeline`,
`getAnimations` (script animations), keyframe processing and timing;
libflow's script effects in the effect stack; yonder's version bump
and frame step. Proof: the sign-in page's generator hashes a real key in the replay;
the validation page's freeze-and-read values match Chrome's (§ 6).

**B: the rest of the API**, built as two PRs, B2 stacked on B1.

- **B1:** `KeyframeEffect` and the `Animation` constructor; the effect's
  `getKeyframes`, `setKeyframes`, `getTiming`, `updateTiming`,
  `getComputedTiming`, `target`, `pseudoElement`, `composite` and
  `iterationComposite` (kept and given back); setting an animation's
  `effect`; `commitStyles()` (the animated value written to the `style`
  attribute); `persist()`, `replaceState` and the automatic removal of
  replaced, filling animations (§ 5.5, which is why `remove` exists). An
  effect is an object of its own: it owns its target and keyframes, and
  an animation has one or none.
- **B2:** `composite: 'add'` and `'accumulate'` per effect and per
  keyframe, applied in libflow's effect stack; `CSSAnimation` objects in
  `getAnimations()`, and the CSS animation events.

**Out of both:** scroll-driven timelines (`ScrollTimeline`,
`ViewTimeline`, `animation-timeline`; 12 hosts), a DEBTS row of their
own; `CSSTransition`, which arrives with transitions; animations of a
pseudo-element as a script sees them — its CSS ones run (slice C) and
are left out of `getAnimations()` and the events, a DEBTS row, and
`pseudoElement` in `animate()`'s options is NotSupportedError until
then.

## 5. Bounds

A script can make animations without end. Each is held by its
`Animation` object and charged to the page's document budget
(yonder.conf's `page_memory_mb`) by its keyframes' size, so a page
that makes too many is refused at `animate()`, by name, as any other
object it makes too many of. That is the bound; there is no cap per
element on what a layout is handed. A finished animation that does not
fill is not handed over at all, and one that does fill still shows,
so dropping it would restyle the element. What keeps a long-lived
page's filling animations from piling up is B's replaceable removal
(§ 5.5: finished, filling, and every property it sets overridden by a
later animation), which is invisible by construction, and is why the
spec has it and the `remove` event.

## 6. How it is proved

- **WPT `web-animations`** (at the checkout's pin, beside the other
  directories): `[required]` lists per slice with exclusions reasoned
  per page, as WebCryptoAPI's were. A takes `timing-model/animations/*`,
  `timing-model/timelines/*`, `animation-model/keyframe-processing/*`,
  the pages of `interfaces/Animation/*` for the surface A builds, and
  `interfaces/Animatable/animate.html`. B1 takes
  `interfaces/KeyframeEffect/*`, `interfaces/AnimationEffect/*`, and
  `interfaces/Animation/commitStyles.html` and `persist.html`; B2
  `animation-model/animation-types/*` and the composite pages, and from
  `css/css-animations` (added to the checkout for it) `AnimationEvent`
  and the event pages that reach a shown page — its CSSAnimation pages
  ask at load, before a page's CSS animations start (DEBTS.md).
- **The sign-in page:** the replay of the 2026-11-08 record, logged as in § 1, shows
  the generator's hash input ending in a real key instead of `000`;
  then the P5 shows the dialog's form.
- **`tools/pages/web-animations.html`:** a spinning, colour-changing
  box with Play, Pause, Reverse, Faster and Slower wired to its
  `Animation`, and measured checks. The sign-in case is held to Chrome's
  STRINGS at three places a mismatch can hide (Fable's finding 1):
  - the colour, interpolated in gamma sRGB, each component rounded to
    the nearest integer and clamped to 0..255 at the serialiser. The
    easing's y1 is -0.07, so early times give a NEGATIVE progress and
    an out-of-gamut colour, which the serialiser must clamp, not wrap;
  - the transform: the angle interpolated (matching functions), then
    the matrix printed as Chrome prints it, at most six decimal places,
    trailing zeros dropped (`matrix(0.965926, 0.258819, -0.258819,
    0.965926, 0, 0)`), through libgarb's number writer;
  - the time: since another page asks another time, the page SWEEPS
    `currentTime` across the run (every multiple of 4096 / 256) and
    holds every one, because the page's hash is only as good as the worst.
  Chrome's strings come from Chris opening the page there, as the W1
  captures did.
- libdom, libflow and libgarb host tests per piece (one reads the
  timeline twice around a busy loop and gets one number), and a mutant
  per rule (mutants_parallel, at most 12 workers, one run at a time).

## 7. Rulings (Chris, 2026-11-09: every lean taken)

1. **Two slices, A then B,** each a PR and its P5 test? My lean: yes;
   A alone settles the sign-in page and the core hosts, and B is a coherent second
   piece, not the remainder of a cut.
2. **`CSSAnimation` objects in B, not A?** My lean: B; A's
   `getAnimations` returns script animations, which is enough for the
   libraries that wait on it to stop throwing.
3. **The CSS animation events with B?** My lean: yes, with
   `CSSAnimation`, for the reason in § 3.
4. **Scroll timelines out,** a DEBTS row? My lean: yes; they are a
   timeline of their own and ride on scrolling, not on this clock.
