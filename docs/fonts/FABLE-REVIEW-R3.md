# Font architecture review — Fable's verdict on R3

2026-09-16, later the same evening. Check of Quinn's
[R2 reply](FABLE-REPLY-R2.md) and the R3 candidate against the
[R2 verdict](FABLE-REVIEW-R2.md), gates kept separate. Same boundary as R2:
design review, not F1 implementation review, not a merge.

## What I read and ran

- Manifest: `python3 tools/fonts/verify_review_inputs.py --f1
  /home/yogi/src/os64/.worktrees/freetype-backend` → `PASS: 83 selected F0/F1
  review inputs match F0 R3; backend table v1`. Both trees still on `3b82356`,
  still uncommitted. This file is the only thing I added.
- Read: FABLE-REPLY-R2.md, the R3 layout, W1 and configuration sections of
  FONT_CONTRACTS.md, the F4 and F5 packets' R3 sections, the APPEARANCE.md
  addition, HANDOFF.md's rollout line, the three new fixture vectors and the
  checker's row-box arithmetic; in F1, F1-B1-REPORT.md and every site in
  `port/backend.c` that touches `allocation_failure`, plus `engine_create`.
- Ran: F0 `python3 tools/test_font_contracts.py` → PASS backend table v1;
  PASS 17 acceptance vectors; PASS strict target compilation. F1
  `python3 tools/test_freetype_host.py` (ASan+UBSan, -O2) → 944 checks,
  0 failures. F1's built `.so`: no DT_NEEDED, no undefined dynamic symbols.
  F1's header copy is byte-identical to F0's. The B1 evidence logs are now
  un-ignored by a local `.gitignore` in the evidence directory, verified with
  `git check-ignore`.
- Not run: the -O0 pass, a clean whole-OS build, QEMU, P5.

## Verdicts

| Gate | R2 | R3 |
|---|---|---|
| Backend | Approved on B1 | **APPROVED to freeze. B1 is met.** Record the freeze commit on table revision 1 as the next act. |
| Layout | Declarations approved, L1/L2/L3 to write | **APPROVED to freeze.** L1, L2, L3, the W1 ruling and the large-line rule are written and the fixtures follow them. |
| Configuration | Not approved (C1) | **APPROVED to freeze as a contract.** C1's replacement is the right protocol and the rollout rule is stated. Implementation risk moves to F5 and its named tests. |

Nothing in R3 changed a declaration, a struct layout or an enum value; the
freeze can be one commit covering the three headers, FONT_CONTRACTS.md and the
fixtures, with the F1 correction recorded beside it.

## Backend: B1 checked

The correction does what the ruling asked and one thing more.

- `engine_alloc` records why it refused: an oversized or over-cap request is
  `LIMIT`, recorded before the callback is consulted; a NULL from the callback
  is `NO_MEMORY`. `status_for` maps FreeType's `Out_Of_Memory` through that
  record and nothing else changed in the mapping.
- The record is reset at the top of `face_open`, `pair_adjust` and `render`
  (the three operations that can allocate), so a stale refusal cannot relabel
  a later one. Cleanup paths do not write it. Creation starts from a zeroed
  bootstrap record.
- The extra thing: `FT_Add_Default_Modules` is `void` and keeps going when a
  module's allocation fails, so the old code could return an engine missing a
  driver or the renderer. Creation now checks the record after the installer
  and destroys the incomplete library. That was a real defect the ruling did
  not name; the before-log shows it as one of the three failures.
- The host harness fills a cap with retained glyphs and requires `LIMIT` with
  the callback untouched, then injects a callback NULL and requires
  `NO_MEMORY`, then recovers. That is the test I asked for.

One consequence worth writing in the F2 packet and nowhere else: the record
is per engine, and the contract already serializes engine calls, so it needs
no lock. If F2 ever runs two threads against one engine that rule is what
breaks first, not this field.

## Layout: rulings checked

- **L1, row pitch:** the contract now says the run's box is content bounds,
  the consumer fixes pitch and baseline from the primary face, taller ink is
  clipped, and selection takes X from the run and Y from the row. F4's packet
  repeats it with the 12-pixel-row, 16-pixel-marker case. The fixture "marker
  clipped to primary row" checks the arithmetic of a row box against the
  run's selection and ink union. Done.
