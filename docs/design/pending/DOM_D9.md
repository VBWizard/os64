# DOM_D9.md — document.write: the script that types into its own parser

*Written 2026-10-05 by Fable, as the builder's brief for slice D9 of
[DOM.md](DOM.md) § Slices, to be built by Opus with Fable reviewing. Read
against `opus/dom-d7b` at `cdf73877` (D7a and D7b built, PRs #226 and #227
in review): D9 is stacked on D7b and assumes it. Names of functions that do
not exist yet are working names; the code settles their spelling, and this
file's "as built" section goes into DOM.md with the PR. Where this file and
DOM.md or `html.h`'s contract disagree, those win and the disagreement is a
finding against this file.*

## What D9 is, and is not

The old web yonder exists to read is full of `document.write`: hit
counters, dates, banners, "last modified" lines, and the script loader
`document.write('<script src="...">')` that predates every other way of
loading a script. DOM.md § The parser with scripting on settled its shape
in one paragraph: it is `os64_html_parser_write(p, utf8, len)`, which
tokenizes the text at the insertion point ahead of the held input and may
itself stop at a script the text contained; it is legal only while the
parser is stopped at a script; after the parse has ended it would mean
"throw the document away and start a new one", which is refused and
booked. D9 builds exactly that: the parser's half in libhtml, the
binding's half in libdom (replacing D7a's throw), and the two lines that
connect them in yonder. The J4 walk's `write.html`, which today fails by
name, is the page that proves it.

**What D9 does not do**, each deliberately and each with a row:

- **`document.open()` as a new document.** The standard's `open()` with no
  parser running replaces the document with an empty one. yonder reads
  the page it was given. `open()` while the parser is stopped at the
  running script answers the document and changes nothing, which is also
  the standard (a script-inserted parser cannot be opened over). Any other
  `open()`, and any `write()` with no insertion point, throws
  `InvalidStateError` naming the reason. DOM.md's booked row stands.
