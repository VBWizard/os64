# LOAD_EVENTS.md — `load` and `error` at link, style and script elements

*Opus 5.5, 2026-11-06, from Fable's brief of the same day
(`~/os64-opus-kit/briefs/LOAD_EVENTS_LINK_SCRIPT.md`). Fable reviews this
before any code. § 4 lists where it departs from the brief and why; the
rest is the design as I would build it.*

DEBTS rows this pays: Quick wins "A stylesheet `link` and a script element
fire no `load` or `error`"; Classic browser scripting "A script element
fires no `load` or `error`" and "A stylesheet link fires no `load` or
`error`"; ES modules "A script element fires no `load` and no `error`".
Size S–M. One branch off `origin/userland`, `opus/load-events`.

## 1. The page, read again

A social network's logged-out front end draws blank on the P5. Its newest
corpus record (11/05) settles the one question the brief left open, which
was where the stylesheet's `<link>` sits relative to the loader:

- The loader is an **inline head script**, and the parser reaches it
  **before** the `<link rel=stylesheet>` that follows it. The loader first
  checks `getElementsByTagName('link')` for its sheet (not there yet), then
  adds capture-phase `load` and `error` listeners on `document`, then
  removes itself (`document.currentScript.remove()`).
- Its handler checks `event.target.tagName === 'LINK'`, `href` and `rel`,
  removes both listeners, and inserts an async `<script type=module>`, the
  page's only entry module. It does the same on `error`.
- The record's kept index has one entry: the sheet, `came`. No script and
  no module were asked for.

So the runtime exists before the sheet lands (the inline script made it),
and the listener is on `document` in capture. What this page needs is
exactly a trusted, non-bubbling `load` at the link after its sheet
arrives. What the entry module needs after that is unknown until it runs.

## 2. What the standard binds

- **`link rel=stylesheet`** (HTML § 4.2.4, "process the linked resource"):
  `load` at the link once the sheet is fetched and created; `error` when
  the fetch failed or the response is not a usable sheet. A sheet whose
  `media` does not match is still fetched and still fires (yonder fetches
  it too: `media` decides only whether first paint waits for it,
  `sheet_holds`). A failed `@import` leaves the link's event `load`. An
  `alternate` sheet is not fetched and fires nothing (libpage leaves it
  out). An href that does not parse fetches nothing and fires nothing.
- **`style`** (§ 4.2.6): `load` at the element once its sheet is made.
- **`script`** (§ 4.12.1.1, "execute the script element"):
  - result null (fetch failed, a module graph that failed) → fire `error`
    at the element and stop;
  - otherwise run it; then, **in the same task**, after the run's
    microtask checkpoint and after `currentScript` is put back, if the
    element is *from an external file*, fire `load`. A script that threw
    still fires `load`. An inline script, classic or module, never fires
    `load`.
  - `src` present but empty: queue an element task to fire `error`. An
    import map with `src`: the same.
- Both events: not bubbling, not cancelable, trusted.
- **The path.** DOM § "get the parent": a Document's parent is the Window
  *except for an event whose type is `load`*. So an element's `load`
  reaches a capture listener on `document` and never one on `window`; an
  element's `error` reaches both. libdom already does this
  (`event.c`, `to_window` excludes `D_EVENT_LOAD`), and the capture loop
  runs over the whole path whether or not the event bubbles.

## 3. The design

### 3.1 Scripts fire from `run_item`, in their own task

`run_item` (scripts.c) is the script's task. It gains two dispatches at
the end, through one internal helper:

- **`error`**: an item that reaches its turn as `STATE_FAILED` (that is,
  `fail_item` put it there) and whose element is from an external file or
  is a module. The failure is reported as it is today, then `error` fires.
- **`load`**: an item whose element is from an external file, after it
  ran (classic: after `os64_dom_script_ended`, `set_current_script(NULL)`
  and `judged`; module: after `run_module` returns), whether or not it
  threw.

*From an external file* is HTML's flag, set when the script is prepared
with a `src` attribute: the `Item` gains a `bool external` set in the
prepare paths that choose `src_fetch`. An item with no element (a link's
script, `MODE_ADDRESS`) fires nothing. An item `run_item` skips because it
left the document fires nothing, as it runs nothing (§ 6, question 3).

The helper is `yonder_scripts_dispatch`'s body without the task reset: the
dispatch is nested in the script's task, and its outcome is folded into
the task's outcome by the rule libdom's `d_outcome_fold` applies to a
nested dispatch, so a throwing `onload` is reported as that task's.

### 3.2 Sheets owe their events to a ring in the scripts host

