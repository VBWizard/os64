# F2 evidence index

See [the report](../F2-REPORT.md) for conclusions and limitations.

| Receipt | Meaning |
|---|---|
| `host-o2.log`, `host-o0.log` | Actual text implementation plus real F1 engine, 7,910 ASan/UBSan checks each |
| `strict-build.log` | Default strict OS/image build output |
| `gui-text.log`, `gui-serial.log`, `gui-monitor.log` | Fresh final-candidate guest application, serial and monitor records |
| `gui-specimen.png`, `gui-pass.png` | Visually inspected specimen and guest result |
| `box-atlas.png`, `atlas.c` | All 160 procedural cells, ordered by code point |
| `spawn-before.log`, `spawn-after.log` | Six raw spawn batches for the private dependency comparison |
| `text-before.log`, `text-after.log` | Real F1/F2 text tests on both comparison variants |
| `ab-link-command.txt` | Intermediate library build recipe used to construct the private comparison pair |
| `elf.log` | Final dynamic dependencies, defined exports and unresolved imports |
| `guest-payloads.json` | SHA256 and byte-comparison result for installed FAT/ext2 libraries and Unicode notice |
| `artifacts.json` | Final build artifact hashes plus the historical private comparison libraries |
| `fsck.log` | Offline read-only checks of final guest ext2 root and home after shutdown |
| `source-inputs.json` | SHA256 of candidate sources/docs and frozen public headers; excludes this evidence directory |
| `stale-refs.log` | Mechanical stale-reference audit and author disposition |

The tracked sources plus base `7c9e2d2` reproduce the final implementation.
Absolute /tmp paths in logs identify original runs; they are not required inputs
to host tests. The A/B comparison is a historical measurement of an intermediate
F2 build, as described in the report, not the final candidate's source manifest.

Host commands are in the report. For the guest, provide a private image directory
containing the freshly built `os64.img` plus an os64 home `data.img`, then run:

```sh
python3 docs/fonts/f2-evidence/run_guest.py "$PWD" /tmp/os64-f2-guest gui-check --gui
```

The driver copies both images into the named run directory, starts QEMU with a
stdio monitor, switches to the GUI boot entry, runs `texttest`, captures the
specimen and result, and requests shutdown. Use a fresh run name. It never needs
the user's live VM disks. The tested home image is ext2 at byte offset 1048576.
For text boot omit `--gui`; add `--bench` to run the six `textspawn` batches.
Read the guest application log and inspect screenshots; process completion alone
is not an assertion that the guest test passed. The final GUI run selected the
FAT boot root. Its embedded ext2 root was also compared and checked offline;
the earlier A/B text boots exercised ext2 as the active root.

To inspect the installed files after shutdown, use `mcopy -i os64.img@@1048576`
for FAT. Extract ext2 with `dd if=os64.img of=root-ext2.img bs=1M skip=65 count=256`
and use `debugfs -R 'dump /lib/libos64.so <output>' root-ext2.img`. The notice is
`/etc/licenses/unicode.txt`. Run `e2fsck -fn` on the extracted root and on
`data.img?offset=1048576`. These commands use disposable copies, not a mounted
filesystem. Compare dumped files against the build byte for byte.

Atlas reproduction:

```sh
cc -std=c11 -Iuserland/libos64 -Iuserland/libos64/include -Iabi/include \
  docs/fonts/f2-evidence/atlas.c userland/libos64/text_bitmap.c -o /tmp/f2-atlas
/tmp/f2-atlas > /tmp/f2-atlas.ppm
```

The PNG is a lossless conversion of that PPM. Image inspection is an additional
check on the pixel regressions, not a replacement for the frozen geometry tests.
