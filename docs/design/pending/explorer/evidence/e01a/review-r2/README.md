# PR #254 — hidden status row font planning

Reviewed base: `63f1211c`. This packet addresses the
[hidden status row finding](https://github.com/VBWizard/os64/pull/254#discussion_r4243591851).
The gallery planner budgets six fixed rows, four rows when the target panel is
visible, and one row when the status bar is visible. Hidden status messages
continue to prepare their runs and retain their ownership tokens.

The host regression drives the production gallery and font consumer. It derives
height boundaries from staged control/status bottom edges rather than copying
the planner's row arithmetic. With the target panel both shown and hidden, it
checks refusal with the status visible, successful adoption after hiding it,
one-pixel-too-short refusal without moving live bounds, retained help/notice
runs and tokens, and restored status-row capacity after showing it. The real-font
variant adopts DejaVu Sans at 32px; commit is checked with allocation denied.

| Command from the E1A worktree | Result | Evidence |
| --- | --- | --- |
| `ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --output /tmp/e01a-r2-before` before the production fix | 420 checks, 2 failures reproducing refusal with the panel shown and hidden; exit 1 | [before](before.log) |
| `ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --output /tmp/e01a-r2-fake` | 428 checks, 0 failures | [fake](fake.log) |
| `ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --real --output /tmp/e01a-r2-real` | 475 checks, 0 failures | [real](real.log) |
| `make -C userland "$PWD/userland/bin/actiongallery"` | Strict cross build passed | [cross build](cross-build.log) |

ASan, UBSan and LeakSanitizer remained enabled through the supported
outside-sandbox route. [Source/binary hashes](tested-files.json) identify this
revision. Sibling height budgets and touched comments were checked;
`tools/stale_refs.sh` found no candidates in the source-only diff; the full
documentation diff flagged existing live handoff/evidence links, verified on
disk. No new superlative claims were reported.
No new QEMU run is claimed for this planner-only fix; earlier OS checkout
results remain historical evidence in the parent packets. Re-review is pending.
