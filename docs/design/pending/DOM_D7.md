# DOM_D7.md — the loop: tasks, timers, events and the order scripts run in

*Written 2026-10-05 by Fable, as the builder's brief for slice D7 of
[DOM.md](DOM.md) § Slices, to be built by Opus with Fable reviewing. Read
against `fable/dom-d4` at `f4be2838` (D4 built, PR #223 in review): D7 is
stacked on D4 and assumes it. Names of functions that do not exist yet are
working names; the code settles their spelling, and this file's "as built"
section goes into DOM.md with the PR. Where this file and DOM.md disagree,
DOM.md's rules win and the disagreement is a finding against this file.*

## What D7 is, and is not

D4 put the parser on the window's thread and made it stop at every
script's end tag; the stream resumes it at once. D7 replaces that resume
with a run, and builds everything that runs inside the loop the run
belongs to: the task a script is, the microtask checkpoint after it, the
timers a script sets, the events a person's input becomes, the handler
attributes the old web is written in, the order the standard runs scripts
in (inline, `src`, `defer`, `async`, inserted), the two lifecycle events
(`DOMContentLoaded`, `load`), a script's way of asking to navigate, and
the policy for a script that runs too long. After D7, J4's evidence can be
gathered, and every later slice (D9 `document.write`, D10 geometry, D11
the surface) runs inside this loop.

**What D7 does not do**, each deliberately and each with its own row:

- **`document.write`** is D9: it is legal only while the parser is stopped
  at a script, which D7 creates, and it re-enters the parser, which is its
  own proof. In D7, `document.write` and `writeln` exist and THROW
  `InvalidStateError` with the message "document.write is not supported
  yet", so a page that calls it fails loudly at the call and not
  mysteriously later.
- **Geometry** is D10. `offsetWidth` and its kind stay absent (undefined),
  as D5 left them.
- **The reporting destroy** is D8 (DOM.md ruling 8). D7 runs on the fatal
  destroy, as D5b did: the leak count is the engine's own assertion not
  firing after the registries are drained.
- **A modal `alert`/`confirm`/`prompt`** stays ruling 2's: the question
  bar, no, nothing.
- **Progressive display of the arriving page** stays D4's booked row. A
  script running mid-parse changes a tree nobody is drawing yet; the
  rendering step runs only for the page on screen.
- **`fetch`, `XMLHttpRequest`, `document.cookie`, storage,
  `requestAnimationFrame`, `window.open`, frames**: the modern-web
  campaign, its own design.
- **`unload` and `beforeunload`**: never (DOM.md § Wrappers, teardown).

## The shape, in one paragraph

yonder's loop stays the loop it is: wait for an event, dispatch the batch,
lay out if owed, then one turn of page work, paint, and ring its own bell
if work remains. A TURN runs at most ONE task per page: for the page
arriving, one slice of parsing or the script the parser is stopped at; for
the page on screen, one queued script or one due timer. An input event is
a task of its own, dispatched where the GUI event is handled today. Every
task that enters the engine is bracketed by `task_begin` and `task_end`
(new libjs entries), which arm one deadline for the whole task, and every
callback the task calls is followed by a microtask checkpoint under that
same deadline. The JavaScript values the host holds live in libdom's three
registries (wrappers, listener lists, timers), each with a drain; yonder
holds nodes and ids, never values. A task that overruns its deadline
retires the runtime, and the page goes on without script.

## The libjs extensions D7 builds

DOM.md § What this asks of libjs reserved three extensions by name
(CONTRACT.md § Browser extensions reserved for later work), "each built
with the slice that needs it". D7 needs the first two, plus one setter the
Settings control needs. They are built in `userland/libjs/runtime/runtime.c`
under CONTRACT.md's rules, declared in `js_engine.h` (they take engine
values, which `js.h` does not know), and the contract's section is
rewritten from "reserved" to "built" with the rules below.

