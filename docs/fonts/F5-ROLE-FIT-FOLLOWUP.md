# Font-role fit follow-up

Follow-up to `ce69b48`. Chris reported that changing Terminal size updated gterm
successfully while Workshop said the font did not fit its editor. The same
generic fixed-row planner ran on every shared font publication, including those
whose interface row height was unchanged. Workshop also mapped unrelated
adoption failures to a layout-fit message.

## Change

The default fixed-layout planner validates initial adoption and changes to
interface row height. After successful settings adoption, it skips that height
guard when the proposed interface row height equals the installed one. A smaller
restored window can therefore retain its existing interface while Terminal or
Document updates. The comparison reads resolved candidate and installed metrics
without allocating or changing the staged measurement view. It does not rely
on the selected tab, filenames, nominal pixel size, or fresh provider identities.

Provider validation and widget-run preparation still execute. Custom application
planners, Scribe's layout transaction, gterm's grid/PTY transaction, publication
semantics and font-provider APIs are unchanged. The comparison helper is private
to libos64; no public structure layout or kernel ABI changes.

Apply still publishes the complete draft. If it includes a pending Interface
height different from Workshop's installed height, that change must pass the
guard even when Terminal is the selected role. This is not permission to adopt
an unvalidated interface change. Resizing alone still does not revalidate fonts;
the broader maximize/restore observation remains deferred as Chris requested.

Workshop's failure message now says **"Session updated; Workshop kept its fonts
and preview"**. The old wording was incorrect for non-layout failures and could
suggest that other applications rejected a successfully published change.

## Validation

Receipts are in [f5-evidence/role-fit/](f5-evidence/role-fit/).
Stored text logs normalize line endings and trim trailing whitespace. Original
logs remain under `/tmp/quinn-font-role-*` for this session. The guest harness
runs with `--resume --label roles` against `/tmp/quinn-font-role-guest` to skip
payload injection; the fixture changes the private home configuration only.

- A host regression fails on the preceding implementation at the unchanged-UI,
  Terminal-only adoption assertion (exit 134), then passes after the fix. It
  covers initial refusal, successful retry, a shortened control after acceptance,
  Terminal and Document changes with unchanged interface height, changed-interface
  refusal without generation advancement, retry after enlarging the control,
  and a combined publication that still contains an oversized interface change.
- The test backend adds opt-in S/T fixtures with size-dependent line heights;
  T supplies fixed-width printable ASCII. Existing P/M/L behavior is preserved.
  The production settings/provider/adoption path is exercised by the test.
- `ASAN_OPTIONS=detect_leaks=0 bash tools/test_appearance_host.sh` passes, including
  the new regression and existing session/customizer checks.
- `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output /tmp/quinn-font-role-ui-real`
  passes 1,376 checks against real FreeType and the pinned fonts.
- `make` passes the strict userland cross-build and full disk/ISO assembly.
  `git diff --check` and the stale-reference scan pass.
- QEMU boots private copies of the rebuilt images. Its personal configuration
  selects Sans 16 for Interface/Document and Mono 24 for Terminal. Workshop
  applies Terminal 29, with the real gterm visibly updated, and Document 29;
  both show session success. Interface 29 is then refused and displays the new
  local-retention message. Screenshots were inspected; the guest shuts down.

The constrained-control regression is host-tested. The guest checks actual role
changes and refusal messaging; they do not claim to reproduce Chris's original
maximize/restore sequence. No P5 execution or Fable approval is claimed.