- **A written script runs after the writing script, not inside it.** The
  standard runs an inline `<script>` that `write` produced synchronously,
  nested in the writer; D7 ruled a script a verb connected runs at the
  next task because libjs refuses nested top-level evaluation by contract
  (DOM_D7.md § Booked), and a written script is the same case. The
  document comes out in the same order either way (below, § Why the order
  is the standard's); what differs is that the writer's own code after
  the write runs before the written script's. Booked with its trigger.
- **A late `<meta charset>`** written by a script changes nothing: the
  encoding was chosen from the sniff window before any script could run,
  and libhtml never re-parses (README.md). The standard's "change the
  encoding" step would restart the navigation; this browser does not.
- **No new verb, no new snapshot rule.** Written text is INPUT: it goes
  through the tokenizer and the tree builder like bytes off the wire,
  charged to the same budgets and moving the version the same way. Nothing
  in libpage, libgarb, libflow or the renderer changes.

## The parser's half: `os64_html_parser_write`

**Today's input order** (`h_pump`, core.c): the sniff window's replay,
then the hold, then the caller's own buffer. A parse stopped at a script
takes no byte further; what is fed meanwhile joins the hold.

**After D9 there is one more source ahead of them all: the written text.**
It is a heap buffer like the hold (input, not tree: the arena is for what
is built, and `max_bytes` bounds both), holding UTF-8, with a cursor
(`written_at`) and ONE insertion index (`insertion`). The order becomes:
written text from the cursor, then the sniff window's remainder, then the
hold. A script in the first 1,024 bytes stops the call that fills the
window, so by the time any script runs the encoding is chosen and the
window may still be mid-replay; written text goes ahead of the rest of
the replay too, which is why it is a source of the pump's and not a
prefix of the hold.

**Decoding.** The document's decoder (`h_decode`, per byte in the
document's encoding) is not used for written text: a script's string is
UTF-8 whatever the page was served in. Written text is decoded by the
parser's own UTF-8 rule into code points and handed to `h_codepoint`, so
CR/LF folding, the input-stream error checks and the work charge are the
tokenizer's as for any input. The text is validated as the verbs validate
theirs (`OS64_HTML_BAD_TEXT`: not UTF-8, or a NUL), and refused whole
before anything is tokenized. (libdom already turns a NUL and a lone
surrogate into U+FFFD before a verb sees them; the parser checks anyway,
as `set_text` does.) `p->offset`, the position error messages carry, is
the input byte count at the insertion point for every written character:
written text has no byte offset of its own, and a number that is at least
not garbage is what a diagnostic can carry.

**Who may write, and where it goes.** The parser is stopped at script S
(`p->script == S`). The host runs S. The parser learns that S is RUNNING
at S's first write: `running = S`, `insertion = written_at` (the cursor:
just after S's end tag, which is the standard's "just before the next
input character"). Each write by the running script inserts its text at
`insertion` and advances `insertion` by its length, then pumps from the
cursor TO THE INSERTION INDEX AND NO FURTHER: the standard's tokenizer
runs "until it reaches the insertion point" and the hold must stay
behind it, because the writer may write again. That pump stops early at
a script the text contained, W: then `p->script = W` and the call answers
`OS64_HTML_SCRIPT`; `os64_html_parser_script` names W from then on. S is
still `running`, and a later write by S still inserts at S's insertion
index, which lies beyond W and beyond everything S wrote before, so its
text waits behind W as the hold does. A write while stopped at W answers
`OS64_HTML_SCRIPT` again, as a `feed` while stopped does.

**`resume` is the host saying "the script I was running is done."** Today
it clears `p->script` and pumps. After D9:

1. If `running` is set, the run is over: `running = NULL`. If `p->script`
   is still that script (it wrote, and nothing it wrote stopped the
   parse), clear it as today. If `p->script` is another (a written W found
   during its writes), leave it: W is the parse's next stop and the call
   answers `OS64_HTML_SCRIPT` at once, pumping nothing, so the host runs W
   through the same door it ran S.
2. Otherwise (the script never wrote), clear `p->script` as today.
3. Pump: written text from the cursor to the end of the buffer (there is
   no insertion index now: nothing is running), then the window's
   remainder, then the hold. The written buffer is freed when the cursor
   reaches its end, as the hold is.

Only one script is ever `running`, because the host runs one task at a
time and a written script runs after its writer (above). So the insertion
bookkeeping is one index and one pointer, not a stack; the day a nested
run exists, the index becomes a stack and the pump's bound the top of it,
and the shape here is the base case of that.

**Why the order is the standard's.** S writes `<script>W</script>REST`
and then writes `MORE`. The standard: the tokenizer reaches W's end tag,
runs W nested with the insertion point just after W's end tag (W's own
writes go there), restores the insertion point to after REST, tokenizes
REST, returns from the first write; the second write inserts MORE after
REST. Here: the tokenizer reaches W's end tag and stops; MORE goes after
REST at S's insertion index; S's task ends; W runs as the next stop with
its insertion index just after its end tag (its writes go there, ahead of
REST); the pump then runs W's text, REST, MORE, then the hold. The same
input stream in the same order, W's run moved from inside S's write call
to after S's task. The proof holds the serialised tree of every such
page equal to a browser's.

**`end`, `finish`, `abandon`.** A script found by `end` (one in the hold
after the input ended) may write like any other; `settle` already sends
the EOF only when the parse is not stopped and the pump has drained what
waits, and the pump now drains written text first, so a page that writes
in its last script is complete before the end-of-input steps. `finish`
(straight through) pumps the written text with the rest; `abandon` frees
the buffer. A `write` after `end` answered OK, or on a parser that is not
stopped at a running script, is `OS64_HTML_BAD_ARGUMENT` and changes
nothing, like a `resume` of a parse that is not stopped.

**Budgets and refusals.** Written bytes count against `input_bytes` and
so against `max_bytes` (a page that writes in a loop forever meets the
same wall as a page that never ends; the `cut` rule and the TOO_LARGE
refusal are the same ones). A write the heap cannot hold is
`OS64_HTML_NO_MEMORY`. A written tree deeper than the limit is
`OS64_HTML_TOO_DEEP`. Each refusal is the parse's: it is recorded in the
document as today, the document stays readable, and every later call
answers it, so the host's `resume` after the task sees the refusal and
cuts the stream short through the road it has. A refusal is NOT an
exception for the script: the standard's `write` does not throw for size,
and a page cut short is a page you can read the beginning of, which is
what a refused parse has always meant. The version moves once per `write`
that tokenizes anything (`h_moving`), before the first thing it parses.