| Entry | What it does |
|---|---|
| `os64_js_task_begin(rt, abi, out)` | Opens a task: arms the execution deadline (`now + execution_ms`), zeroes the job count, sets `turn`. Refused BUSY if a task is open or the previous turn left runnable jobs (the same rule `eval` has). A failed runtime answers FAILED_RUNTIME. |
| `os64_js_call(rt, abi, fn, this, argc, argv, out)` | Calls a function value inside the open task, under its deadline, with the same outcomes as `eval`: OK, EXCEPTION (diagnostic filled, runtime still usable), or a sticky LIMIT/CANCELLED/HOST_FAILURE. Refused if no task is open. The host owns `fn`, `this` and `argv`; the return value is freed by the call (D7 has no caller that reads one except the handler-attribute `false` rule, which the call reports in a `bool *returned_false` out parameter rather than handing a value back). Re-entry from inside a callback is refused, as `eval` is. |
| `os64_js_checkpoint(rt, abi, out)` | A microtask checkpoint inside the open task: drains runnable jobs to exhaustion under the task's deadline and job cap, and judges unhandled rejections now, releasing their bookkeeping, as the turn's end does today. Outcome OK, EXCEPTION, UNHANDLED_REJECTION, or sticky. |
| `os64_js_task_end(rt, abi, out)` | A final checkpoint, then closes the task. After it, `eval`/`run` are accepted again. |
| `os64_js_set_execution_ms(rt, ms, out)` | Owner thread, outside a task: replaces the execution limit for tasks opened from now on. BAD_ARGUMENT for zero or unrepresentable, as `create` refuses. The task that is open keeps the deadline it was armed with. |

`os64_js_run` keeps its meaning and is unchanged: it is `task_begin`, eval,
`task_end` in one call, and a script element runs through it. The
interrupt handler already bounds `JS_Call` by `runtime->deadline`, so a
call under an open task needs no new mechanism, only the arming that the
contract says a raw `JS_Call` lacks.

**Rules the contract gains, in words the reviewer can hold the code to:**
one task open at a time; a task's deadline and job cap continue across its
checkpoints; a checkpoint judges rejections without opening a fresh
budget; a sticky failure inside any call ends the task (`task_end` still
closes it and reports the sticky status); nothing is accepted between a
sticky failure and destroy except drain-and-destroy. The host harness
`tools/test_js_runtime_host.c` gains a case per rule, including a callback
that overruns (LIMIT/EXECUTION reported from `call`, then from every later
entry), a callback that throws (EXCEPTION, the next call in the same task
still runs), a checkpoint that reports an unhandled rejection between two
calls, `set_execution_ms` taking effect at the next `task_begin` and not
the open one, and `call` refused outside a task.

## The listener registry and dispatch, in libdom

