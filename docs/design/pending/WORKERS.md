# WORKERS.md — a page's dedicated workers: a thread and a runtime of their own

*Modern Web arc, from MODERN_WEB.md's excluded list ("Workers, service
workers, WebAssembly, WebRTC, WebGL…"). Written by Fable on 2026-11-07 at
`userland` a0af6b11 (every function named below was found in the tree
that day) and from HTML § 10 (Web workers), § 2.7 (structured clone),
the File API § 9 (blob URLs) and WebIDL's `[Exposed]`. Written the same
day IFRAME.md was revised, because the page that asked for both is one
page: a search engine's unusual-traffic page, whose CAPTCHA is a frame
that constructs a Worker during its `init`, once and with no feature
test, and dies there before it wires the checkbox it just drew. No host
names from the corpus appear here; counts only (`~/os64-corpus`, 349
hosts, read 11/07).*

## What this settles

1. **A dedicated worker is a thread of yonder's with a JavaScript
   runtime of its own.** Not a realm of the page's runtime (a realm
   shares the page's thread and heap, and a worker's whole point is that
   it does not), not a process (os64 has threads; a worker needs the
   page's address space for nothing, but a process per worker would cost
   a task, an ELF and a page table each). The runtime is libjs's, made
   on the worker's thread with its own memory limit, stack limit and
   interrupt flag, and freed on that thread. Two runtimes never share a
   value: everything that crosses is bytes (3), and nothing in libdom's
   page binding is touched from a worker's thread.
2. **The worker's global is a libdom binding with no document.** The
   binding keeps its one struct; a `kind` says whether it is a window's
   or a worker's, and the rows table (modern.c) says for each row which
   globals it is exposed to — WebIDL's `[Exposed=(Window,Worker)]`,
   written down once per row instead of once per member. A worker's
   creation walks the same table with the Worker bit. What a worker gets
   is what the standard exposes to workers and this browser has: events,
   timers, `queueMicrotask`, `console`, `fetch` and its classes, `XMLHttpRequest`,
   `URL`, `TextEncoder`/`TextDecoder`, `atob`/`btoa`, `structuredClone`,
   `Blob`/`File`, `AbortController`, the streams, `crypto`,
   `performance`, `DOMException`, `ImageData`, `MessageChannel`, and its
   own: `self`, `name`, `location` (`WorkerLocation`), `navigator`
   (`WorkerNavigator`), `postMessage`, `onmessage`, `onmessageerror`,
   `onerror`, `close()`, `importScripts()`, `WorkerGlobalScope` and
   `DedicatedWorkerGlobalScope`. Nothing of the document's: no `window`,
   no `document`, no node, no `localStorage` (the standard's choice), no
   `alert`.
3. **Everything that crosses is a packed clone.** clone.c's record is no
   realm's but it is the runtime's (its primitives are engine values, its
   keys atoms). It gains a byte form: `d_clone_pack` turns a written
   record into bytes owned by nobody's engine, `d_clone_unpack` turns
   bytes back into a record in the receiving runtime, and `d_clone_read`
   then builds the value as it does for a message between windows. The
   writer, the reader and what is cloneable do not change; a message to a
   worker is "write, pack, cross, unpack, read" and a message to a frame
   stays "write, cross, read" (IFRAME.md § Workers). Transferred
   `ArrayBuffer`s travel as their bytes, which clone.c already copies.
   `SharedArrayBuffer` stays refused: libjs builds without `Atomics`
   (patch 0001) and no page here is cross-origin isolated.
4. **Messages ride mailboxes, and each side's loop takes from its own.**
   The page's `Worker` object owns a pair: the worker's inbox (the page
   writes, the worker thread reads) and the page's inbox (the worker
   writes, the window reads). Each is a locked list of packed clones plus
   a wake: the worker's wake is a pipe it waits on with `os64_read_for`
   (a timed read, so its timers need no second mechanism); the window's
   wake is one doorbell bit (`BELL_WORKERS`, beside `BELL_WORK`,
   `BELL_MAIL` and the rest in yonder.c). A message is a task on the
   receiving side, oldest first, as message.c's are: the window's loop
   takes one inbox item per turn per worker and fires `message` at the
   `Worker` object; the worker's loop takes one and fires it at its
   global. Order is kept per direction; the two directions are
   independent, as the standard has them.
