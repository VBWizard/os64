# MEMORY_CENSUS.md — who holds a page's memory, in its record

**Status:** design, 2026-11-08. Userland only (yonder and the libraries it
links); no kernel change.

## 1. Why

A social news site's endless feed showed the gap. Its second page arrived,
then libdom's 8 MiB budget ran out and every DOM allocation after it failed;
raised for a test, the feed streamed page after page until the 256 MiB
script heap was nearly spent and yonder held about 1.3 GB. Finding WHO held
the memory took a build with temporary logging, an allocation table keyed by
return address, `addr2line`, and a person scrolling the P5 while the log was
tailed over ssh. The answer was clear once it came (node wrappers were 77 %
of libdom's bytes; the script heap grew about 4 MiB for every MiB of DOM),
but every question after it needs the same apparatus built again, and the
first question it raised — what the other ~1.1 GB was — nothing could answer.

The census makes that apparatus permanent: every page record says who holds
the page's memory, and a page named in yonder.conf says it in detail, over
time.

**What it is not.** It measures; it limits nothing, frees nothing and
changes no budget. It is not a log level: nothing goes to the log or the
glass. It is not a profiler of time (the paint and layout lines already in
the record are that).

## 2. Two tiers, two byte budgets

The corpus is kept, and grows by gigabytes a week, so what the census writes
is held to a budget, and tests hold it there.

| Tier | When | Writes | Budget a record |
|---|---|---|---|
| **Summary** | every record | one `memory:` line: each owner's total | at most 320 bytes |
| **Census** | a page whose address matches a `census` entry | the summary, each owner's breakdown, the script heap's breakdown, and a timeline | at most 6 KiB |

The summary costs nothing to collect: every number in it is a field some
owner already keeps, read at the moment the record is written. The census
pays for its detail (§ 5) only on the pages it was asked for.

## 3. Owners

An **owner** is a part of yonder that holds memory for a page and can say
how much. Each owner reports through one call into a census sheet
(`yonder_census_t`, § 6): a name, a byte total, an optional count, and —
in the census tier — named parts. The census knows no owner's internals; a
new owner (a worker, a frame, a font cache) joins by reporting, without the
census changing.

What each owner has today, and what this design adds:

| Owner | Total today | Added |
|---|---|---|
| **dom** (libdom's own records) | `os64_dom_bytes` | a KIND on every allocation (§ 4), so the total breaks down by what was made |
| **canvas** (libdom's bitmaps) | `dom->canvas_bytes`, no getter | a getter |
| **bodies** (XHR and Blob bytes outside the engine) | `os64_dom_body_stats` | — |
| **script** (the engine's heap) | peak only (`os64_js_heap_peak`) | the LIVE charge (`os64_js_heap_bytes`), and in the census tier the engine's own breakdown (§ 5) |
| **html** (the document) | `doc->arena_bytes` (which already holds the retired blocks pins keep and the change records), `node_count` | — (read as they are) |
| **page** (libpage's state) | `os64_page_state_bytes` | — |
| **sheets** (parsed stylesheets) | `os64_arena_stats` on each sheet's arena | a sum over the page's sheets |
| **cascade** | none | `garb_cascade_bytes` over its private arenas |
| **layout** (libflow's tree and styles) | none public | `flow_tree_stats`: arena bytes, box count, computed-style bytes |
| **pictures** | `Page.kept_bytes`, not printed | printed; with the SVG cache's numbers already recorded |
| **heap** (the whole process) | `/proc/<pid>/heap`, text only | `os64_heap_report_copy` in libos64: the report struct, copied under the heap's own lock |

`heap` is the one owner that is not a part: it is the process, every owner
above plus whatever none of them counts. The summary prints it first, as
mapped, and beside it `uncounted`: what is LIVE in it less the owners' sum —
the number that would have answered "where is the other 1.1 GB".

## 4. Kinds in libdom

libdom has one allocation door, `d_alloc`, and every block carries a header
with its size. The header keeps its 8 bytes: the KIND goes in the size
word's top byte (a block's size is far below 2^56), so a kind costs no
memory at all.

`d_alloc_kind(dom, size, kind)` is the door; `d_alloc(dom, size)` stays,
meaning `D_MEM_OTHER`. Kinds are an enum with a name table (`d_mem_kind_name`),
and the census prints names, never addresses, so a record reads the same
whatever build wrote it. Three kinds cut across libdom and are what the
feed showed matter: `wrapper` (a node wrapper's record; its script object
is the engine's, counted under `script`),
`retained` (any other value held for script) and `listener`. A fourth
says how many of those records needed their rarely used half: `extra`
(the DValueMore a wrapper or value makes when it first needs a view, a
collection, canvas state or the like; internal.h). The rest are
named for the part of libdom that made the block, which stays true as code
moves: `attr`, `collection`, `mutation`, `intersection`, `resize`,
`custom`, `range`, `timer`, `url`, `text` (copies made for content and
encodings), and `other`. The binding's own `os64_dom_t` is charged to
`other` when it is made. The
call sites in those families are tagged; everything else stays `other`, and
`other` is printed like any kind, so how much is still untagged is itself on
the record. `d_free` reads the kind back from the header, so the per-kind
counts come down exactly as they went up.

Per kind libdom keeps live bytes, live blocks, and blocks ever made: three
counters, an add each, in the `os64_dom_t` it already charges.

## 5. The script heap's breakdown

The engine can count its own heap by kind of thing — objects, strings,
functions, arrays, shapes, atoms, bytecode — but only by walking all of it
(QuickJS's `JS_ComputeMemoryUsage`). On the feed's last sample that is
800,000 objects. So:

- the summary prints only the live charge, which the allocator already
  keeps (`os64_js_heap_bytes` exposes it);
- the census tier walks the heap at each timeline sample, through one libjs
  call (`os64_js_heap_census`), never from inside an allocation, and records
  what the walk itself cost (the `census` line's "a census walk cost N ms
  at most"), so a census that changes the page's timing says so.

## 6. Timeline

In the census tier the page is SAMPLED on a timer — the first sample at
the record's first observation (its arrival), from which the timeline's
times count, then every 10 seconds — and once more as the page is left,
before its scripts are released (`census_depart`, wherever a page's scripts
go: the next page's parse beginning, an arrival, scripts switched off, the
page cleared). That last sample stands as the record's last word. Only a sample adds to the timeline, writes
the detail lines and walks the script heap; any other time the record is
observed (pictures in, a record asked for) refreshes the summary alone, so
how often a record is looked at changes neither the timeline's spacing nor
how often the heap is walked. A timer sample also rewrites the page's file,
so what the census gathered survives yonder being stopped or killed
mid-investigation; the file's `written` line still names the record's last
observation, and the timeline says when each sample was taken. A detail line with nothing to say at a sample
is removed, never left standing from an earlier one. A sample keeps the
process heap and the totals of the owners that move with a page's growth:
script, dom, html, layout, pictures and cascade.

The timeline is capped at 32 samples. When it is full, every other sample
is dropped and the interval doubles, so the timeline always spans the
page's whole life at an even spacing: a page left open for an hour writes no
more than one left open for five minutes. The first sample is always kept,
and the newest is always there. The detail lines together are held to the
budget in code (`diag_census`): a line that would pass it is left out.

The timer follows yonder's existing pattern for timed work (a `census_due`
offered in `pictures_schedule`, a `census_turn` in the main loop), so a
sample never runs inside script, layout or an allocation.

## 7. Turning it on

`census = <host>` in yonder.conf, repeatable: a page whose host is that
host or a subdomain of it is censused. `census = *` censuses every page.
The key is read with `os64_conf_find_read` (every line of a repeated key;
`os64_conf_get` keeps only the last). A page with no host (a file, data:)
is censused only by `*`. A host in the file is local policy and never leaves the
machine; nothing about which pages are censused is written anywhere but
that page's own record (`census: on`).

## 8. The record

Summary, one line (illustrative numbers, from the feed's last sample):

```
memory: heap 1.30G (uncounted 610M); pictures 210M 168 pictures; script 180M; layout 140M 390000 boxes; html 52.0M 412000 nodes; dom 48.0M; cascade 9.00M; sheets 3.10M 64 sheets; page 2.20M; bodies 0; canvas 0
```

Census tier, in addition:

```
memory dom: wrapper 36.7M 100242; retained 7.40M 20231; listener 1.32M 19343; collection 739K 5569; other ...
memory script: objects 92.0M 818799; properties ...; strings 3.84M 50462; atoms ...; shapes ...; functions ... (code ...); arrays ...; binary ...; blocks ...; charged 190M
memory timeline: +0s heap 210M script 25.0M dom 2.30M html 9.00M layout 30.0M pictures 40.0M cascade 9.00M | +10s ... | +5m ...
census: on; 32 samples every 20s; a census walk cost 41 ms at most
```

Values are printed with a unit suffix, truncated to three significant
figures — four from 1000 to 1023 of a unit (`yonder_census_size`); times
keep their seconds under two hours (`+12m10s`); the summary's owners come biggest first,
and the line format is `owner value [count unit]` separated by `; `, so a
script can split it without knowing the owners in advance. A summary that
would pass its budget drops its smallest owners and ends `;...`.

## 9. Proof

- libdom host suite: after every case, the per-kind live bytes sum to
  `dom->bytes`, and a kind mutant (a site tagged, a free reading the wrong
  kind) is caught.
- A record built from a synthetic census at its worst case (every owner,
  every kind, 32 samples, the longest numbers) is within its budget; the
  summary of a record with no census is within 320 bytes.
- Decimation: 1,000 samples fed in leave 32, evenly spaced, first and
  latest kept.
- Settings: `census` repeated, a subdomain matching, `*`, and a host that
  must not match a longer one (`ample.com` is not `example.com`).
- `os64_heap_report_copy`: the copy is at an even generation, its live
  figures are the report's, and it keeps the report's audit identity. (It
  takes the heap's lock, the lock every writer holds, so no copy is torn.)
- The feed itself on the P5, censused, is the acceptance: the record answers
  "what does one page of feed cost, and who pays".

## 10. Not in this design

- Limits or reactions (a page warned or stopped at some size): a policy
  for after the numbers exist.
- In-flight fetch bodies: nothing counts them today; booked when a page
  needs it.
- localStorage: held to its quota (a UTF-16 count, not bytes), so it is
  bounded and left out; what it holds shows in `uncounted`.
- What the owners leave out of their own totals — the layout tree's maps
  and paint lists, scratch a call frees before it returns — shows in
  `uncounted` too, which is why that number is printed rather than hidden.
  An owner counts what it holds once: an arena's reserved bytes already
  include the tables carved from it, so those are never added again.
- Per-frame (iframe) and per-worker censuses: each will be an owner when it
  exists (IFRAME.md, WORKERS.md).
- Collecting node wrappers: DEBTS "Node collection beyond D6". The feed
  keeps every post it scrolled past in a script array, so collection would
  not have freed it; the census is what will say which pages it would.
