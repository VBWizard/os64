# E1B fourth review: invariant audit

2026-10-12. PR #259 findings against `5ca61fc3`. The follow-up audits shared
transitions against [controller invariants](../../../E01B_INVARIANTS.md), including
sibling paths from earlier rounds. It keeps the public API and native E1K kernel
unchanged. Session layout lives in the private header so tests observe ownership
without copying opaque layouts or adding public test hooks.

| Finding | Invariant and shared change | Regression evidence |
| --- | --- | --- |
| Refresh clears selection despite failed child destruction | I1/I2: selection is a child's anchor; commit it after descendant removal succeeds. Refresh uses the pointer/keyboard selection transaction. Top-level keyboard replacement likewise commits its displayed heading after successful close. | Failure at two depths and both poll cursors, partial child-first teardown, retry, keyboard/pointer bar replacement; check surviving native ownership, registration, selection and model links. |
| Refresh/view switch leaves old bar pixels | I5: publishing state must schedule bar damage. Cache the model/enabled mask; changed targets schedule damage even for equal masks, while unchanged same-view paints remain idle. Failed refresh publishes disabled state too. Context invocation does not highlight a remembered bar heading. | Production `os64_ui_render`, actual owner pixels and intersecting damage, enabled→disabled refresh, target swap, same-view state update, idle same-view reuse, stale refresh and context invocation. Owner painting does not automatically refresh views, which would mask the defect. |
| External loss strands held keys/mouse suppression | I4: pairing belongs to a family delivery route. Shared reset clears held keys, F10, decoder, Alt, pressed activation and suppression on external loss/INPUT_LOST, including an already-closed shortcut session. Covered cancels pairs and preserves menus. Owner dismissal retains its matching pair. | Interrupted F10/Enter/Ctrl+D/Alt, external peers zero/31, INPUT_LOST and coverage, both poll cursors, first fresh gesture, family releases and mouse suppression. Queued grandchildren are interrupted before the surviving 150-event backlog drains. |
| Alt/F10 silently rejects a stale target | I6: event-only requests need an error hook. Bar mode and popup opening share retain/refresh preparation; failed preparation reports without revealing the hidden bar or borrowing a view. Event-only close failures use a reporting helper too. | Alt/F10 × hidden/visible, held F10 repeat versus a new gesture, and refused F10/Escape/Tab/Enter closure. Verify one error hook call per request, no callback on failed close and successful retry. |

[Pre-fix regressions](regressions-before.txt) run the first 32-sequence version
against the previous implementation and report **45 failures**, reproducing all
four findings. The final suite expands to **44 parameterized sequences** plus
focused partial-teardown, bar-rendering, family-release, close-error and legacy
regressions. The structural observer runs between routed events (at native poll
boundaries) and after turns/direct transitions: 5,622 observation points. Those
points include empty polls and repeat checks; assertion counts are not counts of
unique scenarios or a proof of exhaustiveness.

Final [fake-font](host-fake.txt) and [real-font](host-real.txt) runs pass
84,040 / 84,088 assertions, respectively, with ASan, UBSan and LeakSanitizer
outside the sandbox and `ASAN_OPTIONS=detect_leaks=1`. Real runs link FreeType and
adopt the Interface fixture. Existing allocation-failure sweeps, font refusal,
callback ordering, saved-target validation and editor right-click/focus behavior
remain in the complete menu suite. Clipboard/editor checks run in the separate
real-font UI and Scribe suites. The [strict full userland build](build.txt) passes.

