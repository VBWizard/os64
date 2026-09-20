# F4 C3 — the two corrections from Quinn's review

`c3-review-before.txt` and `c3-review-after.txt` are her own probe
(`../c3-review/run.py`) against the tree before and after the corrections,
run the way her review says to run it:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output <dir>
python3 docs/fonts/f4-evidence/c3-review/run.py --baseline <dir> --output <out>
```

| | before | after |
|---|---|---|
| help round trip: kept window origin | 7904 → **0** | 7904 → **7904** |
| help round trip: caret pixels | 38 → **0** | 38 → **38** |
| LIMIT + adoption: prepared caret / row span | 24,576 / 6,144 | 6,144 / 6,144 |
| LIMIT + adoption: committed caret / row span | **786,432 / 786,432** | 6,144 / 6,144 |
| LIMIT + adoption: allocations after the barrier | **10** | **0** |
| LIMIT + adoption: extent and bar total | 18,874,426 | 147,514 |
| LIMIT + adoption: caret pixels | **0** | **58** |
| the same with allocation refused after the barrier | extent **0**, caret pixels **0** | extent 147,514, caret pixels 58 |

The prepared caret row used to hold a wider span than the other rows
(24,576 against 6,144): preparation lowered the budget partway through its
pass and the rows before that point kept the window the view no longer used.
A pass that ends on a smaller budget than it began with is now released and
run again.

## On the glass

`help-before.png` and `help-after.png` are the same QEMU session: the caret
at the end of the 2.5 MB line, then Ctrl+G into the help page, Down inside
it, and Ctrl+G back. The two are identical — the same window, the same
scroll, the caret still on screen. Before the correction the row re-centred
on return and the caret was off the paper.

`selftest-pass-46.png` is `scribefonttest --selftest` on the corrected
build: `PASS, 46 checks, 0 failed`.
