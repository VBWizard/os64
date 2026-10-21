# Modern Web design review

Reviewer: Quinn (Codex). Date: 2026-10-21.

Reviewed `fable/modern-web` at `30833121`, in the `tnw` worktree:
[MODERN_WEB.md](MODERN_WEB.md), its
[slice briefs](MODERN_WEB_BRIEFS.md), the linked
[RELAYOUT](RELAYOUT.md) and [SVG](SVG.md) designs, and the existing code
their contracts depend on. Source line numbers below refer to that revision.

**Keep the campaign's direction, but revise several contracts before the
full fan-out.** The standards-first approach, small PRs, and separation of
host correctness from P5 performance are sound. Some implementation
shortcuts undermine those protections. These findings concern implementation
and release behavior, not prose cleanup.

1. **[P1] The proposed module-discovery sequence cannot work with the pinned QuickJS.**

   [Slice 3 brief](MODERN_WEB_BRIEFS.md#slice-3-es-modules), lines 630–637.

   The plan compiles with `COMPILE_ONLY`, extracts imports, then fetches
   dependencies. QuickJS actually calls `js_resolve_module()` before
   returning the compile-only result
   ([quickjs.c](../../../userland/libjs/upstream/quickjs.c#L37166),
   lines 37166–37174). An unavailable dependency therefore fails compilation
   before the proposed getter can inspect it.

   A focused probe against the pinned engine reproduced this: compiling one
   import invoked the loader and threw "source not fetched yet." The patch
   needs a genuine compile-without-resolution operation, followed by explicit
   resolution/linking once the graph is available. Adding the request-list
   getter alone is insufficient.

2. **[P1] The synchronous networking path cannot enforce the promised Fetch policy as specified.**

   [Slice 2 brief](MODERN_WEB_BRIEFS.md#slice-2-fetch), lines 532–536;
   [slice 3 brief](MODERN_WEB_BRIEFS.md#slice-3-es-modules), lines 656–660.

   Synchronous XHR, and later dynamic imports, use `way_fetch_whole`. That
   helper replaces the caller's hooks, installs ordinary cookie handling,
   follows redirects through its own callback, and returns no CORS response
   headers. Passing security callbacks into it will not preserve them. See
   [load.c](../../../userland/libway/load.c#L154), lines 154–159 and 62–81,
   and `way_whole_t` in
   [way.h](../../../userland/libway/include/way/way.h#L306), lines 306–317.

   Give asynchronous fetch, synchronous XHR, and module loading one
   policy-capable transport. It must apply credentials and redirect
   decisions before requests leave, and retain the metadata needed to
   validate network and cached responses. Otherwise the synchronous route
   becomes a separate, less protected implementation.

3. **[P1] Per-binding localStorage snapshots can lose another window's data.**

   [Slice 1a brief](MODERN_WEB_BRIEFS.md#slice-1a-storage-and-the-small-globals),
   lines 326–343.

   Each binding loads its map once and later writes the entire image. Open
   two windows on the same origin; A writes key `a`, then B writes unrelated
   key `b` from its older snapshot. B's rename deletes A's update. Clearing
   storage can likewise be undone by a stale window's later flush.

   Atomic rename prevents partial files; it does not solve concurrent
   ownership. This needs shared storage or coordinated per-operation
   updates with invalidation. Deferring `storage` events does not make lost
   writes acceptable.
   [HTML defines local storage as shared state.](https://html.spec.whatwg.org/multipage/webstorage.html#introduction)

   The brief does correctly distinguish `sessionStorage`: per window,
   across navigation, never a file. The campaign's less precise storage
   description is not a separate finding.

4. **[P1] MutationObserver cannot be reconstructed from this lossy layout log.**

   [RELAYOUT change log](RELAYOUT.md#the-change-log-libhtml), lines 124–136;
   [observer delivery](RELAYOUT.md#requestanimationframe-and-the-observer),
   lines 332–349.

   Evicting entries and returning `-1` works for layout: rebuild everything.
   It cannot recover mutation records, their order, or old values. A single
   script can exceed the log cap before its checkpoint.

   Filtering observers only at the checkpoint also uses the wrong moment:
   registrations and ancestor relationships may have changed since the
   mutation. `takeRecords()` must work inside the same task, and setting an
   attribute to its existing value can still require a record despite no
   document-version change.

   Keep the shared mutation instrumentation, but capture observer
   eligibility when mutations happen and preserve pending records.
   [The DOM algorithm queues records at mutation time.](https://dom.spec.whatwg.org/#queue-a-mutation-record)

5. **[P1] The bytecode cache's source hash does not validate the bytecode.**

   [Slice 7 brief](MODERN_WEB_BRIEFS.md#slice-7-the-bytecode-cache),
   lines 786–802 and 825–830.

   A bit flip in the bytecode leaves the stored source digest unchanged,
   so the proposed guard still passes. The suggested corruption test does
   not need a "forced-correct digest": the source digest is already correct.

   Retain source identity, but separately validate the serialized payload's
   integrity, framing, and size before handing it to QuickJS; corrupted
   entries should become cache misses. Keep the trusted-directory assumption
   explicit, because a checksum does not authenticate attacker-controlled
   files.
   [QuickJS warns that its bytecode receives no security validation before execution.](https://bellard.org/quickjs/quickjs.html#Script-evaluation)

6. **[P2] Draining jobs does not mean a module's top-level await has finished.**

   [Slice 3 brief](MODERN_WEB_BRIEFS.md#slice-3-es-modules), lines 614–617
   and 688–692.

   A module awaiting a timer or fetch can have an empty engine job queue
   while its evaluation promise remains pending. The focused probe
   demonstrated an empty queue with a pending module evaluation promise.

   The module host needs an evaluation state that survives across
   event-loop turns, retains the promise, observes eventual rejection, and
   tears down safely on navigation. Waiting inside the original task
   prevents the event that would resolve it; treating an empty queue as
   completion reports success too early.

7. **[P2] The relayout generation rule defeats reuse after every cascade update.**

   [Cascade update](RELAYOUT.md#restyle-the-cascade-and-pass-1),
   lines 155–161; [reuse condition](RELAYOUT.md#the-layout-memo-passes-2-and-3),
   lines 225–228.

   `garb_cascade_update()` advances the generation, while reuse requires the
   previous generation to match. With correctly recorded generations, even
   a one-element change invalidates every candidate subtree.

   Separate changes to the stylesheet/environment from updates to individual
   elements. Also make content and structure dirtiness explicit: style
   equality alone cannot justify reusing text measurements after the text
   changes. The promised "one changed block, steady siblings reused"
   fixture should be an early design test.

8. **[P2] The replay format stops representing page execution once storage and fetch arrive.**

   [Slice 0b brief](MODERN_WEB_BRIEFS.md#slice-0b-the-tally-and-the-replay-corpus),
   lines 250–284.

   The recording contains HTML, scripts, and sheets, indexed by URL. It
   contains neither initial storage state nor fetch/XHR responses, request
   methods/bodies, or response headers. Once `fetch` becomes available,
   pages enter new paths whose requests the replay necessarily refuses.

   Extend capture alongside the features that require it, including request
   identity, response metadata, and relevant initial state. Report missing
   capture data as incomplete replay, separately from browser failures.
   Otherwise correct feature additions produce apparent regressions, while
   the resulting exceptions conceal real ones.

9. **[P2] The WPT gate measures retained passes, not readiness to expose an API.**

   [Slice 0a brief](MODERN_WEB_BRIEFS.md#slice-0a-the-runner-and-the-switch),
   lines 181–185; [shared oracle rules](MODERN_WEB_BRIEFS.md#what-every-slice-does-the-same-way),
   lines 87–94.

   The specified failure condition is losing an existing pass. A new
   interface can therefore pass a small fraction of its tests and still
   satisfy that gate. Excluding tests only as anonymous counts makes it
   difficult to tell whether essential behavior was ever exercised. This
   does not enforce "whole API or none."

   Keep the regression baseline, but add a named required test set for
   enabling each interface, explicit exclusions, and harness-status checks.
   Use WPT's manifest for discovery; its maintainers recommend it because
   enumeration is more complicated than filename scanning.
   [WPT runner guidance.](https://web-platform-tests.org/running-tests/custom-runner.html)

10. **[P2] Standalone SVG images need their own cascade.**

    [SVG integration](SVG.md#the-seam-exactly), lines 157–169.

    The picture job parses an external SVG into a private document, but the
    renderer's style provider consults the enclosing page's cascade. Those
    private nodes have no entries there: the existing `garb_style_for()`
    returns an empty style for unknown nodes
    ([cascade.c](../../../userland/libgarb/cascade.c#L2232), lines 2232–2238).

    Give an SVG image document its own cascade, including its presentation
    attributes and internal styles, with ownership tied to the picture.
    Inline SVG can use the page cascade. Otherwise simple externally styled
    icons lose their declared paint even when parsing and rasterization work.

11. **[P2] History updates must not fire hashchange by any route.**

    [Slice 1b brief](MODERN_WEB_BRIEFS.md#slice-1b-the-windows-view-the-scroll-and-the-history),
    lines 438–448.

    `pushState(..., "#section")` and `replaceState` do not fire
    `hashchange`. The proposed blanket rule sends extra events to routers
    that use those methods specifically to update the URL without
    initiating another navigation.

    Define events separately for state updates, fragment navigation, and
    history traversal. Add a fixture proving that push/replace to a
    different fragment emits neither `hashchange` nor `popstate`.
    [HTML's history algorithms.](https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-history-pushstate)

**Recommended sequence:** continue the runner and capture groundwork while
settling the module lifecycle, shared network policy, storage ownership, and
observer contracts before their builders commit to those designs. Strengthen
the enablement gate before new surface lands enabled by default. The
low-single-digit site-failure target remains an acceptance goal; the parse
census and engine timings do not establish it yet.

**Verification:** the focused C probe compiled the repository's pinned
QuickJS upstream sources with ASan and UBSan. It exercised compile-only
evaluation of an import whose loader refuses the unavailable dependency,
then evaluated a module awaiting an unresolved promise and drained the job
queue. Its output was:

```text
loader called during COMPILE_ONLY: https://example.test/dep.js
compile_only_exception=1 loader_calls=1
ReferenceError: source not fetched yet
after draining 0 jobs: pending_jobs=0 module_promise_pending=1
```

The probe passed ASan, UBSan, and LeakSanitizer. The first leak check hit
the sandbox's thread-attachment `EPERM`; the same binary passed outside the
sandbox with leak detection enabled. This is focused engine evidence, not
full implementation validation. No P5 performance measurements were taken.
The review changed no implementation files.
