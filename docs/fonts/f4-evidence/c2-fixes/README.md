# C2 corrections — evidence

Against Quinn's review of e289ca3 ([F4-C2-QUINN-REVIEW.md](../../F4-C2-QUINN-REVIEW.md)).

`c2-review-after.txt` is her own runner, `../c2-review/run.py`, against the
corrected tree (paths scrubbed). Her probe needed one line changed to build:
`measure_widest()` is `measure_document()` now.

| Probe | e289ca3 (her `observations.txt`) | corrected |
|---|---|---|
| `delete` | Backspace left `636166c3`, Delete left `636166a9` | both `636166`, caret 3 |
| `field-oom` | `636166c3`, cursor 4 | `636166`, cursor 3 |
| `view-oom` | cursor 4, inside `é` | cursor 5 |
| `field-adopt` | caret 192, left 0, not visible | caret 192, left 102, visible |
| `commit` | 5 allocations after the barrier | 0 |
| `commit-oom` | adopted with extent and bar total 0 | nothing after the barrier to refuse; extent 1536 = fresh |
| `save` | `abc` gained `0a`; empty became `0a` | all three byte-identical |

Guest, `/QEMU GUI Boot`, headless, screendumps:

| File | Shows |
|---|---|
| `selftest-pass-41.png` | `scribefonttest: PASS, 41 checks, 0 failed` — C2's 33 plus eight file endings saved untouched through the real disk |
| `production-scribe-builtin.png` | `/bin/scribe` on the builtin face with the 187-line document |
| `prefill-caret-lost-before-reorder.png` | BEFORE the `enter_mode` reorder: the save-before-quit field at 28px, prefilled with the path — the end of the path and the caret are past the field's right edge |
| `field-builtin.png` | after it: Ctrl+O's field takes focus; a typed path, caret at its end |
| `field-dejavu28.png` | the same field after DejaVu Sans 16 then +4 three times: scrolled so the caret sits after `.txt` |
| `field-dejavu12.png` | then down to 12px: the whole path fits and shows from its first letter |

## Against the re-review (C2-R6..R8)

`c2-review-r2-after.txt` is her `../c2-review-r2/run.py` against the
corrected tree; the before-and-after is tabulated under *After the
re-review* in [F4-C2-REPORT.md](../../F4-C2-REPORT.md).

| File | Shows |
|---|---|
| `r2-selftest-pass-41.png` | the self-test after R6..R8: still `PASS, 41 checks, 0 failed` |
| `r2-production-scribe-field.png` | `/bin/scribe`: Ctrl+O's field focused, a typed path, caret at its end |

## Against the second re-review (the R6 follow-up)

`c2-review-r3-after.txt` is her `../c2-review-r3/run.py` against the
corrected tree: the paint that meets a refused binding is wholly bitmap
(caret 42, highlight to 41, no run made), the next is wholly run (34, 33),
and the field's caret sits at 44, the end of the text it drew.

| File | Shows |
|---|---|
| `r3-selftest-pass-41.png` | the self-test after the follow-up: still `PASS, 41 checks, 0 failed` |
