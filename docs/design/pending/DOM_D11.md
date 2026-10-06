# D11: the classic page surface

Status: implementation on `codex/dom-d11`, stacked on `codex/dom-play`
(`28f1d785`, the combined D7/D9/D10 branch). Fable review and Chris's P5
acceptance remain pending. The transferred play build remains frozen.
Governing design: DOM.md and JAVASCRIPT_TASKS.md, D11/J5.

## Consumer scope

Chris chose these URLs on 2026-10-06 and selected their own visible behavior
as the priority:

| Page | Primary consumer | New surface |
|---|---|---|
| `http://www.lileks.com/mpls/` | Dreamweaver image swap/restore and preload construction | named images, live image/form collections, `Image`, resolved image `src`, legacy function arguments |
| `http://www.007museum.com/` | date insertion, named countdown form, inline color/visibility changes | live named form/control lookup, reflected `name`/`title`, inline styles; D9 supplies date insertion |
| `http://www.milliondollarhomepage.com/` | original `gsc3.js` controls, pixel-link titles and zoom pane | navigator sniffing, mouse coordinates, active `window.event`, image maps, inline styles and legacy rectangular clipping |

The Museum URL redirected to `https://www.nybrobildelar.se/` during the source
inspection; the returned page retains the museum's date/countdown scripts and
also includes modern shop scripts. Million Dollar Homepage's response is an
archival version. Modern social/analytics bundles are outside Chris's selected
scope. These responses do not establish that the original J5 counter/menu/form
examples have passed: Chris confirms the chosen pages on the P5.

Downloaded site sources stay outside the repository. Original regression
fixtures exercise their API patterns. An optional browser-host proof reads
local downloads through `D11_CLASSIC_SOURCES`, running Lileks' inline code and
handlers, the Museum date/countdown code, and Million's original `gsc3.js`
against a small control fixture. Its future-date override exercises the
Museum countdown's field-writing branch, whose 2008 deadline has passed.

## Binding and ownership

The native document and shared libpage state remain authoritative. Images and
forms are live HTML collections, with numeric indexing, `item`, `namedItem`
and nonenumerable named lookup. Document named lookup covers HTML forms and
images; form lookup covers the implemented input/select/textarea/button
controls, including associated external controls. Ordinary own properties and
prototype properties keep precedence. Duplicate document/form names return
live filtered collections. Form collections follow attach/detach transitions.
Collections and wrappers retain native holds through the binding's ordered
teardown, including detached elements.

`Image(width,height)` constructs a detached image with optional unsigned
attribute dimensions. `src` resolves against the current document base; typed
attribute mutation changes the same image the browser renders. Image dimensions
here reflect attributes; intrinsic/rendered dimension reporting is outside this
consumer surface. `name` is reflected on implemented named HTML kinds;
`title` is reflected on elements.

Each element has a stable inline style object which holds its native node.
Camel-case accessors cover libgarb's longhands; `cssText`, `getPropertyValue`,
`getPropertyPriority`, `setProperty` and `removeProperty` share the style
attribute. Invalid values leave it unchanged. Property writes remove matching
valid declarations and preserve unrelated text, so mouse motion does not grow
an append-only attribute. Coercions finish before current native text is read.
Custom names retain case, importance is honored, and strings/comments/nested
component values do not split on embedded semicolons. Temporary grammar sets
are heap charged to the binding. Input is capped at 64 KiB and native work
checks the active task budget before and after; parser refusal is an error.
This is source-preserving inline editing, not full CSSOM canonical/shorthand
serialization. The remaining work is recorded in DEBTS.md.

`navigator` is stable and readonly. `userAgent` reads the browser's selected
native agent; `appName` is `Netscape` so the original widget takes its event
argument branch. Legacy `appVersion` returns the text after the first slash;
`platform` maps Windows/Macintosh/iPhone presets or answers `OS64 x86_64`.
These are compatibility values, not claims about the guest operating system.
The owner-thread provider borrows text only for its getter call.
`document.captureEvents`/`releaseEvents` are compatibility no-ops because the
D7 dispatcher already propagates events; `Event.MOUSEMOVE` supplies its flag.

`window.event` reads the current native dispatch frame. Nested dispatch
restores the outer event and teardown retains no stack pointer. Mouse client
coordinates are viewport CSS pixels; page coordinates include scrolling;
screen coordinates remain native screen pixels. Readonly `scrollLeft` and
`scrollTop` use D10's current-layout provider: the standards root or quirks body
supplies viewport scroll, other scrollers supply their native position.
Scroll setters remain outside this slice.

## Engine compatibility

QuickJS's default inherited `Function.prototype.arguments` throws. Lileks'
original Dreamweaver helpers require `MM_preloadImages.arguments` and
`MM_swapImage.arguments`. A hash-checked engine patch adds
`JS_GetLegacyFunctionArguments`; libdom explicitly installs the getter in its
browser context. Standalone runners keep the upstream throwing property.

The helper accepts ordinary non-strict functions in its own context. It copies
an unmapped arguments object from the innermost active frame, or returns null
when inactive. No stack storage escapes; retained snapshots survive return and
writes do not alias parameters. Strict functions, arrows and other unsupported
function kinds throw. Native work is budget checked. This does not introduce
legacy caller access or a general replacement for the engine's arguments
semantics. Recursive and refused accesses are regression cases.

## Native visible behavior