Reproduce from this worktree:

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --menu --output /tmp/e1b-r4-release-fake
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --menu --real --output /tmp/e1b-r4-release-real
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_menu_mutations.py --output /tmp/e1b-r4-release-mutations
make -C userland -j8
```

For the transition checks alone, set `E1B_R4_ONLY=1` when running either host
driver, or run its built binary with that environment variable. The pre-fix log
comes from the earlier test version, so its assertion count is not directly
comparable to the expanded final suite.

[Source](sources.sha256), [guest binary](binaries.sha256) and
[host binary](host-binaries.sha256) manifests identify the final tested
artifacts. Destruction refusal, allocation failures, precise queue backlogs and
stale target generations remain host-injected. The original P5 report precedes
review fixes; no new hardware validation, E1C migration, icon assets, kernel
change or gclock debt work is claimed.

[Ten mutation gates](mutations.txt) detect the four review hazards plus disabled
invocation, stale target, callback before close, stale status token, skipped
font closure and arrow-as-Escape. The gates inject defects in scratch sources;
each binary must report a behavioral assertion failure, rather than merely
crashing for an unrelated reason.

Separate [real-font UI/editor](editor-real.txt) and [Scribe](scribe-real.txt)
runs pass 1,626 and 2,470 checks with all three sanitizers and leak detection.
They preserve clipboard selection/ownership and existing editor behavior; Scribe
is tested without a menu migration. Reproduce with `tools/test_ui_text_host.py
--real` and `tools/test_scribe_host.py`, using `--output` and the same leak-enabled
environment as above.

## Final native checks and focused P5 repeat

Use the [parent private-disk QEMU setup](../README.md#qemu-reproduction) and
[first-review controller](../review-r1/acceptance.py) with the matching binaries
in the manifest. Run `hid`, this directory's `held-input.py hid`, `ps2-font`,
`held-input.py ps2`, `external`, then `quit` against `/tmp/e1b-vm/qmp.sock`.
Controllers require Python/Pillow. `held-input.py` creates a second application,
sends explicit key-down and key-up separately via QMP, releases outside the
family, returns and checks the first fresh F10. It also strands a shortcut guard
while the shortcut has already closed the panel on its down edge.

All modes exit zero: [HID/pointer](controller-hid.txt),
[PS/2/font/overflow](controller-ps2-font.txt),
[held HID](controller-held-hid.txt), [held PS/2](controller-held-ps2.txt),
[external/close](controller-external.txt). [Serial](serial-acceptance.txt)
identifies the run. The unchanged kernel hash is in `binaries.sha256`.

[Native artifact checks](native-checks.txt) pass; reproduce them with
`python3 docs/design/pending/explorer/evidence/e01b/review-r4/check-guest.py`.
The six archived gallery traces show callbacks after menu closure, zero font
bytes/target references at exit and tightly pitched popup surfaces. Both held
input traces show no F10 up reaching the owner before external loss, and two
Record calls (`hits=2,0`), including the first fresh shortcut after return.

The original [held HID](r4-hid-f10-held.png) and
[recovered HID](r4-hid-f10-recovered.png), plus
[held PS/2](r4-ps2-f10-held.png) and
[recovered PS/2](r4-ps2-f10-recovered.png), have matching highlighted bar pixels.
[HID shortcut recovery](r4-hid-shortcut-recovered.png) and
[PS/2 shortcut recovery](r4-ps2-shortcut-recovered.png) show two calls.
[Dismissing click](dismiss-click-final.png) and [later click](later-click-final.png)
still separate dismissal from invocation. Real [32px Interface](font32.txt)
adopts before reopening [View](font32-view-final.png);
[overflow child](overflow-child-final.png) opens, and
[external loss](external-focus-final.png)/[owner close](owner-close-final.png)
leave no gallery cascade.

Twelve warm openings followed by 24 additional cycles return mapped bytes
(1,048,576), live bytes (193,248), live blocks (493), high-water (463,312) and
audit OK to matching values: [warm](heap-warm.txt), [after](heap-after.txt).
[Font contract/ABI checks](font-contracts.txt) also pass with leak detection.

For a focused P5 repeat on the updated build:

1. Open the gallery and another application. Hold F10, switch to the other
   application, release F10 there, then return. The first fresh F10 should
   highlight the menu bar. Repeat with a held menu shortcut; the first fresh
   shortcut should invoke once.
2. Open a context menu after selecting a top-level heading. The context popup
   should appear without highlighting the remembered heading on the bar.
3. Repeat the earlier dismissing-click/later-click and hidden-bar recovery
   checks. Use `--trace` to record the new build's cleanup; the earlier P5 trace
   remains evidence for its original, unspecified revision only.