A sheet's verdict lands in yonder's loop (`sheet_arrived` from the work
pool, `sheet_inline` from `sheet_add`), never inside a script, so its
event is already after whatever script caused the fetch. It is owed, not
dispatched on the spot, so that it runs as a task of its own in the loop's
order, beside the image events, and never from inside `sheet_arrived`
while the sheet table is being changed.

- **Where the ring lives:** `yonder_scripts_t`, as `owed[]` of
  `{node, load|error}`, growable like the item table. The host already
  holds nodes against its document (`os64_html_hold`) and is what moves
  with a Page from stream to shown (`fresh.scripts = g.stream.scripts`),
  so the ring needs no crossing site of its own.
- **`yonder_scripts_owe(s, node, failed)`** holds the node and appends.
  A node is fired at even if it has left the document since (HTML fires at
  a detached element too), and released after.
- **`yonder_scripts_owed_fire(s, out)`** dispatches the oldest, through
  `yonder_scripts_dispatch` with `bubbles = false`. It is drained at the
  two sites that drain image events (`scripts_task`, and the shown page's
  turn), right after them; `yonder_scripts_images_pending`'s caller (the
  loop's "has a task" test) asks for owed events too, or the loop would
  sleep on one.
- **Which host:** the page's own, else the one serving its serial
  (`page_scripts(p)` then `scripts_for(p->serial)`, which is what
  `adoptions_host` already does). No host, as with scripts off: nothing is
  owed.
- **The verdict sites:**
  - `sheet_arrived`: `got->ok` → owe `load` after `sheet_ready` (so the
    cascade already has the sheet when a handler measures). Not ok → owe
    `error`. `got->ok` is false for a non-2xx status, a non-`text/css`
    type where the type is checked (sheet.c), and a sheet yonder could not
    parse (too big, no memory): the last is yonder's limit, and `error` is
    the honest answer to a loader waiting on it.
  - `sheet_inline`: owe `load` after the parse, `error` when the parse is
    refused. (Both paths in `sheet_inline` that return early without a
    parse, out of memory, owe `error`.)
  - Only a Sheet with an element (`sh->node != NULL`; an `@import`'s sheet
    has none, a constructed sheet has none), and never on a measuring page
    (`p->measuring`: the page a geometry read builds while a script is
    running, which parses a `style` the script just inserted; the shown
    page parses it again at its rebuild, and owes then).

### 3.3 Each element is told once per address, across rebuilds

`sheets_restage` and the script rebuild throw the sheet table away and
build a new one from the refreshed model, and their reuse paths call
`sheet_ready` again for the same elements: `sheet_inline`'s reuse branch
by node, `sheet_fetch`'s by URL. A sheet whose fetch failed is not reused;
it is fetched again and fails again. Owing at every `sheet_ready` would
fire `load` once per rebuild on any page that rebuilds, which a
script-heavy page does constantly.

So each Sheet carries **`told`**: its element has had its event for the
address it asked for. `sheet_add` copies `told` from the reuse table's
Sheet for the **same element**, when that one asked for the **same
address** (the address before redirects; the Sheet keeps it, since
`sh->url` becomes the redirected one). The verdict sites owe only when
`told` is false, and set it.

