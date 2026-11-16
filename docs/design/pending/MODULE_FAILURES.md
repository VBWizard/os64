# MODULE_FAILURES.md — a module's late failure, reported

*Opus 5.5, 2026-11-07; Chris read it and took every lean the same day
(the intrinsic `then`, and the FAILED line on the task that surfaced the
failure, § 6). Found while replaying a social network's logged-out page
(P5 record, counts only): its entry module's top-level `await` rejected
with a `TypeError`, and nothing anywhere said so: no status line, no FAILED
line in the record. The page stayed blank and the person had nothing to go
on. Size S. § 3 is the design as built.*

## 1. What happens today, measured

Four fixture pages through yonder's window (the scripted-page host, a
scratch driver), each a module that throws:

| The module | Reported? |
|---|---|
| `throw Error()` at its top level, script-inserted | yes: `FAILED script <url>: …, at 1:12` |
| `await 0; throw Error()`, parser-inserted `<script type=module src>` | no |
| `await 0; throw Error()`, script-inserted, no imports | no |
| `import './dep.js'; await 0; throw Error()`, script-inserted | no |

So it is not the page, the loader or the graph: **any module whose
evaluation rejects after a top-level `await` is never reported.** A
synchronous throw is reported because `os64_js_module_run`'s own outcome
carries it.

**Where the late one goes** (traced with prints, then reverted):

1. The engine rejects the cycle root's evaluation promise
   (`js_async_module_execution_rejected`), with no handler on it, so it
   calls the host's rejection tracker with `handled = false`.
2. libjs's tracker (`runtime.c` `rejection`) finds the module's handle
   (`module_of`), and `module_failed` keeps the failure on the handle and
   hands the error to the host's exception hook (`caught`).
3. libdom's hook (`report.c` `host_exception`) puts it on the binding's
   pending errors (`pending_push`).
4. At the next settle, `d_errors_fire` fires a window `error` event for it
   **if something listens for `error`**, and otherwise pops it and drops
   it. Either way nothing folds it into an outcome.

yonder's record and its status line are both fed from task outcomes, so a
failure that never reaches an outcome is never seen. The handle does keep
it (`os64_js_module_state` would answer REJECTED), but nothing asks.

A neighbour already in DEBTS (ES modules, "A module's late failure goes
unheard when an `import()` of it is waiting") is the case where step 1
never happens at all: an `import()` of the module has put a handler on the
evaluation promise, so the tracker is not called.

## 2. What HTML does

A module script whose evaluation promise rejects has its error reported
("report an exception", HTML § 8.1.4.6): the window's `error` event fires,
and if it is not cancelled the error goes to the developer console. It is
the same report a classic script's uncaught exception gets, and yonder
already gives that one: a FAILED line in the record and a status line.

## 3. The design

**libjs watches each module's evaluation promise itself, and folds a late
failure into the outcome of the call that surfaced it.**

- `os64_js_module_run` attaches a reaction to the handle's evaluation
  promise when it is still pending (`module_watch`, a native onRejected
  whose data is the promise, so a handle released since finds nothing),
  so libjs hears the rejection whether or not anything else handled the
  promise. Not through the page's `Promise.prototype.then`, which a page
  may have replaced: the intrinsic `then`, read at construction before any
  script ran (`intrinsic_then`; a runtime that cannot read it is not
  made, which keeps construction's allocation sweep exact). That also
  covers the DEBTS neighbour, where an `import()` made the promise handled
  and the tracker stayed silent. `then` asks the promise's `constructor`
  for its species, which a page can replace, and so could run the page's
  code inside the run or throw and leave the module unwatched (the cold
  read's second finding): the evaluation's promise is the engine's own and
  no script holds it, so it is given an own `constructor` of undefined
  first, and `then` takes the realm's Promise without asking the page. A
  pending evaluation is therefore always watched (a refused allocation
  ends the runtime, as one anywhere does), and the tracker's module case
  has nothing left to catch, and goes.
- The reaction runs as a job, inside whatever libjs call is draining jobs
  at the time (a task's end, a checkpoint, a dispatch). It calls
  `module_failed` as today (the failure kept on the handle, the error
  handed to the host's hook, so libdom still fires the window `error`
  event with its listeners), and marks the failure **owed**.
- The call's exit (`leave`) folds the first owed failure into that call's
  outcome when the outcome is still OK, with the module's name as its
  source and the error's message and stack (`module_owed_fold`), as
  `unhandled` in libdom already folds an unhandled rejection into the task
  it surfaced in. Only a call that ran jobs or checkpointed folds: a
  task's begin never does, since its outcome decides whether the task runs
  at all. A second owed failure waits for the next such call, in the order
  they failed (each owed failure is numbered), so each is in exactly one
  libjs outcome. (The draft sent the second through libdom's
  `options.failed`; libjs cannot reach it, and carrying it keeps one
  path.) What a host keeps of them is its own policy: libdom and yonder
  keep one failure a task, the first, as they do for a task's exceptions,
  so a second late failure in a task that already failed is heard by the
  window's error event and reaches no FAILED line (the cold read's first
  finding; a DEBTS row, beside the same gap for a task's second
  exception).
- A module that rejects synchronously is unchanged: `os64_js_module_run`
  sees the promise already rejected and reports it in its own outcome, as
  now, and marks nothing owed.

**Why libjs and not libdom:** libdom's pending list cannot tell a late
module failure from the errors whose failure is already in an outcome
(every script run's exception passes through the same hook), so folding
there would report those twice. libjs knows which one has no call of its
own.

**What the record will say:** `FAILED script <address>: not a function,
at <file>:<line>:<column>` (the message and the place the module failure
has always carried in `module_state`), on the task during which it
surfaced, with the status line beside it, as for any script. The address
is that task's script when it has one (the module's own, when it fails in
its own task; another script's, when it surfaces in that one), and the
module's when the task has no script (an event, a timer); the place in the
message names the module either way. The replay of the social network's
page says it under the entry module's address, at the `Intl.Segmenter`
construction.

## 4. Proof

- The scripted-page host, the four fixtures above: each late failure is a
  FAILED line naming its module, once; a window `error` listener hears it
  once; with an `import()` of the module waiting (the DEBTS neighbour's
  case), the same.
- A module that resolves, and one that rejects synchronously, report as
  they do today (no owed failure, no double line).
- The libjs host suite: a late failure is the outcome of the call that
  surfaced it and the next call's is its own; with an `import()` waiting;
  two in one call, the first then the second. And the watch sweep: every
  allocation of a watched module's run and of the task that fails it,
  refused in turn, either leaves the failure reported or ends the runtime,
  and destroy reclaims all (3 positions on the target core, 125 on the
  instrumented engine).
- The social network page's replay: its record names the `TypeError` at
  the line that constructs the missing `Intl.Segmenter` (SEGMENTER.md).

## 5. DEBTS

Pays ES modules' "A module's late failure goes unheard when an `import()`
of it is waiting". Books nothing new unless the build finds it.

## 6. The question, as Chris answered it

Answered: the FAILED line lands on the task that surfaced the failure.

The window `error` event for a late module failure fires in whatever task
surfaced it, which is HTML's answer too (the report happens when the
promise rejects). The record's FAILED line lands on that task's line, so
it can sit under a timer's or an event's task rather than the module's own
"script" line. I lean to that: it is where it happened, and the line names
the module. The alternative is a line of its own, at the cost of a new
record line kind.
