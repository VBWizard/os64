# USB report-protocol follow-up

The P5 diagnostic test identified receiver `03f0:a407`, boot mouse interface
1, endpoint 2, MPS 9. It accepted boot protocol and sent three-byte mouse
reports. Chris reported working movement/clicks and no wheel scrolling in
Frame Studio or VT1. `p5-boot-mode.txt` is the filtered hardware log. It does
not contain a report descriptor; the packed receiver fixture is synthetic.

The implementation reads the descriptor and uses a bounded relative mouse
layout when supported, retaining boot protocol for unsupported descriptors.
Transfers stop at one endpoint packet, with room for reports up to 64 bytes.

Validation:

- Strict full `make -j8` passed.
- `ASAN_OPTIONS=detect_leaks=0 tools/test_mouse_wheel_host.sh` passed. Includes
  actual xHCI nine-byte completion/rearm, unrelated-ID button preservation,
  truncated reports, signed wheel saturation, and existing PS/2/VT checks.
- Descriptor suite covers ID-less and numbered reports, packed 12-bit and
  signed 16-bit fields, Push/Pop, distinct input/output/feature streams,
  no-wheel/absolute/overwide refusal, truncation and 20,000 mutations under
  ASan/UBSan. The script above runs both suites; output is in `host.txt`.
- QEMU Q35, eight CPUs, USB mouse plus USB keyboard: boot log selects report
  protocol, ID 0, four bytes, X 8/8, Y 16/8, wheel 24/8. Keyboard remains in
  boot mode. VT2 wheel-up changes visible text; down restores it exactly
  (crop excludes cursor/status). Appearance list and companion scrollbar
  change on down and restore exactly on up. Screenshot triplets and the
  filtered enumeration/report log are included.
- A fresh boot of the final kernel repeated descriptor selection and VT
  wheel reversal successfully (`final-vt-*`, `qemu-final-report-mode.txt`).
- `git diff --check` and `tools/stale_refs.sh` passed.

Chris subsequently installed this build and confirmed working console and
Frame Studio title-font list scrolling on the P5. This is user-reported
hardware validation; the working boot omitted DEBUG_USB.
Failed SET_PROTOCOL STALL recovery is implemented, but no physical device
or emulator forcing that specific failure was available in this run.
