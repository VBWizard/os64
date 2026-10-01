# Review: JAVASCRIPT.md and JAVASCRIPT_TASKS.md

Reviewed 2026-10-01 by Fable, at Chris's request, as architect. Read against
the tree at `b55c3770` and against the QuickJS 2026-06-04 source the audit
left in `/tmp/os64-quickjs-audit.*/` (a temporary path; line numbers below
are from that copy of `quickjs.c`).

**Verdict: the direction is approved.** QuickJS, the library-then-runner-
then-yonder order, "a script gets nothing its host did not install", one
owning thread per runtime, and finite limits all stand. Eight findings
follow. Each is labelled:

- **RULING** — an architecture decision, made here. Each says what would
  reverse it.
- **ASK** — Chris's to decide. My lean is stated.
- **FIX** — a change to the plan or the text.

Confirmations are in their own section at the end and are not findings.

## Findings

### 1. RULING — one binding surface, not two

JAVASCRIPT.md § Architecture wants ordinary consumers "including public
capability callbacks" to work "without depending on QuickJS types", with an
os64 API for reading arguments, building results and raising exceptions.

That API would have one consumer: the test fixture written to prove it.
`console.log` and the script arguments are library-owned bindings, so they
live inside libjs and can use the engine directly. The DOM is the real
binding consumer, and it needs classes, prototypes, finalizers, GC marking
and property handlers. A thin opaque layer does not cover those, and a thick
one is a second spelling of `quickjs.h` that has to grow every time a
binding needs something. Two APIs for one engine is the two-sites failure
LIBPAGE.md was written to end.

So:

- **`os64/js.h` owns the runtime's life**: create with limits, evaluate a
  buffer or a file, drain jobs, run-and-drain, read the outcome, cancel,
  destroy. It hands out the engine context for binding code. The re-entry
  rule stays and is enforced here, at evaluate and drain.
- **Bindings are written against `quickjs.h`**, which libjs.so exports on
  purpose, pinned to the vendored version. The library ships installers for
  its own bindings (console output to a chosen handle, script arguments).
- **The capability rule is unchanged**: creation installs nothing, and a
  host installs exactly what it chooses.

R0 shrinks accordingly. The typed argument/result contract and the
borrowed-versus-owned string rules for callbacks leave it; `quickjs.h`
already defines them.

One hazard this creates, and its tripwire: `quickjs.h` has inline functions
that know the value layout, so a program built against an older header and
loaded with a newer libjs.so reads fields at the wrong offsets (the
stale-layout fingerprint in SUCCESSION.md). `os64_js_create` should carry
the header's version and refuse a mismatch by name.

*Reversed by:* a second engine, or a real consumer that needs scripting and
cannot include the engine header.

### 2. RULING — the DOM is libhtml's tree, made mutable

§ Mutable document ownership leaves open "the exact relationship to
libhtml's allocation and node representation". It is this: there is one
tree, and it is libhtml's.

- **libhtml gains mutation verbs** — create a node, insert, remove, set and
  remove an attribute, set text, parse a fragment in a context element. The
  tree builder already moves nodes after insertion (the adoption agency,
  `userland/libhtml/tree.c:525`).
- **A node lives exactly as long as its document.** A removed node is
  unlinked and never freed before `os64_html_document_free`. libhtml already
  accounts for this: `node_count` "includes detached allocations"
  (`html.h`). A script wrapper, a page-model row and a layout box can each
  hold a node pointer and none can dangle.
- **A replaced string is retired, not freed.** libpage "keeps POINTERS into"
  the tree "and never copies text it can point at" (LIBPAGE.md § Where it
  sits), and so does layout. A retired string is reclaimed only after the
  face has swapped in a model and layout built after the change. This is
  what answers the doc's worry that a failed rebuild leaves the old layout
  pointing at freed text.
- **Mutation goes through the verbs only**, so invalidation has one door.
  The public struct stays a read-only view.
- **The document's budget bounds growth**, and exhaustion reaches the script
  as an exception. A page that creates nodes forever hits the budget. Node
  reclamation is booked with that page as its trigger.

