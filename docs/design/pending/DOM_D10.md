# D10: synchronous script geometry

Status: implemented and locally validated, based on `userland` `29641e20`.
Governing design: DOM.md § Geometry. D8 is a separate review; D7's dispatcher
is concurrent work. This slice keeps both interfaces additive.

## Scope and host seam

Element.getBoundingClientRect returns a detached numeric snapshot of the union
of an element's border-box fragments, in viewport-relative CSS pixels. HTML
elements expose offsetLeft/Top/Width/Height/Parent; elements expose
clientLeft/Top/Width/Height. Detached nodes and connected elements without a box answer zero offset
dimensions/rectangles and null offsetParent. Client sizes of non-atomic inlines
are zero. Connected root client sizes answer the viewport in standards mode;
the connected body supplies viewport client sizes in quirks mode.
Offset positions use the first border fragment and the offset parent's padding
edge, except a static body uses the document origin. Widths/heights use the
unscrolled union. Body/root/fixed offset parents are null; a positioned
ancestor, body, or table/cell for static elements is the offset parent. Non-HTML elements return zero geometry. SVG layout,
getClientRects, scroll setters and window geometry remain outside this HTML
first slice.

libdom accepts an owner-thread geometry provider through a separate setter,
without growing the options struct. Providers copy results and retain no engine
values. They ensure model/layout freshness before answering, using the view's
size and zoom; a partial parser document can be supplied through the same
snapshot interface. A missing provider or a refused HTML layout throws
InvalidStateError; failed fresh rebuilds throw a bounded error rather than
return stale coordinates.
Prototype accessors preserve ordinary receiver validation and ABI checks.

A callback-safe libjs budget check observes the current turn's deadline,
cancellation and memory failure without entering JS, draining jobs or resetting
the budget. Geometry checks before native work and afterward. A script cannot
catch a native overrun and continue forcing layouts. Native layout is
synchronous and cannot be interrupted midway; its full elapsed wall time is
charged to the turn, including failed attempts and retries.

The browser counts actual layouts attempted inside geometry and reports their
elapsed time in the status line. Repeated reads reuse fresh layout. DOM edits,
viewport/zoom changes and changed sheets force current layout; a failed result
is refused. The existing form/state/model ownership and recovery rules apply.

## Acceptance

Independent numeric expectations cover border vs client size, position relative
to a positioned parent, scroll/fixed/sticky placement, zoom, inline fragments,
hidden/detached nodes, snapshots surviving later edits and fresh reads after
mutation. Failure cuts refuse stale geometry and allow a clean retry; engine
failure remains sticky. Timed native work proves overrun classification and
that caught failures do not allow more layouts. Parser-stop tests exercise a
partial tree at view size before resume. Existing browser/DOM/runtime suites
remain regression checks.

Combined engine/native stack measurements use script recursion plus the
actual flow layout, with deepest observed native frames sampled and retained
headroom reported in host and guest evidence. Measurements establish tested
workloads rather than arbitrary callback safety. The combined play branch
validates D7 listener budgets and fresh geometry against stopped and
stylesheet-waiting loading documents in the browser host suite.

## Guest stack evidence

The actual shared-library consumer on os64's 1 MiB thread stack uses a 128 KiB
engine stack and runs recursive JavaScript to the measured depth of 160 before
forcing native layout. Native font/image callback samples produced:

| Shape | Native nesting | Sampled used bytes | Sampled headroom bytes |
|---|---:|---:|---:|
| Blocks | 509 | 858209 | 190367 |
| Tables | 126 | 194913 | 853663 |
| Inline-blocks | 254 | 783617 | 264959 |
| Absolute | 254 | 662721 | 385855 |

All 42 guest checks passed, including parser-stop geometry, mutation freshness,
engine reuse and heap integrity. These are sampled workload measurements, not
an exact deepest-instruction bound. A 256 KiB engine profile left only 71231
sampled bytes in the block fixture; the browser's 128 KiB selection retains more
than the tested 128 KiB native headroom threshold. Standalone libjs is unchanged.
The browser installs the provider for each document's host. Loading parsers
retain a measurement model and layout until arrival or teardown; shown pages
publish forced layouts through relayout. Blocking scripts wait up to the
stylesheet deadline for preceding sheets; sheets added during a script's task
are fetched and apply when available.

## Final verification

The browser host suite passes 2102 checks, including caption-inclusive tables,
multiline inlines, nested scrolling, sticky placement, native allocation refusal,
and a caught native timeout that cannot force a second layout. The DOM suite
passes 4589 sanitized checks; runtime suites pass 906 target-core and 3098
sanitized-core checks. Target export/dependency and contract-header audits pass.
The strict full image build and optional guest consumer build pass. The final
guest consumer matches the host binary byte for byte and passes all 42 checks.
The validation VMs are stopped.

[Visible fixture](fixtures/dom-geometry.html): with page scripting enabled,
Yonder shows PASS with widths 110 then 150 and client width 146. The status line
reports forced layouts and elapsed time. Serve the file through the existing
browser fixture route or copy it to a browser-accessible location.
