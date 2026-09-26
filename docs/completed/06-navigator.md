# 06 — The navigator, out of `wend` and into a library

## Completed — 2026-09-25

Delivered as yonder's slice Y2 (branch `opus/yonder-nav`,
[YONDER.md](../design/pending/YONDER.md)): **`userland/libway/`**, a
browsing session apart from whatever draws it. `wend.c` is now its terminal
half and nothing more.

**The name is libway** (Chris, 2026-09-25, from Opus's slate; libtrail
and libcourse were the runners-up). The idiom is *to wend one's way*: the
text browser is the verb, the graphical one is where you are going, and
the session is the way between — where you have been, which roads it will
take, and when it stops to ask before crossing into the clear. `wend`
links libway and `yonder` links libway, so the family reads as a sentence.

- **The two halves** are `load.c` (`way_load`, the only caller of libfetch)
  and `session.c` (pages, the history, `way_judge`, `way_refresh_step`,
  `way_typed_address` — pure, and driven by `tools/test_way_host.sh`: 64
  checks under ASan, every judgement and sentence asserted, thirteen
  mutations all killed, and an allocation failure at every allocation of a
  walk through each allocating path).
- **Departure: THREE callbacks, not two.** `confirm` and `cancelled` as
  specified, and `progress` — wend paints "fetching ..." and "reading N KB
  of ..." while a page loads, and the acceptance rule was that nothing wend
  shows may change.
- **Departure: `go` and `back` are assembled by the face.** Laying a page
  out is the face's, and a page replaces the one on screen only once it has
  been laid out, so the library supplies the pieces — `way_load`,
  `way_remember`, `way_last` / `way_forget_last`, `way_may_go` — and the
  rule "built beside, replaced on success" is a few lines in each face.
- **The sentences name the browser** through `session->name`, so wend's
  read exactly as they did and yonder's will say yonder.
- **The acceptance** was a walk of wend through every rule against a local
  server (a page each for a fragment, gopher, mailto, a 404, text/plain,
  a picture, an immediate redirector, a delayed refresh, a chain of
  seven, a self-refresh, a GET form, a POST form, a typed address, reload,
  back and leave): 26 screendumps before the hoist and 26 after, identical
  outside the kernel's heartbeat cell, and the server's access log
  identical — 25 requests, the chain stopping at its sixth page, no POST.
- **Then packet 05 arrived** (#140, #141) and its POST moved in with it:
  `way_judge` no longer refuses one, `way_load` sends the page's body,
  a redirect that would replay a POST in clear asks "Resend form data
  unencrypted", and a non-page reply is judged on the FINAL method (a
  POST's reply cannot be saved by fetching its address). The walk's POST
  step now reaches the server, and Back to it later fetches its address
  by GET, as the help says; the other 23 screens are unchanged.

The original planning handout follows as history.

---

*BROWSER.md promised the graphical browser would inherit "the fetch, the
parse, and the navigator". The first two are libraries. The third is about
two thousand lines of `userland/apps/wend/wend.c` in which the rules about
a SESSION — where you have been, how a page is loaded, when a person is
asked before something is sent — are interleaved with the rules about a
TERMINAL. Chris's ruling, 2026-09-24: what is not about the terminal comes
out, yonder links it, and `wend` is retrofitted onto it afterward.*

## What is in wend.c today, sorted

**The session (comes out):**

- `load` — the fetch options every page gets (`WEND_AGENT`, `WEND_ACCEPT`,
  `max_body` = libhtml's cap, the cancelled and hop callbacks); the
  content-type dispatch (HTML and XHTML and a reply naming no type →
  libhtml; any other `text/*` → raw text; anything else → "not a page,
  save it with os64get"); the charset rule for raw text (the reply's
  label, else a leading UTF-8 BOM, else windows-1252); the model build
  (`os64_page_build` against `head->url_text`); and the SENTENCE about an
  incomplete page (short of memory, a wire that stopped, a parse the
  library refused by size), composed once and shown by the face.
- `go` — build the new page BESIDE the old one and replace only on
  success, so a dead link costs a sentence and not the page you were on.
- `history_push` / `back` — 64 crumbs, oldest dropped; back is a FETCH
  (no cache), the crumb is spent only if the page comes back, and the
  remembered position is a hint checked against what arrived.
- `perform` — the one door every navigation goes through: the POST
  refusal by name (until packet 05), the browser's OWN scheme list (http
  and https; gopher is named and handed to the gopher client; anything
  else is said), the downgrade FACT turned into a question, and the
  fragment applied to the page that arrives.
- `refresh_once` / `refresh_if_declared` / `delayed_refresh` — the three
  rules about OBEYING a declared refresh (a delay is reported and offered,
  never followed; an immediate same-document refresh without a fragment
  is refused; a chain is capped at 5 like redirects) and the rule that a
  redirector never occupies a history entry.
- `hop_ask` — a downgrade inside a fetch is a person's decision.
- `typed_address` — a bare `host/path` means `http://`; an address that
  does not fit is refused, never trimmed.
- The reader's `details` flips — reader state, face-independent, and it
  survives a re-wrap the way an edit does.

**The terminal (stays):** everything that writes an escape sequence, the
key reader and its patience for `ESC [`, raw mode, the fold, the pens,
`confirm` and `prompt` and the type-ahead discipline (drop BOTH queues
before a security question), the edit prompt on the status row, the
scroller and the spot walk, help, exit codes, signal handlers.

## Scope and ownership

Own a new library (`userland/libnav/` as a placeholder — Chris names it;
nothing below depends on the name), its host harness, the retrofit of
`wend` onto it, and the corresponding paragraphs of BROWSER.md § The face.
Do not change libpage, libfetch or libhtml. Do not add a cache, a jar, or
tabs: each is a later slice of THIS library and each is named below so it
has a home.

## Deliverable

**A session object** holding the durable half of what wend's `view_t` and
its history hold today: the current page (URL, the tree or the raw text
and its encoding, the model, the reader's flips, the incomplete-page
sentence), the crumbs, and the browser's identity (agent string, accept
list — passed in by the face, since yonder says `yonder/1.0 (os64)`).

