# F.4b native call facts — validation

Implementation: `ff9fa2f1`, based on `7f647d4e` (F.4a delivered through #570).
Prepared engine identity:
`4ac4141d80a9d89b1ea4c98cdb30a0dc0cecc351b92ba93aeb5cd6a1f6747f0d`.
Embedding wrapper ABI remains 5. This record covers the first native-facts
cut described in [IFRAME_F4_SETTINGS.md](IFRAME_F4_SETTINGS.md), not completed
browser settings attribution, a WindowProxy security gate or failure recovery.

## Final results

| Check | Result |
|---|---|
| Full runtime with target engine | 13,667 checks; zero failures; zero live allocations |
| Full runtime with fully instrumented engine | 61,015 checks; zero failures; zero live allocations |
| Focused `--native-call-contexts`, target | 324 checks; zero failures; no leaked allocations |
| Focused `--native-call-contexts`, instrumented | 324 checks; zero failures; no leaked allocations |
| Compile-failure sweeps | 40 assertions per engine; parser allocation cuts pass |
| Fatal teardown/invariant subprocesses | Expected exit 99 and full `4a534641` badge verified |
| Preparation/hash tests | Six tests pass |
| Contract headers and teardown allocation/global audit | Pass |
| Target library, runtime guest and `js` runner links | Pass; no guest execution claimed |
| Iframe tracker, stale references, local links and diff checks | Pass |

The focused fixture exercises both FATAL and RECLAIM teardown policies. Its
context oracles use independently held A/B/C pointers. Coverage includes:

- Generic, data and neutral data callbacks, direct and borrowed calls,
  Function call/apply, Reflect, spread, bound/empty-proxy calls, native
  getters/setters and constructors, including constructor refusal.
- Raw host entry without an author frame; direct/indirect eval and generated
  functions; generators, async continuation, module/top-level-await execution,
  and Promise execution-context versus callback defining-realm distinctions.
- Nested native/JS/native invocation, normal and abrupt return, GC during
  reentry, runtime cancellation, pending exception preservation, empty-output
  clearing and independent runtime isolation.

The query preserves the existing difference between generic callbacks' defining
context and data callbacks' incoming context. A Promise's execution context is
not assumed to equal its handler's defining realm. No test treats a raw native
pair as current/entry/incumbent settings or as permission to access a target.

## Commands and sanitizer qualification

From the implementing worktree:

```sh
CCACHE_DIR=/tmp/os64-ccache JS_RUNTIME_KEEP=1 ASAN_OPTIONS=detect_leaks=1 \
  bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 "$artifacts/probe" --native-call-contexts
ASAN_OPTIONS=detect_leaks=1 "$artifacts/sanitized-probe" --native-call-contexts
bash tools/test_js_contract_headers.sh
python3 -I tools/test_js_prepare.py
python3 -I tools/test_js_teardown_audit.py
CCACHE_DIR=/tmp/os64-ccache make -C userland -j8 "$(pwd)/userland/bin/js" js-runtime-test
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
bash tools/stale_refs.sh
git diff --check
```

`artifacts` is the retained directory printed by the runtime script. The target
run uses the actual target engine with an ASan/UBSan host wrapper. The fully
instrumented run adds engine ASan; the established harness excludes engine
UBSan while retaining wrapper UBSan. Leak detection stayed enabled.

The initial sandboxed run diagnosed LSan attachment `EPERM` (`errno 1`) with
`LSAN_OPTIONS=verbosity=1:log_threads=1` and `TracerPid: 0`. The existing focused
binary was rerun outside the sandbox. Final full suites, focused selectors and
mutants ran outside it with `ASAN_OPTIONS=detect_leaks=1`. No leak-disabled
result is used as acceptance evidence.

## Mutation and composition checks

Seven isolated final-source mutants were built with engine ASan at `-O0`,
linked to the matching instrumented companion objects, and run through the
focused selector with leak detection enabled. Each exited 1 with fixture
assertion failures:

| Mutant | Observed failure |
|---|---|
| Generic incoming replaced with defining context | Cross-realm call/job fact assertions |
| Data defining replaced with incoming context | Cross-realm call/job fact assertions |
| Missing data dispatcher push | Invocation/context-delivery assertions |
| Missing generic dispatcher push | Invocation/context-delivery assertions |
| Data restore clears instead of restoring enclosing pair | Nested return/exception restoration assertions |
| Generic restore clears instead of restoring enclosing pair | Nested return/exception restoration assertions |
| Missing output clear for absent invocation | Empty-scope output assertions |

F.16 snapshot patch `0023-execution-scope-snapshot.patch` at `91e87db4` and
F.4b patch 0025 apply with `patch -F0` in either order after F.4a's 0022. The
resulting sources are byte-identical:

- `quickjs.c`: `87076a0ece14d740e33988f7869acf7593db73e0a56f1dd9fb2d8279cb9713d4`
- `quickjs.h`: `ea54920d345a374081fd87df6ca82cf919a9ea33fdf62427bb76b259c836b189`

An ASan build of that combined engine passes the 324-check native-context
selector with leak detection enabled. This is focused composition evidence;
it does not claim a full combined runtime suite, enabled F.16 recovery, or
coverage of the separate failed-entry guard implementation.

## Remaining boundary

Production DOM/Yonder do not consume the query. Settings stacks, callback
capture/invocation and job attribution integration remain F.4b work. The full
browser permission/lifetime matrix remains F.4d work. This cut adds no origin
gate, frame publication API, provider detachment, context reuse or budget reset.
No full DOM/WPT/browser-corpus run, QEMU execution or P5 acceptance is claimed.
