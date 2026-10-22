# REPLAY.md — the kept page, its record, and what a replay is

*Written by Fable on 2026-10-22, after five Codex rounds and five cold
reads of PR #326 found fifty-odd ways the corpus gate could say "clean"
about something it had not seen, each round the same kind as the last:
a reader with no grammar, a replay with no stated inputs, a process with
state between pages. Chris's ruling: stop and design it. This document is
the contract the code is held to from here; MODERN_WEB.md § Slice 0b, as
built records what was built against it, and the briefs' rule "a tool
that is a gate gets its format designed before it is built" is this
document's lesson, written into MODERN_WEB_BRIEFS.md.*

## What this settles

1. **A record is a text in one grammar, and there is one grammar.** Every
   line of a page record, a replay record and a kept index matches exactly
   one production below or the file is not a record. The grammar lives in
   `tools/yonder_record_grammar.txt` as vectors (`<kind or REFUSED> TAB
   <line>`), and the Python reader and the C reader each run every vector
   and must agree; a reader that classifies a line the vectors do not
   cover is extended by adding the vector first. Reconciliation (counts
   adding up) never rescues a line the grammar refuses.
2. **A replay has a stated input list, and takes every input from the
   record or the kept files, never from the harness, except the three
   it says it fakes.** The list is § 3. A kept record missing a required
   input is unjudged, never defaulted.
3. **Three invariants, each enforced by structure, each with a mutant
   that proves the structure:** (I) only a page replayed to a verdict is
   judged, and its state is in a record that is whole or absent; (II)
   nothing is written under a name the record did not claim, and no
   record is paired with files that are not its own; (III) no state
   crosses from one page's replay to another's, because they share no
   process.
4. **A check without a mutant is not a check.** Every refusal and every
   requirement in this document has a case in the suite AND a hand mutant
   that removes the check and must fail that case; the mutant list is
   part of the suite (`tools/test_yonder_replay_mutants.py`), not a
   reviewer's afterthought. Five cold reads found cases that passed with
   their fix deleted; this is why.
5. **Older kept pages stay readable.** A format moves only by adding; a
   reader names the version it read and the defaults it took for what
   that version lacks, in the replay record, so a diff never compares a
   page against a different understanding of itself.

## 1. The three files of a kept page

Under the diagnostics directory, for a page claimed as `<host>-<nnnn>`:

| file | written by | when | holds |
|---|---|---|---|
| `<name>.txt` | yonder, through the claim | claimed at load start (the placeholder), replaced whole at arrival and at departure | the page record (§ 2) |
| `<name>.html` | yonder, through the claim | a temporary during the stream, renamed at `stream_finish` | the bytes the parser was fed, after transfer decoding, before character decoding |
| `<name>.d/index` and `<name>.d/<n>.{js,css,...}` | yonder, through the claim | each product as it reached the window; the index rewritten whole after each | the sub-resources the page fetched, as the window received them (§ 4) |

