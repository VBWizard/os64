# E1B sixth review: registry pairing and opening snapshots

2026-10-12. PR #259 reviewed `e37cca21`. The follow-up extends the
[controller invariants](../../../E01B_INVARIANTS.md) across model/view binding
and fresh request preparation. Public APIs and native E1K code are unchanged.

| Finding | Shared obligation and fix | Regression |
| --- | --- | --- |
| Attached bar accepts a model from another registry | I11: model/view pairs retain registry identity. Bar binding rejects a foreign model before retention, font preparation or publication; the existing view setter already checks the opposite direction. | Colliding action IDs in different registries; inactive bind refusal preserves old model, captions, view and references without allocation. Active refusal preserves its live path. Foreign view assignment is refused too. |
| Opening eligibility uses the preceding snapshot | I5/I7/I9: prepare a retained fresh candidate before selecting a heading/group. Assign it to the session after successful teardown. Direct mnemonics, pointer/overflow and horizontal navigation use the prepared snapshot; navigation passes its candidate into opening rather than querying twice. Reveal reflows are checked against that candidate too. | Disabled→enabled and enabled→disabled through visible/hidden mnemonic, pointer and overflow requests, plus Down and Left/Right. Enabled requests invoke the saved target once; disabled requests create no replacement. Declined/failed replacement and stale candidate plus failed teardown retain the old target. |

A candidate sharing the existing session's view also changes that view's live
snapshot. If the request is declined, it runs the same selection reconciliation
as explicit menu refresh: close invalid descendants before clearing the parent
anchor, preserving the path when destruction is refused. A focused regression
caught this interaction in the first candidate implementation and is retained
as [transition-before.txt](transition-before.txt). The fixed checks preserve the
original root model, parent selection/child ownership and error precedence.

[baseline.py](baseline.py) retrieves the reviewed menu sources into a scratch
directory and compiles the current harness against them. Private layouts and
the other linked modules are compatible with the reviewed revision.
[Before-fix results](regressions-before.txt) record source hashes and assertion
failures (26 assertions) without a fixture fault. Repeated invariant assertions are not counts
of distinct scenarios.

```sh
ASAN_OPTIONS=detect_leaks=1 python3 docs/design/pending/explorer/evidence/e01b/review-r6/baseline.py --output /tmp/e1b-r6-baseline
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --menu --output /tmp/e1b-r6-fake
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --menu --real --output /tmp/e1b-r6-real
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_menu_mutations.py --output /tmp/e1b-r6-mutations
make -C userland -j8
```

Set `E1B_R6_ONLY=1` for the focused fixtures. Complete [fake](host-fake.txt) and
[real-font](host-real.txt) suites pass with ASan, UBSan and LeakSanitizer outside
the sandbox with `detect_leaks=1`: 150,351 / 150,399 assertions. Between-event observation checks the bar and
popup registry pairs, visible selections, native paths and retained views. Prior
allocation, font, callback, input and viewport regressions remain in the suite.
The strict [full userland build](build.txt) passes.

[Source](sources.sha256), [guest binary](binaries.sha256) and
[host binary](host-binaries.sha256) manifests identify the tested artifacts.
Earlier editor/Scribe/font contract checks remain [separate evidence](../review-r4/README.md).
Registry collisions and precise snapshot/teardown races are deterministic host
fixtures. No new P5 result, E1C migration, assets, kernel work or gclock debt
change is claimed.

[Sixteen mutation gates](mutations.txt) detect foreign-registry publication,
skipped candidate refresh and the preceding fourteen hazards. Each injected
binary must report a behavioral assertion failure; an unrelated crash is not
counted as detection. The structural counts include repeated observations.

## Final native regression

Use the [parent QEMU setup](../README.md#qemu-reproduction) and
[first-review controller](../review-r1/acceptance.py) with this binary manifest,
running `hid`, `ps2-font`, `external`, then `quit`. All modes exit zero:
[HID/pointer](controller-hid.txt), [PS/2/font/overflow](controller-ps2-font.txt),
[external focus/close](controller-external.txt). [Serial](serial-acceptance.txt)
identifies the run; the unchanged kernel hash is in the manifest.

[Archived trace/heap checks](native-checks.txt) pass. Reproduce them with
`python3 docs/design/pending/explorer/evidence/e01b/review-r6/check-guest.py`.
All four traces exit with zero font/target ownership; callbacks run with menus
closed, and popup surfaces retain explicit capacity. Native
[dismissal](dismiss-click-final.png), [later click](later-click-final.png),
[VT preservation](vt-preserved-final.png), [32px View](font32-view-final.png)
and [overflow child](overflow-child-final.png) retain the acceptance behavior.
The [font report](font32.txt) records real Interface adoption.

After twelve warm openings and 24 more, mapped bytes (1,048,576), live bytes
(193,248), live blocks (493), high-water (463,312) and audit OK return to the
same values: [warm](heap-warm.txt), [after](heap-after.txt). New registry and
precisely interrupted snapshot cases are hosted fixtures; they are not claimed
as induced native races. The earlier P5 result and its unspecified installed
revision remain separate evidence.
