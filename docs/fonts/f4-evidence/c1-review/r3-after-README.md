# Round three, against the corrections

`r3-observations.txt` is Quinn's record against 0cfb485. The `r3-after-*.txt`
files are her two new probes against the fix round, from her own `r3-run.py`.

| Probe | 0cfb485 | corrected |
|---|---|---|
| `caption` (R2b follow-up) | 14 allocation attempts; the new caption painted at X=100 from a width of zero, 876 pixels wrong | 1 attempt — the denied one — and no caption painted at all (`ink left denied=320` is the probe's "nothing found") |
| `parent` (R6) | child `(6,6,188,29)` from the parent's OLD rectangle, outside its new one | child `(46,26,148,29)`, exactly the expected placement |

The `caption` row still reports 876 differing pixels, and should: one surface
has the new caption and the other does not. **That is the chosen failure
policy** — a width that could not be prepared is not a position, so the paint
leaves the caption off rather than placing it from a zero, and the next paint
puts it where it belongs. The host suite asserts both halves: the denied paint
either matches the correct one or shows no caption, and the paint after it is
pixel-identical to a clean one.

`r2-after-list-resize.txt` was refreshed from the same run, since the round-2
probes were rebuilt alongside these.
