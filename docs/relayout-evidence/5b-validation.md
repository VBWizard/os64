# Relayout 5b validation

5b updates the cascade from native mutation history and copies unaffected
computed styles into a fresh tree. Box construction and layout still run
in full. The ownership, invalidation and bounded-renewal contracts are in
[RELAYOUT.md](../design/pending/RELAYOUT.md).

## Automated evidence

- Full libgarb suite passes, including 1,277 incremental-cascade checks and
  nine update allocation-failure positions. Updated winner dumps match
  independent full cascades after class/custom-property changes, text edits,
  moves, removals and repeated updates through renewal. Lost history and
  nested relational/direction selectors request full builds. Existing parser,
  selector, media, allocation and memory-budget tests remain enabled.
- Full libflow suite: 49,556 checks, zero failures. The new restyle tests
  contribute 25,463 checks, comparing layout dumps and computed-property
  serialization against independently rebuilt cascades. They free the source
  tree before reading copied style lists. Inheritance, sibling rules, moves,
  removals, root font/body-link changes, disclosures and display changes are
  covered. All 164 incremental-layout allocation positions are failed both
  individually and persistently; ownership returns to its baseline.
  The existing 12 corpus dumps match and 597 sampled fuzz trees pass.
- Yonder scripted-page host: 12,548 checks, zero failures. New cases check
  cascade identity/update counts, displayed and measured style reuse,
  repeated geometry reads, releasing the measured owner during publication,
  immediate geometry after inline stylesheet edits, and media invalidation.
  The duplicate stylesheet/import ownership sweep and existing animation,
  face-budget recovery, synchronous geometry and browser tests pass.
- The unchanged measurement page completes both 200 rows / 3 samples and
  2,000 rows / 20 samples through the browser host: 16 checks per run,
  zero failures, including cancellation and viewport-change rejection.
- Target Yonder and matching libraries cross-build. ASan, UBSan and
  LeakSanitizer remain enabled for host tests.

Reproduce with `tools/test_garb_host.sh`, `tools/test_libflow_host.sh`,
`tools/test_yonder_scripts_host.sh` and
`tools/test_layout_measure.sh <output-directory>`. Focused restyle runs are
`tools/test_garb_host.sh --updates`, `tools/test_libflow_host.sh --restyle`
and `YONDER_RESTYLE_ONLY=1 tools/test_yonder_scripts_host.sh`.

## Target and hardware evidence

The isolated QEMU guest completed the unchanged 2,000-row / 20-sample page.
Its saved diagnostic records 52 model rebuilds, 53 layout attempts,
50 cascade updates and 2,040 copied computed styles, with zero refused or
partial results and zero clock errors. Layout reuse remains zero, as expected
before 5c. These counters establish that the target executable uses the new
path; emulator timings do not establish a P5 performance improvement.

The frozen delivery cohort contains 23 files, including 18 binaries reused
byte-for-byte from the 5a delivery. It includes a SHA-256 manifest and the
unchanged measurement fixture. The local handoff records the exact source
commit and delivery paths.

P5 acceptance and Fable's review of the restyle rule remain pending. For
comparison with [the 5a baseline](5a-validation.md), use the same settings:
Scripts and Modern Web on, dark appearance, 100% zoom, a fixed 2,526 × 1,259
CSS-pixel viewport, 2,000 rows and 20 samples. Reload before the run:

```
yonder --layout-profile /tests/pages/layout-measure.html
```

Choose **Run measurements**, wait for **Complete**, retain the table/JSON,
and press `d` with ordinary page content focused to save the corpus report.
The `layout:` fact now includes `styles reused` and `cascade updates`;
`reused` continues to count layout reuse and remains zero in this slice.

The fixture's `:nth-child(even)` rule makes the conservative restyle rule
revisit the row group after a row edit. Consequently it is a useful unchanged
comparison but not a best-case demonstration of isolated style reuse. No
speedup is assumed. The dominant layout phase from 5a is addressed by 5c.