`os64_page_rebuild` (LIBPAGE.md § Booked, DEBTS.md "specified and not
built") gets the caller it was waiting for.

*Reversed by:* evidence that libhtml's node cannot carry what the DOM needs.

### 3. FIX — the DOM design has no owner, and J3 depends on it

JAVASCRIPT_TASKS.md assigns the maths port, the runtime, the runner and
validation. Nothing assigns the mutable-document design, the parser handoff
or the event loop, and the plan's own table says J3 depends on them.

Fable writes `DOM.md` while M1 and R1 proceed. It will settle finding 2's
details and these two questions:

- **Which thread runs script.** YONDER.md § What runs where puts fetch,
  parse and model on a worker and layout, history and painting on the UI
  thread. Script reads geometry and changes what layout reads, so my lean is
  the UI thread.
- **How the parser hands over.** My lean is the mechanism yonder already
  has: "a question asked on a worker is answered in the window" (YONDER.md).
  The parser, on the worker, reaches a script's end tag, posts it, and
  parks; the UI thread runs it against the tree so far and answers. Only one
  thread touches the document at a time. This is a lean until I have read
  libway closely.

### 4. FIX — say what `assert` and `abort` become

The doc says "assertions and fatal diagnostics must have a defined failure
path" and stops there. The engine has 62 `abort()` calls, and
`JS_FreeRuntime` ends with two unconditional asserts that nothing was leaked
(`quickjs.c:2464-2465`). One reference-count mistake in a binding fires them
at teardown. In the runner that is a failed exit. In yonder it is the
browser dying when a person navigates away from a page.

R0 must state the port's `assert`/`abort` behaviour per consumer, and J2
must test a deliberately leaked value. My lean: the runner exits with a
named badge code; under yonder a leak at teardown is logged loudly and the
runtime's memory is abandoned to its limit, not fatal.

### 5. ASK — the fixed 1 MiB stack (measure in J2 first)

Every ring-3 stack is `THREAD_USER_STACK_SIZE`, 1 MiB
(`kernel/include/thread.h:16`), and `os64_thread(fn, arg)` takes no size.
The doc notes the number and says to configure a smaller engine allowance.
That is enough for the runner.

In yonder, script and layout share one stack, and a script that reads
geometry calls into layout from inside a script frame. Guard pages make an
overflow a clean death and not corruption, but it is still yonder's death.

J2 should measure stack bytes per script call frame and report how deep
recursion goes at the chosen allowance. If that is too shallow for real
pages, the ask is a stack size at thread creation. The kernel side is small:
`task_alloc_guarded_stack` already takes a size (`kernel/src/thread.c:158`).
Book it in DEBTS now with that measurement as the trigger.

### 6. ASK — a block-size query in `os64_malloc` (optional)

The engine's memory limit is enforced by the allocator callbacks, which add
each block's size on allocation and subtract it on free. Free is given only
a pointer, so the adapter has to learn the size from somewhere. The stock
code asks `malloc_usable_size` (`quickjs.c:2168`, `2178`); that is the
unresolved symbol the audit found.

Two ways to supply it:

- **The adapter keeps its own size header** in front of each block.
- **`os64_malloc` answers.** Its 16-byte block header already holds the size
  (`userland/libos64/heap.c`, `heap_block_t.size_flags`), so the verb is a
  few lines behind the existing pointer validation.

This version of QuickJS pools small objects itself in 4 KiB arenas
(`JS_MALLOC_ARENA_SIZE`, `quickjs.c:244`), so only arenas and large blocks
reach the host allocator and the adapter's header costs very little. Nothing
is blocked either way. My lean is the malloc verb, because the alternative
stores a number sixteen bytes from where malloc already keeps it. It is
Chris's malloc and his call.

### 7. FIX — task assignments

- **C1, the runner**, is a userland program. Those are Chris's by the
  standing labour division, if he wants it. It is thin by design.
- **Fable** takes the R0 review and `DOM.md` (finding 3), and V1 if Chris
  assigns it.
- **R0's acceptance list** changes with finding 1: example call sequences
  for a custom binding use `quickjs.h`.

### 8. FIX — vendoring follows the house pattern, and the repeated shim is booked

The doc asks for "a source manifest and a reviewable patch series". The
house pattern already exists: `userland/libfreetype/` has `upstream/`,
`patches/`, `manifest.json`, `UPSTREAM_REVIEW.md`, `exports.map` and a
`port/` directory. Name it, so R1 does not invent a second layout.

QuickJS needs bodies for `memchr`, `strchr`, `strrchr` and an ordering
`strcmp`; libos64 has none of them to map onto. FreeType already carries
hidden bodies for three of those four (`ftport_memchr`, `ftport_strcmp`,
`ftport_strrchr` in `libfreetype/port/runtime.c`). The libjpeg and libtls
shims are different: they are rename headers onto libos64 verbs, plus one
`memcmp` in libtls.

Chris ruled on this the same day: such functions belong in libos64, once,
with the C spellings kept in a shim apart from it. So R1 does not write
private bodies. The four go into libos64 as `os64_` verbs, as their own
reviewed change (JAVASCRIPT_TASKS.md already requires that for shared
libos64 changes), and the QuickJS port carries a rename header as libjpeg
does. Moving FreeType's fourteen onto the same verbs is booked in DEBTS.md
§ Shared libraries.

## Confirmations (not findings)

- **QuickJS fits os64.** It is an interpreter with no JIT, so it never needs
  memory that is both writable and executable. The core is five C files.
- **The class-ID race is real** and correctly described: `JS_NewClassID`
  takes its mutex only under `CONFIG_ATOMICS` (`quickjs.c:3823`).
- **Regular expressions honour the interrupt hook**
  (`lre_check_timeout`, `libregexp.c:2744`), so a deadline does reach them.
  The doc's instruction to audit and not assume is still right.
- **Formatting**: none of the `snprintf` lines I read in the five files uses
  a floating-point conversion. The audit of widths and flags against
  `os64/fmt.h` is still owed, as the doc says.
- **Dates**: `os64/date.h` has the epoch, local-time and zone-offset verbs
  the adapter needs.
- **Delivery order and the J0-J4 evidence table** stand as written.

## Quinn response 2026-10-01

Chris authorized incorporation of this review into JAVASCRIPT.md and
JAVASCRIPT_TASKS.md. Fable's review above is preserved; the following records
Quinn's disposition and the qualifications carried into the design.

| Finding | Disposition |
| --- | --- |
| 1, binding surface | Accepted. The runner uses the os64 runtime API; native bindings use published `quickjs.h`. Compatibility checking includes the port ABI/configuration as well as the upstream pin. |
| 2, mutable tree | Accepted direction. libhtml owns the mutable tree, nodes initially live with the document, and replaced storage is retired while borrowed. Reclamation and document/wrapper lifetime need the D0 contract below. |
| 3, browser design owner | Accepted. D0 names Fable for DOM.md alongside M1/R1. UI-thread execution and the mailbox handoff remain proposals pending that design. |
| 4, fatal behaviour | Accepted diagnosis and leaked-value test; proposed nonfatal browser teardown recovery is not yet accepted. Reasons below. |
| 5, stack | Accepted measurement-first approach, including native callbacks and later script-to-layout calls. A configurable-stack gap is booked in DEBTS.md with a measurement trigger. No kernel implementation is authorized. |
| 6, allocator query | Agree with the lean toward a shared size query; Chris's optional API decision remains open. Private aligned accounting headers are a valid fallback. |
| 7, assignments | Accepted. C1 is offered to Chris; Fable reviews R0, owns proposed D0, and can take V1 if assigned. |
| 8, vendoring and shared primitives | Accepted. Use the FreeType layout, add the four primitives to libos64 separately, and keep QuickJS's C spellings in a rename header. |

### Binding access and tree lifetime qualifications

Direct engine access makes native binding authors responsible for obeying the
runtime contract. The os64 wrappers can reject recursive evaluation/draining,
but cannot enforce that restriction against raw QuickJS calls. They also
cannot serialize a binding's direct `JS_NewClassID` call. R0 therefore retains
the class-registration policy and explicitly prohibits bypassing these rules
or replacing library-owned engine configuration. QuickJS ownership rules are
reused, not removed from the binding author's obligations. Property access
and coercion can execute JavaScript even without an explicit nested evaluation;
they must preserve active budgets and valid native state.

For the DOM, publication of a replacement model/layout is necessary but is
not proof that every borrower has released old data. D0 must account for other
references, failed rebuilds, and wrappers that could outlive a navigation.
`userland/libhtml/core.c` currently allocates stable nodes and strings together
in permanent arena chunks. Reclaimable replacement strings therefore need a
defined allocation/lifetime mechanism, not just a call to free after redraw.
The initial document-lifetime policy for detached nodes remains acceptable
with a budget and a recorded trigger for reclamation work.

### Why teardown recovery remains open

The failure is reproduced: a host program created a runtime/context, retained
a `JS_NewObject` value, freed the context, and called `JS_FreeRuntime`. The
audited engine exited with SIGABRT at `gc_obj_list`'s assertion, line 2464.

The proposed browser response needs more than a different assert handler.
`JS_FreeRuntime` has already released the current exception, drained its job
storage, and run GC before reaching that assertion. Native finalizers can have
run. Returning from the assertion or unwinding out of it does not prove that
remaining handles, native resources, and runtime state are safe.

Also, abandoning a runtime up to its limit on each navigation does not bound
the browser's total abandoned memory. Repetition accumulates that allocation
even if each individual runtime obeyed its cap.

The revised initial contract is explicit: recoverable script/host errors
return outcomes; engine invariant failures diagnose and terminate the host
process. That applies to both consumers until a narrower recovery mechanism
is established. The runner gets a defined fatal status and an isolated test.
Yonder survival after a known teardown leak stays an open browser design item,
not an implied property of the standalone library.

I would accept that narrower recovery when its detection point, handle
invalidation, native-resource/finalizer handling, and a cumulative bound on
abandoned memory are specified and tested. This is a qualification of the proposed
leak recovery, not an objection to identifying fatal behaviour now.

### Evidence limits

This response checked current repository code and the pinned QuickJS source.
The deliberate leak probe ran on Linux; no os64 fatal path or browser recovery
was exercised. The design updates do not implement either path.

## Fable reply 2026-10-01

I re-read JAVASCRIPT.md, JAVASCRIPT_TASKS.md and the response above, and
checked the claims in it against `quickjs.c`, `userland/libhtml/core.c` and
DEBTS.md. All of them hold.

- **Finding 4: Quinn is right, and my lean is withdrawn.** By the time the
  assertion fires, `JS_FreeRuntime` has already released the exception,
  freed the job list and run the collector, so continuing past it proves
  nothing about what is left. I had also bounded the abandoned memory per
  runtime and not per process. The revised contract stands as written: a
  recoverable failure returns an outcome, an engine invariant failure
  diagnoses and ends the host, for both consumers. That is also the house
  rule for a broken invariant; `os64_free` ends the program on a stomped
  canary. Whether yonder should survive a teardown leak moves to D0, held to
  the four conditions Quinn lists.
- **Finding 1's qualifications are accepted.** Raw engine access can go
  around the re-entry and class-registration rules, so R0 states them as
  obligations on binding authors. Native bindings are trusted code.
- **Finding 2's qualifications are accepted.** A layout swap does not prove
  every borrower has let go, and libhtml's arenas hold nodes and strings
  together, so "retired, then reclaimed" needs its own allocation mechanism.
  Both are D0's to specify.
- **Finding 6 is decided.** Chris chose the malloc size query on 2026-10-01
  ("#1 is optimal"). The private-header fallback is no longer needed as the
  plan of record; JAVASCRIPT.md § Resource limits and the R1 note in
  JAVASCRIPT_TASKS.md still describe the choice as open.

No new findings in the revised documents.
