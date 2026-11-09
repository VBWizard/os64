# Relayout 5c: architecture-review checks

These checks use the existing full-layout implementation at `4184f91f`.
They establish constraints for the proposed graft, not an implemented reuse
path. Fable's read of the design was at `972d1785`.

## A proposed block top does not bound descendant float queries

At width 800, use this fixture twice, changing the spacer height from 200px
to 100px:

```html
<!doctype html>
<style>
html, body { margin: 0; padding: 0 }
.float { float: left; width: 300px; height: 100px }
.spacer { height: 200px }
.candidate { margin: 0; padding: 0 }
.text { margin: 0; margin-top: -80px; font: 16px monospace }
</style>
<div class="float"></div><div class="spacer"></div>
<div class="candidate"><p class="text">alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega</p></div>
```

`tools/test_libflow_host.sh --cascade <fixture> 800` uses the host's deterministic
font backend. The full-layout dumps report:

| Observation | Spacer 200px | Spacer 100px |
|---|---:|---:|
| Float bottom | 100 | 100 |
| Candidate proposed top from incoming cursor and its own margin | 200 | 100 |
| Candidate/paragraph actual top | 120 | 20 |
| Line x | 0 | 300 |
| Line available width | 800 | 500 |
| Line count | 2 | 3 |
| Candidate height | 40 | 60 |

The negative child margin collapses through the candidate. The proposed top
is at the float bottom in the second case, but its actual lines intersect the
float. Replaying the old lines translated by the cursor delta would be wrong.

The proposed revised guard in [RELAYOUT_GRAFT.md](../design/pending/RELAYOUT_GRAFT.md)
therefore uses a conservative lower bound on the candidate's float-sensitive
query positions, not just the root's proposed top. Coordinate conversion also
matters: `line_room` and `avoid_floats` convert through `Space.rel_y`, while the
space's stored bottoms are in unmoved coordinates. An ordinary candidate can
live below a relatively positioned ancestor even though positioned descendants
are excluded from its graft.

Both fixture executions passed with ASan, UBSan and LeakSanitizer enabled.
The sandbox run's LeakSanitizer diagnostic reported thread-attachment EPERM;
the existing binary was rerun outside the sandbox with `detect_leaks=1` and
no sanitizer findings. Source fixtures, original dumps and the executable are
retained with the local proof artifacts. No production source was modified.

## The original article is a full-fallback workload

The frozen streamed article has SHA-256
`b56f866156c264b6b085c83535637d32022783202a8f283f1e35c484500700fc`.
Its CSS contains whole-tree selector dependencies, including `:has()` and
`:dir()`. `garb_cascade_update` refuses that dependency class, creating a new
cascade generation on the fallback path. `f_style_build_reusing` rejects its
previous source on generation mismatch. Yonder's `measured_fallback` also
clears `restyle_from` and releases the old hidden measurement before replacement.

The proposed 5c contract retains those constraints. Refining the float predicate
cannot make this unchanged fixture report geometry reuse. Keep it as the
broad-fallback/no-regression gate, and use a separately identified positive
fixture to prove reuse below floats. Requiring positive reuse from the original
article instead would need an explicit revision to the generation and hidden
measurement lifetime contracts, with its own memory/performance evaluation.
