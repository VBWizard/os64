# Relayout 5c: ownership and correspondence foundation

This checkpoint implements steps 1 and 2 of
[RELAYOUT_GRAFT.md](../design/pending/RELAYOUT_GRAFT.md). It does not copy placed
geometry or intrinsic widths, and Yonder continues using its existing layout
entry. The counters here describe history and structural eligibility, not a
performance improvement.

## Ownership

`os64_text_run_retain` adds an owner without allocating. Reference overflow
returns `OS64_FONT_LIMIT` without changing ownership; NULL returns
`OS64_FONT_BAD_ARGUMENT`. The last release drops the font references and glyph
pins. The context counts run objects, so destruction remains busy until the
last owner releases.

The fake-backend tests exercise either release order, an empty run, reference
overflow, a full allocation budget, cache eviction pressure and surviving
byte/glyph/caret views. The real-backend tests release the original owner and
the caller's font reference before exercising drawing and outline access with
DejaVu Sans, DejaVu Sans Mono, Source Sans 3 and Source Code Pro.

`python3 tools/test_text_host.py --real --output <proof-directory>` passed
**10,504 checks**, with ASan, UBSan and LeakSanitizer enabled. The target
`libos64.so` builds and exports the additive retain API.

## Snapshot history and fresh correspondence

`flow_layout_incremental` snapshots explicit resource/state identities and
revisions. A complete tree retains a native mutation-log reader and saved
preorder DOM membership. Reads accumulate dirty bits back through saved
parents; consuming a later delta cannot erase an earlier mutation. A lost
reader is freed immediately and its snapshot remains disqualified.

Fresh boxes are matched to unique named source boxes, with anonymous children
matched in their parent's ordered sequence. Repeated node identities refuse
an ambiguous match. Equality includes processed text, generated markers,
styles by value, decoration ancestry and child order. Motion and excluded box
roles prevent geometry eligibility; an eligible ordinary descendant inside an
excluded ancestor still gets its own comparison. Call-signature and float
influence proofs belong to step 3.

Persistent membership and call-local correspondence share a metadata ceiling
equal to the effective layout arena budget. Neither allocation holds a chain
of older trees. Preparation refusal leaves the ordinary layout available;
source addresses in correspondence are released before the call returns.
Preparation time is charged to `layout_us`. Existing `profile.reused` remains
zero because geometry is rebuilt.

`tools/test_libflow_host.sh --snapshots` passed **170 checks**, including all
**61 allocation positions** as both single and persistent failures:

- Repeated reads from an older displayed source, saved ancestry after a move,
  and an independent reader observing the same mutation.
- History loss, immediate reader release and refusal after log-budget recovery.
- Font, replaced-size, state and resource-owner changes; clock passage alone
  preserves history readiness.
- A metadata budget refusal with a complete, usable ordinary layout.
- Changed and restored text, inherited style changes, reordered children,
  regenerated list markers, animation ancestry and split inlines.
- Local eligibility below a grid item, plus whole-layout dump equality after
  freeing the source tree.

The full libflow host suite passed **49,726 checks**, including 597 sampled
layout-fuzz trees, 12 frozen corpus dumps and the existing allocation/valid-prefix
sweeps. The final two-sided ambiguity guard is covered by a subsequent focused
snapshot run. The target `libflow.so` builds and exports the new entry and
statistics accessor.

Sanitizer runs keep leak detection enabled. The sandbox's initial leak check
reported thread-attachment `EPERM`; the saved binary and subsequent suites ran
outside the sandbox with `ASAN_OPTIONS=detect_leaks=1`.

## Remaining implementation

Step 3 adds saved call inputs/results, translated origins, float-query bounds,
private geometry copying and intrinsic memoization. Its oracle must additionally
compare public hit/selection/caret/paint behavior and exercise failures while
retaining runs and copying lines. Step 4 connects resource revision producers
and Yonder callers, then applies the documented hardware performance gate.
This checkpoint supplies no P5 bundle and makes no hardware performance claim.