The claim (`yonder_diag_claim`) is the only way any of these names is
written: `<name>.txt` is created exclusively (os64's `"x"` open) holding
`verdict: unwritten <nonce>`, and `.html` and `.d` must not exist by that
name, with `keep_pages` on or off; a name that cannot be claimed is
passed over, and after 64 names nothing is written. The nonce is a random
64-bit value of the claim's (`/dev/random`; where that cannot be read,
the clock, the sequence and the record's address mixed), in sixteen hex
digits, and every record the claim writes carries it too, as the plain
fact `claim: <nonce>`. The write of the record proves the claimed file is
still ITS OWN before renaming over it: it re-reads the file and replaces
it only when the nonce there is its own. A file gone, or one another
yonder claimed after it went (another nonce), means the name is someone
else's now: nothing is written, and that is said once, as a failed write
is.

## 2. The record

A record is lines separated by `\n`, bytes are UTF-8, and NO line
contains a `\r`, a NUL, or any byte below 0x20 other than the `\t` the
escape never leaves (a control byte in a value is spelled `\xHH`), and
no field decodes to a NUL (`\x00`: a string would end at it) or to bytes
that are not UTF-8 (`\xFF`: a value must decode to UTF-8 as well as be
written in it). A record that breaks that anywhere is not a record. The
lines:

```
record      := address (verdict | state verdict?) (head | plain | token | replay)* end?
                                       (in any order after the second line: yonder writes its tokens first)
address     := "address: " text
verdict     := "verdict: clean" | "verdict: unwritten" (" " nonce)?
             | "verdict: " count1 " MISSING" | "verdict: " count1 " FAILED"
             | "verdict: " count1 " MISSING, " count1 " FAILED"
state       := "REPLAY state: verdict"
             | "REPLAY state: " ("incomplete" | "unfinished" | "unjudged") " " text
head        := head-key ": " head-value     (the head set below)
plain       := key ": " value               (any other key)
token       := "MISSING " kind " " name " (" count ")"
             | "FAILED " what ": " message " (" count ")"
replay      := "REPLAY incomplete " text | "REPLAY unfinished " text
             | "REPLAY unjudged: " text
             | "REPLAY inputs stated:" (" " statable "=" value)+   (each key once; a space in
                                                                     a value spelled \x20)
             | "REPLAY inputs defaulted:" (" " defaulted)+     (each name once)
             | "REPLAY kept: assumed whole (version 1)"
             | "REPLAY budget: " count " s"
             | "REPLAY not reproduced:" (" " known-kind)+
end         := "end"
count       := "0" | [1-9][0-9]{0,9}      (no leading zero, at most UINT32_MAX)
count1      := [1-9][0-9]{0,9}            (a verdict names no token it has no line of)
clock       := "0" | [1-9][0-9]{0,15}     (epoch milliseconds, at most 2^53-1)
statable    := "zoom" | "appearance" | "motion" | "animate" | "modern" | "scripts"
             | "script-seconds" | "laid-out-at" | "agent" | "tz"
defaulted   := "cookies" | "clock"
nonce       := [0-9a-f]{16}
known-kind  := "font-fetch" | "image" | "picture-fetch"
```

THE HEAD SET is what both readers parse a value of; any other key is a
plain fact, its value any text:

| key | value |
|---|---|
| `status` | count, then optionally ` ` and the reason |
| `body` | `html` \| `text` \| `image` |
| `encrypted` | `yes` \| `no` |
| `scripts`, `modern`, `animate` | `on` \| `off` |
| `appearance` | `light` \| `dark` |
| `motion` | `full` \| `reduce` |
| `kept` | `whole` \| `not whole: ` text |
| `bytes`, `kept lines`, `zoom`, `script seconds` | count |
| `laid out at` | count [`x` count] ` px` [` in ` count ` ms`] |
| `clock` | clock |
| `agent`, `tz` | text, not empty |
| `final address`, `content-type`, `charset`, `TLS`, `cookies` | text, maybe empty |

yonder writes them in a room of their own (24 lines, counted apart from
the 512 the plain lines may take), except `bytes`, which it writes in
the plain room: a record that ran out of plain room may lose it, and says
it lost lines.

- The SECOND line is the verdict for a page record and the state for a
  replay record; nothing else may stand there.
- `name`, `what` and `message` are non-empty; the separators (` `, `: `,
  ` (`) are required, not inferred; `kind` is one word.
- **`end` is required** of a replay record (`REPLAY state:`) and of a
  kept page's record of version 2 (§ 3) that carries `kept lines:` or
  `kept:`; any other may lack it (the census records written before it
  existed, and the kept pages of version 1). Nothing may follow
  `end`, and nothing yonder writes can render as that line alone.
- **Escapes.** A value is written through one escape and read through one
  unescape, both ONE PASS LEFT TO RIGHT in both readers: `\\` for a
  backslash, `\xHH` for a byte that may not appear raw, `%25` for a `%`
  that a reader would otherwise take for part of a token's spelling
  (`M%49SSING`, `F%41ILED`), and the two token spellings decoded as the
  words they stand for. The readers' decoders are held equal by vectors
  with every escape in every position. What a field decodes to must be
  UTF-8 with no NUL, as the file itself must be: every decoded value is
  an input to some reader, and an escape spells a byte that may not stand
  raw, never one that is no part of a character.
- **Numbers.** One bounded-decimal parser per reader: ASCII digits only,
  no sign, no leading zero past a lone `0`, at most ten digits, at most
  UINT32_MAX (a clock: sixteen digits, at most 2^53-1), nothing trailing.
  Everything numeric (counts, `bytes`, `kept lines`, `zoom`, the clock,
  the index's numbers and version, the runner's own `--seconds`, and the
  sequence in a record's name once its zero padding is passed over) goes
  through it.
- **A second `address:`, a second verdict, a second `end`, a line past
  `end`, a line the grammar does not know**: not a record.

## 3. The replay's inputs

A replay reads a page in the window it was read in. Every input has one
source; "the harness" is a source only where this table says so, and
the rows that say so are the whole of what a replay fakes.

| input | source | head fact | required |
|---|---|---|---|
| the page's bytes | `<name>.html` | `bytes: N` must equal the file's length | yes |
| that the page was kept whole | record | `kept: whole` (a record without `kept:` is not a kept page) | yes (v1: assumed when unsaid, named) |
| the address, final address | record | `address:`, `final address:` | yes |
| the head: status, content type, charset, encrypted, body kind | record | `status:`, `content-type:`, `charset:`, `encrypted:`, `body:` (local pages: `body:` only) | yes |
| TLS | record | `TLS:` read into the head and written back on the replay's record; nothing else on the page reads it | when said |
| the sub-resources | `.d/index` and files | `kept lines: N` must equal the index's lines | yes (v1: when said) |
| the window's size | record | `laid out at: WxH px` (claim; rewritten at arrival with the layout time) | yes (v1: stated when unsaid, named) |
| the modern switch | record | `modern:` | yes (v1: stated when unsaid, named) |
| scripts, their budget | record | `scripts:`, `script seconds:` | yes (v1: stated when unsaid, named) |
| zoom, appearance, motion, animations | record | `zoom:`, `appearance:`, `motion:`, `animate:` | yes (v1: stated when unsaid, named) |
| the user agent | record | `agent:` | yes (v2; v1: stated, named) |
| the script-visible cookies | record | `cookies:` the `document.cookie` string the page could read at claim, HttpOnly excluded | yes (v2; v1: the harness's, named) |
| the clock | record | `clock:` epoch milliseconds at claim | yes (v2; v1: the harness's, named) |
| the zone | record | `tz:` the zone in force at claim | yes (v2; v1: stated, named) |
| the clock while the page runs | the harness | — | stated: `Date` reads the record's clock, frozen; it does not move while the page runs |
| the window's time | the harness | — | stated: an idle turn moves the window's milliseconds a 10 ms tick, the microsecond clock moves 1 ms each time it is read, and the replay ends at a 60 s virtual budget (`--seconds`), which every judged replay record states (`REPLAY budget: <seconds> s`) and the diff compares |
| the fetch's end | the harness | — | stated: a page from the network is posted whole before the first turn, with a verdict of OK always; the record keeps what the parser was fed, not how the fetch ended |
| when products arrive | the harness | — | stated: each script or sheet job is answered at the top of the next turn, in the kept order, never between the chunks of the page's own parse |
| the request | the harness | — | stated: a page that was the reply to a POST is fed back as a GET's |
| the cookie jar | the harness | — | stated: read-only; what the page sets with `document.cookie=`, and a `Set-Cookie` of its own, is not seen again |
| the text face | the harness | — | stated: bitmap face, so measurements differ from the P5's |
| pictures and fonts | not fetched | — | stated: their fetch-decided outcomes are not reproduced (§ 5) |
| the screen | the harness | — | stated: `screen`, once built, answers the harness's size |

**Privacy, said plainly:** a kept page already holds the bytes of a
logged-in page; with `cookies:` it also holds the tokens the page could
read. A kept corpus is exactly as private as the browsing was, and
`keep_pages` is a setting the person turns on knowing that. The record
never holds an HttpOnly cookie, which no script could read either.

**The window changed during a load**: yonder compares, at every write of
the record, the agent, scripts on or off, the modern switch, the zoom,
appearance, motion, animations and the script budget with what the claim
wrote, and, at arrival, the view the page was laid out in with the
claim's. One that differs gives the record `REPLAY unjudged: settings
changed during the load` and the page is not replayable; a replay cannot
be two windows. A view that moves AFTER the page arrived is not compared:
it also moves when yonder's own bars come and go (a replay of a bing.com
page shrank its view 40 px that way), which a replay of the page
reproduces, and the record cannot tell that from a person resizing the
window. The cookies and the clock are what they were at the claim, by
definition, and are not compared.

**Versions.** The index's first line carries its format, `# yonder kept
index <v>`; a record is of version 2 when it says any input version 2
added (`agent:`, `cookies:`, `clock:`, `tz:`), and of version 1 when it
says none, and a record and its index of different versions are damage. A reader accepts every
version it knows. **Version 1** is a record without `agent`, `cookies`,
`clock` and `tz`, with or without `end`, and an index of six fields, no
job identity. A REPLAY TAKES NO INPUT SILENTLY FROM THE HARNESS, and for
version 1 that means:

- The head facts the version 1 builds wrote stay REQUIRED: `status`,
  `content-type`, `charset`, `encrypted`, `body`, `final address`,
  `bytes`. Those builds wrote `content-type` and `charset` only when the
  server named one, so in a version 1 record their absence is that
  format's spelling of empty, and reads as empty.
- The WINDOW's inputs (`zoom`, `appearance`, `motion`, `animate`,
  `modern`, `scripts`, `script seconds`, the view's `laid out at` with
  width and height) and the agent and the zone, when its record does not
  say them, are STATED BY THE PERSON running the replay, never guessed:
  `--window zoom=1000,appearance=dark,...,laid-out-at=WxH,agent=...,tz=...`
  (an item ends at a comma a known key and `=` follow, so an agent string
  may hold commas). The replay record names each it took, with its value:
  `REPLAY inputs stated: zoom=1000 ...`. One the record does not say and
  the flag does not state leaves the page UNJUDGED, named ("its record
  does not say its `zoom` and --window does not state it"). A `laid out
  at` of a width alone (the census-era form) says no view.
- The cookies and the clock, which nobody can state for a page recorded
  before them, are the harness's, named: `REPLAY inputs defaulted:
  cookies clock`.
- KEPT: a record that says `kept:` is a kept page; `kept: not whole`
  refuses it. A version 1 record that says no `kept:` (the 2026-10-21
  corpus predates it) is kept when its `.html` is beside it, ASSUMED
  WHOLE: its `bytes:` must equal the file's length and its `kept lines`,
  when it says one, the index's lines, and the replay record says
  `REPLAY kept: assumed whole (version 1)`.
- Its jobs are matched by kind and address in kept order.

The diff compares two records of a page only when both were run with the
same statement and took the same defaults, so both sides of a measurement
run with the same `--window`. **Version 2** is strict: `--window` states
nothing for it. A version the reader does not know is damage. An index
with no first line, or a first line that is not the version line, is
damage.

## 4. The index

```
index    := version (comment | line)*
version  := "# yonder kept index " count
comment  := "#" text
line     := count TAB kind TAB fields
fields   := (script | sheet)   -- v2: address TAB came|failed TAB final TAB label TAB identity
                               -- v1: the same without identity
```

Lines are numbered 1..N in order, each once; a number repeated, skipped
or out of order is damage. A line of a kind the reader does not know is
damage, so the slice that keeps a new kind (slice 2's `fetch`, slice 1a's
`storage`) teaches both readers that kind in the same change. Fields hold
no raw control byte and decode to no NUL; the C reader never re-splits a field with a
NUL-terminated routine. A job is matched to a line by kind, address and
identity (v1: kind and address, in kept order); each line answers one
job; a job with no line left is `REPLAY incomplete`; a line no job took
is `REPLAY incomplete never asked for`.

## 5. What a replay reproduces, and what it judges

Reproduced and compared: every `MISSING` and `FAILED` kind except those
decided by a fetch the replay does not perform: `image` (a fetched
picture's format), `picture-fetch`, `font-fetch`. A replay record names
them on `REPLAY not reproduced:` and a reader refuses any other word
there. `MISSING picture-scheme`, `FAILED picture` for a refused or
never-asked address, and `MISSING font` are decided without a fetch and
are compared.

The page's state, decided in ONE function from the run itself (never by
reading the record back): UNJUDGED (anything in § 2-4 refused; the kept
record lost lines, or says settings changed during the load; a harness
check failed; the window's own record lost lines; the write faulted; the
child died) outranks UNFINISHED (no
verdict reached: never arrived, busy for ever, a job out at the budget)
outranks INCOMPLETE (an index line unmatched either way, a navigation the
page started) outranks VERDICT. Only VERDICT pages are judged clean or
compared; the tally counts VERDICT records and a census record's kept
lines whatever it lost, and names a placeholder as damage.

## 6. One process per page

The runner forks a child per kept page. The child: replays, writes the
record through the door (temp, fsync, rename; the temporary opened
`O_CREAT|O_EXCL|O_NOFOLLOW`), `_exit`s with a status from a table the
header states (the state, with flags for clean, leaked and defaulted;
none in the sanitizers' range, none below 64). The parent: holds no fake state, starts no thread before the
fork, resets `SIGCHLD` to default at start, waits with `waitpid` retried
on `EINTR` only, enforces a wall-clock limit per child (10 minutes; past
it the child is killed and the page is UNJUDGED "timed out"), REMOVES the
child's temporary (`<name>.txt.<pid>.new`) if the child left one, reads
the child's record, and trusts the record's state line and the exit
status only when they agree: a child that died, exited outside the
table, exited without a record, or whose record disagrees with its exit,
gets a record the parent writes: `REPLAY state: unjudged the replay died:
<signal or status>`. A production run replays nothing in the parent; the
self-test keeps its fixtures in a child too, so children start clean.

The OUT-DIR: a whole run writes into one that holds no records, and is
refused otherwise; OUT-DIR must not be KEPT-DIR, compared by device and
inode so a bind mount is the same directory; one that cannot be listed
refuses the run; and `--only` (a partial run) removes a page's old record
before its child starts, so a record that cannot be written now leaves
none rather than the old one, and one it cannot remove refuses the run.

## 7. The proof

- The grammar vectors, run by both readers, including every refusal in
  this document and every one the five rounds named (leading zeros,
  signs, Unicode digits, NUL, CR anywhere, an empty index, a second
  address, an escape in every position).
- The real records: the 1,601 census records read as VERDICT; the 601
  records kept on 2026-10-21/22 read as version 1, and every kept page
  among them replays with what was stated and defaulted named, or is
  unjudged by a rule above (MODERN_WEB.md § Slice 0b, as built, has the
  count and the statement it was run with).
- The runner self-test: one page per rule in § 2-6, each with the cut or
  the damage that ONLY that rule can catch (a case that another rule
  catches first proves nothing).
- **The mutants**: `tools/test_yonder_replay_mutants.py` applies one hand
  mutant per rule in § 1-7, every rule, not a sample (rule 4: the
  required-fact table minus each entry; each input not applied; the
  identity compare, the numbering check, each-line-one-job, each refusal
  of the readers, the version checks, the end rule, the claim's proof,
  the window-change marks, the parent's wait, limit, exit table,
  agreement and temp removal, and the diff's rules) and requires its
  suite to FAIL on each. It is run by the PR before Codex
  (`tools/test_yonder_replay_host.sh --mutants`) and listed in the PR body
  with its count.
- The diff: pairs by name AND address, and only records that took the
  same inputs from the harness, were run with the same `--window`
  statement and were given the same budget; judges VERDICT pages only; exits 2 on
  anything unjudged on either side and on a page only AFTER has (BEFORE
  did not replay it); else 1 on a regression, a broken page (UNFINISHED
  after), a `newly incomplete` page, or one missing from AFTER; and
  compares only the reproduced kinds.

## 8. Booked, with triggers

| debt | trigger |
|---|---|
Each is a row in DEBTS.md § The replay corpus.

| debt | trigger |
|---|---|
| the text face is the harness's bitmap face, so measured layout differs from the P5's | a replay regression that is a measurement |
| `screen`, once built, answers the harness's size | the slice that builds it, or a page whose MISSING lines depend on it |
| a kept corpus holds script-visible cookies | the day the filesystem has modes, the record is written closed |
| keep_pages writes on the window's thread | browsing with it on visibly hitching while a page's scripts arrive |
| a page that navigates replays only as far as going | pages that redirect by script often enough that their incomplete records hide what slices change |
| pictures and fonts are not fetched on replay | a slice whose work is pictures or fonts wanting the corpus to judge it |
| a form's reply replays as a GET | a kept page from a form whose scripts read what was posted |