**Two halves with a seam between them, so one has a host harness:**

- *The I/O half* — `load`, `go`, `back`, the fetch callbacks — talks to
  libfetch and to nothing else. Its policy is the same as wend's today,
  line for line, including every status sentence.
- *The pure half* — `perform`'s judgements, the refresh rules, the
  history arithmetic, `typed_address` — takes a model and a session and
  returns a DECISION: fetch this, ask this question first (with the
  question's text and its security flag), refuse with this sentence, move
  to this node, do nothing. The face executes decisions; the library
  never paints and never reads a key.

**The face plugs in through callbacks**, exactly two: `confirm(question,
security)` → yes/no, and `cancelled()` → the Ctrl+C flag. The type-ahead
discipline stays on the face's side of `confirm`, because it is about a
terminal's input queue and a GUI has a different one.

**The crumb's position is the face's**: an opaque, fixed-size blob the
library stores and hands back without reading. `wend` puts `top`/`sel` in
it; yonder will put a scroll offset in pixels. The library's own rule
about it — a hint, to be checked against the page that comes back — is
stated where the blob is defined.

**The retrofit**: `wend.c` shrinks to the terminal half and calls the
library for everything above. Its behaviour does not change by one
sentence: that is the acceptance test.

## Required evidence

- Host harness for the pure half, `tools/test_wend_host.sh`'s shape:
  corpus pages through libhtml and libpage, then through `perform`'s
  judgement and the refresh rules with a scripted `confirm`, asserting the
  decision — the sentence, the question, the URL, the chain cap firing at
  the sixth hop, a delayed refresh reported and not followed, a
  same-document immediate refresh refused, a redirector leaving no crumb,
  the sixty-fifth crumb dropping the first, a typed address gaining
  `http://`, one too long refused. Under ASan, with the allocation-failure
  sweep the wend harness already runs at 700 deep.
- `VERIFICATION.md § wend acceptance` run again in the guest against
  `httptestd.py`, unchanged, and passing — the proof that nothing moved.
  A screendump of the same page before and after the hoist, identical.
- `tools/stale_refs.sh` before the commit: most of `wend.c`'s session
  half is changing homes and BROWSER.md names its functions by name.

## Booked out of this packet — the library's later slices, each with a home here

| What | Why it waits | Trigger |
|---|---|---|
| The cookie jar (RFC 6265: domain/path match, expiry, `Secure`, `HttpOnly`, host-only; public suffixes) | needs packet 05's doors; the jar is per session and this is the session | packet 05 lands |
| `Referer` composition | same door as the jar | same |
| A page cache for back | "refetching is honest and simple; a cache is a lifetime problem for later" (BROWSER.md) | back-and-forth on a slow link hurts |
| Forward | wend has no forward; yonder's toolbar wants one, and it is the history's shape (a cursor into the crumbs rather than a stack) — decide it when yonder's face asks, not before | the face slice |
| Tabs / several sessions in one program | one session object per tab is the design; the object exists first | the face slice |
| Bookmarks, a history file on disk | files on the conf ladder, later | someone wants them |
