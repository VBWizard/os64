# YONDER_DIAGNOSTICS.md — the page file and the badge

Status: BRIEF, written 2026-10-08 by Fable at Chris's request, for the
builder of the slice. Opus builds, Fable reviews. The "as built" section
joins this file with the code, in the slice's PR. It runs after D11
(#229) and before the performance slice, so the first P5 run after it
lands is the first census of the Modern Web arc.

**Why.** When a site misbehaves on the P5 there is no way to tell whether
yonder did something wrong or the page asked for something yonder does not
have. The status line says one thing and the next hover erases it. This
slice makes yonder write down, per page, what the page asked for that is
not there and what went wrong, in a form that answers one grep. It is also
the Modern Web census: the files accrue from daily use, and a tally of
them ranks what to build first.

## Obligation 1: the page file

- `diagnostics = <directory>` in yonder.conf. Absent means off. Yonder
  makes the directory if its parent exists; otherwise the status line
  says so once per run and nothing is written. Chris points it at
  /tmp/yonder for a day and at /home/yonder/pages to keep.
- One file per page load: `<host>-<seq>.txt`, the sequence per run,
  zero-padded to four digits so `ls` sorts in browse order. The first
  line carries the full address; the name only has to be findable.
- Written when the page arrives and rewritten when it is left (navigation,
  reload, close), because the asks keep coming after load: timers,
  clicks, a script that dies on hover. The departure write is complete.
  Write a temp beside it and rename over, the `os64_conf_write` shape.
- **The keyword rule.** Two uppercase tokens, `MISSING` and `FAILED`,
  begin the record lines and appear nowhere else in the file. A file with
  neither says the page used nothing yonder knows it lacks and nothing
  went wrong: a layout fault on such a page is OURS. The verdict is on
  line two: `verdict: clean` or `verdict: 7 MISSING, 2 FAILED`.
  `grep -L 'MISSING\|FAILED'` lists the clean files, `grep -l FAILED` the
  ones to read. The proof asserts the tokens are exclusive to record
  lines.
- `MISSING <kind> <name>`, once per name, with a count: a global reached
  for that is not there (bare `fetch`, `window.fetch`, `self.fetch`), a
  script yonder refuses to run (`type=module`, a worker, a service worker
  registration), an element drawn as nothing (canvas, video, audio, svg,
  a custom element), a CSS property, at-rule or function the cascade
  skipped, a font face not fetched.
- `FAILED <what>: <message>`: a script that died with its message and
  source name, one stopped at the execution limit, out of memory in
  layout or reading, a link refused and why. Everything the status line
  says today and then loses.
- Plain lines for the rest: final address after redirects, time to
  arrive, bytes, scripts by source name with outcome, layouts and their
  time including those scripts forced, images, sheets.
- **The instrument changes nothing the page sees.** A global miss is
  recorded at the engine's miss on the global object and the answer
  stays what it is (undefined, or the ReferenceError). The known-names
  approach, installing accessors for `fetch` and friends that record and
  return undefined, is REFUSED: `'fetch' in window` and `hasOwnProperty`
  would say yes, and pages feature-detect with exactly those. If the hook
  needs the engine, it is patch 0008 in the series, with context, under
  `tools/test_js_prepare.py`'s rules. Property misses on DOM objects are
  NOT recorded (a page's own `el.flag` misses legitimately); a missing
  DOM method shows up as FAILED when called, and silent feature
  detection on an element is the stated gap.
- The counters live whether or not the setting is on; the setting decides
  the file.
- Existing `--script-audit` log lines stay as they are, so the log's
  timeline still correlates with the file.

## Obligation 2: the badge

- At the right end of the status bar, present only when nonzero, per
  page: `MISSING 7  FAILED 2` (ASCII; the terminals are Latin-1). The
  same tokens as the file, so what you see is what you grep.
- The existing status text keeps the rest of the bar; the badge is its
  own label at the right edge, so hover addresses never push it around.
- Clicking it does nothing in this slice. Showing the current file as a
  page in yonder is a later, small slice.

## Proof

- Scripted-page host: a dying script writes a FAILED line and the verdict
  counts it; a timer dying after arrival changes the departure write and
  not the arrival one; `typeof fetch` records MISSING global fetch and
  the page's path is unchanged (`'fetch' in window` is false, the
  oracle); a clean page writes `verdict: clean` and no token anywhere; a
  module script and a canvas each record their kind; setting absent
  writes nothing while the badge still counts; directory with no parent
  says so once.
- Token exclusivity: the harness scans every file the cases wrote for the
  tokens outside record lines.
- Mutants: the departure rewrite skipped; a miss counted twice; the
  tokens leaking into a plain line.
- Guest: `/diag.html` on the harness server, then `cat` the file.

## Record

DOM.md gets a row pointing here. The "as built" section of this file
ships in the slice's PR with the code, and names anything above that was
built differently and why.
