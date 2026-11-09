# Combined-review evidence

Reviewed baseline: `10e142d`. This directory records the four P3 corrections in
[Quinn's reply](../../FABLE-COMBINED-REPLY.md).

## Host and build

- `install-regression-before.txt`: the new production-handler fixture failed
  against the old code at the expected missing refresh assertion. Its child
  process abort is the deliberate red-test result, not a sanitizer finding.
- `appearance-host.txt`, `appearance-saved-host.txt`: passing ASan/UBSan suites,
  run with `ASAN_OPTIONS=detect_leaks=0`.
- `strict-build.txt`: successful root `make` (default strict userland build),
  with trailing whitespace removed from the captured command output.
- `source-and-binary-sha256.txt`: touched source/test files and built payload.
- `package-verification.txt`: bytes extracted from the booted ISO and both guest
  filesystem roots compared with the built payload.
- `stale-refs.txt`: inspected retirement scan. Any historical `font_page.c`
  source-checksum paths are intentional: the file still exists and the host
  harness now includes it from `tools/test_font_page_host.c`. `fit_text` in the
  review/reply identifies the removed helper, not a live API. The reviewer
  claim about each appearance event remains true: retries are preserved.

## Guest procedure

Used `../run_guest.py` with separate copied root/home images and `--resume`.
The baseline ISO was the existing private 1920x1080 responsive-layout image;
its Restore implementation predates these fixes. The after ISO was rebuilt
from the current strict build, changing only the private ISO's GUI resolution
from 1024x768 to 1920x1080. Both boots used 8 CPUs and 8 GiB RAM. No user backup
images or production boot configuration were edited.

`commands-before.jsonl` and `commands-after.jsonl` are the exact harness actions.
The harness reads `commands-<label>.jsonl` when invoked with `--resume --label
<label>`. Shell output paths in those actions live on the copied home disk.
`serial-*.txt` are boot receipts (trailing whitespace removed); after logd claims the sink, diagnostics appear
in the guest-written `review-log.txt` instead of serial.

Before: launch `/tests/windowmintest --hold`, hide its titlebar, maximize,
show its titlebar, Restore. `restore-before.txt` retains flag 8 (maximized)
and a 1920x1099 frame despite an unchanged 958x696 minimum.

After: repeat that sequence, then maximize with the titlebar showing and hide
it before Restore, then maximize and toggle the titlebar twice before Restore.
All restore the same 958x696 content. Decorated frame: `(32,24) 960x717`;
undecorated frame: `(32,43) 960x698`. Raise the minimum to 1200x800 while
maximized: Restore refuses and logs its dimensions. Lower it to 958x696:
Restore succeeds. `restore-after.txt` ends with zero fixture failures. The
screenshots show the pre-fix maximized result and the post-fix restored window.

For logging, the private home image contains `bad-fonts.conf` as `/fonts.conf`.
Start `logd /home/review-log.txt`, then Workshop, Control Center and gterm,
each with stdout redirected to its own file. Choose and Apply two different
palettes in Workshop. Screenshots `palette-apply-one.png` and
`palette-apply-two.png` show both applied choices. The log contains 12
malformed-font diagnostics (four startup reads, then four on each Apply);
all three stdout files are empty. `quiet-console.png` records the launching
console. `observations.txt` records assertions checked against the extracted
state, log and stdout files.

The install/refused-preview path is exercised by the deterministic host test;
it is not represented as a guest reproduction in this evidence.