Image maps resolve the image's `usemap` to a connected HTML map name. Rectangles,
circles, polygons and default regions are tested in tree order; first match
wins, including regions without links. Coordinates use the content origin,
exclude borders/padding, and are CSS pixels independent of intrinsic image
size and browser zoom. Coordinate text is bounded at 64 KiB and numeric
magnitudes at 1e12. The hit target is the area, so D7's inline handlers see its
title and libpage handles its navigation. The public flow box now carries its
computed content rectangle in the same frame as its border rectangle.

Legacy `clip: rect(...)` is parsed/cascaded by libgarb and applied by libflow to
absolute/fixed paint, hit testing and overflow. CSS2 allows four lengths/auto,
negative values, and uniform comma or whitespace separators. Empty rectangles
clip everything. Relative/static boxes ignore it. Arbitrary clip paths remain
booked in POSITION.md and DEBTS.md.

Lifecycle geometry can find the arriving Page during `DOMContentLoaded`,
before publication. A measured-then-edited arrival releases its old layout and
cascade before restaging sheets. Initial layout builds a cascade even with no
stylesheet, so inline attributes take effect. Native callbacks neither pump
events nor publish the arriving page during its task.

## Acceptance and remaining work

Host checks cover live named collections and detached ownership, image swaps,
repeated style editing, allocation refusal, nested active events and legacy
arguments snapshots. Browser checks cover map shapes/content origin/native
links/inline handlers, quirks numeric styles, zoom clipping, scrolled event
coordinates and lifecycle measurement. CSS and layout checks use independent
hand-computed grammar and clip expectations and their allocation sweeps.

Chris approved deferring eager detached-image prefetch on 2026-10-06. Images
fetch when displayed, so the first rollover can wait for its asset. Per-image
load/error delivery and eager cache jobs belong to that follow-up.

`document.referrer`, `readyState`, `lastModified`, window geometry, scroll
setters, complete CSSOM, frames/layers and modern embedded application APIs
are not part of these primary widget paths. No modern social-stack support or
whole-page/P5 acceptance is claimed. J5 and scripts-on-by-default stay pending.

## Local verification, 2026-10-06

| Check | Result |
|---|---|
| DOM target / instrumented engine profiles | 3,087 / 17,739 checks, zero failures; native allocation refusal sweeps clean |
| Browser host, including optional downloaded primary scripts | 2,349 checks, zero failures; 200% zoom and scrolled fixed clips included |
| libflow reference/corpus/fuzz/refusal suites | 18,612 checks, zero failures; 12 reference dumps match |
| libgarb parsing/value/cascade suites | 365 declaration cases plus existing suites and allocation sweeps, zero failures |
| libjs runtime target / instrumented profiles | 968 / 3,160 checks, zero failures or live allocations |
| Source manifest, public headers and target symbol/dependency audit | Pass; 187 engine / 17 runtime exports |
| Full boot-image build | Pass in the isolated D11 worktree |

ASan/UBSan and leak detection remained enabled for host verification. The
image is `os64_kernel.iso` in `.worktrees/dom-d11`; the small visible fixture is
installed as `/tests/pages/dom-classic.html`. This is local proof, not a P5 run.

## P5 testing: hover repaint starvation

Chris reported Million Dollar Homepage hover descriptions appearing only after
about ten seconds of main-thread CPU work, and remaining unchanged until mouse
motion stopped. Links still navigated. The original widget moves its popup and
replaces text on each mousemove, causing model/layout rebuilding. Yonder's loop
processed each raw input sample, drained its DOM input queue immediately, and
painted only when the GUI event queue emptied; its per-DOM-queue move merging
could not combine samples dispatched in separate GUI events.

A local profile of the downloaded page's 3,316-link tree and original primary
script measured approximately 1 ms in JavaScript, 16–17 ms rebuilding the model
and 25–27 ms rebuilding rendering per move. These are host measurements, not
P5 timings. They expose repeated work; continuous input also prevented the
rendering boundary from being reached.

The P5-driven fix is marked in code comments. Input work now yields after at
most 32 consumed samples. Consecutive idle moves with unchanged modifiers merge
before DOM dispatch; buttons, drags, wheels, keys, doorbells and pointer-state
changes preserve their order. Rendering, stream work and timers run between
batches even when more input waits. Lookahead remains unconsumed input for the
question bar, which stays disarmed until the queue is empty.

The browser regression feeds 1,000 pointer samples through the same iterator
and native hover/DOM/rendering paths: 32 current-position updates result, and
the first popup/layout update is ready with 968 samples still queued. Mixed
input checks preserve click/drag/wheel/modifier/key/timer/pointer-state order.
The optional `D11_HOVER_PROFILE` host proof reads `million-primary.html` from a
local download directory; its third-party scripts are removed and the original
`gsc3.js` is retained. `--script-audit` now reports model and rendering rebuild
time separately from script execution for the next P5 check.

This fixes the unbounded input drain and redundant pointer sampling. It does
not introduce incremental layout; P5 confirmation of latency remains pending.

Follow-up verification: 2,409 scripted-page host checks with the original-widget
and full-tree profiling options, zero failures; the native Yonder suite passes
179 checks and six paint references. Full updated boot-image build passes.
Only the Yonder executable changes for this follow-up on an existing D11
installation. The rebuilt P5 artifact still awaits Chris's retry.
