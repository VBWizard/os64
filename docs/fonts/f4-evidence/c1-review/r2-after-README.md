# Round two, against the corrections

`r2-observations.txt` is Quinn's record against 5357245 (restored from the
verbatim results quoted in her review after a rerun overwrote the file).
The `r2-after-*.txt` files are the same probes against the fix round, from
her own `r2-run.py`.

| Probe | 5357245 | corrected |
|---|---|---|
| `tabs` (R2a) | 12 adoptions succeeded with a 64px interval where the face needs 40 | 0 — a refused measurement is a status, never a substituted interval |
| `button` (R2b) | 5 allocations per paint; under denial the ink moved 52 → 100, 876 pixels differed | 0 allocations, ink at 52 either way, 0 pixels differ |
| `list-resize` (R2c) | 3 visible rows, 1 retained run; first paint made 10 allocations, 296 pixels differed | 3 rows, 3 retained runs, 0 allocations, 0 pixels differ |
| `list-release` (R5) | teardown and retry both BUSY, 810,587 bytes retained | teardown OK, nothing retained, 0 bytes |
| `busy-state` | BUSY with the active set already dropped | unchanged, and now that is what the header says it does |

`r2-repro.c`'s `resize_plan` moved to `os64_ui_widget_stage_bounds`, which is
the mechanism R2c asked for; it had been writing live bounds at commit
because there was nothing else to write to. The question it asks is the same.

The `busy-state` row is not a fix. Quinn's teardown-policy observation was
right that the header promised an unchanged window and the code delivered a
destructive teardown. The code is the honest one of the two, so the header
now says so: release drops runs and set first and BUSY means INCOMPLETE —
something outside this window is still alive on the context, the allocator
owner is kept so it can free itself, and the window is not usable as it was.
