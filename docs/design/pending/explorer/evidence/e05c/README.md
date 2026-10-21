# E5C acceptance evidence — 2026-10-21

These checks cover the production sources at E5C implementation `9f31fb0f`;
[binary hashes](binary-sha256.txt) identify the installed app/library artifacts.
P5 acceptance and independent review have not occurred.

## Host

`python3 tools/test_roam_launch_host.py --build-only --output /tmp/roam-launch-host`
then `ASAN_OPTIONS=detect_leaks=1 /tmp/roam-launch-host/test_roam_launch`.
[Result and sandbox attachment diagnosis](host.txt). ASan/UBSan/LSan passed
outside the sandbox. The harness compiles the real launch/config/slurp/string
code, directory identity/path helpers and Roam file-operation state machine;
process spawning, native chooser lifecycle and filesystem transport are stubs.

Checks include case-insensitive extensions, final-suffix and dotfile policy,
duplicate/empty mappings, absolute custom config paths, missing/read-error/
allocation-error/malformed/embedded-NUL/oversized input, exact config ceiling,
literal argv with spaces, quotes and shell metacharacters, failed spawn and
argument bounds. Integration covers stale row generations, directory activation,
owned path after a model change, menu retirement/create gating, cancellation,
accepted chooser teardown refusal/retry, failed-open cleanup and terminal argv.

Cross-build passed with warnings treated as errors:

```
CCACHE_DIR=/tmp/roam-launch-ccache make -C userland \
  "$PWD/userland/bin/roam" "$PWD/userland/bin/gterm" "$PWD/userland/bin/scribe"
```

`git diff --check` passed. [Stale-reference output](stale-refs.txt) was inspected:
E05A_DIALOG_POLISH and E05_SCRIBE still exist; their surviving links are valid.

## QEMU

Q35, 4 CPUs, 4 GiB, 1024×768, fresh scratch copies of the repository's root and
home disk images. Installed E5C apps and built shared libraries into the ext2
root; installed shipped configs under /etc. Booted the existing dialog-polish
ISO with its command line selecting ext2 root GUID
`1ec5f5ab-71b7-45cd-a7a4-05646e878e57`, KWORKER HUSK GUI BACKSTOP=10.
The earlier FAT-root setup was corrected before testing the new binaries.
No kernel changes were made. The guest shut down cleanly.

- Enter and double-click on `/home/launch-tests/a space.txt` opened its exact
  contents in Scribe. [Associated file](associated-file.png).
- Unknown `.xyz` opened the shared chooser. Escape cancelled without launching;
  after the existing worker-close delay the parent was usable again.
- Row [context actions](context-menu.png) offered Open With and Copy Full Path;
  Open Terminal Here was disabled for a file and enabled for a folder.
- Explicit Open With selected `/bin/scribe` and opened the unknown file.
  [Selected application](open-with.png).
- Copy Full Path copied `/home/launch-tests/unknown.xyz` exactly, without a newline.
  Terminal Here on `a folder` launched gterm; `pwd` returned
  `/home/launch-tests/a folder`. [Terminal and clipboard](terminal-and-clipboard.png).
- After closing Scribe instances and the launched gterm, a VT1 `ps` snapshot
  showed Roam alive and neither launched application retained on its terminal.
  [Snapshot](after-exit.txt). This checks child processes; it does not claim to
  fix the separately deferred exited dialog-worker threads.
- While Roam remained running, created `/home/roam.conf` containing
  `associations=/home/launch-tests/custom.conf`, and that file containing
  `.xyz=/bin/scribe`. The next Open launched Scribe directly, without a chooser
  or restart. [Custom configuration](custom-config.png).

## P5 walkthrough

Install the matching libos64.so, roam and gterm plus the two shipped configs;
Scribe must include the accepted #319 integration. Use the usual library
bundle if refreshing dependencies. The conf files route to /etc through the
existing os64get `*.conf` rule. A user copy of roam.conf overrides the shipped
copy through the config search path.

Open a text file by Enter/double-click. Right-click another file and choose
Open With, then select /bin/scribe. Cancel once. Copy Full Path and paste it.
Open Terminal Here on a folder containing spaces and run pwd. To change the
mapping location, put `associations = /your/path/associations.conf` in roam.conf.
The mapping format is `.txt = /bin/scribe`; values are literal executable
paths, not commands. Open With is a one-off choice and does not edit mappings.

Appearance Workshop/Frame Studio rendering remains a P5 check. Parent-owned
stacking and the known dialog worker close/lifetime issues remain separate.