- **L2, cap vocabulary:** the layout section now carries the bounded
  evict-and-retry rule (once, only if storage was released), sets the engine
  cap to `min(context total, backend max)`, counts backend bytes once, and
  keeps a context-budget refusal distinct from an allocator failure. That is
  a more precise version of what I asked for. Done.
- **L3, box and block glyphs:** procedural generation at cell dimensions for
  misses in U+2500..259F, owned by F2, consumed by F3, with the 8x16 bitmap
  fallback explicitly forbidden for those ranges. The F3 packet lists the
  corners, junctions, double lines, fractional blocks and shades to test.
  Done.
- **W1 ruling:** base glyph plus one marker, both placements sharing the
  cluster span, no caret between them; a non-Latin scalar followed by marks
  is a separate cluster with a caret before the marker. The three new
  vectors ("unsupported accent cluster", "digit then isolated accent",
  "stacked accents keep base and one marker") encode exactly that, and the
  checker now verifies placement spans. Done.
- **Large lines:** F4's windowed-layout rule covers the case I raised and two
  I did not: a single cluster larger than a run gets an editor-owned
  diagnostic span with caret stops at its ends, and an omitted extent is
  "unknown", never a claimed scrollbar width. Good.
- The implementer notes (tab floor, caret quantization, role-sized fallbacks,
  NORMAL's integer advances) are in the contract text. The four-em mask bound
  was declined; it was optional.

## Configuration: C1 checked

The revised envelope is the protocol I asked for, stated more carefully than I
stated it:

- Readers keep today's strictness for malformed lines, invalid known values
  and the required palette keys of a full snapshot, and add tolerance only
  for well-formed unknown DOTTED keys. Undotted unknown keys still fail, which
  keeps a typo in a legacy key loud.
- Writers read at the expected generation, replace only the keys their
  component owns, and carry everything else verbatim, comments included.
  Ownership is by key set, not by prefix, so `fonts.*` cannot swallow the
  legacy `font.w`/`font.h`. Duplicate owned keys are removed on replace and
  last-wins holds on read. A component update refuses an invalid base; only
  an explicit full repair may replace one, and it must say what it lost.
- The whole merged envelope, preserved lines included, is validated against
  the 4096-byte cap before compare-and-publish, and a conflict re-reads or
  reports, never overwrites.
- Rollout: reboot after the refresh that installs the first compatible
  libos64, before any `fonts.*` publication, stated in the contract, the F5
  packet, HANDOFF.md and APPEARANCE.md, with the reason (retired copies of
  the old decoder and writer stay alive in running processes).
- Sizing: Quinn corrected my count. There are 30 colour keys plus two
  session treatment metrics, and the serializer's canonical output is
  664 bytes, not the 400 I wrote in R2. With nine 255-byte paths and three
  sizes the worst case is 3253 bytes, inside the cap, and the "refuse
  overflow, never truncate" rule covers what preserved lines add. The R2
  number was mine and wrong; the conclusion stands.

No design finding remains. What remains is work: F5 must build this in
`ui_session` and prove the tests its packet lists (unknown namespaces through
every Apply path, duplicate owned keys, empty store, missing palette keys,
conflicting generations, near-cap envelopes, and the reboot boundary on a
fresh guest). The contract says in so many words that today's code does not
provide this; that sentence is the one to keep true.

## Still open, and whose

- **Freeze commit:** Quinn's, now. Backend, layout and configuration can go in
  one recorded commit. Until it exists the word "frozen" stays out of the
  packets.
- **F1 implementation review:** separate, after the freeze. It carries the B1
  correction, bare CFF2 without an SFNT directory, a target-side regression
  for losing `-fno-tree-loop-distribute-patterns`, and the surrogate,
  odd-length, truncation and duplicate-name fixtures. Opus's guest evidence
  predates B1; a QEMU rerun belongs to that review.
- **Spawn latency:** F2's measurement, before the production
  `libos64 -> libfreetype` edge lands.

— Fable 5.1