**The contract block** in `html.h` under THE PARSE THAT STOPS gains the
paragraph above in its own words: who may write, where it goes, what
`resume` means, what a refusal is, and that written text is input and not
tree.

## The binding's half, in libdom

D7a left `document.write` and `writeln` throwing by name (`window.c`,
`W_WRITE`). D9 wires them:

- **The host callback.** `os64_dom_options_t` gains
  `int64_t (*write)(void *opaque, const char *utf8, size_t length)`: the
  host is asked to hand the text to its parser and answers libhtml's
  status. `OS64_HTML_OK` and `OS64_HTML_SCRIPT` are success (the script
  learns nothing of a stop; the host does). `OS64_HTML_BAD_ARGUMENT` means
  there is no insertion point, and libdom throws `InvalidStateError`:
  "document.write after the parse has ended would replace the document,
  which this browser does not do" — one sentence, one place. A refusal
  (negative, other than BAD_ARGUMENT) is the parse's and not the script's:
  the call returns undefined and the host acts after the task. A NULL
  callback (a host with no parser: wend's harnesses, the shown page's
  binding) is BAD_ARGUMENT.
- **The arguments.** Every argument is converted to a string with
  `ToString` and concatenated, as the standard's `write(...text)` is;
  `writeln` appends a newline after them, even with no arguments. The
  concatenation is one allocation charged to the binding's budget
  (`QuotaExceededError` past it, nothing written), freed after the call,
  and the host borrows it for the call only — the parser copies what it
  keeps.
- **`document.open()`** answers the document when the host's callback
  says a write would be legal (ask it with a zero-length write, which
  must tokenize nothing and change nothing; or a second callback if the
  builder prefers one that asks without writing — record which), and
  throws `InvalidStateError` otherwise, with the sentence above.
  **`document.close()`** is a no-op: the standard's `close` acts only on a
  parser that `open()` created, and none exists here.
- **`document.write` from inside a listener or a timer** on the arriving
  page reaches the host with no running blocking script, and is refused:
  the standard would open a new document there too (its script nesting
  level is zero). This is the rule, not an accident of the host's flag.

LIBDOM.md gains a short § `document.write` saying the callback is the
whole of the binding's knowledge of parsing.

## The two lines in yonder

The script host (`scripts.c`) runs the blocking item under `os64_js_run`
(`run_item`). D9 adds: while that run is in progress, the host REMEMBERS
it (a `writing` flag or the running item's index; the builder's call) and
its `write` callback forwards to `options.write` — a new member of
`yonder_scripts_options_t`, which `yonder.c` points at the stream's
parser: `os64_html_parser_write(g.stream.parser, utf8, length)`. A write
that arrives with no blocking item running, or from the shown page's host
(no parser), answers `OS64_HTML_BAD_ARGUMENT` without touching anything.

Nothing else in the loop changes, and that is the point of D7's protocol:
after the task, `stream_turn` calls `os64_html_parser_resume` as it does
today and hands the answer to `stream_parsed`; a written script arrives
there as `OS64_HTML_SCRIPT` naming W, `stream_stop(W)` registers it
(BLOCK for a classic script, inline or `src`; RESUME for a module or a
VBScript, which resumes through the same call, which is step 2 of
`resume` above), and the next turn runs it. A refusal arrives as a
refusal and `stream_cut_short` does what it does. The `--script-audit`
line for a task gains the number of bytes it wrote, so a page that writes
its whole body is visible as such.

`write.html` in `tools/yonder_loop_fixture/` becomes the page that works:
a counter written into the page, a date line, a `document.write`d
`<script src>` that loads `blocking.js` and whose effect the page then
shows, and a written inline script whose own write lands before the
text that followed it.

## What a reviewer should read with a question in mind

- **The insertion index against the cursor:** a second write by S after
  a stop at W must land after everything S wrote, not at the cursor; and
  W's first write must land at the cursor, not at S's old index. The
  harness case for each is the one to read.
- **The pump's bound:** during a write, the pump stops at `insertion`
  and never reads the window's remainder or the hold; during `resume` it
  reads to the end of the written buffer and on. A mutant that drops the
  bound parses the hold under a running script, which the "writer sees
  its own text only" case must catch.
- **`resume`'s three outcomes** (script wrote and nothing stopped; script
  wrote and W stopped; script never wrote), each a case.