5. **A worker's fetches go through the window.** The pool (`os64_work`)
   is the window thread's: "call public operations on the creating
   thread" (work.h). So the worker's binding asks its host for a fetch,
   the host writes the ask to the window's inbox, the window submits the
   job as it submits the page's (`request_job`, with the page's origin
   and serial and the worker's script URL as referrer), and the job's
   answer travels back through the fetch pipe (fetch_pipe.c) — which is
   already a two-thread structure with a `ring(ctx)` the writer calls —
   with the worker's thread as the reader and the worker's pipe as the
   ring. `importScripts` and a synchronous `XMLHttpRequest` are the same
   relay with the worker's loop waiting on its own pipe for the answer,
   which blocks the worker alone, as the standard lets a worker block.
   Cookies, the cache, mixed content and the record's fetch lines are
   the page's rules, run on the page's thread, so a worker can reach
   nothing the page cannot.
6. **A worker lives from `new Worker` to `terminate()`, `close()`, an
   unrecoverable failure, or its document's departure — whichever is
   first — and its thread outlives it.** Threads are pooled per window
   (`YONDER_WORKER_THREADS`, § Bounds): a worker takes a free thread or
   waits for one; a worker's end frees its runtime on its thread and
   returns the thread to the pool. A thread is never let exit while the
   window lives, because os64 has no per-thread teardown inside a living
   task (DEBTS: "No PER-THREAD teardown while the task lives", whose
   "nothing does yet" this design would otherwise make false: a page
   that starts a worker per job, which many do, would strand 1 MiB of
   stack per worker). When that row is paid the pool may shrink idle
   threads; nothing here depends on it.
