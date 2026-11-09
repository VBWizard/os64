# Font diagnostics use the system log

Follow-up to `2ea173e`. Chris reported `libui: font layout refused (4); keeping
current fonts` appearing on VT1. The font follower used `os64_printf`, which
writes to the application's inherited stdout. Desktop-launched applications
therefore wrote these diagnostics to their launching console.

Status 4 is `OS64_FONT_LIMIT`: the candidate exceeded a layout or resource
limit, and that consumer kept its active fonts. It is useful diagnostic
information, but does not need console output. The existing `os64_debug_log`
submits it to the kernel log, which logd consumes into its configured file.
Without a daemon the existing kernel log sink applies; no daemon or logging
protocol changes are needed.

The libui font preparation/refusal messages now use that path and identify the
window. gterm's corresponding font preparation/grid-refusal diagnostics and
the Workshop/Control Center accepted-layout measurements use it too. Genuine
startup/application complaints keep their existing behavior. Publication,
adoption, refusal and retry rules are unchanged.

Evidence is in [f5-evidence/font-logging/](f5-evidence/font-logging/):

- Appearance host tests passed with ASan/UBSan, including refusal and retry.
- Real-FreeType gterm tests passed: 2,406,573 checks, 4,482 allocation-denial
  cases, zero live bytes.
- Strict root `make`, `git diff --check` and `tools/stale_refs.sh` passed.
- A private 1024x768 QEMU guest used saved DejaVu Sans 28-pixel interface
  settings. `logd /home/font-log.txt` ran before launching Workshop and
  Control Center with stdout redirected to separate files. Workshop's
  minimum could not fit this screen, deliberately exercising refusal;
  Control Center accepted the font.
- The guest-written log contains `libui: window 2: font layout refused (4);
  keeping current fonts` and `controlcenter: interface row 33, minimum 493x273`.
  Both application stdout files were verified as zero bytes after shutdown.
  Screenshots show the console and retained Workshop behind Control Center.

This is a userland change. Update libos64 and the three applications, then
restart consumers so they use the new library. The separate Restore change in
`2ea173e` still requires its kernel update. P5 validation remains user-reported.