libdom gains the EventTarget half of the DOM, because the function values
belong in a registry the binding drains (DOM.md § Wrappers: "the listener
lists" are the second of the three registries, and "a value stored
anywhere else is a finding").

**Targets.** Every node wrapper, `document` and `window` are targets. A
listener list hangs off the wrapper record (`DValue`): a node that has
never been wrapped has no listeners, and a handler CONTENT attribute
(`onclick="..."`) on an unwrapped node is found at dispatch by reading the
attribute and wrapping the node then. `window`'s and `document`'s lists
live on their own records.

**The surface.** `addEventListener(type, fn, capture|{capture, once})`,
`removeEventListener(type, fn, capture|{capture})`, `dispatchEvent(ev)`;
`new Event(type, {bubbles, cancelable})`; on an event: `type`, `target`,
`currentTarget`, `eventPhase`, `bubbles`, `cancelable`,
`defaultPrevented`, `timeStamp`, `preventDefault()`, `stopPropagation()`,
`stopImmediatePropagation()`, `returnValue` (the old web's spelling of
`!defaultPrevented`, writable), `srcElement` (= `target`), `cancelBubble`
(writable, = stopPropagation). Mouse events carry `clientX/Y`, `screenX/Y`,
`button`, `which`, `shiftKey/ctrlKey/altKey`, `relatedTarget` for
over/out; key events carry `key`, `keyCode`, `which`, `charCode` (the
old web reads `keyCode`; `key` is the modern name for the same byte),
and the modifier flags. `passive` is accepted and ignored. `useCapture`
as a bare boolean is the old spelling and must work.

**Handler attributes and properties, the old web's own idiom.** For every
event type D7 dispatches, each element, `document` and `window` has an
`on<type>` property. Setting it to a function installs the one handler
(replacing the last); setting it to null removes it; reading it answers
the function or null. A CONTENT attribute `on<type>="BODY"` compiles
lazily, at the first dispatch that reaches it, to a function whose
`this` is the element and whose scope chain is the standard's three:
`with(document) with(form) with(element)` around `BODY`, with `event` as
its one parameter — the standard's "internal raw uncompiled handler"
(HTML, "event handler content attributes"). A compile error is reported as the script error it is
(the status line, like any script exception) and the attribute is treated
as absent until it changes. `setAttribute`/`removeAttribute` of an
`on<type>` attribute invalidates the compiled handler (the verb is
libdom's, so it knows). **`<body onload>`, `<body onunload>` and the
body's other window-event attributes install on WINDOW**, as the standard maps them (HTML's "window-reflecting body element
event handler set"): `<body onload="init()">` is how the 1998 web starts. A handler
(attribute or property) that returns `false` cancels the event's default
action (HTML's "event handler processing algorithm"; the `onerror`
inversion does not apply because no `error` event is dispatched here).
The old web's `return true` after `window.status=` was Netscape's way of
keeping the status text and cancels nothing.

**Dispatch** (DOM, "Dispatching events"): the path is the target's ancestors to the
document (and `window` after `document` for events that reach it: `load`
does not bubble but is dispatched AT window; input events bubble to
window); capture listeners from the top down, then target, then bubble up
when `bubbles`. `once` listeners are removed before they run. A listener
added during dispatch to the current target does not run for this event;
one removed during dispatch does not run. Each listener is one
`os64_js_call` followed by `os64_js_checkpoint` (DOM.md § The event loop:
"after each listener when the event was dispatched by the window"). A
listener that throws is reported and the next one still runs; a sticky
failure ends the dispatch and the task. The handler property/attribute
runs at its target's turn in the list in the position it was first set
(the standard's "event handler map" ordering); the builder may simplify to
"after the listeners added before it was first set, before those after",
which is the same thing.

**The host entrance.** `os64_dom_dispatch(dom, node_or_NULL_for_window,
const os64_dom_event_t *ev, os64_dom_verdict_t *verdict, out)`: the host
describes the event (type, bubbles, cancelable, the mouse or key fields,
the related node) in a C struct, libdom builds the Event object, opens the
task (`task_begin`), dispatches, closes it (`task_end`), and answers
whether the default was prevented and whether the runtime survived. **It
costs nothing when nobody listens**: libdom keeps a count of listeners
and handlers per type across the page, and `os64_dom_listens(dom, type)`
answers it in O(1), so yonder never builds an event object for a
`mousemove` on a page with no `mousemove` handler. A content attribute
counts the moment the parser or a verb sets it, which is the same walk as
the version's (the builder decides whether to count attributes at parse
time through a hook or to answer "listens" for a type conservatively by
scanning `on<type>` attributes once per version; either is acceptable if
`mousemove` on a handler-free page costs no JS entry).

**Scripts calling `dispatchEvent`** from inside a task run the listeners
synchronously as nested calls, with NO checkpoint between them (DOM.md:
checkpoints follow callbacks that return with no script on the stack).
libjs's `call` must therefore permit a nested call FROM a native callback
of the engine (the stack is JS → native dispatchEvent → JS listener),
which is an ordinary `JS_Call` under the open task; what is refused is a
second `task_begin` while one is open.

**Timers** (HTML, "Timers"): `setTimeout(fn|string, ms, ...args)`,
`setInterval`, `clearTimeout`, `clearInterval`, ids from 1, never reused
within a page. A string timer is the old web's `setTimeout("tick()",
1000)` and is evaluated in global scope as a script (through
`os64_js_run`, its source name `<url>#timer-<id>`); a function timer is
`task_begin`, `call` with its args, `task_end`. A delay below zero or NaN
is 0; the resolution is the tick (10 ms, DOM.md says so nobody files a
bug); nesting past level 5 clamps to 4 ms, which the tick makes moot but
costs one line. The table lives in libdom (it holds function values and
argument values), ordered by due then id, bounded (4096 live timers;
the 4097th throws `QuotaExceededError`, charged to the binding's budget).
The host drives it: `os64_dom_timer_next(dom)` answers the earliest due
in the host's clock or UINT64_MAX; `os64_dom_timer_fire(dom, now, out)`
runs AT MOST ONE timer whose due is at or before `now` (an interval is
re-armed at `now + delay` before its callback runs, so a callback that
clears its own interval wins) and answers whether another is due. The
clock is the host's: `os64_dom_options_t` gains `now_ms(opaque)`, which
yonder points at `yonder_now_ms`; the harness points it at a variable.
A cleared timer whose callback is running finishes running.

**A script that asks to navigate.** `location.href =`, `location.assign`,
`location.replace`, `location.reload`, `history.back/forward/go(n)`,
`form.submit()` and `form.reset()` (and `element.click()` on a link or
button, which the old web uses for "submit on Enter"): libdom records the
ask in the dom (one slot: the LAST ask of a task wins, as the last
`location=` wins in a browser) and returns to the script. After the task
ends, yonder reads it with `os64_dom_take_navigation(dom, &ask)` and
performs it — through `os64_page_activate` and `request_navigate` with
`WAY_ASK_GO` for a URL or a form (DOM.md § The event loop: "the page chose
where and nobody pressed anything"; the refresh chain cap applies, and a
script cannot take a person from https to http without the bar), through
`click_back`/`click_forward` for history, through `click_reload` for
reload. `location.href` READS the page's URL; `protocol`, `host`,
`hostname`, `port`, `pathname`, `search`, `hash` read its parts through
libos64's URL parser; `hash =` on the same document scrolls to the
fragment without a navigation. `document.location` is `location`.
`window.status =` is accepted and ignored: the status line is yonder's,
and the old web assigns it on every mouseover. A navigation asked for by a
page that has not yet arrived (its parser still stopped) is performed at
the next task boundary like any other: it cancels the stream, as a click
on a link would.

**Script element preparation moves into libdom.** `classic()` in
`yonder/scripts.c` (type and language qualification) becomes
`os64_dom_script_kind(node)` in libdom, which every caller asks: the
parser stop, the defer and async lists, and a verb that connects a script
element. The standard's "prepare the script element" flags live on the
wrapper record or in a small per-dom table keyed by node: ALREADY STARTED
(a script runs once, whatever moves it), PARSER-INSERTED (set by the
host for the node the parser stopped at), and FROM INNERHTML (set by the
fragment verbs: a script inserted by `innerHTML` never runs, DOM.md § The
parser with scripting on). When a verb CONNECTS a script element that is
not already started and not from innerHTML (`appendChild` of a script
made with `createElement`, the old web's dynamic loader), libdom tells
the host through `options.script_connected(opaque, node)`, and the host
queues it (below).

## The loop, in yonder

### The script host of a page

D5b's `yonder_scripts_t` (`scripts.c`) stays the owner object and grows
into the page's script host: the runtime, the dom, the page's state, the
URL, and now the lists the order needs. Its queue of nodes-in-tree-order
built at arrival goes: scripts run as the parser reaches them. What it
holds instead, all as NODES with holds (never values):

- `blocking`: the parser-inserted script the parser is stopped at, and
  whether it waits for a fetch (`src`) or for sheets;
- `deferred[]`: `defer` scripts in parser order, each with its fetch's
  job id or its arrived source;
- `async[]`: `async` scripts and connected scripts with `src`, run as
  each arrives, in arrival order;
- `ready[]`: scripts whose source is in hand and whose turn it is: an
  inline connected script, an arrived async script, the next deferred
  script once the parse has ended — one is run per turn.

The runtime and dom are created at the FIRST script that will run, which
is now usually mid-stream. `os64_dom_create` needs a state that belongs
to the document: the script host creates it (`os64_page_state_create(doc,
cap)`) and the model built at `stream_finish` ADOPTS it
(`os64_page_build(doc, url, NULL, state)`: "a supplied state must belong
to doc and outlive the model"), so the control values a mid-parse script
set are the ones the model shows. The state is freed by `page_clear` after
the models, as D5b orders it. The script host also carries the page's
execution limit, read from the setting when the host is made.

### The stream's turn

`stream_turn` today feeds a slice and resumes every stop. After D7:

1. A `feed` or `resume` that answers `OS64_HTML_SCRIPT` names the script
   (`os64_html_parser_script`). If `os64_dom_script_kind` says it does
   not run (a module, VBScript, a template's), resume at once as D4 does.
2. Otherwise the stream is STOPPED AT A SCRIPT and the slice ends there
   (no further bytes are fed this turn, however much of the budget was
   left). The script becomes `blocking`.
3. **Sheets named before it are sent for, and waited for.** The sheets
   the tree lists so far (`link rel=stylesheet` and `style` reached before
   the script) are started now if they were not already: this pays D4's
   booked row "linked sheets fetched as the parse goes" (DEBTS.md), and
   it is why the stream's `Page` now exists from the head (its serial is
   drawn at `start_trip`, so a sheet job can name it). The builder chooses
   between building a model on the partial tree (`os64_page_build` on
   `os64_html_parser_document`, the model's sheet list) and a walk of the
   tree for `link`/`style` since the last stop; measured on the saved
   Wikipedia page, whose first stop is in `<head>` after its links. The
   script waits for those sheets at most `SHEETS_WAIT_MS`, like the first
   paint; the wait's deadline rides the ticker like `g.coming.due`.
4. A `src` script is a fetch job on the pool, in the shape of a sheet's
   (`yonder_script_job_t`, `YONDER_JOB_SCRIPT`, carrying the page's
   serial, the page's hooks, the document's encoding for a script with no
   charset; the response is text, read whole, capped at
   `SCRIPT_SOURCE_MAX`), and the stream waits for it; its arrival at
   `reaped` hands the source to the script host. A `defer` or `async`
   script's fetch starts the same way, but the parser resumes at once.
5. When `blocking` has its source and its sheets (or the wait expired),
   it runs: `forms_flush` is not needed (no widgets yet for an arriving
   page; for a connected script on the shown page it is), the source is
   copied as D5b copies it, `os64_js_run` with the name
   `<url>#inline-<n>` or the script's own URL, the outcome reported as
   D5b reports it, then `os64_html_parser_resume`, which may stop at the
   next script — the resume happens in the SAME turn (the turn's one
   task was the script; the resume is the parser carrying on), but the
   NEXT script found waits for the next turn, so a page of a hundred
   small scripts still lets the window read its events a hundred times.
6. `os64_html_parser_end` replaces the blind `finish`: it may stop at a
   script in the hold, handled as 1 to 5. Once `end` answers OK: the
   deferred scripts run, one per turn, in order, each when its source has
   arrived (a deferred script whose fetch failed is skipped with the
   status line saying so); then `finish`; then `DOMContentLoaded` is
   dispatched at `document` (one task); then the page arrives as D4
   arrives it (`stream_finish`'s model build, note, `arrive`), the model
   adopting the script host's state; async scripts still in flight keep
   running as they land, before or after arrival.
7. `load` is dispatched at `window` when the page is SHOWN
   (`arrive_now`), after the sheets wait, not after pictures: the old
   web's `onload` wants the tree and the sheets; a page whose `onload`
   reads a picture's size is D10's trigger and is booked.

Between 2 and 5 the stream's turn answers "nothing to do", not "more":
the bell is not rung for a wait; the pool's reap or the ticker wakes it.
The verdict-before-drain rule, the generation check and the refusal path
are D4's and do not move.

### The shown page's turn

`script_turn` runs AT MOST ONE of, in this order of preference: a script
in `ready[]` (connected or async or deferred), else a due timer
(`os64_dom_timer_fire`). After it, the rendering step as D5b has it
(`script_rebuild` when the version or the state moved), then the
navigation ask if one was recorded, then the bell if `ready[]` is
non-empty or another timer is due. The arriving page's timers fire in the
stream's turn by the same rule, when it is not stopped at a script
(timers set in `<head>` by a page still parsing must tick).

### Input events

Each site below dispatches BEFORE the action it precedes and skips the
action when the default was prevented. Every site first asks
`os64_dom_listens(dom, type)` and does nothing when the answer is no, so
a page without handlers costs what it costs today.

| Where today | Event | Target | Default action |
|---|---|---|---|
| `view_event` BUTTON_DOWN/UP | `mousedown`, `mouseup`, then `click` (press and release on the same node) | the hit box's node (`flow_hit`, `b->node`) | `follow_link` / `picture_button_at` |
| `hover` | `mouseover` on the node newly under the pointer, `mouseout` on the one left (with `relatedTarget`); `mousemove` only when listened to | the hit node; hover tracks the NODE now, not only the link | the status line's link text |
| `button_clicked` | `click` on the button's node; then `submit` (cancelable, bubbles) on the FORM node for a submit button, `reset` for a reset button | | `form_send` / `os64_page_reset` |
| Enter in a field (`form_send(…, IMPLICIT)`) | `submit` on the form | | `form_send` |
| libui text field edits | `input` after each edit that changed the buffer (libui's `os64_ui_textfield_t` gains `on_change`, paying DEBTS.md's "a text field's change callback" row); `change` when the field loses focus or Enter is pressed and the value differs from the one it had at focus | the control's node | none (the edit already happened; `forms_flush` before dispatch so the script reads the new value) |
| checkbox / radio `on_change`, list selection | `click` then `change` | the control's node | a cancelled checkbox `click` is put back (`os64_ui_checkbox_set` and the state) |
| `password_key` / a focused field's keys, taken before libui | `keydown`, `keypress` (printable only), `keyup` | the focused control's node | the edit; a cancelled `keydown` is not typed |
| focus moves (`os64_ui_set_focus`) | `blur` on the old node, `focus` on the new (neither bubbles) | | |
| `arrive_now` | `load` at window | | |
| after `finish` | `DOMContentLoaded` at document | | |

`element.click()`, `focus()`, `blur()` and `form.submit()` from script are
the same paths entered from the dom through host callbacks (`options`
gains `activate(opaque, node, what)`), performed after the task like a
navigation ask, because they reach widgets and the model.

### The clock

`pictures_schedule` hands the ticker the earliest of what it holds today
and `os64_dom_timer_next` of each live dom (the shown page's and the
arriving page's), and the sheets wait of a stopped script. The ticker's
bell handler runs `script_turn`/`stream_turn` as the loop does for any
bell. The covered-window rule does not gate timers: a page that counts
down keeps counting while another window is in front.

### Policy: how long a task may run

DOM.md leaves the execution-time default to D7 and asks for Chris's
Settings control. Ruled here, with the number marked as a lean to be
moved by P5 measurement, not as a measurement:

- **A task's budget is 5 seconds; the range is 1 to 60.** A 1998 page's
  task runs in milliseconds; a framework page's `load` handler on the P5
  runs in hundreds; QEMU without KVM is several times slower than the
  host (D4 measured a parser slice at 35 to 73 ms there against 22 ms for
  the whole page on the host) and D5b's fixture turns there in well under
  a second. Five seconds
  is long enough that no honest task on real hardware meets it and short
  enough that a runaway loop gives the window back before a person
  reaches for Stop. (Chrome's "page unresponsive" is thirty; Chris may
  set that on the P5 and the control is there for him to.)
- **Per task, not per page.** Each task opens a fresh deadline. A page may
  run forever in small tasks; that is what a page is.
- **The job cap is a backstop, not the policy:** `jobs_per_turn` becomes
  1 << 20. A job costs time, so the deadline bounds it; 4096 was D5b's
  deterministic fixture cap and would end an honest Promise chain early.
- **Settings gains a slider beside "Run page scripts": "Script time limit:
  N s"** (libui's `os64_ui_slider`, 1..60, step 1), enabled whether or
  not scripts are on. Apply applies to this window: the current page's
  next task and every page after (`os64_js_set_execution_ms` on each live
  runtime, between tasks, which is where Settings' bell is handled). Save
  as default writes `script_seconds = N` to `yonder.conf` beside
  `scripts =`; absent or unparsable means 5.
- **Overrun** stays DOM.md's: the task ends at the deadline (LIMIT /
  EXECUTION from libjs), the runtime is retired (drain, destroy, free),
  timers and listeners go quiet with it, `ready[]` and `deferred[]` are
  dropped, the parser if stopped is resumed to the end with no more runs,
  and the status line says `Script <name>: execution stopped after N s
  (this page runs without script)`. Links and forms keep working through
  their default actions. The `--script-audit` flag logs each task's name
  and microseconds, as it logs a slice's.

### Teardown

DOM.md's seven steps, now with every step real. `stream_drop` on a page
with a script host: cancel the page's jobs (serial retired, the fetch
cancelled as today), `os64_html_parser_abandon` instead of `destroy`
(DEBTS.md's row; the document passes to the page), then `page_clear`,
which already orders script host (timers and lists dropped, registries
drained, runtime destroyed, dom freed) → widgets → tree → cascade →
models → state → document. `stream_drop` therefore builds the `Page` it
has been filling and clears it; the D4 shortcut of destroying the parser
with its document is gone for every stream, not only scripted ones, so
there is one path. A navigation asked for by a script is performed after
its task, never inside teardown. The mode change (`settings_use`) and the
window's close reach the same `page_clear`.

## What a reviewer should read with a question in mind

- **The lost-wakeup question, again:** a stopped script waiting on a fetch
  or a sheet must be woken by the reap that brings it; the test is a
  `src` script whose fetch lands while the window is idle in
  `os64_gui_event_wait` with nothing else due.
- **Holds:** every node the script host lists is held and released at
  the list's end or at retirement, and a node a script removed from the
  tree while listed is still a valid identity (D5b's rule), skipped at
  its turn with `os64_dom_script_kind` and "is it connected" re-asked.
- **Two runtimes:** a timer of the arriving page fires in the stream's
  turn; a timer of the shown page in `script_turn`; a navigation ask from
  the arriving page cancels its own stream; `stop_trip` with a stopped
  script tears the host down through the same `page_clear`.
- **The task bracket:** no `JS_Call` outside `task_begin`/`task_end`
  anywhere in libdom; `grep -n JS_Call` is the audit.
- **The three registries drain to zero** before destroy, with a listener
  installed, a timer pending and an event object retained by a script, on
  every teardown path (departure, mode change, overrun, window close,
  stream drop).

## The cut: two PRs, stacked

**D7a — the registry and the turn** (inert in yonder; proven on the host).
**Built:** DOM.md § D7a, as built records what was built and where it
departs from this brief.
libjs: the five entries and the contract rewrite, with `test_js_runtime_host`
cases. libdom: EventTarget, Event, handler attributes and properties,
dispatch, `os64_dom_dispatch`/`os64_dom_listens`, timers and their two
host entries, `location`/`history`/`form.submit`/`element.click` as
recorded asks, `window.status`, `document.write` throwing by name,
`os64_dom_script_kind` and the preparation flags with
`options.script_connected`; `test_dom_host` cases for each rule (dispatch
order capture/target/bubble, `once`, removal during dispatch,
`preventDefault` and `returnValue`, `return false`, handler-attribute
scope chain and lazy compile and invalidation on `setAttribute`, `<body
onload>` landing on window, a listener that throws not stopping the
next, timers in order with the harness's clock, an interval clearing
itself, the 4096 cap, string timers, a nested `dispatchEvent`, the
navigation slot's last-wins, `innerHTML` scripts never running and
`appendChild` ones reported once). yonder changes only enough to link:
`classic()` replaced by `os64_dom_script_kind`. LIBDOM.md gains its
events and timers sections. Merges when the host suites are green and
yonder's behaviour is unchanged (`test_yonder_scripts_host` 2099/0).

**D7b — the loop** (proven on the host and in the guest).
**Built, with the input events (D7c) in it:** DOM.md § D7b, as built. Everything under
"The loop, in yonder": the script host's lists, the stream's turn with
runs, `src`/`defer`/`async`/connected scripts and the script job, sheets
at a stop, `end` before `finish`, DOMContentLoaded and load, the shown
page's turn, timers on the ticker, every input-event site, libui's
`on_change`, the policy and its Settings control and `yonder.conf` key,
abandon at `stream_drop`, the audit lines. The input-event sites and
libui's `on_change` were allowed to split off as a third PR if D7b ran past
what one review could hold; D7b carried them, and the name went to the
join below.

**D7c — the join** (on `userland`, once D4, D7a, D7b, D8 and D10 have all
merged there; Opus builds, Fable reviews; it is the first thing the play
branch takes). D8 (DOM_D8.md, the reporting destroy) and D10 (DOM_D10.md,
geometry) were built beside D7 on plain `userland`, and each was approved
with a list of what D7b's rewrite of the script host has to adopt. None of
the three can prove the joined behaviour on its own, so the list is a slice
with its own proof rather than a note pinned to D7b.

What D7c owes, each with the record it comes from:

1. **Teardown** (TEARDOWN.md § Browser ownership). `ensure_runtime` creates
   the page's runtime with `os64_js_create_with_teardown` and `RECLAIM`;
   `retire` destroys it with `os64_js_destroy_report`, sends a reclaimed
   leak through `os64_debug_log` with the page's address and the reclaimed
   block and byte totals, and counts it in `yonder_scripts_teardown_leaks`.
   The order stays DOM.md's: the lists dropped, the registries drained
   (listener callbacks, timer callbacks and arguments, the event objects,
   whose records are the engine's ledger or `d_alloc`), destroy,
   `os64_dom_free`, the holds released. No native resource of D7's may need
   a finalizer to release it; `event.c`'s finalizer frees engine memory
   only, and that stays the rule.
2. **Geometry** (DOM.md § Geometry, DOM_D10.md). The provider is per script
   host, with the host's own page as its opaque: `yonder_scripts_options_t`
   carries it beside `fetch`, `activate` and `now_ms`, and `ensure_runtime`
   installs it with `os64_dom_set_geometry`. The stream's host lays the
   parser's document out at the view's size as far as it has been parsed,
   which is the design's "the page being loaded can be asked too"; the
   coming page's host answers from its own model and a layout at the view's
   size; the shown page's answers as D10 has it. A layout made for a page
   not on screen is not published: no scroll clamp, no widgets, no paint.
3. **The count and the sentence, per task kind.** D10 resets the layout
   count before a task and reports `Script forced N layouts in T ms` after
   it, for the one task kind its host ran. D7b runs six: a blocking script,
   a ready script, a timer, a dispatched input event, `DOMContentLoaded`,
   `load`. Every one of them resets before and reports after, through
   `task_said` or beside it, and the `--script-audit` line carries the
   count.
4. **The stack, remeasured with dispatch on it.** D10 chose a 128 KiB engine
   stack for the browser from script depth 160 plus the deepest layout
   shape. D7b puts `invoke`, `run_record` and `JS_Call` between the task
   and the listener; the join lays out the deepest shape from a listener
   chain and adds that row to DOM_D10.md's table. If the headroom falls
   under the 128 KiB native threshold D10 held, the number moves and the
   doc says why.
5. **One CONTRACT.md.** D7a, D8 and D10 each rewrote its status line and
   its count of exported operations; the join leaves one status and the
   phrase "the embedding operations" rather than a number.

The proof, on the host in `test_yonder_scripts_host.c`: the lost-wrapper
page with a listener installed and a timer pending retires to one log line,
a count of one, a next page that runs, and nothing live; a `DOMContentLoaded`
listener that reads `offsetWidth` answers the laid-out number; a mid-parse
script that measures an element before it, with the rest of the page
unparsed, answers at the view's size; a timer that forces a layout puts the
sentence on the status line; a click listener that overruns inside a forced
layout gets the overrun sentence, the runtime retired, and its link still
working through the default action. One mutant per obligation. In the
guest: D8's lost-wrapper page and D10's `dom-geometry.html` through the
real stream with scripts on, and D7b's walk unchanged.

## The proof

On the host first, in the house's shape:

- **`tools/test_js_runtime_host.sh`** gains the cases listed under the
  extensions; **`tools/test_dom_host.sh`** the cases listed under D7a;
  **`tools/test_dom_mutants.py`** one mutant per new rule.
- **`tools/test_yonder_scripts_host.sh`** gains the loop: its clock stubs
  (`os64_micros`, `yonder_now_ms`) become variables the cases advance,
  and its fake pool learns to answer a script job. Cases: the ORDER
  FIXTURE — a page with an inline script in head, a blocking `src`
  script, a `defer` script, an `async` script whose fetch lands first,
  an inline script in body that `appendChild`s a script, `<body
  onload>`, a `DOMContentLoaded` listener and a `load` listener, each
  pushing its name to an array the case reads back, in the standard's
  order (head inline, src, body inline, connected, async-at-landing in
  its place, defer, DOMContentLoaded, load); a script that reads a form
  value set by an earlier mid-parse script after the model adopted the
  state; a timer set in head firing after arrival; a `click` on a link
  with a listener that prevents it (the fake pool sees no submission) and
  one that does not; `submit` cancelled; a real libui key edit raising
  `input` and the value read back; `change` at blur; `mouseover` on hover
  with no listener costing no engine entry (the case counts entries); a
  task that overruns with the clock advanced, retiring the runtime, its
  pending timer not firing, its stream resumed to the end without runs,
  the status line's sentence; the limit applied by Settings taking effect
  at the next task; a script-asked navigation performed after the task
  and judged `WAY_ASK_GO`; teardown mid-stream with a stopped script, a
  listener, a timer and a retained event, through `stream_drop`,
  `settings_use` and the window's close, under LSan; and the three
  registries' counts at zero before each destroy.
- **Mutants** (`tools/test_yonder_stream_mutants.py` grows or a sibling):
  one per rule of the stream's turn and the shown page's turn.
- **Consumers unchanged**: wend, libpage, libflow, libgarb, libhtml, the
  painter, the stream ring harness, all 0 failed.

Then the guest, on a scratch copy of the image, never Chris's, with
`scripts = on`:

- **J4's evidence**, a fixture page per row served by `httptestd.py`: the
  order fixture rendering its array into a heading; a `document.write`
  counter page FAILING LOUDLY by name (D9's trigger, recorded); a DHTML
  menu that opens on `mouseover` and closes on `mouseout`; a form
  validator on `onsubmit` that refuses an empty field and lets a filled
  one through to the server's log; a `setInterval` clock updating a
  heading once a second, still ticking with gterm in front; a `<body
  onload>` page; a runaway `while(true)` page ending with the sentence
  and its links still working; the Settings slider moved and the next
  runaway ending at the new number.
- **D4's and D5b's guest walks unchanged**: the Y3 walk, `/stall-body`,
  the twenty-eight sheets (now requested from the first stop, earlier
  than D4 requested them), the two JavaScript donuts.
- **Measured**: Wikipedia's time to arrival with scripts on (its scripts
  will mostly throw at the surface they expect; what is measured is the
  loop's cost per stop and the earlier sheet start), and the per-task
  audit on the P5 when Chris runs it, which is where the 5 second lean
  gets its number.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| An inserted INLINE script runs at the next task, not synchronously inside `appendChild` | the standard runs it inside the verb; that is a nested evaluation libjs refuses by contract, and the old web's dynamic loader uses `src` | a page that depends on the synchronous run |
| `load` does not wait for pictures | the old web's `onload` wants the tree; waiting on lazily fetched pictures would hold `load` for a scroll | a page whose `onload` reads a picture's size (D10 has the reader) |
| `mousemove` and `keypress` repeat rate | one dispatch per GUI event, each a task; a page that listens to `mousemove` pays a task per pointer sample | a page whose drag is visibly slow |
| `window.open`, `window.close`, `window.name` | tabs and popups are a different browser | a page that cannot be used without a popup |
| `focus`/`blur` on non-control elements (`tabindex`) | libui's focus is the widget's | a page that focuses a `div` |
| The execution-time default measured on the P5 | 5 s is a lean; the audit line is the instrument | Chris's first runaway page |
| `document.write` | D9 | the counter page in the J4 walk |