Keying on the address is also the standard's answer for a changed `href`:
a new fetch is a new event. Keying on the element keeps two links with one
href (which share a parse through `sheet_fetch`'s reuse) each told.

### 3.4 A page that has no runtime yet

The brief asks for a rule at the drain: with no runtime, fire only at an
element carrying the matching `on<type>` content attribute, making the
runtime; otherwise drop. **That rule already exists, page-wide,** in
`yonder_scripts_dispatch`: with `s->dom == NULL`, it returns at once
unless the document has an `on<type>` attribute for this type
(`handler_types` & `os64_dom_handler_bit`), and makes the runtime if it
does. The owed drain goes through that function and inherits the rule;
nothing new is written. It is page-wide where the brief's was
per-element, so an `onload` on an image makes a sheet's `load` dispatch
too. That costs a dispatch nobody hears, and keeps one rule.

`<link rel=stylesheet media=print onload="this.media='all'">` on a page
with no other script is the case it serves (§ 7, proof 3).

### 3.5 What is not here

- The window's `load` waiting for pending sheets (other browsers wait).
  Its own row if the WPT pages hang on it.
- `modulepreload`: DEBTS' row stays. The ring would make its events a few
  lines, but reporting a script-inserted preload link is the larger half.
- A script removed from the document before its turn: yonder skips it
  (`run_item`'s comment); HTML runs a removed external script. Its own
  question (§ 6).
- A link removed and inserted again: HTML fetches and fires again. Whether
  the table keeps the removed link's Sheet decides what yonder does; not
  reached by any page we have, and not chased here.

## 4. Where this departs from the brief, and why

| # | The brief | This design | Why |
|---|---|---|---|
| 1 | Every event is "a task of its own"; scripts owe to the ring like sheets | Scripts fire inside `run_item`, the script's own task (§ 3.1); only sheets use the ring | HTML fires a script's `load` at the end of "execute the script element", in the same task as the run. Through a ring drained one a turn, a timer or the next script can run between a script and its `load`. WPT's `load-error-events` pages check that a parser-inserted script's `load` comes before the next script runs |
| 2 | "Each verdict site owes exactly once" | Each element owes once per address, remembered across rebuilds (§ 3.3) | The rebuilds call `sheet_ready` again for elements already told, and fetch failed sheets again. Owing at the verdict sites alone would fire `load` on every rebuild |
| 3 | The ring on the Page, travelling to `g.page` where per-page state crosses | The ring in `yonder_scripts_t` (§ 3.2) | The scripts host already moves with the Page and already holds nodes. A Page ring adds a crossing site the host does not need |
| 4 | `fail_item` → owe `error`, every reason | `error` only for an external or module item; nothing for an inline classic script that hit a yonder limit, or an item with no element (§ 3.1) | HTML never fires anything at an inline script. The limits (too long, no memory) are yonder's, and a script that never had a `src` has no loader waiting on its `error` |
| 5 | A new no-runtime rule at the drain, per element | The existing page-wide rule in `yonder_scripts_dispatch` (§ 3.4) | It is already written and already governs image events. One rule, not two that drift |
| 6 | Proof: the `load` reaches a capture listener on `document` **and on `window`** | On `document`, and **not** on `window`; `error` reaches both (§ 2) | DOM § "get the parent" stops a `load` at the document. libdom already does; the test proves it stays so |

Also, a comment the design makes false: `src_fetch`'s "HTML then fires
`error` at the element, which nothing here listens to" changes in the
same commit.

## 5. Order of events, stated once

- A script's `error` or `load` is in the script's own task, after its
  microtasks, in the order the scripts run.
- A sheet's `load` or `error` is a task of its own, in the order owed,
  drained one a turn after the image events. A sheet answered at once (a
  `style`, a reused parse) is still owed and still waits for the loop. The
  tables that owe are built by the loop (`sheets_start` at arrival, the
  rebuild after a script's turn, `sheets_restage`), never inside the script
  that inserted the element; the one table built inside a script is the
  measuring page's, which owes nothing (§ 3.2).

## 6. Questions for Fable

1. **The nested dispatch's outcome** (§ 3.1): fold the `onload` dispatch
   into the script's task outcome, or report it as a task of its own in
   the record? I lean to folding: it is one task in HTML, and the
   record's per-task line should not show two.
2. **The page-wide no-runtime rule** (§ 3.4): accept its cost (a dispatch
   nobody hears on a script-less page with any `onload` attribute), or
   narrow it to the element? I lean to keeping the one rule.
3. **A removed script** (§ 3.5): leave yonder skipping it (and firing
   nothing), or is this the slice that makes a removed external script run
   and fire, as HTML has it? I lean to leaving it and booking it if it is
   not already.

## 7. Proof

- **libdom host:** a trusted non-bubbling `load` at an element reaches a
  capture listener on `document`, no bubble listener on `document`, and
  no listener on `window`; an `error` reaches a capture listener on
  `window` too; an `onload` content attribute runs.
- **yonder scripts host, end to end:**
  1. the loader's shape (§ 1): an inline script adds a capture `load`
     listener on `document`, a parser-inserted `<link>` follows, the
     handler inserts a `<script type=module>`, whose run is the pass;
  2. the same with the sheet a 404: `error`, same listener;
  3. `<link … media=print onload="this.media='all'">` with no other
     script on the page: the media flips;
  4. `<style>` fires `load`;
  5. an external classic script's `onload` fires after it ran, and when
     it threw; an inline script's does not; a parser-inserted script's
     `load` comes before the next script runs;
  6. `<script src="">` fires `error`; an external script whose fetch 404s
     fires `error`, not `load`;
  7. a reused sheet (a rebuild, and a second navigation with the cache)
     fires once, and not inside the inserting script (a flag the inserting
     script sets after `appendChild` is set when the handler runs);
  8. a `href` changed to another sheet fires again.
- **Allocation sweep** over `yonder_scripts_owe` (the ring's growth and
  the hold), proven with a hand mutant.
- **WPT:** `the-script-element/module`'s exclusion "element's load and
  error events" lifted and its pages run; the classic
  `the-script-element/` pages that test `load`/`error` added; whatever
  `document-metadata/the-link-element` and `the-style-element` have for
  `load`. Any line that passed before for the wrong reason is re-read.
- **QEMU** navigate (a wire-facing yonder change), then **the P5**: the
  social network's logged-out page before and after, counts only.
