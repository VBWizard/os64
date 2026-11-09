# F2.5 receipts

The report distinguishes the shared boundary from unfinished consumer behavior.

- `host-o2.log`, `host-o0.log`: real F1 engine plus shared provider/adoption tests.
- `f2-regression.log`: unchanged F2 fake-backend suite.
- `strict-userland.log`: strict userland rebuild and link output.
- `elf.log`: library dependencies and unresolved imports (the leaf getter is
  resolved through libos64's FreeType dependency).
- `guest-test.log`, `guest-pass.png`: public guest test and inspected console.
- `guest-serial.log`, `guest-monitor.log`: raw execution receipts.
- `guest-payloads.json`: installed ext2 library/test hashes and matching result;
  the ISO hash identifies the reused accepted F2 kernel build.
- `fsck.log`: read-only checks after guest shutdown.
- `run_guest.py`: exact text-boot driver used. Use without `--gui` or `--bench`;
  those inherited F2-driver options are not part of this test procedure.

Host commands are in the report. For the guest, build userland, create private
copies of the accepted F2 `os64.img` and home `data.img`, and use that F2 ISO.
Install the new `userland/bin/libos64.so` and `userland/bin/tests/fontsettest`
into `/lib/libos64.so` and `/tests/fontsettest` on both root partitions. Use
`mcopy -o -i os64.img@@1048576` for FAT. For ext2, extract with
`dd if=os64.img of=root-ext2.img bs=1M skip=65 count=256`, replace those files
with `debugfs -w`, and write back with
`dd if=root-ext2.img of=os64.img bs=1M seek=65 conv=notrunc`.
Do this on unmounted disposable copies. Preserve the fixture fonts under
`/tests/fonts` and keep the existing leaf library.

Place the accepted ISO at `<worktree>/os64_kernel.iso`, then:

```sh
python3 docs/fonts/f25-evidence/run_guest.py "$PWD" /tmp/os64-f25-guest new-run
```

The driver copies the input disks into the named run directory, boots ext2,
runs `fontsettest > /home/f25-fontset.log`, displays the log, captures a screenshot
and shuts down. Completion of QEMU alone does not prove a test passed; inspect
the application log and screenshot. Extract the log from the stopped home image
with `debugfs -R 'dump /f25-fontset.log <output>' 'data.img?offset=1048576'`.
Dump the installed library and executable from the ext2 root to compare with the
build byte for byte. Run `e2fsck -fn` on extracted root and home after shutdown.

The final guest runs on the existing F2 kernel. This receipt makes no claim of a
new ISO/root-image build, a real terminal font switch, GUI relayout or P5 testing.
