# F5 evidence

Implementation `419b36d`, integration parent `07d8a69`, branch
`codex/font-settings`. See [F5-REPORT.md](../F5-REPORT.md) for conclusions and
limits. These are receipts, not an independent Fable approval. Serial receipts
use `.txt` filenames and normalize trailing line whitespace; the guest document
byte receipt preserves its contents exactly.

## Host reproduction

From `.worktrees/font-settings`, choose fresh output directories:

```sh
export ASAN_OPTIONS=detect_leaks=0
python3 tools/test_font_config_host.py --output /tmp/f5-config
python3 tools/test_font_config_host.py --settings --backend-objects /tmp/f5-config --output /tmp/f5-settings
bash tools/test_appearance_host.sh
bash tools/test_appearance_saved_host.sh
python3 tools/test_ui_text_host.py --real --output /tmp/f5-ui
python3 tools/test_scribe_host.py --output /tmp/f5-scribe
python3 tools/test_gterm_fonts_host.py --output /tmp/f5-gterm
make -C userland -j8 all
git diff --check
tools/stale_refs.sh 419b36d
```

All host runs enable ASan/UBSan. The native-file test's `--settings` directory
must be fresh because the existing-destination cases intentionally retain their
files. It uses the production config writer, provider, FreeType, session
library and installer; only the session device is an in-memory fixture. A new
process verifies saved startup choices. The appearance suite uses its existing
fake backend and the actual kernel appearance-store code; the real backend is
covered by the other named suites.

## Guest reproduction

Requirements: QEMU, mtools, e2fsprogs, dosfstools and Pillow. The accepted F4
worktree supplies `os64_kernel.iso`, `disk/os64.img` and `disk/os64_data.img`.
Those base files are read/copied, never booted or changed. Disk geometry:
FAT at 1 MiB (64 MiB), root ext2 at 65 MiB (256 MiB), separate home ext2 at
1 MiB (1 GiB). The runner byte-verifies installed ext2 payloads. Offline audits
also verify the replaced FAT/ext2 application files. The GUI is the second
Limine entry, at 1024×768.

```sh
python3 docs/fonts/f5-evidence/run_guest.py . ../font-widgets /tmp/f5-guest
```

The runner prints `READY` then waits for newline-delimited JSON action arrays
in `/tmp/f5-guest/commands.jsonl`. From another shell, copy this folder's
`commands.jsonl` there. It drives actual Workshop installation and preview,
real Scribe edits/selection/help/save, live gterm changes, the public settings
API checks and a clean guest shutdown. The guest source data file is `document.txt`; its edited result is
`f5-document.txt`. This receipt stores their exact UTF-8 contents and digests
in `document-bytes.json`, preserving intentional trailing spaces without
introducing source-format whitespace errors.

After the process exits:

```sh
python3 docs/fonts/f5-evidence/audit_guest.py . /tmp/f5-guest
python3 docs/fonts/f5-evidence/run_guest.py . ../font-widgets /tmp/f5-guest --resume
```

Once `READY`, copy `commands-reboot.jsonl` into that run's file. It checks
persisted choices, starts real gterm and Scribe at the saved sizes, exercises
the friendly terminal refusal and shuts down. The checked-in final code already
includes the gterm frame cap, so this reproduction does not need a third boot.
`commands-fitted.jsonl` and `serial-fitted.txt` record the additional cold boot
that verified that fix during development. To repeat it, use
`--resume --label fitted` and its corresponding action file.

`audit_guest.py` checks exact document edit bytes, exact copied font bytes,
public API markers, available reboot markers, and both ext2 filesystems. It
reports the complete read-only FAT diagnostic separately. `--refresh appearance`
or `--refresh gterm` is available only for offline private images when reproducing
an intermediate binary; no refresh is needed at `419b36d`.

`payload-sha256.txt` is the initial complete installation. The final user-facing
refusal wording was rebuilt/reinstalled before the second boot, recorded by
`payload-reboot-sha256.txt`. The final gterm startup cap was rebuilt/reinstalled
before the third, recorded by `payload-updates-sha256.txt`. `binaries-sha256.txt`
and `source-sha256.txt` identify the delivered final state. Screenshots 11 and
16 from the intermediate wording/oversized-window observations are deliberately
not the final acceptance images; 18 and 19 show the corrected behavior.

## FAT diagnostic control

The pre-boot F4 FAT image checks clean. A private boot/shutdown with its
unmodified kernel **and unmodified userland** creates the same four-cluster
free-space-summary mismatch observed in F5. Reproduce independently:

```sh
python3 docs/fonts/f5-evidence/fat_control.py ../font-widgets /tmp/f5-fat-control
```

`fat-baseline-before.txt`, `fat-baseline-boot.txt`, `fat-baseline-serial.txt`
and `fat-check.txt` retain that distinction. This is not described as a clean
FAT check and is not repaired in the font branch. No cluster-chain/data errors
were reported by these checker runs. `audit.txt` also contains the clean root
and home ext2 checker receipts.