- **Refusal inside a write** leaves the document readable, the script
  running, and the next parser call answering the refusal.
- **`abandon` with written text pending** frees it (LSan), and `finish`
  parses it.

## The proof

On the host first, in the house's shape:

- **`tools/test_html_host.sh` / the corpus driver** (`test_html_driver.c`,
  whose `run_scripts` hook already takes each stop "in the script's
  place"): the hook learns to interpret the one shape the scripted
  tree-construction corpus uses, `document.write(<string literal>)` with
  the literal's escapes, and lists by name any case whose script is
  anything else (a skip row in `SKIPS.tsv`, with the reason, as the
  existing skips are listed). `tools/update_html_fixtures.py` learns to
  pin the `tree-construction/scripted/` directory of html5lib-tests at the
  commit the rest is pinned at, and the `#script-on` cases there join the
  corpus. Acceptance is the parser's own bar: every newly in-scope case
  passes, whole, byte at a time and in random chunks (a write's text is
  never cut, but the bytes around the script are).
- **Hand cases in the driver**, one per rule above: a write with no
  script running refused; text written by S tokenized before the hold;
  two writes by S in order; a written W stopping the parse, with S's
  later text behind it and W's own write ahead of it; a written `src`
  script named by the stop; a write of a NUL refused as BAD_TEXT with
  nothing tokenized; a write past `max_bytes` refused as TOO_LARGE with
  the document readable; a write that nests past the depth limit refused
  as TOO_DEEP; `resume`'s three outcomes; `end` then a write in the last
  script then EOF after it; `finish` with written text pending; `abandon`
  with written text pending under the allocation ledger; the version
  moving once per writing call and not for an empty one; and the
  serialised tree of each case equal to the same text parsed whole with
  the writes spliced in by hand (the oracle: the order argument above,
  made executable).
- **Mutants** (a sibling of `tools/test_html_reclaim_mutants.py`, for the
  parser): the pump's bound, the insertion index's advance, the cursor
  reset, the running pointer's clear, the BAD_ARGUMENT guard, the
  BAD_TEXT check, the budget charge, the version move, the buffer freed
  at the cursor's end, the UTF-8 decode (a 4-byte character written).
- **`tools/test_dom_host.sh`**: `write` and `writeln` reaching the
  callback with the concatenated text (two arguments, a number, no
  arguments to `writeln`), the host's BAD_ARGUMENT as `InvalidStateError`
  with the sentence, a refusal returning undefined, the budget refusal,
  `open()` both ways, `close()` a no-op, a write from a timer refused;
  the binding allocation sweep over `write`.
- **`tools/test_yonder_scripts_host.sh`**, through the real stream with
  the test playing the worker: a page whose head script writes a `<p>`
  that the arrived tree contains before the body's own; a written
  `<script src>` fetched (the fake pool answers it) and run before the
  parser continues; a written inline script whose write lands before the
  text after it; a write from a timer of the arriving page refused and
  the parse unharmed; a write from the shown page refused; a write that
  cuts the parse short arriving with the parser's sentence; the audit
  line's byte count; teardown mid-stream with written text pending (Stop,
  the switch, the close) under LSan.
- **Consumers unchanged**: wend (scripting off never writes), libpage,
  libflow, libgarb, the painter, the stream ring harness, D7's loop
  mutants, all 0 failed.

Then the guest, on a scratch copy of the image, never Chris's, with
`scripts = on`: `write.html` as described, screenshot showing the
counter, the date, the loaded script's effect and the written script's
text in order; the rest of the D7 fixture pages and D4's walk unchanged.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| A written inline script runs after its writer's task, not inside the write call | libjs refuses nested top-level evaluation; the document order is the same | a page whose writer reads what its written script defined, in the same script |
| `document.open()` as a new document, and `document.write` after the parse ended | it replaces the document (DOM.md's row) | a page that needs it |
| A written `<meta charset>` | the encoding is chosen before any script runs, and libhtml never re-parses | none expected |
| A stack of insertion points | one script runs at a time; the index is the stack's base case | a nested run, which the row above would bring |

## Ruled by Chris, 2026-10-05

Both decisions above were put to Chris with their alternatives and he
took the lean on each: a written inline script runs after its writer's
task ("if it becomes an issue we'll address it then"), and a parse
refusal inside a write is the page's, not the script's. Opus builds
against the design as written.
