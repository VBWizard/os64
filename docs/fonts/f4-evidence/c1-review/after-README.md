# The same probes, against the corrections

`observations.txt` and `release-asan.txt` are Quinn's, captured against
d11608b. The `after-*.txt` files are the same probes run against the fix
round, from the same `run.py` — its probe source moved to the corrected API
(status-bearing measurement, the run slot on the draw, a release that
returns) so the questions survived the change rather than the call spellings.

| Probe | d11608b | corrected |
|---|---|---|
| `release` | heap-use-after-free in `binding_free` | BUSY with the owner kept, then OK once the caller lets go |
| `measure` | adoption OK, staged width 32 against an actual 96 | adoption and measurement both report NO_MEMORY, width 0 |
| `paint` | pen 32 for a 96-pixel string | pen 0: nothing painted, nothing invented |
| `clip` | painted y=[16,31], 24 pixels outside a [20,30) row | painted y=[20,29], none outside |
| `register` | adoption OK with 0 planner calls | the planner registers without allocating, so it is called |
| `cross` | foreign set bound OK and measured 32 | bound refused (BAD_ARGUMENT) |
