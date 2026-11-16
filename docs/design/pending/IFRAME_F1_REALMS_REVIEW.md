# F.1 realms brief: design read

Fable, 2026-11-10. Read: `IFRAME_F1_REALMS.md` at `b81fe744`, against
IFRAME.md § Realms / § Retained ownership / § The cut, and against
`userland/libjs/runtime/runtime.c` and the pinned `upstream/quickjs.c`.
Nothing was built or run for this read.

**Verdict: approved, with one correction to make before code (1) and four
asks (2 to 5).** The ownership split is right, the scope line against F.2
to F.4 is right, and it is one reviewable cut once ask 3 trims it.

## What I checked and found true

- The three context-opaque readers are `module_rejected`, `output_call`
  and `console_call`, and nothing outside runtime.c reads either slot.
- The engine keeps `loaded_modules` per context, so "same URL in A and B
  is two modules" needs no engine patch.
- The loader callbacks receive the context of the importing code (a
  bytecode function runs in its own realm), so "resolve the provider from
  the callback's context" routes a borrowed function's `import()` to the
  realm that wrote it, which is what HTML wants.
- `module_watch` reads `runtime->context` and `runtime->promise_then`;
  the brief's per-realm captured `then` covers it.
- Refusing to select a realm by swapping `runtime->context` is the most
  important sentence in the brief. Keep it in the as-built.
- Deferring retire to F.3, and saying plainly that F.1 keeps every
  context until the runtime dies, matches the tracker and IFRAME.md.

## Findings

**1. Rejection lookup must stay keyed by the promise alone, not by
(reporting realm, promise). Correction before code.**

The brief scopes lookup "to that reporting realm and promise identity".
The engine calls the tracker with the context of whatever native function
is running: for a rejection that is the realm of the promise's resolving
function, and for a handler it is the realm of the `then` that attached
it (`perform_promise_then`, quickjs.c near 53997). Those differ in the
ordinary case, not an exotic one: a promise made in B and awaited from A
(`await child.fetchThing()`) reports "rejected" under B and "handled"
under A. With the scoped lookup, the handled notice looks in A, finds
nothing, and B's record stays: B's host takes it and fires
`unhandledrejection` for a promise that has a handler. The same happens
with a router that answers differently at the two moments, which HTML's
own rule (running script, at each moment) does.

HTML has the same per-global lists, and is saved by its notify step,
which re-reads the promise's [[PromiseIsHandled]] before firing. The
engine keeps that bit (`is_handled`) but does not expose it, so we
cannot copy that step without a patch.

The fix needs none. A promise's address is unique in the runtime, and
the table is keyed by it today. Keep that: one table, the record carries
its reporting realm, and a handled notice removes the record whatever
realm the notice is attributed to. Per-realm order for taking is a walk
of the one list filtered by realm, or a per-realm link in the record.
Only the handled-AFTER-take notice needs the policy-selected realm, as
the brief already says.

Proof to add to the "Rejection routing" row: reject in B, attach the
handler from A (both `p.then` called from A's code and `await`), then
take from B: nothing to take, no report.

**2. A realm-count ceiling, refused without latching.** The brief has no
bound on realms; the only refusal is the engine running out of memory
inside `JS_NewContext`, which is sticky and ends the root document with
it. Give `os64_js_realm_create` a count ceiling checked before any
allocation, answered with an ordinary non-sticky status. IFRAME.md's 64
frames is the figure to start from; F.3 will want the same number to
bound retired realms that linger.

**3. Trim the new entry points to the ones a later slice will call.**
Twenty-odd `os64_js_realm_*` counterparts is the house's "implement it
when something asks" rule bent. `realm_run_file` and `realm_install_args`
are the `js` command's; no frame will call them. I would also question
`realm_eval` and `realm_run_bytecode` unless F.2's script host needs
them. For each one kept, the brief should name the slice that calls it;
the rest wait.

**4. Mutants for the routing, once at the end.** The failure this cut
exists to prevent is one site that still reads the primary. No
single-realm test can see that, and a two-realm test sees it only if it
happens to cross that site. One mutant per routed site (select the
primary instead of the realm in: loader lookup, module list, watcher's
`then`, output handle, console depth, script host for exceptions, late
module failure, rejection record's realm) proves the matrix can fail.
Twelve workers, `tools/mutants_parallel.py`.

**5. Say why the descriptors are a separate allocation, or simplify.**
The `os64_malloc` descriptors bring a second accounting path
(`realm_storage_bytes`, lowering `allocation_limit`, rollback, a new
public query, a teardown order). With a ceiling from ask 2, a fixed
table of descriptors inside the wrapper's own allocation, which is
already reserved against the limit, removes all of that. If Quinn has a
reason the table cannot be fixed, the brief should state it; otherwise
my lean is the fixed table. Her call.

## Smaller, text only

- "Setup traps" on a freshly made realm: say who could have installed a
  trap there (the host binding, between create and install), or the
  guard reads as defending against nothing.
- The router is F.4's seam built one slice early, with only the harness
  calling it. That is fine as long as the as-built says the record's
  shape may change when F.4 writes the real selector.

## Answers to what the brief asked of this read

- Service and turn ownership: agreed as tabled.
- Nested setup, three cases: agreed. Case 3 not touching the outer
  outcome, stack origin or guard is right.
- Routing-hook boundary with F.4: agreed, subject to finding 1.
- Rejection lifetime: finding 1.
- Both teardown paths: agreed; ask 5 may shorten one.
- One cut: yes, after ask 3.