7. **A worker's script runs under the page's budgets, per task, and an
   overrun ends the worker, not the page.** Each task (the script's own
   run, a message, a timer, a fetch's arrival) has the page's task
   deadline (`execution_ms`, Settings' script time limit); the engine's
   interrupt handler unwinds it as it does the page's; a worker whose
   task overran is ended with a `FAILED worker <address>: over its
   budget` record line and an `error` event at its `Worker` object. The
   standard gives a worker no deadline at all, and a worker spinning
   forever at a whole core is what a tab's spinner does elsewhere; on the
   P5 it is a fan and a slow page, so the page's rule is the worker's
   (§ Rulings 2).
8. **A worker's uncaught error is reported to its page.** HTML's "report
   the error" for a worker: an `ErrorEvent` at the worker's global first
   (its `onerror`); when not handled, an `ErrorEvent` at the page's
   `Worker` object with `message`, `filename`, `lineno`, `colno` and no
   `error` (the value cannot cross); when not handled there either, the
   page's window `error` and the record's `FAILED worker <address>:
   <message>` line. The console lines a worker writes go where the page's
   go (yonder's output hook), prefixed with the worker's name or address.
9. **Blob URLs come first, as a slice of their own.** `URL.createObjectURL`
   and `revokeObjectURL` are absent today, 112 of 349 hosts call them,
   19 of the 71 hosts that construct a Worker build its script from one
   (`new Worker(URL.createObjectURL(new Blob([code])))`), and both
   challenge pages this doc is for want them (`worker-src blob:` in the
   CDN challenge's CSP; es-module-shims' takeover fetches through them,
   importmap.c's comment). W0 builds the store: `blob:<origin>/<uuid>`,
   one store per page shared by its window and its workers under a lock,
   entries holding the bytes outside any engine, revoked by the verb or
   at the page's departure, fetched by yonder's own door before the pool
   (as `data:` is, `yonder_fetch_data_url`) for pictures, scripts,
   stylesheets, `fetch`, `XMLHttpRequest`, downloads and the worker
   constructor alike. A blob URL made in a worker is in the page's
   store, so a worker can make a picture and the page can show it.
10. **Out of this design, each with its row:** shared workers (4 hosts),
    service workers (34 hosts register one; a service worker is a
    network interposer and a lifetime of its own, and its absence is
    honest: `navigator.serviceWorker` stays undefined so registration is
    feature-tested away), module workers (`type: "module"`: 0 hosts;
    DEBTS "no module workers"), nested workers (`Worker` inside a
    worker), `MessagePort` transfer (W3, with frames: IFRAME.md § Workers),
    `OffscreenCanvas` (33 hosts mention it, every one feature-tested; a
    canvas on another thread is the canvas design's), `WebAssembly` (12
    hosts), `BroadcastChannel`, `IndexedDB` and the Cache API in a
    worker, `Atomics.wait`, `requestAnimationFrame` in a worker, and a
    worker's own CSP. `navigator.hardwareConcurrency` answers the pool's
    size, not the machine's: a page sizes its worker farm by it.

## What the tree is today

- **Threads.** `os64_thread(fn, arg)` (libos64 thread.c) is the one
  verb; a thread's stack is `THREAD_USER_STACK_SIZE`, 1 MiB, always:
  the kernel's `user_stack` takes an `asked` size but the thread syscall
  passes 0 (thread_join.c `createThread(task, false, 0)`). yonder runs
  the window thread, the pool's workers (`os64_work`, work.c, started
  with `os64_thread`) and the ticker (ticker.c). SCRIPT_STACK.md gave the
  window thread 4 MiB by `PT_GNU_STACK` and the engine 2 MiB of it, and
  wrote "a thread yonder spawns (workers) — 1 MiB, as today — workers
  run no scripts". This design makes that line false (§ The thread's
  stack).
- **The engine.** `os64_js_create_with_teardown(&config, policy, …)`
  makes a runtime with `os64_js_limits_t` (memory, stack, source,
  execution ms, jobs per turn); the page's is made in `ensure_runtime`
  (scripts.c) with `OS64_JS_TEARDOWN_RECLAIM`; each runtime has its own
  allocator, memory limit, `JS_SetMaxStackSize`, interrupt handler and
  job queue (runtime.c). QuickJS runtimes are independent of each other
  by construction; what they share process-wide is the class-id counter
  (`JS_NewClassID` is a static counter, not atomic), which § Threads
  rules on. libjs's standalone default limits assume a 1 MiB native
  stack (`os64_js_default_limits`: 256 KiB of engine stack).
- **The binding.** `os64_dom_create` makes one binding per runtime for
  one document, refuses a second on the same runtime, installs the
  classic window (classic.c) and then walks modern.c's rows table,
  `{names[], installer}`, under the `modern` switch; the DOM host holds
  each row to its names. `struct os64_dom` is document-centred
  (`dom->document`, the node prototypes, `dom->url`, `dom->timers`, the
  providers yonder fills). Nothing in it is thread-aware; nothing needs
  to be, since a worker's binding is touched by its own thread alone.
- **The clone.** clone.c: `d_clone_write(dom, ctx, value, transfer, n)`
  → `DClone`, `d_clone_read(dom, ctx, clone)`; the record is `DItem`s
  (`JSValue value` for primitives, `JSAtom key`, kind, sub, id, counts)
  and `DData` (bytes, a blob's type and name, an exception's name and
  message, an error's `stack` as a `JSValue`). Blob and File are
  cloneable (blob.c's `DBlob`, bytes the binding owns).
- **Messages.** message.c: `window.postMessage` to itself and
  `MessageChannel`/`MessagePort` (11/07), one queue of tasks run by the
  host (`os64_dom_message_fire`), a port in a transfer list refused.
- **Fetch.** libdom's fetch.c and xhr.c ask yonder through providers;
  yonder's `request_job` submits to the pool with the page's origin and
  serial; the answer arrives through `yonder_fetch_pipe_t` (head, body
  pieces, end), whose writer runs on a pool worker and whose reader is
  the window, woken by the pipe's `ring` callback; `BELL_BODY` is that
  ring today.
- **Blob URLs.** None: `createObjectURL` has no hit in libdom; url.c's
  `d_origin` already knows a `blob:` URL's origin is its inner URL's.
- **Errors.** event.c carries `ErrorEvent` (its `error`, `message`,
  `filename`, `lineno`, `colno`); the window's `error` event and the
  record's `FAILED script` lines come from scripts.c's `fail_item` and
  `yonder_scripts_failure`.
- **The census.** 71 of 349 hosts construct a Worker; 62 feature-test
  for one (`typeof Worker`, `"Worker" in window`); 72 records say
  `MISSING global Worker`; 3 records die on it (`FAILED … 'Worker' is
  not defined`) — the two challenge pages among them; 18 hosts call
  `importScripts`; 19 construct from a blob URL; 0 ask for a module
  worker; 34 register a service worker (all feature-tested); 4 construct
  a `SharedWorker`.

## The thread's stack

A worker runs scripts on a 1 MiB thread. The page's engine has 2 MiB
under 2 MiB of native headroom because layout runs under it; a worker's
thread runs no layout, no paint and no parser, only the binding's
dispatch frames, the clone and the engine, whose own worst case
SCRIPT_STACK.md measured. Two ways, in order of preference:

- **The kernel lets a thread ask for its stack.** `SYSCALL_THREAD` gains a
  fourth argument, the stack size in bytes (0 = the default), carried to
  `createThread`'s `userStack`, which `user_stack` already clamps to
  [1 MiB, `THREAD_USER_STACK_MAX` 16 MiB] and falls back from when the
  allocation is refused, logging both sizes; libos64 gains
  `os64_thread_sized(fn, arg, stack_bytes)` beside `os64_thread`, which
  stays as it is. yonder's worker threads ask for 4 MiB and give the
  engine what `yonder_script_stack_limit` gives the window's: the same
  arithmetic, the same clamp. A kernel slice of a dozen lines, Fable's
  design by the kernel rule, Chris's to approve (§ Rulings 1). It also
  pays SCRIPT_STACK.md's "a spawned thread keeps 1 MiB" line for any
  program that wants otherwise.
- **Without it,** the worker's engine gets 1 MiB less a measured headroom,
  `YONDER_WORKER_HEADROOM`, the DOM host's worker fixture (a deep
  recursion inside a message handler, under the sanitizers) sets, with
  the engine's stack check doing what it does for the page: a script
  that recurses past it gets a `RangeError`, never a guard fault. The
  fixture runs either way; with the kernel slice it proves 2 MiB, without
  it whatever the headroom leaves (half a megabyte, by SCRIPT_STACK.md's
  numbers). The record's worker line says which the worker got.

The shipped build takes the first when the kernel has it and the second
when it does not (an old kernel, a refused allocation): `os64_thread_sized`
reports what the thread was given, as `user_stack` logs it.

## Threads (yonder `worker.c`)

**The pool.** `YONDER_WORKER_THREADS` threads per window (§ Bounds), made
at the first `new Worker` and kept until the window closes; each runs
`worker_thread`: wait for an assignment, serve it to its end, free
everything on this thread, go back to waiting. An assignment is a
`Worker` record: the script's URL and the page's base, the worker's
name, its credentials mode (parsed, applied by the page's fetch), the
owning Page (IFRAME.md § Workers), the two mailboxes, the pipe, and the
state (`waiting`, `starting`, `running`, `closing`, `ended`) written
under the record's lock. A page past the pool's size queues its worker
(`new Worker` returns at once in every case, as the standard's
constructor does; the worker's script simply starts when a thread is
free); the queue is per window, oldest first.

**Serving a worker, on its thread:**

1. Make the runtime: `os64_js_create_with_teardown` with limits
   `{YONDER_WORKER_MEMORY, the stack § The thread's stack gives, the
   page's source bytes, the page's execution_ms, the page's jobs per
   turn}`, `OS64_JS_TEARDOWN_RECLAIM`, the output hook (console) the
   page's with a prefix; install the page's bytecode cache reader
   (bytecode.c) so a kept worker script compiles from its blob as the
   page's do.
2. Make the binding: `os64_dom_create_worker(runtime, &settings)` with
   the worker's URL, origin, name, the clock and the providers (§ The
   binding). Register the mailbox's reader and the fetch relay.
3. Fetch the script through the relay (§ Fetch): a classic script, the
   worker's URL, `Sec-Fetch-Dest: worker`, the page's credentials mode
   applied, the page's cache. A refusal (a network failure, a non-script
   type by the standard's MIME rule, a blob URL that was revoked, a
   cross-origin URL — a dedicated worker's script must be same-origin or
   a blob/data URL of the page's) ends the worker before it runs with an
   `error` event at the `Worker` object and no `ErrorEvent` detail,
   which is what the standard fires for a script that could not be
   fetched.
4. Run it as a classic script in the global, under the task deadline;
   drain the job queue (microtasks). Then loop: `os64_read_for(pipe,
   …, until the next timer)`; on wake take, in this order and one each,
   a `closing` mark (→ step 5), an inbox message (fire `message`), a
   fetch pipe's news (hand the head, a piece, the end to the binding as
   the window does for the page), a due timer. Each is a task: deadline
   armed, microtasks drained after, the outcome read; `LIMIT` or
   `FAILED_RUNTIME` → step 5 with the record line § What this settles 7
   names; an exception → § What this settles 8's report, and the loop
   goes on.
5. End: mark `ended` under the lock; `os64_dom_free` the binding
   (drains its timers, listeners and requests, as a page's does); free
   the runtime (`RECLAIM`); drop the mailboxes' items not yet taken
   (a message to an ended worker goes nowhere, as the standard says);
   write one note to the window (`BELL_WORKERS`) so the `Worker` object
   learns the end and the record gets its line; return to waiting.

**The window's side.** `new Worker(url, options)` in the page's binding
calls the host's `worker_start(dom, url, name, credentials, out)`
(a provider, as the fetch is): yonder resolves the URL against the
page's base, refuses what step 3 would (so the constructor throws
`SyntaxError` for an unparseable URL and `SecurityError` for a
cross-origin one, synchronously, as the standard does), makes the record
and the mailboxes, assigns a thread or queues, and hands back a handle
the `Worker` object holds. `Worker.prototype.postMessage` writes, packs
and appends to the worker's inbox and writes the pipe's note;
`terminate()` marks `closing`, cancels the runtime
(`os64_js_cancel`, which js.h already names "the cross-thread
operation": sticky, so the worker's next engine check unwinds with
`CANCELLED` and no task of it runs again), and drops the page's inbox for that worker, so no
message of it is heard after the call returns, as the standard requires.
The window's loop, on `BELL_WORKERS`, walks the window's workers: one
inbox item each per turn, fired at its `Worker` object as a task through
message.c's machinery (the packed clone unpacked and read in the page's
realm); an `ended` note → the `Worker` object's `error` when the end was
a failure, the record line, and the handle's release. A `Worker` object
the page let go of (its wrapper finalized) terminates its worker, as the
standard permits a user agent to do once the object is unreachable and
no message can ever reach it; the finalizer marks `closing` and nothing
more, since a finalizer owns no thread.

**Departure.** `page_departs` marks every worker of the Page `closing`
and cancels each runtime (`os64_js_cancel`), then goes on; it does not
wait; the record and the mailboxes stay the window's until the thread's
`ended` note, as js.h asks ("callers keep the runtime alive until
requesting threads have stopped using its handle"). A worker's
thread notices at its next engine check or its next wake (the window
writes its pipe a note as well, for a worker asleep in `os64_read_for`),
unwinds, and ends. The runtime's memory is the worker's thread's to free; the page's
departure holds nothing of it. A navigation away from a page whose
worker is mid-compute therefore costs the page nothing and the worker's
core until the interrupt lands, which the engine checks every few
thousand instructions.

**Class ids.** `JS_NewClassID` keeps a process-wide static counter; libdom
takes every class id it will ever need once, at the first binding's
creation on the window thread, before any worker thread exists
(`d_class_ids_once`), and each runtime registers classes against those
ids (`JS_NewClass` is per runtime and needs no lock). A worker thread
that finds an id untaken is a bug the DOM host's two-thread fixture
catches. libjs's own statics are audited by its owner for the same rule
(§ The cut, W1's libjs line): anything written after startup from two
runtimes at once is a finding.

## The binding (libdom)

**Kinds.** `struct os64_dom` gains `kind` (`D_KIND_WINDOW`,
`D_KIND_WORKER`). `os64_dom_create` sets WINDOW as today;
`os64_dom_create_worker` sets WORKER, takes no document, and builds the
global from the worker floor plus the rows exposed to workers.

**Exposure.** modern.c's row gains `exposed`: `D_EXPOSED_WINDOW`,
`D_EXPOSED_WORKER`, or both, per WebIDL's `[Exposed]` for what the row
installs; a row with both installs the same names into either kind of
global, and its installer must therefore never read `dom->document`
(the DOM host's worker creation under the sanitizers proves it, and a
mutant that marks a window-only row as both must fail there). classic.c
splits once: `d_classic_window` (as today) and `d_classic_worker` — the
floor every global has (`EventTarget`, `Event`, `CustomEvent`,
`ErrorEvent`, `MessageEvent`, `PromiseRejectionEvent`, timers,
`queueMicrotask`, `console`'s hook, `atob`/`btoa`, `structuredClone`,
`DOMException`, `self`, `globalThis`) — and the window floor above it
(`document`, `window`, `frames`, the classic nodes). The table's test
in the DOM host (every row installs exactly its names) runs for both
kinds; the worker's names list is the standard's `DedicatedWorkerGlobalScope`
less what this browser lacks, written in the test.

**The worker's own members** (worker.c in libdom, a modern row exposed to
workers alone): `WorkerGlobalScope` and `DedicatedWorkerGlobalScope`
prototypes (the global's prototype chain, so `self instanceof
DedicatedWorkerGlobalScope`), `self`, `name`, `location`
(`WorkerLocation`: `href`, `origin`, `protocol`, `host`, `hostname`,
`port`, `pathname`, `search`, `hash`, every one read-only, answered by
url.c's `d_url_part` over the worker's URL — the same function the
hyperlink mixin brief asks for), `navigator` (`WorkerNavigator`:
`userAgent`, `language`, `languages`, `onLine`, `platform`,
`hardwareConcurrency`, `appName`…, the page's `Navigator` less its
plugins, mime types and `serviceWorker`), `postMessage(message,
transfer)` and `postMessage(message, {transfer})` (write, pack, hand to
the host's `worker_post`), `onmessage`, `onmessageerror`, `onerror`
(HTML's `OnErrorEventHandler`: its five-argument special form),
`onlanguagechange`, `onoffline`, `ononline`, `onrejectionhandled`,
`onunhandledrejection`, `close()` (sets the closing flag: the current
task finishes, no further task runs, the host ends the worker), and
`importScripts(...urls)`: each URL resolved against the worker's URL,
fetched through the relay in order with the worker waiting, compiled
and run as a classic script in the global, in order, the first failure
thrown as a `NetworkError` (a fetch) or the script's own error; a call
after `close()` runs nothing and throws nothing, as the standard says.

**The `Worker` interface** (worker.c in libdom, a modern row exposed to
windows and workers both — a nested worker is out of scope, so in a
worker the constructor throws `NotSupportedError` until it is in):
`new Worker(scriptURL, {name, type, credentials})`: `type: "module"`
throws `NotSupportedError` (DEBTS); the URL is parsed against the
global's base, the host asked (§ The window's side), and the object is
an `EventTarget` with `postMessage(message, transfer)`,
`postMessage(message, {transfer})`, `terminate()`, `onmessage`,
`onmessageerror`, `onerror`. Its `postMessage` before the worker's
thread has started queues in the inbox like any other, and the worker
hears it after its script's first run, as the standard orders.

**The packed clone** (clone.c): `d_clone_pack(rt, clone) → bytes` and
`d_clone_unpack(rt, bytes, length) → DClone`. The item array is written
as a header and items; a primitive `value` becomes a tagged scalar
(undefined, null, bool, int32, float64, a BigInt's sign and digits, a
string's UTF-16 units — the writer keeps lone surrogates and so must the
pack); a `key` atom becomes its string; a `DData`'s bytes, type, name,
message and `modified` are copied; an error's `stack` string is a string.
Unpack rebuilds the items in the receiving runtime (atoms interned there,
strings made there) and `d_clone_read` is unchanged. A pack is charged
to no engine (libdom's own allocation, freed by the reader); its size is
the record's. A packed clone the reader refuses (a bad header, a
truncated record: no page can make one, only a bug) is a `messageerror`
at the receiving side, which is where HTML's deserialization failure
goes anyway.

**The blob URL store** (W0, url.c or a `blob_url.c`): `d_blob_url_create
(store, blob) → "blob:<origin>/<uuid>"` copies the Blob's bytes and type
into an entry the store owns (libdom memory, refcounted so an entry
being read by a fetch outlives its revocation); `revokeObjectURL(url)`
removes the entry when the URL's origin is the caller's; the store is
the page's (`os64_dom_blob_store_t`, made by yonder per Page, handed to
the window's binding and every worker's, locked, since a worker thread
writes it); yonder's fetch door asks `os64_dom_blob_url_take(store, url)`
before the pool for every request kind, serving the bytes with the
entry's type as a 200 answer (the standard's "blob URL fetch"), and a
missing or revoked entry is a network error. The uuid is `crypto.c`'s
`randomUUID`. A blob URL's origin is its inner origin (url.c already
answers it), so a page cannot mint one for another origin.

## Fetch (the relay)

A worker's binding has providers as the page's does; the worker's fetch
provider is yonder's `worker_fetch(ctx, request)`: the request is
copied (URL, method, headers, body bytes, mode, credentials, the
destination), appended to the window's inbox for that worker with a
`fetch` note, and `BELL_WORKERS` rung. The window's loop submits it as
the page's request through `request_job`, with the worker's owning Page
as the page, the worker's URL as referrer and `Sec-Fetch-Dest` as given
(`worker` for the script, `script` for `importScripts`, `empty` for
`fetch`), and a fetch pipe whose `ring` writes the worker's pipe. The
worker's loop, woken, reads the pipe's news and hands it to its own
binding's `os64_dom_fetch_head/_body/_end`, as the window does for the
page's requests — the same three verbs, a different thread. The job's
reaping stays the window's (work.h: release on the owner). A worker that
ends with a request in flight: the pipe's reader side is released by the
worker's teardown, the writer sees it gone and stops, the job is reaped
as any cancelled one. Blob and data URLs are answered by the window
before the pool, as the page's are.

`importScripts` and a synchronous XHR wait: the worker's loop runs a
nested wait on its pipe until that request's end arrives (or `closing`
is marked, which fails the wait with the standard's abort), taking
nothing else meanwhile; messages and timers queue behind. The page is
never waited on by the window, only the worker on the window, so the
window's loop cannot deadlock on a worker.

## Bounds

One constant each, with the reason, in worker.h:

- `YONDER_WORKER_THREADS` 4 per window (lean; § Rulings 3): the window,
  the pool's workers and the ticker have the P5's other hardware
  threads. A page past it queues.
- `YONDER_WORKER_MEMORY` 64 MiB per worker runtime (libjs's standalone
  default), against the page's 256 MiB; at four workers, the window's
  worst case grows by a quarter of a gigabyte, which is the number
  § Rulings 3 asks about.
- The stack: § The thread's stack.
- The task deadline: the page's (`execution_ms`), § What this settles 7.
- `YONDER_WORKER_INBOX` 4,096 messages per direction; a poster past it
  blocks nothing and loses nothing: the writer waits on the lock, the
  list grows, and the record counts the high-water mark (a page posting
  faster than it hears is its own problem, but the number is in the
  record so it can be seen).

## The record and the replay

The record (diag.c) gains one fact line, `workers: <n> started, <m>
ended, <k> terminated, <f> failed, at most <p> at once, <q> waited for a
thread`, and per-worker lines like a script's: `worker <address>: ran
<n> tasks in <t> ms, heap peak <h> MiB, stack <s> KiB` on end, and the
`FAILED worker <address>: <why>` lines § What this settles 7 and 8 name.
A worker's fetches are the page's fetch lines, marked `(worker)`. With
`keep_pages` on, a worker's script is kept as kind `script` with its
address — the index already has the kind — so a kept page replays with
its worker scripts answered from disk; a blob-URL script is kept with
the blob URL as its address and the bytes as they were. The replay
harness (`tools/test_yonder_replay_host.sh`) runs worker threads for
real on the host (pthreads behind the same `os64_thread_sized` shim the
pool uses there). `MISSING global Worker` leaves the census the day W1
lands.

## The oracle

- **WPT** at the runner's pin: `workers/` — `constructors/Worker/`,
  `semantics/structured-clone/`, `semantics/interface-objects/` (the
  exposure test: which names a dedicated worker's global has — the
  `[Exposed]` table's proof, with the expectation file listing what this
  browser lacks by name), `semantics/multiple-workers/`,
  `semantics/reporting-errors/`, `semantics/run-a-worker/`,
  `Worker_terminate*`, `Worker_dispatchEvent_ErrorEvent.htm`,
  `importscripts_*`, `WorkerLocation*`, `WorkerNavigator*`,
  `postMessage_*`, `name_property.html`; `workers/modules/` and
  `workers/shared*`, `workers/semantics/encodings`' service-worker
  pages excluded by name. `FileAPI/url/` for W0 (`url-format.any.js`,
  `url-with-fetch.any.js`, `url-with-xhr.any.js`, `url-revoke`,
  `url-in-tags.window.js`). The `.any.js` pages run in the worker
  scope too (the runner's `.any.worker.html` variants), which is how the
  exposure table is exercised row by row: every `.any.js` test this
  browser passes in the window must pass in a worker unless the
  expectation file says why.
- **DOM host:** a two-runtime, two-thread fixture under the sanitizers
  (the host build's pthreads): a worker binding made with no document,
  every exposed member touched once; the pack/unpack round trip over
  clone.c's whole cloneable list, with a mutant per tag; a message each
  way; `terminate` mid-task (the interrupt lands, the runtime frees
  clean under ASan); `close()` inside a handler; the deep-recursion
  stack case; the class-id rule (a worker made first, before any window,
  must fail by name, not by corruption); the blob store under two
  threads with a revocation racing a read.
- **Scripted-page host (yonder):** a page starting a worker, posting,
  hearing, terminating; the pool's queue (five workers on four threads);
  departure with a worker mid-loop; the fetch relay (a worker's `fetch`
  answered from the harness's wire, an `importScripts` of two files in
  order, a synchronous XHR); a worker's uncaught error reaching the
  page's `onerror` and the record; a blob-URL worker; the record's lines.
- **QEMU:** `make run-net` loading a page that starts a worker from a
  blob URL and shows what it posts back; then the P5 on the two
  challenge pages, by outcome and not by name (the CAPTCHA frame's
  checkbox wires, which IFRAME.md's F2 then lets it tell its page).

## The cut

| Cut | Builds | Oracle | Owner |
|---|---|---|---|
| W0 | blob URLs: the store, `URL.createObjectURL`/`revokeObjectURL` on window (and workers when they exist), yonder's door for every request kind, `<img src=blob:>`, `<a download href=blob:>` through the download path, `fetch`/XHR of one; the kernel's sized thread as a separate PR if Chris rules for it | `FileAPI/url/`; the scripted-page host's blob cases; a blob picture shown in QEMU | Opus; Chris for the kernel PR |
| W1 | the pool and the worker's loop (yonder worker.c); the binding's kinds and the exposure table; `Worker`, `DedicatedWorkerGlobalScope`, `WorkerLocation`, `WorkerNavigator`; the packed clone; messages both ways; `terminate`, `close`, departure, errors, the budget; the record and the kept script | `workers/` less `importscripts_*` and the module and shared pages; the DOM host's two-thread fixture; the scripted-page host's worker cases | Opus; **Fable reads** the pool's lifetimes, the packed clone and the class-id rule before Codex; the libjs owner audits libjs for two runtimes on two threads |
| W2 | the fetch relay: `fetch`, `XMLHttpRequest` (async and sync), `importScripts`, the pipe's reader on the worker's thread, blob and data answers, a worker ending mid-request | `importscripts_*`, `fetch/api/basic/*.any.worker.html` and the XHR pages' worker variants; the scripted-page host's relay cases | Opus; **Fable reads** the pipe's two-thread release before Codex |
| W3 | `MessagePort` transfer to a worker and (with IFRAME.md's F1) to a frame: a port's queue crossing runtimes as packed messages, entanglement across threads; lifts message.c's refusal once for both | `webmessaging/` with ports, `workers/` port pages | Opus, after F1; Fable reads |

W0 first and alone: it is small, 112 hosts call it with no worker in
sight, and both challenge pages need it before they need anything else
here. W1 and W2 may be built side by side off one branch and land in
order; W2 without W1 is nothing. Each is a PR to `userland` with the
three passes and Codex for W1 and W2 (threads and lifetimes are what the
budget is for; W0 and W3 are reviewed here). Each PR states its WPT
counts before and after, the `MISSING global Worker` count against the
records, and the corpus count; W1's states the P5's memory with four
workers idle and four busy, beside the page's own.

## Rulings

Three for Chris, each with my lean; everything else here is web
semantics, decided and told.

1. **The kernel lets a thread ask for its stack** (§ The thread's stack):
   a fourth argument on `SYSCALL_THREAD`, `os64_thread_sized` in libos64,
   `user_stack`'s existing clamp and fallback doing the rest. **Lean:
   yes, as W0's companion PR.** It is a dozen lines in a function that
   already has the parameter, it gives a worker the page's 2 MiB instead
   of half a megabyte, and it pays SCRIPT_STACK.md's "a spawned thread
   keeps 1 MiB" for the next program that wants otherwise. The design
   stands without it; the record says which a worker got.
2. **A worker's task has the page's deadline** (§ What this settles 7),
   where the standard gives it none. **Lean: the page's.** A worker that
   spins for ever is legal elsewhere and costs a core on the P5 until
   the page is left; the page's `script seconds` is the one knob Chris
   already turns, and a worker whose honest work exceeds it (a decoder,
   a compressor) will show in the record as `over its budget` with its
   address, which is the moment to raise the knob or give workers one of
   their own. The alternative is a worker budget of its own, higher,
   which is a second knob for a case nothing has shown yet.
3. **Four threads and 64 MiB a worker, per window** (§ Bounds). **Lean:
   as stated,** measured on the P5 in W1's PR with four busy workers,
   and revised there if the machine says so. The alternatives are a
   smaller pool (two, which the CAPTCHA and one analytics worker would
   fill) or a larger memory limit (the page's 256 MiB each, which four
   workers would turn into a gigabyte).

## Risks named

- **Two runtimes, one process.** QuickJS runtimes are independent; libjs
  and libdom were written for one. The class-id rule is the known
  shared static; the audit (W1's libjs line) is for the unknown ones,
  and the two-thread DOM host fixture (the host build already links
  `-pthread` under ASan and UBSan; TSan is a flag away for this one
  fixture) is the proof that outlives the audit.
- **The pack's fidelity.** A value that survives write/read but not
  write/pack/unpack/read is a bug a page would see as a changed message.
  The round-trip test runs clone.c's whole cloneable list through both
  paths and compares; the mutant per tag keeps it honest.
- **A worker's end racing its page.** The window holds the `Worker`
  record and both mailboxes until the thread marks `ended` and the
  window has taken the note; the thread frees its runtime and binding
  alone; nothing is freed by two sides. The DOM host's terminate-mid-task
  case and the scripted host's departure case carry it under ASan.
- **The pool's queue starving.** Four long-lived workers (an analytics
  worker never ends) hold the pool, and a fifth page's worker waits for
  ever with no error, which is silent. The record's `waited for a
  thread` count and the per-worker `ran n tasks` line are the
  instrument; if the P5 shows it, the pool grows or an idle worker is
  evicted, and the row says which.
- **A page that measures `hardwareConcurrency` and starts that many
  workers at once** gets four, which is what the answer promised; a page
  that starts sixteen regardless queues twelve, and its progress bar
  waits for them. Common enough in benchmark pages, rare in the corpus.
